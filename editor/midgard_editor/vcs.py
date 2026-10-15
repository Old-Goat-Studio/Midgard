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
