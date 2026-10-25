#!/usr/bin/env python3
"""Check the Graph Editor inside the editor: its curves, Blender's edits on them, and undo."""

from __future__ import annotations

import argparse
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

PLUGIN = r"""
@tool
extends EditorPlugin

# AsosdotGraphEditor::MenuOption, in order.
const FRAME_ALL := 0
const SELECT_NONE := 8
const CONVERT_TO_BEZIER := 21
const KEY_INSERT := 22
const KEY_DELETE := 23
const INTERPOLATION_CONSTANT := 34
const INTERPOLATION_LINEAR := 35
const EASING_IN := 38
const HANDLE_VECTOR := 43

var failures := []


func check(ok: bool, what: String) -> void:
	print(("PASS " if ok else "FAIL ") + what)
	if not ok:
		failures.append(what)


func frames(count := 5) -> void:
	for i in count:
		await get_tree().process_frame


func _enter_tree() -> void:
	run.call_deferred()


func build_scene() -> void:
	var root := Node3D.new()
	root.name = "Shot"
	var door := Node3D.new()
	door.name = "Door"
	var lamp := OmniLight3D.new()
	lamp.name = "Lamp"
	var player := AnimationPlayer.new()
	player.name = "Player"
	var anim := Animation.new()
	anim.length = 4.0
	var position := anim.add_track(Animation.TYPE_POSITION_3D)
	anim.track_set_path(position, "Door")
	anim.track_insert_key(position, 0.0, Vector3(0, 0, 0))
	anim.track_insert_key(position, 1.0, Vector3(1, 2, 0))
	anim.track_insert_key(position, 2.0, Vector3(2, 0, 0))
	var rotation := anim.add_track(Animation.TYPE_ROTATION_3D)
	anim.track_set_path(rotation, "Door")
	anim.track_insert_key(rotation, 0.0, Quaternion())
	anim.track_insert_key(rotation, 2.0, Quaternion(Vector3.UP, deg_to_rad(90.0)))
	var energy := anim.add_track(Animation.TYPE_VALUE)
	anim.track_set_path(energy, "Lamp:light_energy")
	anim.track_set_interpolation_type(energy, Animation.INTERPOLATION_LINEAR)
	anim.track_insert_key(energy, 0.0, 1.0)
	anim.track_insert_key(energy, 2.0, 4.0)
	var red := anim.add_track(Animation.TYPE_BEZIER)
	anim.track_set_path(red, "Lamp:light_color:r")
	anim.bezier_track_insert_key(red, 0.0, 1.0)
	anim.bezier_track_insert_key(red, 1.0, 0.2)
	var library := AnimationLibrary.new()
	library.add_animation("shot", anim)
	player.add_animation_library("", library)
	for node in [door, lamp, player]:
		root.add_child(node)
		node.owner = root
	var packed := PackedScene.new()
	packed.pack(root)
	ResourceSaver.save(packed, "res://shot.tscn")
	root.free()


func set_playhead(time: float) -> void:
	for spin in EditorInterface.get_base_control().find_children("*", "SpinBox", true, false):
		if spin.tooltip_text == "Animation position (in seconds).":
			spin.value = time
			return


func undo(anim: Animation) -> void:
	var manager := EditorInterface.get_editor_undo_redo()
	manager.get_history_undo_redo(manager.get_object_history_id(anim)).undo()


func redo(anim: Animation) -> void:
	var manager := EditorInterface.get_editor_undo_redo()
	manager.get_history_undo_redo(manager.get_object_history_id(anim)).redo()


func mouse(graph: Control, button: MouseButton, at: Vector2, pressed: bool) -> void:
	var event := InputEventMouseButton.new()
	event.button_index = button
	event.position = at
	event.pressed = pressed
	if pressed:
		event.button_mask = MOUSE_BUTTON_MASK_LEFT
	graph.send_view_input(event)


func drag(graph: Control, to: Vector2) -> void:
	var event := InputEventMouseMotion.new()
	event.position = to
	event.button_mask = MOUSE_BUTTON_MASK_LEFT
	graph.send_view_input(event)


func select_channel(graph: Control, channel: int) -> void:
	graph.apply_menu_option(SELECT_NONE)
	graph.select_channel_keys(channel)


func run() -> void:
	await get_tree().create_timer(1.5).timeout
	build_scene()
	EditorInterface.get_resource_filesystem().scan()
	await get_tree().create_timer(1.0).timeout
	EditorInterface.open_scene_from_path("res://shot.tscn")
	await frames(10)
	var scene := EditorInterface.get_edited_scene_root()
	var player: AnimationPlayer = scene.get_node("Player")
	var anim: Animation = player.get_animation("shot")
	player.assigned_animation = "shot"
	EditorInterface.edit_node(player)
	EditorInterface.set_main_screen_editor("Animation")
	await frames(10)

	var toggle: Button = null
	for button in EditorInterface.get_base_control().find_children("*", "Button", true, false):
		if button.tooltip_text.begins_with("Switch between the tracks and the Graph Editor") and button.toggle_mode:
			toggle = button
	check(toggle != null and not toggle.disabled, "the timeline has a Graph Editor switch")
	toggle.button_pressed = true
	await frames(5)
	var graph: Control = null
	for found in EditorInterface.get_base_control().find_children("*", "Control", true, false):
		if found.get_class() == "AsosdotGraphEditor":
			graph = found
	check(graph != null and graph.is_visible_in_tree(), "it opens the Graph Editor in place of the tracks")

	var labels: PackedStringArray = graph.get_channel_labels()
	var expected := PackedStringArray(["Door/X Position", "Door/Y Position", "Door/Z Position", "Door/X Rotation", "Door/Y Rotation", "Door/Z Rotation", "Lamp/light_energy", "Lamp/light_color r"])
	check(labels == expected, "every animated number is a curve: positions, rotations, a property and a Bezier track (%s)" % [labels])

	select_channel(graph, 0)
	check(graph.get_selected_key_count() == 3, "selecting a curve's keys")
	graph.move_selection(0.0, 1.5)
	await frames(2)
	var moved_x := [anim.track_get_key_value(0, 0).x, anim.track_get_key_value(0, 1).x, anim.track_get_key_value(0, 2).x]
	check(moved_x == [1.5, 2.5, 3.5] and anim.track_get_key_value(0, 1).y == 2.0, "moving keys up changes only that curve's number (%s)" % [moved_x])
	undo(anim)
	await frames(2)
	check(anim.track_get_key_value(0, 1) == Vector3(1, 2, 0), "undo puts them back")

	select_channel(graph, 0)
	graph.move_selection(0.5, 0.0)
	await frames(2)
	var times := [anim.track_get_key_time(0, 0), anim.track_get_key_time(0, 1), anim.track_get_key_time(0, 2)]
	check(is_equal_approx(times[0], 0.5) and is_equal_approx(times[2], 2.5), "moving keys sideways moves them in time (%s)" % [times])
	undo(anim)
	await frames(2)

	select_channel(graph, 4)
	graph.move_selection(0.0, 30.0)
	await frames(2)
	var turned: Quaternion = anim.track_get_key_value(1, 1)
	var euler := Basis(turned).get_euler()
	check(is_equal_approx(rad_to_deg(euler.y), 120.0) and is_zero_approx(euler.x), "a rotation curve is edited in degrees, and kept as a quaternion (%.1f)" % rad_to_deg(euler.y))
	undo(anim)
	await frames(2)

	select_channel(graph, 0)
	graph.apply_menu_option(INTERPOLATION_LINEAR)
	check(anim.track_get_interpolation_type(0) == Animation.INTERPOLATION_LINEAR, "T Linear sets the track's interpolation")
	graph.apply_menu_option(INTERPOLATION_CONSTANT)
	check(anim.track_get_interpolation_type(0) == Animation.INTERPOLATION_NEAREST, "T Constant holds each key")
	undo(anim)
	undo(anim)

	select_channel(graph, 6)
	graph.apply_menu_option(EASING_IN)
	check(is_equal_approx(anim.track_get_key_transition(2, 0), 2.0), "Ctrl+E Ease In eases the keys")
	undo(anim)

	select_channel(graph, 7)
	graph.apply_menu_option(HANDLE_VECTOR)
	var out_handle := anim.bezier_track_get_key_out_handle(3, 0)
	check(out_handle.is_equal_approx(Vector2(1.0 / 3.0, -0.8 / 3.0)), "V Vector points a Bezier key's handles at its neighbours (%s)" % out_handle)
	undo(anim)

	var track_count := anim.get_track_count()
	select_channel(graph, 3)
	graph.apply_menu_option(CONVERT_TO_BEZIER)
	await frames(3)
	var y_track := anim.find_track(NodePath("Door:rotation:y"), Animation.TYPE_BEZIER)
	check(anim.find_track(NodePath("Door"), Animation.TYPE_ROTATION_3D) < 0 and y_track >= 0 and anim.get_track_count() == track_count + 2, "Convert to Bezier splits a rotation into X, Y and Z curves")
	if y_track >= 0:
		check(is_equal_approx(anim.bezier_track_interpolate(y_track, 2.0), deg_to_rad(90.0)), "keyed with the same turn, in radians as Node3D.rotation is")
	undo(anim)
	await frames(3)
	var rotation_track := anim.find_track(NodePath("Door"), Animation.TYPE_ROTATION_3D)
	check(rotation_track == 1 and anim.track_get_key_count(1) == 2 and anim.get_track_count() == track_count and anim.track_get_path(2) == NodePath("Lamp:light_energy"), "undo puts the rotation track back where it was, with its keys")
	redo(anim)
	await frames(3)
	check(anim.find_track(NodePath("Door:rotation:x"), Animation.TYPE_BEZIER) >= 0, "and redo converts it again")
	undo(anim)
	await frames(3)

	select_channel(graph, 6)
	graph.apply_menu_option(KEY_DELETE)
	check(anim.track_get_key_count(2) == 0, "X deletes the selected keys")
	undo(anim)
	check(anim.track_get_key_count(2) == 2, "and undo brings them back")

	graph.apply_menu_option(SELECT_NONE)
	set_playhead(1.5)
	await frames(3)
	graph.apply_menu_option(KEY_INSERT)
	await frames(2)
	var energy_key := anim.track_find_key(2, 1.5, Animation.FIND_MODE_APPROX)
	check(anim.track_get_key_count(0) == 4 and energy_key >= 0 and is_equal_approx(anim.track_get_key_value(2, energy_key), 3.25), "I keys every curve at the playhead, with the value it has there")
	undo(anim)

	# The mouse: clicking a key, dragging it, and box selecting.
	graph.apply_menu_option(SELECT_NONE)
	graph.apply_menu_option(FRAME_ALL)
	await frames(3)
	var point: Vector2 = graph.get_key_point(1, 1)
	mouse(graph, MOUSE_BUTTON_LEFT, point, true)
	mouse(graph, MOUSE_BUTTON_LEFT, point, false)
	check(graph.get_selected_key_count() == 1, "clicking a key selects it")
	mouse(graph, MOUSE_BUTTON_LEFT, point, true)
	drag(graph, point + Vector2(0, -20))
	drag(graph, point + Vector2(0, -40))
	mouse(graph, MOUSE_BUTTON_LEFT, point + Vector2(0, -40), false)
	await frames(2)
	var dragged: Vector3 = anim.track_get_key_value(0, 1)
	check(dragged.y > 2.0 and dragged.x == 1.0 and is_equal_approx(anim.track_get_key_time(0, 1), 1.0), "dragging a key up raises its curve's value only (%s)" % dragged)
	undo(anim)
	await frames(2)
	check(anim.track_get_key_value(0, 1) == Vector3(1, 2, 0), "and undo takes the drag back")
	# From between the first two keys: left of the key at 0 s is the channel list.
	var first: Vector2 = graph.get_key_point(0, 0)
	var last: Vector2 = graph.get_key_point(0, 2)
	var corner := Vector2((first.x + graph.get_key_point(0, 1).x) * 0.5, 2)
	graph.apply_menu_option(SELECT_NONE)
	mouse(graph, MOUSE_BUTTON_LEFT, corner, true)
	drag(graph, Vector2(last.x + 10, graph.size.y - 2))
	mouse(graph, MOUSE_BUTTON_LEFT, Vector2(last.x + 10, graph.size.y - 2), false)
	check(graph.get_selected_key_count() > 3, "dragging over empty space box selects (%d keys)" % graph.get_selected_key_count())

	var key := InputEventKey.new()
	key.keycode = KEY_TAB
	key.ctrl_pressed = true
	key.pressed = true
	graph.grab_view_focus()
	get_tree().root.push_input(key)
	await frames(3)
	check(not graph.is_visible_in_tree(), "Ctrl+Tab goes back to the tracks, as in Blender")

	print("GRAPH_OK" if failures.is_empty() else "GRAPH_FAILED")
	get_tree().quit()
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--temp-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="asosdot-graph-editor-", dir=args.temp_dir) as directory:
        work = Path(directory)
        runtime = work / "runtime"
        runtime.mkdir()
        isolated = runtime / args.engine.name
        shutil.copy2(args.engine.resolve(), isolated)
        (runtime / "_sc_").touch()
        project = work / "project"
        (project / "addons/check").mkdir(parents=True)
        (project / "addons/check/plugin.cfg").write_text('[plugin]\nname="Graph check"\nscript="plugin.gd"\n', encoding="utf-8")
        (project / "addons/check/plugin.gd").write_text(PLUGIN, encoding="utf-8")
        (project / "project.godot").write_text(
            f'config_version=5\n[application]\nconfig/name="Graph check {work.name}"\n'
            '[editor_plugins]\nenabled=PackedStringArray("res://addons/check/plugin.cfg")\n',
            encoding="utf-8",
        )
        run([str(isolated), "--path", str(project), "--headless", "--editor"], "GRAPH_OK", work)
        log = (work / "GRAPH_OK.log").read_text(encoding="utf-8", errors="replace")
        print("\n".join(line for line in log.splitlines() if line.startswith(("PASS", "FAIL"))), flush=True)
        print("GRAPH_EDITOR_TEST_OK", flush=True)


if __name__ == "__main__":
    main()
