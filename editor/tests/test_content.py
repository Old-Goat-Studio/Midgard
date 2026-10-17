# SPDX-License-Identifier: Apache-2.0
import json
import os
import sys

import pytest
from test_live_link import FAKE_RUNTIME, echoed
from test_materials import needs_encode
from test_objects import needs_import, png, write_crate

from midgard_editor import about, objects
from midgard_editor.cook import cook
from midgard_editor.live_link import PlaySession, State
from midgard_editor.objects import ObjectCache, ObjectDef, object_content
from midgard_editor.scene_model import Entity
from midgard_editor.textures import TextureCache
from midgard_editor.watch import ContentWatcher


def set_green(project, colour: list[float]) -> None:
    """Saves the crate model again with another colour for its green part."""
    file = project.objects_dir / "crate" / "crate.gltf"
    doc = json.loads(file.read_text("utf-8"))
    doc["materials"][1]["pbrMetallicRoughness"]["baseColorFactor"] = colour
    file.write_text(json.dumps(doc), "utf-8")


def touch_later(file) -> None:
    """A new modification time, as a save a moment later would give it."""
    s = file.stat()
    os.utime(file, ns=(s.st_atime_ns, s.st_mtime_ns + 2_000_000_000))


@pytest.fixture
def crate(project):
    write_crate(project.objects_dir / "crate")
    obj = ObjectDef("obj_crate", "Crate", "crate/crate.gltf")
    project.put_object(obj)
    return obj


@needs_import
def test_imports_are_kept_by_content_and_redone_only_when_it_changes(project, crate,
                                                                      monkeypatch):
    first = ObjectCache()
    green = first.cooked(project.objects_dir, crate).materials[1]["color"]
    assert first.imports == 1
    # A new editor session (an empty memory) finds it on disk.
    again = ObjectCache()
    assert again.cooked(project.objects_dir, crate).materials[1]["color"] == green
    assert again.imports == 0
    # Saved again unchanged: still nothing to do.
    touch_later(project.objects_dir / "crate" / "crate.gltf")
    assert again.cooked(project.objects_dir, crate).materials[1]["color"] == green
    assert again.imports == 0
    # A file it uses changed (the .gltf's image): imported again.
    (project.objects_dir / "crate" / "red.png").write_bytes(png((20, 230, 20, 255)))
    again.cooked(project.objects_dir, crate)
    assert again.imports == 1
    # Material changes never need the model imported again.
    changed = ObjectDef(crate.id, crate.name, crate.source,
                        {"materials": {"1": {"color": [1, 1, 1, 1]}}})
    again.cooked(project.objects_dir, changed)
    assert again.imports == 1
    # The cook uses the same store: cooking twice imports once.
    monkeypatch.setattr(objects, "CACHE", ObjectCache())
    project.put_entity(Entity("e000050", "Crate", None, {"object": {"id": "obj_crate"}}))
    cook(project, project.root / "build" / "a")
    cook(project, project.root / "build" / "b")
    assert objects.CACHE.imports == 0  # (already on disk from above)


@needs_encode
def test_textures_are_encoded_once_per_content(project, monkeypatch):
    (project.root / "textures").mkdir(exist_ok=True)
    image = project.root / "textures" / "bricks.png"
    image.write_bytes(png((200, 80, 60, 255)))
    cache = TextureCache()
    data = cache.data(project.root, "tex:textures/bricks.png")
    assert data[:4] == b"\xabKTX" and cache.encodes == 1
    assert TextureCache().data(project.root, "tex:textures/bricks.png") == data  # from disk
    linear = cache.data(project.root, "tex:textures/bricks.png?linear")  # another encoding
    assert linear != data and cache.encodes == 2
    image.write_bytes(png((10, 80, 60, 255)))
    assert cache.data(project.root, "tex:textures/bricks.png") != data
    assert cache.encodes == 3


