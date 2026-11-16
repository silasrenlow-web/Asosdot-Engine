/**************************************************************************/
/*  skeleton_3d_editor_plugin.cpp                                         */
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

#include "skeleton_3d_editor_plugin.h"

#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/animation/animation_player_editor_plugin.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/inspector/editor_properties.h"
#include "editor/inspector/editor_properties_vector.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/physical_bone_3d.h"
#include "scene/3d/physics/physical_bone_simulator_3d.h"
#include "scene/gui/separator.h"
#include "scene/gui/texture_rect.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/3d/capsule_shape_3d.h"
#include "scene/resources/skeleton_profile.h"
#include "scene/resources/surface_tool.h"

void BonePropertiesEditor::create_editors() {
	section = memnew(EditorInspectorSection);
	section->setup("trf_properties", label, this, Color(0.0f, 0.0f, 0.0f), true);
	section->unfold();
	add_child(section);

	enabled_checkbox = memnew(EditorPropertyCheck());
	enabled_checkbox->set_label("Pose Enabled");
	enabled_checkbox->set_selectable(false);
	enabled_checkbox->connect("property_changed", callable_mp(this, &BonePropertiesEditor::_value_changed));
	section->get_vbox()->add_child(enabled_checkbox);

	// Position property.
	EditorPropertyRangeHint large_range_hint;
	large_range_hint.min = -10000;
	large_range_hint.max = 10000;
	large_range_hint.step = 0.001;

	position_property = memnew(EditorPropertyVector3());
	position_property->setup(large_range_hint);
	position_property->set_label("Position");
	position_property->set_selectable(false);
	position_property->connect("property_changed", callable_mp(this, &BonePropertiesEditor::_value_changed));
	position_property->connect("property_keyed", callable_mp(this, &BonePropertiesEditor::_property_keyed));
	section->get_vbox()->add_child(position_property);

	// Rotation property.
	rotation_property = memnew(EditorPropertyQuaternion());
	// Quaternions are almost never used for human-readable values that need stepifying,
	// so we should be more precise with their step, as much as the float precision allows.
#ifdef REAL_T_IS_DOUBLE
	constexpr double QUATERNION_STEP = 1e-14;
#else
	constexpr double QUATERNION_STEP = 1e-6;
#endif
	EditorPropertyRangeHint quaternion_range_hint;
	quaternion_range_hint.min = -1.0;
	quaternion_range_hint.max = 1.0;
	quaternion_range_hint.step = QUATERNION_STEP;
	rotation_property->setup(quaternion_range_hint);
	rotation_property->set_label("Rotation");
	rotation_property->set_selectable(false);
	rotation_property->connect("property_changed", callable_mp(this, &BonePropertiesEditor::_value_changed));
	rotation_property->connect("property_keyed", callable_mp(this, &BonePropertiesEditor::_property_keyed));
	section->get_vbox()->add_child(rotation_property);

	// Scale property.
	scale_property = memnew(EditorPropertyVector3());
	scale_property->setup(large_range_hint, true);
	scale_property->set_label("Scale");
	scale_property->set_selectable(false);
	scale_property->connect("property_changed", callable_mp(this, &BonePropertiesEditor::_value_changed));
	scale_property->connect("property_keyed", callable_mp(this, &BonePropertiesEditor::_property_keyed));
	section->get_vbox()->add_child(scale_property);

	// Transform/Matrix section.
	rest_section = memnew(EditorInspectorSection);
	rest_section->setup("trf_properties_transform", "Rest", this, Color(0.0f, 0.0f, 0.0f), true);
	section->get_vbox()->add_child(rest_section);

	// Transform/Matrix property.
	rest_matrix = memnew(EditorPropertyTransform3D());
	rest_matrix->setup(large_range_hint);
	rest_matrix->set_label("Transform");
	rest_matrix->set_selectable(false);
	rest_section->get_vbox()->add_child(rest_matrix);

	// Bone Metadata property
	meta_section = memnew(EditorInspectorSection);
	meta_section->setup("bone_meta", TTR("Bone Metadata"), this, Color(.0f, .0f, .0f), true);
	section->get_vbox()->add_child(meta_section);

	EditorInspectorActionButton *add_metadata_button = memnew(EditorInspectorActionButton(TTRC("Add Bone Metadata"), SNAME("Add")));
	add_metadata_button->connect(SceneStringName(pressed), callable_mp(this, &BonePropertiesEditor::_show_add_meta_dialog));
	section->get_vbox()->add_child(add_metadata_button);

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->connect("version_changed", callable_mp(this, &BonePropertiesEditor::_update_properties));
	undo_redo->connect("history_changed", callable_mp(this, &BonePropertiesEditor::_update_properties));
}

void BonePropertiesEditor::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			const Color section_color = get_theme_color(SNAME("prop_subsection"), EditorStringName(Editor));
			section->set_bg_color(section_color);
			rest_section->set_bg_color(section_color);
		} break;
	}
}

