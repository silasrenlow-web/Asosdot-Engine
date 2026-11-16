/**************************************************************************/
/*  transmit_dock.cpp                                                     */
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
/* the following conditions:                                             */
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

#include "transmit_dock.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/settings/editor_command_palette.h"
#include "editor/themes/editor_scale.h"
#include "scene/animation/animation_mixer.h"
#include "scene/gui/box_container.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/option_button.h"
#include "scene/gui/tree.h"
#include "scene/main/global_transmit.h"

static const char *RECEIVERS_META = "_transmit_receivers";

// "#12" is channel number 12; anything else is a channel name.
static String _channel_text(const Variant &p_channel) {
	return p_channel.get_type() == Variant::INT ? "#" + itos(p_channel) : String(p_channel);
}

static Variant _channel_from_text(const String &p_text) {
	if (p_text.begins_with("#") && p_text.substr(1).is_valid_int()) {
		return p_text.substr(1).to_int();
	}
	return StringName(p_text);
}

Variant TransmitDock::_parse_channel(int p_kind, const String &p_text) {
	if (p_kind == 1) {
		const String digits = p_text.trim_prefix("#").strip_edges();
		if (!digits.is_valid_int()) {
			return Variant();
		}
		return digits.to_int();
	}
	const String name = p_text.strip_edges();
	return name.is_empty() ? Variant() : Variant(StringName(name));
}

Node *TransmitDock::_get_node() const {
	return ObjectDB::get_instance<Node>(node_id);
}

Array TransmitDock::_get_receivers() const {
	Node *node = _get_node();
	return node ? Array(node->get_meta(RECEIVERS_META, Array())).duplicate(true) : Array();
}

void TransmitDock::_set_receivers(const Array &p_receivers, const String &p_action) {
	Node *node = _get_node();
	ERR_FAIL_NULL(node);
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(p_action, UndoRedo::MERGE_DISABLE, node);
	if (p_receivers.is_empty()) {
		undo_redo->add_do_method(node, "remove_meta", RECEIVERS_META);
	} else {
		undo_redo->add_do_method(node, "set_meta", RECEIVERS_META, p_receivers);
	}
	if (node->has_meta(RECEIVERS_META)) {
		undo_redo->add_undo_method(node, "set_meta", RECEIVERS_META, _get_receivers());
	} else {
		undo_redo->add_undo_method(node, "remove_meta", RECEIVERS_META);
	}
	undo_redo->add_do_method(this, "_update");
	undo_redo->add_undo_method(this, "_update");
	undo_redo->commit_action();
}

void TransmitDock::_selection_changed() {
	const List<Node *> &selected = EditorNode::get_singleton()->get_editor_selection()->get_top_selected_node_list();
	Node *node = selected.size() == 1 ? selected.front()->get() : nullptr;
	node_id = node ? node->get_instance_id() : ObjectID();
	_update();
}

void TransmitDock::_update() {
	Node *node = _get_node();
	tree->clear();
	const bool has_node = node != nullptr;
	kind->set_disabled(!has_node);
	channel->set_editable(has_node);
	method->set_editable(has_node);
	add->set_disabled(!has_node);
	channel_pick->set_disabled(!has_node);
	method_pick->set_disabled(!has_node);
	if (!node) {
		title->set_text(TTR("Select a single node to choose the channels it listens to."));
		empty_note->hide();
		return;
	}
	title->set_text(vformat(TTR("Channels %s listens to:"), node->get_name()));

	TreeItem *root = tree->create_item();
	const Array receivers = _get_receivers();
	for (int i = 0; i < receivers.size(); i++) {
		const Dictionary receiver = receivers[i];
		const Variant channel_value = receiver.get("channel", Variant());
		const StringName method_name = receiver.get("method", StringName());
		TreeItem *item = tree->create_item(root);
		item->set_text(0, _channel_text(channel_value));
		item->set_editable(0, true);
		item->set_icon(0, get_editor_theme_icon(SNAME("KeyTransmit")));
		item->set_text(1, method_name);
		item->set_editable(1, true);
		item->set_metadata(0, i);
		item->add_button(1, get_editor_theme_icon(SNAME("Remove")), 0, false, TTR("Stop listening on this channel."));
		if (!node->has_method(method_name)) {
			// The method may be in a script that isn't a tool; check the script itself.
			Ref<Script> script = node->get_script();
			if (script.is_null() || !script->has_method(method_name)) {
				item->set_custom_color(1, get_theme_color(SNAME("warning_color"), EditorStringName(Editor)));
				item->set_tooltip_text(1, vformat(TTR("This node has no method '%s'."), method_name));
			}
		}
	}
	empty_note->set_visible(receivers.is_empty());
}

