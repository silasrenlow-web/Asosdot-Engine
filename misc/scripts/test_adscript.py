#!/usr/bin/env python3
"""Check what Asosdot adds to scripting (ADScript), headless. Node.find_node(),
Node.get_node_safe() and Node.wait(): a name found anywhere in the scene, nearest
the top first, then in autoloads; paths and names that don't lead anywhere giving
null without an error; waits that call back, resume, stop for a pause and do
nothing at all once their node is gone; and a script with its own wait(),
find_node() and get_node_safe() still compiling and running its own. And the name:
messages say ADScript while everything code looks the language up by stays
GDScript."""

from __future__ import annotations

import argparse
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

AUTOLOAD = """extends Node

func _enter_tree() -> void:
	for child_name in ["Volume", "Shared"]:
		var child := Node.new()
		child.name = child_name
		add_child(child)
"""

# Methods of its own by the names Node now has, with other signatures, untyped,
# the way projects written for Godot have them.
OWN_METHODS = """extends Node

var calls := []

func wait(t):
	calls.append("wait")
	return t * 2

func find_node(a, b = 0):
	return "own find %s %s" % [a, b]

func get_node_safe() -> String:
	return "own safe"

func wait_from_inside():
	return wait(5)
"""

# Does its waiting in a node that is freed, or taken out of the tree, part way.
WAITER = """extends Node

func start() -> void:
	wait(0.2, func(): get_parent().callbacks += 1)
	await wait(0.2)
	get_parent().resumed += 1
"""

