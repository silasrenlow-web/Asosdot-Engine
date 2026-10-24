#!/usr/bin/env python3
"""Check the editor's freeze recorder: a frozen editor gets a report naming where it was stuck."""

from __future__ import annotations

import argparse
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

# Freezes the editor's main thread on purpose, inside a function the report must name.
PLUGIN = r"""
@tool
extends EditorPlugin

func _enter_tree():
	call_deferred("run")

func run():
	await get_tree().create_timer(3.0).timeout
	print("USER_DATA ", OS.get_user_data_dir())
	hold_the_editor()
	await get_tree().create_timer(3.0).timeout
	print("FREEZE_TEST_OK")
	get_tree().quit()

func hold_the_editor():
	OS.delay_msec(13000)
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--temp-dir", type=Path)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()
    parent = (args.temp_dir or Path(tempfile.gettempdir())).resolve()
    work = Path(tempfile.mkdtemp(prefix="asosdot-freeze-", dir=parent))
    print(f"Test workspace: {work}", flush=True)
    user_data = None
    try:
        runtime = work / "runtime"
        runtime.mkdir()
        isolated = runtime / args.engine.name
        shutil.copy2(args.engine.resolve(), isolated)
        for pdb in args.engine.resolve().parent.glob(args.engine.stem + "*.pdb"):
            shutil.copy2(pdb, runtime / pdb.name)
        (runtime / "_sc_").touch()
        project = work / "project"
        (project / "addons/check").mkdir(parents=True)
        (project / "addons/check/plugin.cfg").write_text('[plugin]\nname="Freeze check"\nscript="plugin.gd"\n', encoding="utf-8")
        (project / "addons/check/plugin.gd").write_text(PLUGIN, encoding="utf-8")
        name = f"Freeze check {work.name}"
        (project / "project.godot").write_text(
            f'config_version=5\n[application]\nconfig/name="{name}"\n'
            '[rendering]\nrenderer/rendering_method="gl_compatibility"\n'
            '[editor_plugins]\nenabled=PackedStringArray("res://addons/check/plugin.cfg")\n',
            encoding="utf-8",
        )
        base = [str(isolated), "--path", str(project)]
        run(base + ["--headless", "--editor", "--quit-after", "200"], None, work)
        # The recorder only runs with a window, as a user's editor does.
        graphics = base + ["--editor", "--rendering-driver", "opengl3", "--audio-driver", "Dummy", "--resolution", "1280x800"]
        run(graphics, "FREEZE_TEST_OK", work)
        log = (work / "FREEZE_TEST_OK.log").read_text(encoding="utf-8", errors="replace")
        user_data = Path(next(line for line in log.splitlines() if line.startswith("USER_DATA ")).split(" ", 1)[1].strip())
        reports = sorted((user_data / "logs/freezes").glob("freeze-*.txt"))
        assert reports, "A freeze report must be written"
        report = reports[-1].read_text(encoding="utf-8", errors="replace")
        print(report, flush=True)
        assert "After 8 s:" in report, "The main thread must be sampled once the freeze passes 8 seconds"
        assert "responded again after" in report, "The report must say when the editor recovered"
        assert any(word in report for word in ("delay_usec", "SleepEx", "Sleep")), "The stack must show where it was stuck"
        assert "The editor stopped responding for a while" in log, "The editor must say where the report is"
        print("FREEZE_RECORDER_OK", flush=True)
    finally:
        if user_data and user_data.name.startswith("Freeze check asosdot-freeze-"):
            shutil.rmtree(user_data, ignore_errors=True)
        if not args.keep:
            # Only remove the specific directory created for this check.
            assert work.resolve().parent == parent and work.name.startswith("asosdot-freeze-")
            shutil.rmtree(work)


if __name__ == "__main__":
    main()
