# SPDX-License-Identifier: Apache-2.0
"""Game scripts with the real engine (skipped where the runtime isn't built with scripting)."""
import json
import os
import subprocess
import textwrap
from functools import cache

import pytest

from midgard_editor.about import runtime_candidates
from midgard_editor.cook import cook
from midgard_editor.scene_model import Entity
from midgard_editor.undo import SetEntityCommand


@cache
def scripting_runtime():
    for runtime in runtime_candidates():
        check = subprocess.run([str(runtime), "--check-python"], capture_output=True, text=True,
                               timeout=60)
        if check.returncode == 0:
            return runtime
    return None


needs_scripting = pytest.mark.skipif(scripting_runtime() is None,
                                     reason="no runtime built with scripting")


def test_the_engine_is_here_when_required():
    """CI's engine jobs set MIDGARD_REQUIRE_ENGINE, so these tests can't quietly skip there."""
    if os.environ.get("MIDGARD_REQUIRE_ENGINE"):
        assert scripting_runtime() is not None, "no runtime built with scripting was found"

MOVER = """
import midgard

def on_update(entity, dt):
    entity.move(2.0 * dt, 0, 0)   # 2 m/s along +x
"""

DOOR = """
import midgard

def on_trigger_enter(entity, other):
    midgard.find("Gate").position = (0, 5, 0)
    midgard.set_sky(1, 0, 0)
    print("opened by", other.name)
"""


def write_script(project, module: str, source: str) -> None:
    folder = project.root / "scripts"
    folder.mkdir(exist_ok=True)
    (folder / f"{module}.py").write_text(textwrap.dedent(source), "utf-8")


def add_door(project) -> None:
    """The cube (the player) walks into a trigger whose script lifts a gate."""
    cube = project.entity("e000004")
    cube.components["script"] = {"module": "mover"}
    project.put_entity(cube)
    project.put_entity(Entity("e000007", "Door", None, {
        "transform": {"position": [3.0, 0.5, 0.0]},
        "trigger": {"size": [1.0, 2.0, 1.0]}, "script": {"module": "door"}}))
    project.put_entity(Entity("e000008", "Gate", None, {
        "transform": {"position": [6.0, 1.0, 0.0]}, "mesh": {"asset": "prim_cube"}}))
    write_script(project, "mover", MOVER)
    write_script(project, "door", DOOR)


def simulate(package, frames: int) -> tuple[dict, subprocess.CompletedProcess]:
    run = subprocess.run([str(scripting_runtime()), "--package", str(package), "--simulate",
                          str(frames), "--dump-scene"], capture_output=True, text=True,
                         timeout=60)
    lines = [line for line in run.stdout.splitlines() if line.startswith("{")]
    return (json.loads(lines[-1]) if lines else {}), run


def by_name(scene: dict, name: str) -> dict:
    return next(e for e in scene["entities"] if e["name"] == name)


@needs_scripting
def test_a_script_reacts_to_a_trigger_and_changes_the_world(project, tmp_path):
    """Phase 4's exit criterion, the engine half."""
    add_door(project)
    package = cook(project, tmp_path / "pkg").out_dir
    assert (package / "scripts" / "door.py").is_file()

    scene, run = simulate(package, 60)  # 1 s: the cube is at x = 2, short of the door
    assert run.returncode == 0, run.stderr
    assert by_name(scene, "Gate")["position"] == [6.0, 1.0, 0.0]

    scene, run = simulate(package, 120)  # 2 s: it walked in at x = 2.5
    assert by_name(scene, "Player")["position"][0] == pytest.approx(4.0, abs=0.05)
    assert by_name(scene, "Gate")["position"] == [0.0, 5.0, 0.0]
    assert scene["sky"] == [1.0, 0.0, 0.0]
    assert "opened by Player" in run.stdout


@needs_scripting
def test_script_errors_name_the_file_and_line(project, tmp_path):
    cube = project.entity("e000004")
    cube.components["script"] = {"module": "broken"}
    project.put_entity(cube)
    write_script(project, "broken", """
        def on_start(entity):
            pass

        def on_update(entity, dt):
            return 1 / 0
    """)
    package = cook(project, tmp_path / "pkg").out_dir
    _scene, run = simulate(package, 10)
    assert run.returncode == 0
    assert "broken.py" in run.stderr and "line 6" in run.stderr
    assert "ZeroDivisionError" in run.stderr
    assert run.stderr.count("ZeroDivisionError: division by zero") == 1  # paused, not repeated


