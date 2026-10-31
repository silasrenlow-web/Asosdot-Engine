#!/usr/bin/env python3
"""Check Camera3D.lens_flare_ghost_limit: the ghosts of several flares pile up in
the middle of the screen, so they are scaled by each flare's share of the light
and together weigh no more than the brightest flare's. A lone flare is untouched,
what sits on the source stays, and a flare hidden behind a wall takes no share.
Renders, so it needs a GPU: run it with the editor binary, not --headless.
Set FLARE_SHOTS to a folder to also save the frames as PNGs."""

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
var flares: Array[LensFlare3D] = []
var wall: MeshInstance3D
var shots := OS.get_environment("FLARE_SHOTS")
var ghosted: LensFlare
var bare: LensFlare


func check(p_name: String, p_ok: bool, p_detail: Variant = "") -> void:
	print(("PASS " if p_ok else "FAIL ") + p_name + "  " + str(p_detail))
	if not p_ok:
		fails += 1


# Brightness of the middle of the frame, of all of it, and of the ring the flare
# draws around the middle, which is where the ghosts of every source pile up.
func measure(p_label: String) -> Vector3:
	for i in 6:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var image := get_viewport().get_texture().get_image()
	if shots != "":
		image.save_png(shots.path_join(p_label + ".png"))
	image.resize(160, 90, Image.INTERPOLATE_BILINEAR)
	var middle := 0.0
	var total := 0.0
	var ring := 0.0
	for y in image.get_height():
		for x in image.get_width():
			var value := image.get_pixel(x, y).get_luminance()
			total += value
			if x >= 56 and x < 104 and y >= 31 and y < 59:
				middle += value
			var radius := Vector2(x - 80, y - 45).length()
			if radius >= 15.0 and radius <= 21.0:
				ring += value
	return Vector3(middle, total, ring)


func use_resource(p_resource: LensFlare) -> void:
	for lens in flares:
		lens.flare = p_resource


func make_flare(p_position: Vector3, p_intensity: float) -> LensFlare3D:
	var lens := LensFlare3D.new()
	lens.flare = ghosted
	lens.intensity = p_intensity
	lens.occlusion_enabled = false
	lens.position = p_position
	lens.visible = false
	add_child(lens)
	flares.append(lens)
	return lens


func show_only(p_lit: Array) -> void:
	for lens in flares:
		lens.visible = lens in p_lit


# Whether two measurements came out the same, give or take a few percent.
func same(p_a: Vector3, p_b: Vector3) -> bool:
	return (p_a - p_b).length() < 0.05 * p_b.length() + 1.0


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

	# The same flare with and without its ghosts, which are what sits off the
	# source along the axis. Subtracting the one from the other leaves only them.
	ghosted = LensFlare.create_procedural()
	bare = LensFlare.create_procedural()
	for i in range(bare.get_element_count() - 1, -1, -1):
		if absf(bare.get_element(i).position) > 0.05:
			bare.remove_element(i)

	# Four lights around the middle of the view.
	make_flare(Vector3(-2.5, 2.3, -2), 1.0)
	make_flare(Vector3(2.5, 0.7, -2), 1.0)
	make_flare(Vector3(-1.0, 0.6, -2), 1.0)
	make_flare(Vector3(1.0, 2.4, -2), 1.0)

	# A wall to hide the second one behind.
	wall = MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = Vector3(2.0, 2.0, 0.2)
	wall.mesh = box
	wall.position = Vector3(2.5, 0.7, -1.0)
	wall.visible = false
	add_child(wall)
	run.call_deferred()


