#!/usr/bin/env python3
"""Check the motion blur (CameraAttributes.motion_blur_*): a camera moving sideways
blurs a striped wall along the way it moves and not across it, an object moving in
front of a still camera blurs the object and not the wall behind, a camera that
turns blurs the sky too, and nothing blurs when it is off or nothing moves.
Renders, so it needs a GPU: run it with the editor binary, not --headless. Forward+
only."""

from __future__ import annotations

import argparse
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

TEST = r"""
extends Node3D

var fails := 0
var camera: Camera3D
var attributes: CameraAttributesPractical
var wall: MeshInstance3D
var box: MeshInstance3D
var world_env: WorldEnvironment


func check(p_name: String, p_ok: bool, p_detail: Variant = "") -> void:
	print(("PASS " if p_ok else "FAIL ") + p_name + "  " + str(p_detail))
	if not p_ok:
		fails += 1


# How sharp the last frame drawn is along each axis: the sum of the square of the
# change in brightness from one pixel to the next, across and down, in the part of
# the frame given. The frame has to be the one the camera moved in, so this does not wait for
# another: a frame after the move shows no velocity, nothing having moved in it.
func sharpness(p_region: Rect2i) -> Vector2:
	var image := get_viewport().get_texture().get_image()
	var across := 0.0
	var down := 0.0
	for y in range(p_region.position.y, p_region.end.y - 1):
		for x in range(p_region.position.x, p_region.end.x - 1):
			var c := image.get_pixel(x, y).get_luminance()
			# Squared, so an edge made wider counts for less: the plain change from one
			# pixel to the next adds up to the same however soft an edge is.
			var dx := image.get_pixel(x + 1, y).get_luminance() - c
			var dy := image.get_pixel(x, y + 1).get_luminance() - c
			across += dx * dx
			down += dy * dy
	return Vector2(across, down)


# How many pixels have some red in them: the box is the only red thing, so this is
# how far it reaches. A box that is smeared reaches further than one that is not,
# and unlike the squared change from pixel to pixel it isn't thrown off by the
# grain a blur is made with.
func red_extent() -> int:
	var image := get_viewport().get_texture().get_image()
	var count := 0
	for y in image.get_height():
		for x in image.get_width():
			var c := image.get_pixel(x, y)
			if c.r - c.g > 0.08:
				count += 1
	return count


func checker() -> ImageTexture:
	var image := Image.create_empty(2, 2, false, Image.FORMAT_RGBA8)
	image.set_pixel(0, 0, Color.WHITE)
	image.set_pixel(1, 1, Color.WHITE)
	image.set_pixel(1, 0, Color.BLACK)
	image.set_pixel(0, 1, Color.BLACK)
	return ImageTexture.create_from_image(image)


# Stripes that only change from row to row, so that blur along the way the camera
# moves has nothing to blur.
func rows() -> ImageTexture:
	var image := Image.create_empty(1, 2, false, Image.FORMAT_RGBA8)
	image.set_pixel(0, 0, Color.WHITE)
	image.set_pixel(0, 1, Color.BLACK)
	return ImageTexture.create_from_image(image)


func set_blur(p_on: bool) -> void:
	attributes.motion_blur_enabled = p_on
	attributes.motion_blur_intensity = 1.0
	attributes.motion_blur_max_length = 10.0
	# Worked out from the distance moved in a frame, whatever time frames take here.
	attributes.motion_blur_target_fps = 0.0


# Moves the camera a step a frame for a few frames, then measures the last of them.
func sweep(p_region: Rect2i, p_step: Vector3, p_turn: float = 0.0, p_frames: int = 8) -> Vector2:
	for i in p_frames:
		camera.position += p_step
		camera.rotation.y += p_turn
		await RenderingServer.frame_post_draw
	return await sharpness(p_region)


func _ready() -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color.BLACK
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color.WHITE
	world_env = WorldEnvironment.new()
	world_env.environment = env
	add_child(world_env)

	attributes = CameraAttributesPractical.new()
	camera = Camera3D.new()
	camera.attributes = attributes
	camera.position = Vector3(0, 0, 0)
	add_child(camera)
	camera.make_current()

	# A wall of checks, wider than the camera ever sees.
	var mesh := QuadMesh.new()
	mesh.size = Vector2(400, 40)
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.albedo_texture = checker()
	material.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	material.uv1_scale = Vector3(400.0 / 0.4, 40.0 / 0.4, 1)
	mesh.material = material
	wall = MeshInstance3D.new()
	wall.mesh = mesh
	wall.position = Vector3(0, 0, -5)
	add_child(wall)

	box = MeshInstance3D.new()
	var box_mesh := BoxMesh.new()
	var box_material := StandardMaterial3D.new()
	box_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	box_material.albedo_color = Color(1, 0.2, 0.1)
	box_mesh.material = box_material
	box_mesh.size = Vector3(0.8, 0.8, 0.8)
	box.mesh = box_mesh
	box.position = Vector3(-3, -1.4, -3)
	box.visible = false
	add_child(box)
	run.call_deferred()


func run() -> void:
	await get_tree().process_frame
	var whole := Rect2i(0, 0, 640, 360)
	var top := Rect2i(0, 0, 640, 120)
	var box_sharp_red := 0
	var box_blur_red := 0
	var box_blur_top := Vector2.ZERO

	check("motion blur is off to begin with", not attributes.motion_blur_enabled)

	# The shaders motion vectors need are made in the background the first time.
	set_blur(true)
	await get_tree().create_timer(2.0).timeout
	set_blur(false)

	# --- The camera moves sideways ---------------------------------------------------
	set_blur(false)
	await RenderingServer.frame_post_draw
	var still_wall: Vector2 = await sharpness(whole)
	var sharp: Vector2 = await sweep(whole, Vector3(0.37, 0, 0))
	check("the wall is sharp to begin with", sharp.x > 500.0 and sharp.y > 500.0, [sharp, still_wall])

	set_blur(true)
	var blurred: Vector2 = await sweep(whole, Vector3(0.37, 0, 0))
	check("moving sideways blurs along the way it moves", blurred.x < sharp.x * 0.6, [blurred, sharp])

	# Along the stripes, nothing changes; across them, blur along the way it moves must not.
	var wall_material: StandardMaterial3D = wall.mesh.material
	var checks_texture := wall_material.albedo_texture
	wall_material.albedo_texture = rows()
	wall_material.uv1_scale = Vector3(1, 40.0 / 0.2, 1)
	set_blur(false)
	var rows_sharp: Vector2 = await sweep(whole, Vector3(0.37, 0, 0))
	set_blur(true)
	var rows_blurred: Vector2 = await sweep(whole, Vector3(0.37, 0, 0))
	check("stripes across the way it moves are not blurred", rows_blurred.y > rows_sharp.y * 0.9 and rows_sharp.y > 500.0, [rows_blurred, rows_sharp])
	wall_material.albedo_texture = checks_texture
	wall_material.uv1_scale = Vector3(400.0 / 0.4, 40.0 / 0.4, 1)

	set_blur(false)
	var off_again: Vector2 = await sweep(whole, Vector3(0.37, 0, 0))
	check("with it off nothing is blurred", off_again.x > sharp.x * 0.9 and off_again.y > sharp.y * 0.9, [off_again, sharp])

	set_blur(true)
	var at_rest: Vector2 = await sweep(whole, Vector3.ZERO)
	check("nothing blurs when nothing moves", at_rest.x > sharp.x * 0.9 and at_rest.y > sharp.y * 0.9, [at_rest, sharp])

	# --- A short streak: less moved, less blurred -----------------------------------------
	set_blur(true)
	var slow: Vector2 = await sweep(whole, Vector3(0.06, 0, 0))
	check("a slow move blurs less than a fast one", slow.x > blurred.x * 1.3, [slow, blurred])

	attributes.motion_blur_intensity = 0.25
	var faint: Vector2 = await sweep(whole, Vector3(0.37, 0, 0))
	check("a shorter shutter blurs less", faint.x > blurred.x * 1.2 and faint.x < sharp.x * 0.95, [faint, blurred, sharp])
	attributes.motion_blur_intensity = 1.0

	attributes.motion_blur_max_length = 1.0
	var capped: Vector2 = await sweep(whole, Vector3(0.37, 0, 0))
	check("the longest streak can be capped", capped.x > blurred.x * 1.2, [capped, blurred])
	attributes.motion_blur_max_length = 10.0

	# --- A thing moves in front of a camera that does not ------------------------------------
	camera.position = Vector3.ZERO
	camera.rotation = Vector3.ZERO
	box.visible = true
	box.position = Vector3(-3, -1.4, -3)
	set_blur(false)
	var box_sharp_top: Vector2 = await sweep(top, Vector3.ZERO)

	for round in 2:
		set_blur(round == 1)
		box.position = Vector3(-3, -1.4, -3)
		for i in 8:
			box.position.x += 0.4
			await RenderingServer.frame_post_draw
		if round == 0:
			box_sharp_top = await sharpness(top)
			box_sharp_red = red_extent()
		else:
			box_blur_top = await sharpness(top)
			box_blur_red = red_extent()
	check("the box is there to begin with", box_sharp_red > 500, box_sharp_red)
	check("the wall behind a moving box stays sharp", box_blur_top.x > box_sharp_top.x * 0.95 and box_blur_top.y > box_sharp_top.y * 0.95, [box_blur_top, box_sharp_top])
	check("and the box is smeared along the way it moves", box_blur_red > box_sharp_red * 1.3, [box_blur_red, box_sharp_red])
	box.visible = false

	# --- The camera turns, over a sky ------------------------------------------------------------
	var sky := Sky.new()
	var sky_material := ProceduralSkyMaterial.new()
	sky_material.sun_angle_max = 8.0
	sky_material.sky_energy_multiplier = 1.0
	sky.sky_material = sky_material
	var sky_env := Environment.new()
	sky_env.background_mode = Environment.BG_SKY
	sky_env.sky = sky
	world_env.environment = sky_env
	wall.visible = false
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-10, 0, 0)
	add_child(sun)
	camera.position = Vector3.ZERO
	camera.rotation = Vector3(0, 0, 0)
	camera.rotation.x = deg_to_rad(8.0)

	set_blur(false)
	var sky_sharp: Vector2 = await sweep(whole, Vector3.ZERO, 0.0, 6)
	var sky_turning_sharp: Vector2 = await sweep(whole, Vector3.ZERO, 0.02, 1)
	camera.rotation.y = 0.0
	set_blur(true)
	await sweep(whole, Vector3.ZERO, 0.0, 6)
	var sky_blurred: Vector2 = await sweep(whole, Vector3.ZERO, 0.02, 1)
	check("the sun has an edge to begin with", sky_sharp.x > 1.0 and sky_turning_sharp.x > 1.0, [sky_sharp, sky_turning_sharp])
	check("a camera that turns blurs the sky", sky_blurred.x < sky_turning_sharp.x * 0.85, [sky_blurred, sky_turning_sharp])

	print("MOTION_BLUR fails=%d" % fails)
	get_tree().quit(1 if fails else 0)
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--rendering-driver", default="", help="vulkan or d3d12; the project default when empty")
    parser.add_argument("--temp-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="asosdot-motion-blur-", dir=args.temp_dir) as directory:
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
            f'config_version=5\n[application]\nconfig/name="Motion blur {work.name}"\nrun/main_scene="res://main.tscn"\n'
            "[display]\nwindow/size/viewport_width=640\nwindow/size/viewport_height=360\n",
            encoding="utf-8",
        )
        command = [str(isolated), "--path", str(project), "--rendering-method", "forward_plus"]
        if args.rendering_driver:
            command += ["--rendering-driver", args.rendering_driver]
        run(command, "MOTION_BLUR fails=0", work)
        log = (work / "MOTION_BLUR fails=0.log").read_text(encoding="utf-8", errors="replace")
        print("\n".join(line for line in log.splitlines() if line.startswith(("PASS", "FAIL"))), flush=True)
        print("MOTION_BLUR_TEST_OK", flush=True)


if __name__ == "__main__":
    main()