@needs_scripting
def test_the_stub_matches_the_engine():
    """schema/midgard.pyi (the editor's completion) lists exactly what the engine defines."""
    import ast

    from midgard_editor import SCHEMA_DIR

    api = json.loads(subprocess.run([str(scripting_runtime()), "--dump-api"], capture_output=True,
                                    text=True, timeout=60, check=True).stdout)
    tree = ast.parse((SCHEMA_DIR / "midgard.pyi").read_text("utf-8"))
    module = sorted(n.name for n in tree.body if isinstance(n, (ast.FunctionDef, ast.ClassDef))
                    and not n.name.startswith("_"))
    entity = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == "Entity")
    members = sorted({n.name if isinstance(n, ast.FunctionDef) else n.target.id
                      for n in entity.body if isinstance(n, (ast.FunctionDef, ast.AnnAssign))}
                     - {"__eq__", "__hash__", "__repr__"})
    assert module == api["module"]
    assert members == api["Entity"]


@needs_scripting
def test_a_script_edited_while_playing_reloads(project, session_factory, qtbot):
    """Phase 4's exit criterion, the editor half: edited and reloaded without restarting."""
    add_door(project)
    write_script(project, "door", DOOR + "\ndef on_reload(entity):\n    print('door v1')\n")
    session = session_factory()
    received = []
    session.message_received.connect(received.append)
    # Headless: the real game loop and live link, without a window (CI has no display).
    session.start([str(scripting_runtime()), "--headless"], cook(project).out_dir)
    qtbot.waitUntil(lambda: any(m.get("text") == "opened by Player" for m in received),
                    timeout=20000)

    write_script(project, "door", DOOR + "\ndef on_reload(entity):\n    print('door v2')\n")
    qtbot.waitUntil(lambda: any(m.get("text") == "door v2" for m in received), timeout=10000)
    assert any(m.get("text") == "Reloaded script 'door'" for m in received)

    errors = []
    session.script_error.connect(errors.append)
    write_script(project, "door", "def on_reload(entity):\n    oops(\n")  # a syntax error
    qtbot.waitUntil(lambda: bool(errors), timeout=10000)
    assert errors[0]["file"] == "door.py" and errors[0]["line"] == 2


@pytest.fixture
def session_factory(project, qtbot):
    from midgard_editor.live_link import PlaySession

    sessions = []

    def make():
        sessions.append(PlaySession(project))
        return sessions[-1]

    yield make
    for s in sessions:
        s.stop()


def test_cooking_warns_about_missing_scripts(project, tmp_path):
    cube = project.entity("e000004")
    cube.components["script"] = {"module": "nowhere"}
    project.undo_stack.push(SetEntityCommand(project, cube, "script"))
    result = cook(project, tmp_path / "pkg")
    assert any("nowhere" in w for w in result.warnings)


PHYSICS = """
import midgard

def on_start(entity):
    hit = midgard.raycast((2, 10, 2), (0, -1, 0))
    midgard.log("hit", hit.entity.name, round(hit.point[1], 3) + 0.0, round(hit.normal[1], 3),
                round(hit.distance, 3))
    midgard.log("miss", midgard.raycast((50, 10, 50), (0, -1, 0)))
    midgard.log("near", [e.name for e in midgard.overlap_sphere((0, 0.5, 0), 0.6)])
    midgard.log("gravity", midgard.gravity((0, 0, 0)), "up", midgard.up((0, 0, 0)))
    entity.velocity = (4, 0, 0)  # sent sliding along the ground
    try:
        midgard.player().velocity = (1, 0, 0)
    except ValueError as e:
        midgard.log("refused:", e)

def on_update(entity, dt):
    if 0.5 < midgard.time() <= 0.5 + dt:
        midgard.log("standing", midgard.player().on_ground)
"""


@needs_scripting
def test_scripts_use_physics(project, tmp_path):
    """Raycasts, overlaps, gravity, velocity and the player's footing from a game script."""
    project.put_entity(Entity("e000050", "Crate", None, {
        "transform": {"position": [0.0, 0.5, -3.0]}, "mesh": {"asset": "prim_cube"},
        "collider": {"shape": "box"}, "rigidbody": {"motion": "dynamic"},
        "script": {"module": "physics_probe"}}))
    write_script(project, "physics_probe", PHYSICS)
    package = cook(project, tmp_path / "pkg").out_dir
    scene, run = simulate(package, 60)
    assert run.returncode == 0, run.stderr
    log = run.stdout
    assert "hit Ground 0.0 1.0 10.0" in log
    assert "miss None" in log
    assert "near ['Ground', 'Player']" in log
    assert "gravity (0.0, -9.81, 0.0) up (0.0, 1.0, 0.0)" in log
    assert "refused: the player walks as a character" in log
    assert "standing True" in log
    # Friction stops the crate after about 1.6 m (4 m/s, slowing at half of g).
    assert by_name(scene, "Crate")["position"][0] == pytest.approx(1.63, abs=0.25)

