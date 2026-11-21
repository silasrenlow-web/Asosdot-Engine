/**************************************************************************/
/*  anchored_pose_modifier_3d.h                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/templates/local_vector.h"
#include "scene/3d/skeleton_modifier_3d.h"
#include "scene/resources/anchored_pose.h"

class Animation;

// Keeps a stored pose (hand, elbow, fingers ...) relative to a node, and lets the pose be made by eye.
class AnchoredPoseModifier3D : public SkeletonModifier3D {
	GDCLASS(AnchoredPoseModifier3D, SkeletonModifier3D);

public:
	struct PoseEntry {
		int bone = -1;
		bool anchored = false; // Follows the anchor (the bone has no parent): `xform` is relative to the anchor.
		Vector3 position;
		Quaternion rotation;
		Vector3 scale = Vector3(1, 1, 1);
	};

private:
	NodePath anchor_node;
	bool holding = true;
	float fade_time = 0.25;
	// Bone name -> Transform3D. Bones without a parent: relative to the anchor. Other bones: the local pose.
	Dictionary pose;
	// Bones without a parent that are stored in the skeleton's space (follow the body), not the anchor.
	PackedStringArray body_bones;
	// An exchange slot to move the pose to and from a resource file (see the buttons).
	Ref<AnchoredPose> pose_resource;
	// Names this modifier for whoever wants to drive it (a PoseTarget3D finds it by this).
	StringName slot;
	bool editor_preview = true;

	// Another node's pose, used until released (see engage).
	bool engaged = false;
	bool lingering = false; // Released, still fading out.
	ObjectID engaged_anchor;
	Ref<AnchoredPose> engaged_pose;
	float engage_fade_in = -1.0;
	float engage_fade_out = -1.0;
	// Or the pose is the live bone poses of a Skeleton3D (a PoseTarget3D's pose skeleton, which can be animated).
	ObjectID engaged_proxy;
	PackedStringArray engaged_body_bones;
	// The bones this slot poses: the roots of their subtrees (for example the hand's and the elbow's IK target bones).
	PackedStringArray pose_roots;

	bool reach_assist = false;
	String reach_target_bone;
	String reach_root_bone;
	String reach_start_bone;
	String reach_middle_bone;
	String reach_tip_bone;
	float reach_comfort = 0.93;
	float reach_keep = 0.0;
	float reach_rest_gap = 0.0; // Shoulder-to-target distance of the pose at rest (measured in the editor, saved).

	bool preview_anchor = true;
	bool editing = false;

	float blend = 0.0;
	LocalVector<PoseEntry> entries;
	bool entries_dirty = true;
	uint64_t entries_version = 0;

	// Reach assist cache.
	float reach_length = 0.0;
	uint64_t reach_version = UINT64_MAX;

	// Editor only.
	struct BoneState {
		Vector3 position;
		Quaternion rotation;
		Vector3 scale = Vector3(1, 1, 1);
		bool operator==(const BoneState &p_other) const;
	};
	LocalVector<BoneState> edit_base; // The stored poses before editing started (put back afterwards, never saved).
	LocalVector<BoneState> edit_last; // The stored poses as of the last frame (to see what the user changed).
	bool edit_active = false;
	bool edit_suspended = false; // While the scene is being saved.
	Transform3D edit_anchor_last;
	ObjectID revealed_anchor;
	bool anchor_was_hidden = false;

	Node3D *_get_anchor() const;
	bool _uses_engaged() const;
	Skeleton3D *_get_proxy() const;
	bool _is_body_bone(const String &p_name) const;
	Transform3D _anchor_in_skeleton(Skeleton3D *p_skeleton, Node3D *p_anchor) const;
	void _rebuild_entries(Skeleton3D *p_skeleton);
	void _apply_pose(Skeleton3D *p_skeleton, Node3D *p_anchor, float p_weight);
	void _lean_to_reach(Skeleton3D *p_skeleton, const Vector3 &p_target, float p_weight);
	void _record_rest_gap(Skeleton3D *p_skeleton, const Vector3 &p_target);
	float _get_reach_length(Skeleton3D *p_skeleton);

	void _store_bone(Skeleton3D *p_skeleton, Node3D *p_anchor, int p_bone, const Transform3D &p_pose_or_global);

	void _mark_scene_edited();
	void _begin_editing();
	void _end_editing();
	void _write_edit_state(const LocalVector<BoneState> &p_states);
	void _resync_anchored();
	void _capture_edits(Skeleton3D *p_skeleton, Node3D *p_anchor);
	void _reveal_anchor(Node3D *p_anchor);
	void _unreveal_anchor();
	static BoneState _read_state(Skeleton3D *p_skeleton, int p_bone);
	static void _write_state(Skeleton3D *p_skeleton, int p_bone, const BoneState &p_state);

protected:
	virtual void _validate_bone_names() override;
	virtual void _skeleton_changed(Skeleton3D *p_old, Skeleton3D *p_new) override;
	virtual void _process_modification(double p_delta) override;
	void _validate_property(PropertyInfo &p_property) const;
	void _notification(int p_what);
	static void _bind_methods();

public:
	virtual PackedStringArray get_configuration_warnings() const override;
	virtual bool has_process() const override { return true; }

	void set_anchor_node(const NodePath &p_path);
	NodePath get_anchor_node() const;
	void set_holding(bool p_holding);
	bool is_holding() const;
	void set_fade_time(float p_seconds);
	float get_fade_time() const;
	float get_blend() const;

	void set_pose(const Dictionary &p_pose);
	Dictionary get_pose() const;

	void set_reach_assist(bool p_enabled);
	bool is_reach_assist() const;
	void set_reach_target_bone(const String &p_name);
	String get_reach_target_bone() const;
	void set_reach_root_bone(const String &p_name);
	String get_reach_root_bone() const;
	void set_reach_start_bone(const String &p_name);
	String get_reach_start_bone() const;
	void set_reach_middle_bone(const String &p_name);
	String get_reach_middle_bone() const;
	void set_reach_tip_bone(const String &p_name);
	String get_reach_tip_bone() const;
	void set_reach_comfort(float p_comfort);
	float get_reach_comfort() const;
	void set_reach_keep(float p_keep);
	float get_reach_keep() const;
	void set_reach_rest_gap(float p_gap);
	float get_reach_rest_gap() const;

	void set_preview_anchor(bool p_preview);
	bool is_preview_anchor() const;
	void set_editing(bool p_editing);
	bool is_editing() const;

	// Scripting API.
	void set_pose_bone(const StringName &p_bone, const Transform3D &p_transform);
	Transform3D get_pose_bone(const StringName &p_bone) const;
	bool has_pose_bone(const StringName &p_bone) const;
	void erase_pose_bone(const StringName &p_bone);
	PackedStringArray get_pose_bones() const;
	void capture_bone(const StringName &p_bone);
	void import_pose_from_animation(const Ref<Animation> &p_animation, double p_time = 0.0);
	void clear_pose();

	void set_body_bones(const PackedStringArray &p_bones);
	PackedStringArray get_body_bones() const;
	void set_pose_resource(const Ref<AnchoredPose> &p_resource);
	Ref<AnchoredPose> get_pose_resource() const;
	void set_slot(const StringName &p_slot);
	StringName get_slot() const;
	void set_editor_preview(bool p_preview);
	bool is_editor_preview() const;

	// Uses `p_pose` relative to `p_anchor` instead of the own pose, fading in over `p_fade_in` seconds
	// (negative = fade_time), until release(), which fades out over `p_fade_out`.
	void engage(Node3D *p_anchor, const Ref<AnchoredPose> &p_pose, float p_fade_in = -1.0, float p_fade_out = -1.0);
	// Same, but the pose is what the bones of `p_proxy` are right now, every frame: animate them and the pose animates.
	void engage_pose_skeleton(Node3D *p_anchor, Skeleton3D *p_proxy, const PackedStringArray &p_body_bones, float p_fade_in = -1.0, float p_fade_out = -1.0);
	void set_pose_roots(const PackedStringArray &p_roots);
	PackedStringArray get_pose_roots() const;
	void release();
	bool is_engaged() const;

	void write_pose_resource();
	void read_pose_resource();
	Callable get_write_pose_button() const;
	Callable get_read_pose_button() const;

	Callable get_clear_pose_button() const;

	AnchoredPoseModifier3D();
};
