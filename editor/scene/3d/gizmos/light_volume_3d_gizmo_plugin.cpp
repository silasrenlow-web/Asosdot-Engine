/**************************************************************************/
/*  light_volume_3d_gizmo_plugin.cpp                                      */
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

#include "light_volume_3d_gizmo_plugin.h"

#include "core/math/math_funcs.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_volume_3d.h"

namespace {
// Closest point parameter along the line (p_origin, p_direction) to the
// segment (p_segment[0], p_segment[1]), all in the same space. Standard
// two-line closest point, clamped to the segment.
real_t closest_axis_parameter(const Vector3 &p_origin, const Vector3 &p_direction, const Vector3 p_segment[2]) {
	Vector3 segment_direction = p_segment[1] - p_segment[0];
	Vector3 between = p_origin - p_segment[0];
	real_t a = p_direction.dot(p_direction);
	real_t b = p_direction.dot(segment_direction);
	real_t c = segment_direction.dot(segment_direction);
	real_t d = p_direction.dot(between);
	real_t e = segment_direction.dot(between);
	real_t denominator = a * c - b * b;
	real_t segment_t = 0.0;
	if (denominator > CMP_EPSILON) {
		segment_t = CLAMP((a * e - b * d) / denominator, 0.0, 1.0);
	}
	Vector3 on_segment = p_segment[0] + segment_direction * segment_t;
	return p_direction.dot(on_segment - p_origin) / MAX(a, CMP_EPSILON);
}

void push_ring(PackedVector3Array &r_lines, const Vector3 &p_center, const Vector2 &p_extent, bool p_box, int p_segments) {
	if (p_box) {
		Vector3 corners[4] = {
			p_center + Vector3(-p_extent.x, -p_extent.y, 0),
			p_center + Vector3(p_extent.x, -p_extent.y, 0),
			p_center + Vector3(p_extent.x, p_extent.y, 0),
			p_center + Vector3(-p_extent.x, p_extent.y, 0),
		};
		for (int i = 0; i < 4; i++) {
			r_lines.push_back(corners[i]);
			r_lines.push_back(corners[(i + 1) % 4]);
		}
		return;
	}
	for (int i = 0; i < p_segments; i++) {
		real_t a0 = Math_TAU * real_t(i) / real_t(p_segments);
		real_t a1 = Math_TAU * real_t(i + 1) / real_t(p_segments);
		r_lines.push_back(p_center + Vector3(Math::cos(a0) * p_extent.x, Math::sin(a0) * p_extent.y, 0));
		r_lines.push_back(p_center + Vector3(Math::cos(a1) * p_extent.x, Math::sin(a1) * p_extent.y, 0));
	}
}
} // namespace

LightVolume3DGizmoPlugin::LightVolume3DGizmoPlugin() {
	create_material("shape_material", Color(1.0, 0.85, 0.4));
	Color fill = Color(1.0, 0.85, 0.4, 0.12);
	create_material("shape_material_internal", fill);
	create_icon_material("light_volume_icon", EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("GizmoLightVolume3D"), EditorStringName(EditorIcons)));
	create_handle_material("handles");
}

bool LightVolume3DGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<LightVolume3D>(p_spatial) != nullptr;
}

String LightVolume3DGizmoPlugin::get_gizmo_name() const {
	return "LightVolume3D";
}

int LightVolume3DGizmoPlugin::get_priority() const {
	return -1;
}

void LightVolume3DGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	LightVolume3D *volume = Object::cast_to<LightVolume3D>(p_gizmo->get_node_3d());
	p_gizmo->clear();
	p_gizmo->add_unscaled_billboard(get_material("light_volume_icon", p_gizmo), 0.04);

	const bool is_box = volume->get_shape() == LightVolume3D::SHAPE_BOX;
	const Vector2 near_extent = volume->get_near_extent();
	const Vector2 far_extent = volume->get_far_extent();
	const Vector3 far_center = Vector3(volume->get_shear().x, volume->get_shear().y, -volume->get_length());

	PackedVector3Array lines;
	push_ring(lines, Vector3(), near_extent, is_box, 24);
	push_ring(lines, far_center, far_extent, is_box, 24);
	// Four rails join the rings so the slant reads at a glance.
	const Vector2 rails[4] = { Vector2(1, 0), Vector2(-1, 0), Vector2(0, 1), Vector2(0, -1) };
	for (const Vector2 &rail : rails) {
		lines.push_back(Vector3(rail.x * near_extent.x, rail.y * near_extent.y, 0));
		lines.push_back(far_center + Vector3(rail.x * far_extent.x, rail.y * far_extent.y, 0));
	}
	p_gizmo->add_lines(lines, get_material("shape_material", p_gizmo));
	p_gizmo->add_collision_segments(lines);

	PackedVector3Array handles;
	handles.push_back(far_center);
	handles.push_back(far_center + Vector3(far_extent.x, 0, 0));
	handles.push_back(Vector3(near_extent.x, 0, 0));
	Vector<int> ids = { HANDLE_LENGTH, HANDLE_END_EXTENT, HANDLE_START_EXTENT };
	p_gizmo->add_handles(handles, get_material("handles", p_gizmo), ids);
}

