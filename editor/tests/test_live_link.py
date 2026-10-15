# SPDX-License-Identifier: Apache-2.0
import json
import sys
import time

import pytest

from midgard_editor import live_link
from midgard_editor.about import runtime_candidates
from midgard_editor.cook import cook
from midgard_editor.live_link import FrameReader, LinkError, PlaySession, State, encode
from midgard_editor.undo import SetEntityCommand

# A stand-in for midgard_runtime: connects, says hello, sends stats, echoes every message back as
# a debug log line, and quits on "stop".
FAKE_RUNTIME = r'''
import json, socket, struct, sys
args = sys.argv[1:]
host, port = args[args.index("--editor-link") + 1].rsplit(":", 1)
s = socket.create_connection((host, int(port)))
def send(m):
    body = json.dumps(m).encode()
    s.sendall(struct.pack(">I", len(body)) + body)
send({"type": "hello", "role": "runtime", "version": "0.0.0", "protocol": 0})
send({"type": "stats", "fps": 60, "draw_calls": 2, "paused": False})
send({"type": "state", "state": "paused"})  # F6 pressed in the game window
send({"type": "state", "state": "playing"})
buf = b""
while True:
    data = s.recv(65536)
    if not data:
        break
    buf += data
    while len(buf) >= 4 and len(buf) >= 4 + struct.unpack(">I", buf[:4])[0]:
        n = struct.unpack(">I", buf[:4])[0]
        m, buf = json.loads(buf[4:4 + n]), buf[4 + n:]
        if m["type"] == "stop":
            sys.exit(0)
        send({"type": "log", "level": "debug", "text": json.dumps(m)})
'''


def test_messages_are_length_prefixed_json():
    data = encode({"type": "play"}) + encode({"type": "entity.delete", "entity": "e1"})
    reader = FrameReader()
    assert reader.feed(data[:6]) == []
    messages = reader.feed(data[6:])
    assert messages == [{"type": "play"}, {"type": "entity.delete", "entity": "e1"}]
    assert data[:4] == len(json.dumps({"type": "play"}, separators=(",", ":"))).to_bytes(4, "big")


def test_malformed_streams_are_refused():
    with pytest.raises(LinkError):
        FrameReader().feed(b"\x7f\x00\x00\x00")
    with pytest.raises(LinkError):
        FrameReader().feed(b"\x00\x00\x00\x03abc")
    with pytest.raises(LinkError):
        FrameReader().feed(encode({"no": "type"}))


def test_entity_messages(project):
    put = live_link.entity_message(project, "e000004")
    assert put["type"] == "entity.put"
    assert put["entity"]["id"] == "e000004" and "transform" in put["entity"]["components"]
    assert live_link.entity_message(project, "nobody") == {"type": "entity.delete",
                                                           "entity": "nobody"}


@pytest.fixture
def session(project, qtbot):
    session = PlaySession(project)
    yield session
    session.stop()


def echoed(messages: list[dict]) -> list[dict]:
    return [json.loads(m["text"]) for m in messages if m.get("type") == "log"]


def test_play_session_with_a_stand_in_game(project, session, qtbot, tmp_path):
    fake = tmp_path / "fake_runtime.py"
    fake.write_text(FAKE_RUNTIME, encoding="utf-8")
    received, states, stats = [], [], []
    session.message_received.connect(received.append)
    session.state_changed.connect(states.append)
    session.stats_received.connect(stats.append)

    session.start([sys.executable, str(fake)], tmp_path / "package")
    qtbot.waitUntil(lambda: session.state == State.PLAYING, timeout=15000)
    qtbot.waitUntil(lambda: bool(stats), timeout=5000)
    assert stats[0]["fps"] == 60
    qtbot.waitUntil(lambda: State.PAUSED in states, timeout=5000)  # F6 in the game window
    qtbot.waitUntil(lambda: session.state == State.PLAYING, timeout=5000)

    # An Inspector edit goes to the game as entity.put.
    cube = project.entity("e000004")
    cube.components["transform"]["position"] = [3.0, 0.5, 0.0]
    project.undo_stack.push(SetEntityCommand(project, cube, "position"))
    qtbot.waitUntil(lambda: any(m["type"] == "entity.put" and
                                m["entity"]["components"]["transform"]["position"][0] == 3.0
                                for m in echoed(received)), timeout=5000)

    # Deleting an entity sends entity.delete; pausing sends pause.
    project.delete_entity("e000004")
    qtbot.waitUntil(lambda: {"type": "entity.delete", "entity": "e000004"} in echoed(received),
                    timeout=5000)
    session.pause()
    assert session.state == State.PAUSED
    qtbot.waitUntil(lambda: {"type": "pause"} in echoed(received), timeout=5000)

    session.stop()
    assert session.state == State.STOPPED
    assert states[0] == State.STARTING and states[-1] == State.STOPPED


def test_a_game_that_cannot_start_is_reported(session, qtbot, tmp_path):
    session.start(tmp_path / "no_such_runtime.exe", tmp_path / "package")
    qtbot.waitUntil(lambda: session.state == State.STOPPED, timeout=5000)


@pytest.mark.skipif(not runtime_candidates(), reason="the engine runtime is not built")
def test_an_inspector_edit_reaches_the_real_game_within_a_second(project, session, qtbot):
    """Phase 3's exit criterion, with the real engine."""
    package = cook(project).out_dir
    received = []
    session.message_received.connect(received.append)
    session.start(runtime_candidates()[0], package)
    qtbot.waitUntil(lambda: session.state == State.PLAYING, timeout=20000)

    cube = project.entity("e000004")
    cube.components["material"]["color"] = [0.9, 0.1, 0.1, 1.0]
    started = time.monotonic()
    project.undo_stack.push(SetEntityCommand(project, cube, "colour"))
    qtbot.waitUntil(lambda: any(m.get("text") == "applied entity.put e000004" for m in received),
                    timeout=5000)
    assert time.monotonic() - started < 1.0
    assert not [m for m in received if m.get("level") in ("warn", "error")]


def test_messages_follow_the_live_link_schema(project):
    from midgard_editor.contract import live_link_errors

    for message in (
        {"type": "hello", "role": "editor", "version": "0.1.0", "protocol": live_link.PROTOCOL},
        live_link.entity_message(project, "e000004"),
        live_link.entity_message(project, "gone"),
        {"type": "entity.set", "entity": "e000004", "component": "material", "value": None},
        {"type": "pause"}, {"type": "stop"},
        {"type": "log", "level": "debug", "text": "applied entity.put e000004"},
        {"type": "stats", "fps": 60, "draw_calls": 2, "paused": False},
        {"type": "state", "state": "paused"},
    ):
        assert live_link_errors(message) == [], message
    assert live_link_errors({"type": "entity.put", "entity": {"id": "x"}})  # no components
