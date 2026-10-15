# SPDX-License-Identifier: Apache-2.0
"""Game scripts in the editor: the project's scripts/ folder, syntax checks, completion with the
midgard API stubs (schema/midgard.pyi), Git change markers, and the snippet libraries.

Snippets come from three places, shown together in the Script editor: the built-in starter set
below, the project's library (the snippet table, shared with everyone on the project) and the
user's own library (editor settings, shared across their projects).
"""
import difflib
import re
import shutil
import subprocess
import uuid
from dataclasses import dataclass
from pathlib import Path

from . import SCHEMA_DIR
from .cook import script_files
from .project import Project
from .settings import Settings

SCRIPTS = "scripts"

NEW_SCRIPT = '''"""{title}"""
import midgard


def on_start(entity):
    """Runs once, when the game starts."""


def on_update(entity, dt):
    """Runs every frame. dt is the frame time in seconds."""
'''


@dataclass
class Snippet:
    name: str
    body: str
    tags: str = ""
    source: str = "built-in"  # "built-in", "project" or "user"
    id: str = ""


BUILTIN_SNIPPETS = [
    Snippet("On trigger enter", '''def on_trigger_enter(entity, other):
    """The player walked into this entity's trigger."""
    midgard.log(other.name, "entered", entity.name)
''', "trigger"),
    Snippet("On trigger exit", '''def on_trigger_exit(entity, other):
    """The player left this entity's trigger."""
    midgard.log(other.name, "left", entity.name)
''', "trigger"),
    Snippet("Move another entity", '''gate = midgard.find("Gate")
if gate is not None:
    gate.position = (0, 3, 0)
''', "entity"),
    Snippet("Spin every frame", '''def on_update(entity, dt):
    x, y, z = entity.rotation
    entity.rotation = (x, y + 90 * dt, z)   # 90 degrees a second
''', "entity"),
    Snippet("Change colour", '''entity.color = (0.9, 0.2, 0.1)   # r, g, b from 0 to 1
''', "entity"),
    Snippet("Change the sky", '''midgard.set_sky(0.9, 0.4, 0.2)   # a sunset
''', "world"),
    Snippet("React to an input action", '''def on_update(entity, dt):
    if midgard.pressed("jump"):
        entity.move(0, 1, 0)
''', "input"),
    Snippet("Do something after a delay", '''def on_update(entity, dt):
    if not getattr(entity_state, "done", False) and midgard.time() > 3.0:
        entity_state.done = True
        midgard.log("Three seconds have passed")


class entity_state:
    """Module-level state survives frames (and is kept when the script is reloaded)."""
''', "time"),
]


# ---- the scripts folder ----------------------------------------------------------------------
def scripts_dir(project: Project) -> Path:
    return project.root / SCRIPTS


def modules(project: Project) -> dict[str, Path]:
    """Script modules in the project: name ("enemies.wolf") -> file."""
    return script_files(scripts_dir(project))


def module_file(project: Project, module: str) -> Path:
    return scripts_dir(project).joinpath(*module.split(".")).with_suffix(".py")


def module_for(project: Project, file: Path) -> str | None:
    """The module name of a file in the scripts folder, or None if it isn't one."""
    for name, path in modules(project).items():
        if path.resolve() == file.resolve():
            return name
    return None


def new_module_name(project: Project, wanted: str) -> str:
    """A module name made from an entity name ("Front Door" -> "front_door"), not yet taken."""
    base = re.sub(r"\W+", "_", wanted.strip().lower()).strip("_") or "script"
    if base[0].isdigit():
        base = "script_" + base
    taken = set(modules(project))
    name, n = base, 2
    while name in taken:
        name, n = f"{base}_{n}", n + 1
    return name


def create_script(project: Project, module: str, title: str = "") -> Path:
    """Writes a new script from the starting template. Returns its file."""
    file = module_file(project, module)
    file.parent.mkdir(parents=True, exist_ok=True)
    if not file.exists():
        file.write_text(NEW_SCRIPT.format(title=title or f"The {module} script."), "utf-8")
    return file


def check_syntax(source: str, filename: str = "<script>") -> tuple[int, str] | None:
    """(line, message) of a syntax error, or None."""
    try:
        compile(source, filename, "exec")
    except SyntaxError as e:
        return e.lineno or 1, f"{type(e).__name__}: {e.msg}"
    return None


# ---- Git ----------------------------------------------------------------------------------------
# Line changes since the last commit, for the Script editor's gutter.
ADDED, MODIFIED, DELETED = "added", "modified", "deleted"


