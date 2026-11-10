# SPDX-License-Identifier: Apache-2.0
"""The Menu editor (milestone 6.0 step 6): screens laid out on a canvas the engine draws, with
undo; checked as the game is cooked; shown and heard by scripts (midgard.ui)."""
import json
import zipfile

import pytest
from PySide6.QtCore import QPointF
from test_scripting import needs_scripting, simulate, write_script

from midgard_editor import engine, menus
from midgard_editor.cook import CookError, cook
from midgard_editor.docks.menu_editor import MenuEditor
from midgard_editor.scene_model import Entity


@pytest.fixture
def editor(qtbot, project):
    e = MenuEditor()
    qtbot.addWidget(e)
    e.set_project(project)
    e.resize(1200, 700)
    return e


def element(project, screen_id: str, element_id: str) -> dict:
    s = menus.load(project, screen_id)
    return next(e for e in s["screen"]["elements"] if e["id"] == element_id)


def ids(project, screen_id: str) -> list[str]:
    return [e["id"] for e in menus.load(project, screen_id)["screen"]["elements"]]


def test_a_project_starts_with_no_screens(project):
    assert menus.has_table(project)
    assert menus.load_all(project) == []
    assert menus.problems(project) == []


def test_problems_are_said_plainly():
    assert menus.problems_of({"elements": [{"kind": "text"}]}) == ["has an element without an id"]
    assert menus.problems_of({"elements": [{"id": "a"}, {"id": "a"}]}) == [
        "has two elements called 'a'"]
    assert menus.problems_of({"elements": [{"id": "a", "parent": "b"}]}) == [
        "element 'a' is inside 'b', which isn't a panel before it"]
    assert menus.problems_of({"elements": [{"id": "a", "kind": "button",
                                            "action": {"do": "open", "screen": "scr_x"}}]},
                             {"scr_menu"}) == ["element 'a' opens 'scr_x', which isn't a screen"]
    assert menus.problems_of({"elements": [{"id": "a", "kind": "slider", "min": 2, "max": 1}]}) \
        == ["element 'a''s most isn't above its least"]
    assert menus.problems_of(menus.NEW_SCREEN) == []


def test_the_editor_makes_and_edits_screens_with_undo(editor, project):
    sid = editor.new_screen("Pause")
    assert sid == "scr_pause"
    s = menus.load(project, sid)
    assert s["name"] == "Pause" and s["screen"] == menus.NEW_SCREEN
    # The screen's own settings, with nothing chosen.
    assert editor.form.isRowVisible(editor.pauses)
    assert not editor.form.isRowVisible(editor.anchor)
    editor.pauses.setChecked(True)
    assert menus.load(project, sid)["screen"]["pauses"] is True
    # A button added inside the chosen panel; its settings.
    editor.choose_element("panel")
    assert editor.add_element("button") == "button"
    assert editor.chosen_element_id() == "button"
    assert element(project, sid, "button")["parent"] == "panel"
    assert editor.form.isRowVisible(editor.action)
    assert not editor.form.isRowVisible(editor.low)
    editor.text.setText("Resume")
    editor.text.editingFinished.emit()
    editor.y.setValue(-40)
    editor.anchor.setCurrentIndex(editor.anchor.findData("bottom"))
    editor.anchor.activated.emit(editor.anchor.currentIndex())
    editor.action.setCurrentIndex(editor.action.findData("close"))
    editor.action.activated.emit(editor.action.currentIndex())
    editor.font_size.setValue(24)
    b = element(project, sid, "button")
    assert (b["text"], b["y"], b["anchor"], b["action"], b["style"]["font_size"]) == \
        ("Resume", -40, "bottom", {"do": "close"}, 24)
    # Renamed: its id for scripts. Undo and redo.
    editor.element_id.setText("resume")
    editor.element_id.editingFinished.emit()
    assert element(project, sid, "resume")["text"] == "Resume"
    project.undo_stack.undo()
    assert element(project, sid, "button")
    project.undo_stack.redo()
    # A button that opens another screen.
    other = editor.new_screen("Settings")
    editor.choose_screen(sid)
    editor.choose_element("resume")
    editor.action.setCurrentIndex(editor.action.findData("open"))
    editor.action.activated.emit(editor.action.currentIndex())
    assert element(project, sid, "resume")["action"] == {"do": "open", "screen": other}
    assert editor.form.isRowVisible(editor.target)
    assert menus.problems(project) == []
    # Deleting the panel takes its parts; undo brings them back.
    editor.choose_element("panel")
    editor.delete_element()
    assert ids(project, sid) == []
    project.undo_stack.undo()
    assert ids(project, sid) == ["panel", "title", "close", "resume"]


