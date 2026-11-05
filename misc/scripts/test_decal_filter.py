#!/usr/bin/env python3
"""Check that a Decal can be limited to chosen nodes: only those are painted, however
the decal's box and layers are set, and a named object stands for all its visible
parts. Also the one-line helpers (Decal.create_at, fit_to_texture). Renders, so it
needs a GPU: run it with the editor binary, not --headless, once for each renderer:
  --rendering-method forward_plus   (the decal is tested per pixel on the GPU)
  --rendering-method mobile         (the decal is paired with objects on the CPU)"""

from __future__ import annotations

import argparse
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

TEST = r"""
extends Node3D

var fails := 0
var decal: Decal
var left: MeshInstance3D
var right: MeshInstance3D
var group: Node3D
var group_a: MeshInstance3D
var group_b: MeshInstance3D


func check(p_name: String, p_ok: bool, p_detail: Variant = "") -> void:
	print(("PASS " if p_ok else "FAIL ") + p_name + "  " + str(p_detail))
	if not p_ok:
		fails += 1


# How red the left and the right half of the frame are.
func redness() -> Vector2:
	for i in 6:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var image := get_viewport().get_texture().get_image()
	image.resize(160, 90, Image.INTERPOLATE_BILINEAR)
	var result := Vector2.ZERO
	for y in image.get_height():
		for x in image.get_width():
			var c := image.get_pixel(x, y)
			var red := maxf(0.0, c.r - (c.g + c.b) * 0.5)
			if x < 80:
				result.x += red
			else:
				result.y += red
	return result


func box(p_position: Vector3, p_size: float = 2.0) -> MeshInstance3D:
	var mesh := BoxMesh.new()
	mesh.size = Vector3.ONE * p_size
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.9, 0.9, 0.9)
	mesh.material = material
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.position = p_position
	return mi


func red_texture() -> ImageTexture:
	return ImageTexture.create_from_image(solid(Color(1, 0, 0), 8, 4))


func solid(p_color: Color, p_w: int, p_h: int) -> Image:
	var image := Image.create_empty(p_w, p_h, false, Image.FORMAT_RGBA8)
	image.fill(p_color)
	return image


func _ready() -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color.BLACK
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color.WHITE
	env.ambient_light_energy = 1.0
	var world_env := WorldEnvironment.new()
	world_env.environment = env
	add_child(world_env)

	var camera := Camera3D.new()
	camera.position = Vector3(0, 1, 6)
	add_child(camera)
	camera.make_current()

	left = box(Vector3(-1.6, 1, 0))
	right = box(Vector3(1.6, 1, 0))
	add_child(left)
	add_child(right)

	# Two parts of one object, off to the side of the others and out of frame.
	group = Node3D.new()
	group.position = Vector3(0, 0, 0)
	add_child(group)
	group_a = box(Vector3(-1.6, 1, -3), 1.0)
	group_b = box(Vector3(1.6, 1, -3), 1.0)
	group.add_child(group_a)
	group.add_child(group_b)
	group.visible = false

	# Projects along Z, towards the camera, over everything in frame.
	decal = Decal.new()
	decal.texture_albedo = red_texture()
	decal.size = Vector3(10, 8, 10)
	decal.rotation_degrees = Vector3(90, 0, 0)
	decal.position = Vector3(0, 1, 0)
	decal.upper_fade = 0.0
	decal.lower_fade = 0.0
	add_child(decal)
	run.call_deferred()


func run() -> void:
	var plain: Vector2 = await redness()
	check("with no limit the decal paints both boxes", plain.x > 10.0 and plain.y > 10.0, plain)

	decal.add_affected_node(left)
	var only_left: Vector2 = await redness()
	check("limited to the left box it paints that", only_left.x > plain.x * 0.8, [only_left, plain])
	check("and nothing else", only_left.y < plain.y * 0.05, [only_left, plain])
	check("the node is listed", decal.get_affected_nodes() == [decal.get_path_to(left)] and decal.get_affected_visuals() == [left], decal.get_affected_nodes())

	decal.clear_affected_nodes()
	decal.add_affected_node(right)
	var only_right: Vector2 = await redness()
	check("limited to the right one it paints that", only_right.y > plain.y * 0.8 and only_right.x < plain.x * 0.05, [only_right, plain])

	decal.add_affected_node(left)
	var both: Vector2 = await redness()
	check("both can be named", both.x > plain.x * 0.8 and both.y > plain.y * 0.8, [both, plain])

	decal.remove_affected_node(right)
	var back_to_left: Vector2 = await redness()
	check("and one taken away again", back_to_left.x > plain.x * 0.8 and back_to_left.y < plain.y * 0.05, [back_to_left, plain])

	# The layers mean nothing to a decal that is limited.
	decal.cull_mask = 1 << 5
	var by_layers: Vector2 = await redness()
	check("a limited decal does not look at layers", by_layers.x > plain.x * 0.8, [by_layers, plain])
	decal.cull_mask = (1 << 20) - 1

	# A name that leads to nothing affects nothing, not everything.
	decal.clear_affected_nodes()
	decal.affected_nodes = [NodePath("Nowhere")]
	var nowhere: Vector2 = await redness()
	check("a node that is not there paints nothing", nowhere.x < plain.x * 0.05 and nowhere.y < plain.y * 0.05, [nowhere, plain])

	# By path, as a scene keeps it.
	decal.affected_nodes = [decal.get_path_to(right)]
	var by_path: Vector2 = await redness()
	check("a path works as a node does", by_path.y > plain.y * 0.8 and by_path.x < plain.x * 0.05, [by_path, plain])

	# An object is all its parts.
	group.visible = true
	left.visible = false
	right.visible = false
	decal.clear_affected_nodes()
	decal.global_position = Vector3(0, 1, -3)
	var group_plain: Vector2 = await redness()
	decal.add_affected_node(group)
	var group_only: Vector2 = await redness()
	check("naming an object names its parts", group_only.x > group_plain.x * 0.8 and group_only.y > group_plain.y * 0.8 and group_only.x > 3.0, [group_only, group_plain])
	check("they are all listed", decal.get_affected_visuals().size() == 2, decal.get_affected_visuals())

	# A body stands for the mesh it belongs to.
	decal.clear_affected_nodes()
	var body := StaticBody3D.new()
	group_a.add_child(body)
	decal.add_affected_node(body)
	var by_body: Vector2 = await redness()
	check("a body named for its collider stands for its mesh", by_body.x > group_plain.x * 0.8 and by_body.y < group_plain.y * 0.05 and by_body.x > 1.0, [by_body, group_plain])
	check("and is not left over", decal.get_affected_visuals().has(group_a), decal.get_affected_visuals())
	decal.clear_affected_nodes()
	body.queue_free()

	# A freed object does not hand what it was to the next one.
	group.visible = false
	left.visible = true
	right.visible = true
	decal.global_position = Vector3(0, 1, 0)
	decal.add_affected_node(left)
	await redness()
	left.queue_free()
	await get_tree().process_frame
	var newcomer := box(Vector3(-1.6, 1, 0))
	add_child(newcomer)
	# A second, blue decal takes the newcomer, which is given the id the freed box had.
	var blue := Decal.new()
	blue.texture_albedo = ImageTexture.create_from_image(solid(Color(0, 0, 1), 8, 4))
	blue.size = decal.size
	blue.rotation_degrees = decal.rotation_degrees
	blue.position = decal.position
	blue.upper_fade = 0.0
	blue.lower_fade = 0.0
	add_child(blue)
	blue.add_affected_node(newcomer)
	var after_free: Vector2 = await redness()
	check("a newcomer is not painted by what the freed one was", after_free.x < plain.x * 0.05 and after_free.y < plain.y * 0.05, [after_free, plain])
	blue.queue_free()
	decal.clear_affected_nodes()

	# --- Helpers ----------------------------------------------------------------
	var tex := ImageTexture.create_from_image(solid(Color.RED, 20, 10))
	var placed := Decal.create_at(self, tex, Vector3(0, 1, 2), Vector3(0, 0, 1), 1.0, right)
	check("create_at makes a decal", placed != null and placed.get_parent() == self and placed.texture_albedo == tex)
	check("shaped like the texture", is_equal_approx(placed.size.x, 1.0) and is_equal_approx(placed.size.z, 0.5), placed.size)
	check("facing along the normal", placed.global_transform.basis.y.is_equal_approx(Vector3(0, 0, 1)), placed.global_transform.basis.y)
	check("on the spot", placed.global_position.is_equal_approx(Vector3(0, 1, 2)), placed.global_position)
	check("limited to the target", placed.get_affected_visuals() == [right], placed.get_affected_visuals())

	var fading := Decal.create_at(self, tex, Vector3(0, 1, 2), Vector3.UP, 1.0, null, 0.6)
	await get_tree().create_timer(1.0).timeout
	check("a decal with a lifetime goes away", not is_instance_valid(fading))

	placed.texture_albedo = ImageTexture.create_from_image(solid(Color.RED, 40, 10))
	placed.fit_to_texture(2.0)
	check("fit_to_texture takes the shape of the texture", is_equal_approx(placed.size.x, 2.0) and is_equal_approx(placed.size.z, 0.5), placed.size)

	print("DECAL_FILTER fails=%d" % fails)
	get_tree().quit(1 if fails else 0)
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--rendering-driver", default="", help="vulkan or d3d12; the project default when empty")
    parser.add_argument("--rendering-method", default="forward_plus", help="forward_plus or mobile")
    parser.add_argument("--temp-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="asosdot-decal-filter-", dir=args.temp_dir) as directory:
        work = Path(directory)
        runtime = work / "runtime"
        runtime.mkdir()
        isolated = runtime / args.engine.name
        shutil.copy2(args.engine.resolve(), isolated)
        if args.engine.name.endswith(".console.exe"):
            main_exe = args.engine.resolve().with_name(args.engine.name.replace(".console.exe", ".exe"))
            shutil.copy2(main_exe, runtime / main_exe.name)
        (runtime / "_sc_").touch()
        project = work / "project"
        project.mkdir()
        (project / "test.gd").write_text(TEST, encoding="utf-8")
        (project / "main.tscn").write_text(
            '[gd_scene load_steps=2 format=3]\n\n[ext_resource type="Script" path="res://test.gd" id="1"]\n\n'
            '[node name="Main" type="Node3D"]\nscript = ExtResource("1")\n',
            encoding="utf-8",
        )
        (project / "project.godot").write_text(
            f'config_version=5\n[application]\nconfig/name="Decal filter {work.name}"\nrun/main_scene="res://main.tscn"\n'
            "[display]\nwindow/size/viewport_width=640\nwindow/size/viewport_height=360\n",
            encoding="utf-8",
        )
        command = [str(isolated), "--path", str(project), "--rendering-method", args.rendering_method]
        if args.rendering_driver:
            command += ["--rendering-driver", args.rendering_driver]
        run(command, "DECAL_FILTER fails=0", work)
        log = (work / "DECAL_FILTER fails=0.log").read_text(encoding="utf-8", errors="replace")
        print("\n".join(line for line in log.splitlines() if line.startswith(("PASS", "FAIL"))), flush=True)
        print("DECAL_FILTER_TEST_OK", flush=True)


if __name__ == "__main__":
    main()