def _git(folder: Path, *args: str) -> str | None:
    """Runs git in a folder. None if git is missing, the folder isn't in a repository, or the
    command fails."""
    git = shutil.which("git")
    if git is None or not folder.is_dir():
        return None
    try:
        run = subprocess.run([git, *args], cwd=folder, capture_output=True, timeout=5,
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except (OSError, subprocess.TimeoutExpired):
        return None
    return run.stdout.decode("utf-8", errors="replace") if run.returncode == 0 else None


@dataclass
class GitBase:
    """A file's last committed text. text is None for a file Git doesn't have yet."""
    text: str | None


def committed(file: Path) -> GitBase | None:
    """The file as of the last commit, or None if it isn't in a Git repository."""
    folder = file.parent
    while not folder.is_dir() and folder != folder.parent:
        folder = folder.parent
    root = _git(folder, "rev-parse", "--show-toplevel")
    if root is None:
        return None
    try:
        relative = file.resolve().relative_to(Path(root.strip()).resolve()).as_posix()
    except ValueError:
        return None
    text = _git(folder, "show", f"HEAD:{relative}")
    return GitBase(text.replace("\r\n", "\n") if text is not None else None)


def line_changes(base: GitBase | None, current: str) -> dict[int, str]:
    """1-based line -> ADDED or MODIFIED, and DELETED on the line after removed lines (or on the
    last line). Empty when the file isn't in a repository or hasn't changed."""
    if base is None:
        return {}
    new = current.split("\n")
    if base.text is None:  # not committed yet: every line is new
        return {i: ADDED for i in range(1, len(new) + 1) if current}
    old = base.text.split("\n")
    changes: dict[int, str] = {}
    matcher = difflib.SequenceMatcher(a=old, b=new, autojunk=False)
    for tag, _i1, _i2, j1, j2 in matcher.get_opcodes():
        if tag == "insert":
            changes.update({j: ADDED for j in range(j1 + 1, j2 + 1)})
        elif tag == "replace":
            changes.update({j: MODIFIED for j in range(j1 + 1, j2 + 1)})
        elif tag == "delete":
            changes.setdefault(min(j1 + 1, len(new)), DELETED)
    return changes


def file_statuses(folder: Path) -> dict[Path, str]:
    """Files under a folder that differ from the last commit: path -> "modified" or "new"."""
    root = _git(folder, "rev-parse", "--show-toplevel")
    out = _git(folder, "status", "--porcelain=v1", "-z", "--untracked-files=all", "--", ".")
    if root is None or out is None:
        return {}
    statuses: dict[Path, str] = {}
    entries = out.split("\0")
    i = 0
    while i < len(entries):
        entry = entries[i]
        i += 1
        if len(entry) < 4:
            continue
        code, path = entry[:2], entry[3:]
        if code[0] in "RC":
            i += 1  # a rename's second path follows
        if "D" in code:
            continue
        state = "new" if code in ("??", "A ") or code[0] == "A" else "modified"
        statuses[(Path(root.strip()) / path).resolve()] = state
    return statuses


# ---- completion --------------------------------------------------------------------------------
@dataclass
class Completion:
    name: str
    insert: str     # what to type after the cursor
    kind: str       # function, class, instance, module, keyword, …
    signature: str  # e.g. "find(name: str) -> Entity | None"


def completions(source: str, line: int, column: int, path: Path | None = None,
                project: Project | None = None) -> list[Completion]:
    """Completions at a 1-based line and 0-based column, with the midgard stubs available."""
    import jedi  # imported here: it takes a moment and only the Script editor needs it

    jedi_project = jedi.Project(
        path=str(scripts_dir(project)) if project else str(SCHEMA_DIR),
        added_sys_path=[str(SCHEMA_DIR)] + ([str(scripts_dir(project))] if project else []),
        smart_sys_path=False, load_unsafe_extensions=False)
    script = jedi.Script(source, path=str(path) if path else None, project=jedi_project)
    out = []
    try:
        found = script.complete(line, column)
    except Exception:  # noqa: BLE001 - jedi can fail on half-typed code; offer nothing
        return []
    for c in found[:200]:
        signatures = c.get_signatures() if c.type == "function" else []
        signature = signatures[0].to_string() if signatures else c.name
        out.append(Completion(c.name, c.complete or "", c.type, signature))
    return out


# ---- snippets ---------------------------------------------------------------------------------
def project_snippets(project: Project) -> list[Snippet]:
    with project.connect() as con:
        rows = con.execute("SELECT id, name, tags, body FROM snippet ORDER BY name").fetchall()
    return [Snippet(name, body, tags, "project", sid) for sid, name, tags, body in rows]


def add_project_snippet(project: Project, name: str, body: str, tags: str = "") -> Snippet:
    snippet = Snippet(name, body, tags, "project", uuid.uuid4().hex[:12])
    with project.connect() as con:
        con.execute("INSERT INTO snippet (id, name, tags, body) VALUES (?, ?, ?, ?)",
                    (snippet.id, name, tags, body))
    return snippet


def delete_project_snippet(project: Project, snippet_id: str) -> None:
    with project.connect() as con:
        con.execute("DELETE FROM snippet WHERE id = ?", (snippet_id,))


def user_snippets(settings: Settings) -> list[Snippet]:
    return [Snippet(s["name"], s["body"], s.get("tags", ""), "user", s["id"])
            for s in settings.get("snippets") or []]


def add_user_snippet(settings: Settings, name: str, body: str, tags: str = "") -> Snippet:
    snippet = Snippet(name, body, tags, "user", uuid.uuid4().hex[:12])
    stored = settings.get("snippets") or []
    settings.set("snippets", stored + [{"id": snippet.id, "name": name, "tags": tags,
                                        "body": body}])
    return snippet


def delete_user_snippet(settings: Settings, snippet_id: str) -> None:
    settings.set("snippets", [s for s in settings.get("snippets") or []
                              if s["id"] != snippet_id])


def all_snippets(project: Project | None, settings: Settings) -> list[Snippet]:
    found = list(BUILTIN_SNIPPETS)
    if project is not None:
        found += project_snippets(project)
    return found + user_snippets(settings)
