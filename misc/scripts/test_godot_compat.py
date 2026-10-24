#!/usr/bin/env python3
"""Check that Godot 4.7 projects work in Asosdot as in Godot 4.7, and back.

A project class with the name of an Asosdot class must take the name, as in Godot 4.7. The
3D viewport's display mode and the selected main screen, which the editor saves with the
project, must open the same in both. With --godot, the check runs Godot 4.7 itself for that.
"""

from __future__ import annotations

import argparse
import json
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

# A project class with the name of an Asosdot class, as a lens flare add-on has.
LENS_FLARE = """@tool
class_name LensFlare
extends Node3D

@export var strength := 1.0

func describe() -> String:
	return "project lens flare %s" % strength
"""

USES = """extends Node

# A typed use of the project's class: it must be the project's, not a native class.
var flare: LensFlare

func make() -> LensFlare:
	flare = LensFlare.new()
	flare.strength = 2.0
	return flare
"""

SCENE = """[gd_scene load_steps=2 format=3]

[ext_resource type="Script" path="res://lens_flare.gd" id="1"]

[node name="World" type="Node3D"]

[node name="Flare" type="Node3D" parent="."]
script = ExtResource("1")
strength = 3.0

[node name="Box" type="CSGBox3D" parent="."]
"""

RUNTIME = r"""
extends SceneTree

var failures := []

func check(condition: bool, message: String) -> void:
	if not condition:
		failures.append(message)

func _initialize():
	# The project's LensFlare takes the name; Asosdot's whole lens flare group steps aside.
	check(not ClassDB.class_exists("LensFlare"), "LensFlare must be the project's script class")
	check(not ClassDB.class_exists("LensFlare3D"), "Asosdot's lens flare group must step aside together")
	check(ClassDB.class_exists("Rope3D"), "Asosdot's other features stay")
	var world = (load("res://world.tscn") as PackedScene).instantiate()
	root.add_child(world)
	check(world.get_node("Flare").describe() == "project lens flare 3.0", "The scene's LensFlare is the project's")
	var uses = load("res://uses.gd").new()
	check(uses.make().describe() == "project lens flare 2.0", "Typed code gets the project's LensFlare")
	uses.free()
	for failure in failures:
		printerr("COMPAT FAILED: " + failure)
	if failures.is_empty():
		print("COMPAT_RUNTIME_OK")
	quit()
"""

