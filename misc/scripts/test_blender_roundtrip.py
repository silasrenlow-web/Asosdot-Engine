#!/usr/bin/env python3
"""Exercise the editor's Blender export and glTF return path with a real Blender installation.

Usage: python misc/scripts/test_blender_roundtrip.py --engine bin/<editor executable> --blender <blender executable>
The editor runs from an isolated copy so this never changes the user's editor settings.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

FIXTURE = r"""
extends SceneTree

func _initialize():
	call_deferred("run")

func run():
	var scene = Node3D.new()
	scene.name = "RoundTrip"
	root.add_child(scene)
	var subject = Node3D.new()
	subject.name = "Character"
	scene.add_child(subject)
	subject.owner = scene
	var skeleton = Skeleton3D.new()
	skeleton.name = "Rig"
	subject.add_child(skeleton)
	skeleton.owner = scene
	skeleton.add_bone("Root")
	skeleton.add_bone("Tip")
	skeleton.set_bone_parent(1, 0)
	skeleton.set_bone_rest(1, Transform3D(Basis.IDENTITY, Vector3(0, 1, 0)))
	var arrays = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([Vector3.ZERO, Vector3.RIGHT, Vector3.UP])
	arrays[Mesh.ARRAY_NORMAL] = PackedVector3Array([Vector3.BACK, Vector3.BACK, Vector3.BACK])
	arrays[Mesh.ARRAY_TEX_UV] = PackedVector2Array([Vector2.ZERO, Vector2.RIGHT, Vector2.UP])
	arrays[Mesh.ARRAY_BONES] = PackedInt32Array([0,0,0,0, 0,0,0,0, 1,0,0,0])
	arrays[Mesh.ARRAY_WEIGHTS] = PackedFloat32Array([1,0,0,0, 1,0,0,0, 1,0,0,0])
	var shape = []
	shape.resize(Mesh.ARRAY_MAX)
	shape[Mesh.ARRAY_VERTEX] = PackedVector3Array([Vector3(0,0,0.25), Vector3.RIGHT, Vector3.UP])
	shape[Mesh.ARRAY_NORMAL] = arrays[Mesh.ARRAY_NORMAL]
	var mesh = ArrayMesh.new()
	mesh.add_blend_shape("Smile")
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, [shape])
	var material = StandardMaterial3D.new()
	material.resource_name = "Painted"
	var image = Image.create(4, 4, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.8, 0.2, 0.1, 1))
	material.albedo_texture = ImageTexture.create_from_image(image)
	var normal = Image.create(4, 4, false, Image.FORMAT_RGBA8)
	normal.fill(Color(0.5, 0.5, 1, 1))
	material.normal_enabled = true
	material.normal_texture = ImageTexture.create_from_image(normal)
	material.metallic = 0.2
	material.roughness = 0.65
	mesh.surface_set_material(0, material)
	var instance = MeshInstance3D.new()
	instance.name = "Body"
	instance.mesh = mesh
	skeleton.add_child(instance)
	instance.owner = scene
	instance.skeleton = NodePath("..")
	instance.skin = Skin.new()
	instance.skin.add_named_bind("Root", Transform3D.IDENTITY)
	instance.skin.add_named_bind("Tip", Transform3D(Basis.IDENTITY, Vector3(0, -1, 0)))
	instance.set_blend_shape_value(0, 0.35)
	var player = AnimationPlayer.new()
	player.name = "AnimationPlayer"
	subject.add_child(player)
	player.owner = scene
	var library = AnimationLibrary.new()
	var clip = Animation.new()
	clip.length = 12.0 # Longer than Blender's default playback range.
	var track = clip.add_track(Animation.TYPE_ROTATION_3D)
	clip.track_set_path(track, NodePath("Rig:Tip"))
	clip.rotation_track_insert_key(track, 0, Quaternion.IDENTITY)
	clip.rotation_track_insert_key(track, 12, Quaternion(Vector3.FORWARD, 0.5))
	library.add_animation("Wave", clip)
	player.add_animation_library("", library)
	var other = MeshInstance3D.new()
	other.name = "OtherObject"
	other.mesh = BoxMesh.new()
	scene.add_child(other)
	other.owner = scene
	var camera = Camera3D.new()
	camera.name = "Camera"
	scene.add_child(camera)
	camera.owner = scene
	var light = OmniLight3D.new()
	light.name = "Light"
	scene.add_child(light)
	light.owner = scene
	var hidden = MeshInstance3D.new()
	hidden.name = "HiddenObject"
	hidden.mesh = BoxMesh.new()
	hidden.visible = false
	scene.add_child(hidden)
	hidden.owner = scene
	var packed = PackedScene.new()
	assert(packed.pack(scene) == OK)
	assert(ResourceSaver.save(packed, "res://fixture.tscn") == OK)
	print("FIXTURE_OK")
	quit()
