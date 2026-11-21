/**************************************************************************/
/*  pose_target_3d.cpp                                                    */
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

#include "pose_target_3d.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/3d/anchored_pose_modifier_3d.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/3d/physics/collision_object_3d.h"
#include "scene/3d/audio_stream_player_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/main/canvas_item.h"
#include "scene/main/canvas_layer.h"
#include "scene/main/scene_tree.h"

// ---------------------------------------------------------------- driving a rig

AnchoredPoseModifier3D *PoseTarget3D::_find_modifier(Node *p_node, const StringName &p_slot, int p_depth) {
	if (!p_node || p_depth > 64) {
		return nullptr;
	}
	AnchoredPoseModifier3D *modifier = Object::cast_to<AnchoredPoseModifier3D>(p_node);
	if (modifier && modifier->get_slot() == p_slot) {
		return modifier;
	}
	for (int i = 0; i < p_node->get_child_count(true); i++) {
		AnchoredPoseModifier3D *found = _find_modifier(p_node->get_child(i, true), p_slot, p_depth + 1);
		if (found) {
			return found;
		}
	}
	return nullptr;
}

Skeleton3D *PoseTarget3D::_get_pose_skeleton() const {
	if (pose_skeleton.is_empty() || !is_inside_tree()) {
		return nullptr;
	}
	return Object::cast_to<Skeleton3D>(get_node_or_null(pose_skeleton));
}

bool PoseTarget3D::engage(Node *p_rig) {
	ERR_FAIL_NULL_V(p_rig, false);
	Skeleton3D *proxy = _get_pose_skeleton();
	if ((pose.is_null() && !proxy) || slot == StringName()) {
		return false;
	}
	AnchoredPoseModifier3D *modifier = _find_modifier(p_rig, slot, 0);
	if (!modifier) {
		return false;
	}
	if (proxy) {
		modifier->engage_pose_skeleton(this, proxy, body_bones, fade_in, fade_out);
	} else {
		modifier->engage(this, pose, fade_in, fade_out);
	}
	return true;
}

// The pose skeleton: the bones of the rig's slot (the subtrees of its pose_roots), as a Skeleton3D child.
// The roots (the hand's IK target bone) start at the target, the rest at rest. Poses live in the bone poses
// of that skeleton, so they can be made with the Skeleton3D tools and animated with any AnimationPlayer.
bool PoseTarget3D::create_pose_skeleton_from(Node *p_rig) {
	ERR_FAIL_NULL_V(p_rig, false);
	ERR_FAIL_COND_V_MSG(!is_inside_tree(), false, "PoseTarget3D must be in the scene tree.");
	if (_get_pose_skeleton()) {
		WARN_PRINT("PoseTarget3D: already has a pose skeleton.");
		return false;
	}
	AnchoredPoseModifier3D *modifier = _find_modifier(p_rig, slot, 0);
	ERR_FAIL_NULL_V_MSG(modifier, false, vformat("No AnchoredPoseModifier3D with the slot '%s' in the rig.", slot));
	Skeleton3D *source = modifier->get_skeleton();
	ERR_FAIL_NULL_V(source, false);
	const PackedStringArray roots = modifier->get_pose_roots();
	ERR_FAIL_COND_V_MSG(roots.is_empty(), false, "The modifier has no pose_roots: set the bones it poses (for example the hand's IK target bone).");

	// The bones: every root and everything below it.
	LocalVector<int> chosen;
	LocalVector<int> index_in_proxy;
	index_in_proxy.resize(source->get_bone_count());
	for (int i = 0; i < source->get_bone_count(); i++) {
		index_in_proxy[i] = -1;
		int walk = i;
		bool inside = false;
		while (walk >= 0) {
			if (roots.has(source->get_bone_name(walk))) {
				inside = true;
				break;
			}
			walk = source->get_bone_parent(walk);
		}
		if (inside) {
			chosen.push_back(i);
		}
	}
	ERR_FAIL_COND_V_MSG(chosen.is_empty(), false, "None of the pose_roots bones exist in the skeleton.");

	Skeleton3D *proxy = memnew(Skeleton3D);
	proxy->set_name("PoseSkeleton");
	add_child(proxy);
	if (get_owner()) {
		proxy->set_owner(get_owner());
	}
	for (uint32_t k = 0; k < chosen.size(); k++) {
		index_in_proxy[chosen[k]] = proxy->add_bone(source->get_bone_name(chosen[k]));
	}
	PackedStringArray others;
	for (uint32_t k = 0; k < chosen.size(); k++) {
		const int from = chosen[k];
		const int to = index_in_proxy[from];
		const int parent = source->get_bone_parent(from);
		proxy->set_bone_parent(to, parent >= 0 ? index_in_proxy[parent] : -1);
		proxy->set_bone_rest(to, source->get_bone_rest(from));
	}
	proxy->reset_bone_poses();
	for (uint32_t k = 0; k < chosen.size(); k++) {
		const int from = chosen[k];
		if (source->get_bone_parent(from) >= 0 && index_in_proxy[source->get_bone_parent(from)] >= 0) {
			continue;
		}
		// A root: the one the slot reaches with (the hand) starts on the target, the others (an elbow) stay at rest
		// in the body's space.
		if (source->get_bone_name(from) == modifier->get_reach_target_bone() || roots.size() == 1) {
			proxy->set_bone_pose(index_in_proxy[from], Transform3D());
		} else {
			others.push_back(source->get_bone_name(from));
		}
	}
	// Same size as the character's skeleton, so the bones are seen where they will be.
	proxy->set_scale(source->get_global_transform().basis.get_scale());
	body_bones = others;
	pose_skeleton = get_path_to(proxy);
	update_configuration_warnings();
	return true;
}

