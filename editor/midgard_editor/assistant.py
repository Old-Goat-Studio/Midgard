# SPDX-License-Identifier: Apache-2.0
"""The Assistant (roadmap §9): Claude, in the editor, about the open project. Version 1 helps
with scripts and the midgard API, and can only look at the project, never change it.

- Off by default; each user turns it on in Preferences and brings their own Anthropic API key,
  kept in the system's credential store (keyring) or read from ANTHROPIC_API_KEY. It is never
  written to midgard.json or the project.
- A project can forbid it: "assistant": {"allowed": false} in {project}.config.
- Tools (read-only): get_schema, query_project (one SELECT, on a read-only connection),
  read_script and list_assets. Everything a tool returns is recorded with the turn, so the user
  can see exactly what was sent.
- History stays local, in .midgard/assistant/ in the project (ignored by version control).

Nothing here is cooked into games, and games never call an LLM.
"""
import contextlib
import json
import os
import sqlite3
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from . import SCHEMA_DIR
from .cook import script_files
from .project import Project

KEYRING_SERVICE, KEYRING_USER = "Midgard", "anthropic"
DEFAULT_MODEL = "claude-opus-5-5"
MODELS = ("claude-opus-5-5", "claude-sonnet-5-5", "claude-haiku-5-5")
EFFORTS = ("low", "medium", "high", "xhigh", "max")
MAX_TOKENS = 16000
MAX_ROWS = 200
MAX_SCRIPT_BYTES = 200_000
FALLBACK_BETA = "server-side-fallback-2026-07-01"


class AssistantError(Exception):
    """Shown to the user."""


# ---- the API key -------------------------------------------------------------------------------
def api_key() -> str | None:
    """The user's key: ANTHROPIC_API_KEY, or the one kept in the credential store."""
    if key := os.environ.get("ANTHROPIC_API_KEY"):
        return key
    try:
        import keyring

        return keyring.get_password(KEYRING_SERVICE, KEYRING_USER)
    except Exception:  # noqa: BLE001 - no credential store on this machine
        return None


def store_api_key(key: str | None) -> None:
    """Keeps the key in the credential store, or forgets it (None)."""
    import keyring

    if key:
        keyring.set_password(KEYRING_SERVICE, KEYRING_USER, key)
    else:
        with contextlib.suppress(keyring.errors.PasswordDeleteError):
            keyring.delete_password(KEYRING_SERVICE, KEYRING_USER)


def allowed(project: Project | None) -> bool:
    return project is None or project.config.get("assistant", {}).get("allowed", True)


