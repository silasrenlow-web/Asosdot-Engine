/**************************************************************************/
/*  procedural_root_3d.cpp                                                */
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

#include "procedural_root_3d.h"

#include "core/config/engine.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

#ifdef TOOLS_ENABLED
static HashSet<ProceduralRoot3D *> &_procedural_roots() {
	static HashSet<ProceduralRoot3D *> roots;
	return roots;
}
#endif // TOOLS_ENABLED

bool ProceduralRoot3D::is_generated_node(const Node *p_node) {
	if (!p_node || p_node->get_owner()) {
		return false;
	}
	return get_generator_of(p_node) != nullptr;
}

ProceduralRoot3D *ProceduralRoot3D::get_generator_of(const Node *p_node) {
	const Node *node = p_node ? p_node->get_parent() : nullptr;
	while (node) {
		if (ProceduralRoot3D *root = Object::cast_to<ProceduralRoot3D>(const_cast<Node *>(node))) {
			return root;
		}
		node = node->get_parent();
	}
	return nullptr;
}

Node *ProceduralRoot3D::get_generated_part(Node *p_node) {
	Node *node = p_node;
	while (node) {
		if (is_generated_node(node)) {
			return node;
		}
		node = node->get_parent();
	}
	return nullptr;
}

void ProceduralRoot3D::_find_parts(Node *p_node, const String &p_path, Vector<Node *> &r_parts) {
	for (int i = 0; i < p_node->get_child_count(false); i++) {
		Node *child = p_node->get_child(i, false);
		const bool generated = is_generated_node(child);
		if (generated && child->get_scene_file_path() == p_path) {
			r_parts.push_back(child);
		} else if (generated && child->is_instance()) {
			// The inside of another part.
			continue;
		} else {
			_find_parts(child, p_path, r_parts);
		}
	}
}

void ProceduralRoot3D::_collect_part_scenes(Node *p_node, HashSet<String> &r_paths) {
	for (int i = 0; i < p_node->get_child_count(false); i++) {
		Node *child = p_node->get_child(i, false);
		if (is_generated_node(child) && child->is_instance()) {
			r_paths.insert(child->get_scene_file_path());
		} else {
			_collect_part_scenes(child, r_paths);
		}
	}
}

PackedStringArray ProceduralRoot3D::get_part_scenes() const {
	HashSet<String> paths;
	_collect_part_scenes(const_cast<ProceduralRoot3D *>(this), paths);
	PackedStringArray result;
	for (const String &path : paths) {
		result.push_back(path);
	}
	result.sort();
	return result;
}

void ProceduralRoot3D::_replace_part(Node *p_old, const Ref<PackedScene> &p_scene) {
	Node *parent = p_old->get_parent();
	ERR_FAIL_NULL(parent);
	Node *fresh = p_scene->instantiate();
	ERR_FAIL_NULL(fresh);

	const int index = p_old->get_index(false);
	const StringName old_name = p_old->get_name();

	Node3D *old_3d = Object::cast_to<Node3D>(p_old);
	Node3D *fresh_3d = Object::cast_to<Node3D>(fresh);
	if (old_3d && fresh_3d) {
		fresh_3d->set_transform(old_3d->get_transform());
		fresh_3d->set_visible(old_3d->is_visible());
	}
	if (p_old->has_meta(SNAME("_part_properties"))) {
		const Dictionary properties = p_old->get_meta(SNAME("_part_properties"));
		for (const KeyValue<Variant, Variant> &kv : properties) {
			fresh->set(kv.key, kv.value);
		}
		fresh->set_meta(SNAME("_part_properties"), properties);
	}

	parent->remove_child(p_old);
	p_old->queue_free();
	fresh->set_name(old_name);
	parent->add_child(fresh);
	parent->move_child(fresh, index);
}

void ProceduralRoot3D::reload_part_scene(const String &p_path) {
	if (part_reload == PART_RELOAD_NONE) {
		return;
	}
	if (part_reload == PART_RELOAD_REGENERATE) {
		if (!generating) {
			generate();
		}
		return;
	}
	Ref<PackedScene> scene = ResourceLoader::load(p_path, "PackedScene");
	ERR_FAIL_COND_MSG(scene.is_null(), vformat("ProceduralRoot3D: can't load the part scene '%s'.", p_path));

	Vector<Node *> parts;
	_find_parts(this, p_path, parts);
	for (Node *part : parts) {
		_replace_part(part, scene);
	}
	if (!parts.is_empty()) {
		emit_signal(SNAME("part_scene_reloaded"), p_path);
	}
}

