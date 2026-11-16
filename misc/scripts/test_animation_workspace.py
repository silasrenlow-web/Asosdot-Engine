#!/usr/bin/env python3
"""Check the Animation workspace inside the editor: hosting the 3D editor, its scene tree and panels,
camera cuts, recording, poses, and several sounds on the lanes of one audio player."""

from __future__ import annotations

import argparse
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

PLUGIN = r"""
@tool
extends EditorPlugin

# AsosdotAnimationWorkspace::PanelsMenu.
const PANELS_SCENE := 0
const PANELS_LIBRARY := 1
const PANELS_MONITOR := 2
const PANELS_DOCKS := 3

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
	var sounds := AudioStreamPlayer.new()
	sounds.name = "Sounds"
	var sounds_3d := AudioStreamPlayer3D.new()
	sounds_3d.name = "Sounds3D"
	var player := AnimationPlayer.new()
	player.name = "Player"
	var anim := Animation.new()
	anim.length = 4.0
	var library := AnimationLibrary.new()
	library.add_animation("cut", anim)
	player.add_animation_library("", library)
	for node in [cam_a, cam_b, door, rig, sounds, sounds_3d, player]:
		root.add_child(node)
		node.owner = root
	# Two sounds, two seconds and half a second long.
	for sound in [["long", 2.0], ["short", 0.5]]:
		var wav := AudioStreamWAV.new()
		wav.format = AudioStreamWAV.FORMAT_16_BITS
		wav.mix_rate = 8000
		var data := PackedByteArray()
		data.resize(int(8000 * sound[1]) * 2)
		wav.data = data
		ResourceSaver.save(wav, "res://%s.tres" % sound[0])
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


func click(control: Control) -> void:
	var press := InputEventMouseButton.new()
	press.button_index = MOUSE_BUTTON_LEFT
	press.pressed = true
	control.gui_input.emit(press)


# Which camera the animation makes live at a time: the latest true key at or before it.
func live_at(anim: Animation, time: float) -> String:
	var best := ""
	var best_time := -1.0
	for track in anim.get_track_count():
		var path := String(anim.track_get_path(track))
		if not path.ends_with(":current"):
			continue
		for key in anim.track_get_key_count(track):
			var key_time := anim.track_get_key_time(track, key)
			if anim.track_get_key_value(track, key) and key_time <= time + 0.0001 and key_time > best_time:
				best_time = key_time
				best = path.get_slice(":", 0)
	return best


func check_monitor(monitor: Control, spatial: Control, player: AnimationPlayer, anim: Animation, cam_a: Camera3D, cam_b: Camera3D) -> void:
	# The monitor is inside a workspace panel (which can float it), and the panel is in the split.
	var panel := monitor.get_parent()
	while panel != null and not (panel.get_parent() is HSplitContainer):
		panel = panel.get_parent()
	var split := panel.get_parent() if panel != null else null
	check(split is HSplitContainer and panel.get_index() > 0 and split.is_ancestor_of(spatial.find_children("*", "Node3DEditorViewportContainer", true, false)[0]), "it sits right of the viewer, side by side")
	check(monitor.get_camera_count() == 2, "the monitor knows the scene's two cameras")
	var source: OptionButton = monitor.find_children("*", "OptionButton", true, false)[0]
	check(source.item_count == 3 and source.get_item_text(0) == "Live Shot", "its source menu offers the live shot and each camera")
	var tiles := monitor.find_children("*", "Control", true, false).filter(func(c): return c.get_class() == "AsosdotCameraView" and c.get_parent() is HBoxContainer)
	check(tiles.size() == 2 and tiles[0].get_camera() == cam_a and tiles[1].get_camera() == cam_b, "the multicam strip has a picture of each camera, in scene order")

	set_playhead(1.0)
	await frames(3)
	check(monitor.get_shown_camera() == cam_a and monitor.get_live_camera() == cam_a, "on the live shot, it shows CamA before the cut")
	set_playhead(3.0)
	await frames(3)
	check(monitor.get_shown_camera() == cam_b, "and CamB after it")
	monitor.show_camera(cam_a)
	await frames(2)
	check(monitor.get_shown_camera() == cam_a and source.selected == 1, "a picked camera shows whatever is live")
	source.select(0)
	source.item_selected.emit(0)
	await frames(2)
	check(monitor.get_shown_camera() == cam_b, "back on the live shot, it follows the cuts again")

	click(tiles[0])
	await frames(3)
	check(live_at(anim, 3.0) == "CamA" and live_at(anim, 2.9) == "CamB", "clicking a camera in the strip cuts to it at the playhead")
	set_playhead(3.5)
	await frames(2)
	var key := InputEventKey.new()
	key.keycode = KEY_2
	key.pressed = true
	get_tree().root.push_input(key)
	await frames(3)
	check(live_at(anim, 3.5) == "CamB" and live_at(anim, 3.4) == "CamA", "pressing a camera's number cuts to it")

	player.stop()
	player.play("cut")
	await get_tree().create_timer(0.3).timeout
	var before := player.current_animation_position
	click(tiles[1])
	player.pause()
	await frames(3)
	var cut_time := -1.0
	var track_b := anim.find_track(NodePath("CamB:current"), Animation.TYPE_VALUE)
	for k in anim.track_get_key_count(track_b):
		var t := anim.track_get_key_time(track_b, k)
		if t > 0.01 and t < 1.9 and anim.track_get_key_value(track_b, k):
			cut_time = t
	var on_frame := cut_time > 0.0 and absf(cut_time / anim.step - roundf(cut_time / anim.step)) < 0.001
	check(on_frame and absf(cut_time - before) <= anim.step * 0.5 + 0.0001, "clicking while playing cuts live, on the frame being shown (%.3f s, playing at %.3f s)" % [cut_time, before])

	# The Panels menu holds the switches for what the workspace shows.
	var panels: PopupMenu = null
	for button in spatial.find_children("*", "MenuButton", true, false):
		if button.text == "Panels":
			panels = button.get_popup()
	check(panels != null, "the toolbar has the Panels menu")
	panels.about_to_popup.emit()
	check(panels.is_item_checked(panels.get_item_index(PANELS_MONITOR)), "its Camera Monitor switch is on")
	panels.id_pressed.emit(PANELS_MONITOR)
	await frames(2)
	check(not monitor.is_visible_in_tree() and not panel.is_visible_in_tree(), "switching it off gives the viewer the width back")
	panels.id_pressed.emit(PANELS_MONITOR)
	await frames(2)
	check(monitor.is_visible_in_tree(), "and on again brings the monitor back")


func audio_lanes(anim: Animation, path: String) -> Array:
	var lanes := []
	for track in anim.get_track_count():
		if anim.track_get_type(track) == Animation.TYPE_AUDIO and String(anim.track_get_path(track)) == path:
			lanes.append(track)
	return lanes


# Several sounds on one player: overlapping ones get a lane each, in one group of the track list.
func check_sounds(scene: Node, library_tabs: TabContainer, player: AnimationPlayer, anim: Animation) -> void:
	var sounds: AudioStreamPlayer = scene.get_node("Sounds")
	var sounds_3d: AudioStreamPlayer3D = scene.get_node("Sounds3D")
	library_tabs.current_tab = 2
	await frames(3)
	var sound_tree: Tree = library_tabs.find_children("*", "Tree", true, false)[3]
	var items := {}
	for item in sound_tree.get_root().get_children():
		items[item.get_text(0)] = item
	check(items.has("long.tres") and items.has("short.tres"), "the Sounds list shows the project's sounds")
	if not (items.has("long.tres") and items.has("short.tres")):
		return

	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(sounds)
	EditorInterface.edit_node(sounds)
	await frames()
	for add in [["long.tres", 0.5], ["long.tres", 1.0], ["short.tres", 1.2], ["short.tres", 3.0]]:
		set_playhead(add[1])
		await frames(2)
		sound_tree.button_clicked.emit(items[add[0]], 0, 0, MOUSE_BUTTON_LEFT)
		await frames(3)
	var lanes := audio_lanes(anim, "Sounds")
	check(lanes.size() == 3, "three sounds that overlap get three lanes on the one player (%d)" % lanes.size())
	if lanes.size() == 3:
		check(anim.track_get_key_count(lanes[0]) == 2 and anim.track_get_key_count(lanes[1]) == 1 and anim.track_get_key_count(lanes[2]) == 1, "a later sound goes back on the first lane, which has room by then")
		check(is_equal_approx(anim.track_get_key_time(lanes[2], 0), 1.2), "each starts at the playhead")
	var history := EditorInterface.get_editor_undo_redo().get_history_undo_redo(EditorInterface.get_editor_undo_redo().get_object_history_id(anim))
	history.undo()
	history.undo()
	await frames(3)
	check(audio_lanes(anim, "Sounds").size() == 2, "undo takes a sound out, and the lane made for it")
	history.redo()
	history.redo()
	await frames(3)
	check(audio_lanes(anim, "Sounds").size() == 3, "redo puts them back")

	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(sounds_3d)
	EditorInterface.edit_node(sounds_3d)
	await frames()
	for add in [["long.tres", 0.6], ["long.tres", 0.9]]:
		set_playhead(add[1])
		await frames(2)
		sound_tree.button_clicked.emit(items[add[0]], 0, 0, MOUSE_BUTTON_LEFT)
		await frames(3)
	check(audio_lanes(anim, "Sounds3D").size() == 2 and audio_lanes(anim, "Sounds").size() == 3, "a 3D player gets lanes of its own the same way")

	var groups := EditorInterface.get_base_control().find_children("*", "AnimationTrackEditGroup", true, false)
	var headers := groups.map(func(group): return group.tooltip_text.get_slice("\n", 0))
	check(headers.count("Sounds") == 1 and headers.count("Sounds3D") == 1, "each player is one group in the track list (%s)" % [headers])
	var lane_rows := EditorInterface.get_base_control().find_children("*", "AnimationTrackEditTypeAudio", true, false)
	check(lane_rows.size() == 5, "with a row per lane under them (%d)" % lane_rows.size())

	set_playhead(0.0)
	await frames(2)
	player.play("cut")
	var most := 0
	var most_3d := 0
	while player.is_playing() and player.current_animation_position < 1.9:
		await get_tree().process_frame
		most = maxi(most, sounds.get_layered_count())
		most_3d = maxi(most_3d, sounds_3d.get_layered_count())
	player.stop()
	check(most == 3, "played in the editor, the one player sounds three at once (%d)" % most)
	check(most_3d == 2, "and the 3D player two (%d)" % most_3d)
	await frames(3)
	check(sounds.get_layered_count() == 0 and not sounds.playing, "stopping silences them, and the player's own stream never played")


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

	# The scene tree, above the library.
	var hierarchy: Control = workspace.find_children("*", "SceneTreeEditor", true, false)[0]
	var hierarchy_tree: Tree = hierarchy.find_children("*", "Tree", true, false)[0]
	await frames(5)
	check(hierarchy.is_visible_in_tree() and hierarchy_tree.get_root() != null and hierarchy_tree.get_root().get_child_count() == 7, "the scene tree lists the scene's nodes")
	check(hierarchy_tree.get_selected() != null and hierarchy_tree.get_selected().get_text(0) == "Door", "it shows the editor's selection")
	EditorInterface.get_selection().clear()
	await frames(3)
	var rig_item: TreeItem = null
	for item in hierarchy_tree.get_root().get_children():
		if item.get_text(0) == "Rig":
			rig_item = item
	# What the tree emits when a row is clicked.
	rig_item.select(0)
	hierarchy_tree.multi_selected.emit(rig_item, 0, true)
	await frames(5)
	check(EditorInterface.get_selection().get_selected_nodes() == [rig], "selecting a node in it selects it in the editor")
	check(EditorInterface.get_inspector().get_edited_object() == rig, "and the Inspector follows")
	check(animation_button.button_pressed, "the workspace stays open")
	var panels_menu: PopupMenu = null
	for button in spatial.find_children("*", "MenuButton", true, false):
		if button.text == "Panels":
			panels_menu = button.get_popup()
	var library_tabs: TabContainer = workspace.find_children("*", "TabContainer", true, false)[0]
	panels_menu.id_pressed.emit(PANELS_SCENE)
	await frames(3)
	check(not hierarchy.is_visible_in_tree() and library_tabs.is_visible_in_tree(), "hiding the scene tree leaves the library the column")
	panels_menu.id_pressed.emit(PANELS_LIBRARY)
	await frames(3)
	check(not workspace.is_visible_in_tree(), "with both hidden the viewer gets the whole width")
	panels_menu.id_pressed.emit(PANELS_SCENE)
	panels_menu.id_pressed.emit(PANELS_LIBRARY)
	await frames(3)
	check(hierarchy.is_visible_in_tree() and library_tabs.is_visible_in_tree(), "and both come back")

	# Camera cuts, through the Shots list.
	var trees: Array = library_tabs.find_children("*", "Tree", true, false)
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

	# The camera monitor, beside the viewer.
	var monitor: Control = null
	for found in spatial.find_children("*", "Control", true, false):
		if found.get_class() == "AsosdotCameraMonitor":
			monitor = found
	check(monitor != null and monitor.is_visible_in_tree(), "the camera monitor shows in the workspace")
	if monitor != null:
		await check_monitor(monitor, spatial, player, anim, cam_a, cam_b)

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

	await check_sounds(scene, library_tabs, player, anim)
	var marked := 0
	var unmarked := 0
	for item in hierarchy_tree.get_root().get_children():
		if item.get_button_count(0) > 0:
			marked += 1
		else:
			unmarked += 1
	# Player has no tracks of its own; the cameras, the door, the rig and both audio players do.
	check(marked == 6 and unmarked == 1, "the scene tree marks the nodes that have tracks (%d marked, %d not)" % [marked, unmarked])

	EditorInterface.set_main_screen_editor("3D")
	await frames(10)
	check(not workspace.is_visible_in_tree() and file_system.is_visible_in_tree(), "leaving the workspace puts the docks back")
	check(spatial.is_visible_in_tree(), "the 3D screen still shows its editor")
	var monitors := spatial.find_children("*", "Control", true, false).filter(func(c): return c.get_class() == "AsosdotCameraMonitor")
	check(monitors.size() == 1 and not monitors[0].is_visible_in_tree(), "and the 3D screen has no monitor")

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
