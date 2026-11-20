/**************************************************************************/
/*  anchored_pose_modifier_3d.cpp                                         */
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

#include "anchored_pose_modifier_3d.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/resources/animation.h"

bool AnchoredPoseModifier3D::BoneState::operator==(const BoneState &p_other) const {
	return position.is_equal_approx(p_other.position) && rotation.is_equal_approx(p_other.rotation) && scale.is_equal_approx(p_other.scale);
}

// ---------------------------------------------------------------- helpers

bool AnchoredPoseModifier3D::_uses_engaged() const {
	return (engaged || lingering) && engaged_pose.is_valid();
}

bool AnchoredPoseModifier3D::_is_body_bone(const String &p_name) const {
	const PackedStringArray list = _uses_engaged() ? engaged_pose->get_body_bones() : body_bones;
	return list.has(p_name);
}

Node3D *AnchoredPoseModifier3D::_get_anchor() const {
	if (engaged || lingering) {
		return ObjectDB::get_instance<Node3D>(engaged_anchor);
	}
	if (anchor_node.is_empty() || !is_inside_tree()) {
		return nullptr;
	}
	return Object::cast_to<Node3D>(get_node_or_null(anchor_node));
}

// The anchor in the skeleton's space, without any scale (the skeleton or the anchor may be scaled).
Transform3D AnchoredPoseModifier3D::_anchor_in_skeleton(Skeleton3D *p_skeleton, Node3D *p_anchor) const {
	const Transform3D to_skeleton = p_skeleton->get_global_transform().affine_inverse();
	const Transform3D world = p_anchor->get_global_transform();
	return Transform3D((to_skeleton.basis * world.basis.orthonormalized()).orthonormalized(), to_skeleton.xform(world.origin));
}

AnchoredPoseModifier3D::BoneState AnchoredPoseModifier3D::_read_state(Skeleton3D *p_skeleton, int p_bone) {
	BoneState state;
	state.position = p_skeleton->get_bone_pose_position(p_bone);
	state.rotation = p_skeleton->get_bone_pose_rotation(p_bone);
	state.scale = p_skeleton->get_bone_pose_scale(p_bone);
	return state;
}

void AnchoredPoseModifier3D::_write_state(Skeleton3D *p_skeleton, int p_bone, const BoneState &p_state) {
	p_skeleton->set_bone_pose_position(p_bone, p_state.position);
	p_skeleton->set_bone_pose_rotation(p_bone, p_state.rotation);
	p_skeleton->set_bone_pose_scale(p_bone, p_state.scale);
}

void AnchoredPoseModifier3D::_rebuild_entries(Skeleton3D *p_skeleton) {
	entries.clear();
	entries_dirty = false;
	entries_version = p_skeleton->get_version();
	const Dictionary source = _uses_engaged() ? engaged_pose->get_bones() : pose;
	for (const Variant &key : source.get_key_list()) {
		const String name = key;
		const int bone = p_skeleton->find_bone(name);
		if (bone < 0 || source[key].get_type() != Variant::TRANSFORM3D) {
			continue;
		}
		const Transform3D xform = source[key];
		PoseEntry entry;
		entry.bone = bone;
		entry.anchored = p_skeleton->get_bone_parent(bone) < 0 && !_is_body_bone(name);
		entry.position = xform.origin;
		entry.rotation = xform.basis.get_rotation_quaternion().normalized();
		entry.scale = xform.basis.get_scale();
		entries.push_back(entry);
	}
}

// ---------------------------------------------------------------- applying the pose

void AnchoredPoseModifier3D::_apply_pose(Skeleton3D *p_skeleton, Node3D *p_anchor, float p_weight) {
	Transform3D anchor_transform;
	Quaternion anchor_rotation;
	if (p_anchor) {
		anchor_transform = _anchor_in_skeleton(p_skeleton, p_anchor);
		anchor_rotation = anchor_transform.basis.get_rotation_quaternion();
	}
	const int target_bone = reach_assist ? p_skeleton->find_bone(reach_target_bone) : -1;
	Vector3 reach_target;
	bool have_target = false;

	for (const PoseEntry &entry : entries) {
		Vector3 position = entry.position;
		Quaternion rotation = entry.rotation;
		if (entry.anchored) {
			if (!p_anchor) {
				continue;
			}
			position = anchor_transform.xform(entry.position);
			rotation = anchor_rotation * entry.rotation;
		}
		p_skeleton->set_bone_pose_position(entry.bone, p_skeleton->get_bone_pose_position(entry.bone).lerp(position, p_weight));
		p_skeleton->set_bone_pose_rotation(entry.bone, p_skeleton->get_bone_pose_rotation(entry.bone).slerp(rotation, p_weight));
		p_skeleton->set_bone_pose_scale(entry.bone, p_skeleton->get_bone_pose_scale(entry.bone).lerp(entry.scale, p_weight));
		if (entry.anchored && entry.bone == target_bone) {
			reach_target = position;
			have_target = true;
		}
	}
	if (have_target) {
		_lean_to_reach(p_skeleton, reach_target, p_weight);
	}
}

