#!/usr/bin/env python3
"""Check exposed children: nodes a scene exposes are shown and saved in scenes that instance it, without Editable Children."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

GUN = """[gd_scene format=3]

[node name="Gun" type="Node3D"]

[node name="Barrel" type="Node3D" parent="."]

[node name="Muzzle" type="Node3D" parent="Barrel"]
exposed_in_instances = true
"""

# Hand and Anim are exposed, Body and Secret aren't, and the Gun instance is exposed with
# what its own scene exposes. The meshes give the viewport something to click.
PLAYER = """[gd_scene load_steps=4 format=3]

[ext_resource type="PackedScene" path="res://gun.tscn" id="1"]

[sub_resource type="BoxMesh" id="BoxMesh_body"]
size = Vector3(1, 2, 1)

[sub_resource type="BoxMesh" id="BoxMesh_hand"]
size = Vector3(0.6, 0.6, 0.6)

[node name="Player" type="Node3D"]

[node name="Body" type="Node3D" parent="."]

[node name="BodyMesh" type="MeshInstance3D" parent="Body"]
mesh = SubResource("BoxMesh_body")

[node name="Hand" type="Node3D" parent="Body"]
transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 1.5, 0, 0)
exposed_in_instances = true

[node name="HandMesh" type="MeshInstance3D" parent="Body/Hand"]
mesh = SubResource("BoxMesh_hand")

[node name="Anim" type="AnimationPlayer" parent="."]
exposed_in_instances = true

[node name="Secret" type="Node3D" parent="."]

[node name="Gun" parent="Body" instance=ExtResource("1")]
exposed_in_instances = true
"""

LEVEL = """[gd_scene load_steps=3 format=3]

[ext_resource type="Script" path="res://level.gd" id="1"]
[ext_resource type="PackedScene" path="res://player.tscn" id="2"]

[node name="Level" type="Node3D"]
script = ExtResource("1")

[node name="Player" parent="." instance=ExtResource("2")]
"""

# The same in 2D: Hand is exposed, Body isn't.
PLAYER_2D = """[gd_scene format=3]

[node name="Player2D" type="Node2D"]

[node name="Body" type="Node2D" parent="."]

[node name="BodyShape" type="Polygon2D" parent="Body"]
polygon = PackedVector2Array(-50, -100, 50, -100, 50, 100, -50, 100)

[node name="Hand" type="Node2D" parent="Body"]
position = Vector2(150, 0)
exposed_in_instances = true

[node name="HandShape" type="Polygon2D" parent="Body/Hand"]
polygon = PackedVector2Array(-40, -40, 40, -40, 40, 40, -40, 40)
"""

LEVEL_2D = """[gd_scene load_steps=2 format=3]

[ext_resource type="PackedScene" path="res://player2d.tscn" id="1"]

[node name="Level2D" type="Node2D"]

[node name="Player2D" parent="." instance=ExtResource("1")]
position = Vector2(300, 250)
"""

LEVEL_SCRIPT = """extends Node3D

func _on_anim_finished(_name: StringName) -> void:
	pass
"""

CHECK = r"""
@tool
extends EditorPlugin

var config: Dictionary

func _enter_tree():
	call_deferred("run")

func frames(count: int) -> void:
	for i in count:
		await get_tree().process_frame

func dock() -> Node:
	return EditorInterface.get_base_control().find_children("*", "SceneTreeDock", true, false)[0]

func tree_editor() -> Node:
	for editor in dock().find_children("*", "SceneTreeEditor", true, false):
		if editor.is_visible_in_tree():
			return editor
	return null

# The Scene dock as text: one line per item, indented by depth.
func outline(item: TreeItem = null, depth := 0) -> Array:
	if item == null:
		item = (tree_editor().find_children("*", "Tree", true, false)[0] as Tree).get_root()
	var lines := ["  ".repeat(depth) + item.get_text(0)]
	for child in item.get_children():
		lines.append_array(outline(child, depth + 1))
	return lines

func item_for(name: String, item: TreeItem = null) -> TreeItem:
	if item == null:
		item = (tree_editor().find_children("*", "Tree", true, false)[0] as Tree).get_root()
	if item.get_text(0) == name:
		return item
	for child in item.get_children():
		var found = item_for(name, child)
		if found:
			return found
	return null

func select(node: Node) -> void:
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(node)
	await frames(3)

