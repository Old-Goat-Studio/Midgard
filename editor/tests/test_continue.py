# SPDX-License-Identifier: Apache-2.0
"""Play and Continue: Play starts from the scene as designed; Continue picks up where the last
Play ended (the player's place, the clock and the camera view), kept in .midgard/play/."""
from pathlib import Path

from test_scripting import needs_scripting, scripting_runtime

from midgard_editor import main_window as main_window_module
from midgard_editor.cook import cook
from midgard_editor.live_link import PlaySession, State


def test_play_starts_afresh_and_continue_keeps_where_it_got_to(window, project, tmp_path,
                                                                monkeypatch):
    window.set_project(project)
    started = []
    monkeypatch.setattr(main_window_module, "runtime_candidates", lambda: [Path("runtime")])
    monkeypatch.setattr(window, "cook_project", lambda: tmp_path / "pkg")
    monkeypatch.setattr(window.session, "start",
                        lambda runtime, package, extra=None: started.append(extra))
    folder = window.play_folder()
    assert folder == project.root / ".midgard" / "play"
    assert not window.can_continue()  # nothing played yet
    window._play_state_changed(State.STOPPED)
    assert not window.continue_action.isEnabled()

    window.play()
    assert started[-1] == ["--world", str(folder / "world"),
                           "--player-profile", str(folder / "profile")]
    # The game saved its world when it ended: now Continue can pick it up.
    (folder / "world").mkdir(parents=True)
    (folder / "world" / "world.db").write_bytes(b"")
    (folder / "world" / "marker").write_text("kept", "utf-8")
    assert window.can_continue()
    window._play_state_changed(State.STOPPED)
    assert window.continue_action.isEnabled()
    window.continue_play()
    assert started[-1] == ["--world", str(folder / "world"),
                           "--player-profile", str(folder / "profile")]
    assert (folder / "world" / "marker").is_file()  # kept
    window.play()  # from the start again: the old world goes
    assert not (folder / "world" / "marker").exists()


@needs_scripting
def test_the_real_game_continues_where_it_was_left(project, qtbot, tmp_path):
    play = tmp_path / "play"
    extra = ["--world", str(play / "world"), "--player-profile", str(play / "profile")]
    package = cook(project, tmp_path / "pkg").out_dir

    def run(then) -> dict:
        received = []
        session = PlaySession(project)
        session.message_received.connect(received.append)
        try:
            session.start([str(scripting_runtime()), "--headless"], package, extra)
            qtbot.waitUntil(lambda: session.state == State.PLAYING, timeout=20000)
            then(session, received)
            count = len(received)
            session.send({"type": "scene.dump"})
            qtbot.waitUntil(lambda: any(m.get("type") == "scene" for m in received[count:]),
                            timeout=5000)
            return next(m for m in received[count:] if m.get("type") == "scene")["scene"]
        finally:
            session.stop()  # the game saves its world as it ends

    def player(scene: dict) -> list[float]:
        return next(e["position"] for e in scene["entities"] if e["id"] == "e000004")

    def move_player(session, received) -> None:  # as the editor's gizmos would, while playing
        entity = project.entity("e000004")
        entity.components["transform"]["position"] = [3.0, 0.5, -2.0]
        session.send({"type": "entity.put", "entity": {
            "id": entity.id, "name": entity.name, "parent": entity.parent,
            "components": entity.components}})
        qtbot.waitUntil(lambda: any(m.get("text") == "applied entity.put e000004"
                                    for m in received), timeout=5000)
        qtbot.wait(300)  # a few frames, so the character stands there

    first = run(move_player)
    assert abs(player(first)[0] - 3.0) < 0.05
    assert (play / "world" / "world.db").is_file()
    again = run(lambda session, received: None)  # Continue: nothing moved this time
    assert abs(player(again)[0] - 3.0) < 0.05 and abs(player(again)[2] + 2.0) < 0.05
    assert again["time"] >= first["time"]  # the clock carried on from where it was
