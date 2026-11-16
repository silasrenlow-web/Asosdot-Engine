#!/usr/bin/env python3
"""Check Global Transmit, headless: the GlobalTransmit singleton, receivers set up on a node
the way the editor's Transmit tab stores them, and the Transmit animation track.

Transmissions on name and number channels reach their listeners with any number of
arguments (100 and 1000 are tried); a listener taking fewer arguments gets the first ones;
duplicate listens are ignored, stopped and freed listeners get nothing; the last arguments,
counts and channels are kept until cleared; "transmitted" reports every transmission;
transmit_deferred delivers at the end of the frame; a bad channel is refused; a listener that
transmits back to itself is stopped at the nesting limit. A node with receivers in its
metadata listens while it is in the tree. A Transmit track fires its keys during playback,
in the mixer's immediate and deferred method modes, and survives saving and loading."""

from __future__ import annotations

import argparse
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

TEST = r"""
extends Node

var fails := 0
var got := []


func check(p_name: String, p_ok: bool, p_detail: Variant = "") -> void:
	print(("PASS " if p_ok else "FAIL ") + p_name + "  " + str(p_detail))
	if not p_ok:
		fails += 1


func on_two(a = null, b = null) -> void:
	got.append([a, b])


func on_one(a) -> void:
	got.append(a)


func on_many(...arguments) -> void:
	got.append(arguments.size())


func on_echo() -> void:
	GlobalTransmit.transmit(&"echo")


func frames(p_count: int) -> void:
	for i in p_count:
		await get_tree().process_frame


func _ready() -> void:
	# Names and numbers.
	GlobalTransmit.listen(&"alarm", on_two)
	GlobalTransmit.listen(7, on_one)
	GlobalTransmit.transmit(&"alarm", 1, 2)
	GlobalTransmit.transmit("alarm", "string name", "is the same channel")
	GlobalTransmit.transmit(7, "seven")
	GlobalTransmit.transmit("7", "not channel seven")
	check("a name channel delivers its arguments", got[0] == [1, 2], got)
	check("a String and a StringName are the same channel", got[1] == ["string name", "is the same channel"])
	check("a number channel delivers", got[2] == "seven")
	check("the number 7 and the name \"7\" are different channels", got.size() == 3, got.size())

	# A listener that takes fewer arguments gets the first ones; defaults fill the rest.
	got.clear()
	GlobalTransmit.transmit(7, "first", "second", "third")
	GlobalTransmit.transmit(&"alarm", "only one")
	check("fewer parameters: the first arguments", got[0] == "first", got)
	check("more parameters: defaults fill in", got[1] == ["only one", null], got)

	# Many arguments.
	got.clear()
	GlobalTransmit.listen(&"many", on_many)
	var hundred := []
	for i in 100:
		hundred.append(i)
	GlobalTransmit.transmit_array(&"many", hundred)
	var thousand := []
	for i in 1000:
		thousand.append(i)
	GlobalTransmit.transmit_array(&"many", thousand)
	check("100 arguments arrive", got[0] == 100, got)
	check("1000 arguments arrive", got[1] == 1000, got)
	check("and the last 1000 are kept for the channel", GlobalTransmit.get_arguments(&"many").size() == 1000 and GlobalTransmit.get_arguments(&"many")[999] == 999)

	# Listening once, stopping, freed listeners.
	GlobalTransmit.listen(7, on_one)
	check("listening twice is listening once", GlobalTransmit.get_listeners(7).size() == 1)
	check("is_listening", GlobalTransmit.is_listening(7, on_one) and not GlobalTransmit.is_listening(7, on_two))
	GlobalTransmit.stop_listening(7, on_one)
	got.clear()
	GlobalTransmit.transmit(7, "nobody")
	check("a stopped listener gets nothing", got.is_empty() and not GlobalTransmit.is_listening(7, on_one))
	var temporary := Node.new()
	temporary.set_script(load("res://receiver.gd"))
	GlobalTransmit.listen(&"freed", Callable(temporary, "receive"))
	temporary.free()
	GlobalTransmit.transmit(&"freed", 1)
	check("a freed listener is dropped", GlobalTransmit.get_listeners(&"freed").is_empty())

	# History.
	check("transmission counts", GlobalTransmit.get_transmission_count(&"alarm") == 3 and GlobalTransmit.get_transmission_count(7) == 3, [GlobalTransmit.get_transmission_count(&"alarm"), GlobalTransmit.get_transmission_count(7)])
	check("the last arguments of a channel", GlobalTransmit.get_arguments(&"alarm") == ["only one"])
	var channels := GlobalTransmit.get_channels()
	check("channels are listed, names and numbers", channels.has(&"alarm") and channels.has(7) and channels.has(&"7"), channels)
	GlobalTransmit.clear_channel(&"7")
	check("clear_channel forgets a channel", not GlobalTransmit.get_channels().has(&"7") and GlobalTransmit.get_transmission_count(&"7") == 0)

	# The catch-all signal.
	var seen := []
	GlobalTransmit.transmitted.connect(func(channel, arguments): seen.append([channel, arguments]))
	GlobalTransmit.transmit(42, "x")
	check("transmitted reports every transmission", seen.size() == 1 and seen[0] == [42, ["x"]], seen)

	# Deferred.
	got.clear()
	GlobalTransmit.listen(&"later", on_one)
	GlobalTransmit.transmit_deferred(&"later", "at the end of the frame")
	check("transmit_deferred doesn't deliver at once", got.is_empty())
	await frames(1)
	check("but by the next frame", got == ["at the end of the frame"], got)

	# A bad channel is refused; a loop is stopped.
	var before := GlobalTransmit.get_channels().size()
	GlobalTransmit.transmit_array(1.5, ["x"])
	check("a channel that is neither a name nor a number is refused", GlobalTransmit.get_channels().size() == before)
	GlobalTransmit.listen(&"echo", on_echo)
	GlobalTransmit.transmit(&"echo")
	check("a listener transmitting back to itself stops at the nesting limit", GlobalTransmit.get_transmission_count(&"echo") == 64, GlobalTransmit.get_transmission_count(&"echo"))
	GlobalTransmit.stop_listening(&"echo", on_echo)

	# A node set up in the editor's Transmit tab: receivers in its metadata.
	var bell := Node.new()
	bell.set_script(load("res://receiver.gd"))
	bell.set_meta("_transmit_receivers", [{"channel": &"bell", "method": &"receive"}, {"channel": 3, "method": &"receive"}])
	GlobalTransmit.transmit(&"bell", "before entering")
	add_child(bell)
	GlobalTransmit.transmit(&"bell", "ding", 2)
	GlobalTransmit.transmit(3, "three")
	check("a node with receivers listens while in the tree", bell.received == [["ding", 2], ["three", null]], bell.received)
	remove_child(bell)
	GlobalTransmit.transmit(&"bell", "after leaving")
	check("and stops when it leaves", bell.received.size() == 2, bell.received)
	bell.free()

	# The Transmit track.
	var animation := Animation.new()
	animation.length = 1.0
	var track := animation.add_track(Animation.TYPE_TRANSMIT)
	animation.track_set_path(track, ".")
	animation.transmit_track_insert_key(track, 0.2, &"door", ["open", 1.5])
	animation.transmit_track_insert_key(track, 0.6, 12, [])
	check("keys keep their channel and arguments", animation.transmit_track_get_channel(track, 0) == &"door" and animation.transmit_track_get_arguments(track, 0) == ["open", 1.5] and animation.transmit_track_get_channel(track, 1) == 12)
	check("a key is a channel and arguments", animation.track_get_key_value(track, 0) == {"channel": &"door", "arguments": ["open", 1.5]}, animation.track_get_key_value(track, 0))
	ResourceSaver.save(animation, "res://transmit.tres")
	var loaded: Animation = ResourceLoader.load("res://transmit.tres", "", ResourceLoader.CACHE_MODE_IGNORE)
	check("a Transmit track survives saving and loading", loaded.track_get_type(0) == Animation.TYPE_TRANSMIT and loaded.track_get_key_value(0, 0) == animation.track_get_key_value(0, 0) and loaded.transmit_track_get_channel(0, 1) == 12)
	check("its file names the track type", FileAccess.get_file_as_string("res://transmit.tres").contains("\"transmit\""))

	var heard := []
	GlobalTransmit.listen(&"door", func(what, speed): heard.append([what, speed]))
	GlobalTransmit.listen(12, func(): heard.append(12))
	var library := AnimationLibrary.new()
	library.add_animation(&"open", animation)
	var player := AnimationPlayer.new()
	add_child(player)
	player.root_node = NodePath("..")
	player.add_animation_library(&"", library)
	for mode in [AnimationMixer.ANIMATION_CALLBACK_MODE_METHOD_IMMEDIATE, AnimationMixer.ANIMATION_CALLBACK_MODE_METHOD_DEFERRED]:
		heard.clear()
		player.callback_mode_method = mode
		player.play(&"open")
		player.seek(0.0, true)
		await get_tree().create_timer(1.3).timeout
		check("the track fires its keys in order (%s)" % ["immediate", "deferred"][mode], heard == [["open", 1.5], 12], heard)
		player.stop()

	print("GLOBAL_TRANSMIT fails=%d" % fails)
	get_tree().quit()
"""

RECEIVER = """extends Node

var received := []


func receive(a = null, b = null) -> void:
	received.append([a, b])
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--temp-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="asosdot-transmit-", dir=args.temp_dir) as directory:
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
        (project / "receiver.gd").write_text(RECEIVER, encoding="utf-8")
        (project / "main.tscn").write_text(
            '[gd_scene load_steps=2 format=3]\n\n[ext_resource type="Script" path="res://test.gd" id="1"]\n\n'
            '[node name="Main" type="Node"]\nscript = ExtResource("1")\n',
            encoding="utf-8",
        )
        (project / "project.godot").write_text(
            f'config_version=5\n[application]\nconfig/name="Transmit {work.name}"\nrun/main_scene="res://main.tscn"\n',
            encoding="utf-8",
        )
        command = [str(isolated), "--headless", "--path", str(project)]
        run(command, "GLOBAL_TRANSMIT fails=0", work)
        log = (work / "GLOBAL_TRANSMIT fails=0.log").read_text(encoding="utf-8", errors="replace")
        print("\n".join(line for line in log.splitlines() if line.startswith(("PASS", "FAIL"))), flush=True)
        print("GLOBAL_TRANSMIT_TEST_OK", flush=True)


if __name__ == "__main__":
    main()
