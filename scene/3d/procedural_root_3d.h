/**************************************************************************/
/*  procedural_root_3d.h                                                  */
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

#include "core/templates/hash_set.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/packed_scene.h"

// The node that holds what a script builds: its `_generate()` makes rooms, props and whole levels out of
// PackedScenes, and the editor shows the result (the nodes have no owner, so they are never saved in the scene)
// with each part's own scene one click away. Saving a part's scene updates every copy of it in the generated
// level.
class ProceduralRoot3D : public Node3D {
	GDCLASS(ProceduralRoot3D, Node3D);

public:
	enum PartReload {
		PART_RELOAD_IN_PLACE, // The saved scene replaces each copy where it stands (same place, same name).
		PART_RELOAD_REGENERATE, // Everything is generated again.
		PART_RELOAD_NONE,
	};

private:
	bool generate_in_editor = true;
	bool async_generation = false;
	PartReload part_reload = PART_RELOAD_IN_PLACE;
	bool generating = false;

#ifdef TOOLS_ENABLED
	// Scenes saved while this node was not in the tree (another tab): its copies are updated when it comes back.
	HashSet<String> pending_parts;
	void _reload_pending_parts();
#endif // TOOLS_ENABLED

	static void _find_parts(Node *p_node, const String &p_path, Vector<Node *> &r_parts);
	static void _collect_part_scenes(Node *p_node, HashSet<String> &r_paths);
	void _replace_part(Node *p_old, const Ref<PackedScene> &p_scene);

protected:
	void _notification(int p_what);
	static void _bind_methods();

	GDVIRTUAL0(_generate)

public:
	PackedStringArray get_configuration_warnings() const override;

	void set_generate_in_editor(bool p_enabled);
	bool is_generate_in_editor() const;
	void set_async_generation(bool p_async);
	bool is_async_generation() const;
	void set_part_reload(PartReload p_mode);
	PartReload get_part_reload() const;

	// Frees what was generated (every child without an owner) and runs `_generate()`.
	void generate();
	void clear_generated();
	// For a `_generate()` that waits for something: call it when the generating is over.
	void finish_generation();
	bool is_generating() const;
	Callable get_generate_button() const;
	Callable get_clear_button() const;

	// Instances a scene as a generated child (of `p_parent`, a node under this one, or of this node), at a
	// transform, with properties set on its root. The properties are kept so a reload puts them back.
	Node3D *add_part(const Ref<PackedScene> &p_scene, const Transform3D &p_transform = Transform3D(), const Dictionary &p_properties = Dictionary(), Node *p_parent = nullptr);
	// The scenes the generated parts come from.
	PackedStringArray get_part_scenes() const;
	// Replaces every generated copy of a scene with the scene as it is now.
	void reload_part_scene(const String &p_path);

	// A node a ProceduralRoot3D made: it has no owner and a ProceduralRoot3D is above it.
	static bool is_generated_node(const Node *p_node);
	// The generated node that `p_node` belongs to (the part's root if it is inside an instanced scene): what
	// a click on it selects. Null when it is not generated.
	static Node *get_generated_part(Node *p_node);
	static ProceduralRoot3D *get_generator_of(const Node *p_node);

#ifdef TOOLS_ENABLED
	// The editor calls this after it saved a scene file.
	static void notify_scene_saved(const String &p_path);
#endif // TOOLS_ENABLED

	ProceduralRoot3D();
	~ProceduralRoot3D();
};

VARIANT_ENUM_CAST(ProceduralRoot3D::PartReload);