Callable PoseTarget3D::get_create_pose_skeleton_button() const {
	return callable_mp(const_cast<PoseTarget3D *>(this), &PoseTarget3D::create_pose_skeleton);
}

void PoseTarget3D::set_pose_skeleton(const NodePath &p_path) {
	pose_skeleton = p_path;
	update_configuration_warnings();
}

NodePath PoseTarget3D::get_pose_skeleton() const {
	return pose_skeleton;
}

void PoseTarget3D::set_body_bones(const PackedStringArray &p_bones) {
	body_bones = p_bones;
}

PackedStringArray PoseTarget3D::get_body_bones() const {
	return body_bones;
}

void PoseTarget3D::disengage(Node *p_rig) {
	ERR_FAIL_NULL(p_rig);
	AnchoredPoseModifier3D *modifier = _find_modifier(p_rig, slot, 0);
	if (modifier && modifier->is_engaged()) {
		modifier->release();
	}
}

// ---------------------------------------------------------------- the preview (editor only)

#ifdef TOOLS_ENABLED
// Editor button: makes the pose skeleton from the preview character, and selects it to be posed.
void PoseTarget3D::create_pose_skeleton() {
	_update_preview();
	Node *ghost = ObjectDB::get_instance<Node>(ghost_id);
	if (!ghost) {
		WARN_PRINT("PoseTarget3D: there is no preview character to take the bones from (see animation/pose_target/preview_rig).");
		return;
	}
	if (create_pose_skeleton_from(ghost)) {
		Object *editor = Engine::get_singleton()->get_singleton_object("EditorInterface");
		if (editor) {
			editor->call("mark_scene_as_unsaved");
		}
		notify_property_list_changed();
		_select_in_editor(_get_pose_skeleton());
	}
}

AnchoredPoseModifier3D *PoseTarget3D::_get_ghost_modifier() const {
	Node *ghost = ObjectDB::get_instance<Node>(ghost_id);
	return ghost ? _find_modifier(ghost, slot, 0) : nullptr;
}

// The ghost: see-through, no shadows, no lights of its own.
void PoseTarget3D::_apply_ghost_look(Node3D *p_ghost) {
	if (ghost_material.is_null()) {
		ghost_material.instantiate();
		ghost_material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
		ghost_material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		ghost_material->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	}
	TypedArray<Node> geometry = p_ghost->find_children("*", "GeometryInstance3D", true, false);
	for (int i = 0; i < geometry.size(); i++) {
		GeometryInstance3D *instance = Object::cast_to<GeometryInstance3D>(geometry[i].operator Object *());
		if (!instance) {
			continue;
		}
		// Only the character itself is shown: what is skinned to a skeleton. Props, gizmo helpers and the like of
		// the rig scene are not part of the preview.
		MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(instance);
		if (!mesh || mesh->get_skin_reference().is_null()) {
			instance->set_visible(false);
			continue;
		}
		instance->set_material_override(ghost_material);
		instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	}
	// Nothing that only draws a helper in the editor: shapes, rays, speakers, the camera's frustum.
	const char *helpers[] = { "CollisionObject3D", "CollisionShape3D", "CollisionPolygon3D", "RayCast3D", "ShapeCast3D", "AudioStreamPlayer3D", "Camera3D", "Marker3D", "NavigationAgent3D" };
	for (const char *helper : helpers) {
		TypedArray<Node> found = p_ghost->find_children("*", helper, true, false);
		for (int i = 0; i < found.size(); i++) {
			Node3D *node = Object::cast_to<Node3D>(found[i].operator Object *());
			if (node) {
				node->set_visible(false);
			}
		}
	}
	TypedArray<Node> lights = p_ghost->find_children("*", "Light3D", true, false);
	for (int i = 0; i < lights.size(); i++) {
		Light3D *light = Object::cast_to<Light3D>(lights[i].operator Object *());
		if (light) {
			light->set_visible(false);
		}
	}
}

