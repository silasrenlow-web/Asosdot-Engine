#!/usr/bin/env python3
"""Check that Blender bone and object constraints, imported from a .blend, act in Asosdot as in Blender."""

from __future__ import annotations

import argparse
import json
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

# One case per constraint setup. Each case has an owner bone under a parent bone
# that turns, and a target bone that moves, turns and scales, so spaces, offsets
# and mixing all show. The owner is animated too. Blender writes the evaluated
# pose of every owner for the compared frames.
FIXTURE = r"""
import bpy, json, math, sys
from mathutils import Euler, Quaternion, Vector

options = json.loads(sys.argv[sys.argv.index('--') + 1])
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.fps = 24
scene.frame_start = 0
scene.frame_end = 24

armature = bpy.data.armatures.new('Zoo')
rig = bpy.data.objects.new('Zoo', armature)
scene.collection.objects.link(rig)
bpy.context.view_layer.objects.active = rig

def setup(c, kind):
    if kind == 'loc_world': pass
    elif kind == 'loc_offset': c.use_offset = True
    elif kind == 'loc_axes': c.use_y = False; c.invert_x = True
    elif kind == 'loc_tail': c.head_tail = 1.0
    elif kind == 'loc_local': c.owner_space = c.target_space = 'LOCAL'
    elif kind == 'loc_parlocal': c.owner_space = c.target_space = 'LOCAL_WITH_PARENT'
    elif kind == 'loc_pose': c.owner_space = c.target_space = 'POSE'
    elif kind == 'loc_influence': c.influence = 0.5
    elif kind == 'rot_axes': c.use_y = False; c.invert_z = True
    elif kind == 'rot_add': c.mix_mode = 'ADD'
    elif kind == 'rot_before': c.mix_mode = 'BEFORE'
    elif kind == 'rot_after': c.mix_mode = 'AFTER'
    elif kind == 'rot_local': c.owner_space = c.target_space = 'LOCAL'
    elif kind == 'scale_offset': c.use_offset = True
    elif kind == 'scale_power': c.power = 2.0; c.use_make_uniform = True
    elif kind == 'trans_local': c.owner_space = c.target_space = 'LOCAL'
    elif kind == 'trans_after': c.mix_mode = 'AFTER'
    elif kind == 'limloc':
        c.owner_space = 'LOCAL'; c.use_min_z = c.use_max_z = c.use_max_x = True
        c.min_z, c.max_z, c.max_x = -0.05, 0.05, 0.02
    elif kind == 'limrot':
        c.owner_space = 'LOCAL'; c.use_limit_x = c.use_limit_z = True
        c.min_x, c.max_x, c.min_z, c.max_z = -0.2, 0.3, -0.1, 0.1
    elif kind == 'limscale':
        c.owner_space = 'LOCAL'; c.use_max_x = c.use_min_y = True
        c.max_x, c.min_y = 1.1, 0.95
    elif kind == 'trackto': c.track_axis = 'TRACK_Y'; c.up_axis = 'UP_Z'
    elif kind == 'lockedtrack': c.track_axis = 'TRACK_Y'; c.lock_axis = 'LOCK_Z'
    elif kind == 'follow': c.subtarget = 'damped.own'
    elif kind == 'stack': c.mix_mode = 'AFTER'
    elif kind == 'transform':
        c.map_from = 'LOCATION'; c.map_to = 'ROTATION'
        c.from_min_x, c.from_max_x = -0.2, 0.2
        c.to_min_z, c.to_max_z = -0.8, 0.8
        c.map_to_z_from = 'X'

CASES = [
    # Reads a bone that a later case constrains, so it must run after that case.
    ('follow', 'COPY_TRANSFORMS'),
    ('loc_world', 'COPY_LOCATION'), ('loc_offset', 'COPY_LOCATION'), ('loc_axes', 'COPY_LOCATION'),
    ('loc_tail', 'COPY_LOCATION'), ('loc_local', 'COPY_LOCATION'), ('loc_parlocal', 'COPY_LOCATION'),
    ('loc_pose', 'COPY_LOCATION'), ('loc_influence', 'COPY_LOCATION'),
    ('rot_world', 'COPY_ROTATION'), ('rot_axes', 'COPY_ROTATION'), ('rot_add', 'COPY_ROTATION'),
    ('rot_before', 'COPY_ROTATION'), ('rot_after', 'COPY_ROTATION'), ('rot_local', 'COPY_ROTATION'),
    ('scale_world', 'COPY_SCALE'), ('scale_offset', 'COPY_SCALE'), ('scale_power', 'COPY_SCALE'),
    ('trans_world', 'COPY_TRANSFORMS'), ('trans_local', 'COPY_TRANSFORMS'), ('trans_after', 'COPY_TRANSFORMS'),
    ('limloc', 'LIMIT_LOCATION'), ('limrot', 'LIMIT_ROTATION'), ('limscale', 'LIMIT_SCALE'),
    ('childof', 'CHILD_OF'), ('stretch', 'STRETCH_TO'), ('trackto', 'TRACK_TO'),
    ('lockedtrack', 'LOCKED_TRACK'), ('damped', 'DAMPED_TRACK'), ('transform', 'TRANSFORM'),
    ('volume', 'MAINTAIN_VOLUME'), ('stack', 'COPY_ROTATION'), ('loc_animinf', 'COPY_LOCATION'),
]

bpy.ops.object.mode_set(mode='EDIT')
edit = armature.edit_bones
root = edit.new('root')
root.head, root.tail = (0, 0, 0), (0, 0.2, 0)
root.use_deform = False
for i, (case, _) in enumerate(CASES):
    x = (i % 6) * 0.8
    z = (i // 6) * 0.8
    parent = edit.new(case + '.par')
    parent.head, parent.tail, parent.roll = (x, 0, z), (x + 0.1, 0.05, z + 0.25), 0.3
    parent.parent = root
    owner = edit.new(case + '.own')
    owner.head, owner.tail, owner.roll = (x + 0.1, 0.05, z + 0.25), (x + 0.15, 0.1, z + 0.5), -0.4
    owner.parent = parent
    target = edit.new(case + '.tgt')
    target.head, target.tail, target.roll = (x + 0.3, -0.2, z + 0.3), (x + 0.3, -0.1, z + 0.55), 0.7
    target.parent = root
    target.use_deform = False
bpy.ops.object.mode_set(mode='POSE')
pose = rig.pose.bones
empty = bpy.data.objects.new('Prop', None)
scene.collection.objects.link(empty)
empty.location = (0, 1, 0)
prop = empty.constraints.new('COPY_LOCATION')
prop.target, prop.subtarget = rig, 'loc_world.tgt'
tracker = bpy.data.objects.new('Tracker', None)
scene.collection.objects.link(tracker)
tracker.location, tracker.rotation_euler = (0.5, -1, 0.3), (0.3, 0.1, -0.2)
aim = tracker.constraints.new('DAMPED_TRACK')
aim.target, aim.subtarget, aim.track_axis = rig, 'damped.tgt', 'TRACK_NEGATIVE_Z'

for case, kind in CASES:
    c = pose[case + '.own'].constraints.new(kind)
    if hasattr(c, 'target'):
        c.target, c.subtarget = rig, case + '.tgt'
    setup(c, case)
    if kind == 'CHILD_OF':
        c.inverse_matrix = (rig.matrix_world @ pose[case + '.tgt'].matrix).inverted()
    elif kind == 'STRETCH_TO':
        c.rest_length = (pose[case + '.tgt'].head - pose[case + '.own'].head).length
    if case == 'stack':
        limit = pose[case + '.own'].constraints.new('LIMIT_ROTATION')
        limit.owner_space, limit.use_limit_y, limit.min_y, limit.max_y = 'LOCAL', True, -0.1, 0.1
    elif case == 'loc_animinf':
        # Animated influence stays baked: the node comes in switched off.
        for frame, influence in ((0, 0.2), (24, 0.9)):
            c.influence = influence
            c.keyframe_insert('influence', frame=frame)

keys = [(0, (0, 0, 0), (0, 0, 0), 1.0), (8, (0.12, -0.05, 0.08), (0.4, -0.2, 0.3), 1.2), (16, (-0.08, 0.1, -0.06), (-0.3, 0.5, -0.4), 0.85), (24, (0.05, 0.05, 0.1), (0.2, 0.1, 0.6), 1.1)]
for case, _ in CASES:
    for frame, location, rotation, scale in keys:
        t = pose[case + '.tgt']
        t.rotation_mode = 'XYZ'
        t.location, t.rotation_euler, t.scale = location, rotation, (scale, scale * 0.9, scale * 1.05)
        for path in ('location', 'rotation_euler', 'scale'):
            t.keyframe_insert(path, frame=frame)
        o = pose[case + '.own']
        o.rotation_mode = 'XYZ'
        o.location = Vector(location) * 0.3
        o.rotation_euler = Vector(rotation) * -0.5
        for path in ('location', 'rotation_euler'):
            o.keyframe_insert(path, frame=frame)
        p = pose[case + '.par']
        p.rotation_mode = 'XYZ'
        p.rotation_euler = Vector(rotation) * 0.4
        p.keyframe_insert('rotation_euler', frame=frame)
rig.animation_data.action.name = 'Zoo'
bpy.ops.object.mode_set(mode='OBJECT')

def columns(matrix):
    m = matrix.to_3x3()
    return [list(m.col[0]), list(m.col[1]), list(m.col[2])]

def shear(matrix):
    m = matrix.to_3x3()
    axes = [m.col[i].normalized() for i in range(3)]
    return max(abs(axes[0].dot(axes[1])), abs(axes[1].dot(axes[2])), abs(axes[0].dot(axes[2])))

def without_shear(matrix):
    # A bone can't shear in Asosdot, as in glTF. The nearest pose keeps the bone's Y
    # axis, as Blender's own shear removal does: X and Z leave Y, then turn apart evenly.
    m = matrix.to_3x3()
    x, y, z = m.col[0].copy(), m.col[1].copy(), m.col[2].copy()
    if y.length_squared > 0:
        x -= y * (x.dot(y) / y.length_squared)
        z -= y * (z.dot(y) / y.length_squared)
    x_length, z_length = x.length, z.length
    x, z = x.normalized(), z.normalized()
    if 1e-4 < abs(x.dot(z)) < 1 - 1.2e-7:
        middle, apart = (x + z).normalized(), (x - z).normalized()
        x, z = (middle + apart) * math.sqrt(0.5), (middle - apart) * math.sqrt(0.5)
    m.col[0], m.col[1], m.col[2] = x * x_length, y, z * z_length
    return m

reference = {'cases': [case for case, _ in CASES], 'frames': {}}
for frame in (0, 4, 8, 12, 16, 20, 24):
    scene.frame_set(frame)
    evaluated = rig.evaluated_get(bpy.context.evaluated_depsgraph_get())
    entry = {}
    for case, _ in CASES:
        b = evaluated.pose.bones[case + '.own']
        entry[case] = {'head': list(b.head), 'basis': columns(without_shear(b.matrix)), 'shear': shear(b.matrix)}
    for item in (empty, tracker):
        world = item.evaluated_get(bpy.context.evaluated_depsgraph_get()).matrix_world
        entry[item.name] = {'head': list(world.translation), 'basis': columns(world)}
    reference['frames'][str(frame)] = entry
scene.frame_set(0)
bpy.ops.wm.save_as_mainfile(filepath=options['blend'])
with open(options['reference'], 'w') as file:
    json.dump(reference, file)
print('CONSTRAINT_FIXTURE_OK')
"""