def test_elements_move_up_and_down_with_their_parts(editor, project):
    sid = editor.new_screen("Menu")
    editor.choose_element("")
    editor.add_element("text")  # on the screen, after the panel
    editor.choose_element("text")
    editor.move_element(-1)  # before the panel (and its parts): drawn under it
    assert ids(project, sid) == ["text", "panel", "title", "close"]
    editor.choose_element("title")
    editor.move_element(1)  # after Close, inside the panel
    assert ids(project, sid) == ["text", "panel", "close", "title"]
    assert menus.problems(project) == []


def test_the_canvas_shows_the_screen_as_the_game_draws_it(editor, project):
    if engine.module() is None:
        pytest.skip("the engine isn't here to draw")
    sid = editor.new_screen("Menu")
    editor.render()
    if editor.canvas.image is None:
        pytest.skip(editor.canvas.message)
    assert editor.canvas.image.width() == 1280
    x, y, w, h = editor.canvas.boxes["panel"]  # 420 x 320, in the middle
    assert (round(x), round(y), round(w), round(h)) == (430, 200, 420, 320)
    # Dragging the Close button 30 dp right and 10 down: one undo step.
    editor.choose_element("close")
    steps = project.undo_stack.count()
    editor.canvas.dragged.emit("close", 10.0, 0.0, False)
    editor.canvas.dragged.emit("close", 30.0, 10.0, False)
    editor.canvas.dropped.emit()
    c = element(project, sid, "close")
    assert (c["x"], c["y"]) == (30, -18)
    assert project.undo_stack.count() == steps + 1
    # Resizing from the corner keeps its top left where it was (bottom-anchored: it moves).
    editor.canvas.dragged.emit("close", 20.0, 10.0, True)
    editor.canvas.dropped.emit()
    c = element(project, sid, "close")
    assert (c["w"], c["h"], c["x"], c["y"]) == (260, 66, 40, -8)
    # The phone held upright: 1.5 pixels a dp.
    editor.size_box.setCurrentIndex(2)
    assert editor.canvas.image.width() == 540
    assert editor.canvas.dp == 1.5
    # Clicking on the canvas chooses what's there.
    editor.size_box.setCurrentIndex(0)
    x, y, w, h = editor.canvas.boxes["title"]
    scale, off = editor.canvas._fit()
    assert editor.canvas.element_at(QPointF(off.x() + (x + w / 2) * scale,
                                            off.y() + (y + h / 2) * scale)) == "title"


def test_the_cook_warns_about_screens_and_refuses_one_it_cant_read(project, tmp_path):
    screens = [{"id": "scr_a", "name": "A", "screen": {"elements": [
        {"id": "go", "kind": "button", "action": {"do": "open", "screen": "scr_gone"}}]}}]
    menus.save_all(project, screens)
    result = cook(project, tmp_path / "pkg")
    assert any("opens 'scr_gone'" in w for w in result.warnings)
    if engine.module() is None:
        return
    screens.append({"id": "scr_b", "name": "Broken", "screen": {"elements": [
        {"id": "x", "kind": "video"}]}})
    menus.save_all(project, screens)
    with pytest.raises(CookError, match="Broken"):
        cook(project, tmp_path / "pkg2")