// Puts a node in the editor's selection, which brings up its own editing tools (a Skeleton3D's pose mode).
void PoseTarget3D::_select_in_editor(Node *p_node) {
	Object *editor = Engine::get_singleton()->get_singleton_object("EditorInterface");
	if (!editor || !p_node) {
		return;
	}
	Object *selection = editor->call("get_selection");
	if (selection) {
		selection->call("clear");
		selection->call("add_node", p_node);
	}
}

void PoseTarget3D::set_posing(bool p_posing) {
#ifdef TOOLS_ENABLED
	if (!Engine::get_singleton()->is_editor_hint() || !is_inside_tree()) {
		posing = false;
		return;
	}
	if (p_posing) {
		if (Skeleton3D *proxy = _get_pose_skeleton()) {
			// The pose skeleton is a real node of the scene: the normal pose tools and keys work on it.
			posing = true;
			_select_in_editor(proxy);
			notify_property_list_changed();
			return;
		}
		if (pose.is_null()) {
			Ref<AnchoredPose> fresh;
			fresh.instantiate();
			set_pose(fresh);
		}
		_update_preview();
		AnchoredPoseModifier3D *modifier = _get_ghost_modifier();
		Skeleton3D *skeleton = modifier ? modifier->get_skeleton() : nullptr;
		if (!modifier || !skeleton) {
			posing = false;
			WARN_PRINT("PoseTarget3D: there is no ghost to pose. Edit the scene this node is in, with a preview rig (see animation/pose_target/preview_rig) that has a modifier with this slot.");
			notify_property_list_changed();
			return;
		}
		modifier->engage(this, pose, fade_in, fade_out);
		modifier->set_editing(true);
		posing = modifier->is_editing();
		if (posing) {
			_select_in_editor(skeleton);
		}
	} else if (posing) {
		AnchoredPoseModifier3D *modifier = _get_ghost_modifier();
		if (modifier && modifier->is_editing()) {
			modifier->set_editing(false);
		}
		posing = false;
		_select_in_editor(this);
	}
	notify_property_list_changed();
#else
	posing = false;
#endif // TOOLS_ENABLED
}

void PoseTarget3D::_free_preview() {
	Node *ghost = ObjectDB::get_instance<Node>(ghost_id);
	if (ghost) {
		remove_child(ghost);
		memdelete(ghost);
	}
	ghost_id = ObjectID();
	ghost_source = String();
}