float AnchoredPoseModifier3D::_get_reach_length(Skeleton3D *p_skeleton) {
	if (reach_version == p_skeleton->get_version()) {
		return reach_length;
	}
	reach_version = p_skeleton->get_version();
	reach_length = 0.0;
	const int start = p_skeleton->find_bone(reach_start_bone);
	const int middle = p_skeleton->find_bone(reach_middle_bone);
	int bone = p_skeleton->find_bone(reach_tip_bone);
	if (start < 0 || bone < 0) {
		return 0.0;
	}
	float length = 0.0;
	if (middle >= 0) {
		// Shoulder to elbow to wrist, as the rest pose has them (the wrist may hang on an IK target bone, so
		// walking up the parents would not find the shoulder).
		length = p_skeleton->get_bone_global_rest(start).origin.distance_to(p_skeleton->get_bone_global_rest(middle).origin) + p_skeleton->get_bone_global_rest(middle).origin.distance_to(p_skeleton->get_bone_global_rest(bone).origin);
	} else {
		while (bone != start) {
			const int parent = p_skeleton->get_bone_parent(bone);
			if (parent < 0) {
				return 0.0; // The start bone is not above the tip bone.
			}
			length += p_skeleton->get_bone_global_rest(bone).origin.distance_to(p_skeleton->get_bone_global_rest(parent).origin);
			bone = parent;
		}
	}
	reach_length = length;
	return reach_length;
}

