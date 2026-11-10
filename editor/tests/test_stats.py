# SPDX-License-Identifier: Apache-2.0
"""The player's stats, scores and counters (milestone 6.0 step 2): three in every project, the
Stats panel with undo, checked as the game is cooked, used by scripts (midgard.stats), kept in
the world's save, and followed by a game under the editor."""
import json

import pytest
from test_scripting import needs_scripting, scripting_runtime, simulate, write_script

from midgard_editor import engine, stats
from midgard_editor.cook import CookError, cook
from midgard_editor.docks.stats_panel import StatsPanel
from midgard_editor.scene_model import Entity


@pytest.fixture
def panel(qtbot, project):
    p = StatsPanel()
    qtbot.addWidget(p)
    p.set_project(project)
    return p


def test_a_project_starts_with_health_stamina_and_score(project):
    found = stats.load_all(project)
    assert [s["id"] for s in found] == ["health", "stamina", "score"]
    assert found[0]["stat"]["max"] == 100 and found[0]["stat"]["regen"] > 0
    assert found[2]["stat"]["kind"] == "score"
    assert stats.problems(project) == []


def test_problems_are_said_plainly():
    assert stats.problems_of({"kind": "dice"}) == ["has an unknown kind 'dice'"]
    assert stats.problems_of({"min": 10, "max": 5}) == [
        "its most (5) isn't above its least (10)"]
    assert stats.problems_of({"max": 50, "start": 80}) == ["starts at 80, outside 0 to 50"]
    assert stats.problems_of({"kind": "counter", "max": 5, "start": 9}) == [
        "starts at 9, outside 0 to 5"]
    assert stats.problems_of({"kind": "counter", "start": 900}) == []  # (no target)
    assert stats.problems_of({"kind": "score", "start": 1e6}) == []
    assert stats.problems_of({"max": "lots"}) == ["'max' isn't a number"]
    assert stats.problems_of({"delay": -1}) == ["waits less than no time to refill"]


def test_the_panel_edits_stats_with_undo(panel, project):
    panel.choose("health")
    panel.max.setValue(150)
    assert stats.load(project, "health")["stat"]["max"] == 150
    panel.choose("health")
    panel.regen.setValue(2.5)
    assert stats.load(project, "health")["stat"]["regen"] == 2.5
    project.undo_stack.undo()
    assert stats.load(project, "health")["stat"]["regen"] == 1

    # A new counter, renamed, moved to the top.
    sid = panel.new_stat()
    assert sid == "new_stat" and panel.chosen_id() == "new_stat"
    panel.kind.setCurrentIndex(panel.kind.findData("counter"))
    panel.kind.activated.emit(panel.kind.currentIndex())
    assert stats.load(project, sid)["stat"]["kind"] == "counter"
    assert not panel.form.isRowVisible(panel.regen)  # (a counter doesn't refill)
    panel.name.setText("Keys")
    panel.name.editingFinished.emit()
    panel.max.setValue(5.4)
    assert stats.load(project, sid) == {
        "id": sid, "name": "Keys", "stat": {"kind": "counter", "min": 0, "max": 5, "start": 0}}
    for _ in range(3):
        panel.move(-1)
    assert [s["id"] for s in stats.load_all(project)][0] == sid
    panel.delete_stat()
    assert stats.load(project, sid) is None
    project.undo_stack.undo()
    assert stats.load(project, sid)["name"] == "Keys"

    # A stat whose most is below its least says so.
    panel.choose("stamina")
    panel.min.setValue(200)
    assert "isn't above its least" in panel.problems.text()


def test_the_cook_warns_about_stats_and_scripts_that_name_ones_not_there(project, tmp_path):
    rows = stats.load_all(project)
    rows.append({"id": "mana", "name": "Health", "stat": {"max": 50, "start": 80}})
    stats.save_all(project, rows)
    write_script(project, "shrine", 'import midgard\n\ndef on_start(entity):\n'
                 '    midgard.stats.add("mana", 5)\n    midgard.stats["Luck"] = 3\n'
                 '    midgard.stats.get("SCORE")\n')
    warnings = cook(project, tmp_path / "pkg").warnings
    assert "Stat 'Health' starts at 80, outside 0 to 50." in warnings
    assert any("Two stats are called 'Health'" in w for w in warnings)
    assert any("Script 'shrine' (line 5) uses stat 'Luck'" in w for w in warnings)
    assert not any("'mana'" in w or "'SCORE'" in w for w in warnings)  # (by id; any case)


def test_the_engine_refuses_a_stat_it_cant_read(project, tmp_path):
    if engine.module() is None:
        pytest.skip("the engine isn't here to check")
    rows = stats.load_all(project)
    rows.append({"id": "luck", "name": "Luck", "stat": {"kind": "dice"}})
    stats.save_all(project, rows)
    with pytest.raises(CookError, match="Luck"):
        cook(project, tmp_path / "pkg")