# Opens the Scene dock's context menu for the selection and picks an item by its text.
func menu_pick(text: String) -> void:
	tree_editor().emit_signal("rmb_pressed", Vector2(40, 40))
	await frames(2)
	for popup in dock().find_children("*", "PopupMenu", true, false):
		if not popup.visible:
			continue
		for i in popup.item_count:
			if popup.get_item_text(i) == text:
				var id = popup.get_item_id(i)
				popup.hide()
				popup.id_pressed.emit(id)
				await frames(3)
				return
	assert(false, "No menu item " + text)

func confirm_dialog() -> void:
	await frames(2)
	for dialog in dock().find_children("*", "ConfirmationDialog", true, false):
		if dialog.visible:
			dialog.hide()
			dialog.confirmed.emit()
			await frames(3)
			return
	assert(false, "No confirmation dialog")

func set_property(node: Node, property: String, value) -> void:
	var undo_redo = EditorInterface.get_editor_undo_redo()
	undo_redo.create_action("Set " + property)
	undo_redo.add_do_property(node, property, value)
	undo_redo.add_undo_property(node, property, node.get(property))
	undo_redo.commit_action()

# Clicks the 3D viewport on a point of a node, as a press and release without dragging. Off its
# center, where a selected node's move gizmo would take the click.
func click(node: Node3D, offset: Vector3) -> void:
	var viewport = EditorInterface.get_editor_viewport_3d(0)
	var camera = viewport.get_camera_3d()
	var surface: Control
	for child in viewport.get_parent().get_parent().get_children():
		if child.get_class() == "Control":
			surface = child
	var position = camera.unproject_position(node.global_position + offset)
	for pressed in [true, false]:
		var event = InputEventMouseButton.new()
		event.button_index = MOUSE_BUTTON_LEFT
		event.pressed = pressed
		event.position = position
		event.global_position = position
		surface.gui_input.emit(event)
		await frames(2)

# Clicks the 2D viewport at a point of the scene, as a press and release without dragging.
func click_2d(point: Vector2) -> void:
	var control: Control = EditorInterface.get_base_control().find_children("*", "CanvasItemEditorViewport", true, false)[0]
	var position = EditorInterface.get_editor_viewport_2d().global_canvas_transform * point
	for pressed in [true, false]:
		var event = InputEventMouseButton.new()
		event.button_index = MOUSE_BUTTON_LEFT
		event.pressed = pressed
		event.position = position
		event.global_position = position
		control.gui_input.emit(event)
		await frames(2)

func selected() -> String:
	var nodes = EditorInterface.get_selection().get_selected_nodes()
	return "" if nodes.is_empty() else String(nodes[0].name)

func run():
	config = JSON.parse_string(FileAccess.get_file_as_string("res://check.json"))
	await frames(10)
	var phase: String = config.phase
	if phase == "scan":
		pass
	elif phase == "source":
		await check_source()
	else:
		EditorInterface.open_scene_from_path("res://level.tscn")
		await frames(10)
		if phase == "level":
			await check_level()
		elif phase == "kept":
			await check_kept()
		elif phase == "editable":
			await check_editable_children()
		elif phase == "click":
			await check_clicks()
			await check_clicks_2d()
	print("EXPOSED_OK")
	get_tree().quit()

func check_level():
	var root = EditorInterface.get_edited_scene_root()
	var player = root.get_node("Player")
	var lines = outline()
	print("Scene dock:\n" + "\n".join(lines))
	# Only what the scenes expose shows under the instance, with nothing in between.
	for name in ["Hand", "Anim", "Gun", "Muzzle"]:
		assert(item_for(name) != null, name + " is exposed and must show")
		assert(root.is_node_editable(player.find_child(name, true, false)), name + " must be editable here")
	for name in ["Body", "Secret", "Barrel", "BodyMesh", "HandMesh"]:
		assert(item_for(name) == null, name + " isn't exposed and must stay hidden")
		assert(not root.is_node_editable(player.find_child(name, true, false)), name + " must not be editable here")
	assert(item_for("Hand").get_parent() == item_for("Player"), "Hand shows under the instance, without Body")
	assert(item_for("Muzzle").get_parent() == item_for("Gun"), "The gun's exposed Muzzle shows under the exposed Gun")
	var direct = item_for("Player").get_children().map(func(i): return i.get_text(0))
	assert(direct.find("Anim") < direct.find("Hand"), "The instance's own children come before nodes shown from deeper down")
	assert(item_for("Hand").get_button_by_id(0, 10) != -1, "An exposed node is marked")

	# Change exposed nodes, add a node under one, connect and group them; save.
	var hand = player.get_node("Body/Hand")
	set_property(hand, "position", Vector3(1, 2, 3))
	set_property(player.get_node("Body/Gun/Barrel/Muzzle"), "position", Vector3(0, 0, 5))
	var sword = Node3D.new()
	sword.name = "Sword"
	hand.add_child(sword)
	sword.owner = root
	hand.add_to_group("grabbable", true)
	player.get_node("Anim").animation_finished.connect(Callable(root, "_on_anim_finished"), CONNECT_PERSIST)
	# A node of this scene under a hidden part of the instance isn't kept, as before.
	var stray = Node3D.new()
	stray.name = "Stray"
	player.get_node("Secret").add_child(stray)
	stray.owner = root
	await frames(3)
	assert(item_for("Sword") != null and item_for("Sword").get_parent() == item_for("Hand"), "A node added under an exposed node shows under it")
	assert(item_for("Stray") == null, "A node under a hidden part of an instance doesn't show")
	assert(EditorInterface.save_scene() == OK)
	print("SAVED")

