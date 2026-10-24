#!/usr/bin/env python3
"""Export a project using Asosdot's nodes with Asosdot's Windows templates, and check the exported game runs them."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

from make_export_templates import TEMPLATES, templates_version
from test_blender_roundtrip import run

# Uses what only Asosdot has: its nodes, layered sounds and camera cuts.
GAME = r"""
extends Node

func _ready() -> void:
	var failures := []
	for name in ["BlenderConstraint3D", "Rope3D", "LensFlare3D", "VoxelTerrain3D"]:
		if not ClassDB.class_exists(name):
			failures.append("no class " + name)
	var rope := get_node_or_null("Rope3D")
	if rope == null or rope.get_class() != "Rope3D":
		failures.append("the saved Rope3D didn't load as one")
	var player := AudioStreamPlayer.new()
	add_child(player)
	if not player.has_method("play_layered"):
		failures.append("AudioStreamPlayer has no play_layered()")
	var animation := Animation.new()
	var track := animation.add_track(Animation.TYPE_AUDIO)
	animation.audio_track_insert_key(track, 0.0, AudioStreamWAV.new())
	if not animation.has_method("audio_track_set_key_volume_db"):
		failures.append("Animation has no audio key volume")
	print("ENGINE ", Engine.get_version_info().get("string", ""))
	if failures.is_empty():
		print("EXPORTED_GAME_OK")
	else:
		print("EXPORTED_GAME_FAILED ", failures)
	get_tree().quit()
"""

SCENE = """[gd_scene load_steps=2 format=3]

[ext_resource type="Script" path="res://game.gd" id="1"]

[node name="Game" type="Node"]
script = ExtResource("1")

[node name="Rope3D" type="Rope3D" parent="."]
"""

PRESET = """[preset.0]

name="Windows Desktop"
platform="Windows Desktop"
runnable=true
export_filter="all_resources"
export_path=""

[preset.0.options]

binary_format/embed_pck=true
binary_format/architecture="x86_64"
codesign/enable=false
application/modify_resources=false
"""


def export_and_run(engine: Path, templates: dict[str, Path], work: Path, label: str) -> str:
    """Exports the project with the given templates and returns what the exported game printed."""
    runtime = work / f"runtime_{label}"
    runtime.mkdir()
    editor = runtime / engine.name
    shutil.copy2(engine, editor)
    (runtime / "_sc_").touch()
    folder = runtime / "editor_data" / "export_templates" / templates_version()
    folder.mkdir(parents=True)
    for name, source in templates.items():
        shutil.copy2(source, folder / name)

    project = work / "project"
    out = work / f"export_{label}"
    out.mkdir()
    game = out / "game.exe"
    run([str(editor), "--headless", "--path", str(project), "--export-release", "Windows Desktop", str(game)], None, work)
    if not game.is_file():
        raise RuntimeError(f"Exporting with the {label} templates made no game")
    result = subprocess.run([str(game), "--headless"], cwd=out, capture_output=True, text=True, errors="replace", timeout=120)
    return result.stdout + result.stderr


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path, help="The Asosdot editor")
    parser.add_argument("--bin", type=Path, default=Path(__file__).resolve().parents[2] / "bin", help="Where the Asosdot templates were built")
    parser.add_argument("--godot-templates", type=Path, help="A folder of Godot's own templates, to show what they'd export")
    parser.add_argument("--temp-dir", type=Path)
    args = parser.parse_args()
    built = {name: args.bin / source for source, name in TEMPLATES.items() if name.startswith("windows_") and "x86_64" in name and (args.bin / source).is_file()}
    if "windows_release_x86_64.exe" not in built:
        raise SystemExit("Build the Windows templates first: scons platform=windows target=template_release arch=x86_64 d3d12=yes")

    with tempfile.TemporaryDirectory(prefix="asosdot-export-", dir=args.temp_dir) as directory:
        work = Path(directory)
        project = work / "project"
        project.mkdir()
        (project / "project.godot").write_text(f'config_version=5\n[application]\nconfig/name="Export check {work.name}"\nrun/main_scene="res://game.tscn"\n', encoding="utf-8")
        (project / "game.gd").write_text(GAME, encoding="utf-8")
        (project / "game.tscn").write_text(SCENE, encoding="utf-8")
        (project / "export_presets.cfg").write_text(PRESET, encoding="utf-8")
        run([str(args.engine.resolve()), "--headless", "--path", str(project), "--editor", "--quit-after", "200"], None, work)

        output = export_and_run(args.engine.resolve(), built, work, "asosdot")
        print("\n".join(line for line in output.splitlines() if line.startswith(("ENGINE", "EXPORTED"))), flush=True)
        if "EXPORTED_GAME_OK" not in output:
            raise SystemExit("The game exported with Asosdot's templates didn't run Asosdot:\n" + output)

        if args.godot_templates:
            godot = {name: args.godot_templates / name for name in built if (args.godot_templates / name).is_file()}
            output = export_and_run(args.engine.resolve(), godot, work, "godot")
            print("With Godot's own templates:", flush=True)
            print("\n".join(line for line in output.splitlines() if line.startswith(("ENGINE", "EXPORTED")) or "Rope3D" in line)[:2000], flush=True)
        print("EXPORT_TEST_OK", flush=True)


if __name__ == "__main__":
    main()
