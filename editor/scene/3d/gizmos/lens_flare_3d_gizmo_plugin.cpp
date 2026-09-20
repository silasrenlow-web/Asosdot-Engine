/**************************************************************************/
/*  lens_flare_3d_gizmo_plugin.cpp                                        */
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

#include "lens_flare_3d_gizmo_plugin.h"

#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "scene/3d/lens_flare_3d.h"

LensFlare3DGizmoPlugin::LensFlare3DGizmoPlugin() {
	create_material("lens_flare_lines", Color(1, 1, 1), false, false, true);
	create_icon_material("lens_flare_icon", EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("GizmoLensFlare3D"), EditorStringName(EditorIcons)));
}

bool LensFlare3DGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<LensFlare3D>(p_spatial) != nullptr;
}

String LensFlare3DGizmoPlugin::get_gizmo_name() const {
	return "LensFlare3D";
}

int LensFlare3DGizmoPlugin::get_priority() const {
	return -1;
}

void LensFlare3DGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	LensFlare3D *flare = Object::cast_to<LensFlare3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL(flare);

	p_gizmo->clear();

	const Color color = flare->get_color();

	// A flare is a point, so the only thing worth drawing is which way it faces,
	// and only for the settings where facing matters.
	if (p_gizmo->is_selected() && (flare->is_directional() || flare->get_angle_limit() > 0.0)) {
		Ref<Material> material = get_material("lens_flare_lines", p_gizmo);
		Vector<Vector3> lines;

		if (flare->is_directional()) {
			// An arrow down -Z, pointing the way the light travels, so a flare
			// lines up with a DirectionalLight3D by matching its rotation.
			const float length = 1.5;
			lines.push_back(Vector3());
			lines.push_back(Vector3(0, 0, -length));

			for (int i = 0; i < 4; i++) {
				const Basis turn(Vector3(0, 0, 1), Math::TAU * i / 4);
				lines.push_back(Vector3(0, 0, -length));
				lines.push_back(turn.xform(Vector3(0, 0.18, -length + 0.4)));
			}
		} else {
			// The cone outside which the flare is not drawn.
			const float length = 1.5;
			const float radius = Math::tan(flare->get_angle_limit()) * length;

			for (int i = 0; i < 4; i++) {
				const Basis turn(Vector3(0, 0, 1), Math::TAU * i / 4);
				lines.push_back(Vector3());
				lines.push_back(turn.xform(Vector3(radius, 0, -length)));
			}

			const int segments = 32;
			for (int i = 0; i < segments; i++) {
				const float a = Math::TAU * i / segments;
				const float b = Math::TAU * (i + 1) / segments;
				lines.push_back(Vector3(Math::cos(a) * radius, Math::sin(a) * radius, -length));
				lines.push_back(Vector3(Math::cos(b) * radius, Math::sin(b) * radius, -length));
			}
		}

		p_gizmo->add_lines(lines, material, false, color);
	}

	Ref<Material> icon = get_material("lens_flare_icon", p_gizmo);
	p_gizmo->add_unscaled_billboard(icon, 0.05, color);
}