# ---- tools ---------------------------------------------------------------------------------
class Tools:
    """What the assistant may look at in the project. Every method returns text for the model."""

    SCHEMAS = [
        {"name": "get_schema",
         "description": "The project database's tables and views (CREATE statements) and its "
                        "schema version. Call this before writing SQL.",
         "input_schema": {"type": "object", "properties": {}}},
        {"name": "query_project",
         "description": f"Runs ONE read-only SELECT on the project database and returns at most "
                        f"{MAX_ROWS} rows as JSON. Entity components are a JSON column; use "
                        "json_extract to read them.",
         "input_schema": {"type": "object", "properties": {
             "sql": {"type": "string", "description": "A single SELECT statement."}},
             "required": ["sql"]}},
        {"name": "read_script",
         "description": "The source of a game script in the project's scripts/ folder.",
         "input_schema": {"type": "object", "properties": {
             "path": {"type": "string",
                      "description": "Relative to scripts/, such as door.py or enemies/wolf.py"}},
             "required": ["path"]}},
        {"name": "list_assets",
         "description": "Lists the project's scripts or meshes.",
         "input_schema": {"type": "object", "properties": {
             "kind": {"type": "string", "enum": ["scripts", "meshes"]}},
             "required": ["kind"]}},
    ]

    def __init__(self, project: Project):
        self.project = project

    def run(self, name: str, arguments: dict[str, Any]) -> str:
        handler = getattr(self, name, None)
        if name not in {s["name"] for s in self.SCHEMAS} or handler is None:
            return f"Unknown tool {name}."
        try:
            return handler(**arguments)
        except (TypeError, ValueError, OSError, sqlite3.Error) as e:
            return f"Error: {e}"

    def _connect(self) -> sqlite3.Connection:
        con = sqlite3.connect(f"{self.project.db_path.as_uri()}?mode=ro", uri=True)
        con.execute("PRAGMA query_only = ON")
        return con

    def get_schema(self) -> str:
        con = self._connect()
        try:
            rows = con.execute("SELECT sql FROM sqlite_master WHERE sql IS NOT NULL AND "
                               "type IN ('table', 'view') ORDER BY name").fetchall()
            version = con.execute("PRAGMA user_version").fetchone()[0]
        finally:
            con.close()
        return f"-- schema version {version}\n" + ";\n\n".join(r[0] for r in rows) + ";"

    def query_project(self, sql: str) -> str:
        if not sql.strip().lower().startswith(("select", "with")):
            return "Error: only one SELECT statement is allowed."
        con = self._connect()
        try:
            # sqlite3 refuses a second statement, and the read-only connection any write.
            cursor = con.execute(sql)
            columns = [d[0] for d in cursor.description or []]
            rows = cursor.fetchmany(MAX_ROWS + 1)
        except sqlite3.Error as e:
            return f"Error: {e}"
        finally:
            con.close()
        more = len(rows) > MAX_ROWS
        result = [dict(zip(columns, row, strict=True)) for row in rows[:MAX_ROWS]]
        return json.dumps(result, ensure_ascii=False, default=str) + (
            f"\n(only the first {MAX_ROWS} rows)" if more else "")

    def read_script(self, path: str) -> str:
        scripts = (self.project.root / "scripts").resolve()
        file = (scripts / path).resolve()
        if scripts not in file.parents or file.suffix != ".py":
            return "Error: only .py files inside the project's scripts/ folder can be read."
        if not file.is_file():
            return f"Error: scripts/{path} does not exist."
        data = file.read_bytes()[:MAX_SCRIPT_BYTES]
        return data.decode("utf-8", errors="replace")

    def list_assets(self, kind: str) -> str:
        if kind == "scripts":
            folder = self.project.root / "scripts"
            return "\n".join(f.relative_to(folder).as_posix()
                             for f in script_files(folder).values()) or "(no scripts)"
        if kind == "meshes":
            return "\n".join(f"{asset_id}  ({label})"
                             for asset_id, label in self.project.mesh_assets().items())
        return "Error: kind must be scripts or meshes."


# ---- the system prompt ---------------------------------------------------------------------
def system_prompt(project: Project) -> str:
    """Stable for a project, so the API can cache it."""
    stub = (SCHEMA_DIR / "midgard.pyi").read_text("utf-8")
    components = (SCHEMA_DIR / "components.md").read_text("utf-8")
    return f"""You are the Assistant in the Midgard editor, helping a game designer with the \
open project "{project.game_title}" (project {project.name}). Midgard is an open-source engine \
for action RPGs: a Python/Qt editor, a C++ engine, and game scripts in Python.

You can look at the project with your tools (its database schema, read-only SQL queries, its \
scripts and meshes), but you cannot change anything: when you suggest code or data changes, \
show them in full for the designer to copy, with the file or entity they belong to. Call \
get_schema before writing SQL.

Game scripts are pure Python modules in the project's scripts/ folder, attached to entities by \
a script component, and talk to the engine only through the midgard module. Its whole API (v1) \
is the stub below; do not invent functions that it doesn't list. If something isn't possible \
with it yet, say so plainly.

<midgard.pyi>
{stub}
</midgard.pyi>

<components.md>
{components}
</components.md>

Be concise. Use Markdown, and fenced ```python blocks for code."""


# ---- a conversation ------------------------------------------------------------------------
@dataclass
class ToolCall:
    name: str
    arguments: dict[str, Any]
    result: str  # what was sent back to the model


@dataclass
class Turn:
    """One question and the answer to it, with what was sent along the way."""
    question: str
    answer: str = ""
    tools: list[ToolCall] = field(default_factory=list)
    usage: dict[str, int] = field(default_factory=dict)
    error: str = ""


@dataclass
class Conversation:
    messages: list[dict] = field(default_factory=list)  # as sent to the API
    turns: list[Turn] = field(default_factory=list)  # as shown to the user

    def usage(self) -> dict[str, int]:
        total: dict[str, int] = {}
        for turn in self.turns:
            for key, value in turn.usage.items():
                total[key] = total.get(key, 0) + value
        return total