func check_kept():
	# player.tscn no longer exposes Hand, but this scene changed it: it stays, with its changes.
	var root = EditorInterface.get_edited_scene_root()
	var hand = root.get_node("Player/Body/Hand")
	print("Scene dock:\n" + "\n".join(outline()))
	assert(not hand.exposed_in_instances)
	assert(hand.is_instance_changes_kept(), "Changes to a node no longer exposed are kept")
	assert(item_for("Hand") != null, "A node no longer exposed, but changed here, stays in the dock")
	assert(item_for("Sword") != null)
	assert(hand.position.is_equal_approx(Vector3(1, 2, 3)))
	# Saving again keeps them.
	set_property(root.get_node("Player/Anim"), "speed_scale", 2.0)
	assert(EditorInterface.save_scene() == OK)
	print("SAVED")

func check_editable_children():
	var root = EditorInterface.get_edited_scene_root()
	var player = root.get_node("Player")
	await select(player)
	await menu_pick("Editable Children")
	assert(root.is_editable_instance(player))
	assert(item_for("Body") != null, "Editable Children shows the whole instance")
	var pebble = Node3D.new()
	pebble.name = "Pebble"
	player.get_node("Body").add_child(pebble)
	pebble.owner = root
	await frames(3)
	# Turning it off keeps what the scene exposes: its changes and the nodes added under it.
	await select(player)
	await menu_pick("Editable Children")
	await confirm_dialog()
	assert(not root.is_editable_instance(player))
	assert(item_for("Body") == null)
	assert(root.get_node("Player/Body/Hand/Sword") != null, "A node under an exposed node stays where it is")
	assert(pebble.get_parent() == player, "A node under a hidden node moves to the instance, as before")
	assert(root.get_node("Player/Body/Hand").position.is_equal_approx(Vector3(1, 2, 3)))
	assert(EditorInterface.save_scene() == OK)
	print("SAVED")

func check_clicks():
	EditorInterface.set_main_screen_editor("3D")
	await frames(10)
	var root = EditorInterface.get_edited_scene_root()
	var player = root.get_node("Player")
	var hand_mesh = player.get_node("Body/Hand/HandMesh")
	EditorInterface.get_selection().clear()
	await frames(2)
	# A first click selects the instance, as always.
	var corner = Vector3(0.2, -0.2, 0.25)
	await click(hand_mesh, corner)
	print("First click: ", selected())
	assert(selected() == "Player", "A first click must select the instance")
	# Clicking it again goes to the node its scene exposes there.
	await click(hand_mesh, corner)
	print("Second click: ", selected())
	assert(selected() == "Hand", "Clicking the selected instance again picks the exposed node")
	# And again, back to the instance.
	await click(hand_mesh, corner)
	print("Third click: ", selected())
	assert(selected() == "Player")
	# Clicking a part of the instance that exposes nothing keeps the instance.
	await click(player.get_node("Body/BodyMesh"), Vector3(-0.3, 0.6, 0.45))
	print("Click on the body: ", selected())
	assert(selected() == "Player")