void BonePropertiesEditor::_value_changed(const String &p_property, const Variant &p_value, const String &p_name, bool p_changing) {
	if (updating || !skeleton) {
		return;
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Set Bone Transform"), UndoRedo::MERGE_ENDS);
	undo_redo->add_undo_property(skeleton, p_property, skeleton->get(p_property));
	undo_redo->add_do_property(skeleton, p_property, p_value);

	Skeleton3DEditor *se = Skeleton3DEditor::get_singleton();
	if (se) {
		undo_redo->add_do_method(se, "update_joint_tree");
		undo_redo->add_undo_method(se, "update_joint_tree");
	}

	undo_redo->commit_action();
}

void BonePropertiesEditor::_meta_changed(const String &p_property, const Variant &p_value, const String &p_name, bool p_changing) {
	if (!skeleton || p_property.get_slicec('/', 2) != "bone_meta") {
		return;
	}

	int bone = p_property.get_slicec('/', 1).to_int();
	if (bone >= skeleton->get_bone_count()) {
		return;
	}

	String key = p_property.get_slicec('/', 3);
	if (!skeleton->has_bone_meta(bone, key)) {
		return;
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(vformat(TTR("Modify metadata '%s' for bone '%s'"), key, skeleton->get_bone_name(bone)));
	undo_redo->add_do_property(skeleton, p_property, p_value);
	undo_redo->add_do_method(meta_editors[p_property], "update_property");
	undo_redo->add_undo_property(skeleton, p_property, skeleton->get_bone_meta(bone, key));
	undo_redo->add_undo_method(meta_editors[p_property], "update_property");
	undo_redo->commit_action();
}

void BonePropertiesEditor::_meta_deleted(const String &p_property) {
	if (!skeleton || p_property.get_slicec('/', 2) != "bone_meta") {
		return;
	}

	int bone = p_property.get_slicec('/', 1).to_int();
	if (bone >= skeleton->get_bone_count()) {
		return;
	}

	String key = p_property.get_slicec('/', 3);
	if (!skeleton->has_bone_meta(bone, key)) {
		return;
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(vformat(TTR("Remove metadata '%s' from bone '%s'"), key, skeleton->get_bone_name(bone)));
	undo_redo->add_do_property(skeleton, p_property, Variant());
	undo_redo->add_undo_property(skeleton, p_property, skeleton->get_bone_meta(bone, key));
	undo_redo->commit_action();

	emit_signal(SNAME("property_deleted"), p_property);
}

void BonePropertiesEditor::_show_add_meta_dialog() {
	if (!add_meta_dialog) {
		add_meta_dialog = memnew(AddMetadataDialog());
		add_meta_dialog->connect(SceneStringName(confirmed), callable_mp(this, &BonePropertiesEditor::_add_meta_confirm));
		add_child(add_meta_dialog);
	}

	int bone = Skeleton3DEditor::get_singleton()->get_selected_bone();
	StringName dialog_title = skeleton->get_bone_name(bone);

	List<StringName> existing_meta_keys;
	skeleton->get_bone_meta_list(bone, &existing_meta_keys);
	add_meta_dialog->open(dialog_title, existing_meta_keys);
}

void BonePropertiesEditor::_add_meta_confirm() {
	int bone = Skeleton3DEditor::get_singleton()->get_selected_bone();
	String name = add_meta_dialog->get_meta_name();
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(vformat(TTR("Add metadata '%s' to bone '%s'"), name, skeleton->get_bone_name(bone)));
	undo_redo->add_do_method(skeleton, "set_bone_meta", bone, name, add_meta_dialog->get_meta_defval());
	undo_redo->add_undo_method(skeleton, "set_bone_meta", bone, name, Variant());
	undo_redo->commit_action();
}

BonePropertiesEditor::BonePropertiesEditor(Skeleton3D *p_skeleton) {
	create_editors();
	set_skeleton(p_skeleton);
}

void BonePropertiesEditor::set_keyable(const bool p_keyable) {
	position_property->set_keying(p_keyable);
	rotation_property->set_keying(p_keyable);
	scale_property->set_keying(p_keyable);
}

void BonePropertiesEditor::set_skeleton(Skeleton3D *p_skeleton) {
	skeleton = p_skeleton;

	if (skeleton) {
		return;
	}

	enabled_checkbox->set_object_and_property(nullptr, String());
	position_property->set_object_and_property(nullptr, String());
	rotation_property->set_object_and_property(nullptr, String());
	scale_property->set_object_and_property(nullptr, String());
	rest_matrix->set_object_and_property(nullptr, String());

	for (KeyValue<StringName, EditorProperty *> &E : meta_editors) {
		E.value->set_object_and_property(nullptr, String());
	}
}

void BonePropertiesEditor::set_target(const String &p_prop) {
	enabled_checkbox->set_object_and_property(skeleton, p_prop + "enabled");
	enabled_checkbox->update_property();

	position_property->set_object_and_property(skeleton, p_prop + "position");
	position_property->update_property();

	rotation_property->set_object_and_property(skeleton, p_prop + "rotation");
	rotation_property->update_property();

	scale_property->set_object_and_property(skeleton, p_prop + "scale");
	scale_property->update_property();

	rest_matrix->set_object_and_property(skeleton, p_prop + "rest");
	rest_matrix->update_property();
}

void BonePropertiesEditor::_property_keyed(const String &p_path, bool p_advance) {
	AnimationTrackEditor *te = AnimationPlayerEditor::get_singleton()->get_track_editor();
	if (!te || !te->has_keying()) {
		return;
	}
	te->_clear_selection();
	PackedStringArray split = p_path.split("/");
	if (split.size() == 3 && split[0] == "bones") {
		int bone_idx = split[1].to_int();
		if (split[2] == "position") {
			te->insert_transform_key(skeleton, skeleton->get_bone_name(bone_idx), Animation::TYPE_POSITION_3D, (Vector3)skeleton->get(p_path) / skeleton->get_motion_scale());
		}
		if (split[2] == "rotation") {
			te->insert_transform_key(skeleton, skeleton->get_bone_name(bone_idx), Animation::TYPE_ROTATION_3D, skeleton->get(p_path));
		}
		if (split[2] == "scale") {
			te->insert_transform_key(skeleton, skeleton->get_bone_name(bone_idx), Animation::TYPE_SCALE_3D, skeleton->get(p_path));
		}
	}
}

void BonePropertiesEditor::_update_properties() {
	if (!skeleton || !Skeleton3DEditor::get_singleton()) {
		return;
	}
	int selected = Skeleton3DEditor::get_singleton()->get_selected_bone();
	List<PropertyInfo> props;
	HashSet<StringName> meta_seen;
	skeleton->get_property_list(&props);
	for (const PropertyInfo &E : props) {
		PackedStringArray split = E.name.split("/");
		if (split.size() >= 3 && split[0] == "bones") {
			if (split[1].to_int() == selected) {
				if (split[2] == "enabled") {
					enabled_checkbox->set_read_only(E.usage & PROPERTY_USAGE_READ_ONLY);
					enabled_checkbox->update_property();
					enabled_checkbox->update_editor_property_status();
					enabled_checkbox->queue_redraw();
				}
				if (split[2] == "position") {
					position_property->set_read_only(E.usage & PROPERTY_USAGE_READ_ONLY);
					position_property->update_property();
					position_property->update_editor_property_status();
					position_property->queue_redraw();
				}
				if (split[2] == "rotation") {
					rotation_property->set_read_only(E.usage & PROPERTY_USAGE_READ_ONLY);
					rotation_property->update_property();
					rotation_property->update_editor_property_status();
					rotation_property->queue_redraw();
				}
				if (split[2] == "scale") {
					scale_property->set_read_only(E.usage & PROPERTY_USAGE_READ_ONLY);
					scale_property->update_property();
					scale_property->update_editor_property_status();
					scale_property->queue_redraw();
				}
				if (split[2] == "rest") {
					rest_matrix->set_read_only(E.usage & PROPERTY_USAGE_READ_ONLY);
					rest_matrix->update_property();
					rest_matrix->update_editor_property_status();
					rest_matrix->queue_redraw();
				}
				if (split[2] == "bone_meta") {
					meta_seen.insert(E.name);
					if (!meta_editors.find(E.name)) {
						EditorProperty *editor = EditorInspectorDefaultPlugin::get_editor_for_property(skeleton, E.type, E.name, PROPERTY_HINT_NONE, "", E.usage);
						editor->set_label(split[3]);
						editor->set_object_and_property(skeleton, E.name);
						editor->set_deletable(true);
						editor->set_selectable(false);
						editor->connect("property_changed", callable_mp(this, &BonePropertiesEditor::_meta_changed));
						editor->connect("property_deleted", callable_mp(this, &BonePropertiesEditor::_meta_deleted));

						meta_section->get_vbox()->add_child(editor);
						editor->update_property();
						editor->update_editor_property_status();
						editor->queue_redraw();

						meta_editors[E.name] = editor;
					}
				}
			}
		}
	}
	// UI for any bone metadata prop not seen during the iteration has to be deleted
	for (KeyValue<StringName, EditorProperty *> iter : meta_editors) {
		if (!meta_seen.has(iter.key)) {
			callable_mp((Node *)meta_section->get_vbox(), &Node::remove_child).call_deferred(iter.value);
			meta_editors.remove(meta_editors.find(iter.key));
		}
	}
}

Skeleton3DEditor *Skeleton3DEditor::singleton = nullptr;

void Skeleton3DEditor::set_keyable(const bool p_keyable) {
	keyable = p_keyable;
	if (p_keyable) {
		animation_hb->show();
	} else {
		animation_hb->hide();
	}
}

void Skeleton3DEditor::set_bone_options_enabled(const bool p_bone_options_enabled) {
	skeleton_options->get_popup()->set_item_disabled(SKELETON_OPTION_RESET_SELECTED_POSES, !p_bone_options_enabled);
	skeleton_options->get_popup()->set_item_disabled(SKELETON_OPTION_SELECTED_POSES_TO_RESTS, !p_bone_options_enabled);
}

void Skeleton3DEditor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("update_all"), &Skeleton3DEditor::update_all);
	ClassDB::bind_method(D_METHOD("update_joint_tree"), &Skeleton3DEditor::update_joint_tree);
}

void Skeleton3DEditor::_on_click_skeleton_option(int p_skeleton_option) {
	ERR_FAIL_COND(!skeleton);

	switch (p_skeleton_option) {
		case SKELETON_OPTION_RESET_ALL_POSES: {
			reset_pose(true);
			break;
		}
		case SKELETON_OPTION_RESET_SELECTED_POSES: {
			reset_pose(false);
			break;
		}
		case SKELETON_OPTION_ALL_POSES_TO_RESTS: {
			pose_to_rest(true);
			break;
		}
		case SKELETON_OPTION_SELECTED_POSES_TO_RESTS: {
			pose_to_rest(false);
			break;
		}
		case SKELETON_OPTION_CREATE_PHYSICAL_SKELETON: {
			create_physical_skeleton();
			break;
		}
		case SKELETON_OPTION_EXPORT_SKELETON_PROFILE: {
			export_skeleton_profile();
			break;
		}
	}
}

void Skeleton3DEditor::reset_pose(const bool p_all_bones) {
	ERR_FAIL_COND(!skeleton);

	const int bone_count = skeleton->get_bone_count();
	if (!bone_count) {
		return;
	}

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Set Bone Transform"), UndoRedo::MERGE_ENDS);
	if (p_all_bones) {
		for (int i = 0; i < bone_count; i++) {
			ur->add_undo_method(skeleton, "set_bone_pose_position", i, skeleton->get_bone_pose_position(i));
			ur->add_undo_method(skeleton, "set_bone_pose_rotation", i, skeleton->get_bone_pose_rotation(i));
			ur->add_undo_method(skeleton, "set_bone_pose_scale", i, skeleton->get_bone_pose_scale(i));
		}
		ur->add_do_method(skeleton, "reset_bone_poses");
	} else {
		const Vector<int> bones = _get_affected_bones(false);
		if (bones.is_empty()) {
			ur->commit_action();
			return;
		}
		for (int bone : bones) {
			ur->add_undo_method(skeleton, "set_bone_pose_position", bone, skeleton->get_bone_pose_position(bone));
			ur->add_undo_method(skeleton, "set_bone_pose_rotation", bone, skeleton->get_bone_pose_rotation(bone));
			ur->add_undo_method(skeleton, "set_bone_pose_scale", bone, skeleton->get_bone_pose_scale(bone));
			ur->add_do_method(skeleton, "reset_bone_pose", bone);
		}
	}

	ur->add_undo_method(this, "update_joint_tree");
	ur->add_do_method(this, "update_joint_tree");

	ur->commit_action();
}

void Skeleton3DEditor::insert_keys(const bool p_all_bones, const bool p_enable_modifier) {
	ERR_FAIL_COND(!skeleton);

	AnimationTrackEditor *te = AnimationPlayerEditor::get_singleton()->get_track_editor();
	bool is_read_only = te->is_read_only();
	if (is_read_only) {
		te->popup_read_only_dialog();
		return;
	}
	if (p_enable_modifier) {
		if (!skeleton->is_connected(SceneStringName(skeleton_updated), callable_mp(this, &Skeleton3DEditor::_insert_keys).bind(p_all_bones))) {
			skeleton->connect(SceneStringName(skeleton_updated), callable_mp(this, &Skeleton3DEditor::_insert_keys).bind(p_all_bones), CONNECT_ONE_SHOT);
		} else {
			WARN_PRINT_ED("A skeleton_updated signal is already connected with _insert_keys().");
		}
		skeleton->force_update_deferred();
		skeleton->notification(Skeleton3D::NOTIFICATION_UPDATE_SKELETON);
		// Force disconnecting signal if remain the connecting just in case.
		if (skeleton->is_connected(SceneStringName(skeleton_updated), callable_mp(this, &Skeleton3DEditor::_insert_keys).bind(p_all_bones))) {
			skeleton->disconnect(SceneStringName(skeleton_updated), callable_mp(this, &Skeleton3DEditor::_insert_keys).bind(p_all_bones));
		}
	} else {
		_insert_keys(p_all_bones);
	}
}

void Skeleton3DEditor::_insert_keys(const bool p_all_bones) {
	ERR_FAIL_COND(!skeleton);

	bool pos_enabled = key_loc_button->is_pressed();
	bool rot_enabled = key_rot_button->is_pressed();
	bool scl_enabled = key_scale_button->is_pressed();

	int bone_len = skeleton->get_bone_count();
	Node *root = EditorNode::get_singleton()->get_tree()->get_root();
	String path = String(root->get_path_to(skeleton));

	// Asosdot: with bones selected, only those bones are keyed.
	HashSet<int> only_bones;
	for (int bone : get_selected_bones(skeleton)) {
		only_bones.insert(bone);
	}

	AnimationTrackEditor *te = AnimationPlayerEditor::get_singleton()->get_track_editor();
	te->make_insert_queue();
	for (int i = 0; i < bone_len; i++) {
		const String name = skeleton->get_bone_name(i);

		if (name.is_empty() || (!only_bones.is_empty() && !only_bones.has(i))) {
			continue;
		}

		if (pos_enabled && (p_all_bones || te->has_track(skeleton, name, Animation::TYPE_POSITION_3D))) {
			te->insert_transform_key(skeleton, name, Animation::TYPE_POSITION_3D, skeleton->get_bone_pose_position(i) / skeleton->get_motion_scale());
		}
		if (rot_enabled && (p_all_bones || te->has_track(skeleton, name, Animation::TYPE_ROTATION_3D))) {
			te->insert_transform_key(skeleton, name, Animation::TYPE_ROTATION_3D, skeleton->get_bone_pose_rotation(i));
		}
		if (scl_enabled && (p_all_bones || te->has_track(skeleton, name, Animation::TYPE_SCALE_3D))) {
			te->insert_transform_key(skeleton, name, Animation::TYPE_SCALE_3D, skeleton->get_bone_pose_scale(i));
		}
	}
	te->commit_insert_queue();
}

void Skeleton3DEditor::pose_to_rest(const bool p_all_bones) {
	ERR_FAIL_COND(!skeleton);

	const int bone_count = skeleton->get_bone_count();
	if (!bone_count) {
		return;
	}

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Set Bone Rest"), UndoRedo::MERGE_ENDS);
	if (p_all_bones) {
		for (int i = 0; i < bone_count; i++) {
			ur->add_do_method(skeleton, "set_bone_rest", i, skeleton->get_bone_pose(i));
			ur->add_undo_method(skeleton, "set_bone_rest", i, skeleton->get_bone_rest(i));
		}
	} else {
		const Vector<int> bones = _get_affected_bones(false);
		if (bones.is_empty()) {
			ur->commit_action();
			return;
		}
		for (int bone : bones) {
			ur->add_do_method(skeleton, "set_bone_rest", bone, skeleton->get_bone_pose(bone));
			ur->add_undo_method(skeleton, "set_bone_rest", bone, skeleton->get_bone_rest(bone));
		}
	}

	ur->add_undo_method(this, "update_joint_tree");
	ur->add_do_method(this, "update_joint_tree");

	ur->commit_action();
}

void Skeleton3DEditor::create_physical_skeleton() {
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ERR_FAIL_NULL(get_tree());
	Node *owner = get_tree()->get_edited_scene_root();

	const int bone_count = skeleton->get_bone_count();

	if (!bone_count) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("Cannot create a physical skeleton for a Skeleton3D node with no bones.")));
		return;
	}

	Vector<BoneInfo> bones_infos;
	bones_infos.resize(bone_count);

	ur->create_action(TTR("Create physical bones"), UndoRedo::MERGE_ALL);

	PhysicalBoneSimulator3D *simulator = memnew(PhysicalBoneSimulator3D);
	ur->add_do_method(skeleton, "add_child", simulator);
	ur->add_do_method(simulator, "set_owner", owner);
	ur->add_do_method(simulator, "set_name", "PhysicalBoneSimulator3D");
	for (int bone_id = 0; bone_count > bone_id; ++bone_id) {
		const int parent = skeleton->get_bone_parent(bone_id);

		if (parent < 0) {
			bones_infos.write[bone_id].relative_rest = skeleton->get_bone_rest(bone_id);
		} else {
			const int parent_parent = skeleton->get_bone_parent(parent);

			bones_infos.write[bone_id].relative_rest = bones_infos[parent].relative_rest * skeleton->get_bone_rest(bone_id);

			// Create physical bone on parent.
			if (!bones_infos[parent].physical_bone) {
				PhysicalBone3D *physical_bone = create_physical_bone(parent, bone_id, bones_infos);
				if (physical_bone && physical_bone->get_child(0)) {
					CollisionShape3D *collision_shape = Object::cast_to<CollisionShape3D>(physical_bone->get_child(0));
					if (collision_shape) {
						bones_infos.write[parent].physical_bone = physical_bone;

						ur->add_do_method(simulator, "add_child", physical_bone);
						ur->add_do_method(physical_bone, "set_owner", owner);
						ur->add_do_method(collision_shape, "set_owner", owner);
						ur->add_do_property(physical_bone, "bone_name", skeleton->get_bone_name(parent));

						// Create joint between parent of parent.
						if (parent_parent != -1) {
							ur->add_do_method(physical_bone, "set_joint_type", PhysicalBone3D::JOINT_TYPE_PIN);
						}

						ur->add_do_method(Node3DEditor::get_singleton(), SceneStringName(_request_gizmo), physical_bone);
						ur->add_do_method(Node3DEditor::get_singleton(), SceneStringName(_request_gizmo), collision_shape);

						ur->add_do_reference(physical_bone);
						ur->add_undo_method(simulator, "remove_child", physical_bone);
					}
				}
			}
		}
	}
	ur->add_undo_method(skeleton, "remove_child", simulator);
	ur->commit_action();
}

