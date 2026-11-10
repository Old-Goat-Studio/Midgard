# SPDX-License-Identifier: Apache-2.0
"""The HUD (milestone 6.0 step 3): scripts' notes, prompt, timers and switches (midgard.ui), and
the Stats panel's "Show on the HUD"."""
import pytest
from test_scripting import needs_scripting, simulate, write_script

from midgard_editor import stats
from midgard_editor.cook import cook
from midgard_editor.docks.stats_panel import StatsPanel
from midgard_editor.scene_model import Entity

HUD_SCRIPT = """
import midgard

def on_start(entity):
    midgard.ui.notify("Picked up a key")
    midgard.ui.notify("Found a secret", seconds=60)
    midgard.ui.prompt("Press E to open")
    midgard.ui.timer("bomb", 1.0, label="Bomb", on_done=boom)
    midgard.ui.timer("lap", 0, label="Lap", count="up")
    midgard.ui.timer("gone", 5, on_done=never)
    print("stopped", midgard.ui.stop_timer("gone"), midgard.ui.stop_timer("gone"))
    print("crosshair", midgard.ui.crosshair, midgard.ui.hud)
    midgard.ui.crosshair = False
    try:
        midgard.ui.timer("x", 1, count="sideways")
    except ValueError as e:
        print("refused", e)

def boom():
    print("boom", round(midgard.ui.timer_value("lap"), 1), midgard.ui.timer_value("bomb"))
    midgard.ui.prompt()

def never():
    print("never")
"""


@needs_scripting
def test_scripts_put_notes_a_prompt_and_timers_on_the_hud(project, tmp_path):
    project.put_entity(Entity("e000093", "Door", None, {
        "transform": {}, "script": {"module": "door"}}))
    write_script(project, "door", HUD_SCRIPT)
    package = cook(project, tmp_path / "pkg").out_dir
    dumped, run = simulate(package, 300)  # (5 seconds)
    assert run.returncode == 0, run.stderr
    lines = run.stdout.splitlines()
    assert "stopped True False" in lines
    assert "crosshair None True" in lines
    assert 'refused a timer counts "down" or "up", not "sideways"' in lines
    assert "boom 1.0 None" in lines  # (the bomb's timer is gone when it goes off)
    assert "never" not in lines
    hud = dumped["hud"]
    assert hud["notes"] == ["Found a secret"]  # (the first faded after 4 s)
    assert hud["prompt"] == ""                 # (boom took it away)
    assert hud["crosshair"] is False and hud["visible"] is True
    assert list(hud["timers"]) == ["lap"] and 4.9 < hud["timers"]["lap"] < 5.1


def test_the_stats_panel_keeps_a_stat_off_the_hud(qtbot, project):
    panel = StatsPanel()
    qtbot.addWidget(panel)
    panel.set_project(project)
    panel.choose("stamina")
    assert panel.on_hud.isChecked()
    panel.on_hud.setChecked(False)
    assert stats.load(project, "stamina")["stat"]["hud"] is False
    # Kept when its kind changes, back on with undo.
    panel.kind.setCurrentIndex(panel.kind.findData("counter"))
    panel.kind.activated.emit(panel.kind.currentIndex())
    assert stats.load(project, "stamina")["stat"] == {
        "kind": "counter", "min": 0, "max": 0, "start": 0, "hud": False}
    project.undo_stack.undo()
    project.undo_stack.undo()
    assert "hud" not in stats.load(project, "stamina")["stat"]
    panel.choose("stamina")
    assert panel.on_hud.isChecked()
    assert stats.problems_of({"hud": "yes"}) == ["'hud' isn't true or false"]


@pytest.mark.parametrize("hidden", [True, False])
def test_the_engine_reads_the_hud_switch(project, tmp_path, hidden):
    from midgard_editor import engine
    if engine.module() is None:
        pytest.skip("the engine isn't here to check")
    rows = stats.load_all(project)
    rows[0]["stat"]["hud"] = not hidden
    stats.save_all(project, rows)
    cook(project, tmp_path / "pkg")  # (the engine reads it: no error)


def test_a_games_screenshot_shows_its_hud_only_when_asked(project, tmp_path):
    import subprocess

    from PIL import Image, ImageChops

    from midgard_editor.about import runtime_candidates
    if not runtime_candidates():
        pytest.skip("engine not built here")
    package = cook(project, tmp_path / "pkg").out_dir
    pictures = {}
    for name, extra in (("world", []), ("hud", ["--show-hud"])):
        out = tmp_path / f"{name}.png"
        run = subprocess.run([str(runtime_candidates()[0]), "--package", str(package),
                              "--size", "320x180", "--screenshot", str(out), *extra],
                             capture_output=True, text=True, timeout=120)
        assert run.returncode == 0, run.stderr
        pictures[name] = Image.open(out).convert("RGB")
    box = ImageChops.difference(pictures["world"], pictures["hud"]).getbbox()
    assert box is not None and box[0] < 160 and box[1] < 40  # (the bars, top left)