func run() -> void:
	check("the limit starts at 1", camera.lens_flare_ghost_limit == 1.0, camera.lens_flare_ghost_limit)

	# One flare has nothing to share with.
	show_only([flares[0]])
	camera.lens_flare_ghost_limit = 0.0
	var single_off: Vector3 = await measure("single_off")
	camera.lens_flare_ghost_limit = 1.0
	var single_on: Vector3 = await measure("single_on")
	check("a lone flare is untouched", same(single_on, single_off), [single_on, single_off])
	check("a lone flare draws", single_off.y > 5.0, single_off)

	# Four equal ones pile their ghosts up in the middle.
	show_only(flares)
	camera.lens_flare_ghost_limit = 0.0
	var crowd_off: Vector3 = await measure("crowd_off")
	camera.lens_flare_ghost_limit = 0.5
	var crowd_half: Vector3 = await measure("crowd_half")
	camera.lens_flare_ghost_limit = 1.0
	var crowd_on: Vector3 = await measure("crowd_on")
	# What the sources draw on their own, so that only the ghosts are left.
	use_resource(bare)
	var crowd_bare: Vector3 = await measure("crowd_bare")
	show_only([flares[0]])
	var single_bare: Vector3 = await measure("single_bare")
	use_resource(ghosted)
	var ghosts_off := crowd_off.z - crowd_bare.z
	var ghosts_half := crowd_half.z - crowd_bare.z
	var ghosts_on := crowd_on.z - crowd_bare.z
	var ghosts_single := single_off.z - single_bare.z
	check("four flares do pile up ghosts", ghosts_off > ghosts_single * 1.4, [ghosts_off, ghosts_single])
	check("the ghosts are calmer with the limit on", ghosts_on < ghosts_off * 0.4, [ghosts_on, ghosts_off])
	check("half the limit is half way", ghosts_on < ghosts_half and ghosts_half < ghosts_off, [ghosts_on, ghosts_half, ghosts_off])
	check("together the ghosts weigh about one flare's worth", ghosts_on < ghosts_single * 1.5 and ghosts_on > ghosts_single * 0.5, [ghosts_on, ghosts_single])
	check("the flares themselves stay", crowd_on.y > crowd_off.y * 0.4, [crowd_on, crowd_off])

	# One bright light and three dim ones: the bright one keeps its ghosts.
	flares[0].intensity = 6.0
	for i in range(1, 4):
		flares[i].intensity = 0.3
	show_only(flares)
	camera.lens_flare_ghost_limit = 1.0
	var bright_on: Vector3 = await measure("bright_on")
	camera.lens_flare_ghost_limit = 0.0
	var bright_off: Vector3 = await measure("bright_off")
	use_resource(bare)
	var bright_bare: Vector3 = await measure("bright_bare")
	show_only([flares[0]])
	var bright_alone_bare: Vector3 = await measure("bright_alone_bare")
	use_resource(ghosted)
	var bright_alone: Vector3 = await measure("bright_alone")
	camera.lens_flare_ghost_limit = 1.0
	var bright_ghosts := bright_on.z - bright_bare.z
	var bright_alone_ghosts := bright_alone.z - bright_alone_bare.z
	check("a bright flare keeps its ghosts beside dim ones", bright_ghosts < bright_alone_ghosts * 1.1 and bright_ghosts > bright_alone_ghosts * 0.7, [bright_ghosts, bright_alone_ghosts])
	check("the dim ones only take a small share", bright_ghosts < bright_off.z - bright_bare.z, [bright_ghosts, bright_off.z - bright_bare.z])
	flares[0].intensity = 1.0
	for i in range(1, 4):
		flares[i].intensity = 1.0

	# A flare behind a wall takes no share of the ghosts.
	wall.visible = true
	flares[1].occlusion_enabled = true
	show_only([flares[0], flares[1]])
	camera.lens_flare_ghost_limit = 1.0
	var hidden_on: Vector3 = await measure("hidden_on")
	show_only([flares[0]])
	var visible_alone: Vector3 = await measure("visible_alone")
	check("a flare behind a wall takes no ghosts from the one in view", same(hidden_on, visible_alone), [hidden_on, visible_alone])

	print("LENS_FLARE_GHOSTS fails=%d" % fails)
	get_tree().quit(1 if fails else 0)
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--rendering-driver", default="", help="vulkan or d3d12; the project default when empty")
    parser.add_argument("--temp-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="asosdot-flare-ghosts-", dir=args.temp_dir) as directory:
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
            f'config_version=5\n[application]\nconfig/name="Lens flare ghosts {work.name}"\nrun/main_scene="res://main.tscn"\n'
            "[display]\nwindow/size/viewport_width=960\nwindow/size/viewport_height=540\n",
            encoding="utf-8",
        )
        command = [str(isolated), "--path", str(project)]
        if args.rendering_driver:
            command += ["--rendering-driver", args.rendering_driver]
        run(command, "LENS_FLARE_GHOSTS fails=0", work)
        log = (work / "LENS_FLARE_GHOSTS fails=0.log").read_text(encoding="utf-8", errors="replace")
        print("\n".join(line for line in log.splitlines() if line.startswith(("PASS", "FAIL"))), flush=True)
        print("LENS_FLARE_GHOSTS_TEST_OK", flush=True)


if __name__ == "__main__":
    main()
