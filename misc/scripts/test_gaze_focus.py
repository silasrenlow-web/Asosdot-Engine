"""Check gaze focus (CameraAttributes.gaze_focus_*): around the gaze point stays
sharp and the rest blurs; the blur follows the gaze point set on the viewport and
the one EyeTracker sets; with a depth range, what is nearer or further than the
looked-at thing blurs too, and the focus moves to a new distance over a moment;
nothing blurs when it is off.
Renders, so it needs a GPU: run it with the editor binary, not --headless. Forward+
only.

    python misc/scripts/test_gaze_focus.py --engine bin/asosdot.windows.editor.x86_64.console.exe --rendering-driver d3d12
"""

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
var near_wall: MeshInstance3D
var far_wall: MeshInstance3D

const LEFT := Rect2i(20, 120, 160, 120)
const RIGHT := Rect2i(460, 120, 160, 120)
const MIDDLE := Rect2i(240, 120, 160, 120)


func check(p_name: String, p_ok: bool, p_detail: Variant = "") -> void:
	print(("PASS " if p_ok else "FAIL ") + p_name + "  " + str(p_detail))
	if not p_ok:
		fails += 1


# Squared differences between neighbours: falls when edges soften.
func sharpness(p_image: Image, p_region: Rect2i) -> float:
	var total := 0.0
	for y in range(p_region.position.y, p_region.end.y - 1):
		for x in range(p_region.position.x, p_region.end.x - 1):
			var c := p_image.get_pixel(x, y).get_luminance()
			var dx := p_image.get_pixel(x + 1, y).get_luminance() - c
			var dy := p_image.get_pixel(x, y + 1).get_luminance() - c
			total += dx * dx + dy * dy
	return total


func frame(p_frames: int = 3) -> Image:
	for i in p_frames:
		await RenderingServer.frame_post_draw
	return get_viewport().get_texture().get_image()


func set_gaze(p_uv: Vector2) -> void:
	RenderingServer.viewport_set_gaze_point(get_viewport().get_viewport_rid(), p_uv)


func checker() -> ImageTexture:
	var image := Image.create_empty(2, 2, false, Image.FORMAT_RGBA8)
	image.set_pixel(0, 0, Color.WHITE)
	image.set_pixel(1, 1, Color.WHITE)
	image.set_pixel(1, 0, Color.BLACK)
	image.set_pixel(0, 1, Color.BLACK)
	return ImageTexture.create_from_image(image)


func wall(p_size: Vector2, p_position: Vector3, p_checks: float) -> MeshInstance3D:
	var mesh := QuadMesh.new()
	mesh.size = p_size
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.albedo_texture = checker()
	material.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	material.uv1_scale = Vector3(p_size.x / p_checks, p_size.y / p_checks, 1)
	mesh.material = material
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.position = p_position
	add_child(instance)
	return instance


func _ready() -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.5, 0.5, 0.5)
	var world_env := WorldEnvironment.new()
	world_env.environment = env
	add_child(world_env)

	attributes = CameraAttributesPractical.new()
	camera = Camera3D.new()
	camera.attributes = attributes
	add_child(camera)
	camera.make_current()

	# One wall across the whole view, checks a few pixels wide.
	near_wall = wall(Vector2(40, 20), Vector3(0, 0, -5), 0.08)
	run.call_deferred()


