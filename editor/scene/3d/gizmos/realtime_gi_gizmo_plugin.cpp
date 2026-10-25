/**************************************************************************/
/*  realtime_gi_gizmo_plugin.cpp                                          */
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

#include "realtime_gi_gizmo_plugin.h"

#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/settings/editor_settings.h"
#include "scene/3d/realtime_gi.h"

RealtimeGIGizmoPlugin::RealtimeGIGizmoPlugin() {
	Color gizmo_color = EDITOR_GET("editors/3d_gizmos/gizmo_colors/lightmap_lines");
	create_material("realtime_gi_bounds", gizmo_color);
	gizmo_color.a = 0.35;
	create_material("realtime_gi_probes", gizmo_color);
	create_icon_material("realtime_gi_icon", EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("GizmoRealtimeGI"), EditorStringName(EditorIcons)));
}

bool RealtimeGIGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<RealtimeGI>(p_spatial) != nullptr;
}

String RealtimeGIGizmoPlugin::get_gizmo_name() const {
	return "RealtimeGI";
}

int RealtimeGIGizmoPlugin::get_priority() const {
	return -1;
}

void RealtimeGIGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	RealtimeGI *gi = Object::cast_to<RealtimeGI>(p_gizmo->get_node_3d());
	Ref<RealtimeGIData> data = gi->get_data();

	p_gizmo->clear();
	p_gizmo->add_unscaled_billboard(get_material("realtime_gi_icon", p_gizmo), 0.05);

	if (data.is_null() || !p_gizmo->is_selected()) {
		return;
	}

	// The bounds the bake lit, and a small cross on every probe, so it's
	// clear where dynamic objects pick up bounced light.
	AABB aabb = data->get_bounds();
	Vector<Vector3> lines;
	for (int i = 0; i < 12; i++) {
		Vector3 a, b;
		aabb.get_edge(i, a, b);
		lines.push_back(a);
		lines.push_back(b);
	}
	p_gizmo->add_lines(lines, get_material("realtime_gi_bounds", p_gizmo));

	PackedVector3Array probes = data->get_probe_points();
	if (!probes.is_empty()) {
		Vector<Vector3> crosses;
		const float size = 0.1;
		for (const Vector3 &p : probes) {
			for (int axis = 0; axis < 3; axis++) {
				Vector3 d;
				d[axis] = size;
				crosses.push_back(p - d);
				crosses.push_back(p + d);
			}
		}
		p_gizmo->add_lines(crosses, get_material("realtime_gi_probes", p_gizmo));
	}
}