PhysicalBone3D *Skeleton3DEditor::create_physical_bone(int bone_id, int bone_child_id, const Vector<BoneInfo> &bones_infos) {
	const Transform3D child_rest = skeleton->get_bone_rest(bone_child_id);

	const real_t half_height(child_rest.origin.length() * 0.5);
	const real_t radius(half_height * 0.2);

	Ref<CapsuleShape3D> bone_shape_capsule;
	bone_shape_capsule.instantiate();
	bone_shape_capsule->set_height(half_height * 2);
	bone_shape_capsule->set_radius(radius);

	CollisionShape3D *bone_shape = memnew(CollisionShape3D);
	bone_shape->set_shape(bone_shape_capsule);
	bone_shape->set_name("CollisionShape3D");

	Transform3D capsule_transform;
	capsule_transform.basis.rows[0] = Vector3(1, 0, 0);
	capsule_transform.basis.rows[1] = Vector3(0, 0, 1);
	capsule_transform.basis.rows[2] = Vector3(0, -1, 0);
	bone_shape->set_transform(capsule_transform);

	/// Get an up vector not collinear with child rest origin
	Vector3 up = Vector3(0, 1, 0);
	if (up.cross(child_rest.origin).is_zero_approx()) {
		up = Vector3(0, 0, 1);
	}

	Transform3D body_transform;
	body_transform.basis = Basis::looking_at(child_rest.origin, up);
	body_transform.origin = body_transform.basis.xform(Vector3(0, 0, -half_height));

	Transform3D joint_transform;
	joint_transform.origin = Vector3(0, 0, half_height);

	PhysicalBone3D *physical_bone = memnew(PhysicalBone3D);
	physical_bone->add_child(bone_shape);
	physical_bone->set_name("Physical Bone " + skeleton->get_bone_name(bone_id));
	physical_bone->set_body_offset(body_transform);
	physical_bone->set_joint_offset(joint_transform);
	return physical_bone;
}

void Skeleton3DEditor::export_skeleton_profile() {
	if (!skeleton->get_bone_count()) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("Cannot export a SkeletonProfile for a Skeleton3D node with no bones.")));
		return;
	}

	file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	file_dialog->set_title(TTR("Export Skeleton Profile As..."));

	List<String> exts;
	ResourceLoader::get_recognized_extensions_for_type("SkeletonProfile", &exts);
	file_dialog->clear_filters();
	for (const String &K : exts) {
		file_dialog->add_filter("*." + K);
	}

	file_dialog->popup_file_dialog();
}

void Skeleton3DEditor::_file_selected(const String &p_file) {
	// Export SkeletonProfile.
	Ref<SkeletonProfile> sp(memnew(SkeletonProfile));

	// Build SkeletonProfile.
	sp->set_group_size(1);

	Vector<Vector2> handle_positions;
	Vector2 position_max;
	Vector2 position_min;

	const int bone_count = skeleton->get_bone_count();
	sp->set_bone_size(bone_count);
	for (int i = 0; i < bone_count; i++) {
		sp->set_bone_name(i, skeleton->get_bone_name(i));
		int parent = skeleton->get_bone_parent(i);
		if (parent >= 0) {
			sp->set_bone_parent(i, skeleton->get_bone_name(parent));
		}
		sp->set_reference_pose(i, skeleton->get_bone_rest(i));

		Transform3D grest = skeleton->get_bone_global_rest(i);
		handle_positions.append(Vector2(grest.origin.x, grest.origin.y));
		if (i == 0) {
			position_max = Vector2(grest.origin.x, grest.origin.y);
			position_min = Vector2(grest.origin.x, grest.origin.y);
		} else {
			position_max = position_max.max(Vector2(grest.origin.x, grest.origin.y));
			position_min = position_min.min(Vector2(grest.origin.x, grest.origin.y));
		}
	}

	// Layout handles provisionaly.
	Vector2 bound = Vector2(position_max.x - position_min.x, position_max.y - position_min.y);
	Vector2 center = Vector2((position_max.x + position_min.x) * 0.5, (position_max.y + position_min.y) * 0.5);
	float nrm = MAX(bound.x, bound.y);
	if (nrm > 0) {
		for (int i = 0; i < bone_count; i++) {
			handle_positions.write[i] = (handle_positions[i] - center) / nrm * 0.9;
			sp->set_handle_offset(i, Vector2(0.5 + handle_positions[i].x, 0.5 - handle_positions[i].y));
		}
	}

	Error err = ResourceSaver::save(sp, p_file);

	if (err != OK) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("Error saving file: %s"), p_file));
		return;
	}
}

Variant Skeleton3DEditor::get_drag_data_fw(const Point2 &p_point, Control *p_from) {
	TreeItem *selected = joint_tree->get_selected();

	if (!selected) {
		return Variant();
	}

	Ref<Texture> icon = selected->get_icon(0);

	VBoxContainer *vb = memnew(VBoxContainer);
	HBoxContainer *hb = memnew(HBoxContainer);
	TextureRect *tf = memnew(TextureRect);
	tf->set_texture(icon);
	tf->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
	hb->add_child(tf);
	Label *label = memnew(Label(selected->get_text(0)));
	label->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	hb->add_child(label);
	vb->add_child(hb);
	hb->set_modulate(Color(1, 1, 1, 1));

	set_drag_preview(vb);
	Dictionary drag_data;
	drag_data["type"] = "skeleton_bone";
	drag_data["source"] = selected->get_instance_id();

	return drag_data;
}

bool Skeleton3DEditor::can_drop_data_fw(const Point2 &p_point, const Variant &p_data, Control *p_from) const {
	Dictionary drag_data = p_data;
	if (!drag_data.has("type") || drag_data["type"] != "skeleton_bone" || !drag_data.has("source")) {
		return false;
	}

	TreeItem *target = (p_point == Vector2(Math::INF, Math::INF)) ? joint_tree->get_selected() : joint_tree->get_item_at_position(p_point);
	if (!target) {
		return false;
	}

	const String path = target->get_metadata(0);
	if (!path.begins_with("bones/")) {
		return false;
	}

	TreeItem *selected = ObjectDB::get_instance<TreeItem>(drag_data["source"]);
	if (!selected || selected->get_tree() != joint_tree || target == selected) {
		return false;
	}

	const String selected_path = selected->get_metadata(0);
	if (!selected_path.begins_with("bones/")) {
		return false;
	}

	return true;
}

void Skeleton3DEditor::drop_data_fw(const Point2 &p_point, const Variant &p_data, Control *p_from) {
	if (!can_drop_data_fw(p_point, p_data, p_from)) {
		return;
	}

	Dictionary drag_data = p_data;

	TreeItem *target = (p_point == Vector2(Math::INF, Math::INF)) ? joint_tree->get_selected() : joint_tree->get_item_at_position(p_point);
	TreeItem *selected = ObjectDB::get_instance<TreeItem>(drag_data["source"]);
	if (!target || !selected || selected->get_tree() != joint_tree || !skeleton) {
		return;
	}

	const String target_path = target->get_metadata(0);
	const String selected_path = selected->get_metadata(0);
	if (!target_path.begins_with("bones/") || !selected_path.begins_with("bones/")) {
		return;
	}

	const BoneId target_boneidx = target_path.get_slicec('/', 1).to_int();
	const BoneId selected_boneidx = selected_path.get_slicec('/', 1).to_int();

	move_skeleton_bone(skeleton->get_path(), selected_boneidx, target_boneidx);
}

void Skeleton3DEditor::move_skeleton_bone(NodePath p_skeleton_path, int32_t p_selected_boneidx, int32_t p_target_boneidx) {
	Node *node = get_node_or_null(p_skeleton_path);
	Skeleton3D *skeleton_node = Object::cast_to<Skeleton3D>(node);
	ERR_FAIL_NULL(skeleton_node);
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Set Bone Parentage"));
	// If the target is a child of ourselves, we move only *us* and not our children.
	if (skeleton_node->is_bone_parent_of(p_target_boneidx, p_selected_boneidx)) {
		const BoneId parent_idx = skeleton_node->get_bone_parent(p_selected_boneidx);
		const int bone_count = skeleton_node->get_bone_count();
		for (BoneId i = 0; i < bone_count; ++i) {
			if (skeleton_node->get_bone_parent(i) == p_selected_boneidx) {
				ur->add_undo_method(skeleton_node, "set_bone_parent", i, skeleton_node->get_bone_parent(i));
				ur->add_do_method(skeleton_node, "set_bone_parent", i, parent_idx);
				skeleton_node->set_bone_parent(i, parent_idx);
			}
		}
	}
	ur->add_undo_method(skeleton_node, "set_bone_parent", p_selected_boneidx, skeleton_node->get_bone_parent(p_selected_boneidx));
	ur->add_do_method(skeleton_node, "set_bone_parent", p_selected_boneidx, p_target_boneidx);

	ur->add_undo_method(this, "update_joint_tree");
	ur->add_do_method(this, "update_joint_tree");

	skeleton_node->set_bone_parent(p_selected_boneidx, p_target_boneidx);

	ur->commit_action();
}

// The inspector, the bone options and the gizmo follow the active bone.
void Skeleton3DEditor::_joint_tree_selection_changed() {
	const bool has_bone = skeleton && selected_bone >= 0 && selected_bone < skeleton->get_bone_count();
	if (has_bone && pose_editor) {
		pose_editor->set_target("bones/" + itos(selected_bone) + "/");
		pose_editor->set_keyable(keyable);
	}

	if (pose_editor && pose_editor->is_inside_tree()) {
		pose_editor->set_visible(has_bone);
	}
	set_bone_options_enabled(!get_selected_bones(skeleton).is_empty());

	_update_properties();
	_update_gizmo_visible();
}

HashMap<ObjectID, Skeleton3DEditor::BoneSelection> Skeleton3DEditor::bone_selections;

Vector<int> Skeleton3DEditor::get_selected_bones(const Skeleton3D *p_skeleton) {
	Vector<int> bones;
	if (!p_skeleton) {
		return bones;
	}
	const BoneSelection *selection = bone_selections.getptr(p_skeleton->get_instance_id());
	if (selection) {
		const int bone_count = p_skeleton->get_bone_count();
		for (int bone : selection->bones) {
			if (bone < bone_count) { // Bones can be removed after they were selected.
				bones.push_back(bone);
			}
		}
	}
	return bones;
}

bool Skeleton3DEditor::is_bone_selected(const Skeleton3D *p_skeleton, int p_bone) {
	if (!p_skeleton || p_bone < 0 || p_bone >= p_skeleton->get_bone_count()) {
		return false;
	}
	const BoneSelection *selection = bone_selections.getptr(p_skeleton->get_instance_id());
	return selection && selection->bones.has(p_bone);
}

int Skeleton3DEditor::get_active_bone(const Skeleton3D *p_skeleton) {
	if (!p_skeleton) {
		return -1;
	}
	const BoneSelection *selection = bone_selections.getptr(p_skeleton->get_instance_id());
	return selection && selection->active < p_skeleton->get_bone_count() ? selection->active : -1;
}