Node3D *ProceduralRoot3D::add_part(const Ref<PackedScene> &p_scene, const Transform3D &p_transform, const Dictionary &p_properties, Node *p_parent) {
	ERR_FAIL_COND_V(p_scene.is_null(), nullptr);
	Node *parent = p_parent ? p_parent : this;
	ERR_FAIL_COND_V_MSG(parent != this && !is_ancestor_of(parent), nullptr, "ProceduralRoot3D.add_part(): the parent must be this node or a node under it.");

	Node *instance = p_scene->instantiate();
	ERR_FAIL_NULL_V(instance, nullptr);
	Node3D *part = Object::cast_to<Node3D>(instance);
	if (!part) {
		memdelete(instance);
		ERR_FAIL_V_MSG(nullptr, "ProceduralRoot3D.add_part(): the root of a part must be a Node3D.");
	}
	part->set_transform(p_transform);
	for (const KeyValue<Variant, Variant> &kv : p_properties) {
		part->set(kv.key, kv.value);
	}
	if (!p_properties.is_empty()) {
		part->set_meta(SNAME("_part_properties"), p_properties);
	}
	parent->add_child(part);
	return part;
}

void ProceduralRoot3D::clear_generated() {
	for (int i = get_child_count(false) - 1; i >= 0; i--) {
		Node *child = get_child(i, false);
		if (!child->get_owner()) {
			remove_child(child);
			child->queue_free();
		}
	}
	// Generated nodes may also stand under nodes the scene owns (a container the user placed).
	Vector<Node *> stack;
	for (int i = 0; i < get_child_count(false); i++) {
		stack.push_back(get_child(i, false));
	}
	while (!stack.is_empty()) {
		Node *node = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);
		for (int i = node->get_child_count(false) - 1; i >= 0; i--) {
			Node *child = node->get_child(i, false);
			if (!child->get_owner()) {
				node->remove_child(child);
				child->queue_free();
			} else if (child->get_owner() == get_owner() || !get_owner()) {
				stack.push_back(child);
			}
		}
	}
}

void ProceduralRoot3D::generate() {
	if (generating) {
		WARN_PRINT("ProceduralRoot3D: generate() was called while it is still generating.");
		return;
	}
	clear_generated();
	generating = true;
	GDVIRTUAL_CALL(_generate);
	if (!async_generation) {
		finish_generation();
	}
}

void ProceduralRoot3D::finish_generation() {
	if (!generating) {
		return;
	}
	generating = false;
	emit_signal(SNAME("generated"));
}

bool ProceduralRoot3D::is_generating() const {
	return generating;
}

Callable ProceduralRoot3D::get_generate_button() const {
	return callable_mp(const_cast<ProceduralRoot3D *>(this), &ProceduralRoot3D::generate);
}

Callable ProceduralRoot3D::get_clear_button() const {
	return callable_mp(const_cast<ProceduralRoot3D *>(this), &ProceduralRoot3D::clear_generated);
}

void ProceduralRoot3D::set_generate_in_editor(bool p_enabled) {
	generate_in_editor = p_enabled;
}

bool ProceduralRoot3D::is_generate_in_editor() const {
	return generate_in_editor;
}

void ProceduralRoot3D::set_async_generation(bool p_async) {
	async_generation = p_async;
}

bool ProceduralRoot3D::is_async_generation() const {
	return async_generation;
}

void ProceduralRoot3D::set_part_reload(PartReload p_mode) {
	part_reload = p_mode;
}

ProceduralRoot3D::PartReload ProceduralRoot3D::get_part_reload() const {
	return part_reload;
}

PackedStringArray ProceduralRoot3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	if (get_script().is_null()) {
		warnings.push_back(RTR("Nothing generates here: attach a script that implements _generate() and builds the level with add_part()."));
	}
	return warnings;
}

#ifdef TOOLS_ENABLED
void ProceduralRoot3D::_reload_pending_parts() {
	if (pending_parts.is_empty()) {
		return;
	}
	Vector<String> paths;
	for (const String &path : pending_parts) {
		paths.push_back(path);
	}
	pending_parts.clear();
	for (const String &path : paths) {
		reload_part_scene(path);
	}
}

void ProceduralRoot3D::notify_scene_saved(const String &p_path) {
	Vector<ProceduralRoot3D *> roots;
	for (ProceduralRoot3D *root : _procedural_roots()) {
		roots.push_back(root);
	}
	for (ProceduralRoot3D *root : roots) {
		if (root->part_reload == PART_RELOAD_NONE) {
			continue;
		}
		HashSet<String> paths;
		_collect_part_scenes(root, paths);
		if (!paths.has(p_path)) {
			continue;
		}
		if (root->is_inside_tree()) {
			root->reload_part_scene(p_path);
		} else {
			root->pending_parts.insert(p_path);
		}
	}
}
#endif // TOOLS_ENABLED

