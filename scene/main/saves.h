/**************************************************************************/
/*  saves.h                                                               */
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

#include "core/object/object.h"
#include "core/variant/dictionary.h"
#include "core/variant/typed_array.h"

class SaveState;

// The save and load framework: slots on disk, written and read with the engine's
// own formats, compressed and encrypted, with the state of the nodes that carry a
// SaveState and whatever else a game keeps in Saves.data.
//
// What a save holds is the world: the state of every SaveState node, kept as a
// dictionary by the id each is saved under. Loading makes that the world and puts
// it onto the nodes that are in the tree, the scene it was saved in if need be,
// and onto the ones that come into the tree later, when their scene is loaded
// after the save.
class Saves : public Object {
	GDCLASS(Saves, Object);

	static Saves *singleton;

	enum Flags {
		FLAG_COMPRESSED = 1,
		FLAG_ENCRYPTED = 2,
		FLAG_PASSWORD = 4,
		FLAG_JSON = 8,
	};

	Dictionary data;
	int save_version = 1;
	double play_time = 0.0;
	String password;

	double autosave_interval = 0.0;
	String autosave_slot = "autosave";
	double autosave_timer = 0.0;

	// Every state the game knows of, by id, and the scenes whose spawnable nodes it
	// accounts for.
	Dictionary world;
	PackedStringArray visited;
	// Bumped by every load, so a node is only given a state once per load.
	int generation = 0;

	ObjectID last_scene;
	bool scene_tracked = false;
	String pending_loaded_slot;

	// A header and a body, in a file.
	Error _pack(const Dictionary &p_header, const Dictionary &p_body, const String &p_password, Vector<uint8_t> &r_file) const;
	Error _unpack(const Vector<uint8_t> &p_file, const String &p_password, bool p_header_only, Dictionary &r_header, Dictionary &r_body) const;

	String _get_project_key() const;
	String _get_directory() const;
	String _get_slot_path(const String &p_slot) const;
	bool _is_valid_slot(const String &p_slot) const;

	Error _write_file(const String &p_slot, const Vector<uint8_t> &p_file) const;
	Error _read_slot(const String &p_slot, const String &p_password, bool p_header_only, Dictionary &r_content) const;
	Dictionary _describe(const String &p_slot, const Dictionary &p_header, int64_t p_size) const;

	Dictionary _capture_live();
	void _apply_world();
	void _spawn(const String &p_id, const Dictionary &p_state);
	void _on_scene_changed();
	String _get_level() const;

	Vector<uint8_t> _capture_thumbnail() const;

protected:
	static void _bind_methods();

public:
	static Saves *get_singleton() { return singleton; }

	// Where a value that is not a plain Variant is turned into one a file can hold
	// and back: a resource by the path it loads from, a node by where it sits.
	static Variant to_storable(const Variant &p_value, int p_depth, const String &p_context);
	static Variant from_storable(const Variant &p_value, int p_depth);

	// Makes the project a key to encrypt its saves with, when it has none. The
	// editor does this when a SaveState is added.
	static void ensure_project_key();

	// Called by the scene tree every frame, and by a SaveState when it is ready.
	void _tick(double p_delta, bool p_paused);
	void _state_entered(SaveState *p_state);

	Error save(const String &p_slot, const Dictionary &p_info = Dictionary(), const String &p_password = String());
	Error load_slot(const String &p_slot, const String &p_password = String(), bool p_restore_scene = true);
	Dictionary read(const String &p_slot, const String &p_password = String()) const;
	Dictionary get_info(const String &p_slot) const;
	PackedStringArray list_slots() const;
	bool has_slot(const String &p_slot) const;
	Error delete_slot(const String &p_slot);
	Error copy_slot(const String &p_from, const String &p_to);
	String get_slot_path(const String &p_slot) const;

	void stash();
	Dictionary snapshot();
	void restore(const Dictionary &p_snapshot);
	void reset();

	void set_data(const Dictionary &p_data);
	Dictionary get_data() const;
	void set_save_version(int p_version);
	int get_save_version() const;
	void set_play_time(double p_seconds);
	double get_play_time() const;
	void set_password(const String &p_password);
	String get_password() const;
	void set_autosave_interval(double p_seconds);
	double get_autosave_interval() const;
	void set_autosave_slot(const String &p_slot);
	String get_autosave_slot() const;

	Saves();
	~Saves();
};
