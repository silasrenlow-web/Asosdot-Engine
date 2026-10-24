#!/usr/bin/env python3
"""Check the Animation workspace inside the editor: hosting the 3D editor, camera cuts, recording and poses."""

from __future__ import annotations

import argparse
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

PLUGIN = r"""
@tool
extends EditorPlugin

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
	root.name = "Cutscene"
	var cam_a := Camera3D.new()
	cam_a.name = "CamA"
	var cam_b := Camera3D.new()
	cam_b.name = "CamB"
	var door := Node3D.new()
	door.name = "Door"
	var rig := Skeleton3D.new()
	rig.name = "Rig"
	rig.add_bone("Hip")
	rig.add_bone("Spine")
	rig.set_bone_parent(1, 0)
	var player := AnimationPlayer.new()
	player.name = "Player"
	var anim := Animation.new()
	anim.length = 4.0
	var library := AnimationLibrary.new()
	library.add_animation("cut", anim)
	player.add_animation_library("", library)
	for node in [cam_a, cam_b, door, rig, player]:
		root.add_child(node)
		node.owner = root
	var packed := PackedScene.new()
	packed.pack(root)
	ResourceSaver.save(packed, "res://cutscene.tscn")
	root.free()


# The Animation dock's time field, which moves the timeline and the scene as dragging the playhead does.
func set_playhead(time: float) -> void:
	for spin in EditorInterface.get_base_control().find_children("*", "SpinBox", true, false):
		if spin.tooltip_text == "Animation position (in seconds).":
			spin.value = time
			return
	push_error("No time field found")


func main_screen_button(name: String) -> Button:
	for button in EditorInterface.get_base_control().find_children(name, "Button", true, false):
		if button.toggle_mode:
			return button
	return null


func run() -> void:
	await get_tree().create_timer(1.5).timeout
	build_scene()
	EditorInterface.get_resource_filesystem().scan()
	await get_tree().create_timer(1.0).timeout
	EditorInterface.open_scene_from_path("res://cutscene.tscn")
	await frames(10)
	var scene := EditorInterface.get_edited_scene_root()
	var player: AnimationPlayer = scene.get_node("Player")
	var door: Node3D = scene.get_node("Door")
	var rig: Skeleton3D = scene.get_node("Rig")
	var cam_a: Camera3D = scene.get_node("CamA")
	var cam_b: Camera3D = scene.get_node("CamB")
	var anim: Animation = player.get_animation("cut")
	player.assigned_animation = "cut"
	EditorInterface.edit_node(player)
	await frames()

	var animation_button := main_screen_button("Animation")
	var button_3d := main_screen_button("3D")
	check(animation_button != null, "the main screen bar has an Animation button")
	check(animation_button.get_index() == button_3d.get_index() + 1, "the Animation button sits beside 3D")
	EditorInterface.set_main_screen_editor("Animation")
	await frames(10)
	check(animation_button.button_pressed, "the Animation workspace opens")
	var spatial: Control = EditorInterface.get_base_control().find_children("*", "Node3DEditor", true, false)[0]
	check(spatial.is_visible_in_tree(), "the workspace shows the 3D editor as its viewer")
	var workspace: Control = EditorInterface.get_base_control().find_children("Animation Library", "VBoxContainer", true, false)[0]
	check(workspace.is_visible_in_tree(), "the library is beside the viewer")
	var inspector: Control = EditorInterface.get_inspector()
	check(inspector.is_visible_in_tree(), "the Inspector stays")
	var file_system: Control = EditorInterface.get_file_system_dock()
	check(not file_system.is_visible_in_tree(), "the other side docks make way")

	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(door)
	EditorInterface.edit_node(door)
	await frames()
	check(animation_button.button_pressed, "selecting a 3D node keeps the workspace open")

	# Camera cuts, through the Shots list.
	var trees: Array = workspace.find_children("*", "Tree", true, false)
	var camera_tree: Tree = trees[1]
	var cut_tree: Tree = trees[2]
	player.play("cut")
	player.pause()
	player.seek(0.0, true)
	await frames()
	var camera_items: Array = camera_tree.get_root().get_children()
	check(camera_items.size() == 2, "the Shots list shows the scene's two cameras")
	camera_tree.button_clicked.emit(camera_items[0], 0, 0, MOUSE_BUTTON_LEFT)
	player.seek(2.0, true)
	await frames()
	camera_items = camera_tree.get_root().get_children()
	camera_tree.button_clicked.emit(camera_items[1], 0, 0, MOUSE_BUTTON_LEFT)
	await frames()
	var track_a := anim.find_track(NodePath("CamA:current"), Animation.TYPE_VALUE)
	var track_b := anim.find_track(NodePath("CamB:current"), Animation.TYPE_VALUE)
	check(track_a >= 0 and track_b >= 0, "cutting makes a current track per camera")
	check(anim.track_get_key_count(track_a) == 2 and anim.track_get_key_value(track_a, 0) == true and anim.track_get_key_value(track_a, 1) == false, "CamA is live from 0 and cut away from at 2")
	check(anim.track_get_key_count(track_b) == 2 and anim.track_get_key_value(track_b, 0) == false and anim.track_get_key_value(track_b, 1) == true, "CamB is off from 0 and live from 2")
	check(cut_tree.get_root().get_child_count() == 2, "the cuts list shows both cuts")

	for at in [1.5, 2.5, 1.0, 3.5, 0.2]:
		player.seek(at, true, true)
		await frames(2)
		var live := "CamA" if cam_a.current else ("CamB" if cam_b.current else "none")
		var expected := "CamA" if at < 2.0 else "CamB"
		check(live == expected, "scrubbed to %.1f s, the shot is %s (got %s)" % [at, expected, live])

	EditorInterface.get_editor_undo_redo().get_history_undo_redo(EditorInterface.get_editor_undo_redo().get_object_history_id(anim)).undo()
	await frames()
	check(anim.find_track(NodePath("CamB:current"), Animation.TYPE_VALUE) < 0, "undo takes the second cut back out")
	EditorInterface.get_editor_undo_redo().get_history_undo_redo(EditorInterface.get_editor_undo_redo().get_object_history_id(anim)).redo()
	await frames()

	# Recording.
	var record: Button = null
	for button in spatial.find_children("*", "Button", true, false):
		if button.text == "Record":
			record = button
	check(record != null, "the 3D editor's toolbar has a Record button")
	record.button_pressed = true
	set_playhead(1.0)
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(door)
	EditorInterface.edit_node(door)
	await frames()
	var undo_redo := EditorInterface.get_editor_undo_redo()
	undo_redo.create_action("Move Door")
	undo_redo.add_do_property(door, "position", Vector3(1, 2, 3))
	undo_redo.add_undo_property(door, "position", door.position)
	undo_redo.commit_action()
	await frames(10)
	var door_track := anim.find_track(NodePath("Door"), Animation.TYPE_POSITION_3D)
	check(door_track >= 0 and anim.track_get_key_count(door_track) == 1, "recording keys a moved node without asking")
	if door_track >= 0:
		check(is_equal_approx(anim.track_get_key_time(door_track, 0), 1.0) and anim.track_get_key_value(door_track, 0).is_equal_approx(Vector3(1, 2, 3)), "at the playhead, where it was moved to (key at %.2f s: %s)" % [anim.track_get_key_time(door_track, 0), anim.track_get_key_value(door_track, 0)])
	check(anim.find_track(NodePath("Door"), Animation.TYPE_ROTATION_3D) < 0, "only what changed is keyed")

	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(rig)
	EditorInterface.edit_node(rig)
	await frames()
	var turned := Quaternion(Vector3.UP, 0.5)
	undo_redo.create_action("Turn Spine")
	undo_redo.add_do_method(rig, "set_bone_pose_rotation", 1, turned)
	undo_redo.add_undo_method(rig, "set_bone_pose_rotation", 1, rig.get_bone_pose_rotation(1))
	undo_redo.commit_action()
	await frames(10)
	var spine_track := anim.find_track(NodePath("Rig:Spine"), Animation.TYPE_ROTATION_3D)
	check(spine_track >= 0 and anim.rotation_track_interpolate(spine_track, 1.0).is_equal_approx(turned), "recording keys a posed bone")
	check(anim.find_track(NodePath("Rig:Hip"), Animation.TYPE_ROTATION_3D) < 0, "bones left alone aren't keyed")
	record.button_pressed = false

	# Poses.
	var dialog: EditorFileDialog = workspace.find_children("*", "EditorFileDialog", true, false)[0]
	dialog.file_selected.emit("res://poses.tres")
	await frames()
	check(FileAccess.file_exists("res://poses.tres"), "a new pose library is saved")
	var save_button: Button = null
	var apply_button: Button = null
	for button in workspace.find_children("*", "Button", true, false):
		if button.text == "Save Pose":
			save_button = button
		elif button.text == "Apply":
			apply_button = button
	save_button.pressed.emit()
	await frames()
	var name_dialog: ConfirmationDialog = null
	for dialog_found in workspace.find_children("*", "ConfirmationDialog", true, false):
		if dialog_found.get_class() == "ConfirmationDialog":
			name_dialog = dialog_found
	name_dialog.find_children("*", "LineEdit", true, false)[0].text = "Turned"
	name_dialog.confirmed.emit()
	name_dialog.hide()
	await frames()
	var saved: AnimationLibrary = ResourceLoader.load("res://poses.tres", "", ResourceLoader.CACHE_MODE_IGNORE)
	check(saved.has_animation("Turned"), "the pose is saved in the library")
	rig.set_bone_pose_rotation(1, Quaternion())
	var poses: ItemList = workspace.find_children("*", "ItemList", true, false)[0]
	poses.select(0)
	apply_button.pressed.emit()
	await frames()
	check(rig.get_bone_pose_rotation(1).is_equal_approx(turned), "applying the pose puts the bone back")

	EditorInterface.set_main_screen_editor("3D")
	await frames(10)
	check(not workspace.is_visible_in_tree() and file_system.is_visible_in_tree(), "leaving the workspace puts the docks back")
	check(spatial.is_visible_in_tree(), "the 3D screen still shows its editor")

	print("WORKSPACE_OK" if failures.is_empty() else "WORKSPACE_FAILED")
	get_tree().quit()
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--temp-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="asosdot-animation-workspace-", dir=args.temp_dir) as directory:
        work = Path(directory)
        runtime = work / "runtime"
        runtime.mkdir()
        isolated = runtime / args.engine.name
        shutil.copy2(args.engine.resolve(), isolated)
        (runtime / "_sc_").touch()
        project = work / "project"
        (project / "addons/check").mkdir(parents=True)
        (project / "addons/check/plugin.cfg").write_text('[plugin]\nname="Workspace check"\nscript="plugin.gd"\n', encoding="utf-8")
        (project / "addons/check/plugin.gd").write_text(PLUGIN, encoding="utf-8")
        (project / "project.godot").write_text(
            f'config_version=5\n[application]\nconfig/name="Workspace check {work.name}"\n'
            '[editor_plugins]\nenabled=PackedStringArray("res://addons/check/plugin.cfg")\n',
            encoding="utf-8",
        )
        run([str(isolated), "--path", str(project), "--headless", "--editor"], "WORKSPACE_OK", work)
        log = (work / "WORKSPACE_OK.log").read_text(encoding="utf-8", errors="replace")
        print("\n".join(line for line in log.splitlines() if line.startswith(("PASS", "FAIL"))), flush=True)
        print("ANIMATION_WORKSPACE_TEST_OK", flush=True)


if __name__ == "__main__":
    main()
