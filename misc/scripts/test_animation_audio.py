#!/usr/bin/env python3
"""Check layered sounds on audio players and AnimationPlayer audio tracks by listening to the mix."""

from __future__ import annotations

import argparse
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

# Every sound is a constant level, so what reaches the Master bus is the sum of what plays.
TEST = r"""
extends Node

var capture := AudioEffectCapture.new()
var failures := []


func check(ok: bool, what: String) -> void:
	print(("PASS " if ok else "FAIL ") + what)
	if not ok:
		failures.append(what)


func near(value: float, expected: float) -> bool:
	return absf(value - expected) < 0.02


func dc(value: float, seconds: float) -> AudioStreamWAV:
	var wav := AudioStreamWAV.new()
	wav.format = AudioStreamWAV.FORMAT_16_BITS
	wav.mix_rate = 44100
	var frames := int(44100 * seconds)
	var data := PackedByteArray()
	data.resize(frames * 2)
	var sample := int(round(value * 32767.0))
	for i in frames:
		data.encode_s16(i * 2, sample)
	wav.data = data
	return wav


# A level that rises steadily from `from` to `to`, so the mix shows which way the sound runs.
func ramp(from: float, to: float, seconds: float) -> AudioStreamWAV:
	var wav := AudioStreamWAV.new()
	wav.format = AudioStreamWAV.FORMAT_16_BITS
	wav.mix_rate = 44100
	var frames := int(44100 * seconds)
	var data := PackedByteArray()
	data.resize(frames * 2)
	for i in frames:
		data.encode_s16(i * 2, int(round(lerpf(from, to, float(i) / frames) * 32767.0)))
	wav.data = data
	return wav


# The average level early and late in what was captured since the buffer was cleared.
func trend() -> Vector2:
	var available := capture.get_frames_available()
	if available < 10:
		return Vector2()
	var frames := capture.get_buffer(available)
	var early := 0.0
	var late := 0.0
	var span := available / 5
	for i in range(span, span * 2):
		early += frames[i].x
	for i in range(span * 3, span * 4):
		late += frames[i].x
	return Vector2(early / span, late / span)


# Moves the playhead from `from` to `to` at normal speed, the way the editor does while dragging.
func drag_playhead(animation_player: AnimationPlayer, from: float, to: float) -> void:
	var started := Time.get_ticks_msec()
	var direction := signf(to - from)
	var at := from
	while (to - at) * direction > 0.0:
		animation_player._set_audio_scrubbing(true)
		animation_player.seek(at, true, true)
		animation_player._set_audio_scrubbing(false)
		await get_tree().process_frame
		at = from + direction * (Time.get_ticks_msec() - started) / 1000.0


# The average left-channel level over the second half of the next `seconds`.
func level(seconds := 0.2) -> float:
	capture.clear_buffer()
	await get_tree().create_timer(seconds).timeout
	var available := capture.get_frames_available()
	if available < 2:
		return 0.0
	var frames := capture.get_buffer(available)
	var total := 0.0
	for i in range(available / 2, available):
		total += frames[i].x
	return total / (available - available / 2)


func wait(seconds: float) -> void:
	await get_tree().create_timer(seconds).timeout


func make_player(anim: Animation, manual := false) -> AnimationPlayer:
	var animation_player := AnimationPlayer.new()
	if manual:
		animation_player.callback_mode_process = AnimationMixer.ANIMATION_CALLBACK_MODE_PROCESS_MANUAL
	var library := AnimationLibrary.new()
	library.add_animation("a", anim)
	animation_player.add_animation_library("", library)
	add_child(animation_player)
	return animation_player


func audio_track(anim: Animation, path: String) -> int:
	var track := anim.add_track(Animation.TYPE_AUDIO)
	anim.track_set_path(track, path)
	return track


func _ready() -> void:
	capture.buffer_length = 2.0
	AudioServer.add_bus_effect(0, capture)
	await wait(0.3)
	check(near(await level(), 0.0), "silence before anything plays")

	await layered_sounds_from_scripts()
	await two_tracks_on_one_player()
	await keys_in_one_frame()
	await key_volume_and_pitch()
	await seeking_resumes_overlapping_keys()
	await polyphony_limit()
	await players_2d_and_3d()
	await scrubbing_follows_the_playhead()
	await playing_backwards_reverses_sounds()
	await playing_property_tracks(Animation.UPDATE_DISCRETE)
	await playing_property_tracks(Animation.UPDATE_CONTINUOUS)
	saving_keys()

	if failures.is_empty():
		print("ANIMATION_AUDIO_OK")
	else:
		print("FAILED: ", failures)
	get_tree().quit()


func layered_sounds_from_scripts() -> void:
	var player := AudioStreamPlayer.new()
	add_child(player)
	var finished := []
	player.layered_finished.connect(func(id): finished.append(id))
	var first: int = player.play_layered(dc(0.2, 3.0))
	var second: int = player.play_layered(dc(0.1, 3.0), 0.0, -6.0206)
	check(first >= 0 and second >= 0 and first != second, "play_layered() returns an ID for each sound")
	check(not player.playing, "layered sounds don't count as the player's own stream playing")
	check(near(await level(), 0.25), "two layered sounds mix, the second at its own volume")
	check(player.get_layered_count() == 2, "both layered sounds are counted")
	player.set_layered_volume_db(first, -6.0206)
	check(near(await level(), 0.15), "a layered sound's volume can change while it plays")
	player.stop_layered(first)
	await wait(0.1)
	check(not player.is_layered_playing(first) and player.is_layered_playing(second), "stop_layered() stops just that sound")
	check(finished.is_empty(), "a sound stopped on purpose isn't reported as finished")
	var short: int = player.play_layered(dc(0.05, 0.1))
	await wait(0.4)
	check(finished == [short], "layered_finished reports a sound that ended by itself")
	player.volume_db = -6.0206
	check(near(await level(), 0.025), "the player's volume applies to layered sounds")
	player.volume_db = 0.0
	player.stream = dc(0.3, 3.0)
	player.play()
	check(near(await level(), 0.35), "the player's own stream plays beside layered sounds")
	player.stream = dc(0.1, 3.0)
	check(player.is_layered_playing(second), "changing the stream keeps layered sounds")
	player.stop()
	await wait(0.1)
	check(player.get_layered_count() == 0 and near(await level(), 0.0), "stop() silences layered sounds too")
	player.queue_free()


func two_tracks_on_one_player() -> void:
	var player := AudioStreamPlayer.new()
	player.name = "Shared"
	add_child(player)
	var own := dc(0.1, 5.0)
	player.stream = own
	var anim := Animation.new()
	anim.length = 3.0
	anim.audio_track_insert_key(audio_track(anim, "Shared"), 0.0, dc(0.25, 3.0))
	anim.audio_track_insert_key(audio_track(anim, "Shared"), 0.0, dc(0.125, 3.0))
	var animation_player := make_player(anim)
	player.play()
	animation_player.play("a")
	check(near(await level(), 0.475), "two audio tracks on one player and its own stream are all heard")
	check(player.get_layered_count() == 2, "each track's key plays its own sound")
	animation_player.stop()
	await wait(0.1)
	check(player.stream == own, "the player keeps its own stream")
	check(player.playing and near(await level(), 0.1), "stopping the animation leaves the player's own stream playing")
	player.stop()
	animation_player.queue_free()
	player.queue_free()


func keys_in_one_frame() -> void:
	var player := AudioStreamPlayer.new()
	player.name = "Burst"
	add_child(player)
	var anim := Animation.new()
	anim.length = 3.0
	var track := audio_track(anim, "Burst")
	anim.audio_track_insert_key(track, 0.10, dc(0.1, 3.0))
	anim.audio_track_insert_key(track, 0.15, dc(0.1, 3.0))
	anim.audio_track_insert_key(track, 0.20, dc(0.1, 3.0))
	var animation_player := make_player(anim, true)
	animation_player.play("a")
	animation_player.advance(0.3)
	check(player.get_layered_count() == 3, "every key passed in one update plays")
	check(near(await level(), 0.3), "the keys passed in one update are all heard")
	animation_player.stop()
	animation_player.queue_free()
	player.queue_free()


func key_volume_and_pitch() -> void:
	var player := AudioStreamPlayer.new()
	player.name = "Shaped"
	add_child(player)
	var anim := Animation.new()
	anim.length = 3.0
	var track := audio_track(anim, "Shaped")
	var key := anim.audio_track_insert_key(track, 0.0, dc(0.4, 3.0))
	anim.audio_track_set_key_volume_db(track, key, -6.0206)
	var short := audio_track(anim, "Shaped")
	var fast := anim.audio_track_insert_key(short, 0.0, dc(0.1, 1.0))
	anim.audio_track_set_key_pitch_scale(short, fast, 4.0)
	check(is_equal_approx(anim.audio_track_get_key_volume_db(track, key), -6.0206), "a key keeps its volume")
	check(is_equal_approx(anim.audio_track_get_key_pitch_scale(short, fast), 4.0), "a key keeps its pitch")
	var value: Dictionary = anim.track_get_key_value(track, key)
	check(is_equal_approx(value.get("volume_db", 0.0), -6.0206), "a key's value carries its volume")
	var animation_player := make_player(anim)
	animation_player.play("a")
	await wait(0.1)
	check(player.get_layered_count() == 2, "both shaped keys start")
	await wait(0.4)
	check(player.get_layered_count() == 1, "a key at pitch 4 ends four times sooner")
	check(near(await level(), 0.2), "a key plays at its own volume")
	animation_player.stop()
	animation_player.queue_free()
	player.queue_free()


func seeking_resumes_overlapping_keys() -> void:
	var player := AudioStreamPlayer.new()
	player.name = "Seek"
	add_child(player)
	var anim := Animation.new()
	anim.length = 4.0
	var track := audio_track(anim, "Seek")
	anim.audio_track_insert_key(track, 0.0, dc(0.2, 3.0))
	anim.audio_track_insert_key(track, 0.3, dc(0.1, 3.0))
	anim.audio_track_insert_key(track, 0.4, dc(0.05, 0.05))
	anim.audio_track_insert_key(track, 1.0, dc(0.4, 3.0))
	var animation_player := make_player(anim, true)
	animation_player.play("a")
	animation_player.seek(0.5, true)
	check(player.get_layered_count() == 2, "seeking resumes every key still sounding there, and only those")
	check(near(await level(), 0.3), "the resumed keys are heard together")
	animation_player.seek(0.6, true)
	await wait(0.1)
	check(player.get_layered_count() == 2, "seeking again doesn't double the resumed sounds")
	animation_player.stop()
	animation_player.queue_free()
	player.queue_free()


func polyphony_limit() -> void:
	var player := AudioStreamPlayer.new()
	player.name = "Limited"
	add_child(player)
	var anim := Animation.new()
	anim.length = 3.0
	var track := audio_track(anim, "Limited")
	for i in 3:
		anim.audio_track_insert_key(track, 0.1 * i, dc(0.1, 3.0))
	var animation_player := make_player(anim, true)
	animation_player.audio_max_polyphony = 2
	animation_player.play("a")
	animation_player.advance(0.25)
	await wait(0.1)
	check(player.get_layered_count() == 2, "audio_max_polyphony stops the oldest sound")
	animation_player.stop()
	animation_player.queue_free()
	player.queue_free()


func players_2d_and_3d() -> void:
	var player_2d := AudioStreamPlayer2D.new()
	player_2d.name = "Flat"
	add_child(player_2d)
	player_2d.global_position = get_viewport().get_visible_rect().size * 0.5
	var player_3d := AudioStreamPlayer3D.new()
	player_3d.name = "Deep"
	var camera := Camera3D.new()
	add_child(camera)
	camera.make_current()
	add_child(player_3d)
	player_3d.position = Vector3(0, 0, -1)
	var anim := Animation.new()
	anim.length = 3.0
	anim.audio_track_insert_key(audio_track(anim, "Flat"), 0.0, dc(0.2, 3.0))
	anim.audio_track_insert_key(audio_track(anim, "Deep"), 0.0, dc(0.2, 3.0))
	var animation_player := make_player(anim)
	animation_player.play("a")
	await wait(0.2)
	check(player_2d.get_layered_count() == 1, "an AudioStreamPlayer2D plays an audio track's key")
	check(player_3d.get_layered_count() == 1, "an AudioStreamPlayer3D plays an audio track's key")
	check(await level() > 0.1, "positional layered sounds are heard")
	var id: int = player_3d.play_layered(dc(0.2, 3.0))
	check(player_3d.is_layered_playing(id), "an AudioStreamPlayer3D plays a layered sound from a script")
	animation_player.stop()
	player_3d.stop()
	await wait(0.1)
	check(near(await level(), 0.0), "positional layered sounds stop")
	animation_player.queue_free()
	player_2d.queue_free()
	player_3d.queue_free()
	camera.queue_free()


func scrubbing_follows_the_playhead() -> void:
	var player := AudioStreamPlayer.new()
	player.name = "Scrubbed"
	add_child(player)
	var anim := Animation.new()
	anim.length = 3.0
	anim.audio_track_insert_key(audio_track(anim, "Scrubbed"), 0.0, ramp(0.0, 0.6, 3.0))
	var animation_player := make_player(anim, true)
	animation_player.play("a")
	animation_player.pause()
	check(near(await level(), 0.0), "a paused animation is silent")

	capture.clear_buffer()
	await drag_playhead(animation_player, 0.5, 1.5)
	var forward := trend()
	check(forward.x > 0.05 and forward.y > forward.x + 0.05, "dragging the playhead forwards plays the sound forwards (%.3f to %.3f)" % [forward.x, forward.y])
	await wait(0.3)
	check(near(await level(), 0.0), "the sound falls silent when the playhead stops")

	capture.clear_buffer()
	await drag_playhead(animation_player, 1.5, 0.5)
	var backward := trend()
	check(backward.y > 0.05 and backward.x > backward.y + 0.05, "dragging the playhead backwards plays the sound backwards (%.3f to %.3f)" % [backward.x, backward.y])
	await wait(1.5)
	check(player.get_layered_count() == 0, "a scrubbed sound ends once the playhead has been still a moment")

	animation_player.seek(0.5, true, true)
	await wait(0.2)
	check(near(await level(), 0.0), "seeks that aren't scrubs stay silent")
	animation_player.stop()
	animation_player.queue_free()
	player.queue_free()


func playing_backwards_reverses_sounds() -> void:
	var player := AudioStreamPlayer.new()
	player.name = "Reversed"
	add_child(player)
	var anim := Animation.new()
	anim.length = 3.0
	anim.audio_track_insert_key(audio_track(anim, "Reversed"), 0.0, ramp(0.0, 0.6, 3.0))
	var animation_player := make_player(anim)
	animation_player.play_backwards("a")
	await wait(0.3)
	capture.clear_buffer()
	await wait(0.8)
	var reversed := trend()
	check(reversed.y > 0.2 and reversed.x > reversed.y + 0.03, "playing backwards plays the sound backwards (%.3f to %.3f)" % [reversed.x, reversed.y])
	check(player.get_layered_count() == 1, "one sound follows the playhead backwards")
	animation_player.pause()
	await wait(0.1)
	check(player.get_layered_count() == 0 and near(await level(), 0.0), "pausing stops it")

	animation_player.play("a")
	await wait(0.3)
	capture.clear_buffer()
	await wait(0.6)
	var forward := trend()
	check(forward.y > forward.x + 0.02, "playing forwards again plays it forwards (%.3f to %.3f)" % [forward.x, forward.y])
	animation_player.stop()
	animation_player.queue_free()
	player.queue_free()


# A value track switching an AudioStreamPlayer3D's `playing` on with one key, as cutscenes often do.
func playing_property_tracks(update_mode: int) -> void:
	var mode := "discrete" if update_mode == Animation.UPDATE_DISCRETE else "continuous"
	var camera := Camera3D.new()
	add_child(camera)
	camera.make_current()
	var player := AudioStreamPlayer3D.new()
	player.name = "Owned_" + mode
	player.stream = ramp(0.0, 0.6, 3.0)
	player.attenuation_model = AudioStreamPlayer3D.ATTENUATION_DISABLED
	player.panning_strength = 0.0
	add_child(player)
	player.position = Vector3(0, 0, -1)
	var anim := Animation.new()
	anim.length = 4.0
	var track := anim.add_track(Animation.TYPE_VALUE)
	anim.track_set_path(track, "Owned_%s:playing" % mode)
	anim.value_track_set_update_mode(track, update_mode)
	anim.track_insert_key(track, 1.0, true)
	var animation_player := make_player(anim, true)
	animation_player.play("a")
	animation_player.pause()

	capture.clear_buffer()
	await drag_playhead(animation_player, 0.1, 0.8)
	await wait(0.1)
	check(not player.playing and near(await level(), 0.0), "[%s] scrubbing before the key leaves the stream silent" % mode)

	capture.clear_buffer()
	await drag_playhead(animation_player, 1.5, 2.5)
	var forward := trend()
	check(not player.playing and player.get_layered_count() == 1, "[%s] scrubbing after the key plays the stream under the playhead, not the player itself" % mode)
	check(forward.x > 0.03 and forward.y > forward.x + 0.02, "[%s] dragging forwards plays it forwards (%.3f to %.3f)" % [mode, forward.x, forward.y])
	await wait(0.3)
	check(near(await level(), 0.0), "[%s] it falls silent when the playhead stops" % mode)

	animation_player.callback_mode_process = AnimationMixer.ANIMATION_CALLBACK_MODE_PROCESS_IDLE
	animation_player.play("a")
	animation_player.seek(2.0, true)
	await wait(0.05)
	check(player.playing and absf(player.get_playback_position() - 1.05) < 0.15, "[%s] seeking past the key resumes the stream partway (at %.2f s)" % [mode, player.get_playback_position()])
	animation_player.seek(0.5, true)
	await wait(0.05)
	check(not player.playing, "[%s] seeking back before the key stops it" % mode)
	await wait(0.7)
	check(player.playing and player.get_playback_position() < 0.4, "[%s] passing the key starts the stream from its beginning (at %.2f s)" % [mode, player.get_playback_position()])
	await wait(0.4)
	check(player.get_playback_position() > 0.4, "[%s] the stream keeps playing rather than restarting every frame (at %.2f s)" % [mode, player.get_playback_position()])

	animation_player.play_backwards("a")
	animation_player.seek(3.0, true)
	await wait(0.3)
	capture.clear_buffer()
	await wait(0.6)
	var reversed := trend()
	check(not player.playing and player.get_layered_count() == 1, "[%s] playing backwards follows the playhead instead of restarting the stream" % mode)
	check(reversed.x > reversed.y + 0.02, "[%s] playing backwards plays it backwards (%.3f to %.3f)" % [mode, reversed.x, reversed.y])
	animation_player.stop()
	player.stop()
	animation_player.queue_free()
	player.queue_free()
	camera.queue_free()


func saving_keys() -> void:
	var anim := Animation.new()
	var track := audio_track(anim, "Any")
	var stream := dc(0.1, 0.1)
	var plain := anim.audio_track_insert_key(track, 0.0, stream)
	var shaped := anim.audio_track_insert_key(track, 0.5, stream)
	anim.audio_track_set_key_volume_db(track, shaped, -3.0)
	anim.audio_track_set_key_pitch_scale(track, shaped, 1.5)
	check(ResourceSaver.save(anim, "user://keys.tres") == OK, "an animation with shaped audio keys saves")
	var text := FileAccess.get_file_as_string("user://keys.tres")
	check(text.count("volume_db") == 1 and text.count("pitch_scale") == 1, "only keys that change volume or pitch store them")
	var loaded: Animation = ResourceLoader.load("user://keys.tres", "", ResourceLoader.CACHE_MODE_IGNORE)
	check(is_equal_approx(loaded.audio_track_get_key_volume_db(track, shaped), -3.0) and is_equal_approx(loaded.audio_track_get_key_pitch_scale(track, shaped), 1.5), "volume and pitch load back")
	check(is_zero_approx(loaded.audio_track_get_key_volume_db(track, plain)) and is_equal_approx(loaded.audio_track_get_key_pitch_scale(track, plain), 1.0), "plain keys load with no change")
	print("KEYS_FILE ", ProjectSettings.globalize_path("user://keys.tres"))
"""

