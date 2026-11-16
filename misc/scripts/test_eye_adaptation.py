#!/usr/bin/env python3
"""Check the eye-like parts of auto exposure on a real GPU (Forward+), at a fixed 60 fps.

A wall is lit and the brightness of the rendered frame is watched while the light goes out
and comes back. With auto_exposure_dark_speed at 1 the image recovers from the dark at the
same pace as from the light; at 0.2 it takes several times longer to come back up after the
lights go out, while coming back down after the lights come on stays as fast. Then a bright
spot sits in the middle of a dark wall: with auto_exposure_center_weight the exposure follows
the spot, so the rest of the frame ends up darker than with the whole screen weighted equally.
Needs a window and a GPU, so it can't run headless."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

TEST = r"""
extends Node3D

var fails := 0
var light: DirectionalLight3D
var attributes: CameraAttributesPractical
var spot: MeshInstance3D

const BRIGHT := 3.0
const DARK := 0.1


func check(p_name: String, p_ok: bool, p_detail: Variant = "") -> void:
	print(("PASS " if p_ok else "FAIL ") + p_name + "  " + str(p_detail))
	if not p_ok:
		fails += 1


# Mean brightness of a part of the frame (p_region in 0..1 of its width and height).
func brightness(p_region := Rect2(0, 0, 1, 1)) -> float:
	var image := get_viewport().get_texture().get_image()
	var total := 0.0
	var count := 0
	for iy in 12:
		for ix in 20:
			var x := int((p_region.position.x + p_region.size.x * (ix + 0.5) / 20.0) * image.get_width())
			var y := int((p_region.position.y + p_region.size.y * (iy + 0.5) / 12.0) * image.get_height())
			var c := image.get_pixel(x, y)
			total += (c.r + c.g + c.b) / 3.0
			count += 1
	return total / count


func frames(p_count: int) -> void:
	for i in p_count:
		await get_tree().process_frame


# Runs p_frames frames and returns the brightness every 4 frames, the first one taken right after the change.
func record(p_frames: int) -> PackedFloat32Array:
	var samples := PackedFloat32Array()
	await get_tree().process_frame
	for i in p_frames / 4:
		samples.append(brightness())
		await frames(4)
	return samples


# Frames until the samples have covered 90 % of the way from the first to the last one.
func time_to_90(p_samples: PackedFloat32Array) -> int:
	var first := p_samples[0]
	var last := p_samples[p_samples.size() - 1]
	for i in p_samples.size():
		if absf(p_samples[i] - last) <= 0.1 * absf(first - last):
			return i * 4
	return p_samples.size() * 4


func cycle(p_dark_speed: float) -> Dictionary:
	attributes.auto_exposure_dark_speed = p_dark_speed
	light.light_energy = BRIGHT
	await frames(240)
	light.light_energy = DARK
	var into_dark := await record(900)
	light.light_energy = BRIGHT
	var into_light := await record(900)
	return {
		"dark": time_to_90(into_dark), "light": time_to_90(into_light),
		"dark_first": into_dark[0], "dark_last": into_dark[into_dark.size() - 1],
		"light_first": into_light[0], "light_last": into_light[into_light.size() - 1],
	}


func settled_edge(p_center_weight: float) -> float:
	attributes.auto_exposure_center_weight = p_center_weight
	await frames(480)
	return brightness(Rect2(0.0, 0.0, 0.2, 1.0))


