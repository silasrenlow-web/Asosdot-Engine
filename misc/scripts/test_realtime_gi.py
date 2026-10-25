#!/usr/bin/env python3
"""Check RealtimeGI end to end: bake a closed room lit only by a spot light, then
move, recolour, brighten and switch off the light and check the bounce follows.

It renders, so it needs a GPU: run it with the editor binary, not --headless.
"""

from __future__ import annotations

import argparse
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

TEST = r"""
extends Node3D

# Realtime GI check: a closed room lit only by a spot light pointing at the
# floor, with no ambient light. Without GI the ceiling is black; with it, the
# floor's bounce lights the ceiling, the coloured walls bleed onto it, and the
# light can move, change and switch off with the bounce following.

var fails := 0
var camera: Camera3D
var spot: SpotLight3D
var gi: RealtimeGI
var sphere: MeshInstance3D
var shots_dir := ""


func check(p_name: String, p_ok: bool, p_detail := "") -> void:
	if p_ok:
		print("PASS ", p_name, "  ", p_detail)
	else:
		fails += 1
		print("FAIL ", p_name, "  ", p_detail)


func box(p_size: Vector3, p_pos: Vector3, p_color: Color) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	var mesh := BoxMesh.new()
	mesh.size = p_size
	mesh.add_uv2 = true
	var mat := StandardMaterial3D.new()
	mat.albedo_color = p_color
	mesh.material = mat
	mi.mesh = mesh
	mi.position = p_pos
	mi.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	add_child(mi)
	return mi


func frames(p_count: int) -> void:
	for i in p_count:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw


func region(p_img: Image, p_rect: Rect2i) -> Color:
	var sum := Color(0, 0, 0, 0)
	var n := 0
	for y in range(p_rect.position.y, p_rect.end.y):
		for x in range(p_rect.position.x, p_rect.end.x):
			sum += p_img.get_pixel(x, y)
			n += 1
	return sum / float(max(n, 1))


func shot(p_name: String) -> Image:
	await frames(2)
	var img := get_viewport().get_texture().get_image()
	if shots_dir != "":
		img.save_png(shots_dir.path_join(p_name + ".png"))
	return img


func luminance(c: Color) -> float:
	var l := c.srgb_to_linear()
	return l.r * 0.2126 + l.g * 0.7152 + l.b * 0.0722


func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	for a in args:
		if a.begins_with("--shots="):
			shots_dir = a.substr(8)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color.BLACK
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color.BLACK
	env.ambient_light_energy = 0.0
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)

	var white := Color(0.8, 0.8, 0.8)
	box(Vector3(8, 0.2, 8), Vector3(0, -0.1, 0), white) # floor
	box(Vector3(8, 0.2, 8), Vector3(0, 4.1, 0), white) # ceiling
	box(Vector3(0.2, 4, 8), Vector3(-4.1, 2, 0), Color(0.8, 0.1, 0.1)) # left, red
	box(Vector3(0.2, 4, 8), Vector3(4.1, 2, 0), Color(0.1, 0.8, 0.1)) # right, green
	box(Vector3(8, 4, 0.2), Vector3(0, 2, -4.1), white) # back
	box(Vector3(8, 4, 0.2), Vector3(0, 2, 4.1), white) # front, behind the camera

	spot = SpotLight3D.new()
	spot.position = Vector3(0, 3.6, 0)
	spot.rotation_degrees = Vector3(-90, 0, 0)
	spot.spot_angle = 35
	spot.spot_range = 10
	spot.light_energy = 3.0
	spot.shadow_enabled = true
	add_child(spot)

	sphere = MeshInstance3D.new()
	var sm := SphereMesh.new()
	sm.radius = 0.4
	sm.height = 0.8
	var smat := StandardMaterial3D.new()
	smat.albedo_color = Color(0.9, 0.9, 0.9)
	sm.material = smat
	sphere.mesh = sm
	sphere.position = Vector3(2.6, 1.6, -1.5)
	sphere.gi_mode = GeometryInstance3D.GI_MODE_DYNAMIC
	add_child(sphere)

	camera = Camera3D.new()
	camera.fov = 95
	add_child(camera)
	camera.position = Vector3(0, 2, 3.9)
	camera.look_at(Vector3(0, 2, -4))
	camera.current = true

	gi = RealtimeGI.new()
	gi.probe_spacing = 1.0
	add_child(gi)

	var size := get_viewport().get_visible_rect().size
	var ceiling := Rect2i(int(size.x * 0.4), 2, int(size.x * 0.2), int(size.y * 0.06))
	var ceiling_left := Rect2i(int(size.x * 0.12), 2, int(size.x * 0.1), int(size.y * 0.06))
	var ceiling_right := Rect2i(int(size.x * 0.78), 2, int(size.x * 0.1), int(size.y * 0.06))
	var sp := camera.unproject_position(sphere.global_position + Vector3(0, -0.25, 0.3))
	var sphere_rect := Rect2i(int(sp.x) - 3, int(sp.y) - 3, 6, 6)

	await frames(10)
	var img := await shot("0_no_gi")
	var c_none := region(img, ceiling)
	var s_none := region(img, sphere_rect)
	print("no GI: ceiling ", c_none, " sphere ", s_none)
	check("ceiling is dark without GI", luminance(c_none) < 0.01, str(luminance(c_none)))

	var t := Time.get_ticks_msec()
	var err := gi.bake(self, "res://test.rgi")
	var data := gi.data
	check("bake succeeded", err == RealtimeGI.BAKE_ERROR_OK, "error %d in %d ms" % [err, Time.get_ticks_msec() - t])
	if data:
		print("bake: %d patches, %d clusters, %d entries, %d triangles, %d probes, atlas %s" % [data.get_patch_count(), data.get_cluster_count(), data.get_transport_entry_count(), data.get_triangle_count(), data.get_probe_count(), data.get_atlas_size()])
		check("bake made patches", data.get_patch_count() > 100)
		check("bake made probes", data.get_probe_count() > 8)

	await frames(90)
	img = await shot("1_gi")
	var c_gi := region(img, ceiling)
	var cl := region(img, ceiling_left)
	var cr := region(img, ceiling_right)
	var s_gi := region(img, sphere_rect)
	print("GI: ceiling ", c_gi, " left ", cl, " right ", cr, " sphere ", s_gi)
	check("ceiling lit by bounce", luminance(c_gi) > 0.02, str(luminance(c_gi)))
	# Bleeding onto the ceiling is a second bounce, so compare the two sides.
	check("walls bleed their colour onto the ceiling", cl.r / cl.g > (cr.r / cr.g) * 1.08, "left %s right %s" % [cl, cr])
	check("dynamic sphere lit by probes", luminance(s_gi) > luminance(s_none) + 0.01, "%f -> %f" % [luminance(s_none), luminance(s_gi)])

	# Swing the light onto the red wall: the room should turn red.
	spot.position = Vector3(-1.5, 2, 0)
	spot.rotation_degrees = Vector3(0, 90, 0)
	await frames(60)
	img = await shot("2_light_on_red_wall")
	var c_red := region(img, ceiling)
	print("light on red wall: ceiling ", c_red)
	check("moving the light moves the bounce", c_red.r > c_red.g * 1.5, str(c_red))

	spot.visible = false
	await frames(60)
	img = await shot("3_light_off")
	var c_off := region(img, ceiling)
	check("switching the light off removes the bounce", luminance(c_off) < 0.005, str(luminance(c_off)))

	spot.visible = true
	spot.position = Vector3(0, 3.6, 0)
	spot.rotation_degrees = Vector3(-90, 0, 0)
	await frames(60)
	img = await shot("4_back")
	var c_back := region(img, ceiling)
	spot.light_energy = 6.0
	await frames(60)
	img = await shot("5_double")
	var c_double := region(img, ceiling)
	var ratio: float = luminance(c_double) / max(luminance(c_back), 1e-5)
	check("doubling the energy doubles the bounce", absf(ratio - 2.0) < 0.25, "ratio %f" % ratio)

	spot.light_color = Color(0.2, 0.4, 1.0)
	await frames(60)
	img = await shot("6_blue")
	var c_blue := region(img, ceiling)
	check("light colour carries into the bounce", c_blue.b > c_blue.r * 1.5, str(c_blue))

	print("DONE fails=%d" % fails)
	get_tree().quit(1 if fails else 0)
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--rendering-driver", default="", help="vulkan or d3d12; the project default when empty")
    parser.add_argument("--shots", type=Path, help="Folder to save the frames it checks")
    parser.add_argument("--temp-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="asosdot-realtime-gi-", dir=args.temp_dir) as directory:
        work = Path(directory)
        runtime = work / "runtime"
        runtime.mkdir()
        isolated = runtime / args.engine.name
        shutil.copy2(args.engine.resolve(), isolated)
        # The console wrapper starts the main executable beside it.
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
            f'config_version=5\n[application]\nconfig/name="Realtime GI check {work.name}"\nrun/main_scene="res://main.tscn"\n'
            "[display]\nwindow/size/viewport_width=640\nwindow/size/viewport_height=400\n"
            '[rendering]\nrenderer/rendering_method="forward_plus"\n',
            encoding="utf-8",
        )
        command = [str(isolated), "--path", str(project)]
        if args.rendering_driver:
            command += ["--rendering-driver", args.rendering_driver]
        if args.shots:
            args.shots.mkdir(parents=True, exist_ok=True)
            command += ["--", f"--shots={args.shots.resolve()}"]
        run(command, "DONE fails=0", work)
        log = (work / "DONE fails=0.log").read_text(encoding="utf-8", errors="replace")
        print("\n".join(line for line in log.splitlines() if line.startswith(("PASS", "FAIL", "bake:"))), flush=True)
        print("REALTIME_GI_TEST_OK", flush=True)


if __name__ == "__main__":
    main()