// Editor only: remembers how far the target is from the shoulder when the pose is at rest (nothing
// is swaying the anchor), so the game can keep that distance (see reach_keep).
void AnchoredPoseModifier3D::_record_rest_gap(Skeleton3D *p_skeleton, const Vector3 &p_target) {
#ifdef TOOLS_ENABLED
	if (!Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	const int start = p_skeleton->find_bone(reach_start_bone);
	if (start < 0) {
		return;
	}
	const float gap = p_skeleton->get_bone_global_pose(start).origin.distance_to(p_target);
	if (!Math::is_equal_approx(gap, reach_rest_gap)) {
		reach_rest_gap = gap;
	}
#endif // TOOLS_ENABLED
}

// A target further than the limb can reach: the root bone leans towards it, so the hand still gets there
// and the elbow keeps a slight bend. With reach_keep, a target that comes closer than it rests also moves
// the root bone, the other way, so the elbow keeps its bend when the anchor sways sideways.
void AnchoredPoseModifier3D::_lean_to_reach(Skeleton3D *p_skeleton, const Vector3 &p_target, float p_weight) {
	const int root = p_skeleton->find_bone(reach_root_bone);
	const int start = p_skeleton->find_bone(reach_start_bone);
	const float length = _get_reach_length(p_skeleton);
	if (root < 0 || start < 0 || length <= 0.0) {
		return;
	}
	_record_rest_gap(p_skeleton, p_target);
	const Vector3 shoulder = p_skeleton->get_bone_global_pose(start).origin;
	const float gap = shoulder.distance_to(p_target);
	const float allowed = length * reach_comfort;
	float amount = gap > allowed ? gap - allowed : 0.0f;
	if (reach_keep > 0.0f && reach_rest_gap > 0.0f) {
		const float keep = (gap - reach_rest_gap) * reach_keep;
		amount = gap > allowed ? MAX(amount, keep) : keep;
	}
	if (Math::is_zero_approx(amount)) {
		return;
	}
	// The root bone's pose is in its parent's space.
	Vector3 lean = (p_target - shoulder).normalized() * amount * p_weight;
	const int parent = p_skeleton->get_bone_parent(root);
	if (parent >= 0) {
		lean = p_skeleton->get_bone_global_pose(parent).basis.inverse().xform(lean);
	}
	p_skeleton->set_bone_pose_position(root, p_skeleton->get_bone_pose_position(root) + lean);
}

void AnchoredPoseModifier3D::_process_modification(double p_delta) {
	Skeleton3D *skeleton = get_skeleton();
	if (!skeleton) {
		return;
	}
	if ((engaged || lingering) && !ObjectDB::get_instance<Node3D>(engaged_anchor)) {
		// The node whose pose this was is gone.
		engaged = false;
		lingering = false;
		engaged_pose.unref();
		entries_dirty = true;
	}
	if (entries_dirty || entries_version != skeleton->get_version() || (_uses_engaged() && Engine::get_singleton()->is_editor_hint())) {
		_rebuild_entries(skeleton);
	}
	Node3D *anchor = _get_anchor();

	bool in_editor = false;
#ifdef TOOLS_ENABLED
	if (Engine::get_singleton()->is_editor_hint()) {
		in_editor = true;
		if (preview_anchor && anchor) {
			_reveal_anchor(anchor);
		} else {
			_unreveal_anchor();
		}
		if (edit_active && !edit_suspended) {
			if (reach_assist) {
				const int target_bone = skeleton->find_bone(reach_target_bone);
				if (target_bone >= 0 && skeleton->get_bone_parent(target_bone) < 0) {
					_record_rest_gap(skeleton, skeleton->get_bone_pose_position(target_bone));
				}
			}
			// The bones are being posed by hand: only watch what changes, change nothing.
			_capture_edits(skeleton, anchor);
			return;
		}
		// The editor shows the hold at once (what is being posed has to be seen), unless it is told not to.
		blend = (editor_preview || engaged || holding) && !lingering ? 1.0f : 0.0f;
	}
#endif // TOOLS_ENABLED
	if (!in_editor) {
		const float wanted = engaged ? 1.0f : (lingering ? 0.0f : (holding ? 1.0f : 0.0f));
		float seconds = fade_time;
		if (wanted > blend && engaged && engage_fade_in >= 0.0f) {
			seconds = engage_fade_in;
		} else if (wanted < blend && lingering && engage_fade_out >= 0.0f) {
			seconds = engage_fade_out;
		}
		blend = Math::move_toward(blend, wanted, seconds > 0.0f ? (float)p_delta / seconds : 1.0f);
		if (lingering && blend <= 0.0f) {
			lingering = false;
			engaged_pose.unref();
			entries_dirty = true;
		}
	}
	if (blend <= 0.0f) {
		return;
	}
	_apply_pose(skeleton, anchor, Math::smoothstep(0.0f, 1.0f, blend));
}

// ---------------------------------------------------------------- storing

// Stores a bone's pose. A bone without a parent follows the anchor, so its pose (which is in the skeleton's
// space) is stored relative to the anchor. Any other bone: its local pose.
void AnchoredPoseModifier3D::_store_bone(Skeleton3D *p_skeleton, Node3D *p_anchor, int p_bone, const Transform3D &p_pose_or_global) {
	Transform3D stored = p_pose_or_global;
	if (p_skeleton->get_bone_parent(p_bone) < 0 && !body_bones.has(p_skeleton->get_bone_name(p_bone))) {
		if (!p_anchor) {
			WARN_PRINT(vformat("AnchoredPoseModifier3D: bone '%s' follows the anchor, but there is no anchor_node. Not stored.", p_skeleton->get_bone_name(p_bone)));
			return;
		}
		stored = _anchor_in_skeleton(p_skeleton, p_anchor).affine_inverse() * p_pose_or_global;
	}
	pose[p_skeleton->get_bone_name(p_bone)] = stored;
	entries_dirty = true;
}

void AnchoredPoseModifier3D::set_pose_bone(const StringName &p_bone, const Transform3D &p_transform) {
	Skeleton3D *skeleton = get_skeleton();
	ERR_FAIL_NULL_MSG(skeleton, "AnchoredPoseModifier3D must be a child of a Skeleton3D.");
	const int bone = skeleton->find_bone(p_bone);
	ERR_FAIL_COND_MSG(bone < 0, vformat("Bone '%s' not found.", p_bone));
	_store_bone(skeleton, _get_anchor(), bone, p_transform);
}

Transform3D AnchoredPoseModifier3D::get_pose_bone(const StringName &p_bone) const {
	const String name = p_bone;
	ERR_FAIL_COND_V_MSG(!pose.has(name), Transform3D(), vformat("Bone '%s' is not in the pose.", p_bone));
	return pose[name];
}

bool AnchoredPoseModifier3D::has_pose_bone(const StringName &p_bone) const {
	return pose.has(String(p_bone));
}

void AnchoredPoseModifier3D::erase_pose_bone(const StringName &p_bone) {
	pose.erase(String(p_bone));
	entries_dirty = true;
}

PackedStringArray AnchoredPoseModifier3D::get_pose_bones() const {
	PackedStringArray names;
	for (const Variant &key : pose.get_key_list()) {
		names.push_back(key);
	}
	return names;
}

void AnchoredPoseModifier3D::capture_bone(const StringName &p_bone) {
	Skeleton3D *skeleton = get_skeleton();
	ERR_FAIL_NULL_MSG(skeleton, "AnchoredPoseModifier3D must be a child of a Skeleton3D.");
	const int bone = skeleton->find_bone(p_bone);
	ERR_FAIL_COND_MSG(bone < 0, vformat("Bone '%s' not found.", p_bone));
	_store_bone(skeleton, _get_anchor(), bone, skeleton->get_bone_pose(bone));
}

void AnchoredPoseModifier3D::import_pose_from_animation(const Ref<Animation> &p_animation, double p_time) {
	ERR_FAIL_COND(p_animation.is_null());
	Skeleton3D *skeleton = get_skeleton();
	ERR_FAIL_NULL_MSG(skeleton, "AnchoredPoseModifier3D must be a child of a Skeleton3D.");
	HashMap<int, BoneState> found;
	for (int track = 0; track < p_animation->get_track_count(); track++) {
		const NodePath path = p_animation->track_get_path(track);
		if (path.get_subname_count() == 0 || p_animation->track_get_key_count(track) == 0) {
			continue;
		}
		const int bone = skeleton->find_bone(path.get_subname(0));
		if (bone < 0) {
			continue;
		}
		if (!found.has(bone)) {
			found[bone] = _read_state(skeleton, bone);
		}
		BoneState &state = found[bone];
		switch (p_animation->track_get_type(track)) {
			case Animation::TYPE_POSITION_3D: {
				state.position = p_animation->position_track_interpolate(track, p_time);
			} break;
			case Animation::TYPE_ROTATION_3D: {
				state.rotation = p_animation->rotation_track_interpolate(track, p_time);
			} break;
			case Animation::TYPE_SCALE_3D: {
				state.scale = p_animation->scale_track_interpolate(track, p_time);
			} break;
			default: {
			} break;
		}
	}
	Node3D *anchor = _get_anchor();
	for (const KeyValue<int, BoneState> &entry : found) {
		const Transform3D local(Basis(entry.value.rotation).scaled(entry.value.scale), entry.value.position);
		_store_bone(skeleton, anchor, entry.key, local);
	}
}

void AnchoredPoseModifier3D::clear_pose() {
	pose.clear();
	entries_dirty = true;
	if (edit_active) {
		// Back to the unposed skeleton while editing.
		Skeleton3D *skeleton = get_skeleton();
		if (skeleton && edit_base.size() == (uint32_t)skeleton->get_bone_count()) {
			_write_edit_state(edit_base);
			edit_last = edit_base;
		}
	}
}

// ---------------------------------------------------------------- another node's pose

void AnchoredPoseModifier3D::engage(Node3D *p_anchor, const Ref<AnchoredPose> &p_pose, float p_fade_in, float p_fade_out) {
	ERR_FAIL_NULL(p_anchor);
	ERR_FAIL_COND(p_pose.is_null());
	if (edit_active) {
		WARN_PRINT("AnchoredPoseModifier3D: can not take another pose while editing.");
		return;
	}
	engaged = true;
	lingering = false;
	engaged_anchor = p_anchor->get_instance_id();
	engaged_pose = p_pose;
	engage_fade_in = p_fade_in;
	engage_fade_out = p_fade_out;
	entries_dirty = true;
}

void AnchoredPoseModifier3D::release() {
	if (!engaged) {
		return;
	}
	engaged = false;
	lingering = true; // Keeps the pose and the anchor until it has faded out.
}

bool AnchoredPoseModifier3D::is_engaged() const {
	return engaged;
}

void AnchoredPoseModifier3D::set_body_bones(const PackedStringArray &p_bones) {
	body_bones = p_bones;
	entries_dirty = true;
}

PackedStringArray AnchoredPoseModifier3D::get_body_bones() const {
	return body_bones;
}

void AnchoredPoseModifier3D::set_pose_resource(const Ref<AnchoredPose> &p_resource) {
	pose_resource = p_resource;
}

Ref<AnchoredPose> AnchoredPoseModifier3D::get_pose_resource() const {
	return pose_resource;
}

void AnchoredPoseModifier3D::set_slot(const StringName &p_slot) {
	slot = p_slot;
}

StringName AnchoredPoseModifier3D::get_slot() const {
	return slot;
}

void AnchoredPoseModifier3D::set_editor_preview(bool p_preview) {
	editor_preview = p_preview;
}

bool AnchoredPoseModifier3D::is_editor_preview() const {
	return editor_preview;
}

// Copies the pose into the resource (made if there is none), to save it as a file and use it elsewhere.
void AnchoredPoseModifier3D::write_pose_resource() {
	if (pose_resource.is_null()) {
		pose_resource.instantiate();
	}
	pose_resource->set_bones(pose);
	pose_resource->set_body_bones(body_bones);
	notify_property_list_changed();
}

// Takes the pose from the resource.
void AnchoredPoseModifier3D::read_pose_resource() {
	ERR_FAIL_COND_MSG(pose_resource.is_null(), "Assign a pose_resource first.");
	pose = pose_resource->get_bones();
	body_bones = pose_resource->get_body_bones();
	entries_dirty = true;
}

Callable AnchoredPoseModifier3D::get_write_pose_button() const {
	return callable_mp(const_cast<AnchoredPoseModifier3D *>(this), &AnchoredPoseModifier3D::write_pose_resource);
}

Callable AnchoredPoseModifier3D::get_read_pose_button() const {
	return callable_mp(const_cast<AnchoredPoseModifier3D *>(this), &AnchoredPoseModifier3D::read_pose_resource);
}

// ---------------------------------------------------------------- editing by hand (editor only)

void AnchoredPoseModifier3D::_write_edit_state(const LocalVector<BoneState> &p_states) {
	Skeleton3D *skeleton = get_skeleton();
	if (!skeleton || p_states.size() != (uint32_t)skeleton->get_bone_count()) {
		return;
	}
	for (uint32_t i = 0; i < p_states.size(); i++) {
		_write_state(skeleton, i, p_states[i]);
	}
}

// The stored bone poses are what the Skeleton3D inspector, the pose tools and "insert key" show, and a
// modifier never changes them (it only changes what is drawn). So while editing, the held pose is
// written into the stored poses, the normal tools work on it, and what they change is picked up and
// kept in `pose`. The stored poses are put back when editing ends and while the scene is saved.
void AnchoredPoseModifier3D::_begin_editing() {
	Skeleton3D *skeleton = get_skeleton();
	if (!skeleton || !is_inside_tree() || edit_active) {
		return;
	}
	const int count = skeleton->get_bone_count();
	edit_base.resize(count);
	for (int i = 0; i < count; i++) {
		edit_base[i] = _read_state(skeleton, i);
	}
	edit_last = edit_base;
	_rebuild_entries(skeleton);

	Node3D *anchor = _get_anchor();
	Transform3D anchor_transform;
	if (anchor) {
		anchor_transform = _anchor_in_skeleton(skeleton, anchor);
		edit_anchor_last = anchor_transform;
	}
	for (const PoseEntry &entry : entries) {
		BoneState state;
		state.position = entry.position;
		state.rotation = entry.rotation;
		state.scale = entry.scale;
		if (entry.anchored) {
			if (!anchor) {
				continue;
			}
			state.position = anchor_transform.xform(entry.position);
			state.rotation = anchor_transform.basis.get_rotation_quaternion() * entry.rotation;
		}
		_write_state(skeleton, entry.bone, state);
		edit_last[entry.bone] = state;
	}
	edit_active = true;
	edit_suspended = false;
}

void AnchoredPoseModifier3D::_end_editing() {
	if (!edit_active) {
		return;
	}
	_write_edit_state(edit_base);
	edit_active = false;
	edit_suspended = false;
	edit_base.clear();
	edit_last.clear();
}

// Called while the skeleton is updated: sees which bones the user changed since the last frame.
void AnchoredPoseModifier3D::_capture_edits(Skeleton3D *p_skeleton, Node3D *p_anchor) {
	if (edit_last.size() != (uint32_t)p_skeleton->get_bone_count()) {
		edit_active = false; // The skeleton was rebuilt under us.
		return;
	}
	bool changed = false;
	for (int i = 0; i < p_skeleton->get_bone_count(); i++) {
		const BoneState now = _read_state(p_skeleton, i);
		if (now == edit_last[i]) {
			continue;
		}
		edit_last[i] = now;
		_store_bone(p_skeleton, p_anchor, i, Transform3D(Basis(now.rotation).scaled(now.scale), now.position));
		changed = true;
	}
	if (changed) {
		_rebuild_entries(p_skeleton);
		if (p_anchor) {
			edit_anchor_last = _anchor_in_skeleton(p_skeleton, p_anchor);
		}
		return;
	}
	// The anchor was moved: what follows it goes with it.
	if (p_anchor) {
		const Transform3D anchor_transform = _anchor_in_skeleton(p_skeleton, p_anchor);
		if (!anchor_transform.is_equal_approx(edit_anchor_last)) {
			edit_anchor_last = anchor_transform;
			callable_mp(this, &AnchoredPoseModifier3D::_resync_anchored).call_deferred();
		}
	}
}

void AnchoredPoseModifier3D::_resync_anchored() {
	Skeleton3D *skeleton = get_skeleton();
	Node3D *anchor = _get_anchor();
	if (!skeleton || !anchor || !edit_active || edit_last.size() != (uint32_t)skeleton->get_bone_count()) {
		return;
	}
	const Transform3D anchor_transform = _anchor_in_skeleton(skeleton, anchor);
	for (const PoseEntry &entry : entries) {
		if (!entry.anchored) {
			continue;
		}
		BoneState state;
		state.position = anchor_transform.xform(entry.position);
		state.rotation = anchor_transform.basis.get_rotation_quaternion() * entry.rotation;
		state.scale = entry.scale;
		_write_state(skeleton, entry.bone, state);
		edit_last[entry.bone] = state;
	}
}

void AnchoredPoseModifier3D::set_editing(bool p_editing) {
	editing = p_editing;
	if (!Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	if (editing) {
		_begin_editing();
		editing = edit_active; // Could not start (no skeleton yet).
	} else {
		_end_editing();
	}
	notify_property_list_changed();
}

bool AnchoredPoseModifier3D::is_editing() const {
	return editing;
}

// The anchor (the flashlight) is hidden in the scene, because the game shows it when it is picked up.
// In the editor it is shown (never saved: put back before saving and when this node leaves) so the hand can
// be posed on it.
void AnchoredPoseModifier3D::_reveal_anchor(Node3D *p_anchor) {
	if (revealed_anchor == p_anchor->get_instance_id() || p_anchor->is_visible()) {
		return;
	}
	_unreveal_anchor();
	anchor_was_hidden = true;
	revealed_anchor = p_anchor->get_instance_id();
	p_anchor->set_visible(true);
}

void AnchoredPoseModifier3D::_unreveal_anchor() {
	Node3D *anchor = ObjectDB::get_instance<Node3D>(revealed_anchor);
	if (anchor && anchor_was_hidden) {
		anchor->set_visible(false);
	}
	revealed_anchor = ObjectID();
	anchor_was_hidden = false;
}

void AnchoredPoseModifier3D::set_preview_anchor(bool p_preview) {
	preview_anchor = p_preview;
	if (!preview_anchor) {
		_unreveal_anchor();
	}
}

bool AnchoredPoseModifier3D::is_preview_anchor() const {
	return preview_anchor;
}

// ---------------------------------------------------------------- properties

void AnchoredPoseModifier3D::set_anchor_node(const NodePath &p_path) {
	_unreveal_anchor();
	anchor_node = p_path;
	update_configuration_warnings();
}

NodePath AnchoredPoseModifier3D::get_anchor_node() const {
	return anchor_node;
}

void AnchoredPoseModifier3D::set_holding(bool p_holding) {
	holding = p_holding;
}

bool AnchoredPoseModifier3D::is_holding() const {
	return holding;
}

void AnchoredPoseModifier3D::set_fade_time(float p_seconds) {
	fade_time = MAX(p_seconds, 0.0f);
}

float AnchoredPoseModifier3D::get_fade_time() const {
	return fade_time;
}

float AnchoredPoseModifier3D::get_blend() const {
	return blend;
}

void AnchoredPoseModifier3D::set_pose(const Dictionary &p_pose) {
	pose = p_pose.duplicate();
	entries_dirty = true;
}

Dictionary AnchoredPoseModifier3D::get_pose() const {
	return pose;
}

void AnchoredPoseModifier3D::set_reach_assist(bool p_enabled) {
	reach_assist = p_enabled;
	notify_property_list_changed();
}

bool AnchoredPoseModifier3D::is_reach_assist() const {
	return reach_assist;
}

void AnchoredPoseModifier3D::set_reach_target_bone(const String &p_name) {
	reach_target_bone = p_name;
}

String AnchoredPoseModifier3D::get_reach_target_bone() const {
	return reach_target_bone;
}

void AnchoredPoseModifier3D::set_reach_root_bone(const String &p_name) {
	reach_root_bone = p_name;
}

String AnchoredPoseModifier3D::get_reach_root_bone() const {
	return reach_root_bone;
}

void AnchoredPoseModifier3D::set_reach_start_bone(const String &p_name) {
	reach_start_bone = p_name;
	reach_version = UINT64_MAX;
}

String AnchoredPoseModifier3D::get_reach_start_bone() const {
	return reach_start_bone;
}

void AnchoredPoseModifier3D::set_reach_middle_bone(const String &p_name) {
	reach_middle_bone = p_name;
	reach_version = UINT64_MAX;
}

String AnchoredPoseModifier3D::get_reach_middle_bone() const {
	return reach_middle_bone;
}

void AnchoredPoseModifier3D::set_reach_tip_bone(const String &p_name) {
	reach_tip_bone = p_name;
	reach_version = UINT64_MAX;
}

String AnchoredPoseModifier3D::get_reach_tip_bone() const {
	return reach_tip_bone;
}

void AnchoredPoseModifier3D::set_reach_comfort(float p_comfort) {
	reach_comfort = CLAMP(p_comfort, 0.5f, 1.0f);
}

float AnchoredPoseModifier3D::get_reach_comfort() const {
	return reach_comfort;
}

void AnchoredPoseModifier3D::set_reach_keep(float p_keep) {
	reach_keep = CLAMP(p_keep, 0.0f, 1.0f);
}

float AnchoredPoseModifier3D::get_reach_keep() const {
	return reach_keep;
}

void AnchoredPoseModifier3D::set_reach_rest_gap(float p_gap) {
	reach_rest_gap = MAX(p_gap, 0.0f);
}

float AnchoredPoseModifier3D::get_reach_rest_gap() const {
	return reach_rest_gap;
}

Callable AnchoredPoseModifier3D::get_clear_pose_button() const {
	return callable_mp(const_cast<AnchoredPoseModifier3D *>(this), &AnchoredPoseModifier3D::clear_pose);
}

void AnchoredPoseModifier3D::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name == "reach_target_bone" || p_property.name == "reach_root_bone" || p_property.name == "reach_start_bone" || p_property.name == "reach_middle_bone" || p_property.name == "reach_tip_bone") {
		if (!reach_assist) {
			p_property.usage = PROPERTY_USAGE_NONE;
		} else if (Engine::get_singleton()->is_editor_hint()) {
			Skeleton3D *skeleton = get_skeleton();
			if (skeleton) {
				p_property.hint = PROPERTY_HINT_ENUM_SUGGESTION;
				p_property.hint_string = skeleton->get_concatenated_bone_names();
			}
		}
	}
	if ((p_property.name == "reach_comfort" || p_property.name == "reach_keep") && !reach_assist) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}
}