void Skeleton3DEditor::set_selected_bones(Skeleton3D *p_skeleton, const Vector<int> &p_bones, int p_active) {
	ERR_FAIL_NULL(p_skeleton);

	BoneSelection selection;
	const int bone_count = p_skeleton->get_bone_count();
	for (int bone : p_bones) {
		if (bone >= 0 && bone < bone_count && !selection.bones.has(bone)) {
			selection.bones.push_back(bone);
		}
	}
	if (selection.bones.has(p_active)) {
		selection.active = p_active;
	} else if (!selection.bones.is_empty()) {
		selection.active = selection.bones[selection.bones.size() - 1];
	}

	const ObjectID id = p_skeleton->get_instance_id();
	if (selection.bones.is_empty()) {
		bone_selections.erase(id);
	} else {
		bone_selections[id] = selection;
	}

	if (singleton && singleton->skeleton == p_skeleton) {
		singleton->_apply_bone_selection();
	}
	p_skeleton->update_gizmos();

	AnimationPlayerEditor *player_editor = AnimationPlayerEditor::get_singleton();
	if (player_editor && player_editor->get_track_editor()) {
		player_editor->get_track_editor()->bone_selection_changed();
	}
}

// Shows the stored selection in the bone tree, the inspector and the viewport.
void Skeleton3DEditor::_apply_bone_selection() {
	if (!skeleton) {
		return;
	}
	selected_bone = get_active_bone(skeleton);

	updating_tree_selection = true;
	joint_tree->deselect_all();
	TreeItem *active_item = nullptr;
	for (int bone : get_selected_bones(skeleton)) {
		TreeItem *ti = _find(joint_tree->get_root(), "bones/" + itos(bone));
		if (!ti) {
			continue;
		}
		// Make visible when it's collapsed.
		for (TreeItem *node = ti->get_parent(); node && node != joint_tree->get_root(); node = node->get_parent()) {
			node->set_collapsed(false);
		}
		if (bone == selected_bone) {
			active_item = ti;
		} else {
			ti->select(0, false);
		}
	}
	if (active_item) {
		active_item->select(0, true); // The active bone is the tree's cursor.
		joint_tree->scroll_to_item(active_item);
	}
	updating_tree_selection = false;

	update_bone_original();
	_joint_tree_selection_changed();
}

void Skeleton3DEditor::_joint_tree_multi_selected(Object *p_item, int p_column, bool p_selected) {
	if (updating_tree_selection || tree_selection_pending) {
		return;
	}
	// One click sends a signal for every row it selects or deselects; read the result once.
	tree_selection_pending = true;
	callable_mp(this, &Skeleton3DEditor::_apply_tree_selection).call_deferred();
}

void Skeleton3DEditor::_apply_tree_selection() {
	tree_selection_pending = false;
	if (!skeleton) {
		return;
	}
	Vector<int> bones;
	for (TreeItem *item = joint_tree->get_next_selected(joint_tree->get_root()); item; item = joint_tree->get_next_selected(item)) {
		const String path = item->get_metadata(0);
		if (path.begins_with("bones/")) {
			bones.push_back(path.get_slicec('/', 1).to_int());
		}
	}
	int active = -1;
	TreeItem *cursor = joint_tree->get_selected();
	if (cursor && cursor->is_selected(0)) {
		const String path = cursor->get_metadata(0);
		if (path.begins_with("bones/")) {
			active = path.get_slicec('/', 1).to_int();
		}
	}
	set_selected_bones(skeleton, bones, active);
}

Vector<int> Skeleton3DEditor::_get_affected_bones(bool p_all_bones) const {
	if (!p_all_bones) {
		return get_selected_bones(skeleton);
	}
	Vector<int> bones;
	for (int i = 0; skeleton && i < skeleton->get_bone_count(); i++) {
		bones.push_back(i);
	}
	return bones;
}

// The viewport keeps its own list of selected subgizmos, so it is told every selected bone.
void Skeleton3DEditor::_sync_viewport_selection() {
	if (!skeleton || !edit_mode) {
		return;
	}
	Node3DEditorSelectedItem *se = EditorNode::get_singleton()->get_editor_selection()->get_node_editor_data<Node3DEditorSelectedItem>(skeleton);
	if (!se) {
		return;
	}
	Ref<EditorNode3DGizmo> skeleton_gizmo;
	for (const Ref<Node3DGizmo> &gizmo : skeleton->get_gizmos()) {
		Ref<EditorNode3DGizmo> editor_gizmo = gizmo;
		if (editor_gizmo.is_valid() && Object::cast_to<Skeleton3DGizmoPlugin>(editor_gizmo->get_plugin().ptr())) {
			skeleton_gizmo = editor_gizmo;
			break;
		}
	}

	se->subgizmos.clear();
	const Vector<int> bones = get_selected_bones(skeleton);
	// The active bone goes first, so a single-bone gizmo sits on it.
	if (selected_bone >= 0) {
		se->subgizmos.insert(selected_bone, skeleton->get_bone_global_pose(selected_bone));
	}
	for (int bone : bones) {
		if (!se->subgizmos.has(bone)) {
			se->subgizmos.insert(bone, skeleton->get_bone_global_pose(bone));
		}
	}
	se->gizmo = se->subgizmos.is_empty() || skeleton_gizmo.is_null() ? Ref<EditorNode3DGizmo>() : skeleton_gizmo;
	if (se->gizmo.is_null()) {
		se->subgizmos.clear();
	}
	skeleton->update_gizmos();
	Node3DEditor::get_singleton()->update_transform_gizmo();
}

bool Skeleton3DEditor::get_bone_original(int p_bone, Vector3 &r_position, Quaternion &r_rotation, Vector3 &r_scale) const {
	const BonePose *pose = bone_originals.getptr(p_bone);
	if (pose) {
		r_position = pose->position;
		r_rotation = pose->rotation;
		r_scale = pose->scale;
		return true;
	}
	if (skeleton && p_bone >= 0 && p_bone < skeleton->get_bone_count()) {
		r_position = skeleton->get_bone_pose_position(p_bone);
		r_rotation = skeleton->get_bone_pose_rotation(p_bone);
		r_scale = skeleton->get_bone_pose_scale(p_bone);
	}
	return false;
}

// May be not used with single select mode.
void Skeleton3DEditor::_joint_tree_rmb_select(const Vector2 &p_pos, MouseButton p_button) {
}

void Skeleton3DEditor::_joint_tree_button_clicked(Object *p_item, int p_column, int p_id, MouseButton p_button) {
	if (!skeleton) {
		return;
	}

	TreeItem *tree_item = Object::cast_to<TreeItem>(p_item);
	if (tree_item) {
		String tree_item_metadata = tree_item->get_metadata(0);

		String bone_enabled_property = tree_item_metadata + "/enabled";
		String bone_parent_property = tree_item_metadata + "/parent";
		String bone_name_property = tree_item_metadata + "/name";
		String bone_position_property = tree_item_metadata + "/position";
		String bone_rotation_property = tree_item_metadata + "/rotation";
		String bone_scale_property = tree_item_metadata + "/scale";
		String bone_rest_property = tree_item_metadata + "/rest";

		Variant current_enabled = skeleton->get(bone_enabled_property);
		Variant current_parent = skeleton->get(bone_parent_property);
		Variant current_name = skeleton->get(bone_name_property);
		Variant current_position = skeleton->get(bone_position_property);
		Variant current_rotation = skeleton->get(bone_rotation_property);
		Variant current_scale = skeleton->get(bone_scale_property);
		Variant current_rest = skeleton->get(bone_rest_property);

		EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
		ur->create_action(TTR("Revert Bone"));

		bool can_revert_enabled = EditorPropertyRevert::can_property_revert(skeleton, bone_enabled_property, &current_enabled);
		if (can_revert_enabled) {
			bool is_valid = false;
			Variant new_enabled = EditorPropertyRevert::get_property_revert_value(skeleton, bone_enabled_property, &is_valid);
			if (is_valid) {
				ur->add_undo_method(skeleton, "set", bone_enabled_property, current_enabled);
				ur->add_do_method(skeleton, "set", bone_enabled_property, new_enabled);
			}
		}

		bool can_revert_parent = EditorPropertyRevert::can_property_revert(skeleton, bone_parent_property, &current_parent);
		if (can_revert_parent) {
			bool is_valid = false;
			Variant new_parent = EditorPropertyRevert::get_property_revert_value(skeleton, bone_parent_property, &is_valid);
			if (is_valid) {
				ur->add_undo_method(skeleton, "set", bone_parent_property, current_parent);
				ur->add_do_method(skeleton, "set", bone_parent_property, new_parent);
			}
		}
		bool can_revert_name = EditorPropertyRevert::can_property_revert(skeleton, bone_name_property, &current_name);
		if (can_revert_name) {
			bool is_valid = false;
			Variant new_name = EditorPropertyRevert::get_property_revert_value(skeleton, bone_name_property, &is_valid);
			if (is_valid) {
				ur->add_undo_method(skeleton, "set", bone_name_property, current_name);
				ur->add_do_method(skeleton, "set", bone_name_property, new_name);
			}
		}
		bool can_revert_position = EditorPropertyRevert::can_property_revert(skeleton, bone_position_property, &current_position);
		if (can_revert_position) {
			bool is_valid = false;
			Variant new_position = EditorPropertyRevert::get_property_revert_value(skeleton, bone_position_property, &is_valid);
			if (is_valid) {
				ur->add_undo_method(skeleton, "set", bone_position_property, current_position);
				ur->add_do_method(skeleton, "set", bone_position_property, new_position);
			}
		}
		bool can_revert_rotation = EditorPropertyRevert::can_property_revert(skeleton, bone_rotation_property, &current_rotation);
		if (can_revert_rotation) {
			bool is_valid = false;
			Variant new_rotation = EditorPropertyRevert::get_property_revert_value(skeleton, bone_rotation_property, &is_valid);
			if (is_valid) {
				ur->add_undo_method(skeleton, "set", bone_rotation_property, current_rotation);
				ur->add_do_method(skeleton, "set", bone_rotation_property, new_rotation);
			}
		}
		bool can_revert_scale = EditorPropertyRevert::can_property_revert(skeleton, bone_scale_property, &current_scale);
		if (can_revert_scale) {
			bool is_valid = false;
			Variant new_scale = EditorPropertyRevert::get_property_revert_value(skeleton, bone_scale_property, &is_valid);
			if (is_valid) {
				ur->add_undo_method(skeleton, "set", bone_scale_property, current_scale);
				ur->add_do_method(skeleton, "set", bone_scale_property, new_scale);
			}
		}
		bool can_revert_rest = EditorPropertyRevert::can_property_revert(skeleton, bone_rest_property, &current_rest);
		if (can_revert_rest) {
			bool is_valid = false;
			Variant new_rest = EditorPropertyRevert::get_property_revert_value(skeleton, bone_rest_property, &is_valid);
			if (is_valid) {
				ur->add_undo_method(skeleton, "set", bone_rest_property, current_rest);
				ur->add_do_method(skeleton, "set", bone_rest_property, new_rest);
			}
		}

		ur->add_undo_method(this, "update_all");
		ur->add_do_method(this, "update_all");

		ur->commit_action();
	}
	return;
}

void Skeleton3DEditor::_update_properties() {
	if (pose_editor) {
		pose_editor->_update_properties();
	}
	if (!skeleton || !skeleton->is_inside_tree()) {
		return;
	}
	Node3DEditor::get_singleton()->update_transform_gizmo();
}

