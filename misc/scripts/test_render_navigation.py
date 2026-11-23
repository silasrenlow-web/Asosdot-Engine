#!/usr/bin/env python3
"""Check that the Render workspace navigates and lights the scene like the 3D view.

The studio camera must follow the 3D view's navigation (orbit, pan, zoom and freelook
with the editor's settings), and Scene Lighting must show the scene as the 3D view does,
including when View Environment is off.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

FIXTURE = r"""
extends SceneTree
func _initialize():
	call_deferred("run")
func run():
	var scene = Node3D.new()
	scene.name = "NavigationFixture"
	root.add_child(scene)
	var ball = MeshInstance3D.new()
	ball.name = "Ball"
	ball.mesh = SphereMesh.new()
	var material = StandardMaterial3D.new()
	material.albedo_color = Color(0.7, 0.7, 0.7)
	material.roughness = 0.8
	ball.material_override = material
	scene.add_child(ball)
	ball.owner = scene
	var packed = PackedScene.new()
	assert(packed.pack(scene) == OK)
	assert(ResourceSaver.save(packed, "res://fixture.tscn") == OK)
	print("NAVIGATION_FIXTURE_OK")
	quit()
"""

CHECK = r"""
@tool
extends EditorPlugin

func _enter_tree():
	call_deferred("run")

func find_class(node: Node, type: String) -> Node:
	if node.get_class() == type:
		return node
	for child in node.get_children():
		var found = find_class(child, type)
		if found:
			return found
	return null

func shot_state(render: Node) -> Dictionary:
	var state = render.get_state()
	return state.shots[state.selected_shot]

func mouse_button(surface: Control, index: MouseButton, pressed: bool):
	var event = InputEventMouseButton.new()
	event.button_index = index
	event.pressed = pressed
	event.position = Vector2(100, 100)
	event.global_position = surface.get_global_rect().position + event.position
	surface.gui_input.emit(event)
	if not pressed:
		# A real mouse keeps moving after a button is let go, which ends the gesture.
		mouse_motion(surface, 0, Vector2(1, 0))

func mouse_motion(surface: Control, mask: int, relative: Vector2):
	var event = InputEventMouseMotion.new()
	event.button_mask = mask
	event.relative = relative
	event.position = Vector2(100, 100)
	event.global_position = surface.get_global_rect().position + event.position
	surface.gui_input.emit(event)

# The workspace lets go of freelook when Input says the right button is up, so the test
# tells Input as well, away from every control.
func right_button_in_input(pressed: bool):
	# Input applies it one frame later.
	var event = InputEventMouseButton.new()
	event.button_index = MOUSE_BUTTON_RIGHT
	event.pressed = pressed
	event.position = Vector2(-50, -50)
	event.global_position = Vector2(-50, -50)
	Input.parse_input_event(event)
	await frames(2)

func key(code: Key, pressed: bool):
	var event = InputEventKey.new()
	event.physical_keycode = code
	event.keycode = code
	event.pressed = pressed
	Input.parse_input_event(event)

func frames(count: int):
	for i in count:
		await get_tree().process_frame

func patch_color(image: Image) -> Color:
	# Above the middle, where the 3D view's grid does not cross the ball.
	var size = image.get_size()
	var center = Vector2i(size.x / 2, int(size.y * 0.42))
	var total = Color(0, 0, 0)
	var count = 0
	for y in range(center.y - 4, center.y + 5):
		for x in range(center.x - 4, center.x + 5):
			total += image.get_pixel(x, y)
			count += 1
	return total / count

func luminance(color: Color) -> float:
	return color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722

func look_through_3d(render: Node, view: SubViewport, xform: Transform3D):
	view.get_camera_3d().global_transform = xform
	await frames(8)