func check_clicks_2d():
	EditorInterface.open_scene_from_path("res://level2d.tscn")
	await frames(10)
	EditorInterface.set_main_screen_editor("2D")
	await frames(10)
	var player = EditorInterface.get_edited_scene_root().get_node("Player2D")
	# Off the hand's origin, where a selected node's pivot handle would take the click.
	var hand_point = player.get_node("Body/Hand").global_position + Vector2(25, 20)
	EditorInterface.get_selection().clear()
	await frames(2)
	await click_2d(hand_point)
	print("First 2D click: ", selected())
	assert(selected() == "Player2D", "A first click must select the instance")
	await click_2d(hand_point)
	print("Second 2D click: ", selected())
	assert(selected() == "Hand", "Clicking the selected instance again picks the exposed node")
	await click_2d(hand_point)
	print("Third 2D click: ", selected())
	assert(selected() == "Player2D")
	await click_2d(player.global_position + Vector2(-30, 60))
	print("2D click on the body: ", selected())
	assert(selected() == "Player2D")

func check_source():
	EditorInterface.open_scene_from_path("res://player.tscn")
	await frames(10)
	var root = EditorInterface.get_edited_scene_root()
	var anim = root.get_node("Anim")
	var secret = root.get_node("Secret")
	assert(item_for("Anim").get_button_by_id(0, 10) != -1, "An exposed node shows the exposed button in its own scene")
	assert(item_for("Secret").get_button_by_id(0, 10) == -1)
	await select(secret)
	await menu_pick("Expose in Instances")
	assert(secret.exposed_in_instances, "The menu exposes the node")
	assert(item_for("Secret").get_button_by_id(0, 10) != -1)
	EditorInterface.get_editor_undo_redo().get_history_undo_redo(EditorInterface.get_editor_undo_redo().get_object_history_id(secret)).undo()
	await frames(3)
	assert(not secret.exposed_in_instances, "Undo stops exposing it")
	await select(anim)
	await menu_pick("Expose in Instances")
	assert(not anim.exposed_in_instances, "The menu toggles an exposed node off")
"""

RUNTIME = r"""
extends SceneTree

var failures := []

func check(condition: bool, message: String) -> void:
	if not condition:
		failures.append(message)