@needs_import
def test_the_watcher_tells_what_changed_files_affect(project, crate, qtbot):
    (project.root / "textures").mkdir(exist_ok=True)
    (project.root / "textures" / "bricks.png").write_bytes(png((200, 80, 60, 255)))
    project.put_entity(Entity("e000050", "Wall", None, {
        "mesh": {"asset": "prim_cube"},
        "material": {"textures": {"base_color": "tex:textures/bricks.png"}}}))
    project.put_entity(Entity("e000051", "Plain", None, {"mesh": {"asset": "prim_cube"}}))
    watcher = ContentWatcher(project)
    files, objects_changed, entities = [], [], []
    project.files_changed.connect(files.append)
    project.objects_changed.connect(objects_changed.append)
    project.entities_changed.connect(entities.append)
    assert watcher.check() == set()

    # The model saved again with a new colour: the object is imported again.
    before = object_content(project, crate).materials[1]["color"]
    set_green(project, [0.9, 0.1, 0.8, 1.0])
    touch_later(project.objects_dir / "crate" / "crate.gltf")
    assert watcher.check() == {"objects/crate/crate.gltf"}
    assert objects_changed == ["obj_crate"] and entities == []
    assert object_content(project, crate).materials[1]["color"] != before
    # An image a material uses: the entities using it.
    (project.root / "textures" / "bricks.png").write_bytes(png((10, 80, 60, 255)))
    assert watcher.check() == {"textures/bricks.png"}
    assert entities == ["e000050"]
    # A new model: listed for the Objects panel.
    write_crate(project.objects_dir / "barrel")
    assert "objects/barrel/crate.gltf" in watcher.check()
    assert "objects/barrel/crate.gltf" in files[-1]


@needs_import
def test_the_running_game_gets_changed_objects(project, crate, qtbot, tmp_path):
    fake = tmp_path / "fake_runtime.py"
    fake.write_text(FAKE_RUNTIME, encoding="utf-8")
    received = []
    session = PlaySession(project)
    session.message_received.connect(received.append)
    try:
        session.start([sys.executable, str(fake)], cook(project, tmp_path / "pkg").out_dir)
        qtbot.waitUntil(lambda: session.state == State.PLAYING, timeout=15000)

        def puts():
            return [m for m in echoed(received) if m["type"] == "object.put"]

        # Placed while playing: the object (not in the package yet) goes first.
        project.put_entity(Entity("e000050", "Crate", None, {"object": {"id": "obj_crate"}}))
        qtbot.waitUntil(lambda: any(m["type"] == "entity.put" for m in echoed(received)),
                        timeout=5000)
        kinds = [m["type"] for m in echoed(received)]
        assert kinds.index("object.put") < kinds.index("entity.put")
        assert [p["name"] for p in puts()[-1]["parts"]] == ["red", "green", "collider"]
        assert any(m["type"] == "asset.changed" and m["id"] == "obj:obj_crate/0"
                   for m in echoed(received))
        # A setting changed: sent again.
        project.put_object(ObjectDef(crate.id, crate.name, crate.source, {"collider": "none"}))
        qtbot.waitUntil(lambda: len(puts()) == 2, timeout=5000)
        assert [p["name"] for p in puts()[-1]["parts"]] == ["red", "green"]
        # Nothing changed: nothing sent.
        project.put_object(ObjectDef(crate.id, "Big Crate", crate.source, {"collider": "none"}))
        qtbot.wait(300)
        assert len(puts()) == 2
        # Deleted: its instances empty.
        project.delete_object(crate.id)
        qtbot.waitUntil(lambda: len(puts()) == 3, timeout=5000)
        assert puts()[-1]["parts"] == []
    finally:
        session.stop()


@needs_import
@pytest.mark.skipif(not about.runtime_candidates(), reason="engine not built here")
def test_the_real_game_follows_a_model_saved_again(project, crate, qtbot, tmp_path):
    received = []
    session = PlaySession(project)
    session.message_received.connect(received.append)
    watcher = ContentWatcher(project)
    try:
        session.start(about.runtime_candidates()[0], cook(project, tmp_path / "pkg").out_dir)
        qtbot.waitUntil(lambda: session.state == State.PLAYING, timeout=20000)

        def applied(text: str) -> int:
            return sum(m.get("text") == text for m in received)

        project.put_entity(Entity("e000050", "Crate", None, {
            "transform": {"position": [0.0, 0.5, -2.0]}, "object": {"id": "obj_crate"}}))
        qtbot.waitUntil(lambda: applied("applied entity.put e000050"), timeout=10000)
        assert applied("applied object.put obj_crate") == 1

        set_green(project, [0.9, 0.1, 0.8, 1.0])
        touch_later(project.objects_dir / "crate" / "crate.gltf")
        watcher.check()
        qtbot.waitUntil(lambda: applied("applied object.put obj_crate") == 2, timeout=10000)
        assert applied("applied asset.changed obj:obj_crate/1")
        assert not [m for m in received if m.get("level") in ("warn", "error")], received
    finally:
        session.stop()