// While you edit the scene this target is in, a ghost of the character (the project's preview rig, see
// animation/pose_target/preview_rig) stands where the player would, looking at the target, with its
// slot posed by this target. The ghost is an internal child: not in the scene tree dock, never saved.
void PoseTarget3D::_update_preview() {
	SceneTree *tree = get_tree();
	const bool wanted = preview && tree && is_inside_tree() && get_owner() != nullptr && get_owner() == tree->get_edited_scene_root();
	if (!wanted) {
		_free_preview();
		return;
	}

	Ref<PackedScene> scene = preview_rig;
	String source = scene.is_valid() ? scene->get_path() : String();
	if (scene.is_null()) {
		source = GLOBAL_GET("animation/pose_target/preview_rig");
		if (source.is_empty() || !ResourceLoader::exists(source)) {
			_free_preview();
			return;
		}
	}

	Node3D *ghost = ObjectDB::get_instance<Node3D>(ghost_id);
	if (ghost && ghost_source != source) {
		_free_preview();
		ghost = nullptr;
	}
	if (!ghost) {
		if (scene.is_null()) {
			scene = ResourceLoader::load(source);
		}
		if (scene.is_null()) {
			return;
		}
		Node *instance = scene->instantiate(PackedScene::GEN_EDIT_STATE_DISABLED);
		ghost = Object::cast_to<Node3D>(instance);
		if (!ghost) {
			if (instance) {
				memdelete(instance);
			}
			return;
		}
		ghost->set_as_top_level(true);
		add_child(ghost, false, INTERNAL_MODE_BACK);
		ghost_id = ghost->get_instance_id();
		ghost_source = source;

		_apply_ghost_look(ghost);

		// A preview has no interface: hide every 2D root of the rig.
		TypedArray<Node> items = ghost->find_children("*", "CanvasItem", true, false);
		for (int i = 0; i < items.size(); i++) {
			CanvasItem *item = Object::cast_to<CanvasItem>(items[i].operator Object *());
			if (item && !Object::cast_to<CanvasItem>(item->get_parent())) {
				item->hide();
			}
		}
		TypedArray<Node> layers = ghost->find_children("*", "CanvasLayer", true, false);
		for (int i = 0; i < layers.size(); i++) {
			CanvasLayer *layer = Object::cast_to<CanvasLayer>(layers[i].operator Object *());
			if (layer) {
				layer->hide();
			}
		}
		// Where the eyes are in the rig.
		ghost_camera_in_root = Transform3D();
		TypedArray<Node> cameras = ghost->find_children("*", "Camera3D", true, false);
		if (cameras.size() > 0) {
			Camera3D *camera = Object::cast_to<Camera3D>(cameras[0].operator Object *());
			if (camera) {
				ghost_camera_in_root = ghost->get_global_transform().affine_inverse() * camera->get_global_transform();
			}
		}
	}

	if (ghost_material.is_valid()) {
		ghost_material->set_albedo(preview_color);
	}

	// The player stands in front of the object (its own +Z, level), a little back and up, looking at the target.
	const Vector3 target = get_global_transform().origin;
	Vector3 forward(0, 0, 1);
	Node3D *parent = get_parent_node_3d();
	if (parent) {
		forward = parent->get_global_transform().basis.get_column(2);
	}
	forward.y = 0;
	forward = forward.is_zero_approx() ? Vector3(0, 0, 1) : forward.normalized();
	forward = Basis(Vector3(0, 1, 0), Math::deg_to_rad(preview_yaw)).xform(forward);
	const Vector3 eye = target + forward * preview_distance + Vector3(0, preview_height, 0);
	const Vector3 look = target - eye;
	if (look.is_zero_approx()) {
		return;
	}
	const Transform3D eye_transform(Basis::looking_at(look, Vector3(0, 1, 0)), eye);
	ghost->set_global_transform(eye_transform * ghost_camera_in_root.affine_inverse());

	// Pose the slot of this target; the rig's other slots stay at rest.
	TypedArray<Node> modifiers = ghost->find_children("*", "AnchoredPoseModifier3D", true, false);
	for (int i = 0; i < modifiers.size(); i++) {
		AnchoredPoseModifier3D *modifier = Object::cast_to<AnchoredPoseModifier3D>(modifiers[i].operator Object *());
		if (!modifier) {
			continue;
		}
		if (modifier->get_slot() == slot && _get_pose_skeleton()) {
			modifier->engage_pose_skeleton(this, _get_pose_skeleton(), body_bones, fade_in, fade_out);
		} else if (pose.is_valid() && modifier->get_slot() == slot) {
			modifier->engage(this, pose, fade_in, fade_out);
		} else {
			if (modifier->is_engaged()) {
				modifier->release();
			}
			modifier->set_editor_preview(false);
			modifier->set_preview_anchor(false); // Nothing of the preview character's own holds is shown.
			modifier->set_holding(false);
		}
	}
}
#endif // TOOLS_ENABLED

bool PoseTarget3D::is_posing() const {
	return posing;
}

void PoseTarget3D::set_preview_color(const Color &p_color) {
	preview_color = p_color;
}

Color PoseTarget3D::get_preview_color() const {
	return preview_color;
}

#ifndef TOOLS_ENABLED
void PoseTarget3D::set_posing(bool p_posing) {
	posing = false; // Posing the ghost is an editor tool.
}

void PoseTarget3D::create_pose_skeleton() {
	// Making the pose skeleton from the preview character is an editor tool (see create_pose_skeleton_from).
}
#endif // TOOLS_ENABLED

