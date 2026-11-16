/**************************************************************************/
/*  transmit_dock.h                                                       */
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

#pragma once

#include "editor/docks/editor_dock.h"

class Button;
class Label;
class LineEdit;
class MenuButton;
class OptionButton;
class Tree;

// Asosdot: the Transmit tab. Lists the Global Transmit channels the selected node listens to and
// the method each one calls; the node starts listening when it enters the tree while the game
// runs. Stored in the node's "_transmit_receivers" metadata, so it is saved with the scene.
class TransmitDock : public EditorDock {
	GDCLASS(TransmitDock, EditorDock);

	static inline TransmitDock *singleton = nullptr;

	ObjectID node_id;

	Label *title = nullptr;
	Tree *tree = nullptr;
	OptionButton *kind = nullptr;
	LineEdit *channel = nullptr;
	MenuButton *channel_pick = nullptr;
	LineEdit *method = nullptr;
	MenuButton *method_pick = nullptr;
	Button *add = nullptr;
	Label *empty_note = nullptr;

	Node *_get_node() const;
	Array _get_receivers() const;
	void _set_receivers(const Array &p_receivers, const String &p_action);
	static Variant _parse_channel(int p_kind, const String &p_text);

	void _selection_changed();
	void _update();
	void _add_pressed();
	void _tree_edited();
	void _tree_button_clicked(Object *p_item, int p_column, int p_id, MouseButton p_button);
	void _fill_channel_pick();
	void _channel_picked(int p_index);
	void _fill_method_pick();
	void _method_picked(int p_index);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	static TransmitDock *get_singleton() { return singleton; }

	TransmitDock();
	~TransmitDock();
};