void TransmitDock::_add_pressed() {
	const Variant channel_value = _parse_channel(kind->get_selected(), channel->get_text());
	const String method_name = method->get_text().strip_edges();
	if (channel_value.get_type() == Variant::NIL) {
		EditorNode::get_singleton()->show_warning(kind->get_selected() == 1 ? TTR("Type the channel's number.") : TTR("Type the channel's name."));
		return;
	}
	if (method_name.is_empty()) {
		EditorNode::get_singleton()->show_warning(TTR("Type or pick the method that receives the transmission."));
		return;
	}
	Array receivers = _get_receivers();
	for (const Variant &entry : receivers) {
		const Dictionary receiver = entry;
		if (receiver.get("channel", Variant()) == channel_value && String(receiver.get("method", "")) == method_name) {
			return; // Already there.
		}
	}
	Dictionary receiver;
	receiver["channel"] = channel_value;
	receiver["method"] = StringName(method_name);
	receivers.push_back(receiver);
	_set_receivers(receivers, TTR("Listen to Channel"));
	channel->clear();
}

void TransmitDock::_tree_edited() {
	TreeItem *item = tree->get_edited();
	if (!item) {
		return;
	}
	const int index = item->get_metadata(0);
	Array receivers = _get_receivers();
	ERR_FAIL_INDEX(index, receivers.size());
	Dictionary receiver = receivers[index];
	if (tree->get_edited_column() == 0) {
		const Variant channel_value = _channel_from_text(item->get_text(0).strip_edges());
		if (String(channel_value).is_empty()) {
			callable_mp(this, &TransmitDock::_update).call_deferred();
			return;
		}
		receiver["channel"] = channel_value;
	} else {
		const String method_name = item->get_text(1).strip_edges();
		if (method_name.is_empty()) {
			callable_mp(this, &TransmitDock::_update).call_deferred();
			return;
		}
		receiver["method"] = StringName(method_name);
	}
	receivers[index] = receiver;
	// Deferred: the tree is rebuilt by the change, which must not happen inside its own signal.
	callable_mp(this, &TransmitDock::_set_receivers).call_deferred(receivers, TTR("Change Channel Receiver"));
}

void TransmitDock::_tree_button_clicked(Object *p_item, int p_column, int p_id, MouseButton p_button) {
	TreeItem *item = Object::cast_to<TreeItem>(p_item);
	if (!item || p_button != MouseButton::LEFT) {
		return;
	}
	const int index = item->get_metadata(0);
	Array receivers = _get_receivers();
	ERR_FAIL_INDEX(index, receivers.size());
	receivers.remove_at(index);
	callable_mp(this, &TransmitDock::_set_receivers).call_deferred(receivers, TTR("Stop Listening to Channel"));
}

// Channels the scene already uses: on Transmit tracks and on receivers.
void TransmitDock::_fill_channel_pick() {
	PopupMenu *popup = channel_pick->get_popup();
	popup->clear();
	Node *scene = EditorNode::get_singleton()->get_edited_scene();
	if (!scene) {
		return;
	}
	Array found;
	auto note = [&](const Variant &p_channel) {
		if (GlobalTransmit::is_valid_channel(p_channel) && !found.has(p_channel)) {
			found.push_back(p_channel);
		}
	};
	LocalVector<Node *> stack;
	stack.push_back(scene);
	while (!stack.is_empty()) {
		Node *node = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);
		for (int i = 0; i < node->get_child_count(); i++) {
			stack.push_back(node->get_child(i));
		}
		for (const Variant &entry : Array(node->get_meta(RECEIVERS_META, Array()))) {
			note(Dictionary(entry).get("channel", Variant()));
		}
		AnimationMixer *mixer = Object::cast_to<AnimationMixer>(node);
		if (!mixer) {
			continue;
		}
		LocalVector<StringName> names;
		mixer->get_animation_list(&names);
		for (const StringName &name : names) {
			Ref<Animation> animation = mixer->get_animation(name);
			for (int t = 0; animation.is_valid() && t < animation->get_track_count(); t++) {
				if (animation->track_get_type(t) != Animation::TYPE_TRANSMIT) {
					continue;
				}
				for (int k = 0; k < animation->track_get_key_count(t); k++) {
					note(animation->transmit_track_get_channel(t, k));
				}
			}
		}
	}
	if (found.is_empty()) {
		popup->add_item(TTR("No channels in this scene yet."));
		popup->set_item_disabled(-1 + popup->get_item_count(), true);
		return;
	}
	found.sort();
	for (const Variant &value : found) {
		popup->add_item(_channel_text(value));
		popup->set_item_metadata(-1 + popup->get_item_count(), value);
	}
}

void TransmitDock::_channel_picked(int p_index) {
	const Variant value = channel_pick->get_popup()->get_item_metadata(p_index);
	kind->select(value.get_type() == Variant::INT ? 1 : 0);
	channel->set_text(value.get_type() == Variant::INT ? itos(value) : String(value));
}

void TransmitDock::_fill_method_pick() {
	PopupMenu *popup = method_pick->get_popup();
	popup->clear();
	Node *node = _get_node();
	Ref<Script> script = node ? Ref<Script>(node->get_script()) : Ref<Script>();
	if (script.is_null()) {
		popup->add_item(TTR("Attach a script to pick its methods."));
		popup->set_item_disabled(-1 + popup->get_item_count(), true);
		return;
	}
	List<MethodInfo> methods;
	script->get_script_method_list(&methods);
	PackedStringArray names;
	for (const MethodInfo &mi : methods) {
		if (!names.has(mi.name)) {
			names.push_back(mi.name);
		}
	}
	names.sort();
	for (const String &name : names) {
		popup->add_item(name);
	}
}