PackedStringArray AnchoredPoseModifier3D::get_configuration_warnings() const {
	PackedStringArray warnings = SkeletonModifier3D::get_configuration_warnings();
	if (!anchor_node.is_empty() && is_inside_tree() && !_get_anchor()) {
		warnings.push_back(RTR("anchor_node does not point to a Node3D."));
	} else if (anchor_node.is_empty()) {
		warnings.push_back(RTR("Set anchor_node: the node (the held item) that the posed IK target bones follow."));
	}
	return warnings;
}

void AnchoredPoseModifier3D::_validate_bone_names() {
	notify_property_list_changed();
}

void AnchoredPoseModifier3D::_skeleton_changed(Skeleton3D *p_old, Skeleton3D *p_new) {
	SkeletonModifier3D::_skeleton_changed(p_old, p_new);
	entries_dirty = true;
	reach_version = UINT64_MAX;
}

void AnchoredPoseModifier3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			entries_dirty = true;
		} break;
		case NOTIFICATION_EXIT_TREE: {
#ifdef TOOLS_ENABLED
			_end_editing();
			editing = false;
			_unreveal_anchor();
#endif
		} break;
#ifdef TOOLS_ENABLED
		case NOTIFICATION_EDITOR_PRE_SAVE: {
			// Nothing of the editing may reach the file.
			if (edit_active && !edit_suspended) {
				_write_edit_state(edit_base);
				edit_suspended = true;
			}
			_unreveal_anchor();
		} break;
		case NOTIFICATION_EDITOR_POST_SAVE: {
			if (edit_active && edit_suspended) {
				_write_edit_state(edit_last);
				edit_suspended = false;
			}
		} break;
