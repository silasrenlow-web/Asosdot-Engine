#!/usr/bin/env python3
"""Check the Render sequencer with an isolated editor, scene and soundtrack."""

from __future__ import annotations

import argparse
import io
import json
import math
import shutil
import struct
import tempfile
import wave
from pathlib import Path

from test_blender_roundtrip import run

FIXTURE = r"""
extends SceneTree
func _initialize():
	call_deferred("run")
func run():
	var scene = Node3D.new()
	scene.name = "SequenceFixture"
	root.add_child(scene)
	var subject = MeshInstance3D.new()
	subject.name = "Subject"
	subject.mesh = BoxMesh.new()
	subject.mesh.size = Vector3(1, 1.5, 0.5)
	var material = StandardMaterial3D.new()
	material.albedo_color = Color(0.85, 0.15, 0.1)
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	subject.material_override = material
	scene.add_child(subject)
	subject.owner = scene
	for i in 2:
		var camera = Camera3D.new()
		camera.name = "CameraA" if i == 0 else "CameraB"
		scene.add_child(camera)
		camera.owner = scene
		camera.position = Vector3(0, 0, 4) if i == 0 else Vector3(3, 1, 4)
		camera.look_at(Vector3.ZERO)
		camera.fov = 30 if i == 0 else 60
	var player = AnimationPlayer.new()
	player.name = "AnimationPlayer"
	scene.add_child(player)
	player.owner = scene
	var library = AnimationLibrary.new()
	for i in 2:
		var clip = Animation.new()
		clip.length = 2.0
		var track = clip.add_track(Animation.TYPE_POSITION_3D)
		clip.track_set_path(track, NodePath("Subject"))
		clip.position_track_insert_key(track, 0, Vector3.ZERO)
		clip.position_track_insert_key(track, 2, Vector3(2, 0, 0) if i else Vector3(0, 2, 0))
		library.add_animation("Move" if i else "Rise", clip)
	player.add_animation_library("", library)
	var packed = PackedScene.new()
	assert(packed.pack(scene) == OK)
	assert(ResourceSaver.save(packed, "res://fixture.tscn") == OK)
	print("SEQUENCE_FIXTURE_OK")
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

func find_button(node: Node, text: String) -> Button:
	if node is Button and (node.text == text or node.name == text):
		return node
	for child in node.get_children():
		var found = find_button(child, text)
		if found:
			return found
	return null

func press_key(key: Key, shift := false, ctrl := false):
	var event = InputEventKey.new()
	event.keycode = key
	event.pressed = true
	event.shift_pressed = shift
	event.ctrl_pressed = ctrl
	get_tree().root.push_input(event)

func wait_render(render: Node):
	for i in 240:
		if not render.is_rendering():
			return
		await get_tree().process_frame
	assert(false, "Render did not finish")

func capture_ui(path: String):
	for i in 6:
		await get_tree().process_frame
	RenderingServer.force_draw(false)
	assert(EditorInterface.get_base_control().get_viewport().get_texture().get_image().save_png(path) == OK)

func check_layout(render: Node):
	var preview_panel = render.find_child("PreviewPanel", true, false) as Control
	var properties = render.find_child("PropertiesPanel", true, false) as Control
	var shots = render.find_child("ShotsPanel", true, false) as Control
	var sequence = render.find_child("SequencePanel", true, false) as Control
	assert(not preview_panel.get_global_rect().intersects(properties.get_global_rect()))
	assert(shots.get_global_rect().position.y >= preview_panel.get_global_rect().end.y - 1)
	assert(sequence.get_global_rect().position.y >= properties.get_global_rect().end.y - 1)
	assert(not shots.get_global_rect().intersects(sequence.get_global_rect()))
	var bounds = EditorInterface.get_editor_main_screen().get_global_rect()
	for panel in [preview_panel, properties, shots, sequence]:
		assert(bounds.encloses(panel.get_global_rect().grow(-1)), "Render panel exceeds the workspace")
	# The status line and sequence must be on screen, not pushed below the window.
	var window = Rect2(Vector2.ZERO, EditorInterface.get_base_control().get_viewport().get_visible_rect().size)
	var status = render.find_child("StatusBar", true, false) as Control
	for control in [sequence, status]:
		assert(window.encloses(control.get_global_rect().grow(-1)), "Render workspace runs off the window: %s" % control.name)

func run():
	while not EditorInterface.get_edited_scene_root():
		await get_tree().process_frame
	for i in 12:
		await get_tree().process_frame
	EditorInterface.set_main_screen_editor("Render")
	await get_tree().process_frame
	var scene = EditorInterface.get_edited_scene_root()
	var subject = scene.get_node("Subject")
	var render = find_class(get_tree().root, "AsosdotRenderEditor")
	assert(render != null)
	var preview = render.find_child("RenderPreview", true, false)
	var camera = preview.get_node("RenderCamera")
	var a = scene.get_node("CameraA")
	var b = scene.get_node("CameraB")
	var legacy = {"shot": {"camera": NodePath("CameraA"), "duration": 0.5, "fps": 4,
		"resolution": 8, "width": 128, "height": 96, "supersampling": 1,
		"studio": 0, "camera_motion": 0, "animation_player": NodePath("AnimationPlayer"), "animation": "Rise"},
		"orbit_target": Vector3(1, 2, 3), "orbit_yaw": 0.7, "orbit_distance": 6.0, "framed": true}
	render.set_state(legacy)
	var state = render.get_state()
	assert(state.shots.size() == 1 and state.output.fps == 4 and state.output.width == 128)
	assert(state.shots[0].orbit_target == Vector3(1, 2, 3))
	assert(camera.global_transform.is_equal_approx(a.global_transform))
	var inspected_shot = find_class(render, "EditorInspector").get_edited_object()
	get_undo_redo().create_action("Change shot name", UndoRedo.MERGE_DISABLE, scene, false, false)
	get_undo_redo().add_do_property(inspected_shot, "shot_name", "Renamed")
	get_undo_redo().add_undo_property(inspected_shot, "shot_name", "Shot 1")
	get_undo_redo().commit_action()
	# Duplicate through the actual UI, and exercise its scene undo history.
	find_button(render, "Duplicate").pressed.emit()
	assert(render.get_state().shots.size() == 2)
	var history = get_undo_redo().get_history_undo_redo(get_undo_redo().get_object_history_id(scene))
	assert(history.undo())
	assert(render.get_state().shots.size() == 1)
	assert(find_class(render, "EditorInspector").get_edited_object() == inspected_shot)
	assert(history.undo())
	assert(render.get_state().shots[0].shot.shot_name == "Shot 1", "Inspector undo lost its shot")
	assert(history.redo())
	assert(render.get_state().shots[0].shot.shot_name == "Renamed")
	assert(history.redo())
	assert(render.get_state().shots.size() == 2)
	state = render.get_state()
	state.shots[0].shot.shot_name = "Front"
	state.shots[1].shot.shot_name = "Side"
	state.shots[1].shot.camera = NodePath("CameraB")
	state.shots[1].shot.animation = "Move"
	state.shots[1].shot.animation_start = 0.25
	state.shots[1].shot.animation_speed = 2.0
	state.selected_shot = 0
	state.sequence_time = 0.0
	render.set_state(state)
	render.seek_sequence(0.25)
	assert(subject.position.is_equal_approx(Vector3(0, 0.25, 0)))
	render.seek_sequence(0.5)
	assert(render.get_state().selected_shot == 1, "A boundary must belong to the incoming shot")
	assert(camera.global_transform.is_equal_approx(b.global_transform))
	assert(subject.position.is_equal_approx(Vector3(0.25, 0, 0)))
	render.seek_sequence(0.75)
	assert(subject.position.is_equal_approx(Vector3(0.75, 0, 0)))
	EditorInterface.set_main_screen_editor("3D")
	assert(subject.position.is_equal_approx(Vector3.ZERO), "Preview leaked its pose")
	EditorInterface.set_main_screen_editor("Render")
	state.shots[1].shot.camera_blend = 0.25
	render.set_state(state)
	render.seek_sequence(0.625)
	var halfway = a.global_transform.interpolate_with(b.global_transform, 0.5)
	assert(camera.global_transform.is_equal_approx(halfway), "Direct scrubbing into a blend is incorrect")
	assert(is_equal_approx(camera.fov, 45.0))
	render.seek_sequence(0.1)
	render.seek_sequence(0.625)
	assert(camera.global_transform.is_equal_approx(halfway))
	b.projection = Camera3D.PROJECTION_ORTHOGONAL
	render.seek_sequence(0.625)
	assert(camera.global_transform.is_equal_approx(b.global_transform))
	b.projection = Camera3D.PROJECTION_PERSPECTIVE
	# Reorder and remove are reversible, and preserve settings and studio views.
	find_button(render, "Up").pressed.emit()
	assert(render.get_state().shots[0].shot.shot_name == "Side")
	assert(history.undo())
	assert(render.get_state().shots[1].shot.shot_name == "Side")
	find_button(render, "Remove").pressed.emit()
	assert(render.get_state().shots.size() == 1)
	assert(history.undo())
	assert(render.get_state().shots.size() == 2)
	# State contains serializable values, and no resource object references.
	# Frame controls use output FPS, and focus mode restores the editor docks.
	find_button(render, "SequenceStart").pressed.emit()
	find_button(render, "NextFrame").pressed.emit()
	assert(is_equal_approx(render.get_state().sequence_time, 0.25))
	# Keyboard transport: Left and Right step frames, Shift steps seconds, Home
	# and End go to the ends, [ and ] change shot, and Ctrl+Left/Right visit keys.
	var before_keys = render.get_state()
	var keyed = render.get_state()
	keyed.shots[1].shot.duration = 2.0
	keyed.shots[1].shot.animation = "Move"
	keyed.shots[1].shot.animation_start = 0.0
	keyed.shots[1].shot.animation_speed = 1.0
	keyed.selected_shot = 0
	keyed.sequence_time = 0.0
	render.set_state(keyed)
	press_key(KEY_RIGHT)
	assert(is_equal_approx(render.get_state().sequence_time, 0.25), "Right must step one frame")
	press_key(KEY_RIGHT, true)
	assert(is_equal_approx(render.get_state().sequence_time, 1.25), "Shift+Right must step one second")
	press_key(KEY_LEFT)
	assert(is_equal_approx(render.get_state().sequence_time, 1.0), "Left must step back one frame")
	press_key(KEY_END)
	assert(is_equal_approx(render.get_state().sequence_time, 2.25), "End must go to the last frame")
	press_key(KEY_HOME)
	assert(render.get_state().sequence_time == 0.0 and render.get_state().selected_shot == 0, "Home must go to the start")
	press_key(KEY_BRACKETRIGHT)
	assert(render.get_state().selected_shot == 1, "] must select the next shot")
	press_key(KEY_BRACKETLEFT)
	assert(render.get_state().selected_shot == 0, "[ must select the previous shot")
	press_key(KEY_BRACKETRIGHT)
	press_key(KEY_RIGHT, false, true)
	assert(is_equal_approx(render.get_state().sequence_time, 2.5), "Ctrl+Right must go to the next key")
	press_key(KEY_LEFT, false, true)
	assert(is_equal_approx(render.get_state().sequence_time, 0.5), "Ctrl+Left must go to the previous key")
	render.set_state(before_keys)
	find_button(render, "PreviousFrame").pressed.emit()
	assert(is_equal_approx(render.get_state().sequence_time, 0.0))
	find_button(render, "SequenceEnd").pressed.emit()
	assert(is_equal_approx(render.get_state().sequence_time, 0.75))
	render.find_child("PreviewSplit", true, false).split_offsets = PackedInt32Array([24])
	render.find_child("WorkspaceSplit", true, false).split_offsets = PackedInt32Array([-18])
	find_button(render, "MaximizePreview").button_pressed = true
	assert(not render.find_child("PropertiesPanel", true, false).visible)
	assert(not render.find_child("SequenceSplit", true, false).visible)
	var focused_state = render.get_state()
	render.set_state(focused_state)
	assert(not render.find_child("PropertiesPanel", true, false).visible)
	find_button(render, "MaximizePreview").button_pressed = false
	assert(abs(render.find_child("PreviewSplit", true, false).split_offsets[0] - 24) <= 1)
	assert(abs(render.find_child("WorkspaceSplit", true, false).split_offsets[0] + 18) <= 1)
	find_button(render, "ResetLayout").pressed.emit()
	var editor_focus_before = EditorInterface.is_distraction_free_mode_enabled()
	find_button(render, "FocusWorkspace").button_pressed = true
	assert(EditorInterface.is_distraction_free_mode_enabled())
	EditorInterface.set_main_screen_editor("3D")
	assert(EditorInterface.is_distraction_free_mode_enabled() == editor_focus_before)
	EditorInterface.set_main_screen_editor("Render")
	assert(EditorInterface.is_distraction_free_mode_enabled())
	find_button(render, "FocusWorkspace").button_pressed = false
	assert(EditorInterface.is_distraction_free_mode_enabled() == editor_focus_before)
	find_button(render, "CompositionGuides").button_pressed = true
	var config = ConfigFile.new()
	config.set_value("render", "state", render.get_state())
	assert(config.save("res://saved_sequence.cfg") == OK)
	var loaded = ConfigFile.new()
	assert(loaded.load("res://saved_sequence.cfg") == OK)
	render.set_state(loaded.get_value("render", "state"))
	assert(render.get_state().shots[1].shot.animation_start == 0.25)
	assert(EditorInterface.save_scene() == OK)
	var saved = ResourceLoader.load("res://fixture.tscn", "PackedScene", ResourceLoader.CACHE_MODE_IGNORE).instantiate()
	assert(saved.get_node("Subject").position.is_equal_approx(Vector3.ZERO), "Scene save included the preview pose")
	saved.free()
	print("SEQUENCE_STATE_OK")
	var options = JSON.parse_string(FileAccess.get_file_as_string("res://check.json"))
	if not options.graphics:
		EditorInterface.set_main_screen_editor("3D")
		get_tree().quit()
		return
	# One second, four frames, with a hard camera cut at frame three.
	state.shots[1].shot.camera_blend = 0.0
	state.output.music = "res://music.wav"
	state.output.music_start = 0.25
	state.shots[1].shot.dialogue = "res://dialogue.wav"
	state.shots[1].shot.dialogue_offset = 0.1
	state.sequence_time = 0.75
	render.set_state(state)
	await capture_ui(options.ui)
	check_layout(render)
	find_button(render, "FocusWorkspace").button_pressed = true
	find_button(render, "CompositionGuides").button_pressed = true
	await capture_ui(options.focus_ui)
	check_layout(render)
	var old_size = DisplayServer.window_get_size()
	var old_mode = DisplayServer.window_get_mode()
	DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_WINDOWED)
	DisplayServer.window_set_size(Vector2i(1280, 800))
	find_button(render, "FocusWorkspace").button_pressed = false
	find_button(render, "Pose").button_pressed = true
	await capture_ui(options.narrow_ui)
	check_layout(render)
	find_button(render, "ResetLayout").pressed.emit()
	find_button(render, "Pose").button_pressed = false
	DisplayServer.window_set_size(old_size)
	DisplayServer.window_set_mode(old_mode)
	await get_tree().process_frame
	render.render_sequence(options.png)
	assert(render.is_rendering())
	await wait_render(render)
	assert(is_equal_approx(render.get_state().sequence_time, 0.75))
	assert(subject.position.is_equal_approx(Vector3(0.75, 0, 0)))
	render.render_sequence(options.avi)
	await wait_render(render)
	assert(is_equal_approx(render.get_state().sequence_time, 0.75))
	render.render_sequence(options.ogv)
	await wait_render(render)
	assert(FileAccess.file_exists(options.ogv))
	# Seven FPS does not divide 48 kHz. Reuse both encoders and verify the
	# fractional audio samples and their frame indices in the output check.
	state.output.fps = 7
	render.set_state(state)
	render.render_sequence(options.avi7)
	await wait_render(render)
	render.render_sequence(options.ogv7)
	await wait_render(render)
	# Failed startup must restore time/state and permit a subsequent render.
	render.render_sequence(options.missing)
	assert(not render.is_rendering())
	assert(is_equal_approx(render.get_state().sequence_time, 0.75))
	render.render_sequence(options.cancel)
	EditorInterface.set_main_screen_editor("3D")
	await wait_render(render)
	assert(subject.position.is_equal_approx(Vector3.ZERO), "Cancellation leaked its pose into the scene")
	print("SEQUENCE_RENDER_OK")
	get_tree().quit()
"""


