#!/usr/bin/env python3
"""Check that a Blender IK rig, imported as a .blend, moves in Asosdot as it does in Blender."""

from __future__ import annotations

import argparse
import json
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

# A rig as riggers make it by hand: two arms with IK to hand controls and pole
# targets, the hands copying their controls' rotation, and a head that tracks a
# target. The bones have different rolls and the arms bend different ways, so
# the twist each one gets is checked too. Only the controls are animated, and
# Blender writes its own evaluated pose for the frames that are compared.
RIG_FIXTURE = r"""
import bpy, json, sys
from mathutils import Quaternion

options = json.loads(sys.argv[sys.argv.index('--') + 1])
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.fps = 24
scene.frame_start = 0
scene.frame_end = 24

armature = bpy.data.armatures.new('Rig')
rig = bpy.data.objects.new('Rig', armature)
scene.collection.objects.link(rig)
bpy.context.view_layer.objects.active = rig
bpy.ops.object.mode_set(mode='EDIT')
edit = armature.edit_bones

def bone(name, head, tail, parent=None, connect=False, roll=0.0, deform=True):
    b = edit.new(name)
    b.head, b.tail, b.roll = head, tail, roll
    if parent:
        b.parent = edit[parent]
        b.use_connect = connect
    b.use_deform = deform

def signed_angle(u, v, normal):
    angle = u.angle(v)
    return -angle if u.cross(v).angle(normal) < 1 else angle

def pole_angle(base, tip, pole):
    # The pole angle that keeps the rest pose where it is, as riggers set it.
    normal = (edit[tip].tail - edit[base].head).cross(pole - edit[base].head)
    axis = normal.cross(edit[base].tail - edit[base].head)
    return signed_angle(edit[base].x_axis, axis, edit[base].tail - edit[base].head)

bone('root', (0, 0, 0), (0, 0.2, 0), deform=False)
bone('spine', (0, 0, 1.0), (0, 0, 1.45), 'root')
bone('head', (0, 0, 1.5), (0, 0, 1.75), 'spine')
bone('upper_arm.L', (0.15, 0, 1.4), (0.45, 0.04, 1.4), 'spine', roll=0.4)
bone('forearm.L', (0.45, 0.04, 1.4), (0.75, 0, 1.4), 'upper_arm.L', True, roll=-0.3)
bone('hand.L', (0.75, 0, 1.4), (0.85, 0, 1.4), 'forearm.L', True, roll=0.1)
bone('upper_arm.R', (-0.15, 0, 1.4), (-0.45, 0, 1.33), 'spine', roll=-1.0)
bone('forearm.R', (-0.45, 0, 1.33), (-0.75, 0, 1.4), 'upper_arm.R', True, roll=0.7)
bone('hand.R', (-0.75, 0, 1.4), (-0.85, 0, 1.4), 'forearm.R', True, roll=-0.2)
bone('hand_ik.L', (0.75, 0, 1.4), (0.85, 0, 1.4), 'root', roll=0.1, deform=False)
bone('hand_ik.R', (-0.75, 0, 1.4), (-0.85, 0, 1.4), 'root', roll=-0.2, deform=False)
bone('elbow_pole.L', (0.45, 0.6, 1.4), (0.45, 0.7, 1.4), 'root', deform=False)
bone('elbow_pole.R', (-0.45, 0, 0.8), (-0.45, 0.1, 0.8), 'root', deform=False)
bone('look_target', (0, -1, 1.6), (0, -1, 1.7), 'root', deform=False)
angles = {
    'L': pole_angle('upper_arm.L', 'forearm.L', edit['elbow_pole.L'].head),
    'R': pole_angle('upper_arm.R', 'forearm.R', edit['elbow_pole.R'].head),
}
bpy.ops.object.mode_set(mode='POSE')
pose = rig.pose.bones
for side in ('L', 'R'):
    ik = pose['forearm.' + side].constraints.new('IK')
    ik.target, ik.subtarget = rig, 'hand_ik.' + side
    ik.pole_target, ik.pole_subtarget = rig, 'elbow_pole.' + side
    ik.pole_angle = angles[side]
    ik.chain_count = 2
    copy = pose['hand.' + side].constraints.new('COPY_ROTATION')
    copy.target, copy.subtarget = rig, 'hand_ik.' + side
track = pose['head'].constraints.new('DAMPED_TRACK')
track.target, track.subtarget = rig, 'look_target'

keys = {
    'hand_ik.L': [(0, (0, 0, 0), (1, 0, 0, 0)), (8, (0.05, -0.15, 0.1), Quaternion((1, 0, 0), 0.5)), (16, (-0.1, 0.05, -0.2), Quaternion((0, 0, 1), -0.6)), (24, (0, 0, 0), (1, 0, 0, 0))],
    'hand_ik.R': [(0, (0, 0, 0), (1, 0, 0, 0)), (8, (-0.1, 0.1, 0.05), Quaternion((0, 1, 0), 0.4)), (16, (0.1, -0.2, 0.1), (1, 0, 0, 0)), (24, (0, 0, 0), (1, 0, 0, 0))],
    'elbow_pole.L': [(0, (0, 0, 0), None), (12, (0.2, 0, -0.1), None), (24, (0, 0, 0), None)],
    'elbow_pole.R': [(0, (0, 0, 0), None), (12, (0, 0.2, 0.1), None), (24, (0, 0, 0), None)],
    'look_target': [(0, (0, 0, 0), None), (12, (0.5, 0, 0.3), None), (24, (-0.3, 0, 0), None)],
}
for name, frames in keys.items():
    for frame, location, rotation in frames:
        pose[name].location = location
        pose[name].keyframe_insert('location', frame=frame)
        if rotation is not None:
            pose[name].rotation_quaternion = rotation
            pose[name].keyframe_insert('rotation_quaternion', frame=frame)
rig.animation_data.action.name = 'Reach'
bpy.ops.object.mode_set(mode='OBJECT')

def columns(matrix):
    m = matrix.to_quaternion().to_matrix()
    return [list(m.col[0]), list(m.col[1]), list(m.col[2])]

checked = ['upper_arm.L', 'forearm.L', 'hand.L', 'upper_arm.R', 'forearm.R', 'hand.R', 'head']
reference = {'rest': {}, 'frames': {}}
for name in checked:
    rest = armature.bones[name]
    reference['rest'][name] = {'head': list(rest.head_local), 'basis': columns(rest.matrix_local)}
for frame in range(0, 25, 4):
    scene.frame_set(frame)
    evaluated = rig.evaluated_get(bpy.context.evaluated_depsgraph_get())
    reference['frames'][str(frame)] = {name: {'head': list(evaluated.pose.bones[name].head), 'basis': columns(evaluated.pose.bones[name].matrix)} for name in checked}
scene.frame_set(0)
bpy.ops.wm.save_as_mainfile(filepath=options['blend'])
with open(options['reference'], 'w') as file:
    json.dump(reference, file)
print('RIG_FIXTURE_OK')
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

const CHECKED = ["upper_arm.L", "forearm.L", "hand.L", "upper_arm.R", "forearm.R", "hand.R", "head"]
var reference: Dictionary
var captured := {}

func _initialize():
	call_deferred("run")

func blender_vector(value: Array) -> Vector3:
	# Blender's armature space is Z up; the glTF export turns it Y up.
	return Vector3(value[0], value[2], -value[1])

func blender_basis(value: Array) -> Basis:
	var basis = Basis(Vector3(value[0][0], value[0][1], value[0][2]), Vector3(value[1][0], value[1][1], value[1][2]), Vector3(value[2][0], value[2][1], value[2][2]))
	return Basis(Vector3(1, 0, 0), Vector3(0, 0, -1), Vector3(0, 1, 0)) * basis

func find_class(node: Node, type: String) -> Node:
	if node.get_class() == type:
		return node
	for child in node.get_children():
		var found = find_class(child, type)
		if found:
			return found
	return null

func instance() -> Node:
	var scene = (load("res://rig.blend") as PackedScene).instantiate()
	root.add_child(scene)
	return scene

# Modifiers change bones only while the skeleton updates, so the pose is read then.
func capture(skeleton: Skeleton3D):
	for name in CHECKED:
		captured[name] = skeleton.get_bone_global_pose(skeleton.find_bone(name))

func compare(label: String, scene: Node, animation: String, position_tolerance: float, angle_tolerance: float) -> void:
	var skeleton = find_class(scene, "Skeleton3D") as Skeleton3D
	var player = find_class(scene, "AnimationPlayer") as AnimationPlayer
	skeleton.skeleton_updated.connect(capture.bind(skeleton))
	# How each bone's axes here relate to Blender's, from the rest pose. The glTF
	# export keeps them, which makes these identity, but the check doesn't rely on it.
	var local_axes := {}
	for name in CHECKED:
		var rest = reference.rest[name]
		local_axes[name] = blender_basis(rest.basis).inverse() * skeleton.get_bone_global_rest(skeleton.find_bone(name)).basis.orthonormalized()
	var worst_position := 0.0
	var worst_angle := 0.0
	var worst_at := ""
	# Paused, so each frame is read at the time it was asked for.
	player.play(animation)
	player.pause()
	for frame in reference.frames:
		player.seek(int(frame) / 24.0, true)
		for i in 3:
			await process_frame
		for name in CHECKED:
			var expected = reference.frames[frame][name]
			var pose: Transform3D = captured[name]
			var position_error = pose.origin.distance_to(blender_vector(expected.head))
			var expected_basis = blender_basis(expected.basis) * local_axes[name]
			var angle_error = rad_to_deg(Quaternion(pose.basis.orthonormalized()).angle_to(Quaternion(expected_basis.orthonormalized())))
			if position_error > worst_position or angle_error > worst_angle:
				worst_at = "%s at frame %s" % [name, frame]
			worst_position = max(worst_position, position_error)
			worst_angle = max(worst_angle, angle_error)
	print("%s: worst joint position error %.6f m, worst bone rotation error %.4f degrees (%s)" % [label, worst_position, worst_angle, worst_at])
	assert(worst_position < position_tolerance, label + ": joints are not where Blender puts them")
	assert(worst_angle < angle_tolerance, label + ": bones are not turned and twisted as Blender turns them")

func run():
	var config = JSON.parse_string(FileAccess.get_file_as_string("res://check.json"))
	reference = JSON.parse_string(FileAccess.get_file_as_string(config.reference))
	var scene = instance()
	var skeleton = find_class(scene, "Skeleton3D") as Skeleton3D
	assert(skeleton != null, "The rig has no skeleton")
	var kinds := {}
	for child in skeleton.get_children():
		kinds[child.get_class()] = kinds.get(child.get_class(), 0) + 1
		if child is BlenderConstraint3D:
			assert(child.active, String(child.name) + " must act live: the import mutes it while Blender bakes the clip")
	print("Rebuilt: ", kinds)
	assert(kinds.get("TwoBoneIK3D", 0) == 2, "Both arms' IK must be rebuilt")
	# Both hands' Copy Rotation and the head's Damped Track.
	assert(kinds.get("BlenderConstraint3D", 0) == 3, "Both hands' Copy Rotation and the head's Damped Track must be rebuilt")
	assert(kinds.get("ModifierBoneTarget3D", 0) == 4, "Each IK control bone needs one target")
	var player = find_class(scene, "AnimationPlayer") as AnimationPlayer
	var clip_name = player.get_animation_list()[0]
	# The imported clip, with Blender's baked arm keys; the hands and head turn here, live.
	# It must play as Blender played it. The glTF export resamples it, so a little slack.
	await compare("Baked clip", scene, clip_name, 0.01, 1.0)
	scene.queue_free()
	# Only the controls keyed, so the arms and head come from the IK alone, as they do in Blender.
	var live_scene = instance()
	var live_player = find_class(live_scene, "AnimationPlayer") as AnimationPlayer
	var live = live_player.get_animation(clip_name).duplicate(true) as Animation
	for i in range(live.get_track_count() - 1, -1, -1):
		if String(live.track_get_path(i).get_concatenated_subnames()) in CHECKED:
			live.remove_track(i)
	var library = AnimationLibrary.new()
	library.add_animation("controls", live)
	live_player.add_animation_library("check", library)
	# Blender's IK solver iterates and lands within about a millimetre of the exact
	# answer, which is what TwoBoneIK3D computes.
	await compare("Controls only", live_scene, "check/controls", 0.002, 0.5)
	print("IK_RIG_OK")
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
    work = Path(tempfile.mkdtemp(prefix="asosdot-ik-rig-", dir=parent))
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
            'config_version=5\n[application]\nconfig/name="IK rig check"\n'
            "[debug]\nfile_logging/enable_file_logging=false\n"
            '[rendering]\nrenderer/rendering_method="gl_compatibility"\n'
            '[editor_plugins]\nenabled=PackedStringArray("res://addons/setup/plugin.cfg")\n',
            encoding="utf-8",
        )
        options = {
            "blender": str(args.blender.resolve()),
            "blend": str(work / "rig.blend"),
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
                RIG_FIXTURE,
                "--",
                json.dumps(options),
            ],
            "RIG_FIXTURE_OK",
            work,
        )
        base = [str(isolated), "--headless", "--path", str(project)]
        # The Blender path is set before the .blend is in the project, so its first import uses Blender.
        run(base + ["--editor"], "SETUP_OK", work)
        project_file.write_text(
            project_file.read_text(encoding="utf-8").replace(
                'enabled=PackedStringArray("res://addons/setup/plugin.cfg")', "enabled=PackedStringArray()"
            ),
            encoding="utf-8",
        )
        shutil.copy2(options["blend"], project / "rig.blend")
        run(base + ["--editor", "--import"], None, work)
        # The import's animation optimizer drops keys within a small angle, the
        # controls' included. Off, the check measures the rig and not the optimizer.
        import_file = project / "rig.blend.import"
        settings = import_file.read_text(encoding="utf-8")
        assert "_subresources={}" in settings
        import_file.write_text(
            settings.replace(
                "_subresources={}", '_subresources={"nodes": {"PATH:AnimationPlayer": {"optimizer/enabled": false}}}'
            ),
            encoding="utf-8",
        )
        shutil.rmtree(project / ".godot/imported")
        run(base + ["--editor", "--import"], None, work)
        run(base + ["--script", "check.gd", "--quit-after", "600"], "IK_RIG_OK", work)
        print(
            (work / "IK_RIG_OK.log").read_text(encoding="utf-8", errors="replace").strip().splitlines()[-4:], flush=True
        )
        print("BLENDER_IK_RIG_OK", flush=True)
    finally:
        if not args.keep:
            # Only remove the specific directory created for this check.
            assert work.resolve().parent == parent and work.name.startswith("asosdot-ik-rig-")
            shutil.rmtree(work)


if __name__ == "__main__":
    main()
