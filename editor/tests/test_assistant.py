# SPDX-License-Identifier: Apache-2.0
"""The Assistant, with a stand-in for the Anthropic API (nothing is sent anywhere)."""
import json
from contextlib import contextmanager
from types import SimpleNamespace

import pytest

from midgard_editor import assistant
from midgard_editor.assistant import Conversation, Tools


class Block(SimpleNamespace):
    def model_dump(self, exclude_none: bool = True) -> dict:
        return {k: v for k, v in vars(self).items() if v is not None or not exclude_none}


def text_reply(text: str):
    return SimpleNamespace(content=[Block(type="text", text=text)], stop_reason="end_turn",
                           usage=SimpleNamespace(input_tokens=100, output_tokens=20))


def tool_reply(name: str, arguments: dict):
    return SimpleNamespace(
        content=[Block(type="text", text="Let me look."),
                 Block(type="tool_use", id="toolu_1", name=name, input=arguments)],
        stop_reason="tool_use", usage=SimpleNamespace(input_tokens=90, output_tokens=10))


class FakeClient:
    """Plays back replies, and records each request."""

    def __init__(self, *replies):
        self.replies, self.requests = list(replies), []
        self.beta = SimpleNamespace(messages=SimpleNamespace(stream=self._stream))

    @contextmanager
    def _stream(self, **request):
        self.requests.append(json.loads(json.dumps(request, default=str)))
        reply = self.replies.pop(0)
        texts = [b.text for b in reply.content if b.type == "text"]
        yield SimpleNamespace(text_stream=iter(texts), get_final_message=lambda: reply)


# ---- tools --------------------------------------------------------------------------------
def test_the_tools_only_read(project):
    tools = Tools(project)
    assert "CREATE TABLE entity" in tools.get_schema()
    rows = json.loads(tools.query_project("SELECT id, name FROM entity ORDER BY id"))
    assert rows[3] == {"id": "e000004", "name": "Player"}
    assert "Error" in tools.query_project("DELETE FROM entity")
    assert "Error" in tools.run("query_project", {"sql": "SELECT 1; DELETE FROM entity"})
    assert "Error" in tools.query_project(
        "WITH x AS (SELECT 1) INSERT INTO meta (key, value) VALUES ('a', 'b')")
    assert len(project.entities()) == 6  # nothing changed

    (project.root / "scripts" / "door.py").write_bytes(b"print('door')\n")
    assert tools.read_script("door.py") == "print('door')\n"
    assert "Error" in tools.read_script("../HelloCube.config")
    assert "Error" in tools.read_script("missing.py")
    assert tools.list_assets("scripts") == "door.py"
    assert "prim_cube" in tools.list_assets("meshes")
    assert tools.run("rm_rf", {}).startswith("Unknown tool")


def test_the_project_can_forbid_it_and_keys_come_from_the_environment(project, monkeypatch):
    assert assistant.allowed(project)
    project.set_config("assistant", {"allowed": False})
    assert not assistant.allowed(project)
    monkeypatch.setenv("ANTHROPIC_API_KEY", "sk-ant-test")
    assert assistant.api_key() == "sk-ant-test"


