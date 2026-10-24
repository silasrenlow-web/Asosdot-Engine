#!/usr/bin/env python3
"""Check the animation editor's body-part track groups and the clip browser."""

from __future__ import annotations

import argparse
import json
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

# A humanoid skeleton with a rotation track on every bone, and clips named the way
# a Blender import names them, in two libraries.
FIXTURE = r"""
extends SceneTree

var skeleton: Skeleton3D

func _initialize():
	call_deferred("run")

func bone(name: String, parent: String, offset: Vector3):
	var index = skeleton.get_bone_count()
	skeleton.add_bone(name)
	if not parent.is_empty():
		skeleton.set_bone_parent(index, skeleton.find_bone(parent))
	skeleton.set_bone_rest(index, Transform3D(Basis(), offset))

func run():
	var scene = Node3D.new()
	scene.name = "Organize"
	skeleton = Skeleton3D.new()
	skeleton.name = "Skeleton3D"
	scene.add_child(skeleton)
	skeleton.owner = scene
	var up = Vector3(0, 0.1, 0)
	bone("hips", "", Vector3(0, 1, 0))
	bone("spine", "hips", up)
	bone("spine1", "spine", up)
	bone("chest", "spine1", up)
	bone("neck", "chest", up)
	bone("head", "neck", up)
	bone("eye.L", "head", Vector3(0.03, 0.05, 0.08))
	bone("eye.R", "head", Vector3(-0.03, 0.05, 0.08))
	for side in ["L", "R"]:
		var x = 0.1 if side == "L" else -0.1
		bone("shoulder." + side, "chest", Vector3(x, 0.05, 0))
		bone("upper_arm." + side, "shoulder." + side, Vector3(x, 0, 0))
		bone("forearm." + side, "upper_arm." + side, Vector3(x * 3, 0, 0))
		bone("hand." + side, "forearm." + side, Vector3(x * 3, 0, 0))
		for finger in ["thumb", "index", "middle", "ring", "pinky"]:
			var parent = "hand." + side
			for i in 3:
				var name = "%s.%02d.%s" % [finger, i + 1, side]
				bone(name, parent, Vector3(x * 0.3, 0, 0))
				parent = name
		bone("thigh." + side, "hips", Vector3(x, -0.05, 0))
		bone("shin." + side, "thigh." + side, Vector3(0, -0.45, 0))
		bone("foot." + side, "shin." + side, Vector3(0, -0.45, 0))
		bone("toe." + side, "foot." + side, Vector3(0, -0.05, 0.1))
	var player = AnimationPlayer.new()
	player.name = "AnimationPlayer"
	scene.add_child(player)
	player.owner = scene
	var global_library = AnimationLibrary.new()
	for clip in ["Armature|walk_forward", "Armature|idle", "Armature|rifle_reload", "Armature|walk_back", "Armature|rifle_fire"]:
		var animation = Animation.new()
		animation.length = 1.0
		for b in skeleton.get_bone_count():
			var track = animation.add_track(Animation.TYPE_ROTATION_3D)
			animation.track_set_path(track, NodePath("Skeleton3D:" + skeleton.get_bone_name(b)))
			animation.rotation_track_insert_key(track, 0.0, Quaternion())
		global_library.add_animation(clip, animation)
	player.add_animation_library("", global_library)
	var pistol = AnimationLibrary.new()
	for clip in ["reload", "fire"]:
		pistol.add_animation(clip, Animation.new())
	player.add_animation_library("pistol", pistol)
	var packed = PackedScene.new()
	assert(packed.pack(scene) == OK)
	assert(ResourceSaver.save(packed, "res://organize.tscn") == OK)
	print("ORGANIZE_FIXTURE_OK")
	quit()
"""