func run():
	while not EditorInterface.get_edited_scene_root():
		await get_tree().process_frame
	await frames(12)
	EditorInterface.set_main_screen_editor("Render")
	await frames(2)
	var render = find_class(get_tree().root, "AsosdotRenderEditor")
	assert(render != null)
	var preview = render.find_child("RenderPreview", true, false)
	var holder = preview.get_parent().get_parent()
	var surface = holder.get_child(holder.get_child_count() - 1) as Control
	var camera = preview.get_node("RenderCamera")
	var options = JSON.parse_string(FileAccess.get_file_as_string("res://check.json"))
	var headless = DisplayServer.get_name() == "headless"

	# Orbit with the middle mouse button.
	var before = shot_state(render)
	mouse_button(surface, MOUSE_BUTTON_MIDDLE, true)
	mouse_motion(surface, MOUSE_BUTTON_MASK_MIDDLE, Vector2(40, 0))
	mouse_motion(surface, MOUSE_BUTTON_MASK_MIDDLE, Vector2(0, 24))
	mouse_button(surface, MOUSE_BUTTON_MIDDLE, false)
	var after = shot_state(render)
	assert(after.orbit_yaw < before.orbit_yaw, "Dragging right must turn the camera like the 3D view")
	assert(after.orbit_pitch < before.orbit_pitch, "Dragging down must tilt the camera like the 3D view")
	assert(after.orbit_target.is_equal_approx(before.orbit_target) and is_equal_approx(after.orbit_distance, before.orbit_distance))

	# The left button picks, it does not orbit.
	before = after
	mouse_button(surface, MOUSE_BUTTON_LEFT, true)
	mouse_motion(surface, MOUSE_BUTTON_MASK_LEFT, Vector2(40, 10))
	mouse_button(surface, MOUSE_BUTTON_LEFT, false)
	after = shot_state(render)
	assert(is_equal_approx(after.orbit_yaw, before.orbit_yaw) and is_equal_approx(after.orbit_pitch, before.orbit_pitch), "The left button must not orbit")

	# Shift pans.
	before = after
	key(KEY_SHIFT, true)
	# Keys parsed into Input are only seen once a frame has flushed them.
	await frames(2)
	mouse_button(surface, MOUSE_BUTTON_MIDDLE, true)
	mouse_motion(surface, MOUSE_BUTTON_MASK_MIDDLE, Vector2(30, 0))
	mouse_button(surface, MOUSE_BUTTON_MIDDLE, false)
	key(KEY_SHIFT, false)
	await frames(2)
	after = shot_state(render)
	assert(not after.orbit_target.is_equal_approx(before.orbit_target), "Shift and the middle button must pan")
	assert(is_equal_approx(after.orbit_yaw, before.orbit_yaw) and is_equal_approx(after.orbit_distance, before.orbit_distance))

	# The wheel zooms.
	before = after
	mouse_button(surface, MOUSE_BUTTON_WHEEL_UP, true)
	after = shot_state(render)
	assert(after.orbit_distance < before.orbit_distance, "The wheel must zoom in")
	mouse_button(surface, MOUSE_BUTTON_WHEEL_DOWN, true)
	mouse_button(surface, MOUSE_BUTTON_WHEEL_DOWN, true)
	assert(shot_state(render).orbit_distance > after.orbit_distance, "The wheel must zoom out")

	# Right mouse button held: freelook with W.
	before = shot_state(render)
	var start = camera.global_position
	var forward = -camera.global_transform.basis.z
	await right_button_in_input(true)
	mouse_button(surface, MOUSE_BUTTON_RIGHT, true)
	if not headless:
		assert(Input.mouse_mode == Input.MOUSE_MODE_CAPTURED, "Freelook must capture the mouse")
	key(KEY_W, true)
	await frames(20)
	key(KEY_W, false)
	var moved = camera.global_position - start
	assert(moved.dot(forward) > 0.0005, "W must fly the camera forward: %s" % moved)
	assert(is_equal_approx(shot_state(render).orbit_yaw, before.orbit_yaw) and is_equal_approx(shot_state(render).orbit_pitch, before.orbit_pitch))
	await right_button_in_input(false)
	mouse_button(surface, MOUSE_BUTTON_RIGHT, false)
	if not headless:
		assert(Input.mouse_mode == Input.MOUSE_MODE_VISIBLE, "Letting go must give the mouse back")
	# Q and E are not pose tools while flying, and the camera looks around.
	var yaw = shot_state(render).orbit_yaw
	await right_button_in_input(true)
	mouse_button(surface, MOUSE_BUTTON_RIGHT, true)
	mouse_motion(surface, MOUSE_BUTTON_MASK_RIGHT, Vector2(30, 0))
	assert(shot_state(render).orbit_yaw != yaw, "Moving the mouse while flying must look around")
	# Escape ends the flight.
	var escape = InputEventKey.new()
	escape.keycode = KEY_ESCAPE
	escape.pressed = true
	surface.gui_input.emit(escape)
	if not headless:
		assert(Input.mouse_mode == Input.MOUSE_MODE_VISIBLE, "Escape must end the flight")
	await right_button_in_input(false)
	mouse_button(surface, MOUSE_BUTTON_RIGHT, false)
	await frames(2)
	print("NAVIGATION_STATE_OK")
	if not options.graphics:
		get_tree().quit()
		return

	# Lighting. Both views look at the ball from the same place with the same field of view.
	var state = render.get_state()
	state.shots[0].shot.studio = 0
	state.shots[0].shot.fov = 70.0
	state.shots[0].shot.camera = NodePath("")
	state.shots[0].orbit_target = Vector3.ZERO
	state.shots[0].orbit_yaw = 0.0
	state.shots[0].orbit_pitch = -0.35
	state.shots[0].orbit_distance = 3.0
	state.shots[0].framed = true
	render.set_state(state)
	var view = EditorInterface.get_editor_viewport_3d(0)
	var results = {}
	for environment_on in [true, false]:
		EditorInterface.set_main_screen_editor("3D")
		await frames(4)
		# What the View Environment menu item does: no environment of its own while on,
		# the editor's plain one while off.
		view.get_camera_3d().environment = null if environment_on else Environment.new()
		EditorInterface.set_main_screen_editor("Render")
		await frames(10)
		var render_image = preview.get_texture().get_image()
		EditorInterface.set_main_screen_editor("3D")
		await frames(4)
		await look_through_3d(render, view, camera.global_transform)
		await frames(10)
		var view_image = view.get_texture().get_image()
		var a = patch_color(render_image)
		var b = patch_color(view_image)
		results[environment_on] = [luminance(a), luminance(b)]
		print("LIGHT environment=%s render=%s 3d=%s" % [environment_on, a, b])
		assert(absf(luminance(a) - luminance(b)) <= 0.03 * maxf(luminance(b), 0.05), "Scene Lighting differs from the 3D view (view environment %s): %s against %s" % [environment_on, a, b])
	# The check must be able to see a difference: a studio light changes the picture.
	state = render.get_state()
	state.shots[0].shot.studio = 1
	render.set_state(state)
	EditorInterface.set_main_screen_editor("Render")
	await frames(10)
	var studio = luminance(patch_color(preview.get_texture().get_image()))
	assert(absf(studio - results[false][1]) > 0.03 * maxf(results[false][1], 0.05) or absf(studio - results[true][1]) > 0.03 * maxf(results[true][1], 0.05), "The lighting check cannot tell a studio light from the 3D view")
	view.get_camera_3d().environment = null
	print("NAVIGATION_LIGHT_OK")
	get_tree().quit()
