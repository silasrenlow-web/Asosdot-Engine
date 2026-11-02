/**************************************************************************/
/*  save_state.cpp                                                        */
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

#include "save_state.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "scene/main/save_crypto.h"
#include "scene/main/saves.h"
#include "scene/main/scene_tree.h"

Node *SaveState::_get_scene_root() const {
	SceneTree *tree = get_tree();
	if (!tree) {
		return nullptr;
	}
#ifdef TOOLS_ENABLED
	if (Engine::get_singleton()->is_editor_hint()) {
		return tree->get_edited_scene_root();
	}
#endif
	return tree->get_current_scene();
}

Node *SaveState::get_target_node() const {
	if (target_path.is_empty()) {
		return get_parent();
	}
	return get_node_or_null(target_path);
}

String SaveState::get_state_id() const {
	if (save_id != StringName()) {
		return save_id;
	}

	Node *target = get_target_node();
	if (!target) {
		return String();
	}

	Node *scene_root = _get_scene_root();
	const bool in_scene = scene_root && (scene_root == target || scene_root->is_ancestor_of(target));

	// A node a script added to the scene after it was built has no place in the
	// scene file to name it by, and the engine may rename it to keep it from
	// clashing, so it gets an id of its own. What is outside the scene, an
	// autoload say, is told apart by its path.
	if (in_scene && target != scene_root && target->get_owner() == nullptr) {
		if (generated_id.is_empty()) {
			uint8_t bytes[8];
			SaveCrypto::random_bytes(bytes, 8);
			generated_id = "spawn-" + String::hex_encode_buffer(bytes, 8);
		}
		return generated_id;
	}

	if (in_scene) {
		return String(scene_root->get_path_to(target));
	}
	return String(target->get_path());
}

Variant SaveState::_get_property(Node *p_target, const String &p_property, bool &r_valid) const {
	// "position:x" reaches into a property, the way a track path does.
	const NodePath path = NodePath(":" + p_property);
	return p_target->get_indexed(path.get_subnames(), &r_valid);
}

Dictionary SaveState::capture() {
	Dictionary state;
	Node *target = get_target_node();
	ERR_FAIL_NULL_V(target, state);

	Dictionary values;
	for (const String &property : properties) {
		bool valid = false;
		const Variant value = _get_property(target, property, valid);
		if (!valid) {
			WARN_PRINT(vformat("SaveState \"%s\": %s has no property \"%s\", so it is not saved.", get_path(), target->get_class(), property));
			continue;
		}
		values[property] = Saves::to_storable(value, 0, property);
	}
	state["props"] = values;

	// A script gets to add what no property holds.
	Dictionary extra;
	emit_signal(SNAME("saving"), extra);
	state["extra"] = Saves::to_storable(extra, 0, "extra");

	state["name"] = String(target->get_name());
	state["spawnable"] = spawnable;
	if (spawnable) {
		state["scene"] = target->get_scene_file_path();
		if (Node *parent = target->get_parent()) {
			state["parent"] = parent->get_path();
		}
	}
	return state;
}

void SaveState::apply(const Dictionary &p_state) {
	Node *target = get_target_node();
	ERR_FAIL_NULL(target);

	const Dictionary values = p_state.get("props", Dictionary());
	for (const String &property : properties) {
		if (!values.has(property)) {
			// Added to the list since the save was made.
			continue;
		}

		bool valid = false;
		const Variant current = _get_property(target, property, valid);
		if (!valid) {
			continue;
		}

		Variant value = Saves::from_storable(values[property], 0);
		// A typed array refuses an untyped one, so what was saved is poured into
		// one of the same type.
		if (current.get_type() == Variant::ARRAY && value.get_type() == Variant::ARRAY) {
			const Array current_array = current;
			if (current_array.is_typed()) {
				Array typed;
				typed.set_typed(current_array.get_typed_builtin(), current_array.get_typed_class_name(), current_array.get_typed_script());
				typed.assign(value);
				value = typed;
			}
		}

		const NodePath path = NodePath(":" + property);
		bool set_valid = false;
		target->set_indexed(path.get_subnames(), value, &set_valid);
		if (!set_valid) {
			WARN_PRINT(vformat("SaveState \"%s\": could not put \"%s\" back on %s.", get_path(), property, target->get_class()));
		}
	}

	Dictionary extra = Saves::from_storable(p_state.get("extra", Dictionary()), 0);
	emit_signal(SNAME("loaded"), extra);
}