func _ready() -> void:
	var camera := Camera3D.new()
	add_child(camera)

	var wall := MeshInstance3D.new()
	var quad := QuadMesh.new()
	quad.size = Vector2(60, 40)
	wall.mesh = quad
	var wall_material := StandardMaterial3D.new()
	wall_material.albedo_color = Color(0.5, 0.5, 0.5)
	wall.material_override = wall_material
	wall.position = Vector3(0, 0, -8)
	add_child(wall)

	light = DirectionalLight3D.new()
	light.light_energy = BRIGHT
	add_child(light)

	spot = MeshInstance3D.new()
	var spot_quad := QuadMesh.new()
	spot_quad.size = Vector2(2.2, 2.2)
	spot.mesh = spot_quad
	var spot_material := StandardMaterial3D.new()
	spot_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	spot_material.albedo_color = Color(2.0, 2.0, 2.0)
	spot.material_override = spot_material
	spot.position = Vector3(0, 0, -7.9)
	spot.visible = false
	add_child(spot)

	attributes = CameraAttributesPractical.new()
	attributes.auto_exposure_enabled = true
	attributes.auto_exposure_speed = 2.0
	attributes.auto_exposure_scale = 0.4
	attributes.auto_exposure_min_sensitivity = 50.0
	attributes.auto_exposure_max_sensitivity = 800.0
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color.BLACK
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var world := WorldEnvironment.new()
	world.environment = environment
	world.camera_attributes = attributes
	add_child(world)

	print("INFO renderer: ", RenderingServer.get_current_rendering_method())

	var normal: Dictionary = await cycle(1.0)
	print("INFO dark speed 1.0: %s" % [normal])
	var slow: Dictionary = await cycle(0.2)
	print("INFO dark speed 0.2: %s" % [slow])
	check("lights out: the image really recovers", normal.dark_last > normal.dark_first * 1.5, [normal.dark_first, normal.dark_last])
	check("lights out: at the same speed the image recovers in %d frames" % normal.dark, normal.dark > 0 and normal.dark < 600, normal.dark)
	check("lights out: a dark speed of 0.2 makes it clearly slower (%d vs %d frames)" % [slow.dark, normal.dark], slow.dark > 2.5 * normal.dark, [slow.dark, normal.dark])
	check("lights on: coming back from the glare is just as fast (%d vs %d frames)" % [slow.light, normal.light], slow.light <= 1.3 * normal.light + 8 and slow.light >= 0.7 * normal.light - 8, [slow.light, normal.light])
	check("lights on: and the image gets back to the same brightness", absf(slow.light_last - normal.light_last) < 0.05, [slow.light_last, normal.light_last])

	attributes.auto_exposure_dark_speed = 1.0
	light.light_energy = 0.3
	spot.visible = true
	var even := await settled_edge(0.0)
	var weighted := await settled_edge(6.0)
	print("INFO the edge of the frame settles at %.3f with the whole screen weighted equally, %.3f weighted to the middle" % [even, weighted])
	check("a bright spot in the middle: the weighted exposure follows it, so the edges are darker", weighted < 0.8 * even, [even, weighted])
	var back := await settled_edge(0.0)
	check("and with the weight taken off the edges return to where they were", absf(back - even) < 0.03, [back, even])

	print("EYE_ADAPTATION fails=%d" % fails)
	get_tree().quit()
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--temp-dir", type=Path)
    parser.add_argument("--driver", default="d3d12", help="Rendering driver for the window (d3d12 or vulkan).")
    parser.add_argument(
        "--method",
        default="forward_plus",
        help="Rendering method (forward_plus or mobile; mobile draws the effect with fragment shaders).",
    )
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="asosdot-eye-", dir=args.temp_dir) as directory:
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
            f'config_version=5\n[application]\nconfig/name="Eye adaptation {work.name}"\nrun/main_scene="res://main.tscn"\n'
            "[debug]\nfile_logging/enable_file_logging=false\n",
            encoding="utf-8",
        )
        command = [
            str(isolated), "--path", str(project), "--rendering-driver", args.driver, "--rendering-method", args.method, "--audio-driver", "Dummy",
            "--resolution", "640x360", "--fixed-fps", "60", "--disable-vsync",
        ]  # fmt: skip
        log_path = work / "eye_adaptation.log"
        with log_path.open("w", encoding="utf-8") as stream:
            try:
                subprocess.run(command, cwd=work, stdout=stream, stderr=subprocess.STDOUT, timeout=900)
            except subprocess.TimeoutExpired:
                print("TIMEOUT: the engine did not finish in 900 s", flush=True)
        log = log_path.read_text(encoding="utf-8", errors="replace")
        print(
            "\n".join(line for line in log.splitlines() if line.startswith(("PASS", "FAIL", "INFO", "SCRIPT ERROR"))),
            flush=True,
        )
        if "EYE_ADAPTATION fails=0" not in log:
            raise SystemExit("EYE_ADAPTATION_TEST_FAILED")
        print("EYE_ADAPTATION_TEST_OK", flush=True)


if __name__ == "__main__":
    main()
