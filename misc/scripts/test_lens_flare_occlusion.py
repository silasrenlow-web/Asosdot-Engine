#!/usr/bin/env python3
"""Check LensFlare3D occlusion exceptions: a flare inside its own bulb is hidden
by the glass until the bulb is excluded, while walls, even one right in front of
the bulb, still hide it. Renders, so it needs a GPU: run it with the editor
binary, not --headless."""

from __future__ import annotations

import argparse
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

TEST = r"""
extends Node3D

var fails := 0
var lens: LensFlare3D
var bulb: MeshInstance3D
var lamp: Node3D
var near_wall: MeshInstance3D
var far_wall: MeshInstance3D
var other: MeshInstance3D


func check(p_name: String, p_ok: bool, p_detail: Variant = "") -> void:
	print(("PASS " if p_ok else "FAIL ") + p_name + "  " + str(p_detail))
	if not p_ok:
		fails += 1


func frame_luminance() -> float:
	for i in 6:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var image := get_viewport().get_texture().get_image()
	image.resize(160, 90, Image.INTERPOLATE_BILINEAR)
	var total := 0.0
	for y in image.get_height():
		for x in image.get_width():
			total += image.get_pixel(x, y).get_luminance()
	return total


# How much the flare adds to the frame: with it, minus without it.
func flare_amount() -> float:
	lens.visible = true
	var with_flare: float = await frame_luminance()
	lens.visible = false
	var without: float = await frame_luminance()
	lens.visible = true
	return with_flare - without


func box_mesh(p_size: Vector3, p_pos: Vector3) -> MeshInstance3D:
	var mesh := BoxMesh.new()
	mesh.size = p_size
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.position = p_pos
	mi.visible = false
	add_child(mi)
	return mi


func _ready() -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color.BLACK
	env.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	var world_env := WorldEnvironment.new()
	world_env.environment = env
	add_child(world_env)

	var camera := Camera3D.new()
	camera.position = Vector3(0, 1.5, 4)
	add_child(camera)
	camera.make_current()

	# A lamp: an opaque globe with the flare at its centre, as a child of the
	# globe, the way a bulb scene is built.
	lamp = Node3D.new()
	lamp.position = Vector3(1.0, 1.9, 0)
	add_child(lamp)
	bulb = MeshInstance3D.new()
	var sphere := SphereMesh.new()
	sphere.radius = 0.3
	sphere.height = 0.6
	var glass := StandardMaterial3D.new()
	glass.albedo_color = Color(1, 0.9, 0.7)
	glass.emission_enabled = true
	glass.emission = Color(1, 0.9, 0.7)
	sphere.material = glass
	bulb.mesh = sphere
	lamp.add_child(bulb)
	lens = LensFlare3D.new()
	lens.flare = LensFlare.create_procedural()
	bulb.add_child(lens)

	# Right in front of the globe, 0.2 m off its surface and wider than it.
	near_wall = box_mesh(Vector3(1.5, 1.5, 0.1), lamp.position + Vector3(0, 0, 0.55))
	far_wall = box_mesh(Vector3(1.5, 1.5, 0.1), lamp.position + Vector3(0, 0, 2.0))
	# Something unrelated, off to the side.
	other = box_mesh(Vector3(0.3, 0.3, 0.3), Vector3(-2, 1, 0))
	other.visible = true
	run.call_deferred()


func run() -> void:
	lens.occlusion_enabled = false
	var unoccluded: float = await flare_amount()
	check("the flare draws when nothing can hide it", unoccluded > 5.0, unoccluded)

	lens.occlusion_enabled = true
	var hidden: float = await flare_amount()
	check("the bulb's own glass hides the flare", hidden < unoccluded * 0.1, "%f of %f" % [hidden, unoccluded])

	lens.add_occlusion_exception(other)
	var wrong_object: float = await flare_amount()
	check("excluding an unrelated object changes nothing", wrong_object < unoccluded * 0.1, "%f of %f" % [wrong_object, unoccluded])
	lens.remove_occlusion_exception(other)

	lens.add_occlusion_exception(bulb)
	var excluded: float = await flare_amount()
	check("excluding the bulb lets the flare shine", excluded > unoccluded * 0.9, "%f of %f" % [excluded, unoccluded])
	check("the exception is listed", lens.get_occlusion_exception_nodes() == [bulb] and lens.occlusion_exceptions.size() == 1, lens.occlusion_exceptions)

	near_wall.visible = true
	var close: float = await flare_amount()
	check("a wall just in front of the bulb still hides it", close < unoccluded * 0.1, "%f of %f" % [close, unoccluded])
	near_wall.visible = false

	far_wall.visible = true
	var far: float = await flare_amount()
	check("a wall further out hides it", far < unoccluded * 0.1, "%f of %f" % [far, unoccluded])
	far_wall.visible = false

	lamp.position += Vector3(0, 0, -0.5)
	var moved: float = await flare_amount()
	check("the exception follows the bulb when it moves", moved > unoccluded * 0.9, "%f of %f" % [moved, unoccluded])
	lamp.position -= Vector3(0, 0, -0.5)

	lens.remove_occlusion_exception(bulb)
	var removed: float = await flare_amount()
	check("removing the exception hides it again", removed < unoccluded * 0.1, "%f of %f" % [removed, unoccluded])

	lens.add_occlusion_exception(bulb)
	lens.clear_occlusion_exceptions()
	var cleared: float = await flare_amount()
	check("clearing the exceptions hides it again", cleared < unoccluded * 0.1 and lens.get_occlusion_exception_nodes().is_empty(), "%f of %f" % [cleared, unoccluded])

	# Saved as a path, so a scene keeps it.
	lens.occlusion_exceptions = [lens.get_path_to(bulb)]
	var by_path: float = await flare_amount()
	check("an exception set by path works", by_path > unoccluded * 0.9, "%f of %f" % [by_path, unoccluded])

	print("LENS_FLARE fails=%d" % fails)
	get_tree().quit(1 if fails else 0)
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--rendering-driver", default="", help="vulkan or d3d12; the project default when empty")
    parser.add_argument("--temp-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="asosdot-lens-flare-", dir=args.temp_dir) as directory:
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
            f'config_version=5\n[application]\nconfig/name="Lens flare check {work.name}"\nrun/main_scene="res://main.tscn"\n'
            "[display]\nwindow/size/viewport_width=640\nwindow/size/viewport_height=360\n",
            encoding="utf-8",
        )
        command = [str(isolated), "--path", str(project)]
        if args.rendering_driver:
            command += ["--rendering-driver", args.rendering_driver]
        run(command, "LENS_FLARE fails=0", work)
        log = (work / "LENS_FLARE fails=0.log").read_text(encoding="utf-8", errors="replace")
        print("\n".join(line for line in log.splitlines() if line.startswith(("PASS", "FAIL"))), flush=True)
        print("LENS_FLARE_TEST_OK", flush=True)


if __name__ == "__main__":
    main()
