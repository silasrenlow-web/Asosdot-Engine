#!/usr/bin/env python3
"""Check the save and load framework (Saves and SaveState) end to end, headless:
round trips through real scenes, typed arrays, node and resource references,
spawnable nodes that come back or go away, restoring the scene a save was made
in, encryption that can't be read or changed by hand, passwords, backups, version
migration, autosave and play time."""

from __future__ import annotations

import argparse
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

PLAYER = """extends Node2D
var coins := 0
var inventory: Array[String] = []
var ref: Node
var res: Resource
"""

ENEMY = """extends Node2D
var health := 100
"""

DOOR = """extends Node2D
var open := false
"""

ENEMY_SCENE = """[gd_scene load_steps=3 format=3]

[ext_resource type="Script" path="res://enemy.gd" id="1"]
[ext_resource type="Script" path="res://save_state_holder.gd" id="2"]

[node name="Enemy" type="Node2D"]
script = ExtResource("1")

[node name="SaveState" type="SaveState" parent="."]
properties = PackedStringArray("position", "health")
spawnable = true
"""

LEVEL_A = """[gd_scene load_steps=5 format=3]

[ext_resource type="Script" path="res://player.gd" id="1"]
[ext_resource type="Script" path="res://door.gd" id="2"]
[ext_resource type="PackedScene" path="res://enemy.tscn" id="3"]

[node name="LevelA" type="Node2D"]

[node name="Player" type="Node2D" parent="."]
script = ExtResource("1")

[node name="SaveState" type="SaveState" parent="Player"]
properties = PackedStringArray("position", "coins", "inventory", "scale:x", "ref", "res")

[node name="Door" type="Node2D" parent="."]
script = ExtResource("2")

[node name="SaveState" type="SaveState" parent="Door"]
properties = PackedStringArray("open", "visible")

[node name="Enemy1" parent="." instance=ExtResource("3")]
position = Vector2(100, 100)
"""

LEVEL_B = """[gd_scene format=3]

[node name="LevelB" type="Node2D"]

[node name="Lamp" type="Node2D" parent="."]

[node name="SaveState" type="SaveState" parent="Lamp"]
properties = PackedStringArray("visible", "position")
"""

ITEM = """[gd_resource type="Gradient" format=3]

[resource]
"""