void Skeleton3DEditor::update_joint_tree() {
	joint_tree->clear();

	if (!skeleton) {
		return;
	}

	TreeItem *root = joint_tree->create_item();

	HashMap<int, TreeItem *> items;

	items.insert(-1, root);

	Ref<Texture> bone_icon = get_editor_theme_icon(SNAME("Bone"));

	Vector<int> bones_to_process = skeleton->get_parentless_bones();
	while (bones_to_process.size() > 0) {
		int current_bone_idx = bones_to_process[0];
		bones_to_process.erase(current_bone_idx);

		const int parent_idx = skeleton->get_bone_parent(current_bone_idx);
		TreeItem *parent_item = items.find(parent_idx)->value;

		TreeItem *joint_item = joint_tree->create_item(parent_item);
		items.insert(current_bone_idx, joint_item);

		joint_item->set_text(0, skeleton->get_bone_name(current_bone_idx));
		joint_item->set_icon(0, bone_icon);
		joint_item->set_selectable(0, true);
		joint_item->set_metadata(0, "bones/" + itos(current_bone_idx));

		String bone_enabled_property = "bones/" + itos(current_bone_idx) + "/enabled";
		String bone_parent_property = "bones/" + itos(current_bone_idx) + "/parent";
		String bone_name_property = "bones/" + itos(current_bone_idx) + "/name";
		String bone_position_property = "bones/" + itos(current_bone_idx) + "/position";
		String bone_rotation_property = "bones/" + itos(current_bone_idx) + "/rotation";
		String bone_scale_property = "bones/" + itos(current_bone_idx) + "/scale";
		String bone_rest_property = "bones/" + itos(current_bone_idx) + "/rest";

		Variant current_enabled = skeleton->get(bone_enabled_property);
		Variant current_parent = skeleton->get(bone_parent_property);
		Variant current_name = skeleton->get(bone_name_property);
		Variant current_position = skeleton->get(bone_position_property);
		Variant current_rotation = skeleton->get(bone_rotation_property);
		Variant current_scale = skeleton->get(bone_scale_property);
		Variant current_rest = skeleton->get(bone_rest_property);

		bool can_revert_enabled = EditorPropertyRevert::can_property_revert(skeleton, bone_enabled_property, &current_enabled);
		bool can_revert_parent = EditorPropertyRevert::can_property_revert(skeleton, bone_parent_property, &current_parent);
		bool can_revert_name = EditorPropertyRevert::can_property_revert(skeleton, bone_name_property, &current_name);
		bool can_revert_position = EditorPropertyRevert::can_property_revert(skeleton, bone_position_property, &current_position);
		bool can_revert_rotation = EditorPropertyRevert::can_property_revert(skeleton, bone_rotation_property, &current_rotation);
		bool can_revert_scale = EditorPropertyRevert::can_property_revert(skeleton, bone_scale_property, &current_scale);
		bool can_revert_rest = EditorPropertyRevert::can_property_revert(skeleton, bone_rest_property, &current_rest);

		if (can_revert_enabled || can_revert_parent || can_revert_name || can_revert_position || can_revert_rotation || can_revert_scale || can_revert_rest) {
			joint_item->add_button(0, get_editor_theme_icon(SNAME("ReloadSmall")), JOINT_BUTTON_REVERT, false, TTR("Revert"));
		}

		// Add the bone's children to the list of bones to be processed.
		Vector<int> current_bone_child_bones = skeleton->get_bone_children(current_bone_idx);
		int child_bone_size = current_bone_child_bones.size();
		for (int i = 0; i < child_bone_size; i++) {
			bones_to_process.push_back(current_bone_child_bones[i]);
		}

		if (is_bone_selected(skeleton, current_bone_idx)) {
			updating_tree_selection = true;
			joint_item->select(0, current_bone_idx == selected_bone);
			updating_tree_selection = false;
		}
	}
}

void Skeleton3DEditor::update_all() {
	_update_properties();
	update_joint_tree();
}

void Skeleton3DEditor::create_editors() {
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_focus_mode(FOCUS_ALL);

	Node3DEditor *ne = Node3DEditor::get_singleton();
	AnimationTrackEditor *te = AnimationPlayerEditor::get_singleton()->get_track_editor();

	// Create File dialog.
	file_dialog = memnew(EditorFileDialog);
	file_dialog->connect("file_selected", callable_mp(this, &Skeleton3DEditor::_file_selected));
	add_child(file_dialog);

	// Create Top Menu Bar.
	topmenu_bar = memnew(HBoxContainer);
	ne->add_control_to_menu_panel(topmenu_bar);

	// Create Skeleton Option in Top Menu Bar.
	skeleton_options = memnew(MenuButton);
	skeleton_options->set_flat(false);
	skeleton_options->set_theme_type_variation("FlatMenuButtonNoIconTint");
	topmenu_bar->add_child(skeleton_options);

	skeleton_options->set_text(TTR("Skeleton3D"));

	// Skeleton options.
	PopupMenu *p = skeleton_options->get_popup();
	p->add_shortcut(ED_SHORTCUT("skeleton_3d_editor/reset_all_poses", TTRC("Reset All Bone Poses")), SKELETON_OPTION_RESET_ALL_POSES);
	p->add_shortcut(ED_SHORTCUT("skeleton_3d_editor/reset_selected_poses", TTRC("Reset Selected Poses")), SKELETON_OPTION_RESET_SELECTED_POSES);
	p->add_shortcut(ED_SHORTCUT("skeleton_3d_editor/all_poses_to_rests", TTRC("Apply All Poses to Rests")), SKELETON_OPTION_ALL_POSES_TO_RESTS);
	p->add_shortcut(ED_SHORTCUT("skeleton_3d_editor/selected_poses_to_rests", TTRC("Apply Selected Poses to Rests")), SKELETON_OPTION_SELECTED_POSES_TO_RESTS);
	p->add_item(TTR("Create Physical Skeleton"), SKELETON_OPTION_CREATE_PHYSICAL_SKELETON);
	p->add_item(TTR("Export Skeleton Profile"), SKELETON_OPTION_EXPORT_SKELETON_PROFILE);

	p->connect(SceneStringName(id_pressed), callable_mp(this, &Skeleton3DEditor::_on_click_skeleton_option));
	set_bone_options_enabled(false);

	Vector<Variant> button_binds;
	button_binds.resize(1);

	edit_mode_button = memnew(Button);
	topmenu_bar->add_child(edit_mode_button);
	edit_mode_button->set_theme_type_variation(SceneStringName(FlatButton));
	edit_mode_button->set_toggle_mode(true);
	edit_mode_button->set_focus_mode(FOCUS_ACCESSIBILITY);
	edit_mode_button->set_tooltip_text(TTR("Edit Mode\nShow buttons on joints."));
	edit_mode_button->connect(SceneStringName(toggled), callable_mp(this, &Skeleton3DEditor::edit_mode_toggled));

	edit_mode = false;

	if (skeleton) {
		skeleton->add_child(handles_mesh_instance);
		handles_mesh_instance->set_skeleton_path(NodePath(""));
	}

	// Keying buttons.
	animation_hb = memnew(HBoxContainer);
	topmenu_bar->add_child(animation_hb);
	animation_hb->add_child(memnew(VSeparator));
	animation_hb->hide();

	key_loc_button = memnew(Button);
	key_loc_button->set_theme_type_variation(SceneStringName(FlatButton));
	key_loc_button->set_toggle_mode(true);
	key_loc_button->set_pressed(editor_plugin->loc_pressed);
	key_loc_button->set_focus_mode(FOCUS_ACCESSIBILITY);
	key_loc_button->set_tooltip_text(TTR("Translation mask for inserting keys."));
	animation_hb->add_child(key_loc_button);
	if (!key_loc_button->is_connected(SceneStringName(toggled), callable_mp(this, &Skeleton3DEditor::_loc_toggled))) {
		key_loc_button->connect(SceneStringName(toggled), callable_mp(this, &Skeleton3DEditor::_loc_toggled));
	}

	key_rot_button = memnew(Button);
	key_rot_button->set_theme_type_variation(SceneStringName(FlatButton));
	key_rot_button->set_toggle_mode(true);
	key_rot_button->set_pressed(editor_plugin->rot_pressed);
	key_rot_button->set_focus_mode(FOCUS_ACCESSIBILITY);
	key_rot_button->set_tooltip_text(TTR("Rotation mask for inserting keys."));
	animation_hb->add_child(key_rot_button);
	if (!key_rot_button->is_connected(SceneStringName(toggled), callable_mp(this, &Skeleton3DEditor::_rot_toggled))) {
		key_rot_button->connect(SceneStringName(toggled), callable_mp(this, &Skeleton3DEditor::_rot_toggled));
	}

	key_scale_button = memnew(Button);
	key_scale_button->set_theme_type_variation(SceneStringName(FlatButton));
	key_scale_button->set_toggle_mode(true);
	key_scale_button->set_pressed(editor_plugin->scl_pressed);
	key_scale_button->set_focus_mode(FOCUS_ACCESSIBILITY);
	key_scale_button->set_tooltip_text(TTR("Scale mask for inserting keys."));
	animation_hb->add_child(key_scale_button);
	if (!key_scale_button->is_connected(SceneStringName(toggled), callable_mp(this, &Skeleton3DEditor::_scl_toggled))) {
		key_scale_button->connect(SceneStringName(toggled), callable_mp(this, &Skeleton3DEditor::_scl_toggled));
	}

	key_insert_button = memnew(Button);
	key_insert_button->set_theme_type_variation(SceneStringName(FlatButton));
	key_insert_button->set_focus_mode(FOCUS_ACCESSIBILITY);
	key_insert_button->connect(SceneStringName(pressed), callable_mp(this, &Skeleton3DEditor::insert_keys).bind(false, false));
	key_insert_button->set_tooltip_text(TTRC("Insert key (based on mask) for bones with an existing track.\nWhen bones are selected, only the selected bones are keyed."));
	key_insert_button->set_shortcut(ED_SHORTCUT("skeleton_3d_editor/insert_key_to_existing_tracks", TTRC("Insert Key (Existing Tracks)"), Key::INSERT));
	animation_hb->add_child(key_insert_button);

	key_insert_new_button = memnew(Button);
	key_insert_new_button->set_theme_type_variation(SceneStringName(FlatButton));
	key_insert_new_button->set_focus_mode(FOCUS_ACCESSIBILITY);
	key_insert_new_button->connect(SceneStringName(pressed), callable_mp(this, &Skeleton3DEditor::insert_keys).bind(true, false));
	key_insert_new_button->set_tooltip_text(TTRC("Insert key (based on mask) for all bones.\nWhen bones are selected, only the selected bones are keyed."));
	key_insert_new_button->set_shortcut(ED_SHORTCUT("skeleton_3d_editor/insert_key_of_all_bones", TTRC("Insert Key (All Bones)"), KeyModifierMask::CMD_OR_CTRL + Key::INSERT));
	animation_hb->add_child(key_insert_new_button);

	key_mod_insert_button = memnew(Button);
	key_mod_insert_button->set_theme_type_variation(SceneStringName(FlatButton));
	key_mod_insert_button->set_focus_mode(FOCUS_ACCESSIBILITY);
	key_mod_insert_button->connect(SceneStringName(pressed), callable_mp(this, &Skeleton3DEditor::insert_keys).bind(false, true));
	key_mod_insert_button->set_tooltip_text(TTRC("Insert key (based on mask) for modified bones with an existing track."));
	key_mod_insert_button->set_shortcut(ED_SHORTCUT("skeleton_3d_editor/insert_key_to_existing_tracks", TTRC("Insert Key (Existing Tracks)"), Key::INSERT));
	animation_hb->add_child(key_mod_insert_button);

	key_mod_insert_new_button = memnew(Button);
	key_mod_insert_new_button->set_theme_type_variation(SceneStringName(FlatButton));
	key_mod_insert_new_button->set_focus_mode(FOCUS_ACCESSIBILITY);
	key_mod_insert_new_button->connect(SceneStringName(pressed), callable_mp(this, &Skeleton3DEditor::insert_keys).bind(true, true));
	key_mod_insert_new_button->set_tooltip_text(TTRC("Insert new key (based on mask) for all modified bones."));
	key_mod_insert_new_button->set_shortcut(ED_SHORTCUT("skeleton_3d_editor/insert_key_of_all_bones", TTRC("Insert Key (All Bones)"), KeyModifierMask::CMD_OR_CTRL + Key::INSERT));
	animation_hb->add_child(key_mod_insert_new_button);

	// Bone tree.
	bones_section = memnew(EditorInspectorSection);
	bones_section->setup("bones", "Bones", skeleton, Color(0.0f, 0.0, 0.0f), true);
	add_child(bones_section);
	bones_section->unfold();

	ScrollContainer *s_con = memnew(ScrollContainer);
	s_con->set_h_size_flags(SIZE_EXPAND_FILL);
	s_con->set_custom_minimum_size(Size2(1, 350) * EDSCALE);
	bones_section->get_vbox()->add_child(s_con);

	joint_tree = memnew(Tree);
	joint_tree->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	joint_tree->set_columns(1);
	joint_tree->set_select_mode(Tree::SELECT_MULTI); // Asosdot: Ctrl/Shift+click selects several bones.
	joint_tree->set_hide_root(true);
	joint_tree->set_v_size_flags(SIZE_EXPAND_FILL);
	joint_tree->set_h_size_flags(SIZE_EXPAND_FILL);
	joint_tree->set_allow_rmb_select(true);
	joint_tree->set_theme_type_variation("TreeSecondary");
	SET_DRAG_FORWARDING_GCD(joint_tree, Skeleton3DEditor);
	s_con->add_child(joint_tree);

	pose_editor = memnew(BonePropertiesEditor(skeleton));
	pose_editor->set_label(TTR("Bone Transform"));
	pose_editor->set_visible(false);
	add_child(pose_editor);

	set_keyable(te->has_keying());
}