SCREEN_SCRIPT = """
import midgard

def on_start(entity):
    midgard.ui.on_click("scr_pause", "resume", lambda: print("clicked resume"))
    midgard.ui.show("scr_pause")
    print("shown", midgard.ui.shown("scr_pause"), midgard.ui.screens)
    midgard.ui.set_text("scr_pause", "title", "Paused!")
    print("volume", midgard.ui.value("scr_pause", "volume"))
    midgard.ui.set_value("scr_pause", "volume", 2.0)
    print("volume", midgard.ui.value("scr_pause", "volume"), midgard.ui.value("scr_pause", "subs"))
    print("enabled", midgard.ui.enabled("scr_pause", "resume"))
    midgard.ui.set_enabled("scr_pause", "resume", False)
    print("enabled", midgard.ui.enabled("scr_pause", "resume"))
    midgard.ui.set_enabled("scr_pause", "resume", True)
    print("hard", midgard.ui.value("scr_pause", "hard"))
    midgard.ui.set_value("scr_pause", "easy", True)
    value = midgard.ui.value
    print("easy", value("scr_pause", "easy"), "hard", value("scr_pause", "hard"))
    midgard.ui.click("scr_pause", "resume")
    print("after click", midgard.ui.screens)
    for bad in (lambda: midgard.ui.show("scr_nothing"),
                lambda: midgard.ui.on_click("scr_pause", "volume", print)):
        try:
            bad()
        except (KeyError, ValueError) as e:
            print("refused", type(e).__name__)

def on_update(entity, dt):
    pass
"""


@needs_scripting
def test_scripts_show_screens_and_hear_their_buttons(project, tmp_path):
    menus.save_all(project, [{"id": "scr_pause", "name": "Pause", "screen": {"elements": [
        {"id": "title", "kind": "text", "text": "Paused"},
        {"id": "resume", "kind": "button", "action": {"do": "close"}},
        {"id": "volume", "kind": "slider", "min": 0, "max": 1, "value": 0.5},
        {"id": "subs", "kind": "check", "value": True},
        {"id": "easy", "kind": "option", "group": "level"},
        {"id": "hard", "kind": "option", "group": "level", "value": True}]}}])
    project.put_entity(Entity("e000091", "Menus", None, {
        "transform": {}, "script": {"module": "menus_script"}}))
    write_script(project, "menus_script", SCREEN_SCRIPT)
    package = cook(project, tmp_path / "pkg").out_dir
    dumped, run = simulate(package, 5)
    assert run.returncode == 0, run.stderr
    lines = run.stdout.splitlines()
    assert "shown True ['scr_pause']" in lines
    assert "volume 0.5" in lines
    assert "volume 1.0 True" in lines
    assert "after click []" in lines
    assert lines.count("enabled True") == 1 and "enabled False" in lines
    assert "hard True" in lines and "easy True hard False" in lines
    assert lines.count("refused KeyError") == 1 and lines.count("refused ValueError") == 1
    assert "clicked resume" in lines
    assert dumped["hud"]["screens"] == []


def test_the_live_message_has_every_screen(project):
    menus.save_all(project, [{"id": "scr_a", "name": "A", "screen": menus.NEW_SCREEN}])
    message = {"type": "screens.put", "screens": menus.live_message(project)}
    assert json.loads(json.dumps(message))["screens"][0]["id"] == "scr_a"