void PoseTarget3D::_notification(int p_what) {
	switch (p_what) {
#ifdef TOOLS_ENABLED
		case NOTIFICATION_ENTER_TREE: {
			set_process_internal(Engine::get_singleton()->is_editor_hint());
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_free_preview();
			posing = false;
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			_update_preview();
		} break;
		case NOTIFICATION_EDITOR_PRE_SAVE: {
			// A pose that was posed by hand and lives in its own file is saved with the scene.
			if (pose_dirty && pose.is_valid() && !pose->is_built_in() && !pose->get_path().is_empty()) {
				ResourceSaver::save(pose, pose->get_path());
			}
			pose_dirty = false;
		} break;
#endif // TOOLS_ENABLED
		default:
			break;
	}
}

// ---------------------------------------------------------------- properties

PackedStringArray PoseTarget3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	if (slot == StringName()) {
		warnings.push_back(RTR("Set slot to the name of the AnchoredPoseModifier3D this target poses (for example the right hand's)."));
	}
	if (pose.is_null() && pose_skeleton.is_empty()) {
		warnings.push_back(RTR("Press \"Create the pose skeleton\" (a Skeleton3D to pose and animate), or assign a pose (an AnchoredPose resource)."));
	}
	return warnings;
}

void PoseTarget3D::set_slot(const StringName &p_slot) {
	slot = p_slot;
	update_configuration_warnings();
}

StringName PoseTarget3D::get_slot() const {
	return slot;
}

void PoseTarget3D::_pose_changed() {
	pose_dirty = true;
}

void PoseTarget3D::set_pose(const Ref<AnchoredPose> &p_pose) {
	if (pose.is_valid()) {
		pose->disconnect_changed(callable_mp(this, &PoseTarget3D::_pose_changed));
	}
	pose = p_pose;
	if (pose.is_valid()) {
		pose->connect_changed(callable_mp(this, &PoseTarget3D::_pose_changed));
	}
	pose_dirty = false;
	update_configuration_warnings();
}

Ref<AnchoredPose> PoseTarget3D::get_pose() const {
	return pose;
}

void PoseTarget3D::set_fade_in(float p_seconds) {
	fade_in = MAX(p_seconds, 0.0f);
}

float PoseTarget3D::get_fade_in() const {
	return fade_in;
}

void PoseTarget3D::set_fade_out(float p_seconds) {
	fade_out = MAX(p_seconds, 0.0f);
}

float PoseTarget3D::get_fade_out() const {
	return fade_out;
}

void PoseTarget3D::set_preview(bool p_preview) {
	preview = p_preview;
}

bool PoseTarget3D::is_preview() const {
	return preview;
}

void PoseTarget3D::set_preview_rig(const Ref<PackedScene> &p_scene) {
	preview_rig = p_scene;
}

Ref<PackedScene> PoseTarget3D::get_preview_rig() const {
	return preview_rig;
}

void PoseTarget3D::set_preview_distance(float p_distance) {
	preview_distance = p_distance;
}

float PoseTarget3D::get_preview_distance() const {
	return preview_distance;
}

void PoseTarget3D::set_preview_height(float p_height) {
	preview_height = p_height;
}

float PoseTarget3D::get_preview_height() const {
	return preview_height;
}

void PoseTarget3D::set_preview_yaw(float p_degrees) {
	preview_yaw = p_degrees;
}

float PoseTarget3D::get_preview_yaw() const {
	return preview_yaw;
}