#endif // TOOLS_ENABLED
	}
}

void AnchoredPoseModifier3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_anchor_node", "path"), &AnchoredPoseModifier3D::set_anchor_node);
	ClassDB::bind_method(D_METHOD("get_anchor_node"), &AnchoredPoseModifier3D::get_anchor_node);
	ClassDB::bind_method(D_METHOD("set_holding", "holding"), &AnchoredPoseModifier3D::set_holding);
	ClassDB::bind_method(D_METHOD("is_holding"), &AnchoredPoseModifier3D::is_holding);
	ClassDB::bind_method(D_METHOD("set_fade_time", "seconds"), &AnchoredPoseModifier3D::set_fade_time);
	ClassDB::bind_method(D_METHOD("get_fade_time"), &AnchoredPoseModifier3D::get_fade_time);
	ClassDB::bind_method(D_METHOD("get_blend"), &AnchoredPoseModifier3D::get_blend);
	ClassDB::bind_method(D_METHOD("set_pose", "pose"), &AnchoredPoseModifier3D::set_pose);
	ClassDB::bind_method(D_METHOD("get_pose"), &AnchoredPoseModifier3D::get_pose);

	ClassDB::bind_method(D_METHOD("set_reach_assist", "enabled"), &AnchoredPoseModifier3D::set_reach_assist);
	ClassDB::bind_method(D_METHOD("is_reach_assist"), &AnchoredPoseModifier3D::is_reach_assist);
	ClassDB::bind_method(D_METHOD("set_reach_target_bone", "bone_name"), &AnchoredPoseModifier3D::set_reach_target_bone);
	ClassDB::bind_method(D_METHOD("get_reach_target_bone"), &AnchoredPoseModifier3D::get_reach_target_bone);
	ClassDB::bind_method(D_METHOD("set_reach_root_bone", "bone_name"), &AnchoredPoseModifier3D::set_reach_root_bone);
	ClassDB::bind_method(D_METHOD("get_reach_root_bone"), &AnchoredPoseModifier3D::get_reach_root_bone);
	ClassDB::bind_method(D_METHOD("set_reach_start_bone", "bone_name"), &AnchoredPoseModifier3D::set_reach_start_bone);
	ClassDB::bind_method(D_METHOD("get_reach_start_bone"), &AnchoredPoseModifier3D::get_reach_start_bone);
	ClassDB::bind_method(D_METHOD("set_reach_middle_bone", "bone_name"), &AnchoredPoseModifier3D::set_reach_middle_bone);
	ClassDB::bind_method(D_METHOD("get_reach_middle_bone"), &AnchoredPoseModifier3D::get_reach_middle_bone);
	ClassDB::bind_method(D_METHOD("set_reach_tip_bone", "bone_name"), &AnchoredPoseModifier3D::set_reach_tip_bone);
	ClassDB::bind_method(D_METHOD("get_reach_tip_bone"), &AnchoredPoseModifier3D::get_reach_tip_bone);
	ClassDB::bind_method(D_METHOD("set_reach_comfort", "comfort"), &AnchoredPoseModifier3D::set_reach_comfort);
	ClassDB::bind_method(D_METHOD("get_reach_comfort"), &AnchoredPoseModifier3D::get_reach_comfort);
	ClassDB::bind_method(D_METHOD("set_reach_keep", "keep"), &AnchoredPoseModifier3D::set_reach_keep);
	ClassDB::bind_method(D_METHOD("get_reach_keep"), &AnchoredPoseModifier3D::get_reach_keep);
	ClassDB::bind_method(D_METHOD("set_reach_rest_gap", "gap"), &AnchoredPoseModifier3D::set_reach_rest_gap);
	ClassDB::bind_method(D_METHOD("get_reach_rest_gap"), &AnchoredPoseModifier3D::get_reach_rest_gap);

	ClassDB::bind_method(D_METHOD("set_preview_anchor", "preview"), &AnchoredPoseModifier3D::set_preview_anchor);
	ClassDB::bind_method(D_METHOD("is_preview_anchor"), &AnchoredPoseModifier3D::is_preview_anchor);
	ClassDB::bind_method(D_METHOD("set_editing", "editing"), &AnchoredPoseModifier3D::set_editing);
	ClassDB::bind_method(D_METHOD("is_editing"), &AnchoredPoseModifier3D::is_editing);
	ClassDB::bind_method(D_METHOD("get_clear_pose_button"), &AnchoredPoseModifier3D::get_clear_pose_button);

	ClassDB::bind_method(D_METHOD("set_pose_bone", "bone", "transform"), &AnchoredPoseModifier3D::set_pose_bone);
	ClassDB::bind_method(D_METHOD("get_pose_bone", "bone"), &AnchoredPoseModifier3D::get_pose_bone);
	ClassDB::bind_method(D_METHOD("has_pose_bone", "bone"), &AnchoredPoseModifier3D::has_pose_bone);
	ClassDB::bind_method(D_METHOD("erase_pose_bone", "bone"), &AnchoredPoseModifier3D::erase_pose_bone);
	ClassDB::bind_method(D_METHOD("get_pose_bones"), &AnchoredPoseModifier3D::get_pose_bones);
	ClassDB::bind_method(D_METHOD("capture_bone", "bone"), &AnchoredPoseModifier3D::capture_bone);
	ClassDB::bind_method(D_METHOD("import_pose_from_animation", "animation", "time"), &AnchoredPoseModifier3D::import_pose_from_animation, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("clear_pose"), &AnchoredPoseModifier3D::clear_pose);

	ClassDB::bind_method(D_METHOD("set_body_bones", "bones"), &AnchoredPoseModifier3D::set_body_bones);
	ClassDB::bind_method(D_METHOD("get_body_bones"), &AnchoredPoseModifier3D::get_body_bones);
	ClassDB::bind_method(D_METHOD("set_pose_resource", "resource"), &AnchoredPoseModifier3D::set_pose_resource);
	ClassDB::bind_method(D_METHOD("get_pose_resource"), &AnchoredPoseModifier3D::get_pose_resource);
	ClassDB::bind_method(D_METHOD("set_slot", "slot"), &AnchoredPoseModifier3D::set_slot);
	ClassDB::bind_method(D_METHOD("get_slot"), &AnchoredPoseModifier3D::get_slot);
	ClassDB::bind_method(D_METHOD("set_editor_preview", "preview"), &AnchoredPoseModifier3D::set_editor_preview);
	ClassDB::bind_method(D_METHOD("is_editor_preview"), &AnchoredPoseModifier3D::is_editor_preview);
	ClassDB::bind_method(D_METHOD("engage", "anchor", "pose", "fade_in", "fade_out"), &AnchoredPoseModifier3D::engage, DEFVAL(-1.0), DEFVAL(-1.0));
	ClassDB::bind_method(D_METHOD("release"), &AnchoredPoseModifier3D::release);
	ClassDB::bind_method(D_METHOD("is_engaged"), &AnchoredPoseModifier3D::is_engaged);
	ClassDB::bind_method(D_METHOD("write_pose_resource"), &AnchoredPoseModifier3D::write_pose_resource);
	ClassDB::bind_method(D_METHOD("read_pose_resource"), &AnchoredPoseModifier3D::read_pose_resource);
	ClassDB::bind_method(D_METHOD("get_write_pose_button"), &AnchoredPoseModifier3D::get_write_pose_button);
	ClassDB::bind_method(D_METHOD("get_read_pose_button"), &AnchoredPoseModifier3D::get_read_pose_button);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "anchor_node", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_anchor_node", "get_anchor_node");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "holding"), "set_holding", "is_holding");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fade_time", PROPERTY_HINT_RANGE, "0,5,0.01,or_greater,suffix:s"), "set_fade_time", "get_fade_time");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "pose", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_pose", "get_pose");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "body_bones"), "set_body_bones", "get_body_bones");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "slot"), "set_slot", "get_slot");

	ADD_GROUP("Reach Assist", "reach_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "reach_assist"), "set_reach_assist", "is_reach_assist");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "reach_target_bone", PROPERTY_HINT_ENUM_SUGGESTION, ""), "set_reach_target_bone", "get_reach_target_bone");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "reach_root_bone", PROPERTY_HINT_ENUM_SUGGESTION, ""), "set_reach_root_bone", "get_reach_root_bone");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "reach_start_bone", PROPERTY_HINT_ENUM_SUGGESTION, ""), "set_reach_start_bone", "get_reach_start_bone");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "reach_middle_bone", PROPERTY_HINT_ENUM_SUGGESTION, ""), "set_reach_middle_bone", "get_reach_middle_bone");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "reach_tip_bone", PROPERTY_HINT_ENUM_SUGGESTION, ""), "set_reach_tip_bone", "get_reach_tip_bone");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reach_comfort", PROPERTY_HINT_RANGE, "0.5,1,0.01"), "set_reach_comfort", "get_reach_comfort");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reach_keep", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_reach_keep", "get_reach_keep");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reach_rest_gap", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_reach_rest_gap", "get_reach_rest_gap");

	ADD_GROUP("Editor", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "preview_anchor", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT), "set_preview_anchor", "is_preview_anchor");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "editor_preview"), "set_editor_preview", "is_editor_preview");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "editing", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR), "set_editing", "is_editing");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "pose_resource", PROPERTY_HINT_RESOURCE_TYPE, "AnchoredPose"), "set_pose_resource", "get_pose_resource");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "write_pose_button", PROPERTY_HINT_TOOL_BUTTON, "Write the pose into the resource,Save", PROPERTY_USAGE_EDITOR), "", "get_write_pose_button");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "read_pose_button", PROPERTY_HINT_TOOL_BUTTON, "Read the pose from the resource,Load", PROPERTY_USAGE_EDITOR), "", "get_read_pose_button");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "clear_pose_button", PROPERTY_HINT_TOOL_BUTTON, "Clear the pose,Remove", PROPERTY_USAGE_EDITOR), "", "get_clear_pose_button");
}

AnchoredPoseModifier3D::AnchoredPoseModifier3D() {
}