# ---- a conversation -----------------------------------------------------------------------
def test_a_question_that_uses_a_tool(project):
    client = FakeClient(tool_reply("query_project", {"sql": "SELECT name FROM entity ORDER BY id"}),
                        text_reply("There are **6** entities."))
    conversation = Conversation()
    streamed, tools_seen = [], []
    turn = assistant.ask(client, project, conversation, "How many entities?",
                         on_text=streamed.append, on_tool=tools_seen.append)
    assert not turn.error
    assert turn.answer.endswith("There are **6** entities.")
    assert "Let me look." in streamed and len(tools_seen) == 1
    assert json.loads(turn.tools[0].result)[0] == {"name": "Camera"}  # what was sent back
    assert turn.usage["input_tokens"] == 190 and turn.usage["output_tokens"] == 30
    roles = [m["role"] for m in conversation.messages]
    assert roles == ["user", "assistant", "user", "assistant"]
    assert conversation.messages[2]["content"][0]["tool_use_id"] == "toolu_1"

    first = client.requests[0]
    assert first["model"] == assistant.DEFAULT_MODEL and first["output_config"]["effort"] == \
        "medium"
    assert "midgard.pyi" in first["system"][0]["text"]
    assert first["system"][0]["cache_control"] == {"type": "ephemeral"}
    assert {t["name"] for t in first["tools"]} == {"get_schema", "query_project",
                                                   "read_script", "list_assets"}

    # The next question carries the conversation so far.
    client.replies.append(text_reply("Yes."))
    assistant.ask(client, project, conversation, "Sure?")
    assert len(client.requests[-1]["messages"]) == 5


def test_stopping_and_errors_leave_the_conversation_as_it_was(project):
    conversation = Conversation()
    turn = assistant.ask(FakeClient(text_reply("Long answer")), project, conversation, "Hi",
                         cancelled=lambda: True)
    assert turn.error == "Stopped." and conversation.messages == []

    class Broken:
        beta = SimpleNamespace(messages=SimpleNamespace(stream=lambda **_k: 1 / 0))

    turn = assistant.ask(Broken(), project, conversation, "Hi")
    assert "ZeroDivisionError" in turn.error and conversation.messages == []
    assert len(conversation.turns) == 2  # both shown to the user


def test_history_is_kept_in_the_project(project):
    conversation = Conversation()
    assistant.ask(FakeClient(text_reply("Hello!")), project, conversation, "Hi")
    assistant.save(project, conversation)
    assert assistant.history_file(project).parent.parent == project.root / ".midgard"
    again = assistant.load(project)
    assert again.turns[0].answer == "Hello!" and again.messages == conversation.messages


# ---- the panel ----------------------------------------------------------------------------
@pytest.fixture
def panel(qtbot, settings):
    from midgard_editor.docks.assistant import AssistantPanel

    widget = AssistantPanel(settings)
    qtbot.addWidget(widget)
    yield widget
    widget.cancel()


def test_the_panel_explains_what_it_needs(panel, project, settings, monkeypatch):
    monkeypatch.setattr(assistant, "api_key", lambda: None)
    panel.set_project(project)
    assert panel.stack.currentIndex() == 0 and "is off" in panel.message.text()
    panel._message_action()  # Turn On
    assert settings.get("assistant_enabled")
    assert "API key" in panel.message.text()
    monkeypatch.setattr(assistant, "api_key", lambda: "sk-ant-test")
    panel.refresh_state()
    assert panel.stack.currentIndex() == 1
    project.set_config("assistant", {"allowed": False})
    panel.refresh_state()
    assert "doesn't allow" in panel.message.text()


def test_asking_from_the_panel(panel, project, settings, monkeypatch, qtbot):
    import anthropic

    settings.set("assistant_enabled", True)
    monkeypatch.setattr(assistant, "api_key", lambda: "sk-ant-test")
    client = FakeClient(tool_reply("list_assets", {"kind": "meshes"}), text_reply("Three."))
    monkeypatch.setattr(anthropic, "Anthropic", lambda api_key: client)
    panel.set_project(project)
    panel.input.setPlainText("Which meshes are there?")
    panel.send_or_stop()
    assert panel.send.text() == "Stop"
    qtbot.waitUntil(lambda: not panel.busy, timeout=10000)
    assert panel.current.answer.text().endswith("Three.")
    assert panel.current.tools.count() == 1
    assert "tokens in" in panel.status.text()
    assert assistant.load(project).turns[0].question == "Which meshes are there?"

    panel.ask_about("print('hi')\n", "door.py")
    assert "scripts/door.py" in panel.input.toPlainText()
    assert "```python\nprint('hi')\n```" in panel.input.toPlainText()