void Skeleton3DEditor::_loc_toggled(bool p_toggled_on) {
	if (!editor_plugin) {
		return;
	}
	editor_plugin->loc_pressed = p_toggled_on;
}
void Skeleton3DEditor::_rot_toggled(bool p_toggled_on) {
	if (!editor_plugin) {
		return;
	}
	editor_plugin->rot_pressed = p_toggled_on;
}
void Skeleton3DEditor::_scl_toggled(bool p_toggled_on) {
	if (!editor_plugin) {
		return;
	}
	editor_plugin->scl_pressed = p_toggled_on;
}

void Skeleton3DEditor::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			update_joint_tree();

			joint_tree->connect("multi_selected", callable_mp(this, &Skeleton3DEditor::_joint_tree_multi_selected));
			if (!get_selected_bones(skeleton).is_empty()) {
				// The inspector rebuilt this editor; bring back the skeleton's bone selection.
				callable_mp(this, &Skeleton3DEditor::_apply_bone_selection).call_deferred();
			}
			joint_tree->connect("item_mouse_selected", callable_mp(this, &Skeleton3DEditor::_joint_tree_rmb_select));
			joint_tree->connect("button_clicked", callable_mp(this, &Skeleton3DEditor::_joint_tree_button_clicked));

			skeleton->connect(SceneStringName(pose_updated), callable_mp(this, &Skeleton3DEditor::_draw_gizmo));
			skeleton->connect(SceneStringName(pose_updated), callable_mp(this, &Skeleton3DEditor::_update_properties));
			skeleton->connect(SceneStringName(bone_enabled_changed), callable_mp(this, &Skeleton3DEditor::_bone_enabled_changed));
			skeleton->connect(SceneStringName(show_rest_only_changed), callable_mp(this, &Skeleton3DEditor::_update_gizmo_visible));

			get_tree()->connect("node_removed", callable_mp(this, &Skeleton3DEditor::_node_removed));
		} break;
		case NOTIFICATION_READY: {
			// Will trigger NOTIFICATION_THEME_CHANGED, but won't cause any loops if called here.
			add_theme_constant_override("separation", 0);
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			skeleton_options->set_button_icon(get_editor_theme_icon(SNAME("Skeleton3D")));
			edit_mode_button->set_button_icon(get_editor_theme_icon(SNAME("ToolBoneSelect")));
			key_loc_button->set_button_icon(get_editor_theme_icon(SNAME("KeyPosition")));
			key_rot_button->set_button_icon(get_editor_theme_icon(SNAME("KeyRotation")));
			key_scale_button->set_button_icon(get_editor_theme_icon(SNAME("KeyScale")));
			key_insert_button->set_button_icon(get_editor_theme_icon(SNAME("InsertKey")));
			key_insert_new_button->set_button_icon(get_editor_theme_icon(SNAME("NewKey")));
			key_mod_insert_button->set_button_icon(get_editor_theme_icon(SNAME("InsertModKey")));
			key_mod_insert_new_button->set_button_icon(get_editor_theme_icon(SNAME("NewModKey")));
			bones_section->set_bg_color(get_theme_color(SNAME("prop_subsection"), EditorStringName(Editor)));

			update_joint_tree();
		} break;
		case NOTIFICATION_PREDELETE: {
			_disconnect_from_tree();
			if (skeleton) {
				// The bone selection is kept for the next editor of this skeleton; only this editor lets go of it.
				selected_bone = -1;
				_disconnect_from_skeleton();
				skeleton->set_transform_gizmo_visible(true);

				if (handles_mesh_instance->get_parent()) {
					handles_mesh_instance->get_parent()->remove_child(handles_mesh_instance);
				}
			}
			edit_mode_toggled(false);
		} break;
	}
}

void Skeleton3DEditor::_node_removed(Node *p_node) {
	if (!skeleton || p_node != skeleton) {
		return;
	}

	_disconnect_from_tree();
	_disconnect_from_skeleton();
	if (pose_editor) {
		pose_editor->set_skeleton(nullptr);
		pose_editor->set_visible(false);
	}
	edit_mode = false;
	skeleton = nullptr;
	skeleton_options->hide();

	_update_properties();
}

void Skeleton3DEditor::_disconnect_from_skeleton() {
	if (!skeleton) {
		return;
	}

	if (skeleton->is_connected(SceneStringName(show_rest_only_changed), callable_mp(this, &Skeleton3DEditor::_update_gizmo_visible))) {
		skeleton->disconnect(SceneStringName(show_rest_only_changed), callable_mp(this, &Skeleton3DEditor::_update_gizmo_visible));
	}
	if (skeleton->is_connected(SceneStringName(bone_enabled_changed), callable_mp(this, &Skeleton3DEditor::_bone_enabled_changed))) {
		skeleton->disconnect(SceneStringName(bone_enabled_changed), callable_mp(this, &Skeleton3DEditor::_bone_enabled_changed));
	}
	if (skeleton->is_connected(SceneStringName(pose_updated), callable_mp(this, &Skeleton3DEditor::_draw_gizmo))) {
		skeleton->disconnect(SceneStringName(pose_updated), callable_mp(this, &Skeleton3DEditor::_draw_gizmo));
	}
	if (skeleton->is_connected(SceneStringName(pose_updated), callable_mp(this, &Skeleton3DEditor::_update_properties))) {
		skeleton->disconnect(SceneStringName(pose_updated), callable_mp(this, &Skeleton3DEditor::_update_properties));
	}
}

void Skeleton3DEditor::_disconnect_from_tree() {
	if (!is_inside_tree()) {
		return;
	}

	SceneTree *tree = get_tree();
	if (!tree) {
		return;
	}

	if (tree->is_connected("node_removed", callable_mp(this, &Skeleton3DEditor::_node_removed))) {
		tree->disconnect("node_removed", callable_mp(this, &Skeleton3DEditor::_node_removed));
	}
}

void Skeleton3DEditor::edit_mode_toggled(const bool pressed) {
	edit_mode = pressed;
	_update_gizmo_visible();
}

Skeleton3DEditor::Skeleton3DEditor(EditorInspectorPluginSkeleton *e_plugin, Skeleton3D *p_skeleton) :
		editor_plugin(e_plugin),
		skeleton(p_skeleton) {
	singleton = this;

	// Handle.
	handle_material.instantiate();
	handle_shader.instantiate();
	handle_shader->set_code(R"(
// Skeleton 3D gizmo handle shader.

shader_type spatial;
render_mode unshaded, shadows_disabled, depth_draw_always, fog_disabled;

uniform sampler2D texture_albedo : source_color;
uniform float point_size : hint_range(0, 128) = 32;

void vertex() {
	if (!OUTPUT_IS_SRGB) {
		COLOR.rgb = mix(pow((COLOR.rgb + vec3(0.055)) * (1.0 / (1.0 + 0.055)), vec3(2.4)), COLOR.rgb * (1.0 / 12.92), lessThan(COLOR.rgb, vec3(0.04045)));
	}

	VERTEX = VERTEX;
	POSITION = PROJECTION_MATRIX * VIEW_MATRIX * MODEL_MATRIX * vec4(VERTEX.xyz, 1.0);
	POSITION.z = mix(POSITION.z, POSITION.w, 0.999);
	POINT_SIZE = point_size;
}

void fragment() {
	vec4 albedo_tex = texture(texture_albedo, POINT_COORD);
	vec3 col = albedo_tex.rgb + COLOR.rgb;
	col = vec3(min(col.r, 1.0), min(col.g, 1.0), min(col.b, 1.0));
	ALBEDO = col;

	if (albedo_tex.a < 0.5) {
		discard;
	}

	ALPHA = albedo_tex.a;
}
)");
	handle_material->set_shader(handle_shader);
	Ref<Texture2D> handle = EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("EditorBoneHandle"), EditorStringName(EditorIcons));
	handle_material->set_shader_parameter("point_size", handle->get_width());
	handle_material->set_shader_parameter("texture_albedo", handle);

	handles_mesh_instance = memnew(MeshInstance3D);
	handles_mesh_instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	handles_mesh.instantiate();
	handles_mesh_instance->set_mesh(handles_mesh);

	create_editors();
}

void Skeleton3DEditor::update_bone_original() {
	if (!skeleton) {
		return;
	}
	bone_originals.clear();
	for (int bone : get_selected_bones(skeleton)) {
		BonePose pose;
		pose.position = skeleton->get_bone_pose_position(bone);
		pose.rotation = skeleton->get_bone_pose_rotation(bone);
		pose.scale = skeleton->get_bone_pose_scale(bone);
		bone_originals.insert(bone, pose);
	}
	if (skeleton->get_bone_count() == 0 || selected_bone == -1) {
		return;
	}
	bone_original_position = skeleton->get_bone_pose_position(selected_bone);
	bone_original_rotation = skeleton->get_bone_pose_rotation(selected_bone);
	bone_original_scale = skeleton->get_bone_pose_scale(selected_bone);
}

void Skeleton3DEditor::_hide_handles() {
	handles_mesh_instance->hide();
}

void Skeleton3DEditor::_draw_gizmo() {
	if (!skeleton) {
		return;
	}

	// If you call get_bone_global_pose() while drawing the surface, such as toggle rest mode,
	// the skeleton update will be done first and
	// the drawing surface will be interrupted once and an error will occur.
	skeleton->force_update_all_dirty_bones();

	// Handles.
	if (edit_mode) {
		_draw_handles();
	} else {
		_hide_handles();
	}
}

void Skeleton3DEditor::_draw_handles() {
	const int bone_count = skeleton->get_bone_count();

	handles_mesh->clear_surfaces();

	if (bone_count) {
		handles_mesh_instance->show();

		handles_mesh->surface_begin(Mesh::PRIMITIVE_POINTS);

		for (int i = 0; i < bone_count; i++) {
			Color c;
			if (i == selected_bone) {
				c = Color(1, 1, 0);
			} else if (is_bone_selected(skeleton, i)) {
				c = Color(1, 0.55, 0.1); // Selected, but not the active bone.
			} else {
				c = Color(0.1, 0.25, 0.8);
			}
			Vector3 point = skeleton->get_bone_global_pose(i).origin;
			handles_mesh->surface_set_color(c);
			handles_mesh->surface_add_vertex(point);
		}
		handles_mesh->surface_end();
		handles_mesh->surface_set_material(0, handle_material);
	} else {
		handles_mesh_instance->hide();
	}
}