"""

EDITOR_CHECK = r"""
@tool
extends EditorPlugin

func _enter_tree():
	call_deferred("run")

func find_menu(node: Node, text: String) -> Callable:
	if node is PopupMenu:
		for i in node.item_count:
			if node.get_item_text(i) == text:
				return node.get_item_metadata(i)
	for child in node.get_children():
		var found = find_menu(child, text)
		if found.is_valid():
			return found
	return Callable()

func find_dialog(node: Node, title: String) -> Window:
	if node is Window and node.title == title:
		return node
	for child in node.get_children():
		var found = find_dialog(child, title)
		if found:
			return found
	return null

func set_animation_option(node: Node, enabled: bool):
	if node is CheckBox and node.text == "Include Animations":
		node.button_pressed = enabled
	for child in node.get_children():
		set_animation_option(child, enabled)

func export_file(menu: String, destination: String, animations: bool):
	var action = find_menu(get_tree().root, menu)
	assert(action.is_valid(), "Blender export menu missing")
	action.call()
	var files = find_dialog(get_tree().root, "Export Scene to Blender") as EditorFileDialog
	assert(files != null)
	files.hide()
	files.file_selected.emit(destination)
	var settings = find_dialog(get_tree().root, "Blender Export Settings") as ConfirmationDialog
	assert(settings != null)
	set_animation_option(settings, animations)
	settings.hide()
	settings.confirmed.emit()
	var deadline = Time.get_ticks_msec() + 90000
	while not FileAccess.file_exists(destination) and Time.get_ticks_msec() < deadline:
		await get_tree().create_timer(0.1).timeout
	assert(FileAccess.file_exists(destination), "Blender export did not finish")
	await get_tree().create_timer(1.0).timeout

func run():
	var config = JSON.parse_string(FileAccess.get_file_as_string("res://check.json"))
	EditorInterface.get_editor_settings().set("filesystem/import/blender/blender_path", config.blender)
	EditorInterface.get_editor_settings().set("filesystem/import/blender/rpc_port", 0)
	await get_tree().create_timer(1.0).timeout
	EditorInterface.open_scene_from_path("res://fixture.tscn")
	await get_tree().create_timer(1.0).timeout
	var scene = EditorInterface.get_edited_scene_root()
	assert(scene != null)
	await export_file("Blender Scene...", config.whole, true)
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(scene.get_node("Character"))
	await export_file("Selected Branch to Blender...", config.branch, false)
	assert(is_equal_approx(scene.get_node("Character/Rig/Body").get_blend_shape_value(0), 0.35))
	print("EDITOR_EXPORT_OK")
	get_tree().quit()
"""

BLENDER_CHECK = r"""
import bpy
import json
import sys