func run() -> void:
	await get_tree().process_frame
	set_gaze(Vector2(0.5, 0.5))

	var off := await frame()
	var off_left := sharpness(off, LEFT)
	var off_right := sharpness(off, RIGHT)
	check("off: both sides equally sharp", absf(off_left - off_right) < 0.15 * off_left and off_left > 100.0, [off_left, off_right])

	attributes.gaze_focus_enabled = true
	attributes.gaze_focus_radius = 0.2
	attributes.gaze_focus_softness = 0.2
	attributes.gaze_focus_amount = 1.0
	attributes.gaze_focus_depth_range = 0.0

	set_gaze(Vector2(0.15, 0.5))
	var at_left := await frame()
	var l1 := sharpness(at_left, LEFT)
	var r1 := sharpness(at_left, RIGHT)
	check("gaze left: left stays sharp", l1 > 0.85 * off_left, [l1, off_left])
	check("gaze left: right is blurred", r1 < 0.25 * off_right, [r1, off_right])

	set_gaze(Vector2(0.85, 0.5))
	var at_right := await frame()
	var l2 := sharpness(at_right, LEFT)
	var r2 := sharpness(at_right, RIGHT)
	check("gaze right: right stays sharp", r2 > 0.85 * off_right, [r2, off_right])
	check("gaze right: left is blurred", l2 < 0.25 * off_left, [l2, off_left])

	attributes.gaze_focus_amount = 0.4
	var half := await frame()
	var l3 := sharpness(half, LEFT)
	check("less amount: blurred less", l3 > l2 * 1.3 and l3 < 0.9 * off_left, [l3, l2, off_left])
	attributes.gaze_focus_amount = 0.0
	var none := await frame()
	check("amount 0: nothing blurred", sharpness(none, LEFT) > 0.95 * off_left, sharpness(none, LEFT))
	attributes.gaze_focus_amount = 1.0

	# EyeTracker sets the main viewport's gaze point every frame while it runs.
	set_gaze(Vector2(0.15, 0.5))
	EyeTracker.start(EyeTracker.SOURCE_SCREEN_CENTER)
	var tracked := await frame()
	var lt := sharpness(tracked, LEFT)
	var mt := sharpness(tracked, MIDDLE)
	check("EyeTracker: the middle is sharp", mt > 0.85 * sharpness(off, MIDDLE), [mt, sharpness(off, MIDDLE)])
	check("EyeTracker: the sides are blurred", lt < 0.25 * off_left, [lt, off_left])
	EyeTracker.stop()

	# --- Depth: a near wall on the left, a far one behind it ----------------------
	near_wall.queue_free()
	# At 3 m the left measuring region spans about x -3.9 to -1.8; the wall covers it.
	near_wall = wall(Vector2(3.4, 6), Vector3(-2.6, 0, -3), 0.05)
	far_wall = wall(Vector2(80, 40), Vector3(0, 0, -20), 0.5)
	attributes.gaze_focus_radius = 2.0 # The screen term out of the way.
	attributes.gaze_focus_depth_range = 2.0
	attributes.gaze_focus_adapt_speed = 0.0 # Refocus at once.
	set_gaze(Vector2(0.15, 0.5)) # On the near wall.
	await frame()
	attributes.gaze_focus_enabled = false
	var depth_off := await frame()
	var near_off := sharpness(depth_off, LEFT)
	var far_off := sharpness(depth_off, RIGHT)
	attributes.gaze_focus_enabled = true
	var focus_near := await frame()
	check("depth: looking at the near wall keeps it sharp", sharpness(focus_near, LEFT) > 0.85 * near_off, [sharpness(focus_near, LEFT), near_off])
	check("depth: the far wall is out of focus", sharpness(focus_near, RIGHT) < 0.4 * far_off, [sharpness(focus_near, RIGHT), far_off])

	set_gaze(Vector2(0.85, 0.5)) # On the far wall.
	var focus_far := await frame()
	check("depth: looking at the far wall makes it sharp", sharpness(focus_far, RIGHT) > 0.85 * far_off, [sharpness(focus_far, RIGHT), far_off])
	check("depth: the near wall goes out of focus", sharpness(focus_far, LEFT) < 0.4 * near_off, [sharpness(focus_far, LEFT), near_off])

	# Refocusing takes a moment with a slow adapt speed.
	attributes.gaze_focus_adapt_speed = 2.0
	set_gaze(Vector2(0.15, 0.5))
	var refocusing := await frame(2)
	check("slow refocus: the near wall is still soft at first", sharpness(refocusing, LEFT) < 0.6 * near_off, [sharpness(refocusing, LEFT), near_off])
	await get_tree().create_timer(4.0).timeout
	var refocused := await frame()
	check("slow refocus: sharp after a few seconds", sharpness(refocused, LEFT) > 0.85 * near_off, [sharpness(refocused, LEFT), near_off])

	print("GAZE_FOCUS fails=%d" % fails)
	get_tree().quit(1 if fails else 0)
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--rendering-driver", default="", help="vulkan or d3d12; the project default when empty")
    parser.add_argument("--temp-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="asosdot-gaze-focus-", dir=args.temp_dir) as directory:
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
            f'config_version=5\n[application]\nconfig/name="Gaze focus {work.name}"\nrun/main_scene="res://main.tscn"\n'
            "[display]\nwindow/size/viewport_width=640\nwindow/size/viewport_height=360\n",
            encoding="utf-8",
        )
        command = [str(isolated), "--path", str(project), "--rendering-method", "forward_plus"]
        if args.rendering_driver:
            command += ["--rendering-driver", args.rendering_driver]
        try:
            run(command, "GAZE_FOCUS fails=0", work)
        finally:
            for log in work.glob("*.log"):
                print(
                    "\n".join(
                        line
                        for line in log.read_text(encoding="utf-8", errors="replace").splitlines()
                        if line.startswith(("PASS", "FAIL"))
                    ),
                    flush=True,
                )
        print("GAZE_FOCUS_TEST_OK", flush=True)


if __name__ == "__main__":
    main()