# Inside the editor, a paused or stopped preview must stay quiet unless the playhead is dragged.
EDITOR_CHECK = r"""
@tool
extends EditorPlugin

var failures := []


func check(ok: bool, what: String) -> void:
	print(("PASS " if ok else "FAIL ") + "[editor] " + what)
	if not ok:
		failures.append(what)


func dc(value: float, seconds: float) -> AudioStreamWAV:
	var wav := AudioStreamWAV.new()
	wav.format = AudioStreamWAV.FORMAT_16_BITS
	wav.mix_rate = 44100
	var frames := int(44100 * seconds)
	var data := PackedByteArray()
	data.resize(frames * 2)
	var sample := int(round(value * 32767.0))
	for i in frames:
		data.encode_s16(i * 2, sample)
	wav.data = data
	return wav


func wait(seconds: float) -> void:
	await get_tree().create_timer(seconds).timeout


func _enter_tree() -> void:
	run.call_deferred()


func run() -> void:
	await wait(2.0)
	var root := Node.new()
	EditorInterface.get_base_control().add_child(root)
	var voice := AudioStreamPlayer.new()
	voice.name = "Voice"
	voice.stream = dc(0.2, 3.0)
	root.add_child(voice)
	var clip := AudioStreamPlayer.new()
	clip.name = "Clip"
	root.add_child(clip)
	var anim := Animation.new()
	anim.length = 4.0
	var track := anim.add_track(Animation.TYPE_VALUE)
	anim.track_set_path(track, "Voice:playing")
	anim.value_track_set_update_mode(track, Animation.UPDATE_DISCRETE)
	anim.track_insert_key(track, 1.0, true)
	var audio := anim.add_track(Animation.TYPE_AUDIO)
	anim.track_set_path(audio, "Clip")
	anim.audio_track_insert_key(audio, 0.5, dc(0.2, 3.0))
	var animation_player := AnimationPlayer.new()
	var library := AnimationLibrary.new()
	library.add_animation("a", anim)
	animation_player.add_animation_library("", library)
	root.add_child(animation_player)
	animation_player.play("a")
	animation_player.pause()

	# What the animation editor does while the playhead is dragged.
	for i in 10:
		animation_player._set_audio_scrubbing(true)
		animation_player.seek(1.5 + i * 0.05, true, true)
		animation_player._set_audio_scrubbing(false)
		await get_tree().process_frame
	check(not voice.playing and voice.get_layered_count() == 1 and clip.get_layered_count() == 1, "dragging the playhead of a paused preview sounds both tracks under it")

	# What it does after a key is edited.
	animation_player.advance(0)
	await wait(0.1)
	check(not voice.playing and voice.get_layered_count() == 0 and clip.get_layered_count() == 0, "refreshing a paused preview starts no sound")
	animation_player.seek(2.5, true)
	await wait(0.1)
	check(not voice.playing and clip.get_layered_count() == 0, "seeking a paused preview starts no sound")

	animation_player.seek(0.8, true)
	animation_player.play("a")
	await wait(0.5)
	check(voice.playing and clip.get_layered_count() == 1, "a playing preview starts both at their keys")
	animation_player.pause()
	await get_tree().process_frame
	check(not voice.playing and clip.get_layered_count() == 0, "pausing the preview silences both")
	animation_player.play("a")
	await wait(0.1)
	check(voice.playing and clip.get_layered_count() == 1, "resuming the preview picks both up again")
	animation_player.stop()
	await get_tree().process_frame
	check(not voice.playing and clip.get_layered_count() == 0, "stopping the preview silences both")

	root.queue_free()
	print("EDITOR_AUDIO_OK" if failures.is_empty() else "EDITOR_AUDIO_FAILED")
	get_tree().quit()
"""