CHECK = r"""
@tool
extends EditorPlugin

func _enter_tree():
	call_deferred("run")

func find_all(node: Node, type: String, found: Array) -> Array:
	if node.get_class() == type:
		found.append(node)
	for child in node.get_children():
		find_all(child, type, found)
	return found

func find_where(node: Node, test: Callable) -> Node:
	if test.call(node):
		return node
	for child in node.get_children():
		var found = find_where(child, test)
		if found:
			return found
	return null

func clips(tree: Tree) -> Array:
	var found = []
	var item = tree.get_root()
	while item:
		if item.get_metadata(0) is String:
			found.append(item.get_metadata(0))
		item = item.get_next_in_tree()
	return found

func item_for(tree: Tree, clip: String) -> TreeItem:
	var item = tree.get_root()
	while item:
		if item.get_metadata(0) is String and item.get_metadata(0) == clip:
			return item
		item = item.get_next_in_tree()
	return null

func frames(count: int):
	for i in count:
		await get_tree().process_frame

func run():
	var config = JSON.parse_string(FileAccess.get_file_as_string("res://check.json"))
	while not EditorInterface.get_edited_scene_root():
		await get_tree().process_frame
	await frames(12)
	var scene = EditorInterface.get_edited_scene_root()
	var player = scene.get_node("AnimationPlayer") as AnimationPlayer
	EditorInterface.edit_node(player)
	await frames(12)
	var editor = find_all(get_tree().root, "AnimationPlayerEditor", [])[0]
	var dropdown = find_where(editor, func(n): return n is OptionButton and n.tooltip_text == "Display list of animations in player.") as OptionButton
	for i in dropdown.item_count:
		if dropdown.get_item_text(i) == "Armature|idle":
			dropdown.select(i)
			dropdown.item_selected.emit(i)
	await frames(6)

	# Bone tracks: one group per body part, not one for the whole skeleton.
	var groups = find_all(editor, "AnimationTrackEditGroup", [])
	print("Track groups: ", groups.size())
	assert(groups.size() == 9, "Expected hips, spine, neck and head, two arms, two hands' fingers and two legs")

	# Fold every body part by clicking its arrow, as a user does. Each fold brings the next header up.
	var viewport = EditorInterface.get_base_control().get_viewport()
	for group in groups:
		var scroll = group.get_parent()
		while scroll and not scroll is ScrollContainer:
			scroll = scroll.get_parent()
		scroll.ensure_control_visible(group)
		await frames(2)
		for pressed in [true, false]:
			var click = InputEventMouseButton.new()
			click.button_index = MOUSE_BUTTON_LEFT
			click.pressed = pressed
			click.position = group.get_global_rect().position + Vector2(6, group.size.y / 2)
			click.global_position = click.position
			viewport.push_input(click)
		await frames(2)
	var shown_rows = find_all(editor, "AnimationTrackEdit", []).filter(func(row): return row.is_visible_in_tree())
	print("Bone track rows shown with every part folded: ", shown_rows.size())
	assert(shown_rows.is_empty(), "Every bone track belongs to a part, and each part folds")

	# The clip browser.
	var browser = find_all(editor, "AsosdotAnimationClipBrowser", [])[0]
	var open = find_where(editor, func(n): return n is Button and n.tooltip_text.begins_with("Browse Clips")) as Button
	open.pressed.emit()
	await frames(4)
	assert(browser.visible, "The clip browser did not open")
	var tree = find_all(browser, "Tree", [])[0] as Tree
	var libraries = []
	for library in tree.get_root().get_children():
		var children = []
		for child in library.get_children():
			children.append(child.get_text(0) + ("/" if child.get_child_count() > 0 else ""))
		libraries.append([library.get_text(0), children])
	print("Browser: ", libraries)
	assert(libraries[0][0] == "[Global]" and libraries[1][0] == "pistol")
	# Blender's shared "Armature|" groups nothing; the words after it do.
	assert(libraries[0][1] == ["Armature|idle", "rifle/", "walk/"], "Clips sharing a first word must be grouped")
	assert(libraries[1][1] == ["fire", "reload"])

	var search = find_all(browser, "LineEdit", [])[0] as LineEdit
	search.text = "reload"
	search.text_changed.emit("reload")
	print("Search for reload: ", clips(tree))
	assert(clips(tree) == ["Armature|rifle_reload", "pistol/reload"])
	search.text = ""
	search.text_changed.emit("")

	# Loop several clips at once, then undo it in one step.
	tree.deselect_all()
	item_for(tree, "Armature|rifle_fire").select(0)
	item_for(tree, "Armature|rifle_reload").select(0)
	var loop_button = find_where(browser, func(n): return n is Button and n.text == "Loop") as Button
	loop_button.pressed.emit()
	var fire = player.get_animation("Armature|rifle_fire")
	var reload = player.get_animation("Armature|rifle_reload")
	assert(fire.loop_mode == Animation.LOOP_LINEAR and reload.loop_mode == Animation.LOOP_LINEAR, "The selected clips must loop")
	assert(player.get_animation("Armature|idle").loop_mode == Animation.LOOP_NONE, "Unselected clips must not change")
	var undo = get_undo_redo()
	assert(undo.get_history_undo_redo(undo.get_object_history_id(fire)).undo())
	assert(fire.loop_mode == Animation.LOOP_NONE and reload.loop_mode == Animation.LOOP_NONE, "One undo must restore every clip")

	if config.graphics:
		item_for(tree, "Armature|walk_back").select(0)
		await frames(6)
		RenderingServer.force_draw(false)
		assert(EditorInterface.get_base_control().get_viewport().get_texture().get_image().save_png(config.screenshot) == OK)

	# Opening a clip from the browser shows it in the editor.
	tree.deselect_all()
	item_for(tree, "pistol/fire").select(0)
	tree.item_activated.emit()
	await frames(4)
	assert(not browser.visible)
	assert(dropdown.get_item_text(dropdown.selected) == "pistol/fire", "Opening a clip must select it in the editor")
	print("ORGANIZE_OK")
	get_tree().quit()
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--temp-dir", type=Path)
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--state-only", action="store_true", help="Skip the graphics run and its screenshot")
    args = parser.parse_args()
    parent = (args.temp_dir or Path(tempfile.gettempdir())).resolve()
    work = Path(tempfile.mkdtemp(prefix="asosdot-organize-", dir=parent))
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
        (addon / "plugin.cfg").write_text('[plugin]\nname="Organize check"\nscript="plugin.gd"\n', encoding="utf-8")
        (addon / "plugin.gd").write_text(CHECK, encoding="utf-8")
        (project / "fixture.gd").write_text(FIXTURE, encoding="utf-8")
        (project / "project.godot").write_text(
            'config_version=5\n[application]\nconfig/name="Organize check"\n'
            "[debug]\nfile_logging/enable_file_logging=false\n"
            '[rendering]\nrenderer/rendering_method="gl_compatibility"\n'
            '[editor_plugins]\nenabled=PackedStringArray("res://addons/check/plugin.cfg")\n',
            encoding="utf-8",
        )
        options = {"graphics": False, "screenshot": str(work / "organize.png")}
        (project / "check.json").write_text(json.dumps(options), encoding="utf-8")
        base = [str(isolated), "--path", str(project)]
        run(base + ["--headless", "--script", "fixture.gd", "--quit-after", "120"], "ORGANIZE_FIXTURE_OK", work)
        run(base + ["--headless", "--editor", "res://organize.tscn"], "ORGANIZE_OK", work)
        if not args.state_only:
            options["graphics"] = True
            (project / "check.json").write_text(json.dumps(options), encoding="utf-8")
            graphics = base + ["--editor", "res://organize.tscn", "--rendering-driver", "opengl3", "--audio-driver", "Dummy"]
            run(graphics + ["--resolution", "1440x900", "--position", "80,80", "--single-window"], "ORGANIZE_OK", work)
        print("ANIMATION_ORGANIZATION_OK", flush=True)
    finally:
        if not args.keep:
            # Only remove the specific directory created for this check.
            assert work.resolve().parent == parent and work.name.startswith("asosdot-organize-")
            shutil.rmtree(work)


if __name__ == "__main__":
    main()
