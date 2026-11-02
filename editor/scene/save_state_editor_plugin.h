/**************************************************************************/
/*  save_state_editor_plugin.h                                            */
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

#include "editor/inspector/editor_inspector.h"
#include "editor/plugins/editor_plugin.h"

class AcceptDialog;
class Button;
class LineEdit;
class Tree;
class TreeItem;

// The list of properties a SaveState keeps, shown as a button that opens the
// target's properties with a tick beside each, instead of a list of names to type.
class EditorPropertySaveProperties : public EditorProperty {
	GDCLASS(EditorPropertySaveProperties, EditorProperty);

	Button *button = nullptr;
	AcceptDialog *dialog = nullptr;
	LineEdit *search = nullptr;
	LineEdit *custom = nullptr;
	Tree *tree = nullptr;

	void _open();
	void _fill();
	void _filter_changed(const String &p_text);
	void _add_custom(const String &p_text);
	void _confirmed();

	TreeItem *_add_item(const String &p_name, bool p_checked);

protected:
	static void _bind_methods() {}

public:
	virtual void update_property() override;

	EditorPropertySaveProperties();
};

class EditorInspectorPluginSaveState : public EditorInspectorPlugin {
	GDCLASS(EditorInspectorPluginSaveState, EditorInspectorPlugin);

public:
	virtual bool can_handle(Object *p_object) override;
	virtual bool parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide = false) override;
};

class SaveStateEditorPlugin : public EditorPlugin {
	GDCLASS(SaveStateEditorPlugin, EditorPlugin);

public:
	virtual String get_plugin_name() const override { return "SaveState"; }

	SaveStateEditorPlugin();
};