func _initialize():
	var level = (load("res://level.tscn") as PackedScene).instantiate()
	root.add_child(level)
	var hand = level.get_node("Player/Body/Hand")
	check(hand.position.is_equal_approx(Vector3(1, 2, 3)), "The saved change to an exposed node applies at runtime")
	check(level.get_node_or_null("Player/Body/Hand/Sword") != null, "The node added under an exposed node is there")
	check(level.get_node("Player/Body/Gun/Barrel/Muzzle").position.is_equal_approx(Vector3(0, 0, 5)), "The nested exposed node keeps its change")
	check(hand.is_in_group("grabbable"), "The group added to an exposed node is there")
	check(level.get_node("Player/Anim").animation_finished.is_connected(Callable(level, "_on_anim_finished")), "The signal from an exposed node is connected")
	check(level.get_node_or_null("Player/Secret/Stray") == null, "Nothing is saved under hidden nodes")
	# Packing at runtime keeps them too, as it does for editable children.
	var packed = PackedScene.new()
	check(packed.pack(level) == OK, "The level packs")
	var again = packed.instantiate()
	check(again.get_node("Player/Body/Hand").position.is_equal_approx(Vector3(1, 2, 3)), "Packing at runtime keeps the change")
	check(again.get_node_or_null("Player/Body/Hand/Sword") != null, "Packing at runtime keeps the added node")
	again.free()
	for failure in failures:
		printerr("RUNTIME FAILED: " + failure)
	if failures.is_empty():
		print("RUNTIME_OK")
	quit()
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--temp-dir", type=Path)
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--state-only", action="store_true", help="Skip the graphics run that clicks the viewport")
    args = parser.parse_args()
    parent = (args.temp_dir or Path(tempfile.gettempdir())).resolve()
    work = Path(tempfile.mkdtemp(prefix="asosdot-exposed-", dir=parent))
    print(f"Test workspace: {work}", flush=True)
    try:
        runtime = work / "runtime"
        runtime.mkdir()
        isolated = runtime / args.engine.name
        shutil.copy2(args.engine.resolve(), isolated)
        (runtime / "_sc_").touch()
        project = work / "project"
        project.mkdir()
        addon = project / "addons/check"
        addon.mkdir(parents=True)
        (addon / "plugin.cfg").write_text('[plugin]\nname="Exposed check"\nscript="plugin.gd"\n', encoding="utf-8")
        (addon / "plugin.gd").write_text(CHECK, encoding="utf-8")
        (project / "runtime.gd").write_text(RUNTIME, encoding="utf-8")
        files = {
            "gun.tscn": GUN,
            "player.tscn": PLAYER,
            "level.tscn": LEVEL,
            "level.gd": LEVEL_SCRIPT,
            "player2d.tscn": PLAYER_2D,
            "level2d.tscn": LEVEL_2D,
        }
        for name, text in files.items():
            (project / name).write_text(text, encoding="utf-8")
        (project / "project.godot").write_text(
            'config_version=5\n[application]\nconfig/name="Exposed check"\n'
            "[debug]\nfile_logging/enable_file_logging=false\n"
            '[rendering]\nrenderer/rendering_method="gl_compatibility"\n'
            '[editor_plugins]\nenabled=PackedStringArray("res://addons/check/plugin.cfg")\n',
            encoding="utf-8",
        )
        base = [str(isolated), "--path", str(project)]

        def editor(phase: str, graphics: bool = False) -> None:
            (project / "check.json").write_text(json.dumps({"phase": phase}), encoding="utf-8")
            command = base + ["--editor"]
            if graphics:
                command += ["--rendering-driver", "opengl3", "--audio-driver", "Dummy", "--resolution", "1440x900"]
                command += ["--position", "80,80", "--single-window"]
            else:
                command += ["--headless"]
            run(command, "EXPOSED_OK", work)
            log = (work / "EXPOSED_OK.log").read_text(encoding="utf-8", errors="replace")
            (work / f"{phase}.log").write_text(log, encoding="utf-8")

        editor("scan")
        editor("level")
        level = (project / "level.tscn").read_text(encoding="utf-8")
        print(level, flush=True)
        # The changes to exposed nodes are saved as overrides, without Editable Children.
        assert '[node name="Hand" parent="Player/Body"' in level
        assert re.search(r'\[node name="Hand" parent="Player/Body"[^\]]*groups=\[\s*"grabbable"', level)
        assert "transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 2, 3)" in level
        assert '[node name="Muzzle" parent="Player/Body/Gun/Barrel"' in level
        assert '[node name="Sword" type="Node3D" parent="Player/Body/Hand"' in level
        assert '[connection signal="animation_finished" from="Player/Anim" to="." method="_on_anim_finished"' in level
        assert "[editable" not in level
        assert "Secret" not in level and "Stray" not in level and '[node name="Body"' not in level
        run(base + ["--headless", "--script", "runtime.gd"], "RUNTIME_OK", work)

        # The author stops exposing Hand: the level keeps it and its changes.
        player = project / "player.tscn"
        player.write_text(
            PLAYER.replace(
                "transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 1.5, 0, 0)\nexposed_in_instances = true",
                "transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 1.5, 0, 0)",
            ),
            encoding="utf-8",
        )
        editor("kept")
        level = (project / "level.tscn").read_text(encoding="utf-8")
        assert "transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 2, 3)" in level, (
            "Changes to a node no longer exposed must survive a save"
        )
        assert '[node name="Sword" type="Node3D" parent="Player/Body/Hand"' in level
        assert "speed_scale = 2.0" in level
        run(base + ["--headless", "--script", "runtime.gd"], "RUNTIME_OK", work)

        player.write_text(PLAYER, encoding="utf-8")
        editor("editable")
        level = (project / "level.tscn").read_text(encoding="utf-8")
        assert "[editable" not in level
        assert '[node name="Sword" type="Node3D" parent="Player/Body/Hand"' in level
        assert '[node name="Pebble" type="Node3D" parent="Player"' in level
        assert "transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 2, 3)" in level
        run(base + ["--headless", "--script", "runtime.gd"], "RUNTIME_OK", work)

        editor("source")
        if not args.state_only:
            editor("click", graphics=True)
        for phase in ("level", "kept", "editable", "source", "click"):
            log = work / f"{phase}.log"
            if log.exists():
                lines = [
                    line
                    for line in log.read_text(encoding="utf-8").splitlines()
                    if "click" in line.lower() or line.startswith(("Scene dock", " ", "Player", "Level"))
                ]
                if lines:
                    print(f"--- {phase}\n" + "\n".join(lines), flush=True)
        print("EXPOSED_CHILDREN_OK", flush=True)
    finally:
        if not args.keep:
            # Only remove the specific directory created for this check.
            assert work.resolve().parent == parent and work.name.startswith("asosdot-exposed-")
            shutil.rmtree(work)


if __name__ == "__main__":
    main()