void ProceduralRoot3D::_notification(int p_what) {
	switch (p_what) {
#ifdef TOOLS_ENABLED
		case NOTIFICATION_ENTER_TREE: {
			if (Engine::get_singleton()->is_editor_hint() && !pending_parts.is_empty()) {
				callable_mp(this, &ProceduralRoot3D::_reload_pending_parts).call_deferred();
			}
		} break;
#endif // TOOLS_ENABLED
		case NOTIFICATION_READY: {
			if (Engine::get_singleton()->is_editor_hint() && generate_in_editor && is_part_of_edited_scene() && get_script_instance()) {
				callable_mp(this, &ProceduralRoot3D::generate).call_deferred();
			}
		} break;
	}
}

void ProceduralRoot3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_generate_in_editor", "enabled"), &ProceduralRoot3D::set_generate_in_editor);
	ClassDB::bind_method(D_METHOD("is_generate_in_editor"), &ProceduralRoot3D::is_generate_in_editor);
	ClassDB::bind_method(D_METHOD("set_async_generation", "async"), &ProceduralRoot3D::set_async_generation);
	ClassDB::bind_method(D_METHOD("is_async_generation"), &ProceduralRoot3D::is_async_generation);
	ClassDB::bind_method(D_METHOD("set_part_reload", "mode"), &ProceduralRoot3D::set_part_reload);
	ClassDB::bind_method(D_METHOD("get_part_reload"), &ProceduralRoot3D::get_part_reload);

	ClassDB::bind_method(D_METHOD("generate"), &ProceduralRoot3D::generate);
	ClassDB::bind_method(D_METHOD("clear_generated"), &ProceduralRoot3D::clear_generated);
	ClassDB::bind_method(D_METHOD("finish_generation"), &ProceduralRoot3D::finish_generation);
	ClassDB::bind_method(D_METHOD("is_generating"), &ProceduralRoot3D::is_generating);
	ClassDB::bind_method(D_METHOD("get_generate_button"), &ProceduralRoot3D::get_generate_button);
	ClassDB::bind_method(D_METHOD("get_clear_button"), &ProceduralRoot3D::get_clear_button);
	ClassDB::bind_method(D_METHOD("add_part", "scene", "transform", "properties", "parent"), &ProceduralRoot3D::add_part, DEFVAL(Transform3D()), DEFVAL(Dictionary()), DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("get_part_scenes"), &ProceduralRoot3D::get_part_scenes);
	ClassDB::bind_method(D_METHOD("reload_part_scene", "path"), &ProceduralRoot3D::reload_part_scene);

	ClassDB::bind_static_method("ProceduralRoot3D", D_METHOD("is_generated_node", "node"), &ProceduralRoot3D::is_generated_node);
	ClassDB::bind_static_method("ProceduralRoot3D", D_METHOD("get_generated_part", "node"), &ProceduralRoot3D::get_generated_part);
	ClassDB::bind_static_method("ProceduralRoot3D", D_METHOD("get_generator_of", "node"), &ProceduralRoot3D::get_generator_of);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "generate_in_editor"), "set_generate_in_editor", "is_generate_in_editor");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "async_generation"), "set_async_generation", "is_async_generation");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "part_reload", PROPERTY_HINT_ENUM, "In Place,Regenerate,None"), "set_part_reload", "get_part_reload");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "generate_button", PROPERTY_HINT_TOOL_BUTTON, "Generate,Reload", PROPERTY_USAGE_EDITOR), "", "get_generate_button");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "clear_button", PROPERTY_HINT_TOOL_BUTTON, "Clear generated,Remove", PROPERTY_USAGE_EDITOR), "", "get_clear_button");

	ADD_SIGNAL(MethodInfo("generated"));
	ADD_SIGNAL(MethodInfo("part_scene_reloaded", PropertyInfo(Variant::STRING, "path")));

	GDVIRTUAL_BIND(_generate);

	BIND_ENUM_CONSTANT(PART_RELOAD_IN_PLACE);
	BIND_ENUM_CONSTANT(PART_RELOAD_REGENERATE);
	BIND_ENUM_CONSTANT(PART_RELOAD_NONE);
}

ProceduralRoot3D::ProceduralRoot3D() {
#ifdef TOOLS_ENABLED
	_procedural_roots().insert(this);
#endif // TOOLS_ENABLED
}

ProceduralRoot3D::~ProceduralRoot3D() {
#ifdef TOOLS_ENABLED
	_procedural_roots().erase(this);
#endif // TOOLS_ENABLED
}