# Stock Godot 4.7 must still open what Asosdot saved.
STOCK_CHECK = r"""
extends SceneTree

func _initialize() -> void:
	var anim: Animation = load("res://keys.tres")
	print("STOCK_KEYS ", anim.track_get_key_count(0))
	quit()
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--stock-engine", type=Path, help="A Godot 4.7 editor, to check it loads the saved keys")
    parser.add_argument("--temp-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="asosdot-animation-audio-", dir=args.temp_dir) as directory:
        work = Path(directory)
        runtime = work / "runtime"
        runtime.mkdir()
        isolated = runtime / args.engine.name
        shutil.copy2(args.engine.resolve(), isolated)
        (runtime / "_sc_").touch()
        project = work / "project"
        project.mkdir()
        (project / "test.gd").write_text(TEST, encoding="utf-8")
        (project / "main.tscn").write_text(
            '[gd_scene load_steps=2 format=3]\n[ext_resource type="Script" path="res://test.gd" id="1"]\n'
            '[node name="Main" type="Node"]\nscript = ExtResource("1")\n',
            encoding="utf-8",
        )
        name = f"Animation audio check {work.name}"
        (project / "project.godot").write_text(
            f'config_version=5\n[application]\nconfig/name="{name}"\nrun/main_scene="res://main.tscn"\n',
            encoding="utf-8",
        )
        base = [str(isolated), "--path", str(project)]
        run(base + ["--headless", "--editor", "--quit-after", "200"], None, work)
        run(base + ["--headless", "--audio-driver", "Dummy"], "ANIMATION_AUDIO_OK", work)
        log = (work / "ANIMATION_AUDIO_OK.log").read_text(encoding="utf-8", errors="replace")
        print("\n".join(line for line in log.splitlines() if line.startswith(("PASS", "FAIL"))), flush=True)

        editor_project = work / "editor_project"
        (editor_project / "addons/check").mkdir(parents=True)
        (editor_project / "addons/check/plugin.cfg").write_text('[plugin]\nname="Audio check"\nscript="plugin.gd"\n', encoding="utf-8")
        (editor_project / "addons/check/plugin.gd").write_text(EDITOR_CHECK, encoding="utf-8")
        (editor_project / "project.godot").write_text(
            f'config_version=5\n[application]\nconfig/name="{name}"\n'
            '[editor_plugins]\nenabled=PackedStringArray("res://addons/check/plugin.cfg")\n',
            encoding="utf-8",
        )
        run([str(isolated), "--path", str(editor_project), "--headless", "--editor"], "EDITOR_AUDIO_OK", work)
        editor_log = (work / "EDITOR_AUDIO_OK.log").read_text(encoding="utf-8", errors="replace")
        print("\n".join(line for line in editor_log.splitlines() if line.startswith(("PASS", "FAIL"))), flush=True)
        saved = Path(next(line for line in log.splitlines() if line.startswith("KEYS_FILE ")).split(" ", 1)[1].strip())
        try:
            if args.stock_engine:
                stock_project = work / "stock"
                stock_project.mkdir()
                shutil.copy2(saved, stock_project / "keys.tres")
                (stock_project / "project.godot").write_text('config_version=5\n[application]\nconfig/name="Stock check"\n', encoding="utf-8")
                (stock_project / "check.gd").write_text(STOCK_CHECK, encoding="utf-8")
                stock_runtime = work / "stock_runtime"
                stock_runtime.mkdir()
                stock = stock_runtime / args.stock_engine.name
                shutil.copy2(args.stock_engine.resolve(), stock)
                # Godot's console launcher starts the main executable beside it.
                main_exe = args.stock_engine.resolve().with_name(args.stock_engine.name.replace("_console", ""))
                if main_exe.exists():
                    shutil.copy2(main_exe, stock_runtime / main_exe.name)
                (stock_runtime / "_sc_").touch()
                run([str(stock), "--headless", "--path", str(stock_project), "--script", "res://check.gd"], "STOCK_KEYS 2", work)
                stock_log = (work / "STOCK_KEYS 2.log").read_text(encoding="utf-8", errors="replace")
                assert "ERROR" not in stock_log, stock_log
        finally:
            # Only the user data folder this run's uniquely named project made.
            if saved.parent.name == name:
                shutil.rmtree(saved.parent, ignore_errors=True)
        print("ANIMATION_AUDIO_TEST_OK", flush=True)


if __name__ == "__main__":
    main()