STATS_SCRIPT = """
import midgard

def on_start(entity):
    midgard.stats.on_change(changed)
    print("ids", midgard.stats.ids)
    print("health", midgard.stats.get("health"), midgard.stats.max("Health"))
    print("hurt", midgard.stats.add("health", -30))
    print("score", midgard.stats.add("Score", 10.4), type(midgard.stats["score"]).__name__)
    print("no most", midgard.stats.max("score"), midgard.stats.kind("score"))
    midgard.stats.set_max("health", 120)
    print("most", midgard.stats.max("health"), "health" in midgard.stats, "luck" in midgard.stats)
    try:
        midgard.stats.get("luck")
    except KeyError as e:
        print("refused", e)
    try:
        midgard.stats.set_max("score", 5)
    except ValueError as e:
        print("refused", e)

def changed(stat, before, now):
    print("changed", stat, before, now)

def on_update(entity, dt):
    pass
"""


@needs_scripting
def test_scripts_use_stats_and_hear_them_change(project, tmp_path):
    project.put_entity(Entity("e000091", "Shrine", None, {
        "transform": {}, "script": {"module": "shrine"}}))
    write_script(project, "shrine", STATS_SCRIPT)
    package = cook(project, tmp_path / "pkg").out_dir
    # Health refills 1 a second after 5 s: 6 s (360 frames) in, it has begun.
    dumped, run = simulate(package, 360)
    assert run.returncode == 0, run.stderr
    lines = run.stdout.splitlines()
    assert "ids ['health', 'stamina', 'score']" in lines
    assert "health 100.0 100.0" in lines
    assert "hurt 70.0" in lines
    assert "score 10 int" in lines
    assert "no most None score" in lines
    assert "most 120.0 True False" in lines
    assert "refused \"there is no stat 'luck'\"" in lines
    assert any(line.startswith("refused 'Score' can't have a most") for line in lines)
    changes = [line for line in lines if line.startswith("changed")]
    assert changes[:2] == ["changed health 100.0 70.0", "changed score 0 10"]
    assert any(line.startswith("changed health 70.0 70.0") for line in changes[2:])
    assert dumped["stats"]["score"]["value"] == 10
    assert 70.0 < dumped["stats"]["health"]["value"] < 72.0  # (about a second of refilling)
    assert dumped["stats"]["health"]["max"] == 120.0


@needs_scripting
def test_the_worlds_save_keeps_the_stats(project, tmp_path):
    import subprocess
    project.put_entity(Entity("e000092", "Shrine", None, {
        "transform": {}, "script": {"module": "points"}}))
    write_script(project, "points", "import midgard\n\ndef on_start(entity):\n"
                 "    print('score', midgard.stats.add('score', 7))\n")
    package = cook(project, tmp_path / "pkg").out_dir
    world = tmp_path / "world"
    lines = []
    for _ in range(2):
        run = subprocess.run([str(scripting_runtime()), "--package", str(package), "--simulate",
                              "10", "--world", str(world)], capture_output=True, text=True,
                             timeout=120)
        assert run.returncode == 0, run.stderr
        lines += [line for line in run.stdout.splitlines() if line.startswith("score")]
    assert lines == ["score 7", "score 14"]


def test_a_game_under_the_editor_follows_the_stats_panel(qtbot, panel, project):
    from midgard_editor.about import runtime_candidates
    from midgard_editor.live_link import PlaySession, State
    if not runtime_candidates():
        pytest.skip("engine not built here")
    session = PlaySession(project)
    try:
        received = []
        session.message_received.connect(received.append)
        session.start(runtime_candidates()[0], cook(project).out_dir)
        qtbot.waitUntil(lambda: session.state == State.PLAYING, timeout=20000)
        panel.choose("health")
        panel.max.setValue(40)  # (its value, 100, comes down to the new most)
        panel.new_stat("Coins", "counter")
        qtbot.waitUntil(lambda: sum(m.get("text") == "applied stats.put"
                                    for m in received) >= 2, timeout=10000)
        session.send({"type": "scene.dump"})
        qtbot.waitUntil(lambda: any(m.get("type") == "scene" for m in received), timeout=5000)
        dumped = next(m for m in received if m.get("type") == "scene")["scene"]
        assert dumped["stats"]["health"] == {"value": 40.0, "max": 40.0}
        assert dumped["stats"]["coins"]["value"] == 0
        assert not [m for m in received if m.get("level") in ("warn", "error")]
    finally:
        session.stop()


def test_the_message_has_every_stat_in_order(project):
    message = stats.live_message(project)
    assert [s["id"] for s in message] == ["health", "stamina", "score"]
    assert json.loads(json.dumps(message)) == message
