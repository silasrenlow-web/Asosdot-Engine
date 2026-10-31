/**************************************************************************/
/*  light_catcher_3d_gizmo_plugin.cpp                                     */
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

#include "light_catcher_3d_gizmo_plugin.h"

#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/light_catcher_3d.h"

LightCatcher3DGizmoPlugin::LightCatcher3DGizmoPlugin() {
	Ref<StandardMaterial3D> lines = memnew(StandardMaterial3D);
	lines->set_shading_mode(StandardMaterial3D::SHADING_MODE_UNSHADED);
	lines->set_flag(StandardMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	lines->set_flag(StandardMaterial3D::FLAG_SRGB_VERTEX_COLOR, true);
	lines->set_flag(StandardMaterial3D::FLAG_DISABLE_FOG, true);
	lines->set_transparency(StandardMaterial3D::TRANSPARENCY_ALPHA);
	lines->set_flag(StandardMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
	lines->set_render_priority(StandardMaterial3D::RENDER_PRIORITY_MAX);
	add_material("light_catcher_lines", lines);
	create_icon_material("light_catcher_icon", EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("GizmoLightCatcher3D"), EditorStringName(EditorIcons)));
}

bool LightCatcher3DGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<LightCatcher3D>(p_spatial) != nullptr;
}

String LightCatcher3DGizmoPlugin::get_gizmo_name() const {
	return "LightCatcher3D";
}

int LightCatcher3DGizmoPlugin::get_priority() const {
	return -1;
}

void LightCatcher3DGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	LightCatcher3D *catcher = Object::cast_to<LightCatcher3D>(p_gizmo->get_node_3d());
	p_gizmo->clear();
	p_gizmo->add_unscaled_billboard(get_material("light_catcher_icon", p_gizmo), 0.04);

	// Each sample point is a small star, from dark grey in shadow to bright
	// yellow at the threshold and beyond, so dark corners show while you
	// place lights.
	const PackedVector3Array points = catcher->get_sample_points();
	const real_t threshold = MAX(catcher->get_threshold(), (real_t)0.0001);
	Vector<Vector3> lines;
	Vector<Color> colors;
	for (int i = 0; i < points.size(); i++) {
		const real_t brightness = catcher->get_point_brightness(i);
		const real_t t = CLAMP(brightness / threshold, (real_t)0.0, (real_t)1.0);
		const Color color = Color(0.25, 0.25, 0.28).lerp(Color(1.0, 0.92, 0.35), t);
		const real_t size = 0.08 + 0.06 * t;
		static const Vector3 axes[6] = { Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1), Vector3(0.577, 0.577, 0.577), Vector3(-0.577, 0.577, 0.577), Vector3(0.577, -0.577, 0.577) };
		for (const Vector3 &axis : axes) {
			lines.push_back(points[i] - axis * size);
			lines.push_back(points[i] + axis * size);
			colors.push_back(color);
			colors.push_back(color);
		}
	}

	// Selected, it also draws a line to every light that reaches it, fading
	// with how much that light gives.
	if (p_gizmo->is_selected()) {
		const Transform3D inv = catcher->get_global_transform().affine_inverse();
		const Vector3 origin = points.is_empty() ? Vector3() : points[0];
		const TypedArray<Light3D> lights = catcher->get_lights();
		for (int i = 0; i < lights.size(); i++) {
			Light3D *light = Object::cast_to<Light3D>(lights[i]);
			if (!light) {
				continue;
			}
			const real_t amount = CLAMP(catcher->get_light_contribution(light) / threshold, (real_t)0.0, (real_t)1.0);
			Vector3 to = inv.xform(light->get_global_position());
			if (light->get_light_type() == RSE::LIGHT_DIRECTIONAL) {
				to = origin + inv.basis.xform(light->get_global_transform().basis.get_column(2)).normalized() * 3.0;
			}
			const Color color = light->get_color();
			lines.push_back(origin);
			lines.push_back(to);
			colors.push_back(Color(color.r, color.g, color.b, 0.15 + 0.7 * amount));
			colors.push_back(Color(color.r, color.g, color.b, 0.05));
		}
	}

	if (!lines.is_empty()) {
		// add_lines takes one colour for all of them; a line mesh carries one
		// per segment.
		Ref<ArrayMesh> mesh;
		mesh.instantiate();
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = lines;
		arrays[Mesh::ARRAY_COLOR] = colors;
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_LINES, arrays);
		p_gizmo->add_mesh(mesh, get_material("light_catcher_lines", p_gizmo));
		p_gizmo->add_collision_segments(lines);
	}
}