def test_any_element_shows_a_picture_from_the_gui_section(editor, project, tmp_path):
    from PySide6.QtGui import QColor, QImage

    from midgard_editor import asset_library

    picture = tmp_path / "button_frame.png"
    image = QImage(64, 24, QImage.Format.Format_RGBA8888)
    image.fill(QColor(200, 40, 40))
    assert image.save(str(picture))
    asset = asset_library.import_asset(project, [picture], "gui", "Red Frame",
                                       licence="CC-BY-4.0", author="Someone",
                                       title="Red Frame")
    found = menus.gui_images(project)
    assert len(found) == 1
    ref, label = next(iter(found.items()))
    assert ref.startswith("ui:library/") and ref.endswith("button_frame.png")
    assert label == "Red Frame (button_frame.png)"
    assert menus.image_file(project, ref) is not None
    assert menus.image_file(project, "ui:../outside.png") is None

    # In the Menu editor: the Close button gets the picture; a Picture element too.
    sid = editor.new_screen("Menu")
    editor.choose_element("close")
    assert editor.form.isRowVisible(editor.image)
    editor.image.setCurrentIndex(editor.image.findData(ref))
    editor.image.activated.emit(editor.image.currentIndex())
    assert element(project, sid, "close")["image"] == ref
    editor.choose_element("panel")
    pic = editor.add_element("image")
    assert element(project, sid, pic)["kind"] == "image"
    editor.image.setCurrentIndex(editor.image.findData(ref))
    editor.image.activated.emit(editor.image.currentIndex())
    assert menus.images_of(menus.load(project, sid)["screen"]) == [ref]
    assert menus.image_files(project, menus.load_all(project)) == {
        ref: str(menus.image_file(project, ref))}
    assert menus.problems(project) == []
    # None takes it off.
    editor.choose_element("close")
    editor.image.setCurrentIndex(0)
    editor.image.activated.emit(0)
    assert "image" not in element(project, sid, "close")

    # The canvas draws it: the picture element is red where it sits.
    if engine.module() is not None:
        editor.render()
        if editor.canvas.image is not None:
            x, y, w, h = editor.canvas.boxes[pic]
            colour = editor.canvas.image.pixelColor(int(x + w / 2), int(y + h / 2))
            assert colour.red() > 150 and colour.green() < 90

    # The cook packs it as it is, and credits its author.
    result = cook(project, tmp_path / "pkg")
    with zipfile.ZipFile(result.out_dir / "game.pak") as pak:
        names = pak.namelist()
    assert "ui/" + ref[3:] in names
    assert asset in asset_library.used_by(project, [ref])
    # A picture that's gone is a warning, and the screen draws without it.
    element_ref = "ui:library/gui/gone.png"
    screens = menus.load_all(project)
    screens[0]["screen"]["elements"][0]["image"] = element_ref
    menus.save_all(project, screens)
    assert any("gone.png" in p and "isn't in the project" in p for p in menus.problems(project))


def test_buttons_checks_options_and_sliders_have_a_picture_for_each_state(editor, project,
                                                                            tmp_path):
    from PySide6.QtGui import QColor, QImage

    from midgard_editor import asset_library

    refs = {}
    for name, colour in (("normal", "green"), ("hover", "yellow"), ("pressed", "orange"),
                         ("disabled", "grey"), ("off", "black"), ("on", "white"),
                         ("track", "blue"), ("pointer", "red"), ("backdrop", "purple")):
        file = tmp_path / f"{name}.png"
        image = QImage(32, 32, QImage.Format.Format_RGBA8888)
        image.fill(QColor(colour))
        assert image.save(str(file))
        asset_library.import_asset(project, [file], "gui", name.title())
    for ref in menus.gui_images(project):
        refs[ref.rsplit("/", 1)[-1][:-4]] = ref
    sid = editor.new_screen("Options")

    def pick(slot: str, name: str) -> None:
        box = editor.pictures[slot]
        assert editor.form.isRowVisible(box), slot
        box.setCurrentIndex(box.findData(refs[name]))
        box.activated.emit(box.currentIndex())

    # The screen's own picture.
    editor.choose_element("")
    pick("screen_image", "backdrop")
    assert menus.load(project, sid)["screen"]["image"] == refs["backdrop"]
    # A button: normal, hover, pressed and disabled.
    editor.choose_element("close")
    assert editor.form.labelForField(editor.image).text() == "Normal picture"
    assert not editor.form.isRowVisible(editor.pictures["track"])
    for slot, name in (("image", "normal"), ("hover", "hover"), ("pressed", "pressed"),
                       ("disabled", "disabled")):
        pick(slot, name)
    editor.enabled.setChecked(False)
    close = element(project, sid, "close")
    assert close["image"] == refs["normal"] and close["enabled"] is False
    assert close["images"] == {"hover": refs["hover"], "pressed": refs["pressed"],
                               "disabled": refs["disabled"]}
    editor.enabled.setChecked(True)
    assert "enabled" not in element(project, sid, "close")
    # A check box and option buttons: unchecked and checked.
    editor.choose_element("panel")
    check = editor.add_element("check")
    assert not editor.form.isRowVisible(editor.image)
    pick("unchecked", "off")
    pick("checked", "on")
    assert element(project, sid, check)["images"] == {"unchecked": refs["off"],
                                                      "checked": refs["on"]}
    editor.choose_element("panel")
    easy = editor.add_element("option")
    assert editor.form.labelForField(editor.pictures["checked"]).text() == "Selected picture"
    editor.group.setText("difficulty")
    editor.group.editingFinished.emit()
    pick("unchecked", "off")
    editor.ticked.setChecked(True)
    option = element(project, sid, easy)
    assert option["group"] == "difficulty" and option["value"] is True
    # A slider: its background and its pointer.
    editor.choose_element("panel")
    slider = editor.add_element("slider")
    pick("track", "track")
    pick("pointer", "pointer")
    assert element(project, sid, slider)["images"] == {"track": refs["track"],
                                                       "pointer": refs["pointer"]}
    # None takes a state's picture off.
    editor.pictures["pointer"].setCurrentIndex(0)
    editor.pictures["pointer"].activated.emit(0)
    assert element(project, sid, slider)["images"] == {"track": refs["track"]}
    # Every picture is packed; nothing's wrong.
    assert menus.problems(project) == []
    assert set(menus.images_of(menus.load(project, sid)["screen"])) == \
        {refs[n] for n in ("backdrop", "normal", "hover", "pressed", "disabled", "off", "on",
                           "track")}
    assert menus.problems_of({"elements": [{"id": "a", "kind": "text",
                                            "images": {"hover": refs["hover"]}}]}) == [
        "element 'a' is a text, which has no 'hover' picture"]
    if engine.module() is not None:
        editor.render()
        result = cook(project, tmp_path / "pkg")
        with zipfile.ZipFile(result.out_dir / "game.pak") as pak:
            assert "ui/" + refs["track"][3:] in pak.namelist()


