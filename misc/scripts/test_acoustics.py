#!/usr/bin/env python3
"""Check the Asosdot Acoustic Engine (AAE 0.1), headless, in a level built in code:
two rooms with a wall between them and one doorway in it, and a navigation mesh.
Sound in the open falls off with distance; a wall lets through what its material
transmits, several walls multiply; with nothing getting through it comes round
through the doorway, from the doorway, further and quieter; colliders without a
material use the default, CSG shapes have one too; enemies hear footsteps through
the doorway but not their own, nor sounds they don't listen for, nor sounds too
quiet or too far for their ears; a 3D audio player with acoustics on is heard from
the doorway and muffled through a thin wall; and the listener's room drives a
reverb bus that gets shorter when the room is made absorbent."""

from __future__ import annotations

import argparse
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

TEST = r"""
extends Node3D

var fails := 0
var heard := []
var emitted := 0
var wall_material := AcousticMaterial.new()
var floor_material := AcousticMaterial.new()

const A := Vector3(-5, 1, -5)
const B := Vector3(5, 1, -5)
const C := Vector3(5, 1, 5)


func check(p_name: String, p_ok: bool, p_detail: Variant = "") -> void:
	print(("PASS " if p_ok else "FAIL ") + p_name + "  " + str(p_detail))
	if not p_ok:
		fails += 1


func near(p_a: float, p_b: float, p_tolerance := 0.01) -> bool:
	return absf(p_a - p_b) <= p_tolerance


func box(p_parent: Node, p_size: Vector3, p_position: Vector3, p_material: AcousticMaterial) -> StaticBody3D:
	var body := StaticBody3D.new()
	var shape := CollisionShape3D.new()
	var box_shape := BoxShape3D.new()
	box_shape.size = p_size
	shape.shape = box_shape
	body.add_child(shape)
	body.position = p_position
	body.acoustic_material = p_material
	p_parent.add_child(body)
	return body


func character(p_name: String, p_position: Vector3) -> CharacterBody3D:
	var body := CharacterBody3D.new()
	body.name = p_name
	var shape := CollisionShape3D.new()
	shape.shape = CapsuleShape3D.new()
	body.add_child(shape)
	body.position = p_position
	add_child(body)
	return body


func ears(p_parent: Node, p_name: String) -> AcousticHearing3D:
	var hearing := AcousticHearing3D.new()
	hearing.name = p_name
	p_parent.add_child(hearing)
	hearing.sound_heard.connect(func(p_event: AcousticEvent, p_loudness: float, p_from: Vector3) -> void: heard.append([String(hearing.name), p_loudness, p_from]))
	return hearing


func physics_frames(p_count: int) -> void:
	for i in p_count:
		await get_tree().physics_frame


func seconds(p_time: float) -> void:
	await get_tree().create_timer(p_time).timeout


func sound(p_source: Node, p_position: Vector3, p_type: String, p_intensity: float) -> int:
	var event := AcousticEvent.new()
	event.source = p_source
	event.position = p_position
	event.sound_type = p_type
	event.intensity = p_intensity
	heard.clear()
	return AcousticEngine.emit_sound(event)


func heard_by(p_name: String) -> Array:
	for entry in heard:
		if entry[0] == p_name:
			return entry
	return []


func _ready() -> void:
	run.call_deferred()


func run() -> void:
	# The materials.
	var material := AcousticMaterial.new()
	material.absorption = 0.65
	material.transmission = 0.25
	material.scattering = 0.4
	check("a material keeps what it is given", near(material.absorption, 0.65) and near(material.transmission, 0.25) and near(material.scattering, 0.4))
	material.absorption = 3.0
	material.transmission = -1.0
	check("a material keeps to 0 to 1", material.absorption == 1.0 and material.transmission == 0.0)

	# Two rooms, x < 0 and x > 0, with a wall between them from one edge of the
	# floor to the other, and a doorway in it between z = 6 and z = 7.5.
	var region := NavigationRegion3D.new()
	var navigation_mesh := NavigationMesh.new()
	navigation_mesh.geometry_parsed_geometry_type = NavigationMesh.PARSED_GEOMETRY_STATIC_COLLIDERS
	navigation_mesh.agent_radius = 0.3
	region.navigation_mesh = navigation_mesh
	add_child(region)
	box(region, Vector3(40, 1, 20), Vector3(0, -0.5, 0), floor_material)
	box(region, Vector3(0.4, 4, 16), Vector3(0, 2, -2), wall_material)
	box(region, Vector3(0.4, 4, 2.5), Vector3(0, 2, 8.75), wall_material)
	region.bake_navigation_mesh(false)
	await physics_frames(4)
	check("the level has a navigation mesh", navigation_mesh.get_polygon_count() > 0, navigation_mesh.get_polygon_count())

	AcousticEngine.sound_emitted.connect(func(_p_event: AcousticEvent) -> void: emitted += 1)

	# Through the air.
	var open: Dictionary = AcousticEngine.trace_sound(A, Vector3(-5, 1, 5))
	check("in the open a sound falls off with distance", near(open.loudness, 0.75) and not open.around and open.transmission == 1.0, open)

	# Through the wall.
	wall_material.transmission = 0.25
	var through: Dictionary = AcousticEngine.trace_sound(B, A)
	check("a wall lets through what its material transmits", near(through.transmission, 0.25), through.transmission)
	var panel := box(self, Vector3(0.2, 3, 2), Vector3(2.5, 1.5, -5), material)
	material.transmission = 0.5
	await physics_frames(2)
	var two: Dictionary = AcousticEngine.trace_sound(B, A)
	check("two walls multiply", near(two.transmission, 0.125), two.transmission)
	panel.acoustic_material = null
	var fallback: Dictionary = AcousticEngine.trace_sound(B, A)
	check("a collider without a material uses the default", near(fallback.transmission, 0.25 * 0.05, 0.001), fallback.transmission)
	panel.free()
	var csg := CSGBox3D.new()
	csg.size = Vector3(0.2, 3, 2)
	csg.position = Vector3(2.5, 1.5, -5)
	csg.use_collision = true
	csg.acoustic_material = material
	add_child(csg)
	await physics_frames(3)
	var through_csg: Dictionary = AcousticEngine.trace_sound(B, A)
	check("a CSG shape has a material too", near(through_csg.transmission, 0.125), through_csg.transmission)
	csg.free()
	await physics_frames(2)

	# Round through the doorway.
	wall_material.transmission = 0.0
	var around: Dictionary = AcousticEngine.trace_sound(B, A)
	var seems: Vector3 = around.apparent_position
	check("with nothing getting through, it comes round through the doorway", around.around and around.loudness > 0.0 and around.corners >= 1, around)
	check("further than straight, and quieter", around.distance > 20.0 and around.loudness < 0.75 * 0.5, [around.distance, around.loudness])
	# One doorway is one bend, however many points the navigation mesh puts on it.
	check("a doorway costs what the sound bends round it", around.bend > 90.0 and around.bend < 200.0 and around.loudness > 0.2, [around.bend, around.loudness])
	check("from the doorway", absf(seems.x) < 1.5 and seems.z > 5.0 and seems.z < 8.5, seems)

	# Enemies.
	var enemy := character("Enemy", A)
	var enemy_ears := ears(enemy, "EnemyEars")
	var player := character("Player", B)
	ears(player, "PlayerEars")
	var picky := character("Picky", A + Vector3(0, 0, 1.5))
	var picky_ears := ears(picky, "PickyEars")
	picky_ears.sound_types = PackedStringArray(["gunshot"])
	var near_ears := ears(self, "NearEars")
	near_ears.position = C
	await physics_frames(2)

	var count := sound(player, B, "footsteps", 1.0)
	var enemy_heard := heard_by("EnemyEars")
	check("an enemy hears footsteps through the doorway", not enemy_heard.is_empty() and enemy_heard[1] > 0.0, enemy_heard)
	check("and knows where they came in", not enemy_heard.is_empty() and absf(enemy_heard[2].x) < 1.5 and enemy_heard[2].z > 5.0, enemy_heard)
	check("nobody hears their own footsteps", heard_by("PlayerEars").is_empty())
	check("ears that listen for something else don't hear them", heard_by("PickyEars").is_empty())
	check("emit_sound says how many heard", count == heard.size() and count >= 2, [count, heard])
	check("and the engine says a sound was made", emitted == 1, emitted)
	sound(player, B, "gunshot", 1.0)
	check("ears hear what they listen for", not heard_by("PickyEars").is_empty())

	sound(player, B, "footsteps", 0.2)
	check("a quiet sound doesn't carry", heard_by("NearEars").is_empty() and heard_by("EnemyEars").is_empty(), heard)
	near_ears.sensitivity = 2.0
	sound(player, B, "footsteps", 0.2)
	var keen := heard_by("NearEars")
	check("keener ears hear it from further", not keen.is_empty() and near(keen[1], 0.375, 0.02), keen)
	near_ears.min_loudness = 0.5
	sound(player, B, "footsteps", 0.2)
	check("too quiet for an ear's least loudness isn't heard", heard_by("NearEars").is_empty())
	near_ears.min_loudness = 0.02
	near_ears.sensitivity = 1.0

	# One that frees another when it hears: nothing breaks.
	near_ears.sound_heard.connect(func(_e: AcousticEvent, _l: float, _f: Vector3) -> void:
		if is_instance_valid(picky):
			picky.free()
	)
	sound(player, B, "gunshot", 1.0)
	check("ears freed while a sound is being heard don't break it", not is_instance_valid(picky))

	# A 3D audio player with acoustics, heard from the enemy's place.
	var camera := Camera3D.new()
	# Beside the enemy rather than in it, or its body would be a wall.
	camera.position = A + Vector3(0, 0.7, 2)
	add_child(camera)
	camera.make_current()
	var speaker := AudioStreamPlayer3D.new()
	speaker.position = B
	var generator := AudioStreamGenerator.new()
	speaker.stream = generator
	speaker.acoustics = true
	add_child(speaker)
	speaker.play()
	await seconds(1.0)
	var state: Dictionary = speaker.get_acoustic_state()
	var offset: Vector3 = state.apparent_offset
	check("a player with acoustics is traced", state.valid, state)
	check("and heard from the doorway, as far as the way round", offset.z > 4.0 and offset.length() > 20.0, offset)
	check("and quieter for the corners", state.gain < 1.0, state.gain)

	wall_material.transmission = 0.9
	await seconds(1.0)
	state = speaker.get_acoustic_state()
	offset = state.apparent_offset
	check("through a thin wall it is heard straight through", offset.x > 8.0 and offset.z < 0.0 and offset.length() < 12.0, offset)
	check("a little quieter and muffled", near(state.gain, 0.9, 0.03) and state.muffle > 0.02 and state.muffle < 0.15, state)

	# The room around the listener.
	var bus := AudioServer.get_bus_index(ProjectSettings.get_setting("audio/acoustics/reverb_bus"))
	var has_reverb := bus >= 0 and AudioServer.get_bus_effect_count(bus) > 0 and AudioServer.get_bus_effect(bus, 0) is AudioEffectReverb
	check("a reverb bus is made for the listener's room", has_reverb, bus)
	var room: Dictionary = AcousticEngine.get_room()
	check("the room is measured", room.enclosure > 0.0 and room.reverb_time > 0.0 and state.reverb_send > 0.0, [room, state.reverb_send])
	var reflective_time: float = room.reverb_time
	wall_material.absorption = 0.95
	floor_material.absorption = 0.95
	await seconds(2.0)
	room = AcousticEngine.get_room()
	check("an absorbent room rings for less long", room.reverb_time < reflective_time * 0.5, [reflective_time, room.reverb_time])

	speaker.acoustics = false
	check("turning acoustics off forgets the way", not speaker.get_acoustic_state().valid)

	print("ACOUSTICS fails=%d" % fails)
	get_tree().quit(1 if fails else 0)
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--temp-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="asosdot-acoustics-", dir=args.temp_dir) as directory:
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
            f'config_version=5\n[application]\nconfig/name="Acoustics {work.name}"\nrun/main_scene="res://main.tscn"\n',
            encoding="utf-8",
        )
        command = [str(isolated), "--headless", "--path", str(project)]
        run(command, "ACOUSTICS fails=0", work)
        log = (work / "ACOUSTICS fails=0.log").read_text(encoding="utf-8", errors="replace")
        print("\n".join(line for line in log.splitlines() if line.startswith(("PASS", "FAIL"))), flush=True)
        print("ACOUSTICS_TEST_OK", flush=True)


if __name__ == "__main__":
    main()
