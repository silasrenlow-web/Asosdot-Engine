/**************************************************************************/
/*  gaze_target_3d_gizmo_plugin.cpp                                       */
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

#include "gaze_target_3d_gizmo_plugin.h"

#include "../gaze_target_3d.h"

#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"

GazeTarget3DGizmoPlugin::GazeTarget3DGizmoPlugin() {
	create_material("gaze_target_lines", Color(1.0, 0.55, 0.2), false, false, true);
	create_icon_material("gaze_target_icon", EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("GazeTarget3D"), EditorStringName(EditorIcons)));
}

bool GazeTarget3DGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<GazeTarget3D>(p_spatial) != nullptr;
}

String GazeTarget3DGizmoPlugin::get_gizmo_name() const {
	return "GazeTarget3D";
}

int GazeTarget3DGizmoPlugin::get_priority() const {
	return -1;
}

void GazeTarget3DGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	GazeTarget3D *target = Object::cast_to<GazeTarget3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL(target);
	p_gizmo->clear();

	// Three rings, one around each axis.
	const float r = target->get_radius();
	const int segments = 48;
	Vector<Vector3> lines;
	for (int axis = 0; axis < 3; axis++) {
		for (int i = 0; i < segments; i++) {
			const float a = Math::TAU * i / segments;
			const float b = Math::TAU * (i + 1) / segments;
			Vector3 p(Math::cos(a) * r, Math::sin(a) * r, 0);
			Vector3 q(Math::cos(b) * r, Math::sin(b) * r, 0);
			if (axis == 1) {
				p = Vector3(p.x, 0, p.y);
				q = Vector3(q.x, 0, q.y);
			} else if (axis == 2) {
				p = Vector3(0, p.x, p.y);
				q = Vector3(0, q.x, q.y);
			}
			lines.push_back(p);
			lines.push_back(q);
		}
	}
	p_gizmo->add_lines(lines, get_material("gaze_target_lines", p_gizmo));
	p_gizmo->add_unscaled_billboard(get_material("gaze_target_icon", p_gizmo), 0.05);
}

EditorPluginEyeTracking::EditorPluginEyeTracking() {
	Ref<GazeTarget3DGizmoPlugin> gizmo_plugin;
	gizmo_plugin.instantiate();
	Node3DEditor::get_singleton()->add_gizmo_plugin(gizmo_plugin);
}