TreeItem *Skeleton3DEditor::_find(TreeItem *p_node, const NodePath &p_path) {
	if (!p_node) {
		return nullptr;
	}

	NodePath np = p_node->get_metadata(0);
	if (np == p_path) {
		return p_node;
	}

	TreeItem *children = p_node->get_first_child();
	while (children) {
		TreeItem *n = _find(children, p_path);
		if (n) {
			return n;
		}
		children = children->get_next();
	}

	return nullptr;
}

void Skeleton3DEditor::_subgizmo_selection_change() {
	if (!skeleton) {
		return;
	}

	// Once validated by subgizmos_intersect_ray, but required if through inspector's bones tree.
	if (!edit_mode) {
		skeleton->clear_subgizmo_selection();
		return;
	}

	// Deferred, after the viewport has finished handling the click that changed the selection,
	// as it would otherwise put back the one bone it picked.
	callable_mp(this, &Skeleton3DEditor::_sync_viewport_selection).call_deferred();
}

void Skeleton3DEditor::select_bone(int p_idx) {
	if (!skeleton) {
		return;
	}
	Vector<int> bones;
	if (p_idx >= 0) {
		bones.push_back(p_idx);
	}
	set_selected_bones(skeleton, bones, p_idx);
}

Skeleton3DEditor::~Skeleton3DEditor() {
	singleton = nullptr;

	handles_mesh_instance->queue_free();

	Node3DEditor *ne = Node3DEditor::get_singleton();

	ne->remove_control_from_menu_panel(topmenu_bar);
	memdelete(topmenu_bar);
}

bool EditorInspectorPluginSkeleton::can_handle(Object *p_object) {
	return Object::cast_to<Skeleton3D>(p_object) != nullptr;
}

void EditorInspectorPluginSkeleton::parse_begin(Object *p_object) {
	Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(p_object);
	ERR_FAIL_NULL(skeleton);

	skel_editor = memnew(Skeleton3DEditor(this, skeleton));
	add_custom_control(skel_editor);
}

Skeleton3DEditorPlugin::Skeleton3DEditorPlugin() {
	skeleton_plugin = memnew(EditorInspectorPluginSkeleton);

	EditorInspector::add_inspector_plugin(skeleton_plugin);

	Ref<Skeleton3DGizmoPlugin> gizmo_plugin = Ref<Skeleton3DGizmoPlugin>(memnew(Skeleton3DGizmoPlugin));
	Node3DEditor::get_singleton()->add_gizmo_plugin(gizmo_plugin);
}

EditorPlugin::AfterGUIInput Skeleton3DEditorPlugin::forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	Skeleton3DEditor *se = Skeleton3DEditor::get_singleton();
	if (se && se->is_edit_mode()) {
		const Ref<InputEventMouseButton> mb = p_event;
		if (mb.is_valid() && mb->get_button_index() == MouseButton::LEFT && mb->is_pressed()) {
			se->click_appends = mb->is_shift_pressed(); // Read by the bone picking and box select of this click.
			se->update_bone_original();
		}
		return EditorPlugin::AFTER_GUI_INPUT_CUSTOM;
	}
	return EditorPlugin::AFTER_GUI_INPUT_PASS;
}

bool Skeleton3DEditorPlugin::handles(Object *p_object) const {
	return p_object->is_class("Skeleton3D");
}

void Skeleton3DEditor::_bone_enabled_changed(const int p_bone_id) {
	_update_gizmo_visible();
}

void Skeleton3DEditor::_update_gizmo_visible() {
	if (!skeleton) {
		return;
	}

	_subgizmo_selection_change();
	if (edit_mode) {
		if (selected_bone == -1) {
			skeleton->set_transform_gizmo_visible(false);
		} else {
			if (skeleton->is_bone_enabled(selected_bone) && !skeleton->is_show_rest_only()) {
				skeleton->set_transform_gizmo_visible(true);
			} else {
				skeleton->set_transform_gizmo_visible(false);
			}
		}
	} else {
		skeleton->set_transform_gizmo_visible(true);
	}
	_draw_gizmo();
}

int Skeleton3DEditor::get_selected_bone() const {
	return selected_bone;
}

Skeleton3DGizmoPlugin::SelectionMaterials Skeleton3DGizmoPlugin::selection_materials;

Skeleton3DGizmoPlugin::Skeleton3DGizmoPlugin() {
	selection_materials.unselected_mat.instantiate();
	selection_materials.unselected_mat->set_shading_mode(StandardMaterial3D::SHADING_MODE_UNSHADED);
	selection_materials.unselected_mat->set_transparency(StandardMaterial3D::TRANSPARENCY_ALPHA);
	selection_materials.unselected_mat->set_flag(StandardMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	selection_materials.unselected_mat->set_flag(StandardMaterial3D::FLAG_SRGB_VERTEX_COLOR, true);
	selection_materials.unselected_mat->set_flag(StandardMaterial3D::FLAG_DISABLE_FOG, true);

	selection_materials.selected_mat.instantiate();
	Ref<Shader> selected_sh = Ref<Shader>(memnew(Shader));
	selected_sh->set_code(R"(
// Skeleton 3D gizmo bones shader.

shader_type spatial;
render_mode unshaded, shadows_disabled;

void vertex() {
	if (!OUTPUT_IS_SRGB) {
		COLOR.rgb = mix(pow((COLOR.rgb + vec3(0.055)) * (1.0 / (1.0 + 0.055)), vec3(2.4)), COLOR.rgb * (1.0 / 12.92), lessThan(COLOR.rgb,vec3(0.04045)));
	}
	VERTEX = VERTEX;
	POSITION = PROJECTION_MATRIX * VIEW_MATRIX * MODEL_MATRIX * vec4(VERTEX.xyz, 1.0);
	POSITION.z = mix(POSITION.z, POSITION.w, 0.998);
}

void fragment() {
	ALBEDO = COLOR.rgb;
	ALPHA = COLOR.a;
}
)");
	selection_materials.selected_mat->set_shader(selected_sh);
}

Skeleton3DGizmoPlugin::~Skeleton3DGizmoPlugin() {
	selection_materials.unselected_mat.unref();
	selection_materials.selected_mat.unref();
}

bool Skeleton3DGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<Skeleton3D>(p_spatial) != nullptr;
}

String Skeleton3DGizmoPlugin::get_gizmo_name() const {
	return "Skeleton3D";
}

int Skeleton3DGizmoPlugin::get_priority() const {
	return -1;
}

int Skeleton3DGizmoPlugin::subgizmos_intersect_ray(const EditorNode3DGizmo *p_gizmo, Camera3D *p_camera, const Vector2 &p_point) const {
	Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL_V(skeleton, -1);

	Skeleton3DEditor *se = Skeleton3DEditor::get_singleton();

	if (!se || !se->is_edit_mode()) {
		return -1;
	}

	int closest_idx = skeleton_intersect_ray(skeleton, p_camera, p_point);
	const bool append = se->get_click_appends();

	if (closest_idx >= 0) {
		// Shift+click adds the bone, or takes it out if it was selected.
		Vector<int> bones;
		int active = closest_idx;
		if (append) {
			bones = Skeleton3DEditor::get_selected_bones(skeleton);
			if (bones.has(closest_idx)) {
				bones.erase(closest_idx);
				active = Skeleton3DEditor::get_active_bone(skeleton);
			} else {
				bones.push_back(closest_idx);
			}
		} else {
			bones.push_back(closest_idx);
		}
		Skeleton3DEditor::set_selected_bones(skeleton, bones, active);
		se->update_bone_original();
		return closest_idx;
	}

	// A miss keeps a Shift selection; the box select that may follow adds to it.
	if (!append) {
		Skeleton3DEditor::set_selected_bones(skeleton, Vector<int>(), -1);
	}
	return -1;
}

Vector<int> Skeleton3DGizmoPlugin::subgizmos_intersect_frustum(const EditorNode3DGizmo *p_gizmo, const Camera3D *p_camera, const Vector<Plane> &p_frustum) const {
	Vector<int> hits;
	Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL_V(skeleton, hits);
	Skeleton3DEditor *se = Skeleton3DEditor::get_singleton();
	if (!se || !se->is_edit_mode() || se->get_skeleton() != skeleton) {
		return hits;
	}

	const Transform3D gt = skeleton->get_global_transform();
	for (int i = 0; i < skeleton->get_bone_count(); i++) {
		const Vector3 joint = gt.xform(skeleton->get_bone_global_pose(i).origin);
		bool inside = true;
		for (const Plane &plane : p_frustum) {
			if (plane.is_point_over(joint)) {
				inside = false;
				break;
			}
		}
		if (inside) {
			hits.push_back(i);
		}
	}

	Vector<int> bones = se->get_click_appends() ? Skeleton3DEditor::get_selected_bones(skeleton) : Vector<int>();
	for (int bone : hits) {
		if (!bones.has(bone)) {
			bones.push_back(bone);
		}
	}
	int active = Skeleton3DEditor::get_active_bone(skeleton);
	if (!bones.has(active)) {
		active = bones.is_empty() ? -1 : bones[0];
	}
	Skeleton3DEditor::set_selected_bones(skeleton, bones, active);
	se->update_bone_original();
	return hits;
}

bool Skeleton3DGizmoPlugin::wants_subgizmo_region_select(const EditorNode3DGizmo *p_gizmo) const {
	Skeleton3DEditor *se = Skeleton3DEditor::get_singleton();
	return se && se->is_edit_mode() && se->get_skeleton() == p_gizmo->get_node_3d();
}

int Skeleton3DGizmoPlugin::skeleton_intersect_ray(const Skeleton3D *p_skeleton, Camera3D *p_camera, const Vector2 &p_point) {
	real_t grab_threshold = 8 * EDSCALE;
	Vector3 ray_from = p_camera->get_global_transform().origin;
	Transform3D gt = p_skeleton->get_global_transform();
	int closest_idx = -1;
	real_t closest_dist = 1e10;
	const int bone_count = p_skeleton->get_bone_count();
	for (int i = 0; i < bone_count; i++) {
		Vector3 joint_pos_3d = gt.xform(p_skeleton->get_bone_global_pose(i).origin);
		Vector2 joint_pos_2d = p_camera->unproject_position(joint_pos_3d);
		real_t dist_3d = ray_from.distance_to(joint_pos_3d);
		real_t dist_2d = p_point.distance_to(joint_pos_2d);
		if (dist_2d < grab_threshold && dist_3d < closest_dist) {
			closest_dist = dist_3d;
			closest_idx = i;
		}
	}
	return closest_idx;
}

Transform3D Skeleton3DGizmoPlugin::get_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id) const {
	Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL_V(skeleton, Transform3D());

	return skeleton->get_bone_global_pose(p_id);
}