void TransmitDock::_method_picked(int p_index) {
	method->set_text(method_pick->get_popup()->get_item_text(p_index));
}

void TransmitDock::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			EditorNode::get_singleton()->get_editor_selection()->connect("selection_changed", callable_mp(this, &TransmitDock::_selection_changed));
			_update();
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			add->set_button_icon(get_editor_theme_icon(SNAME("Add")));
			channel_pick->set_button_icon(get_editor_theme_icon(SNAME("GuiDropdown")));
			method_pick->set_button_icon(get_editor_theme_icon(SNAME("GuiDropdown")));
		} break;
	}
}

void TransmitDock::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_update"), &TransmitDock::_update);
}

TransmitDock::TransmitDock() {
	singleton = this;
	set_name(TTRC("Transmit"));
	set_icon_name("KeyTransmit");
	set_dock_shortcut(ED_SHORTCUT_AND_COMMAND("docks/open_transmit", TTRC("Open Transmit Dock")));
	set_default_slot(EditorDock::DOCK_SLOT_RIGHT_UL);

	VBoxContainer *main = memnew(VBoxContainer);
	main->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(main);

	title = memnew(Label);
	title->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	title->set_custom_minimum_size(Size2(200 * EDSCALE, 0));
	main->add_child(title);

	tree = memnew(Tree);
	tree->set_columns(2);
	tree->set_column_titles_visible(true);
	tree->set_column_title(0, TTR("Channel"));
	tree->set_column_title(1, TTR("Method"));
	tree->set_hide_root(true);
	tree->set_v_size_flags(SIZE_EXPAND_FILL);
	tree->set_custom_minimum_size(Size2(0, 120 * EDSCALE));
	tree->connect("item_edited", callable_mp(this, &TransmitDock::_tree_edited));
	tree->connect("button_clicked", callable_mp(this, &TransmitDock::_tree_button_clicked));
	main->add_child(tree);

	empty_note = memnew(Label);
	empty_note->set_text(TTR("Not listening to any channel. Add one below: when anything transmits on it, from code or from a Transmit track, the method is called with the transmission's arguments while the game runs. A method that takes fewer arguments gets the first ones."));
	empty_note->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	empty_note->set_custom_minimum_size(Size2(200 * EDSCALE, 0));
	empty_note->set_theme_type_variation("HeaderSmall");
	empty_note->set_modulate(Color(1, 1, 1, 0.7));
	main->add_child(empty_note);

	HBoxContainer *channel_row = memnew(HBoxContainer);
	main->add_child(channel_row);
	kind = memnew(OptionButton);
	kind->add_item(TTR("Name"));
	kind->add_item(TTR("Number"));
	kind->set_tooltip_text(TTR("A channel is a name or a number."));
	channel_row->add_child(kind);
	channel = memnew(LineEdit);
	channel->set_placeholder(TTR("Channel"));
	channel->set_h_size_flags(SIZE_EXPAND_FILL);
	channel->connect(SceneStringName(text_submitted), callable_mp(this, &TransmitDock::_add_pressed).unbind(1));
	channel_row->add_child(channel);
	channel_pick = memnew(MenuButton);
	channel_pick->set_tooltip_text(TTR("Pick a channel used in this scene."));
	channel_pick->set_flat(false);
	channel_pick->get_popup()->connect("about_to_popup", callable_mp(this, &TransmitDock::_fill_channel_pick));
	channel_pick->get_popup()->connect("index_pressed", callable_mp(this, &TransmitDock::_channel_picked));
	channel_row->add_child(channel_pick);

	HBoxContainer *method_row = memnew(HBoxContainer);
	main->add_child(method_row);
	method = memnew(LineEdit);
	method->set_placeholder(TTR("Method"));
	method->set_h_size_flags(SIZE_EXPAND_FILL);
	method->connect(SceneStringName(text_submitted), callable_mp(this, &TransmitDock::_add_pressed).unbind(1));
	method_row->add_child(method);
	method_pick = memnew(MenuButton);
	method_pick->set_tooltip_text(TTR("Pick one of the script's methods."));
	method_pick->set_flat(false);
	method_pick->get_popup()->connect("about_to_popup", callable_mp(this, &TransmitDock::_fill_method_pick));
	method_pick->get_popup()->connect("index_pressed", callable_mp(this, &TransmitDock::_method_picked));
	method_row->add_child(method_pick);
	add = memnew(Button);
	add->set_text(TTR("Listen"));
	add->connect(SceneStringName(pressed), callable_mp(this, &TransmitDock::_add_pressed));
	method_row->add_child(add);
}

TransmitDock::~TransmitDock() {
	singleton = nullptr;
}
