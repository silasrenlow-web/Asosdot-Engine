#!/usr/bin/env python3
"""Check LightCatcher3D: its brightness against the renderer's light formulas,
walls blocking light, spot cones, the sun, its signals and hysteresis."""

from __future__ import annotations

import argparse
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

TEST = r"""
extends Node3D

var fails := 0
var catcher: LightCatcher3D
var player: CharacterBody3D
var events := []


func check(p_name: String, p_ok: bool, p_detail: Variant = "") -> void:
	print(("PASS " if p_ok else "FAIL ") + p_name + "  " + str(p_detail))
	if not p_ok:
		fails += 1


func near(a: float, b: float, eps := 1e-4) -> bool:
	return absf(a - b) <= eps * maxf(1.0, absf(b))


func physics(p_frames := 2) -> void:
	for i in p_frames:
		await get_tree().physics_frame


func wall(p_size: Vector3, p_pos: Vector3, p_parent: Node = self) -> StaticBody3D:
	var body := StaticBody3D.new()
	var shape := CollisionShape3D.new()
	var box := BoxShape3D.new()
	box.size = p_size
	shape.shape = box
	body.add_child(shape)
	body.position = p_pos
	p_parent.add_child(body)
	return body


# The renderer's omni falloff, written out separately from the C++ to check it.
func omni(d: float, r: float, decay: float) -> float:
	var nd := pow(d / r, 4.0)
	return pow(maxf(1.0 - nd, 0.0), 2.0) * pow(d, -decay)


func _ready() -> void:
	player = CharacterBody3D.new()
	var capsule := CollisionShape3D.new()
	capsule.shape = CapsuleShape3D.new()
	capsule.position = Vector3(0, 1, 0)
	player.add_child(capsule)
	add_child(player)
	catcher = LightCatcher3D.new()
	catcher.sample_points = PackedVector3Array([Vector3(0, 1, 0)])
	catcher.threshold = 0.1
	player.add_child(catcher)
	catcher.lit_changed.connect(func(lit: bool) -> void: events.append("lit" if lit else "dark"))
	catcher.light_entered.connect(func(l: Light3D) -> void: events.append("enter " + l.name))
	catcher.light_exited.connect(func(l: Light3D) -> void: events.append("exit " + l.name))
	run.call_deferred()


func run() -> void:
	await physics()
	check("dark with no lights", catcher.get_brightness() == 0.0 and not catcher.is_lit(), catcher.get_brightness())

	var lamp := OmniLight3D.new()
	lamp.name = "Lamp"
	lamp.omni_range = 5.0
	lamp.light_energy = 1.0
	lamp.position = Vector3(0, 1, -2)
	add_child(lamp)
	await physics()
	var expected := omni(2.0, 5.0, 1.0)
	check("omni matches the renderer's falloff", near(catcher.get_brightness(), expected), "%f vs %f" % [catcher.get_brightness(), expected])
	check("lit and signalled", catcher.is_lit() and events.has("lit") and events.has("enter Lamp"), events)
	check("brightest light is the lamp", catcher.get_brightest_light() == lamp)

	lamp.light_color = Color(1, 0, 0)
	await physics()
	var c := catcher.get_light_color()
	check("light colour is the lamp's", c.r > 0.0 and c.g == 0.0 and c.b == 0.0, c)
	lamp.light_color = Color(1, 1, 1)

	lamp.light_energy = 2.0
	await physics()
	check("energy scales linearly", near(catcher.get_brightness(), expected * 2.0), catcher.get_brightness())
	lamp.light_energy = 1.0

	lamp.position = Vector3(0, 1, -6)
	await physics()
	check("out of range gives nothing", catcher.get_brightness() == 0.0 and not catcher.is_lit(), catcher.get_brightness())
	check("leaving signalled", events.has("dark") and events.has("exit Lamp"), events)
	lamp.position = Vector3(0, 1, -2)

	# A wall between them.
	var blocker := wall(Vector3(4, 4, 0.2), Vector3(0, 1, -1))
	await physics()
	check("a wall blocks the light", catcher.get_brightness() == 0.0, catcher.get_brightness())
	catcher.occlusion = LightCatcher3D.OCCLUSION_SHADOWED_LIGHTS
	await physics()
	check("an unshadowed light shines through, as it renders", near(catcher.get_brightness(), expected), catcher.get_brightness())
	lamp.shadow_enabled = true
	lamp.shadow_opacity = 0.75
	await physics()
	check("a shadowed light is dimmed by its shadow opacity", near(catcher.get_brightness(), expected * 0.25), catcher.get_brightness())
	catcher.occlusion = LightCatcher3D.OCCLUSION_DISABLED
	await physics()
	check("occlusion off ignores walls", near(catcher.get_brightness(), expected), catcher.get_brightness())
	catcher.occlusion = LightCatcher3D.OCCLUSION_ALWAYS
	blocker.queue_free()

	# A lamp fitting with collision round the bulb lights the room, and the
	# player's own capsule doesn't shadow its own head.
	var fitting := StaticBody3D.new()
	var fitting_shape := CollisionShape3D.new()
	var sphere := SphereShape3D.new()
	sphere.radius = 0.4
	fitting_shape.shape = sphere
	fitting.add_child(fitting_shape)
	fitting.position = Vector3(0, 1, -2)
	lamp.reparent(fitting, false)
	lamp.position = Vector3()
	add_child(fitting)
	catcher.sample_points = PackedVector3Array([Vector3(0, 1, 0.3)])
	await physics()
	var through_self := omni(Vector3(0, 1, 0.3).distance_to(Vector3(0, 1, -2)), 5.0, 1.0)
	check("the lamp's fitting and the player's body don't block", near(catcher.get_brightness(), through_self), "%f vs %f" % [catcher.get_brightness(), through_self])
	catcher.sample_points = PackedVector3Array([Vector3(0, 1, 0)])
	fitting.queue_free()
	await physics()
	check("a freed light stops counting", catcher.get_brightness() == 0.0, catcher.get_brightness())

	# A flashlight: on the player, then swung away.
	var flashlight := SpotLight3D.new()
	flashlight.name = "Flashlight"
	flashlight.spot_range = 10.0
	flashlight.spot_angle = 20.0
	flashlight.light_energy = 3.0
	add_child(flashlight)
	flashlight.position = Vector3(0, 1, 5)
	flashlight.look_at(Vector3(0, 1, 0))
	events.clear()
	await physics()
	var spot_expected := 3.0 * omni(5.0, 10.0, 1.0)
	check("a spot aimed at the point lights it fully", near(catcher.get_brightness(), spot_expected), "%f vs %f" % [catcher.get_brightness(), spot_expected])
	check("the beam landing is signalled", events.has("enter Flashlight"), events)
	flashlight.look_at(Vector3(5, 1, 0))
	await physics()
	check("swung away it lights nothing", catcher.get_brightness() == 0.0 and events.has("exit Flashlight"), catcher.get_brightness())
	flashlight.look_at(Vector3(0, 1, 0))
	flashlight.rotate_object_local(Vector3.UP, deg_to_rad(12.0))
	await physics()
	var edge := catcher.get_brightness()
	check("near the cone's edge it fades", edge > 0.0 and edge < spot_expected, "%f of %f" % [edge, spot_expected])
	flashlight.queue_free()

	# The sun, and a roof.
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-90, 0, 0)
	sun.light_energy = 1.5
	add_child(sun)
	await physics()
	check("the sun lights by its energy", near(catcher.get_brightness(), 1.5), catcher.get_brightness())
	var roof := wall(Vector3(10, 0.2, 10), Vector3(0, 4, 0))
	await physics()
	check("a roof shades the sun", catcher.get_brightness() == 0.0, catcher.get_brightness())
	roof.queue_free()
	await physics()

	# A negative light takes light away.
	var shade := OmniLight3D.new()
	shade.light_negative = true
	shade.omni_range = 5.0
	shade.light_energy = 0.5
	shade.position = Vector3(0, 1, -1)
	add_child(shade)
	await physics()
	check("a negative light subtracts", near(catcher.get_brightness(), 1.5 - 0.5 * omni(1.0, 5.0, 1.0)), catcher.get_brightness())
	shade.queue_free()
	sun.light_energy = 1.0
	await physics()

	# Hysteresis: dimming to just under the threshold keeps it lit; well
	# under turns it dark.
	events.clear()
	sun.light_energy = 0.09
	await physics()
	check("just under the threshold stays lit", catcher.is_lit() and not events.has("dark"), catcher.get_brightness())
	sun.light_energy = 0.05
	await physics()
	check("well under the threshold goes dark", not catcher.is_lit() and events.has("dark"), catcher.get_brightness())
	sun.light_energy = 1.0

	# Smoothing.
	catcher.response_time = 0.5
	sun.light_energy = 0.0
	await physics(60)
	sun.light_energy = 1.0
	await physics(3)
	var rising := catcher.get_brightness()
	check("response time smooths the change", rising > 0.0 and rising < 0.5, rising)
	catcher.response_time = 0.0

	# Measuring any point on demand, with a surface normal.
	check("measure_point facing the sun", near(catcher.measure_point(Vector3(3, 1, 3)), 1.0), catcher.measure_point(Vector3(3, 1, 3)))
	check("measure_point on a wall facing sideways", near(catcher.measure_point(Vector3(3, 1, 3), Vector3(1, 0, 0)), 0.0), catcher.measure_point(Vector3(3, 1, 3), Vector3(1, 0, 0)))

	# The light mask.
	sun.layers = 2
	catcher.light_mask = 1
	await physics()
	check("the light mask leaves other layers out", catcher.get_brightness() == 0.0, catcher.get_brightness())
	sun.queue_free()
	catcher.light_mask = 0xFFFFFFFF
	await physics()

	await beam_on_wall()
	await cookies()
	await distance_fade()
	if DisplayServer.get_name() != "headless":
		await shadow_casters()
		await cookies_match_screen()
	else:
		print("SKIP shadow casters and screen comparison: they need a renderer")

	print("LIGHT_CATCHER fails=%d" % fails)
	get_tree().quit(1 if fails else 0)


# An enemy looks where the player's flashlight beam lands on a wall, though
# the player is out of sight.
func beam_on_wall() -> void:
	var far_wall := wall(Vector3(8, 4, 0.2), Vector3(0, 2, -6))
	var flashlight := SpotLight3D.new()
	flashlight.spot_range = 12.0
	flashlight.spot_angle = 15.0
	flashlight.light_energy = 4.0
	flashlight.shadow_enabled = true
	add_child(flashlight)
	flashlight.position = Vector3(1, 1.2, 2)
	flashlight.look_at(Vector3(0, 1.5, -6))
	await physics()
	var space := get_world_3d().direct_space_state
	var from := flashlight.global_position
	var hit := space.intersect_ray(PhysicsRayQueryParameters3D.create(from, from - flashlight.global_basis.z * 20.0))
	check("the beam's ray finds the wall", not hit.is_empty() and absf(hit.position.z + 5.9) < 0.01, hit.get("position"))
	if not hit.is_empty():
		for mode in [LightCatcher3D.OCCLUSION_SOURCE_COLLIDERS, LightCatcher3D.OCCLUSION_SOURCE_BOTH]:
			catcher.occlusion_source = mode
			var on_wall := catcher.measure_point(hit.position, hit.normal)
			check("the lit spot on the wall reads lit (source %d)" % mode, on_wall > catcher.threshold, on_wall)
		var beside := catcher.measure_point(hit.position + Vector3(3.5, 0, 0), hit.normal)
		check("the wall beside the beam reads dark", beside == 0.0, beside)
	catcher.occlusion_source = LightCatcher3D.OCCLUSION_SOURCE_COLLIDERS
	flashlight.queue_free()
	far_wall.queue_free()
	await physics()


func cookie_texture(p_white: Callable) -> ImageTexture:
	var image := Image.create_empty(64, 64, false, Image.FORMAT_RGBA8)
	for y in 64:
		for x in 64:
			image.set_pixel(x, y, Color.WHITE if p_white.call(x, y) else Color.BLACK)
	return ImageTexture.create_from_image(image)


# A cookie whose left half is white: the light reaches its left and not its
# right, seen from the light.
func cookies() -> void:
	var spot := SpotLight3D.new()
	spot.spot_range = 10.0
	spot.spot_angle = 30.0
	spot.light_energy = 3.0
	spot.light_projector = cookie_texture(func(x: int, _y: int) -> bool: return x < 32)
	spot.shadow_enabled = true # cookies need shadows, as Light3D.light_projector says
	add_child(spot)
	spot.position = Vector3(0, 1, 5)
	spot.look_at(Vector3(0, 1, 0))
	await physics()
	# Seen from the spot, looking down -Z towards the player, its local +X is
	# the world's -X here.
	var right_of_light := spot.global_transform * Vector3(1.0, 0, -5)
	var left_of_light := spot.global_transform * Vector3(-1.0, 0, -5)
	var dark := catcher.measure_point(right_of_light)
	var lit := catcher.measure_point(left_of_light)
	check("a spot cookie's black half blocks light", dark == 0.0, dark)
	check("a spot cookie's white half lets it through", lit > 0.1, lit)
	spot.light_projector = cookie_texture(func(_x: int, y: int) -> bool: return y < 32)
	await physics()
	var up := catcher.measure_point(spot.global_transform * Vector3(0, 1.0, -5))
	var down := catcher.measure_point(spot.global_transform * Vector3(0, -1.0, -5))
	check("the cookie's top is the light's up", up > 0.1 and down == 0.0, "%f / %f" % [up, down])
	spot.light_projector = null
	await physics()
	check("removing the cookie lights both halves", catcher.measure_point(right_of_light) > 0.1)
	spot.light_projector = cookie_texture(func(x: int, _y: int) -> bool: return x < 32)
	spot.shadow_enabled = false
	await physics()
	check("without shadows a cookie does nothing, as in the renderer", catcher.measure_point(right_of_light) > 0.1)
	spot.queue_free()

	var omni_light := OmniLight3D.new()
	omni_light.omni_range = 10.0
	omni_light.light_projector = cookie_texture(func(_x: int, _y: int) -> bool: return false)
	omni_light.shadow_enabled = true
	add_child(omni_light)
	omni_light.position = Vector3(0, 3, 0)
	await physics()
	check("an all-black omni cookie blocks everything", catcher.get_brightness() == 0.0, catcher.get_brightness())
	omni_light.queue_free()
	await physics()


func distance_fade() -> void:
	var camera := Camera3D.new()
	add_child(camera)
	camera.make_current()
	var lamp := OmniLight3D.new()
	lamp.omni_range = 6.0
	lamp.position = Vector3(0, 1, -2)
	lamp.distance_fade_enabled = true
	lamp.distance_fade_begin = 20.0
	lamp.distance_fade_length = 5.0
	add_child(lamp)
	camera.position = Vector3(0, 1, 2)
	await physics()
	var close := catcher.get_brightness()
	camera.position = Vector3(0, 1, 60)
	await physics()
	var far := catcher.get_brightness()
	check("a light faded out by distance gives nothing, as on screen", close > 0.1 and far == 0.0, "%f / %f" % [close, far])
	lamp.queue_free()
	camera.queue_free()
	await physics()


# With shadow casters as the source, occlusion follows what's drawn: a wall
# with no collision blocks, a collider with no mesh doesn't, and a mesh that
# doesn't cast shadows doesn't.
func shadow_casters() -> void:
	var lamp := OmniLight3D.new()
	lamp.omni_range = 6.0
	lamp.shadow_enabled = true
	lamp.position = Vector3(0, 1, -2)
	add_child(lamp)
	var visible_wall := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = Vector3(4, 4, 0.2)
	visible_wall.mesh = box
	visible_wall.position = Vector3(0, 1, -1)
	add_child(visible_wall)
	await physics()
	catcher.occlusion_source = LightCatcher3D.OCCLUSION_SOURCE_COLLIDERS
	await physics()
	check("colliders: a wall with no collision doesn't block", catcher.get_brightness() > 0.1, catcher.get_brightness())
	catcher.occlusion_source = LightCatcher3D.OCCLUSION_SOURCE_SHADOW_CASTERS
	await physics()
	check("shadow casters: the drawn wall blocks", catcher.get_brightness() == 0.0, catcher.get_brightness())
	visible_wall.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	await physics()
	check("shadow casters: a mesh that casts no shadow doesn't block", catcher.get_brightness() > 0.1, catcher.get_brightness())
	visible_wall.queue_free()
	var invisible := wall(Vector3(4, 4, 0.2), Vector3(0, 1, -1))
	await physics()
	check("shadow casters: an invisible collider doesn't block", catcher.get_brightness() > 0.1, catcher.get_brightness())
	catcher.occlusion_source = LightCatcher3D.OCCLUSION_SOURCE_BOTH
	await physics()
	check("both: the collider blocks", catcher.get_brightness() == 0.0, catcher.get_brightness())
	catcher.occlusion_source = LightCatcher3D.OCCLUSION_SOURCE_COLLIDERS
	invisible.queue_free()
	lamp.queue_free()
	await physics()


# What counts: the catcher agrees with the pixels the renderer draws. A white
# floor, lit only through a cookie, looked at from above; every sample the
# catcher calls lit must be lit on screen and every dark one dark.
func cookies_match_screen() -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color.BLACK
	env.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var world_env := WorldEnvironment.new()
	world_env.environment = env
	add_child(world_env)
	var floor_mesh := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(30, 30)
	var white := StandardMaterial3D.new()
	white.albedo_color = Color.WHITE
	plane.material = white
	floor_mesh.mesh = plane
	floor_mesh.position = Vector3(0, -0.5, 20)
	add_child(floor_mesh)
	var camera := Camera3D.new()
	add_child(camera)
	camera.position = Vector3(0, 12, 20)
	camera.rotation_degrees = Vector3(-90, 0, 0)
	camera.fov = 70
	camera.make_current()
	catcher.occlusion = LightCatcher3D.OCCLUSION_DISABLED

	# White in the top-left quarter only, so both axes are checked.
	var quarter := cookie_texture(func(x: int, y: int) -> bool: return x < 32 and y < 32)
	var lights := []
	var spot := SpotLight3D.new()
	spot.spot_range = 20.0
	spot.spot_angle = 45.0
	spot.spot_attenuation = 0.0001
	spot.light_energy = 4.0
	spot.light_projector = quarter
	spot.shadow_enabled = true
	spot.position = Vector3(0, 4, 20)
	spot.rotation_degrees = Vector3(-90, 30, 0)
	lights.append(spot)
	var omni_light := OmniLight3D.new()
	omni_light.omni_range = 20.0
	omni_light.light_energy = 4.0
	omni_light.light_projector = cookie_texture(func(x: int, y: int) -> bool: return x < 32 and (y % 32) < 16)
	omni_light.shadow_enabled = true
	omni_light.position = Vector3(0, 3, 20)
	omni_light.rotation_degrees = Vector3(20, 30, 10)
	lights.append(omni_light)

	for light in lights:
		add_child(light)
		for i in 8:
			await get_tree().process_frame
		await RenderingServer.frame_post_draw
		var image := get_viewport().get_texture().get_image()
		var agree := 0
		var total := 0
		var disagreements := []
		for gz in range(-8, 9):
			for gx in range(-8, 9):
				var p := Vector3(gx * 0.7, -0.5, 20 + gz * 0.7)
				var measured := catcher.measure_point(p, Vector3.UP)
				var screen := camera.unproject_position(p)
				if screen.x < 2 or screen.y < 2 or screen.x >= image.get_width() - 2 or screen.y >= image.get_height() - 2:
					continue
				var pixel := image.get_pixelv(Vector2i(screen)).get_luminance()
				# Skip points right on a cookie edge, where the renderer's
				# filtering and mipmaps blend: where the catcher's own answer
				# changes within a quarter metre, or the screen is half lit.
				if pixel > 0.03 and pixel < 0.3:
					continue
				var on_edge := false
				for offset in [Vector3(0.25, 0, 0), Vector3(-0.25, 0, 0), Vector3(0, 0, 0.25), Vector3(0, 0, -0.25)]:
					if (catcher.measure_point(p + offset, Vector3.UP) > 0.02) != (measured > 0.02):
						on_edge = true
				if on_edge:
					continue
				total += 1
				if (measured > 0.02) == (pixel >= 0.3):
					agree += 1
				else:
					disagreements.append("%s m=%.3f px=%.3f" % [p, measured, pixel])
		check("%s cookie: lit and dark agree with the screen" % light.get_class(), total > 50 and agree == total, "%d of %d %s" % [agree, total, disagreements.slice(0, 4)])
		light.queue_free()
		await physics()
	catcher.occlusion = LightCatcher3D.OCCLUSION_ALWAYS
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--rendering-driver", default="", help="Run with a window on this driver (vulkan, d3d12) to also check shadow casters and cookies against the screen; headless when empty")
    parser.add_argument("--temp-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="asosdot-light-catcher-", dir=args.temp_dir) as directory:
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
            f'config_version=5\n[application]\nconfig/name="Light catcher check {work.name}"\nrun/main_scene="res://main.tscn"\n',
            encoding="utf-8",
        )
        command = [str(isolated), "--path", str(project)]
        command += ["--rendering-driver", args.rendering_driver] if args.rendering_driver else ["--headless"]
        run(command, "LIGHT_CATCHER fails=0", work)
        log = (work / "LIGHT_CATCHER fails=0.log").read_text(encoding="utf-8", errors="replace")
        print("\n".join(line for line in log.splitlines() if line.startswith(("PASS", "FAIL", "SKIP"))), flush=True)
        print("LIGHT_CATCHER_TEST_OK", flush=True)


if __name__ == "__main__":
    main()