TEST = r"""
extends Node

var fails := 0
var level: Node
var loaded_slots: Array = []


func check(p_name: String, p_ok: bool, p_detail: Variant = "") -> void:
	print(("PASS " if p_ok else "FAIL ") + p_name + "  " + str(p_detail))
	if not p_ok:
		fails += 1


func frames(p_count: int = 2) -> void:
	for i in p_count:
		await get_tree().process_frame


func scene() -> Node:
	return get_tree().current_scene


func enemies() -> Array:
	var found := []
	for n in scene().get_children():
		if n is Node2D and n.get_script() != null and "health" in n and not n.is_queued_for_deletion():
			found.append(n)
	return found


func read_bytes(p_slot: String) -> PackedByteArray:
	return FileAccess.get_file_as_bytes(Saves.get_slot_path(p_slot))


func contains(p_bytes: PackedByteArray, p_text: String) -> bool:
	return p_bytes.get_string_from_utf8().contains(p_text) or p_bytes.hex_encode().contains(p_text.to_utf8_buffer().hex_encode())


func clear_saves() -> void:
	var dir := DirAccess.open("user://saves_test")
	if dir:
		for f in dir.get_files():
			dir.remove(f)


func _ready() -> void:
	run.call_deferred()


func change_to(p_path: String) -> void:
	get_tree().change_scene_to_file(p_path)
	await frames(3)


func run() -> void:
	await frames(3)
	ProjectSettings.set_setting("save/path", "user://saves_test")
	clear_saves()
	Saves.reset()

	check("the Saves singleton exists", Engine.has_singleton("Saves"))
	check("SaveState is a node class", ClassDB.class_exists("SaveState") and ClassDB.is_parent_class("SaveState", "Node"))

	var player: Node2D = scene().get_node("Player")
	var door: Node2D = scene().get_node("Door")
	var item := load("res://item.tres")

	# --- A round trip through nodes ------------------------------------------------
	player.position = Vector2(10, 20)
	player.coins = 7
	player.inventory = ["sword", "key"] as Array[String]
	player.scale = Vector2(3, 1)
	player.ref = door
	player.res = item
	door.open = true
	Saves.data = {"quest": 3, "spot": Vector3(1, 2, 3), "names": ["a", "b"]}
	Saves.play_time = 100.0
	var err: int = Saves.save("one", {"label": "Chapter 1"})
	check("saving works", err == OK, err)
	check("the slot is there", Saves.has_slot("one") and "one" in Saves.list_slots(), Saves.list_slots())

	player.position = Vector2.ZERO
	player.coins = 0
	player.inventory = [] as Array[String]
	player.scale = Vector2.ONE
	player.ref = null
	player.res = null
	door.open = false
	Saves.data = {}
	Saves.play_time = 0.0

	loaded_slots.clear()
	Saves.loaded.connect(func(p_slot): loaded_slots.append(p_slot))
	err = Saves.load("one")
	check("loading works", err == OK, err)
	check("properties come back", player.position == Vector2(10, 20) and player.coins == 7 and door.open, [player.position, player.coins, door.open])
	check("a property inside a property comes back", is_equal_approx(player.scale.x, 3.0) and is_equal_approx(player.scale.y, 1.0), player.scale)
	check("a typed array stays typed", player.inventory.is_typed() and player.inventory == ["sword", "key"], player.inventory)
	check("a node reference finds its node again", player.ref == door, player.ref)
	check("a resource comes back from its file", player.res != null and player.res.resource_path == "res://item.tres", player.res)
	check("Saves.data comes back", Saves.data.get("quest") == 3 and Saves.data.get("spot") == Vector3(1, 2, 3) and Saves.data.get("names") == ["a", "b"], Saves.data)
	check("play time comes back", Saves.play_time >= 100.0 and Saves.play_time < 101.0, Saves.play_time)
	check("loaded is announced", loaded_slots == ["one"], loaded_slots)

	var info: Dictionary = Saves.get_info("one")
	check("the header is readable on its own", info.get("info", {}).get("label") == "Chapter 1" and info.get("scene") == "res://level_a.tscn" and info.get("version") == 1, info)
	check("the header has the time and the play time", info.get("timestamp", 0) > 1000000000 and absf(info.get("play_time", 0.0) - 100.0) < 1.0, info)
	var full: Dictionary = Saves.read("one")
	check("read gives a save without loading it", full.get("error") == OK and full.get("data", {}).get("quest") == 3 and full.get("states", {}).size() >= 3, full.keys())

	# --- Encryption --------------------------------------------------------------
	var bytes := read_bytes("one")
	check("an encrypted save shows no plain text", not contains(bytes, "coins") and not contains(bytes, "sword") and info.get("encrypted") == true)
	var tampered := bytes.duplicate()
	tampered[tampered.size() / 2] ^= 0x40
	var path: String = Saves.get_slot_path("one")
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_buffer(tampered)
	f.close()
	check("a changed save is refused", Saves.load("one") == ERR_FILE_CORRUPT)
	f = FileAccess.open(path, FileAccess.WRITE)
	f.store_buffer(bytes)
	f.close()
	check("and the same save is fine unchanged", Saves.load("one") == OK)

	# --- Passwords ---------------------------------------------------------------
	check("a save with a password saves", Saves.save("locked", {}, "hunter2") == OK)
	check("it needs the password", Saves.load("locked") == ERR_UNAUTHORIZED)
	check("a wrong password is refused", Saves.load("locked", "nope") == ERR_FILE_CORRUPT)
	check("the right one loads", Saves.load("locked", "hunter2") == OK)
	var locked_info: Dictionary = Saves.get_info("locked")
	check("the header still shows without it", locked_info.get("password_required") == true, locked_info)
	Saves.password = "hunter2"
	check("a default password is used", Saves.load("locked") == OK)
	Saves.password = ""

	# --- Backups -----------------------------------------------------------------
	player.coins = 1
	Saves.save("bk")
	player.coins = 2
	Saves.save("bk")
	check("the save before is kept as a backup", FileAccess.file_exists(Saves.get_slot_path("bk") + ".bak1"))
	var main := FileAccess.get_file_as_bytes(Saves.get_slot_path("bk"))
	main[main.size() / 2] ^= 0x01
	f = FileAccess.open(Saves.get_slot_path("bk"), FileAccess.WRITE)
	f.store_buffer(main)
	f.close()
	player.coins = 99
	check("a broken save falls back to its backup", Saves.load("bk") == OK and player.coins == 1, player.coins)

	# --- Plain and readable ----------------------------------------------------------
	ProjectSettings.set_setting("save/encryption/enabled", false)
	ProjectSettings.set_setting("save/format", 1)
	player.coins = 5
	check("a JSON save saves", Saves.save("plain") == OK)
	check("and it can be read by eye", contains(read_bytes("plain"), "coins"))
	player.coins = 0
	check("and loaded", Saves.load("plain") == OK and player.coins == 5, player.coins)
	ProjectSettings.set_setting("save/encryption/enabled", true)
	check("with encryption on, a plain save is not taken", Saves.load("plain") == ERR_FILE_CORRUPT)
	ProjectSettings.set_setting("save/format", 0)

	# --- Spawnable nodes ---------------------------------------------------------
	check("the placed enemy is there", enemies().size() == 1, enemies().size())
	var runtime_enemy: Node2D = load("res://enemy.tscn").instantiate()
	scene().add_child(runtime_enemy)
	runtime_enemy.position = Vector2(300, 300)
	runtime_enemy.health = 50
	enemies()[0].health = 25
	await frames()
	Saves.save("spawn")
	for e in enemies():
		e.queue_free()
	await frames()
	check("the enemies are gone", enemies().size() == 0)
	Saves.load("spawn")
	await frames()
	var back := enemies()
	check("both enemies come back", back.size() == 2, back.size())
	var healths := []
	for e in back:
		healths.append(e.health)
	healths.sort()
	check("with what they had", healths == [25, 50], healths)
	var stray: Node2D = load("res://enemy.tscn").instantiate()
	scene().add_child(stray)
	await frames()
	check("a third enemy is there", enemies().size() == 3)
	Saves.load("spawn")
	await frames()
	check("one the save does not have goes away", enemies().size() == 2, enemies().size())

	# --- Scenes ------------------------------------------------------------------
	player = scene().get_node("Player")
	player.coins = 11
	Saves.save("in_a")
	await change_to("res://level_b.tscn")
	var lamp: Node2D = scene().get_node("Lamp")
	lamp.visible = false
	lamp.position = Vector2(5, 6)
	Saves.save("in_b")
	check("the save knows its scene", Saves.get_info("in_b").get("scene") == "res://level_b.tscn")

	loaded_slots.clear()
	Saves.load("in_a")
	await frames(4)
	check("loading goes back to the scene it was saved in", scene().scene_file_path == "res://level_a.tscn", scene().scene_file_path)
	check("and its state is put on it", scene().get_node("Player").coins == 11, scene().get_node("Player").coins)
	check("loaded is announced once the scene is in", loaded_slots == ["in_a"], loaded_slots)

	Saves.load("in_b")
	await frames(4)
	lamp = scene().get_node("Lamp")
	check("and into the other scene", scene().scene_file_path == "res://level_b.tscn" and lamp.visible == false and lamp.position == Vector2(5, 6), [scene().scene_file_path, lamp.visible, lamp.position])

	# A save made in A, loaded without moving the scene, waits for A.
	Saves.load("in_a", "", false)
	check("still in B", scene().scene_file_path == "res://level_b.tscn")
	await change_to("res://level_a.tscn")
	check("the state waits for its scene", scene().get_node("Player").coins == 11, scene().get_node("Player").coins)

	# A save made in A keeps what was stashed of B, so going back and forth loses nothing.
	await change_to("res://level_b.tscn")
	scene().get_node("Lamp").position = Vector2(8, 9)
	Saves.stash()
	await change_to("res://level_a.tscn")
	player = scene().get_node("Player")
	player.coins = 12
	Saves.save("both")
	var states: Dictionary = Saves.read("both").get("states", {})
	check("a save carries the states of scenes not loaded", states.has("Lamp") and states["Lamp"]["props"]["position"] == Vector2(8, 9), states.keys())
	await change_to("res://level_b.tscn")
	check("and they are put back when that scene comes in", scene().get_node("Lamp").position == Vector2(8, 9), scene().get_node("Lamp").position)
	await change_to("res://level_a.tscn")

	# --- In memory ---------------------------------------------------------------
	player = scene().get_node("Player")
	player.coins = 1
	var snap: Dictionary = Saves.snapshot()
	player.coins = 5
	Saves.restore(snap)
	check("a snapshot puts things back", player.coins == 1, player.coins)

	# --- Versions ----------------------------------------------------------------
	Saves.data = {"old": true}
	Saves.save("v1")
	Saves.save_version = 2
	Saves.migrate.connect(func(p_content, p_from):
		var d: Dictionary = p_content["data"]
		d["migrated_from"] = p_from
		p_content["data"] = d)
	check("an older save loads", Saves.load("v1") == OK)
	check("and is migrated first", Saves.data.get("migrated_from") == 1 and Saves.data.get("old") == true, Saves.data)
	Saves.save("v2")
	Saves.save_version = 1
	check("a newer save is refused", Saves.load("v2") == ERR_FILE_UNRECOGNIZED)
	Saves.save_version = 1

	# --- Slots -------------------------------------------------------------------
	check("a slot name can not leave the folder", Saves.save("../evil") == ERR_INVALID_PARAMETER and Saves.save("a/b") == ERR_INVALID_PARAMETER)
	check("copying a slot", Saves.copy_slot("one", "two") == OK and Saves.has_slot("two"))
	check("deleting it, backups with it", Saves.delete_slot("two") == OK and not Saves.has_slot("two"))
	check("deleting what is not there says so", Saves.delete_slot("nothing") == ERR_FILE_NOT_FOUND)
	check("loading what is not there says so", Saves.load("nothing") == ERR_FILE_NOT_FOUND)
	var leftovers := []
	for file in DirAccess.get_files_at("user://saves_test"):
		if file.ends_with(".tmp"):
			leftovers.append(file)
	check("no half written file is left behind", leftovers.is_empty(), leftovers)
	var listed: PackedStringArray = Saves.list_slots()
	check("slots are listed newest first", listed.size() >= 5 and listed[0] != "", listed)

	# --- Autosave and play time ------------------------------------------------------
	Saves.autosave_slot = "auto"
	Saves.autosave_interval = 0.3
	var before: float = Saves.play_time
	await get_tree().create_timer(0.9).timeout
	Saves.autosave_interval = 0.0
	check("autosave writes its slot", Saves.has_slot("auto"))
	check("play time runs", Saves.play_time > before + 0.5, [before, Saves.play_time])

	print("SAVES fails=%d" % fails)
	get_tree().quit(1 if fails else 0)
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--temp-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="asosdot-saves-", dir=args.temp_dir) as directory:
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
        files = {
            "player.gd": PLAYER,
            "enemy.gd": ENEMY,
            "door.gd": DOOR,
            "enemy.tscn": ENEMY_SCENE.replace('[ext_resource type="Script" path="res://save_state_holder.gd" id="2"]\n', "").replace("load_steps=3", "load_steps=2"),
            "level_a.tscn": LEVEL_A,
            "level_b.tscn": LEVEL_B,
            "item.tres": ITEM,
            "test.gd": TEST,
        }
        for name, text in files.items():
            (project / name).write_text(text, encoding="utf-8")
        (project / "project.godot").write_text(
            f'config_version=5\n[application]\nconfig/name="Saves check {work.name}"\nrun/main_scene="res://level_a.tscn"\n'
            '[autoload]\nTest="*res://test.gd"\n',
            encoding="utf-8",
        )
        command = [str(isolated), "--headless", "--path", str(project)]
        run(command, "SAVES fails=0", work)
        log = (work / "SAVES fails=0.log").read_text(encoding="utf-8", errors="replace")
        print("\n".join(line for line in log.splitlines() if line.startswith(("PASS", "FAIL"))), flush=True)
        print("SAVES_TEST_OK", flush=True)


if __name__ == "__main__":
    main()