def history_file(project: Project) -> Path:
    return project.work_dir / "assistant" / "conversation.json"


def load(project: Project) -> Conversation:
    try:
        data = json.loads(history_file(project).read_text("utf-8"))
    except (OSError, ValueError):
        return Conversation()
    turns = [Turn(t["question"], t.get("answer", ""),
                  [ToolCall(**c) for c in t.get("tools", [])], t.get("usage", {}),
                  t.get("error", "")) for t in data.get("turns", [])]
    return Conversation(data.get("messages", []), turns)


def save(project: Project, conversation: Conversation) -> None:
    file = history_file(project)
    file.parent.mkdir(parents=True, exist_ok=True)
    data = {"messages": conversation.messages,
            "turns": [{"question": t.question, "answer": t.answer, "usage": t.usage,
                       "error": t.error,
                       "tools": [c.__dict__ for c in t.tools]} for t in conversation.turns]}
    tmp = file.with_suffix(".tmp")
    tmp.write_text(json.dumps(data, ensure_ascii=False, indent=1), "utf-8")
    os.replace(tmp, file)


def _block_param(block: Any) -> dict:
    """A response content block as a request parameter."""
    return block.model_dump(exclude_none=True)


def ask(client: Any, project: Project, conversation: Conversation, question: str, *,
        model: str = DEFAULT_MODEL, effort: str = "medium",
        on_text: Callable[[str], None] = lambda _t: None,
        on_tool: Callable[[ToolCall], None] = lambda _c: None,
        cancelled: Callable[[], bool] = lambda: False) -> Turn:
    """Asks a question and lets the model use the tools until it answers. Streams text to on_text
    and reports each tool call. Adds the turn to the conversation (unless cancelled before any
    reply) and returns it."""
    turn = Turn(question)
    tools = Tools(project)
    pending = conversation.messages + [{"role": "user", "content": question}]
    system = [{"type": "text", "text": system_prompt(project),
               "cache_control": {"type": "ephemeral"}}]
    try:
        while True:
            with client.beta.messages.stream(
                    model=model, max_tokens=MAX_TOKENS, system=system, messages=pending,
                    tools=Tools.SCHEMAS, output_config={"effort": effort},
                    betas=[FALLBACK_BETA], fallbacks="default") as stream:
                for text in stream.text_stream:
                    if cancelled():
                        turn.error = "Stopped."
                        break
                    turn.answer += text
                    on_text(text)
                if turn.error:
                    break
                message = stream.get_final_message()
            for key in ("input_tokens", "output_tokens", "cache_read_input_tokens",
                        "cache_creation_input_tokens"):
                turn.usage[key] = turn.usage.get(key, 0) + (getattr(message.usage, key, 0) or 0)
            pending.append({"role": "assistant",
                            "content": [_block_param(b) for b in message.content]})
            if message.stop_reason != "tool_use":
                break
            results = []
            for block in message.content:
                if getattr(block, "type", "") == "tool_use":
                    call = ToolCall(block.name, dict(block.input),
                                    tools.run(block.name, dict(block.input)))
                    turn.tools.append(call)
                    on_tool(call)
                    results.append({"type": "tool_result", "tool_use_id": block.id,
                                    "content": call.result})
            pending.append({"role": "user", "content": results})
            turn.answer += "\n\n"
    except Exception as e:  # noqa: BLE001 - network, key or API errors: shown in the dock
        turn.error = _describe(e)
    if turn.error and not turn.answer.strip():
        conversation.turns.append(turn)  # shown, but not sent again
        return turn
    if not turn.error:
        conversation.messages = pending
    conversation.turns.append(turn)
    return turn


def _describe(error: Exception) -> str:
    name = type(error).__name__
    if name == "AuthenticationError":
        return "The API key was not accepted. Set it again with Set API Key…"
    if name in ("APIConnectionError", "APITimeoutError"):
        return "Couldn't reach the Anthropic API. Check the internet connection."
    if name == "RateLimitError":
        return "The API is limiting requests; wait a moment and try again."
    if name == "PermissionDeniedError":
        return f"The API refused the request: {error}"
    return f"{name}: {error}"