String LightVolume3DGizmoPlugin::get_handle_name(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	switch (p_id) {
		case HANDLE_LENGTH:
			return "Length";
		case HANDLE_END_EXTENT: {
			LightVolume3D *volume = Object::cast_to<LightVolume3D>(p_gizmo->get_node_3d());
			return volume->get_shape() == LightVolume3D::SHAPE_BOX ? "Width" : "End Radius";
		}
		case HANDLE_START_EXTENT: {
			LightVolume3D *volume = Object::cast_to<LightVolume3D>(p_gizmo->get_node_3d());
			return volume->get_shape() == LightVolume3D::SHAPE_BOX ? "Height" : "Start Radius";
		}
	}
	return "";
}

Variant LightVolume3DGizmoPlugin::get_handle_value(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	LightVolume3D *volume = Object::cast_to<LightVolume3D>(p_gizmo->get_node_3d());
	switch (p_id) {
		case HANDLE_LENGTH:
			return volume->get_length();
		case HANDLE_END_EXTENT:
			return volume->get_shape() == LightVolume3D::SHAPE_BOX ? Variant(volume->get_size()) : Variant(volume->get_end_radius());
		case HANDLE_START_EXTENT:
			return volume->get_shape() == LightVolume3D::SHAPE_BOX ? Variant(volume->get_size()) : Variant(volume->get_start_radius());
	}
	return Variant();
}

void LightVolume3DGizmoPlugin::set_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, Camera3D *p_camera, const Point2 &p_point) {
	LightVolume3D *volume = Object::cast_to<LightVolume3D>(p_gizmo->get_node_3d());
	Transform3D global = volume->get_global_transform();
	Transform3D inverse = global.affine_inverse();

	Vector3 segment_global[2] = {
		p_camera->project_ray_origin(p_point),
		p_camera->project_ray_origin(p_point) + p_camera->project_ray_normal(p_point) * 1000.0,
	};
	Vector3 segment[2] = { inverse.xform(segment_global[0]), inverse.xform(segment_global[1]) };

	switch (p_id) {
		case HANDLE_LENGTH: {
			real_t t = closest_axis_parameter(Vector3(), Vector3(0, 0, -1), segment);
			volume->set_length(CLAMP(t, 0.05, 4096.0));
		} break;
		case HANDLE_END_EXTENT: {
			Vector3 far_center = Vector3(volume->get_shear().x, volume->get_shear().y, -volume->get_length());
			real_t t = closest_axis_parameter(far_center, Vector3(1, 0, 0), segment);
			if (volume->get_shape() == LightVolume3D::SHAPE_BOX) {
				Vector2 size = volume->get_size();
				size.x = MAX(t * 2.0, 0.02);
				volume->set_size(size);
			} else {
				volume->set_end_radius(MAX(t, 0.0));
			}
		} break;
		case HANDLE_START_EXTENT: {
			if (volume->get_shape() == LightVolume3D::SHAPE_BOX) {
				real_t t = closest_axis_parameter(Vector3(), Vector3(0, 1, 0), segment);
				Vector2 size = volume->get_size();
				size.y = MAX(t * 2.0, 0.02);
				volume->set_size(size);
			} else {
				real_t t = closest_axis_parameter(Vector3(), Vector3(1, 0, 0), segment);
				volume->set_start_radius(MAX(t, 0.0));
			}
		} break;
	}
}

void LightVolume3DGizmoPlugin::commit_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, const Variant &p_restore, bool p_cancel) {
	LightVolume3D *volume = Object::cast_to<LightVolume3D>(p_gizmo->get_node_3d());
	String property;
	switch (p_id) {
		case HANDLE_LENGTH:
			property = "length";
			break;
		case HANDLE_END_EXTENT:
			property = volume->get_shape() == LightVolume3D::SHAPE_BOX ? "size" : "end_radius";
			break;
		case HANDLE_START_EXTENT:
			property = volume->get_shape() == LightVolume3D::SHAPE_BOX ? "size" : "start_radius";
			break;
		default:
			return;
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (p_cancel) {
		volume->set(property, p_restore);
		return;
	}
	undo_redo->create_action(TTR("Set Light Volume ") + get_handle_name(p_gizmo, p_id, p_secondary));
	undo_redo->add_do_property(volume, property, volume->get(property));
	undo_redo->add_undo_property(volume, property, p_restore);
	undo_redo->commit_action();
}
