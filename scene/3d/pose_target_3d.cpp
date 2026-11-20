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

bool PoseTarget3D::engage(Node *p_rig) {
	ERR_FAIL_NULL_V(p_rig, false);
	if (pose.is_null() || slot == StringName()) {
		return false;
	}
	AnchoredPoseModifier3D *modifier = _find_modifier(p_rig, slot, 0);
	if (!modifier) {
		return false;
	}
	modifier->engage(this, pose, fade_in, fade_out);
	return true;
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
		if (modifier) {
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
		if (pose.is_valid() && modifier->get_slot() == slot) {
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
	if (pose.is_null()) {
		warnings.push_back(RTR("Assign a pose (an AnchoredPose resource) to show and use."));
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
	ClassDB::bind_method(D_METHOD("engage", "rig"), &PoseTarget3D::engage);
	ClassDB::bind_method(D_METHOD("disengage", "rig"), &PoseTarget3D::disengage);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "slot"), "set_slot", "get_slot");
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