options = json.loads(sys.argv[sys.argv.index('--') + 1])
for path, animated in ((options['whole'], True), (options['branch'], False)):
    bpy.ops.wm.open_mainfile(filepath=path)
    assert bpy.context.scene.render.fps == 30
    body = bpy.data.objects['Body']
    assert len(body.data.vertices) == 3
    assert len(body.data.uv_layers) == 1
    assert body.find_armature() is not None
    assert len(body.find_armature().data.bones) == 2
    assert abs(body.data.shape_keys.key_blocks['Smile'].value - 0.35) < 0.001
    painted = body.data.materials[0]
    textures = [node.image for node in painted.node_tree.nodes if node.type == 'TEX_IMAGE']
    assert len(textures) >= 2
    assert all(image.packed_file is not None for image in textures)
    assert all(tuple(image.size) == (4, 4) for image in textures)
    assert any(abs(image.pixels[0] - 0.8) < 0.01 for image in textures)
    assert bool(bpy.data.actions) == animated
    if animated:
        assert 'OtherObject' in bpy.data.objects
        assert 'Camera' in bpy.data.objects and 'Light' in bpy.data.objects
        assert 'HiddenObject' not in bpy.data.objects
        assert bpy.context.scene.frame_end >= 360
        bpy.ops.export_scene.gltf(filepath=options['returned'], export_format='GLB',
            export_animations=True, export_frame_range=False, export_cameras=True, export_lights=True,
            export_import_convert_lighting_mode='COMPAT')
    else:
        assert 'OtherObject' not in bpy.data.objects
print('BLENDER_CHECK_OK')
"""

IMPORT_CHECK = r"""
extends SceneTree

func _initialize():
	call_deferred("run")

func run():
	var config = JSON.parse_string(FileAccess.get_file_as_string("res://check.json"))
	var document = GLTFDocument.new()
	var state = GLTFState.new()
	assert(document.append_from_file(config.returned, state) == OK)
	assert(state.get_images().size() >= 2)
	assert(state.get_skins().size() == 1)
	assert(state.get_animations().size() > 0)
	assert(state.get_cameras().size() == 1)
	var scene = document.generate_scene(state)
	assert(scene != null)
	root.add_child(scene)
	var body = scene.find_child("Body", true, false) as MeshInstance3D
	assert(body != null)
	assert(body.mesh.get_blend_shape_count() == 1)
	assert(is_equal_approx(body.get_blend_shape_value(0), 0.35))
	var material = body.get_active_material(0) as StandardMaterial3D
	assert(material != null and material.albedo_texture != null and material.normal_texture != null)
	assert(material.albedo_texture.get_width() == 4)
	assert(abs(material.roughness - 0.65) < 0.01)
	assert(abs(material.metallic - 0.2) < 0.01)
	var blend_scene = load("res://roundtrip.blend") as PackedScene
	assert(blend_scene != null)
	var blend_root = blend_scene.instantiate()
	root.add_child(blend_root)
	var blend_body = blend_root.find_child("Body", true, false) as MeshInstance3D
	assert(blend_body != null and blend_body.skin != null)
	assert(is_equal_approx(blend_body.get_blend_shape_value(0), 0.35))
	var blend_material = blend_body.get_active_material(0) as StandardMaterial3D
	assert(blend_material != null and blend_material.albedo_texture != null and blend_material.normal_texture != null)
	assert(blend_material.albedo_texture.get_width() == 4)
	print("IMPORT_CHECK_OK")
	quit()