PackedStringArray SaveState::get_configuration_warnings() const {
	PackedStringArray warnings = Node::get_configuration_warnings();

	Node *target = get_target_node();
	if (!target) {
		warnings.push_back(RTR("The target path leads to no node, so there is nothing to save."));
		return warnings;
	}
	if (properties.is_empty()) {
		warnings.push_back(RTR("No properties are chosen, so only what a script adds in the saving signal is saved."));
	}
	if (spawnable && target->get_scene_file_path().is_empty()) {
		warnings.push_back(RTR("A spawnable node has to be the root of an instanced scene, so that loading can create it again. Save the target as its own scene, or turn Spawnable off."));
	}

	const String id = get_state_id();
	for (const SaveState *other : states_in_tree) {
		if (other != this && other->get_state_id() == id && !id.is_empty()) {
			warnings.push_back(vformat(RTR("Another SaveState is also saved as \"%s\". Give one of them its own Save Id."), id));
			break;
		}
	}
	return warnings;
}

void SaveState::set_target(const NodePath &p_path) {
	target_path = p_path;
	update_configuration_warnings();
}

NodePath SaveState::get_target() const {
	return target_path;
}

void SaveState::set_save_id(const StringName &p_id) {
	save_id = p_id;
	update_configuration_warnings();
}

StringName SaveState::get_save_id() const {
	return save_id;
}

void SaveState::set_properties(const PackedStringArray &p_properties) {
	properties.clear();
	for (const String &property : p_properties) {
		if (!property.is_empty() && !properties.has(property)) {
			properties.push_back(property);
		}
	}
	update_configuration_warnings();
}

PackedStringArray SaveState::get_properties() const {
	PackedStringArray result;
	for (const String &property : properties) {
		result.push_back(property);
	}
	return result;
}

void SaveState::set_spawnable(bool p_spawnable) {
	spawnable = p_spawnable;
	update_configuration_warnings();
}

bool SaveState::is_spawnable() const {
	return spawnable;
}

void SaveState::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			states_in_tree.push_back(this);
#ifdef TOOLS_ENABLED
			if (Engine::get_singleton()->is_editor_hint()) {
				// A project that saves games gets a key of its own to encrypt them with.
				Saves::ensure_project_key();
			}
#endif
		} break;
		case NOTIFICATION_READY: {
			if (!Engine::get_singleton()->is_editor_hint()) {
				// A scene that was not loaded when the save was, comes with its
				// states waiting.
				Saves::get_singleton()->_state_entered(this);
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			states_in_tree.erase(this);
		} break;
	}
}

void SaveState::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_target", "path"), &SaveState::set_target);
	ClassDB::bind_method(D_METHOD("get_target"), &SaveState::get_target);
	ClassDB::bind_method(D_METHOD("set_save_id", "id"), &SaveState::set_save_id);
	ClassDB::bind_method(D_METHOD("get_save_id"), &SaveState::get_save_id);
	ClassDB::bind_method(D_METHOD("set_properties", "properties"), &SaveState::set_properties);
	ClassDB::bind_method(D_METHOD("get_properties"), &SaveState::get_properties);
	ClassDB::bind_method(D_METHOD("set_spawnable", "spawnable"), &SaveState::set_spawnable);
	ClassDB::bind_method(D_METHOD("is_spawnable"), &SaveState::is_spawnable);
	ClassDB::bind_method(D_METHOD("get_target_node"), &SaveState::get_target_node);
	ClassDB::bind_method(D_METHOD("get_state_id"), &SaveState::get_state_id);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node"), "set_target", "get_target");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "properties"), "set_properties", "get_properties");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "save_id"), "set_save_id", "get_save_id");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "spawnable"), "set_spawnable", "is_spawnable");

	ADD_SIGNAL(MethodInfo("saving", PropertyInfo(Variant::DICTIONARY, "extra")));
	ADD_SIGNAL(MethodInfo("loaded", PropertyInfo(Variant::DICTIONARY, "extra")));
}

SaveState::SaveState() {
}

SaveState::~SaveState() {
}