EDITOR_SETUP = r"""
@tool
extends EditorPlugin

func _enter_tree():
	call_deferred("run")

func run():
	var config = JSON.parse_string(FileAccess.get_file_as_string("res://check.json"))
	EditorInterface.get_editor_settings().set("filesystem/import/blender/blender_path", config.blender)
	EditorInterface.get_editor_settings().set("filesystem/import/blender/rpc_port", 0)
	print("SETUP_OK")
	get_tree().quit()
"""

CHECK = r"""
extends SceneTree

var reference: Dictionary
var captured := {}
var skeleton: Skeleton3D
var objects := {}
const OBJECTS = ["Prop", "Tracker"]
# Cases that must stay baked into the animation, their node switched off.
const BAKED = ["loc_animinf"]

func _initialize():
	call_deferred("run")

# Blender's armature space is Z up; the glTF export turns it Y up and keeps bone axes.
const C = Basis(Vector3(1, 0, 0), Vector3(0, 0, -1), Vector3(0, 1, 0))

func blender_vector(value: Array) -> Vector3:
	return C * Vector3(value[0], value[1], value[2])

func blender_basis(value: Array) -> Basis:
	return C * Basis(Vector3(value[0][0], value[0][1], value[0][2]), Vector3(value[1][0], value[1][1], value[1][2]), Vector3(value[2][0], value[2][1], value[2][2]))

func find_class(node: Node, type: String) -> Node:
	if node.get_class() == type:
		return node
	for child in node.get_children():
		var found = find_class(child, type)
		if found:
			return found
	return null

func capture():
	for case in reference.cases:
		captured[case] = skeleton.get_bone_global_pose(skeleton.find_bone(case + ".own"))
	for item in objects:
		captured[item] = objects[item].global_transform

func run():
	var config = JSON.parse_string(FileAccess.get_file_as_string("res://check.json"))
	reference = JSON.parse_string(FileAccess.get_file_as_string(config.reference))
	var scene = (load("res://zoo.blend") as PackedScene).instantiate()
	root.add_child(scene)
	skeleton = find_class(scene, "Skeleton3D") as Skeleton3D
	for item in OBJECTS:
		objects[item] = scene.find_child(item, true, false) as Node3D
	skeleton.skeleton_updated.connect(capture)
	var built := {}
	for child in skeleton.get_children():
		for case in reference.cases + OBJECTS:
			if String(child.name).contains(case):
				built[case] = child.get_class() + ("" if child.active else " (off)")
	var player = find_class(scene, "AnimationPlayer") as AnimationPlayer
	# The imported clip, played as imported: it must move every owner as Blender does.
	player.play(player.get_animation_list()[0])
	player.pause()
	var worst := {}
	for frame in reference.frames:
		player.seek(int(frame) / 24.0, true)
		for i in 3:
			await process_frame
		for case in reference.cases + OBJECTS:
			if not captured.has(case):
				continue
			var expected = reference.frames[frame][case]
			var pose: Transform3D = captured[case]
			var basis = blender_basis(expected.basis)
			var origin = blender_vector(expected.head)
			if case in OBJECTS:
				# An object's own axes are converted too, not only its place.
				basis = basis * C.inverse()
			var position_error = pose.origin.distance_to(origin)
			var basis_error = 0.0
			for axis in 3:
				basis_error = max(basis_error, (pose.basis[axis] - basis[axis]).length())
			var w = worst.get(case, [0.0, 0.0, 0.0])
			worst[case] = [max(w[0], position_error), max(w[1], basis_error), max(w[2], expected.get("shear", 0.0))]
	# The constraints act live, not only as baked: move a target and its owner follows.
	var target = skeleton.find_bone("loc_world.tgt")
	skeleton.set_bone_pose_position(target, skeleton.get_bone_pose_position(target) + Vector3(0.3, -0.2, 0.1))
	for i in 3:
		await process_frame
	var follows = captured["loc_world"].origin.distance_to(skeleton.get_bone_global_pose(target).origin) < 0.0001
	follows = follows and objects["Prop"].global_position.distance_to(skeleton.global_transform * skeleton.get_bone_global_pose(target).origin) < 0.0001
	print("Live: owners follow a moved target: %s" % ("yes" if follows else "NO"))
	var failed := []
	for case in reference.cases + OBJECTS:
		var w = worst.get(case, [INF, INF, 0.0])
		var expected_class = "BlenderConstraint3D (off)" if case in BAKED else "BlenderConstraint3D"
		var ok = w[0] < 0.002 and w[1] < 0.01 and built.get(case, "") == expected_class
		# Blender's pose has shear there, which no bone holds here; the check compares the pose without it.
		var note = "  (Blender shears it by %.3f)" % w[2] if w[2] > 0.001 else ""
		print("%-14s %-28s position %.4f  axes %.4f  %s%s" % [case, built.get(case, "not built"), w[0], w[1], "ok" if ok else "DIFFERS", note])
		if not ok:
			failed.append(case)
	var total = reference.cases.size() + OBJECTS.size()
	print("Matching Blender: %d of %d" % [total - failed.size(), total])
	if failed.is_empty() and follows:
		print("CONSTRAINTS_OK")
	quit()
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--blender", required=True, type=Path)
    parser.add_argument("--temp-dir", type=Path)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()
    parent = (args.temp_dir or Path(tempfile.gettempdir())).resolve()
    work = Path(tempfile.mkdtemp(prefix="asosdot-constraints-", dir=parent))
    print(f"Test workspace: {work}", flush=True)
    try:
        runtime = work / "runtime"
        runtime.mkdir()
        isolated = runtime / args.engine.name
        shutil.copy2(args.engine.resolve(), isolated)
        (runtime / "_sc_").touch()
        project = work / "project"
        project.mkdir()
        addon = project / "addons/setup"
        addon.mkdir(parents=True)
        (addon / "plugin.cfg").write_text('[plugin]\nname="Setup"\nscript="plugin.gd"\n', encoding="utf-8")
        (addon / "plugin.gd").write_text(EDITOR_SETUP, encoding="utf-8")
        project_file = project / "project.godot"
        project_file.write_text(
            'config_version=5\n[application]\nconfig/name="Constraint check"\n'
            "[debug]\nfile_logging/enable_file_logging=false\n"
            '[rendering]\nrenderer/rendering_method="gl_compatibility"\n'
            '[editor_plugins]\nenabled=PackedStringArray("res://addons/setup/plugin.cfg")\n',
            encoding="utf-8",
        )
        options = {
            "blender": str(args.blender.resolve()),
            "blend": str(work / "zoo.blend"),
            "reference": str(work / "reference.json"),
        }
        (project / "check.json").write_text(json.dumps(options), encoding="utf-8")
        (project / "check.gd").write_text(CHECK, encoding="utf-8")
        run(
            [
                options["blender"],
                "--background",
                "--factory-startup",
                "--python-exit-code",
                "1",
                "--python-expr",
                FIXTURE,
                "--",
                json.dumps(options),
            ],
            "CONSTRAINT_FIXTURE_OK",
            work,
        )
        base = [str(isolated), "--headless", "--path", str(project)]
        run(base + ["--editor"], "SETUP_OK", work)
        project_file.write_text(
            project_file.read_text(encoding="utf-8").replace(
                'enabled=PackedStringArray("res://addons/setup/plugin.cfg")', "enabled=PackedStringArray()"
            ),
            encoding="utf-8",
        )
        shutil.copy2(options["blend"], project / "zoo.blend")
        run(base + ["--editor", "--import"], None, work)
        # The import's animation optimizer drops keys within a small angle; off, the
        # check measures the constraints and not the optimizer.
        import_file = project / "zoo.blend.import"
        settings = import_file.read_text(encoding="utf-8")
        import_file.write_text(
            settings.replace(
                "_subresources={}", '_subresources={"nodes": {"PATH:AnimationPlayer": {"optimizer/enabled": false}}}'
            ),
            encoding="utf-8",
        )
        shutil.rmtree(project / ".godot/imported")
        run(base + ["--editor", "--import"], None, work)
        log = work / "CONSTRAINTS_OK.log"
        try:
            run(base + ["--script", "check.gd", "--quit-after", "900"], "CONSTRAINTS_OK", work)
        finally:
            print(
                "\n".join(
                    line
                    for line in log.read_text(encoding="utf-8", errors="replace").splitlines()
                    if "  " in line or line.startswith(("Matching", "Live"))
                ),
                flush=True,
            )
        print("BLENDER_CONSTRAINTS_OK", flush=True)
    finally:
        if not args.keep:
            # Only remove the specific directory created for this check.
            assert work.resolve().parent == parent and work.name.startswith("asosdot-constraints-")
            shutil.rmtree(work)


if __name__ == "__main__":
    main()
