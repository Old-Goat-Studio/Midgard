# SPDX-License-Identifier: Apache-2.0
"""Version control for projects, with Git: File ▸ Put Project Under Git…, and the New Project
dialog's "Keep the project in Git". A project folder becomes a repository with a first commit;
its .gitignore (written when the project is created) leaves out .midgard/ and build/.

The Script editor's change markers (scripting.committed / line_changes) read from it.
"""
import shutil
from dataclasses import dataclass
from pathlib import Path

from .project import Project
from .scripting import _git

GITIGNORE = ".midgard/\nbuild/\n"


class VcsError(Exception):
    """Shown to the user."""


@dataclass
class Identity:
    name: str
    email: str


def available() -> bool:
    return shutil.which("git") is not None


def repository_root(folder: Path) -> Path | None:
    """The repository a folder is in, or None."""
    out = _git(folder, "rev-parse", "--show-toplevel")
    return Path(out.strip()) if out else None


def identity(folder: Path) -> Identity | None:
    """The name and email Git would commit with here, or None if it has none."""
    name = (_git(folder, "config", "user.name") or "").strip()
    email = (_git(folder, "config", "user.email") or "").strip()
    return Identity(name, email) if name and email else None


def put_under_git(project: Project, who: Identity | None = None) -> str:
    """Makes the project folder a repository and commits the project. `who` is saved in the
    repository's own config (not the user's global one) when Git has no identity. Returns the
    first commit's short id. Raises VcsError."""
    if not available():
        raise VcsError("Git is not installed (https://git-scm.com).")
    root = project.root
    if (existing := repository_root(root)) is not None:
        raise VcsError(f"The project is already in a Git repository ({existing}).")
    ignore = root / ".gitignore"
    if not ignore.is_file():
        ignore.write_text(GITIGNORE, "utf-8")
    if _git(root, "init", "-q") is None:
        raise VcsError("git init failed.")
    if identity(root) is None:
        if who is None:
            shutil.rmtree(root / ".git", ignore_errors=True)
            raise VcsError("Git needs a name and email to commit with.")
        _git(root, "config", "user.name", who.name)
        _git(root, "config", "user.email", who.email)
    if _git(root, "add", "-A") is None or \
            _git(root, "commit", "-q", "-m", f"Create the {project.name} project") is None:
        raise VcsError("The first commit failed; see the repository with git status.")
    return (_git(root, "rev-parse", "--short", "HEAD") or "").strip()


def status_text(project: Project | None) -> tuple[bool, str]:
    """(whether Put Project Under Git can run, the menu text explaining why not)."""
    if project is None:
        return False, "Put Project Under &Git…"
    if not available():
        return False, "Put Project Under &Git… (Git is not installed)"
    if repository_root(project.root) is not None:
        return False, "Project Is in &Git"
    return True, "Put Project Under &Git…"


# ---- committing -----------------------------------------------------------------------------
STATES = {"M": "modified", "A": "new", "?": "new", "D": "deleted", "R": "renamed",
          "C": "new", "T": "modified", "U": "conflict"}


@dataclass
class ChangedFile:
    path: str   # relative to the project folder, with forward slashes
    state: str  # modified, new, deleted, renamed or conflict


def changed_files(project: Project) -> list[ChangedFile]:
    """Files in the project folder that differ from the last commit, sorted by path."""
    root = repository_root(project.root)
    out = _git(project.root, "status", "--porcelain=v1", "-z", "--untracked-files=all",
               "--", ".")
    if root is None or out is None:
        return []
    files: list[ChangedFile] = []
    entries = out.split("\0")
    i = 0
    while i < len(entries):
        entry = entries[i]
        i += 1
        if len(entry) < 4:
            continue
        code, path = entry[:2], entry[3:]
        if code[0] in "RC":
            i += 1  # the original name follows
        letter = next((c for c in code if c not in " ?"), "?")
        absolute = (root / path).resolve()
        try:
            relative = absolute.relative_to(project.root.resolve()).as_posix()
        except ValueError:
            continue
        files.append(ChangedFile(relative, STATES.get(letter, "modified")))
    return sorted(files, key=lambda f: f.path)


def suggest_message(files: list[ChangedFile]) -> str:
    """A first line for the commit message: "Update door.py", "Add gate.py", "Update 3 files"."""
    if len(files) == 1:
        verb = {"new": "Add", "deleted": "Delete", "renamed": "Rename"}.get(files[0].state,
                                                                            "Update")
        return f"{verb} {Path(files[0].path).name}"
    return f"Update {len(files)} files" if files else ""


def commit(project: Project, paths: list[str], message: str,
           who: Identity | None = None) -> str:
    """Commits these files (paths relative to the project folder) with a message, leaving any
    other changes uncommitted. Returns the commit's short id. Raises VcsError."""
    if not message.strip():
        raise VcsError("Write a commit message first.")
    if not paths:
        raise VcsError("Tick at least one file to commit.")
    if repository_root(project.root) is None:
        raise VcsError("The project is not in Git yet (File ▸ Put Project Under Git…).")
    if identity(project.root) is None:
        if who is None:
            raise VcsError("Git needs a name and email to commit with.")
        _git(project.root, "config", "user.name", who.name)
        _git(project.root, "config", "user.email", who.email)
    # add -A stages new and deleted files too; commit -- paths commits only these.
    if _git(project.root, "add", "-A", "--", *paths) is None:
        raise VcsError("Git could not add the files.")
    if _git(project.root, "commit", "-q", "-m", message.strip(), "--", *paths) is None:
        raise VcsError("The commit failed; see the project with git status.")
    return (_git(project.root, "rev-parse", "--short", "HEAD") or "").strip()
