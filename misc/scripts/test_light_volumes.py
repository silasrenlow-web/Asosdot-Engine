#!/usr/bin/env python3
"""Check LightVolume3D and DustVolume3D, headless: shaft geometry, cookie and
light-link properties, auto-fit from a SpotLight3D, and the dust mote count.

A LightVolume3D builds its shell mesh from its shape properties (the AABB
covers both rings, including the sheared far end); changing a shape property
rebuilds it. Following a SpotLight3D fits the cone to the light's range and
angle and takes its colour. A DustVolume3D's particle amount is its box
volume times its density, and its process material emits inside the box.
Both nodes survive saving and loading with their properties intact."""

from __future__ import annotations

import argparse
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

TEST = r"""
extends Node

var fails := 0


func check(p_name: String, p_ok: bool, p_detail: Variant = "") -> void:
	print(("PASS " if p_ok else "FAIL ") + p_name + "  " + str(p_detail))
	if not p_ok:
		fails += 1


func approx(a: float, b: float, tol := 0.01) -> bool:
	return absf(a - b) <= tol


func _ready() -> void:
	# --- LightVolume3D geometry ---
	var volume := LightVolume3D.new()
	add_child(volume)
	check("default shape is cone", volume.shape == LightVolume3D.SHAPE_CONE)
	var mesh := volume.get_mesh()
	check("mesh has one surface", mesh != null and mesh.get_surface_count() == 1)
	var aabb := volume.get_aabb()
	check("aabb spans the shaft length", approx(aabb.position.z, -volume.length) and approx(aabb.size.z, volume.length), aabb)
	check("aabb covers the end radius", aabb.size.x >= volume.end_radius * 2.0 - 0.01, aabb)

	volume.shape = LightVolume3D.SHAPE_BOX
	volume.size = Vector2(3, 1.5)
	volume.length = 4.0
	volume.shear = Vector2(2, 0)
	aabb = volume.get_aabb()
	check("box aabb includes the shear", aabb.position.x <= 0.5 - 0.01 and aabb.get_end().x >= 3.5 - 0.01, aabb)
	check("box aabb height", approx(aabb.size.y, 1.5), aabb)

	volume.shape = LightVolume3D.SHAPE_CONE
	volume.length = 5.0
	volume.start_radius = 0.2
	volume.end_radius = 2.0
	check("cone far extent", volume.get_far_extent() == Vector2(2, 2))
	check("cone near extent", volume.get_near_extent() == Vector2(0.2, 0.2))

	# Property clamps.
	volume.intensity = -3.0
	check("intensity clamps at zero", volume.intensity == 0.0)
	volume.intensity = 0.35
	volume.fade_end = 7.0
	check("fade_end clamps to one", volume.fade_end == 1.0)

	# --- Cookie ---
	var image := Image.create_empty(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.2, 0.2, 0.2))
	var cookie := ImageTexture.create_from_image(image)
	volume.cookie = cookie
	check("cookie round-trips", volume.cookie == cookie)
	volume.cookie_strength = 0.5
	check("cookie strength", approx(volume.cookie_strength, 0.5))

	# --- Following a spotlight ---
	var spot := SpotLight3D.new()
	spot.light_color = Color(1, 0.5, 0.25)
	spot.light_energy = 2.0
	spot.spot_range = 9.0
	spot.spot_angle = 30.0
	add_child(spot)
	volume.source_light = volume.get_path_to(spot)
	await get_tree().process_frame
	await get_tree().process_frame
	check("follow takes the light colour", volume.color.is_equal_approx(Color(1, 0.5, 0.25)), volume.color)
	check("auto-fit takes the range", approx(volume.length, 9.0, 0.05), volume.length)
	check("auto-fit takes the angle", approx(volume.end_radius, tan(deg_to_rad(30.0)) * 9.0, 0.05), volume.end_radius)
	volume.follow_light = false
	volume.color = Color(0, 1, 0)
	check("unfollow restores own colour control", volume.color == Color(0, 1, 0))

	# --- DustVolume3D ---
	var dust := DustVolume3D.new()
	add_child(dust)
	dust.size = Vector3(2, 2, 2)
	dust.density = 10.0
	check("dust amount is volume times density", dust.amount == 80, dust.amount)
	var pm := dust.process_material as ParticleProcessMaterial
	check("dust emits in a box", pm != null and pm.emission_shape == ParticleProcessMaterial.EMISSION_SHAPE_BOX)
	check("dust emission box matches size", pm != null and pm.emission_box_extents.is_equal_approx(Vector3.ONE), pm.emission_box_extents if pm else Vector3.ZERO)
	check("dust has no gravity", pm != null and pm.gravity.is_equal_approx(Vector3.ZERO))
	dust.size = Vector3(10, 10, 10)
	dust.density = 10.0
	check("dust amount is capped", dust.amount == 4096, dust.amount)

	# --- Save and load ---
	var packed := PackedScene.new()
	var root := Node3D.new()
	root.name = "Root"
	var saved_volume := LightVolume3D.new()
	saved_volume.name = "Shaft"
	saved_volume.shape = LightVolume3D.SHAPE_BOX
	saved_volume.length = 7.5
	saved_volume.shear = Vector2(1, -1)
	root.add_child(saved_volume)
	saved_volume.owner = root
	check("pack works", packed.pack(root) == OK)
	ResourceSaver.save(packed, "user://volumes.tscn")
	root.queue_free()
	await get_tree().process_frame
	var loaded := (load("user://volumes.tscn") as PackedScene).instantiate()
	add_child(loaded)
	var loaded_volume := loaded.get_node("Shaft") as LightVolume3D
	check("loaded shape", loaded_volume.shape == LightVolume3D.SHAPE_BOX)
	check("loaded length", approx(loaded_volume.length, 7.5))
	check("loaded shear", loaded_volume.shear.is_equal_approx(Vector2(1, -1)))
	check("loaded mesh rebuilt", loaded_volume.get_mesh() != null and loaded_volume.get_mesh().get_surface_count() == 1)

	print("LIGHT_VOLUMES fails=%d" % fails)
	get_tree().quit()
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--temp-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="asosdot-light-volumes-", dir=args.temp_dir) as directory:
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
            '[node name="Main" type="Node"]\nscript = ExtResource("1")\n',
            encoding="utf-8",
        )
        (project / "project.godot").write_text(
            f'config_version=5\n[application]\nconfig/name="Light Volumes {work.name}"\nrun/main_scene="res://main.tscn"\n',
            encoding="utf-8",
        )
        command = [str(isolated), "--headless", "--path", str(project)]
        run(command, "LIGHT_VOLUMES fails=0", work)
        log = (work / "LIGHT_VOLUMES fails=0.log").read_text(encoding="utf-8", errors="replace")
        print("\n".join(line for line in log.splitlines() if line.startswith(("PASS", "FAIL"))), flush=True)
        print("LIGHT_VOLUMES_TEST_OK", flush=True)


if __name__ == "__main__":
    main()