"""


def run(command: list[str], marker: str | None, cwd: Path) -> None:
    log_path = cwd / f"{marker or 'EDITOR_IMPORT'}.log"
    environment = os.environ.copy()
    temporary = cwd / "temp"
    temporary.mkdir(exist_ok=True)
    environment.update(TMP=str(temporary), TEMP=str(temporary))
    with log_path.open("w", encoding="utf-8") as log:
        try:
            result = subprocess.run(
                command, cwd=cwd, env=environment, stdout=log, stderr=subprocess.STDOUT, timeout=120
            )
        except subprocess.TimeoutExpired as error:
            output = log_path.read_text(encoding="utf-8", errors="replace")
            raise RuntimeError(f"Timed out: {command[0]}\n{output}") from error
    output = log_path.read_text(encoding="utf-8", errors="replace")
    if result.returncode != 0 or (marker is not None and marker not in output) or "SCRIPT ERROR" in output:
        raise RuntimeError(f"Command failed: {command[0]}\n{output}")
    print(marker or "EDITOR_IMPORT_OK", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--blender", required=True, type=Path)
    parser.add_argument("--temp-dir", type=Path, help="Parent directory for the isolated test workspace")
    args = parser.parse_args()
    engine = args.engine.resolve()
    blender = args.blender.resolve()
    source = Path(__file__).resolve().parents[2] / "modules/gltf/editor/editor_scene_exporter_blend_plugin.cpp"
    script = re.search(r'BLENDER_EXPORT_SCRIPT = R"PY\((.*?)\)PY";', source.read_text(encoding="utf-8"), re.S)
    assert script is not None
    with tempfile.TemporaryDirectory(prefix="asosdot-blender-check-", dir=args.temp_dir) as directory:
        work = Path(directory)
        runtime = work / "runtime"
        runtime.mkdir()
        isolated = runtime / engine.name
        shutil.copy2(engine, isolated)
        (runtime / "_sc_").touch()
        project = work / "project"
        project.mkdir()
        addon = project / "addons/check"
        addon.mkdir(parents=True)
        (addon / "plugin.cfg").write_text(
            '[plugin]\nname="Blender round-trip check"\nscript="plugin.gd"\n', encoding="utf-8"
        )
        (addon / "plugin.gd").write_text(EDITOR_CHECK, encoding="utf-8")
        (project / "project.godot").write_text(
            'config_version=5\n[application]\nconfig/name="Blender check"\n'
            "[debug]\nfile_logging/enable_file_logging=false\n"
            '[rendering]\nrenderer/rendering_method="gl_compatibility"\n'
            '[editor_plugins]\nenabled=PackedStringArray("res://addons/check/plugin.cfg")\n',
            encoding="utf-8",
        )
        options = {
            "blender": str(blender),
            "whole": str(work / "painted character's 场景.blend"),
            "branch": str(work / "selected branch.blend"),
            "returned": str(work / "returned.glb"),
        }
        (project / "check.json").write_text(json.dumps(options), encoding="utf-8")
        (project / "fixture.gd").write_text(FIXTURE, encoding="utf-8")
        (project / "import_check.gd").write_text(IMPORT_CHECK, encoding="utf-8")
        base = [str(isolated), "--headless", "--path", str(project)]
        run(base + ["--script", "fixture.gd", "--quit-after", "120"], "FIXTURE_OK", work)
        run(base + ["--editor"], "EDITOR_EXPORT_OK", work)
        run(
            [
                str(blender),
                "--background",
                "--factory-startup",
                "--disable-autoexec",
                "--python-exit-code",
                "1",
                "--python-expr",
                BLENDER_CHECK,
                "--",
                json.dumps(options),
            ],
            "BLENDER_CHECK_OK",
            work,
        )
        shutil.copy2(options["whole"], project / "roundtrip.blend")
        project_file = project / "project.godot"
        project_file.write_text(
            project_file.read_text(encoding="utf-8").replace(
                'enabled=PackedStringArray("res://addons/check/plugin.cfg")', "enabled=PackedStringArray()"
            ),
            encoding="utf-8",
        )
        run(base + ["--editor", "--import"], None, work)
        run(base + ["--script", "import_check.gd", "--quit-after", "120"], "IMPORT_CHECK_OK", work)
        # A failed conversion must preserve the existing output file byte for byte.
        previous = Path(options["whole"]).read_bytes()
        failure = {"glb_path": str(work / "missing.glb"), "blend_path": options["whole"], "fps": 30}
        failure_options = work / "failure.json"
        failure_options.write_text(json.dumps(failure), encoding="utf-8")
        result = subprocess.run(
            [
                str(blender),
                "--background",
                "--factory-startup",
                "--disable-autoexec",
                "--python-exit-code",
                "1",
                "--python-expr",
                script.group(1),
                "--",
                str(failure_options),
            ],
            capture_output=True,
            timeout=90,
        )
        assert result.returncode != 0
        assert Path(options["whole"]).read_bytes() == previous
        assert not list(work.glob(".asosdot-*"))
        print("FAILURE_PRESERVES_OUTPUT_OK")


if __name__ == "__main__":
    main()