"""


def run(command: list[str], marker: str, cwd: Path) -> None:
    """Run the editor and require the marker and no script error.

    The exit status is not checked: the editor reports errors for objects freed after the
    rendering server at quit, which says nothing about this check.
    """
    log_path = cwd / f"{marker}.log"
    environment = os.environ.copy()
    temporary = cwd / "temp"
    temporary.mkdir(exist_ok=True)
    environment.update(TMP=str(temporary), TEMP=str(temporary))
    with log_path.open("w", encoding="utf-8") as log:
        try:
            subprocess.run(command, cwd=cwd, env=environment, stdout=log, stderr=subprocess.STDOUT, timeout=180)
        except subprocess.TimeoutExpired as error:
            output = log_path.read_text(encoding="utf-8", errors="replace")
            raise RuntimeError(f"Timed out: {command[0]}\n{output}") from error
    output = log_path.read_text(encoding="utf-8", errors="replace")
    if marker not in output or "SCRIPT ERROR" in output:
        raise RuntimeError(f"Command failed: {command[0]}\n{output}")
    for line in output.splitlines():
        if line.startswith("LIGHT"):
            print(line, flush=True)
    print(marker, flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--temp-dir", type=Path)
    parser.add_argument("--state-only", action="store_true")
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()
    parent = (args.temp_dir or Path(tempfile.gettempdir())).resolve()
    work = Path(tempfile.mkdtemp(prefix="asosdot-navigation-check-", dir=parent))
    print(f"Test workspace: {work}", flush=True)
    try:
        runtime = work / "runtime"
        runtime.mkdir()
        isolated = runtime / args.engine.name
        shutil.copy2(args.engine.resolve(), isolated)
        (runtime / "_sc_").touch()
        project = work / "project"
        project.mkdir()
        addon = project / "addons/check"
        addon.mkdir(parents=True)
        (addon / "plugin.cfg").write_text('[plugin]\nname="Navigation check"\nscript="plugin.gd"\n', encoding="utf-8")
        (addon / "plugin.gd").write_text(CHECK, encoding="utf-8")
        (project / "fixture.gd").write_text(FIXTURE, encoding="utf-8")
        (project / "project.godot").write_text(
            'config_version=5\n[application]\nconfig/name="Navigation check"\n'
            "[debug]\nfile_logging/enable_file_logging=false\n"
            '[rendering]\nrenderer/rendering_method="gl_compatibility"\n'
            '[editor_plugins]\nenabled=PackedStringArray("res://addons/check/plugin.cfg")\n',
            encoding="utf-8",
        )
        options = {"graphics": False}
        (project / "check.json").write_text(json.dumps(options), encoding="utf-8")
        base = [str(isolated), "--headless", "--path", str(project)]
        run(base + ["--script", "fixture.gd", "--quit-after", "120"], "NAVIGATION_FIXTURE_OK", work)
        run(base + ["--editor", "res://fixture.tscn"], "NAVIGATION_STATE_OK", work)
        if not args.state_only:
            options["graphics"] = True
            (project / "check.json").write_text(json.dumps(options), encoding="utf-8")
            run(
                [
                    str(isolated),
                    "--path",
                    str(project),
                    "--editor",
                    "res://fixture.tscn",
                    "--rendering-driver",
                    "opengl3",
                    "--audio-driver",
                    "Dummy",
                    "--resolution",
                    "1440x900",
                    "--position",
                    "80,80",
                    "--single-window",
                ],
                "NAVIGATION_LIGHT_OK",
                work,
            )
    finally:
        if not args.keep:
            # Only remove the specific directory created for this check.
            assert work.resolve().parent == parent and work.name.startswith("asosdot-navigation-check-")
            shutil.rmtree(work)


if __name__ == "__main__":
    main()
