/**************************************************************************/
/*  save_state.h                                                          */
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

#include "scene/main/node.h"

// Marks a node as part of the saved game. Put one under any node, name the
// properties to keep, and the Saves singleton writes them into a save and puts
// them back on load. Nothing needs a script.
//
// The node whose properties are kept is the target, the parent unless a path says
// otherwise. A node that can come and go, an enemy that is killed or an item that
// is picked up, is spawnable: loading brings it back when the save has it and
// removes it when the save does not.
class SaveState : public Node {
	GDCLASS(SaveState, Node);

	NodePath target_path = NodePath("..");
	StringName save_id;
	Vector<String> properties;
	bool spawnable = false;

	// Chosen on the first need, for a node a script added with no id to its name.
	mutable String generated_id;

	static inline LocalVector<SaveState *> states_in_tree;

	Node *_get_scene_root() const;
	Variant _get_property(Node *p_target, const String &p_property, bool &r_valid) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// The load whose state this node was last given, so one load gives it once.
	int applied_generation = -1;

	// Every SaveState in the tree, in no particular order.
	static const LocalVector<SaveState *> &get_states_in_tree() { return states_in_tree; }

	PackedStringArray get_configuration_warnings() const override;

	void set_target(const NodePath &p_path);
	NodePath get_target() const;

	void set_save_id(const StringName &p_id);
	StringName get_save_id() const;

	void set_properties(const PackedStringArray &p_properties);
	PackedStringArray get_properties() const;

	void set_spawnable(bool p_spawnable);
	bool is_spawnable() const;

	// The node whose properties are kept, or null when the path leads nowhere.
	Node *get_target_node() const;

	// The id this node is saved under: the one set, or else where its target sits in
	// the scene, or for a node a script added at runtime, one that was made up.
	String get_state_id() const;

	// What a save holds for this node. Emits saving first so a script can add to it.
	Dictionary capture();
	// Puts a captured state back. Emits loaded afterwards.
	void apply(const Dictionary &p_state);

	SaveState();
	~SaveState();
};