def test_ready_made_screens_work_as_they_are(editor, project):
    # A title screen brings the settings screen its Settings button opens.
    title = editor.new_from_template("title")
    screens = menus.load_all(project)
    assert [s["screen"].get("role") for s in screens] == ["title", "settings"]
    assert editor.chosen_screen_id() == title
    settings = screens[1]["id"]
    assert element(project, title, "settings")["action"] == {"do": "open", "screen": settings}
    # A pause menu uses that settings screen; game over respawns.
    pause = editor.new_from_template("pause")
    assert element(project, pause, "settings")["action"]["screen"] == settings
    over = editor.new_from_template("game_over")
    assert element(project, over, "again")["action"] == {"do": "respawn"}
    assert menus.load(project, over)["screen"]["stat"] == "health"
    assert len(menus.load_all(project)) == 4
    # A second pause menu is a spare: only one screen is the pause menu.
    spare = editor.new_from_template("pause")
    assert "role" not in menus.load(project, spare)["screen"]
    assert menus.problems(project) == []
    # The settings: sliders tied to volumes, buttons to keys.
    assert element(project, settings, "music")["binds"] == "volume:music"
    assert element(project, settings, "jump")["binds"] == "key:jump"
    editor.choose_screen(settings)
    editor.choose_element("music")
    assert editor.form.isRowVisible(editor.binds)
    assert editor.binds.currentData() == "volume:music"
    editor.binds.setCurrentIndex(editor.binds.findData("volume:effects"))
    editor.binds.activated.emit(editor.binds.currentIndex())
    assert element(project, settings, "music")["binds"] == "volume:effects"
    editor.choose_element("use")
    assert editor.binds.findData("key:descend") >= 0
    # The screen's role, and game over's stat, in its settings.
    editor.choose_screen(over)
    editor.choose_element("")
    assert editor.role.currentData() == "game_over"
    assert editor.form.isRowVisible(editor.stat)
    assert editor.stat.currentData() == "health"
    editor.choose_screen(title)
    editor.choose_element("")
    assert not editor.form.isRowVisible(editor.stat)
    # Two screens with one role: said.
    editor.role.setCurrentIndex(editor.role.findData("pause"))
    editor.role.activated.emit(editor.role.currentIndex())
    assert any("both the pause menu" in p for p in menus.problems(project))
    project.undo_stack.undo()
    # Drawn by the engine at every preview size: nothing too small, unreachable or cut off.
    if engine.module() is None:
        return
    from midgard_editor.docks.menu_editor import screen_preview
    for kind, (_name, screen) in menus.TEMPLATES.items():
        made = json.loads(json.dumps(screen).replace("{settings}", "scr_settings"))
        for label, w, h, scale, safe in menus.PREVIEWS:
            try:
                _pixels, _boxes, checks = screen_preview().render(
                    json.dumps(made), w, h, scale, list(safe), scale > 1.0)
            except RuntimeError as e:
                pytest.skip(str(e))
            assert checks == [], (kind, label, checks)