TEST = r"""
extends Node

const OwnMethods = preload("res://own_methods.gd")

var fails := 0
var callbacks := 0
var resumed := 0
var hits := 0
var done_pausable := false
var done_always := false


func check(p_name: String, p_ok: bool, p_detail: Variant = "") -> void:
	print(("PASS " if p_ok else "FAIL ") + p_name + "  " + str(p_detail))
	if not p_ok:
		fails += 1


func make(p_parent: Node, p_name: String) -> Node:
	var node := Node.new()
	node.name = p_name
	p_parent.add_child(node)
	return node


func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	run.call_deferred()


func run() -> void:
	# Built in code, so none of it has an owner: find_child() with its default of
	# owned nodes only finds none of it.
	var level := make(self, "Level")
	var enemies := make(level, "Enemies")
	var pack := make(enemies, "Pack")
	var decoy := make(pack, "Player")
	var actors := make(level, "Actors")
	var player := make(actors, "Player")
	var camera := make(player, "Camera")
	var hud := make(self, "HUD")
	var score := make(hud, "Score")
	var shared := make(hud, "Shared")

	# find_node()
	check("finds a node anywhere in the scene", find_node("Camera") == camera)
	check("finds the one nearest the top, not the first in tree order", find_node("Player") == player and find_child("Player", true, false) == decoy)
	check("finds nodes without an owner, which find_child() doesn't", find_child("Player") == null and find_node("Player") != null)
	check("searches the whole scene from anywhere in it", score.find_node("Player") == player and camera.find_node("Score") == score)
	check("finds the scene's root too", find_node(name) == self)
	check("returns null for a name that isn't there", find_node("Nobody") == null and find_node("") == null)
	check("looks in autoloads when the scene has none", find_node("Volume") != null and find_node("Volume").get_parent().name == "Helpers")
	check("prefers the scene over an autoload", find_node("Shared") == shared)

	# find_node(name, true): only inside this node.
	check("inside a node, finds the nearest of its own", level.find_node("Player", true) == player and actors.find_node("Camera", true) == camera)
	check("inside a node, doesn't look outside it", hud.find_node("Player", true) == null and hud.find_node("Score", true) == score)
	check("inside a node, the node itself doesn't count", player.find_node("Player", true) == null and level.find_node("Level", true) == null)
	check("inside a node, a deeper one is found when it is the only one", enemies.find_node("Player", true) == decoy)
	check("inside a node, the autoloads aren't searched", find_node("Volume", true) == null)
	var detached := Node.new()
	var inner := make(detached, "Inner")
	check("inside a node, works out of the tree too", detached.find_node("Inner", true) == inner)
	detached.free()

	# get_node_safe()
	check("follows a relative path", get_node_safe("HUD/Score") == score)
	check("finds a bare name anywhere", get_node_safe("Player") == player)
	check("finds the first name of a path anywhere and follows the rest", hud.get_node_safe("Actors/Player/Camera") == camera)
	check("follows an absolute path", get_node_safe(str(hud.get_path())) == hud)
	score.owner = self
	score.unique_name_in_owner = true
	check("follows a unique name", get_node_safe("%Score") == score)
	check("returns null for paths that lead nowhere", get_node_safe("Nobody/Thing") == null and get_node_safe("") == null and get_node_safe("../../../../Nobody") == null and get_node_safe("/root/Nobody") == null)
	var loose := Node.new()
	check("returns null for an absolute path outside the tree", loose.get_node_safe("/root") == null)
	loose.free()

	# wait()
	var started := Time.get_ticks_msec()
	await wait(0.3)
	var waited := Time.get_ticks_msec() - started
	# Timers count game time, which runs a little apart from the clock headless.
	check("await wait() resumes after the time", waited >= 200 and waited < 1000, waited)

	wait(0.1, func(): hits += 1)
	check("the callback isn't called straight away", hits == 0)
	await wait(0.25)
	check("the callback is called when the time is up", hits == 1, hits)

	var freed := Node.new()
	freed.set_script(load("res://waiter.gd"))
	add_child(freed)
	freed.start()
	var removed := Node.new()
	removed.set_script(load("res://waiter.gd"))
	add_child(removed)
	removed.start()
	await wait(0.05)
	freed.free()
	remove_child(removed)
	await wait(0.4)
	check("a freed or removed node's wait neither calls back nor resumes", callbacks == 0 and resumed == 0, [callbacks, resumed])
	removed.free()

	var pausable := make(self, "Pausable")
	pausable.process_mode = Node.PROCESS_MODE_PAUSABLE
	var always := make(self, "Always")
	always.process_mode = Node.PROCESS_MODE_ALWAYS
	pausable.wait(0.2, func(): done_pausable = true)
	always.wait(0.2, func(): done_always = true)
	get_tree().paused = true
	await get_tree().create_timer(0.4).timeout
	check("a wait stops while its node is paused", not done_pausable)
	check("a wait goes on for a node that runs while paused", done_always)
	get_tree().paused = false
	await get_tree().create_timer(0.4).timeout
	check("a paused wait finishes once the game goes on", done_pausable)

	# A project's own methods by these names: typed as its script, which the
	# compiler would otherwise turn into a direct call to Node's, and typed as Node.
	var own: OwnMethods = OwnMethods.new()
	add_child(own)
	check("a script's own wait() compiles and is the one called", own.wait(3) == 6 and own.calls == ["wait"], own.calls)
	check("and from inside the script", own.wait_from_inside() == 10)
	check("a script's own find_node() and get_node_safe() are the ones called", own.find_node("x") == "own find x 0" and own.get_node_safe() == "own safe")
	var as_node: Node = own
	var from_node = as_node.wait(4)
	# Typed as Node, the argument is passed as Node's float, so it comes back 8.0.
	check("and through a variable typed as Node", typeof(from_node) != TYPE_SIGNAL and from_node == 8, from_node)

	# The name: what people read says ADScript, what code finds it by says GDScript.
	var traces := Engine.capture_script_backtraces()
	var trace: ScriptBacktrace = traces[0] if not traces.is_empty() else null
	check("a backtrace is headed ADScript", trace != null and trace.format().begins_with("ADScript backtrace"), trace.format().get_slice("\n", 0) if trace else "none")
	check("but the language is still found by the name GDScript", trace != null and trace.get_language_name() == "GDScript")
	var own_script: Script = OwnMethods
	check("and scripts are still GDScript resources", own_script.get_class() == "GDScript" and own_script is GDScript)

	print("ADSCRIPT fails=%d" % fails)
	get_tree().quit(1 if fails else 0)
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--temp-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="asosdot-adscript-", dir=args.temp_dir) as directory:
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
            "test.gd": TEST,
            "autoload.gd": AUTOLOAD,
            "own_methods.gd": OWN_METHODS,
            "waiter.gd": WAITER,
            "main.tscn": '[gd_scene load_steps=2 format=3]\n\n[ext_resource type="Script" path="res://test.gd" id="1"]\n\n'
            '[node name="Main" type="Node"]\nscript = ExtResource("1")\n',
        }
        for name, text in files.items():
            (project / name).write_text(text, encoding="utf-8")
        (project / "project.godot").write_text(
            f'config_version=5\n[application]\nconfig/name="ADScript {work.name}"\nrun/main_scene="res://main.tscn"\n'
            '[autoload]\nHelpers="*res://autoload.gd"\n',
            encoding="utf-8",
        )
        command = [str(isolated), "--headless", "--path", str(project)]
        run(command, "ADSCRIPT fails=0", work)
        log = (work / "ADSCRIPT fails=0.log").read_text(encoding="utf-8", errors="replace")
        # Nothing these did should have printed an error: not a missing node, not a
        # function resumed in a node that's gone.
        for complaint in ("Node not found", "class instance is gone", "after await", "ERROR"):
            if complaint in log:
                raise RuntimeError(f"Unexpected '{complaint}' in the output:\n{log}")
        print("\n".join(line for line in log.splitlines() if line.startswith(("PASS", "FAIL"))), flush=True)
        print("ADSCRIPT_TEST_OK", flush=True)


if __name__ == "__main__":
    main()