def tone(path: Path, frequency: float) -> None:
    rate = 48000
    samples = [int(math.sin(2 * math.pi * frequency * i / rate) * 6000) for i in range(rate * 2)]
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(struct.pack(f"<{len(samples)}h", *samples))


def check_outputs(work: Path) -> None:
    from PIL import Image, ImageChops

    frames = sorted(work.glob("frames_*.png"))
    assert len(frames) == 4, frames
    images = [Image.open(path).convert("RGB") for path in frames]
    assert all(image.size == (128, 96) for image in images)
    assert ImageChops.difference(images[0], images[2]).getbbox() is not None
    assert any(r > 150 and r > g * 2 for r, g, _ in images[0].convert("RGB").get_flattened_data()), (
        "Frame contains no subject"
    )
    videos: list[bytes] = []
    audio: list[bytes] = []
    audio_format: tuple[int, ...] | None = None

    def chunks(data: bytes, start: int, end: int) -> None:
        nonlocal audio_format
        while start + 8 <= end:
            tag = data[start : start + 4]
            size = struct.unpack_from("<I", data, start + 4)[0]
            body = start + 8
            finish = min(body + size, end)
            if tag == b"LIST":
                chunks(data, body + 4, finish)
            elif tag == b"00db":
                videos.append(data[body:finish])
            elif tag == b"01wb":
                audio.append(data[body:finish])
            elif tag == b"strf" and size == 16:
                audio_format = struct.unpack_from("<HHIIHH", data, body)
            start = body + size + (size & 1)

    for filename, count in (("sequence.avi", 4), ("sequence7.avi", 7)):
        videos.clear()
        audio.clear()
        audio_format = None
        avi = (work / filename).read_bytes()
        assert avi[:4] == b"RIFF" and avi[8:12] == b"AVI "
        assert struct.unpack_from("<I", avi, 4)[0] == len(avi) - 8
        chunks(avi, 12, len(avi))
        assert len(videos) == count and len(audio) == count
        assert all(Image.open(io.BytesIO(frame)).size == (128, 96) for frame in videos)
        # AVI's index must match the actual (potentially varying) audio blocks.
        index = avi.rfind(b"idx1")
        movi = avi.find(b"movi")
        for frame in range(count):
            entry = index + 8 + frame * 32 + 16
            tag, _, offset, size = struct.unpack_from("<4sIII", avi, entry)
            assert tag == b"01wb" and size == len(audio[frame])
            assert avi[movi + offset : movi + offset + 4] == b"01wb"
        assert audio_format is not None
        _, channels, rate, _, _, bits = audio_format
        assert channels == 2 and rate == 48000 and bits in (16, 32)
        raw = b"".join(audio)
        sample_type = "h" if bits == 16 else "i"
        values = struct.unpack(f"<{len(raw) // (bits // 8)}{sample_type}", raw)[::channels]
        assert len(values) == rate, "Fractional audio sample counts drifted"
        assert not any(values[: rate // 4]), "Music started before its scheduled time"

        def energy(start: float, end: float, frequency: float) -> float:
            block = values[int(start * rate) : int(end * rate)]
            real = sum(value * math.cos(2 * math.pi * frequency * i / rate) for i, value in enumerate(block))
            imag = sum(value * math.sin(2 * math.pi * frequency * i / rate) for i, value in enumerate(block))
            return math.hypot(real, imag) / len(block)

        music = energy(0.3, 0.4, 440)
        assert music > 100
        assert energy(0.3, 0.4, 880) < music * 0.02, "Dialogue started before its shot"
        assert energy(0.6, 0.7, 880) > music * 0.8, "Dialogue missing from movie"
        assert energy(0.6, 0.7, 440) > music * 0.8, "Music stopped at a camera cut"
    for filename in ("sequence.ogv", "sequence7.ogv"):
        data = (work / filename).read_bytes()
        offset = 0
        vorbis = None
        last_sample = 0
        while offset < len(data):
            assert data[offset : offset + 4] == b"OggS"
            granule, serial = struct.unpack_from("<QI", data, offset + 6)
            segments = data[offset + 26]
            body = offset + 27 + segments
            length = sum(data[offset + 27 : body])
            if data[body : body + 7] == b"\x01vorbis":
                vorbis = serial
            if serial == vorbis and granule != 0xFFFFFFFFFFFFFFFF:
                last_sample = max(last_sample, granule)
            offset = body + length
        assert last_sample == 48000, "OGV soundtrack length is incorrect"
    print("SEQUENCE_OUTPUTS_OK", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--temp-dir", type=Path)
    parser.add_argument("--state-only", action="store_true")
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()
    parent = (args.temp_dir or Path(tempfile.gettempdir())).resolve()
    work = Path(tempfile.mkdtemp(prefix="asosdot-sequence-check-", dir=parent))
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
        (addon / "plugin.cfg").write_text('[plugin]\nname="Sequence check"\nscript="plugin.gd"\n', encoding="utf-8")
        (addon / "plugin.gd").write_text(CHECK, encoding="utf-8")
        (project / "fixture.gd").write_text(FIXTURE, encoding="utf-8")
        (project / "project.godot").write_text(
            'config_version=5\n[application]\nconfig/name="Sequence check"\n'
            "[debug]\nfile_logging/enable_file_logging=false\n"
            '[rendering]\nrenderer/rendering_method="gl_compatibility"\n'
            '[editor_plugins]\nenabled=PackedStringArray("res://addons/check/plugin.cfg")\n',
            encoding="utf-8",
        )
        options = {
            "graphics": False,
            "png": str(work / "frames.png"),
            "avi": str(work / "sequence.avi"),
            "ogv": str(work / "sequence.ogv"),
            "avi7": str(work / "sequence7.avi"),
            "ogv7": str(work / "sequence7.ogv"),
            "cancel": str(work / "cancel.avi"),
            "missing": str(work / "missing/output.avi"),
            "ui": str(work / "sequencer_ui.png"),
            "focus_ui": str(work / "workspace_focus.png"),
            "narrow_ui": str(work / "workspace_narrow.png"),
        }
        tone(project / "music.wav", 440)
        tone(project / "dialogue.wav", 880)
        (project / "check.json").write_text(json.dumps(options), encoding="utf-8")
        base = [str(isolated), "--headless", "--path", str(project)]
        run(base + ["--script", "fixture.gd", "--quit-after", "120"], "SEQUENCE_FIXTURE_OK", work)
        run(base + ["--editor", "res://fixture.tscn"], "SEQUENCE_STATE_OK", work)
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
                "SEQUENCE_RENDER_OK",
                work,
            )
            check_outputs(work)
    finally:
        if not args.keep:
            # Only remove the specific directory created for this check.
            assert work.resolve().parent == parent and work.name.startswith("asosdot-sequence-check-")
            shutil.rmtree(work)


if __name__ == "__main__":
    main()