def test_the_project_page_sets_the_splash_crosshair_pointer_and_menu_music(qtbot, project,
                                                                            tmp_path):
    from PySide6.QtGui import QColor, QImage

    from midgard_editor import asset_library, music
    from midgard_editor.docks.project_page import ProjectPage

    for name, colour in (("splash", "white"), ("aim", "red"), ("arrow", "blue")):
        file = tmp_path / f"{name}.png"
        image = QImage(32, 32, QImage.Format.Format_RGBA8888)
        image.fill(QColor(colour))
        assert image.save(str(file))
        asset_library.import_asset(project, [file], "gui", name.title())
    refs = {r.rsplit("/", 1)[-1][:-4]: r for r in menus.gui_images(project)}
    music.put(project, {"id": "mus_menu", "name": "Menu Theme", "playlist": {"tracks": []}})
    page = ProjectPage()
    qtbot.addWidget(page)
    page.set_project(project)

    def pick(box, data) -> None:
        box.setCurrentIndex(box.findData(data))
        box.activated.emit(box.currentIndex())

    pick(page.splash, refs["splash"])
    pick(page.crosshair, refs["aim"])
    pick(page.crosshair_views, "all")
    page.crosshair_size.setValue(48)
    page.crosshair_size.editingFinished.emit()
    pick(page.pointer, refs["arrow"])
    assert page.menu_music.findText("Menu Theme") >= 0
    pick(page.menu_music, "mus_menu")
    assert menus.game_settings(project) == {
        "splash": refs["splash"], "crosshair": refs["aim"], "crosshair_views": "all",
        "crosshair_size": 48, "pointer": refs["arrow"], "menu_music": "mus_menu"}
    # One undo step each; back to the defaults, they're left out.
    project.undo_stack.undo()
    assert "menu_music" not in menus.game_settings(project)
    pick(page.crosshair_views, "first")
    assert "crosshair_views" not in menus.game_settings(project)
    pick(page.menu_music, "mus_menu")
    assert menus.settings_problems(project) == []
    # The cook packs the pictures; one that's gone, or a playlist that's gone, is said.
    result = cook(project, tmp_path / "pkg")
    with zipfile.ZipFile(result.out_dir / "game.pak") as pak:
        names = pak.namelist()
    for name in ("splash", "aim", "arrow"):
        assert "ui/" + refs[name][3:] in names
    settings = menus.game_settings(project)
    menus.save_game_settings(project, {**settings, "pointer": "ui:library/gone.png",
                                       "menu_music": "mus_gone"})
    found = menus.settings_problems(project)
    assert any("menu pointer" in p and "gone.png" in p for p in found)
    assert any("mus_gone" in p for p in found)
    # A screen can keep the menu music off.
    editor = MenuEditor()
    qtbot.addWidget(editor)
    editor.set_project(project)
    sid = editor.new_screen("Map")
    assert editor.form.isRowVisible(editor.music) and editor.music.isChecked()
    editor.music.setChecked(False)
    assert menus.load(project, sid)["screen"]["music"] is False


def test_pictures_imported_later_are_in_the_lists_when_they_open(qtbot, project, tmp_path):
    """The Project Page (or the Menu editor) open first, a picture imported into the GUI
    section afterwards: opening a list offers it."""
    from PySide6.QtGui import QColor, QImage

    from midgard_editor import asset_library
    from midgard_editor.docks.project_page import ProjectPage

    page = ProjectPage()
    qtbot.addWidget(page)
    page.set_project(project)
    editor = MenuEditor()
    qtbot.addWidget(editor)
    editor.set_project(project)
    editor.new_screen("Menu")
    editor.choose_element("close")
    assert page.splash.count() == 1 and editor.image.count() == 1  # (None only)
    file = tmp_path / "logo.png"
    image = QImage(16, 16, QImage.Format.Format_RGBA8888)
    image.fill(QColor("green"))
    assert image.save(str(file))
    asset_library.import_asset(project, [file], "gui", "Logo")
    for box in (page.splash, page.crosshair, page.pointer, editor.image):
        box.showPopup()
        box.hidePopup()
        assert box.findText("Logo (logo.png)") >= 0