void PoseTarget3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_slot", "slot"), &PoseTarget3D::set_slot);
	ClassDB::bind_method(D_METHOD("get_slot"), &PoseTarget3D::get_slot);
	ClassDB::bind_method(D_METHOD("set_pose", "pose"), &PoseTarget3D::set_pose);
	ClassDB::bind_method(D_METHOD("get_pose"), &PoseTarget3D::get_pose);
	ClassDB::bind_method(D_METHOD("set_fade_in", "seconds"), &PoseTarget3D::set_fade_in);
	ClassDB::bind_method(D_METHOD("get_fade_in"), &PoseTarget3D::get_fade_in);
	ClassDB::bind_method(D_METHOD("set_fade_out", "seconds"), &PoseTarget3D::set_fade_out);
	ClassDB::bind_method(D_METHOD("get_fade_out"), &PoseTarget3D::get_fade_out);
	ClassDB::bind_method(D_METHOD("set_preview", "preview"), &PoseTarget3D::set_preview);
	ClassDB::bind_method(D_METHOD("is_preview"), &PoseTarget3D::is_preview);
	ClassDB::bind_method(D_METHOD("set_preview_rig", "scene"), &PoseTarget3D::set_preview_rig);
	ClassDB::bind_method(D_METHOD("get_preview_rig"), &PoseTarget3D::get_preview_rig);
	ClassDB::bind_method(D_METHOD("set_preview_distance", "distance"), &PoseTarget3D::set_preview_distance);
	ClassDB::bind_method(D_METHOD("get_preview_distance"), &PoseTarget3D::get_preview_distance);
	ClassDB::bind_method(D_METHOD("set_preview_height", "height"), &PoseTarget3D::set_preview_height);
	ClassDB::bind_method(D_METHOD("get_preview_height"), &PoseTarget3D::get_preview_height);
	ClassDB::bind_method(D_METHOD("set_preview_yaw", "degrees"), &PoseTarget3D::set_preview_yaw);
	ClassDB::bind_method(D_METHOD("get_preview_yaw"), &PoseTarget3D::get_preview_yaw);
	ClassDB::bind_method(D_METHOD("set_preview_color", "color"), &PoseTarget3D::set_preview_color);
	ClassDB::bind_method(D_METHOD("get_preview_color"), &PoseTarget3D::get_preview_color);
	ClassDB::bind_method(D_METHOD("set_posing", "posing"), &PoseTarget3D::set_posing);
	ClassDB::bind_method(D_METHOD("is_posing"), &PoseTarget3D::is_posing);
	ClassDB::bind_method(D_METHOD("set_pose_skeleton", "path"), &PoseTarget3D::set_pose_skeleton);
	ClassDB::bind_method(D_METHOD("get_pose_skeleton"), &PoseTarget3D::get_pose_skeleton);
	ClassDB::bind_method(D_METHOD("set_body_bones", "bones"), &PoseTarget3D::set_body_bones);
	ClassDB::bind_method(D_METHOD("get_body_bones"), &PoseTarget3D::get_body_bones);
	ClassDB::bind_method(D_METHOD("create_pose_skeleton_from", "rig"), &PoseTarget3D::create_pose_skeleton_from);
	ClassDB::bind_method(D_METHOD("create_pose_skeleton"), &PoseTarget3D::create_pose_skeleton);
	ClassDB::bind_method(D_METHOD("get_create_pose_skeleton_button"), &PoseTarget3D::get_create_pose_skeleton_button);
	ClassDB::bind_method(D_METHOD("engage", "rig"), &PoseTarget3D::engage);
	ClassDB::bind_method(D_METHOD("disengage", "rig"), &PoseTarget3D::disengage);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "slot"), "set_slot", "get_slot");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "pose_skeleton", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Skeleton3D"), "set_pose_skeleton", "get_pose_skeleton");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "create_pose_skeleton_button", PROPERTY_HINT_TOOL_BUTTON, "Create the pose skeleton,Skeleton3D", PROPERTY_USAGE_EDITOR), "", "get_create_pose_skeleton_button");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "body_bones"), "set_body_bones", "get_body_bones");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "pose", PROPERTY_HINT_RESOURCE_TYPE, "AnchoredPose"), "set_pose", "get_pose");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fade_in", PROPERTY_HINT_RANGE, "0,3,0.01,or_greater,suffix:s"), "set_fade_in", "get_fade_in");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fade_out", PROPERTY_HINT_RANGE, "0,3,0.01,or_greater,suffix:s"), "set_fade_out", "get_fade_out");

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "posing", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR), "set_posing", "is_posing");

	ADD_GROUP("Preview", "preview_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "preview_enabled"), "set_preview", "is_preview");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "preview_rig", PROPERTY_HINT_RESOURCE_TYPE, "PackedScene"), "set_preview_rig", "get_preview_rig");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "preview_distance", PROPERTY_HINT_RANGE, "0,5,0.01,or_greater,suffix:m"), "set_preview_distance", "get_preview_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "preview_height", PROPERTY_HINT_RANGE, "-2,3,0.01,or_greater,or_less,suffix:m"), "set_preview_height", "get_preview_height");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "preview_color"), "set_preview_color", "get_preview_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "preview_yaw", PROPERTY_HINT_RANGE, "-180,180,0.1,degrees"), "set_preview_yaw", "get_preview_yaw");
}

PoseTarget3D::PoseTarget3D() {
}
