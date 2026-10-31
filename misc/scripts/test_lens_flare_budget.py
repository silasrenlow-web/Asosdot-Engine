#!/usr/bin/env python3
"""Check Camera3D.lens_flare_max_count: with more flares than the camera may draw
it keeps the ones nearest to it, ignores flares it can't be looking at, counts a
directional flare as the nearest of all, and when no limit is set still keeps the
nearest of the renderer's 32 instead of the first 32 created. Renders, so it needs
a GPU: run it with the editor binary, not --headless."""

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
var near_left: LensFlare3D
var far_right: LensFlare3D
var behind: LensFlare3D
var sun: LensFlare3D
var fillers: Array[LensFlare3D] = []


func check(p_name: String, p_ok: bool, p_detail: Variant = "") -> void:
	print(("PASS " if p_ok else "FAIL ") + p_name + "  " + str(p_detail))
	if not p_ok:
		fails += 1


# Brightness of the left and the right half of the frame.
func halves() -> Vector2:
	for i in 6:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var image := get_viewport().get_texture().get_image()
	image.resize(160, 90, Image.INTERPOLATE_BILINEAR)
	var left := 0.0
	var right := 0.0
	for y in image.get_height():
		for x in image.get_width():
			var value := image.get_pixel(x, y).get_luminance()
			if x < 80:
				left += value
			else:
				right += value
	return Vector2(left, right)


func make_flare(p_position: Vector3) -> LensFlare3D:
	var lens := LensFlare3D.new()
	lens.flare = LensFlare.create_procedural()
	lens.occlusion_enabled = false
	lens.position = p_position
	lens.visible = false
	add_child(lens)
	return lens


func show_only(p_lit: Array) -> void:
	for lens in [near_left, far_right, behind, sun] + fillers:
		lens.visible = lens in p_lit


func _ready() -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color.BLACK
	env.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	var world_env := WorldEnvironment.new()
	world_env.environment = env
	add_child(world_env)

	camera = Camera3D.new()
	camera.position = Vector3(0, 1.5, 4)
	add_child(camera)
	camera.make_current()

	# Made first, so a limit that keeps the first flares created would keep
	# these and lose the nearer ones below.
	for i in 32:
		fillers.append(make_flare(Vector3(0.5 + 0.02 * i, 1.5, -40)))

	near_left = make_flare(Vector3(-1.0, 1.5, 2.5))
	far_right = make_flare(Vector3(1.0, 1.5, -6.0))
	# Behind the camera and the nearest flare of all.
	behind = make_flare(Vector3(0, 1.5, 5.0))
	# Points at the right half of the frame, a long way off.
	sun = make_flare(Vector3(0, 1.5, 4))
	sun.directional = true
	sun.look_at(sun.position - Vector3(0.3, 0.1, -1.0))
	run.call_deferred()


# Whether two frames came out the same, give or take a few percent.
func same(p_a: Vector2, p_b: Vector2) -> bool:
	return (p_a - p_b).length() < 0.05 * p_b.length() + 1.0


func alone(p_lens: LensFlare3D) -> Vector2:
	show_only([p_lens])
	camera.lens_flare_max_count = 0
	return await halves()


func run() -> void:
	# Each flare drawn alone, to tell afterwards which of them is on screen.
	var near_only: Vector2 = await alone(near_left)
	var far_only: Vector2 = await alone(far_right)
	var sun_only: Vector2 = await alone(sun)
	check("every flare draws on its own", near_only.length() > 5.0 and far_only.length() > 5.0 and sun_only.length() > 5.0, [near_only, far_only, sun_only])

	show_only([near_left, far_right])
	var both: Vector2 = await halves()
	check("both flares draw with no limit", both.length() > near_only.length() * 1.2 and both.length() > far_only.length() * 1.2, [both, near_only, far_only])

	camera.lens_flare_max_count = 1
	var one: Vector2 = await halves()
	check("a limit of 1 draws only the nearer flare", same(one, near_only), [one, near_only])

	camera.lens_flare_max_count = 2
	var two: Vector2 = await halves()
	check("a limit of 2 draws both again", same(two, both), [two, both])

	show_only([near_left, far_right, behind])
	camera.lens_flare_max_count = 1
	var with_behind: Vector2 = await halves()
	check("a flare behind the camera does not take the place", same(with_behind, near_only), [with_behind, near_only])

	camera.lens_flare_max_count = 0
	var unlimited: Vector2 = await halves()
	check("no limit draws every flare in front", same(unlimited, both), [unlimited, both])

	show_only([near_left, sun])
	camera.lens_flare_max_count = 1
	var sun_first: Vector2 = await halves()
	check("a directional flare is kept over a nearby one", same(sun_first, sun_only), [sun_first, sun_only])

	# The setter refuses a negative limit and leaves the last one alone.
	camera.lens_flare_max_count = -1
	check("a negative limit is refused", camera.lens_flare_max_count == 1)

	# More flares than the renderer holds, the farthest created first. The two
	# nearest must be among the ones drawn.
	show_only(fillers)
	var fillers_only: Vector2 = await halves()
	show_only(fillers + [near_left, far_right])
	camera.lens_flare_max_count = 0
	var crowd: Vector2 = await halves()
	check("past 32 flares the nearest are the ones drawn", crowd.length() > fillers_only.length() + near_only.length() * 0.5, [crowd, fillers_only, near_only])

	print("LENS_FLARE_BUDGET fails=%d" % fails)
	get_tree().quit(1 if fails else 0)
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--rendering-driver", default="", help="vulkan or d3d12; the project default when empty")
    parser.add_argument("--temp-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="asosdot-flare-budget-", dir=args.temp_dir) as directory:
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
            f'config_version=5\n[application]\nconfig/name="Lens flare budget {work.name}"\nrun/main_scene="res://main.tscn"\n'
            "[display]\nwindow/size/viewport_width=640\nwindow/size/viewport_height=360\n",
            encoding="utf-8",
        )
        command = [str(isolated), "--path", str(project)]
        if args.rendering_driver:
            command += ["--rendering-driver", args.rendering_driver]
        run(command, "LENS_FLARE_BUDGET fails=0", work)
        log = (work / "LENS_FLARE_BUDGET fails=0.log").read_text(encoding="utf-8", errors="replace")
        print("\n".join(line for line in log.splitlines() if line.startswith(("PASS", "FAIL"))), flush=True)
        print("LENS_FLARE_BUDGET_TEST_OK", flush=True)


if __name__ == "__main__":
    main()
