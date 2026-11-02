/**************************************************************************/
/*  decal_inspector_plugin.cpp                                            */
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

#include "decal_inspector_plugin.h"

#include "core/object/callable_mp.h"
#include "editor/editor_data.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/decal.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/scene_string_names.h"

void DecalInspectorControls::_fit() {
	ERR_FAIL_NULL(decal);
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Fit Decal to Texture"));
	undo_redo->add_do_method(decal, "fit_to_texture", 0.0);
	undo_redo->add_undo_property(decal, "size", decal->get_size());
	undo_redo->commit_action();
}

void DecalInspectorControls::_affect_selected() {
	ERR_FAIL_NULL(decal);
	TypedArray<NodePath> paths;
	for (Node *node : EditorNode::get_singleton()->get_editor_selection()->get_top_selected_node_list()) {
		if (node != decal && decal->is_inside_tree() && node->is_inside_tree()) {
			paths.push_back(decal->get_path_to(node));
		}
	}
	if (paths.is_empty()) {
		return;
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Limit Decal to Selected Nodes"));
	undo_redo->add_do_property(decal, "affected_nodes", paths);
	undo_redo->add_undo_property(decal, "affected_nodes", decal->get_affected_nodes());
	undo_redo->commit_action();
}

void DecalInspectorControls::_clear() {
	ERR_FAIL_NULL(decal);
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Remove Decal Limit"));
	undo_redo->add_do_property(decal, "affected_nodes", TypedArray<NodePath>());
	undo_redo->add_undo_property(decal, "affected_nodes", decal->get_affected_nodes());
	undo_redo->commit_action();
}

void DecalInspectorControls::_update_buttons() {
	if (!decal) {
		return;
	}
	bool others_selected = false;
	for (Node *node : EditorNode::get_singleton()->get_editor_selection()->get_top_selected_node_list()) {
		if (node != decal) {
			others_selected = true;
			break;
		}
	}
	affect->set_disabled(!others_selected);
	clear->set_visible(decal->get_affected_nodes().size() > 0);
}

void DecalInspectorControls::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_VISIBILITY_CHANGED:
		case NOTIFICATION_ENTER_TREE: {
			_update_buttons();
		} break;
	}
}

void DecalInspectorControls::set_decal(Decal *p_decal) {
	decal = p_decal;
	_update_buttons();
}

DecalInspectorControls::DecalInspectorControls() {
	fit = memnew(Button);
	fit->set_text(TTR("Fit to Texture"));
	fit->set_tooltip_text(TTR("Give the decal the shape of its texture, so one that is not square is not squashed."));
	fit->connect(SceneStringName(pressed), callable_mp(this, &DecalInspectorControls::_fit));
	add_child(fit);

	affect = memnew(Button);
	affect->set_text(TTR("Limit to Selected Nodes"));
	affect->set_tooltip_text(TTR("Paint only the other nodes that are selected with this one, and nothing else. Select the wall or the prop, then add the decal to the selection."));
	affect->connect(SceneStringName(pressed), callable_mp(this, &DecalInspectorControls::_affect_selected));
	add_child(affect);

	clear = memnew(Button);
	clear->set_text(TTR("Remove Limit"));
	clear->set_tooltip_text(TTR("Paint everything the decal's Cull Mask lets through again."));
	clear->connect(SceneStringName(pressed), callable_mp(this, &DecalInspectorControls::_clear));
	add_child(clear);
}

bool EditorInspectorPluginDecal::can_handle(Object *p_object) {
	return Object::cast_to<Decal>(p_object) != nullptr;
}

void EditorInspectorPluginDecal::parse_begin(Object *p_object) {
	DecalInspectorControls *controls = memnew(DecalInspectorControls);
	controls->set_decal(Object::cast_to<Decal>(p_object));
	add_custom_control(controls);
}

DecalInspectorEditorPlugin::DecalInspectorEditorPlugin() {
	Ref<EditorInspectorPluginDecal> plugin;
	plugin.instantiate();
	add_inspector_plugin(plugin);
}
