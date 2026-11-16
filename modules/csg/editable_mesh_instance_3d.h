/**************************************************************************/
/*  editable_mesh_instance_3d.h                                           */
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

#include "scene/3d/mesh_instance_3d.h"

class CSGShape3D;

// Asosdot: a MeshInstance3D whose mesh can be shaped like CSG. CSG shapes added as
// children cut into it, add to it or intersect it, live in the editor. The result is an
// ordinary mesh, saved with the scene: the node keeps everything a MeshInstance3D has
// (materials, overrides, shadows, GI, layers), the faces the shapes don't touch keep the
// source mesh's own normals, UVs, UV2 and colors, and nothing is computed at runtime unless
// asked for.
class EditableMeshInstance3D : public MeshInstance3D {
	GDCLASS(EditableMeshInstance3D, MeshInstance3D);

	Ref<Mesh> source_mesh;
	bool update_at_runtime = false;

	bool rebuild_queued = false;
	bool source_not_closed = false;
	bool source_partly_open = false;
	bool dropped_animation_data = false;
	int operand_count = 0;

	void _queue_rebuild();
	void _rebuild();
	bool _can_rebuild() const;
	Vector<CSGShape3D *> _get_operands() const;

protected:
	void _notification(int p_what);
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	// Called by CSG shapes that are children of this node when they change.
	void _operand_changed();

	void set_source_mesh(const Ref<Mesh> &p_mesh);
	Ref<Mesh> get_source_mesh() const;

	void set_update_at_runtime(bool p_enable);
	bool is_updating_at_runtime() const;

	void rebuild();
	bool has_edits() const;
	bool is_source_closed() const;
	void apply_edits();

	PackedStringArray get_configuration_warnings() const override;

	EditableMeshInstance3D();
};
