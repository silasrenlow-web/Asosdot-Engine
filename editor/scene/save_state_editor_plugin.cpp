/**************************************************************************/
/*  save_state_editor_plugin.cpp                                          */
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

#include "save_state_editor_plugin.h"

#include "core/object/callable_mp.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/tree.h"
#include "scene/main/save_state.h"
#include "scene/scene_string_names.h"

TreeItem *EditorPropertySaveProperties::_add_item(const String &p_name, bool p_checked) {
	TreeItem *item = tree->create_item(tree->get_root());
	item->set_cell_mode(0, TreeItem::CELL_MODE_CHECK);
	item->set_editable(0, true);
	item->set_text(0, p_name);
	item->set_checked(0, p_checked);
	return item;
}

void EditorPropertySaveProperties::_fill() {
	tree->clear();
	tree->create_item();

	SaveState *state = Object::cast_to<SaveState>(get_edited_object());
	if (!state) {
		return;
	}
	const PackedStringArray chosen = state->get_properties();

	Node *target = state->get_target_node();
	HashSet<String> listed;
	if (target) {
		List<PropertyInfo> list;
		target->get_property_list(&list);
		for (const PropertyInfo &info : list) {
			if (info.type == Variant::NIL || (info.usage & (PROPERTY_USAGE_GROUP | PROPERTY_USAGE_SUBGROUP | PROPERTY_USAGE_CATEGORY))) {
				continue;
			}
			// What the inspector shows, and what a script made and exported, are what
			// is worth keeping.
			if (!(info.usage & (PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_SCRIPT_VARIABLE))) {
				continue;
			}
			if (info.name.begins_with("metadata/") || info.name == "script") {
				continue;
			}
			listed.insert(info.name);
			_add_item(info.name, chosen.has(info.name));
		}
	}

	// What is kept and the target does not list: a path inside a property, or a
	// property the target has since lost.
	for (const String &name : chosen) {
		if (!listed.has(name)) {
			_add_item(name, true);
		}
	}
	_filter_changed(search->get_text());
}

void EditorPropertySaveProperties::_filter_changed(const String &p_text) {
	TreeItem *root = tree->get_root();
	if (!root) {
		return;
	}
	for (TreeItem *item = root->get_first_child(); item; item = item->get_next()) {
		item->set_visible(p_text.is_empty() || item->get_text(0).containsn(p_text));
	}
}

void EditorPropertySaveProperties::_add_custom(const String &p_text) {
	const String name = p_text.strip_edges();
	if (name.is_empty()) {
		return;
	}
	TreeItem *root = tree->get_root();
	for (TreeItem *item = root->get_first_child(); item; item = item->get_next()) {
		if (item->get_text(0) == name) {
			item->set_checked(0, true);
			custom->clear();
			return;
		}
	}
	_add_item(name, true);
	custom->clear();
}

void EditorPropertySaveProperties::_confirmed() {
	PackedStringArray chosen;
	TreeItem *root = tree->get_root();
	if (root) {
		for (TreeItem *item = root->get_first_child(); item; item = item->get_next()) {
			if (item->is_checked(0)) {
				chosen.push_back(item->get_text(0));
			}
		}
	}
	emit_changed(get_edited_property(), chosen);
}

void EditorPropertySaveProperties::_open() {
	search->clear();
	custom->clear();
	_fill();
	dialog->popup_centered_clamped(Size2(420, 520) * EDSCALE);
	search->grab_focus();
}

void EditorPropertySaveProperties::update_property() {
	SaveState *state = Object::cast_to<SaveState>(get_edited_object());
	const int count = state ? state->get_properties().size() : 0;
	button->set_text(count == 0 ? TTR("Choose...") : vformat(TTRN("%d property", "%d properties", count), count));
	button->set_tooltip_text(TTR("Choose the properties of the target that are saved."));
}

EditorPropertySaveProperties::EditorPropertySaveProperties() {
	button = memnew(Button);
	button->set_clip_text(true);
	button->connect(SceneStringName(pressed), callable_mp(this, &EditorPropertySaveProperties::_open));
	add_child(button);
	add_focusable(button);

	dialog = memnew(AcceptDialog);
	dialog->set_title(TTR("Properties to save"));
	dialog->set_ok_button_text(TTR("Done"));
	dialog->connect(SceneStringName(confirmed), callable_mp(this, &EditorPropertySaveProperties::_confirmed));
	add_child(dialog);

	VBoxContainer *box = memnew(VBoxContainer);
	dialog->add_child(box);

	search = memnew(LineEdit);
	search->set_placeholder(TTR("Filter properties"));
	search->set_clear_button_enabled(true);
	search->connect(SceneStringName(text_changed), callable_mp(this, &EditorPropertySaveProperties::_filter_changed));
	box->add_child(search);

	tree = memnew(Tree);
	tree->set_hide_root(true);
	tree->set_v_size_flags(SIZE_EXPAND_FILL);
	tree->set_custom_minimum_size(Size2(0, 320) * EDSCALE);
	box->add_child(tree);

	custom = memnew(LineEdit);
	custom->set_placeholder(TTR("Add a path inside a property, such as scale:x, and press Enter"));
	custom->connect(SceneStringName(text_submitted), callable_mp(this, &EditorPropertySaveProperties::_add_custom));
	box->add_child(custom);
}

bool EditorInspectorPluginSaveState::can_handle(Object *p_object) {
	return Object::cast_to<SaveState>(p_object) != nullptr;
}

bool EditorInspectorPluginSaveState::parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide) {
	if (p_path == "properties") {
		add_property_editor(p_path, memnew(EditorPropertySaveProperties));
		return true;
	}
	return false;
}

SaveStateEditorPlugin::SaveStateEditorPlugin() {
	Ref<EditorInspectorPluginSaveState> plugin;
	plugin.instantiate();
	add_inspector_plugin(plugin);
}