void Skeleton3DGizmoPlugin::set_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id, Transform3D p_transform) {
	Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL(skeleton);

	// Asosdot: with several bones selected, a bone below another selected bone follows it and is
	// not moved twice. Rotating or scaling several separate bones turns each one where it is,
	// rather than swinging them round the middle of the selection.
	bool keep_position = false;
	const Vector<int> selected = Skeleton3DEditor::get_selected_bones(skeleton);
	if (selected.size() > 1) {
		auto has_selected_ancestor = [&](int p_bone) {
			for (int parent = skeleton->get_bone_parent(p_bone); parent >= 0; parent = skeleton->get_bone_parent(parent)) {
				if (selected.has(parent)) {
					return true;
				}
			}
			return false;
		};
		if (has_selected_ancestor(p_id)) {
			return;
		}
		int top_bones = 0;
		for (int bone : selected) {
			top_bones += has_selected_ancestor(bone) ? 0 : 1;
		}
		keep_position = top_bones > 1 && !p_transform.basis.is_equal_approx(skeleton->get_bone_global_pose(p_id).basis);
	}

	// Prepare for global to local.
	Transform3D original_to_local;
	int parent_idx = skeleton->get_bone_parent(p_id);
	if (parent_idx >= 0) {
		original_to_local = skeleton->get_bone_global_pose(parent_idx);
	}
	Basis to_local = original_to_local.get_basis().inverse();

	// Prepare transform.
	Transform3D t;

	// Basis.
	t.basis = to_local * p_transform.get_basis();

	// Origin.
	Vector3 orig = skeleton->get_bone_pose(p_id).origin;
	Vector3 sub = p_transform.origin - skeleton->get_bone_global_pose(p_id).origin;
	t.origin = keep_position ? orig : orig + to_local.xform(sub);

	// Apply transform.
	skeleton->set_bone_pose_position(p_id, t.origin);
	skeleton->set_bone_pose_rotation(p_id, t.basis.get_rotation_quaternion());
	skeleton->set_bone_pose_scale(p_id, t.basis.get_scale());
}

void Skeleton3DGizmoPlugin::commit_subgizmos(const EditorNode3DGizmo *p_gizmo, const Vector<int> &p_ids, const Vector<Transform3D> &p_restore, bool p_cancel) {
	Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL(skeleton);

	Skeleton3DEditor *se = Skeleton3DEditor::get_singleton();
	Node3DEditor *ne = Node3DEditor::get_singleton();

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Set Bone Transform"));

	// Asosdot: each selected bone goes back to its own pose from when the transform began.
	struct Original {
		Vector3 position;
		Quaternion rotation;
		Vector3 scale;
	};
	HashMap<int, Original> originals;
	for (int id : p_ids) {
		Original original;
		se->get_bone_original(id, original.position, original.rotation, original.scale);
		originals.insert(id, original);
	}

	if (p_cancel) {
		for (int id : p_ids) {
			ur->add_do_method(skeleton, "set_bone_pose_position", id, originals[id].position);
			ur->add_do_method(skeleton, "set_bone_pose_rotation", id, originals[id].rotation);
			ur->add_do_method(skeleton, "set_bone_pose_scale", id, originals[id].scale);
			ur->add_undo_method(skeleton, "set_bone_pose_position", id, skeleton->get_bone_pose_position(id));
			ur->add_undo_method(skeleton, "set_bone_pose_rotation", id, skeleton->get_bone_pose_rotation(id));
			ur->add_undo_method(skeleton, "set_bone_pose_scale", id, skeleton->get_bone_pose_scale(id));
		}
	} else {
		Node3DEditor::ToolMode tool_mode = ne->get_tool_mode();
		if (tool_mode == Node3DEditor::TOOL_MODE_MOVE || tool_mode == Node3DEditor::TOOL_MODE_TRANSFORM) {
			for (int id : p_ids) {
				ur->add_do_method(skeleton, "set_bone_pose_position", id, skeleton->get_bone_pose_position(id));
				ur->add_undo_method(skeleton, "set_bone_pose_position", id, originals[id].position);
			}
		}
		if (tool_mode == Node3DEditor::TOOL_MODE_ROTATE || tool_mode == Node3DEditor::TOOL_MODE_TRANSFORM) {
			for (int id : p_ids) {
				ur->add_do_method(skeleton, "set_bone_pose_rotation", id, skeleton->get_bone_pose_rotation(id));
				ur->add_undo_method(skeleton, "set_bone_pose_rotation", id, originals[id].rotation);
			}
		}
		if (tool_mode == Node3DEditor::TOOL_MODE_SCALE || tool_mode == Node3DEditor::TOOL_MODE_TRANSFORM) {
			for (int id : p_ids) {
				ur->add_do_method(skeleton, "set_bone_pose_scale", id, skeleton->get_bone_pose_scale(id));
				ur->add_undo_method(skeleton, "set_bone_pose_scale", id, originals[id].scale);
			}
		}
	}

	ur->add_do_method(se, "update_joint_tree");
	ur->add_undo_method(se, "update_joint_tree");

	ur->commit_action();
}

void Skeleton3DGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(p_gizmo->get_node_3d());
	p_gizmo->clear();

	if (!skeleton->get_bone_count()) {
		return;
	}

	int selected = -1;
	Skeleton3DEditor *se = Skeleton3DEditor::get_singleton();
	if (se) {
		selected = se->get_selected_bone();
	}

	Ref<ArrayMesh> m = get_bones_mesh(skeleton, selected, p_gizmo->is_selected());
	p_gizmo->add_mesh(m, Ref<Material>(), Transform3D(), skeleton->register_skin(skeleton->create_skin_from_rest_transforms()));
}

Ref<ArrayMesh> Skeleton3DGizmoPlugin::get_bones_mesh(Skeleton3D *p_skeleton, int p_selected, bool p_is_selected) {
	Color bone_color = EDITOR_GET("editors/3d_gizmos/gizmo_colors/skeleton");
	Color selected_bone_color = EDITOR_GET("editors/3d_gizmos/gizmo_colors/selected_bone");
	real_t bone_axis_length = EDITOR_GET("editors/3d_gizmos/gizmo_settings/bone_axis_length");
	int bone_shape = EDITOR_GET("editors/3d_gizmos/gizmo_settings/bone_shape");

	LocalVector<Color> axis_colors;
	axis_colors.push_back(Node3DEditor::get_singleton()->get_theme_color(SNAME("axis_x_color"), EditorStringName(Editor)));
	axis_colors.push_back(Node3DEditor::get_singleton()->get_theme_color(SNAME("axis_y_color"), EditorStringName(Editor)));
	axis_colors.push_back(Node3DEditor::get_singleton()->get_theme_color(SNAME("axis_z_color"), EditorStringName(Editor)));

	Ref<SurfaceTool> surface_tool(memnew(SurfaceTool));
	surface_tool->begin(Mesh::PRIMITIVE_LINES);

	if (p_is_selected) {
		surface_tool->set_material(selection_materials.selected_mat);
	} else {
		selection_materials.unselected_mat->set_albedo(bone_color);
		surface_tool->set_material(selection_materials.unselected_mat);
	}

	LocalVector<int> bones;
	LocalVector<float> weights;
	bones.resize(4);
	weights.resize(4);
	for (int i = 0; i < 4; i++) {
		bones[i] = 0;
		weights[i] = 0;
	}
	weights[0] = 1;

	int current_bone_index = 0;
	Vector<int> bones_to_process = p_skeleton->get_parentless_bones();

	while (bones_to_process.size() > current_bone_index) {
		int current_bone_idx = bones_to_process[current_bone_index];
		current_bone_index++;

		// Asosdot: every selected bone is drawn selected, not only the active one.
		const bool bone_selected = current_bone_idx == p_selected || (p_selected >= 0 && Skeleton3DEditor::is_bone_selected(p_skeleton, current_bone_idx));
		Color current_bone_color = bone_selected ? selected_bone_color : bone_color;

		Vector<int> child_bones_vector;
		child_bones_vector = p_skeleton->get_bone_children(current_bone_idx);
		int child_bones_size = child_bones_vector.size();

		for (int i = 0; i < child_bones_size; i++) {
			// Something wrong.
			if (child_bones_vector[i] < 0) {
				continue;
			}

			int child_bone_idx = child_bones_vector[i];

			Vector3 v0 = p_skeleton->get_bone_global_rest(current_bone_idx).origin;
			Vector3 v1 = p_skeleton->get_bone_global_rest(child_bone_idx).origin;
			Vector3 d = (v1 - v0).normalized();
			real_t dist = v0.distance_to(v1);

			// Find closest axis.
			int closest = -1;
			real_t closest_d = 0.0;
			for (int j = 0; j < 3; j++) {
				real_t dp = Math::abs(p_skeleton->get_bone_global_rest(current_bone_idx).basis[j].normalized().dot(d));
				if (j == 0 || dp > closest_d) {
					closest = j;
				}
			}

			// Draw bone.
			switch (bone_shape) {
				case 0: { // Wire shape.
					surface_tool->set_color(current_bone_color);
					bones[0] = current_bone_idx;
					surface_tool->set_bones(Vector<int>(bones));
					surface_tool->set_weights(Vector<float>(weights));
					surface_tool->add_vertex(v0);
					bones[0] = child_bone_idx;
					surface_tool->set_bones(Vector<int>(bones));
					surface_tool->set_weights(Vector<float>(weights));
					surface_tool->add_vertex(v1);
				} break;

				case 1: { // Octahedron shape.
					Vector3 first;
					Vector3 points[6];
					int point_idx = 0;
					for (int j = 0; j < 3; j++) {
						Vector3 axis;
						if (first == Vector3()) {
							axis = d.cross(d.cross(p_skeleton->get_bone_global_rest(current_bone_idx).basis[j])).normalized();
							first = axis;
						} else {
							axis = d.cross(first).normalized();
						}

						surface_tool->set_color(current_bone_color);
						for (int k = 0; k < 2; k++) {
							if (k == 1) {
								axis = -axis;
							}
							Vector3 point = v0 + d * dist * 0.2;
							point += axis * dist * 0.1;

							bones[0] = current_bone_idx;
							surface_tool->set_bones(Vector<int>(bones));
							surface_tool->set_weights(Vector<float>(weights));
							surface_tool->add_vertex(v0);
							surface_tool->set_bones(Vector<int>(bones));
							surface_tool->set_weights(Vector<float>(weights));
							surface_tool->add_vertex(point);

							surface_tool->set_bones(Vector<int>(bones));
							surface_tool->set_weights(Vector<float>(weights));
							surface_tool->add_vertex(point);
							bones[0] = child_bone_idx;
							surface_tool->set_bones(Vector<int>(bones));
							surface_tool->set_weights(Vector<float>(weights));
							surface_tool->add_vertex(v1);
							points[point_idx++] = point;
						}
					}
					surface_tool->set_color(current_bone_color);
					SWAP(points[1], points[2]);
					bones[0] = current_bone_idx;
					for (int j = 0; j < 6; j++) {
						surface_tool->set_bones(Vector<int>(bones));
						surface_tool->set_weights(Vector<float>(weights));
						surface_tool->add_vertex(points[j]);
						surface_tool->set_bones(Vector<int>(bones));
						surface_tool->set_weights(Vector<float>(weights));
						surface_tool->add_vertex(points[(j + 1) % 6]);
					}
				} break;
			}

			// Axis as root of the bone.
			for (int j = 0; j < 3; j++) {
				bones[0] = current_bone_idx;
				surface_tool->set_color(axis_colors[j]);
				surface_tool->set_bones(Vector<int>(bones));
				surface_tool->set_weights(Vector<float>(weights));
				surface_tool->add_vertex(v0);
				surface_tool->set_bones(Vector<int>(bones));
				surface_tool->set_weights(Vector<float>(weights));
				surface_tool->add_vertex(v0 + (p_skeleton->get_bone_global_rest(current_bone_idx).basis.inverse())[j].normalized() * dist * bone_axis_length);

				if (j == closest) {
					continue;
				}
			}

			// Axis at the end of the bone children.
			if (i == child_bones_size - 1) {
				for (int j = 0; j < 3; j++) {
					bones[0] = child_bone_idx;
					surface_tool->set_color(axis_colors[j]);
					surface_tool->set_bones(Vector<int>(bones));
					surface_tool->set_weights(Vector<float>(weights));
					surface_tool->add_vertex(v1);
					surface_tool->set_bones(Vector<int>(bones));
					surface_tool->set_weights(Vector<float>(weights));
					surface_tool->add_vertex(v1 + (p_skeleton->get_bone_global_rest(child_bone_idx).basis.inverse())[j].normalized() * dist * bone_axis_length);

					if (j == closest) {
						continue;
					}
				}
			}

			// Add the bone's children to the list of bones to be processed.
			bones_to_process.push_back(child_bones_vector[i]);
		}
	}

	return surface_tool->commit();
}
