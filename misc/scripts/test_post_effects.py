#!/usr/bin/env python3
"""Check the Environment's post effects (vignette, film grain, chromatic aberration) in Forward+."""

from __future__ import annotations

import argparse
import shutil
import tempfile
from pathlib import Path

from PIL import Image

from test_blender_roundtrip import run

# Each scene is captured before and after one effect is turned on, so the
# effect is the only thing that can change the picture.
CHECK = r"""
extends SceneTree

var out_dir := ""
var scene: Node3D
var environment := Environment.new()

func _initialize():
	call_deferred("run")

func plane(color: Color, at: Vector3, size: float) -> MeshInstance3D:
	var quad = MeshInstance3D.new()
	var mesh = PlaneMesh.new()
	mesh.size = Vector2(size, size)
	quad.mesh = mesh
	var material = StandardMaterial3D.new()
	material.albedo_color = color
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	quad.material_override = material
	quad.position = at
	quad.rotation_degrees = Vector3(90, 0, 0)
	return quad

func stage(planes: Array) -> void:
	if scene:
		scene.queue_free()
		await process_frame
	scene = Node3D.new()
	root.add_child(scene)
	var camera = Camera3D.new()
	camera.position = Vector3(0, 0, 5)
	camera.fov = 60
	scene.add_child(camera)
	camera.current = true
	var world = WorldEnvironment.new()
	world.environment = environment
	scene.add_child(world)
	for quad in planes:
		scene.add_child(quad)
	await process_frame
	await process_frame

func capture(name: String) -> void:
	await RenderingServer.frame_post_draw
	root.get_texture().get_image().save_png(out_dir.path_join(name + ".png"))

func run():
	out_dir = OS.get_cmdline_user_args()[0]
	environment.background_mode = Environment.BG_COLOR
	await stage([plane(Color.WHITE, Vector3.ZERO, 30.0)])
	await capture("white")
	environment.post_vignette_intensity = 1.0
	environment.post_vignette_softness = 0.5
	await process_frame
	await capture("vignette")
	environment.post_vignette_intensity = 0.0
	await stage([plane(Color(0.5, 0.5, 0.5), Vector3.ZERO, 30.0)])
	await capture("grey")
	environment.post_film_grain = 1.0
	await process_frame
	await capture("grain_a")
	await process_frame
	await capture("grain_b")
	environment.post_film_grain = 0.0
	# A black and white edge, right of centre, so the fringing is visible.
	await stage([plane(Color.BLACK, Vector3(-8.5, 0, 0), 20.0), plane(Color.WHITE, Vector3(11.5, 0, 0), 20.0)])
	await capture("edge")
	environment.post_chromatic_aberration = 1.0
	await process_frame
	await capture("edge_fringed")
	print("POST_EFFECTS_RENDER_OK")
	quit()
"""


def mean_brightness(image: Image.Image, box: tuple[int, int, int, int]) -> float:
    pixels = list(image.crop(box).convert("RGB").getdata())
    return sum(sum(pixel) / 3 for pixel in pixels) / (len(pixels) * 255)


def spread(image: Image.Image, box: tuple[int, int, int, int]) -> float:
    values = [sum(pixel) / 3 for pixel in image.crop(box).convert("RGB").getdata()]
    mean = sum(values) / len(values)
    return (sum((value - mean) ** 2 for value in values) / len(values)) ** 0.5 / 255


def largest_red_blue_gap(image: Image.Image) -> float:
    return max(abs(red - blue) for red, _, blue in image.convert("RGB").getdata()) / 255


def mean_absolute_difference(first: Image.Image, second: Image.Image, box: tuple[int, int, int, int]) -> float:
    a = list(first.crop(box).convert("RGB").getdata())
    b = list(second.crop(box).convert("RGB").getdata())
    return sum(abs(sum(p) - sum(q)) / 3 for p, q in zip(a, b)) / (len(a) * 255)


def check_outputs(frames: Path) -> None:
    images = {name: Image.open(frames / f"{name}.png") for name in ("white", "vignette", "grey", "grain_a", "grain_b", "edge", "edge_fringed")}
    width, height = images["white"].size
    centre = (width // 2 - 40, height // 2 - 40, width // 2 + 40, height // 2 + 40)
    corners = [(0, 0, 40, 40), (width - 40, 0, width, 40), (0, height - 40, 40, height), (width - 40, height - 40, width, height)]

    # Vignette: the centre keeps its brightness, and the corners go dark.
    assert abs(mean_brightness(images["vignette"], centre) - mean_brightness(images["white"], centre)) < 0.02, "Vignette darkened the centre"
    for corner in corners:
        assert mean_brightness(images["vignette"], corner) < 0.5 * mean_brightness(images["white"], corner), "Vignette did not darken a corner"

    # Film grain: a flat grey is smooth without it, grainy with it, and the grain moves between frames.
    assert spread(images["grey"], centre) < 0.01, "A flat grey was already noisy"
    assert spread(images["grain_a"], centre) > 0.01, "Film grain did not add noise"
    assert mean_absolute_difference(images["grain_a"], images["grain_b"], centre) > 0.005, "Film grain did not change between frames"

    # Chromatic aberration: black and white has no colour, so any red and blue gap is fringing.
    assert largest_red_blue_gap(images["edge"]) < 0.03, "A black and white edge already had colour"
    assert largest_red_blue_gap(images["edge_fringed"]) > 0.1, "Chromatic aberration did not split the channels"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--temp-dir", type=Path)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()
    parent = (args.temp_dir or Path(tempfile.gettempdir())).resolve()
    work = Path(tempfile.mkdtemp(prefix="asosdot-post-effects-", dir=parent))
    print(f"Test workspace: {work}", flush=True)
    try:
        runtime = work / "runtime"
        runtime.mkdir()
        isolated = runtime / args.engine.name
        shutil.copy2(args.engine.resolve(), isolated)
        (runtime / "_sc_").touch()
        project = work / "project"
        project.mkdir()
        (project / "check.gd").write_text(CHECK, encoding="utf-8")
        (project / "project.godot").write_text(
            'config_version=5\n[application]\nconfig/name="Post effects check"\n'
            "[debug]\nfile_logging/enable_file_logging=false\n"
            '[rendering]\nrenderer/rendering_method="forward_plus"\n',
            encoding="utf-8",
        )
        frames = work / "frames"
        frames.mkdir()
        run(
            [
                str(isolated),
                "--path",
                str(project),
                "--rendering-driver",
                "vulkan",
                "--audio-driver",
                "Dummy",
                "--resolution",
                "640x480",
                "--position",
                "80,80",
                "--single-window",
                "--script",
                "check.gd",
                "--",
                str(frames),
            ],
            "POST_EFFECTS_RENDER_OK",
            work,
        )
        check_outputs(frames)
        print("POST_EFFECTS_OK", flush=True)
    finally:
        if not args.keep:
            # Only remove the specific directory created for this check.
            assert work.resolve().parent == parent and work.name.startswith("asosdot-post-effects-")
            shutil.rmtree(work)


if __name__ == "__main__":
    main()