# Runs in the editor, in Asosdot or Godot 4.7: sets or reads the 3D display mode and the main screen.
EDITOR = r"""
@tool
extends EditorPlugin

func _enter_tree():
	call_deferred("run")

func frames(count):
	for i in count:
		await get_tree().process_frame

func display_menu() -> PopupMenu:
	var viewport = EditorInterface.get_editor_viewport_3d(0).get_parent().get_parent()
	for button in viewport.find_children("*", "MenuButton", true, false):
		var popup = button.get_popup()
		for i in popup.item_count:
			if popup.get_item_text(i) == "Display Wireframe":
				return popup
	return null

func run():
	var config = JSON.parse_string(FileAccess.get_file_as_string("res://editor.json"))
	await frames(20)
	if config.step == "set":
		EditorInterface.open_scene_from_path("res://world.tscn")
		EditorInterface.set_main_screen_editor("3D")
		await frames(10)
		var popup = display_menu()
		for i in popup.item_count:
			if popup.get_item_text(i) == config.display:
				popup.id_pressed.emit(popup.get_item_id(i))
		await frames(10)
		EditorInterface.save_scene()
		EditorInterface.set_main_screen_editor(config.screen)
		await frames(60)
		print("EDITOR_SET_OK")
		# Closing the window the way a user does saves the editor layout and scene state.
		EditorInterface.get_base_control().get_parent().notification(NOTIFICATION_WM_CLOSE_REQUEST)
		return
	else:
		await frames(60)
		var draw = EditorInterface.get_editor_viewport_3d(0).debug_draw
		var popup = display_menu()
		var checked = ""
		for i in popup.item_count:
			if popup.is_item_checked(i) and popup.get_item_text(i).begins_with("Display "):
				checked = popup.get_item_text(i)
		var screen = ""
		for button in EditorInterface.get_base_control().find_children("*", "Button", true, false):
			if button.toggle_mode and button.button_pressed and button.text in ["2D", "3D", "Script", "Game", "Render", "AssetLib", "Asset Store"]:
				screen = button.text
		print("EDITOR_STATE display=%s debug_draw=%d screen=%s" % [checked, draw, screen])
		print("EDITOR_READ_OK")
	get_tree().quit()
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--godot", type=Path, help="A Godot 4.7 editor, to check editor state saved by each")
    parser.add_argument("--temp-dir", type=Path)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()
    parent = (args.temp_dir or Path(tempfile.gettempdir())).resolve()
    work = Path(tempfile.mkdtemp(prefix="asosdot-compat-", dir=parent))
    print(f"Test workspace: {work}", flush=True)
    try:
        engines = {}
        for name, binary in (("asosdot", args.engine), ("godot", args.godot)):
            if binary is None:
                continue
            folder = work / f"runtime_{name}"
            folder.mkdir()
            shutil.copy2(binary.resolve(), folder / binary.name)
            (folder / "_sc_").touch()
            engines[name] = folder / binary.name
        project = work / "project"
        project.mkdir()
        files = {"lens_flare.gd": LENS_FLARE, "uses.gd": USES, "world.tscn": SCENE, "runtime.gd": RUNTIME}
        for name, text in files.items():
            (project / name).write_text(text, encoding="utf-8")
        addon = project / "addons/check"
        addon.mkdir(parents=True)
        (addon / "plugin.cfg").write_text('[plugin]\nname="Compat check"\nscript="plugin.gd"\n', encoding="utf-8")
        (addon / "plugin.gd").write_text(EDITOR, encoding="utf-8")
        (project / "project.godot").write_text(
            'config_version=5\n[application]\nconfig/name="Compat check"\nconfig/features=PackedStringArray("4.7", "Forward Plus")\n'
            "[debug]\nfile_logging/enable_file_logging=false\n",
            encoding="utf-8",
        )

        def editor(engine: str, step: str, display: str = "", screen: str = "", plugin: bool = True) -> str:
            (project / "editor.json").write_text(json.dumps({"step": step, "display": display, "screen": screen}), encoding="utf-8")
            text = (project / "project.godot").read_text(encoding="utf-8").split("[editor_plugins]")[0]
            if plugin:
                text += '[editor_plugins]\nenabled=PackedStringArray("res://addons/check/plugin.cfg")\n'
            (project / "project.godot").write_text(text, encoding="utf-8")
            marker = {"set": "EDITOR_SET_OK", "read": "EDITOR_READ_OK", "scan": None}[step]
            command = [str(engines[engine]), "--path", str(project), "--editor"]
            if step == "scan":
                command += ["--headless", "--quit-after", "300"]
            else:
                command += ["--rendering-driver", "opengl3", "--audio-driver", "Dummy", "--resolution", "1280x800"]
            run(command, marker, work)
            log = (work / f"{marker or 'EDITOR_IMPORT'}.log").read_text(encoding="utf-8", errors="replace")
            (work / f"{engine}_{step}.log").write_text(log, encoding="utf-8")
            return log

        # A fresh project, with no class cache yet: Asosdot finds the LensFlare script class itself.
        log = editor("asosdot", "scan", plugin=False)
        assert "hides a native class" not in log and "SCRIPT ERROR" not in log, log
        assert "takes the name as in Godot 4.7" in log, "Asosdot should say which feature it leaves out"
        run([str(engines["asosdot"]), "--path", str(project), "--headless", "--script", "runtime.gd"], "COMPAT_RUNTIME_OK", work)
        print("Script class takes the name: ok", flush=True)

        if "godot" in engines:
            # Each way, the editor state one saves must open the same in the other.
            # Godot 4.7 has no Render screen: it opens 3D there, and Asosdot opens Render again after.
            cases = (
                ("godot", "asosdot", "Display Wireframe", "Script", "Script"),
                ("asosdot", "godot", "Display Overdraw", "2D", "2D"),
                ("asosdot", "godot", "Display Unshaded", "Render", "3D"),
            )
            for writer, reader, display, screen, expected in cases:
                editor(writer, "set", display, screen)
                log = editor(reader, "read")
                state = next(line for line in log.splitlines() if line.startswith("EDITOR_STATE"))
                print(f"{writer} saved {display}, {screen}; {reader} opens: {state}", flush=True)
                assert f"display={display}" in state, state
                assert f"screen={expected}" in state, state
            log = editor("asosdot", "read")
            state = next(line for line in log.splitlines() if line.startswith("EDITOR_STATE"))
            print(f"asosdot reopens: {state}", flush=True)
            assert "screen=Render" in state, state
        print("GODOT_COMPAT_OK", flush=True)
    finally:
        if not args.keep:
            # Only remove the specific directory created for this check.
            assert work.resolve().parent == parent and work.name.startswith("asosdot-compat-")
            shutil.rmtree(work)


if __name__ == "__main__":
    main()
