/**************************************************************************/
/*  quick_mesh_brush_3d_editor_plugin.cpp                                 */
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

#include "quick_mesh_brush_3d_editor_plugin.h"

#include "core/object/callable_mp.h"
#include "core/os/keyboard.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/gui/editor_spin_slider.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/debugger/view_3d_controller.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/separator.h"

#ifndef PHYSICS_3D_DISABLED
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "scene/resources/3d/concave_polygon_shape_3d.h"
#endif // PHYSICS_3D_DISABLED

static void add_ring(Vector<Vector3> &r_lines, const Transform3D &p_frame, real_t p_radius) {
	const int segments = 48;
	const Basis basis = p_frame.basis.orthonormalized();
	Vector3 previous = p_frame.origin + basis.get_column(0) * p_radius;
	for (int i = 1; i <= segments; i++) {
		const real_t angle = Math::TAU * i / segments;
		const Vector3 next = p_frame.origin + (basis.get_column(0) * Math::cos(angle) + basis.get_column(2) * Math::sin(angle)) * p_radius;
		r_lines.push_back(previous);
		r_lines.push_back(next);
		previous = next;
	}
}

// Gizmo.

bool QuickMeshBrush3DGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<QuickMeshBrush3D>(p_spatial) != nullptr;
}

String QuickMeshBrush3DGizmoPlugin::get_gizmo_name() const {
	return "QuickMeshBrush3D";
}

int QuickMeshBrush3DGizmoPlugin::get_priority() const {
	return -1;
}

void QuickMeshBrush3DGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	p_gizmo->clear();
	QuickMeshBrush3D *brush = Object::cast_to<QuickMeshBrush3D>(p_gizmo->get_node_3d());
	QuickMeshBrush3DEditorPlugin *editor = QuickMeshBrush3DEditorPlugin::singleton;
	if (!brush || !editor || !editor->is_editing(brush)) {
		return;
	}

	const Vector<int> selection = p_gizmo->get_subgizmo_selection();
	Vector<Vector3> rings, selected_rings, offsets, hover_ring;
	Vector<Vector3> handles, selected_handles;
	for (int i = 0; i < brush->get_point_count(); i++) {
		const bool selected = selection.has(i);
		const Transform3D transform = brush->get_point_transform(i);
		const Transform3D rest = brush->get_point_rest(i);
		const real_t radius = brush->get_point_radius(i);
		const real_t hardness = brush->get_point_hardness(i);
		Vector<Vector3> &ring = selected ? selected_rings : rings;
		add_ring(ring, transform, radius);
		if (hardness > 0.01) {
			add_ring(ring, transform, radius * hardness);
		}
		// Where the point started, so a moved point shows how far it bends the surface.
		if (rest.origin.distance_to(transform.origin) > 0.0001) {
			offsets.push_back(rest.origin);
			offsets.push_back(transform.origin);
			add_ring(offsets, rest, radius * 0.08);
		}
		(selected ? selected_handles : handles).push_back(transform.origin);
	}
	Transform3D hover;
	real_t hover_radius = 0.0;
	if (editor->get_hover(brush, hover, hover_radius)) {
		add_ring(hover_ring, hover, hover_radius);
		hover_ring.push_back(hover.origin);
		hover_ring.push_back(hover.origin + hover.basis.get_column(1).normalized() * hover_radius * 0.3);
	}

	if (!rings.is_empty()) {
		p_gizmo->add_lines(rings, get_material("ring", p_gizmo));
	}
	if (!selected_rings.is_empty()) {
		p_gizmo->add_lines(selected_rings, get_material("ring_selected", p_gizmo));
	}
	if (!offsets.is_empty()) {
		p_gizmo->add_lines(offsets, get_material("offset", p_gizmo));
	}
	if (!hover_ring.is_empty()) {
		p_gizmo->add_lines(hover_ring, get_material("hover", p_gizmo));
	}
	if (!handles.is_empty()) {
		p_gizmo->add_vertices(handles, get_material("point", p_gizmo), Mesh::PRIMITIVE_POINTS);
	}
	if (!selected_handles.is_empty()) {
		p_gizmo->add_vertices(selected_handles, get_material("point_selected", p_gizmo), Mesh::PRIMITIVE_POINTS);
	}
}

int QuickMeshBrush3DGizmoPlugin::subgizmos_intersect_ray(const EditorNode3DGizmo *p_gizmo, Camera3D *p_camera, const Vector2 &p_point) const {
	QuickMeshBrush3D *brush = Object::cast_to<QuickMeshBrush3D>(p_gizmo->get_node_3d());
	QuickMeshBrush3DEditorPlugin *editor = QuickMeshBrush3DEditorPlugin::singleton;
	if (!brush || !editor || !editor->is_editing(brush)) {
		return -1;
	}
	const Transform3D global = brush->get_global_transform();
	int best = -1;
	real_t best_distance = 12 * EDSCALE;
	for (int i = 0; i < brush->get_point_count(); i++) {
		const Vector3 position = global.xform(brush->get_point_transform(i).origin);
		if (p_camera->is_position_behind(position)) {
			continue;
		}
		const real_t distance = p_camera->unproject_position(position).distance_to(p_point);
		if (distance < best_distance) {
			best_distance = distance;
			best = i;
		}
	}
	return best;
}

Vector<int> QuickMeshBrush3DGizmoPlugin::subgizmos_intersect_frustum(const EditorNode3DGizmo *p_gizmo, const Camera3D *p_camera, const Vector<Plane> &p_frustum) const {
	Vector<int> inside;
	QuickMeshBrush3D *brush = Object::cast_to<QuickMeshBrush3D>(p_gizmo->get_node_3d());
	QuickMeshBrush3DEditorPlugin *editor = QuickMeshBrush3DEditorPlugin::singleton;
	if (!brush || !editor || !editor->is_editing(brush)) {
		return inside;
	}
	const Transform3D global = brush->get_global_transform();
	for (int i = 0; i < brush->get_point_count(); i++) {
		const Vector3 position = global.xform(brush->get_point_transform(i).origin);
		bool contained = true;
		for (const Plane &plane : p_frustum) {
			if (plane.distance_to(position) > 0) {
				contained = false;
				break;
			}
		}
		if (contained) {
			inside.push_back(i);
		}
	}
	return inside;
}

bool QuickMeshBrush3DGizmoPlugin::wants_subgizmo_region_select(const EditorNode3DGizmo *p_gizmo) const {
	QuickMeshBrush3DEditorPlugin *editor = QuickMeshBrush3DEditorPlugin::singleton;
	return editor && editor->is_editing(p_gizmo->get_node_3d());
}

Transform3D QuickMeshBrush3DGizmoPlugin::get_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id) const {
	QuickMeshBrush3D *brush = Object::cast_to<QuickMeshBrush3D>(p_gizmo->get_node_3d());
	if (!brush || p_id < 0 || p_id >= brush->get_point_count()) {
		return Transform3D(); // A selection left over from an undone point.
	}
	return brush->get_point_transform(p_id);
}

void QuickMeshBrush3DGizmoPlugin::set_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id, Transform3D p_transform) {
	QuickMeshBrush3D *brush = Object::cast_to<QuickMeshBrush3D>(p_gizmo->get_node_3d());
	if (!brush || p_id < 0 || p_id >= brush->get_point_count()) {
		return;
	}
	brush->set_point_transform(p_id, p_transform);
	brush->update_gizmos();
}

void QuickMeshBrush3DGizmoPlugin::commit_subgizmos(const EditorNode3DGizmo *p_gizmo, const Vector<int> &p_ids, const Vector<Transform3D> &p_restore, bool p_cancel) {
	QuickMeshBrush3D *brush = Object::cast_to<QuickMeshBrush3D>(p_gizmo->get_node_3d());
	if (!brush) {
		return;
	}
	if (p_cancel) {
		for (int i = 0; i < p_ids.size(); i++) {
			if (p_ids[i] >= 0 && p_ids[i] < brush->get_point_count()) {
				brush->set_point_transform(p_ids[i], p_restore[i]);
			}
		}
		brush->update_gizmos();
		return;
	}
	Array before = brush->get_points_data();
	for (int i = 0; i < p_ids.size(); i++) {
		if (p_ids[i] >= 0 && p_ids[i] < before.size()) {
			Dictionary point = Dictionary(before[p_ids[i]]).duplicate();
			point["transform"] = p_restore[i];
			before[p_ids[i]] = point;
		}
	}
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(p_ids.size() == 1 ? TTR("Move Brush Point") : TTR("Move Brush Points"), UndoRedo::MERGE_DISABLE, brush);
	ur->add_do_method(brush, "set_points_data", brush->get_points_data());
	ur->add_undo_method(brush, "set_points_data", before);
	ur->add_do_method(brush, "update_gizmos");
	ur->add_undo_method(brush, "update_gizmos");
	ur->commit_action();
}

QuickMeshBrush3DGizmoPlugin::QuickMeshBrush3DGizmoPlugin() {
	create_material("ring", Color(0.9, 0.95, 1.0, 0.75));
	create_material("ring_selected", Color(1.0, 0.62, 0.2, 1.0));
	create_material("offset", Color(0.4, 0.75, 1.0, 0.9));
	create_material("hover", Color(0.55, 1.0, 0.55, 0.9));
	const Ref<Texture2D> handle = EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("EditorPathSmoothHandle"), EditorStringName(EditorIcons));
	create_handle_material("point", false, handle);
	create_handle_material("point_selected", false, handle);
	get_material("point")->set_albedo(Color(0.9, 0.95, 1.0));
	get_material("point_selected")->set_albedo(Color(1.0, 0.62, 0.2));
}

// Editor plugin.

bool QuickMeshBrush3DEditorPlugin::get_hover(const Node3D *p_node, Transform3D &r_hover, real_t &r_radius) const {
	if (!hovering || !is_editing(p_node) || tool != TOOL_ADD) {
		return false;
	}
	r_hover = hover;
	r_radius = new_radius;
	return true;
}

Ref<EditorNode3DGizmo> QuickMeshBrush3DEditorPlugin::_get_gizmo() const {
	if (!brush) {
		return Ref<EditorNode3DGizmo>();
	}
	for (const Ref<Node3DGizmo> &gizmo : brush->get_gizmos()) {
		Ref<EditorNode3DGizmo> editor_gizmo = gizmo;
		if (editor_gizmo.is_valid() && editor_gizmo->get_plugin() == gizmo_plugin) {
			return editor_gizmo;
		}
	}
	return Ref<EditorNode3DGizmo>();
}

Vector<int> QuickMeshBrush3DEditorPlugin::_get_selected_points() const {
	Vector<int> selected;
	const Ref<EditorNode3DGizmo> gizmo = _get_gizmo();
	if (gizmo.is_null()) {
		return selected;
	}
	for (int id : gizmo->get_subgizmo_selection()) {
		if (id >= 0 && id < brush->get_point_count()) {
			selected.push_back(id);
		}
	}
	selected.sort();
	return selected;
}

void QuickMeshBrush3DEditorPlugin::_select_points(const Vector<int> &p_points) {
	if (!brush) {
		return;
	}
	Node3DEditorSelectedItem *se = EditorNode::get_singleton()->get_editor_selection()->get_node_editor_data<Node3DEditorSelectedItem>(brush);
	if (!se) {
		return;
	}
	se->subgizmos.clear();
	se->gizmo.unref();
	const Ref<EditorNode3DGizmo> gizmo = _get_gizmo();
	if (gizmo.is_valid()) {
		for (int id : p_points) {
			if (id >= 0 && id < brush->get_point_count()) {
				se->subgizmos.insert(id, brush->get_point_transform(id));
			}
		}
		if (!se->subgizmos.is_empty()) {
			se->gizmo = gizmo;
		}
	}
	brush->update_gizmos();
	Node3DEditor::get_singleton()->update_transform_gizmo();
}

bool QuickMeshBrush3DEditorPlugin::_pick_surface(Camera3D *p_camera, const Vector2 &p_point, Dictionary &r_hit) const {
	if (!brush) {
		return false;
	}
	const Transform3D to_local = brush->get_global_transform().affine_inverse();
	const Vector3 from = to_local.xform(p_camera->project_ray_origin(p_point));
	const Vector3 dir = to_local.basis.xform(p_camera->project_ray_normal(p_point));
	r_hit = brush->intersect_ray(from, dir);
	return !r_hit.is_empty();
}

void QuickMeshBrush3DEditorPlugin::_commit_points(const String &p_name, const Array &p_before, UndoRedo::MergeMode p_merge) {
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(p_name, p_merge, brush);
	ur->add_do_method(brush, "set_points_data", brush->get_points_data());
	ur->add_undo_method(brush, "set_points_data", p_before);
	ur->add_do_method(brush, "update_gizmos");
	ur->add_undo_method(brush, "update_gizmos");
	if (p_before.size() != brush->get_point_count()) {
		// Points came or went: a selection made of their numbers means nothing any more.
		ur->add_do_method(Node3DEditor::get_singleton(), "_clear_subgizmo_selection", brush);
		ur->add_undo_method(Node3DEditor::get_singleton(), "_clear_subgizmo_selection", brush);
	}
	ur->commit_action();
}

void QuickMeshBrush3DEditorPlugin::_add_point(const Dictionary &p_hit) {
	const Array before = brush->get_points_data();
	const int index = brush->add_point(p_hit["position"], p_hit["normal"], new_radius, new_hardness);
	brush->set_point_surface_position(index, p_hit["surface_position"]);
	_commit_points(TTR("Add Brush Point"), before);
	_select_points({ index });
}

void QuickMeshBrush3DEditorPlugin::_reset_selected() {
	const Vector<int> selected = _get_selected_points();
	if (selected.is_empty()) {
		return;
	}
	const Array before = brush->get_points_data();
	for (int id : selected) {
		brush->reset_point(id);
	}
	_commit_points(TTR("Reset Brush Points"), before);
	_select_points(selected);
}

void QuickMeshBrush3DEditorPlugin::_delete_selected() {
	Vector<int> selected = _get_selected_points();
	if (selected.is_empty()) {
		return;
	}
	const Array before = brush->get_points_data();
	for (int i = selected.size() - 1; i >= 0; i--) {
		brush->remove_point(selected[i]);
	}
	_commit_points(selected.size() == 1 ? TTR("Delete Brush Point") : TTR("Delete Brush Points"), before);
}

void QuickMeshBrush3DEditorPlugin::_create_collision() {
#ifndef PHYSICS_3D_DISABLED
	Node *root = EditorNode::get_singleton()->get_edited_scene();
	if (!brush || !root) {
		return;
	}
	String name = "StaticBody3D";
	for (int i = 2; brush->has_node(NodePath(name)); i++) {
		name = "StaticBody3D" + itos(i);
	}
	StaticBody3D *body = memnew(StaticBody3D);
	body->set_name(name);
	CollisionShape3D *shape_node = memnew(CollisionShape3D);
	shape_node->set_name("CollisionShape3D");
	Ref<ConcavePolygonShape3D> shape;
	shape.instantiate();
	shape_node->set_shape(shape);
	body->add_child(shape_node);

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Create Brush Collision"), UndoRedo::MERGE_DISABLE, brush);
	ur->add_do_method(brush, "add_child", body, true);
	ur->add_do_method(body, "set_owner", root);
	ur->add_do_method(shape_node, "set_owner", root);
	ur->add_do_property(brush, "collision_shape", NodePath(name + "/CollisionShape3D"));
	ur->add_do_method(brush, "rebuild");
	ur->add_undo_property(brush, "collision_shape", brush->get_collision_shape());
	ur->add_undo_method(brush, "remove_child", body);
	ur->add_do_reference(body);
	ur->commit_action();
#endif // PHYSICS_3D_DISABLED
}

void QuickMeshBrush3DEditorPlugin::_apply_edits() {
	if (!brush || brush->get_point_count() == 0) {
		return;
	}
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Apply Brush Edits"), UndoRedo::MERGE_DISABLE, brush);
	ur->add_do_method(Node3DEditor::get_singleton(), "_clear_subgizmo_selection", brush);
	ur->add_do_method(brush, "apply_edits");
	ur->add_undo_method(brush, "set_source_mesh", brush->get_source_mesh());
	ur->add_undo_method(brush, "set_points_data", brush->get_points_data());
	ur->add_undo_method(Node3DEditor::get_singleton(), "_clear_subgizmo_selection", brush);
	ur->add_do_method(brush, "update_gizmos");
	ur->add_undo_method(brush, "update_gizmos");
	ur->commit_action();
}

void QuickMeshBrush3DEditorPlugin::_option_pressed(int p_option) {
	if (!brush) {
		return;
	}
	switch (p_option) {
		case OPTION_SELECT_ALL: {
			Vector<int> all;
			for (int i = 0; i < brush->get_point_count(); i++) {
				all.push_back(i);
			}
			_select_points(all);
		} break;
		case OPTION_RESET_ALL: {
			const Array before = brush->get_points_data();
			for (int i = 0; i < brush->get_point_count(); i++) {
				brush->reset_point(i);
			}
			_commit_points(TTR("Reset Brush Points"), before);
		} break;
		case OPTION_CLEAR_POINTS: {
			const Array before = brush->get_points_data();
			brush->clear_points();
			_commit_points(TTR("Clear Brush Points"), before);
		} break;
		case OPTION_CREATE_COLLISION: {
			_create_collision();
		} break;
		case OPTION_APPLY_EDITS: {
			_apply_edits();
		} break;
	}
}

void QuickMeshBrush3DEditorPlugin::_radius_changed(double p_value) {
	if (updating_sliders || !brush) {
		return;
	}
	new_radius = p_value;
	const Vector<int> selected = _get_selected_points();
	if (selected.is_empty()) {
		brush->update_gizmos(); // The hover ring shows the size new points get.
		return;
	}
	const Array before = brush->get_points_data();
	for (int id : selected) {
		brush->set_point_radius(id, p_value);
	}
	_commit_points(TTR("Set Brush Point Radius"), before, UndoRedo::MERGE_ENDS);
}

void QuickMeshBrush3DEditorPlugin::_hardness_changed(double p_value) {
	if (updating_sliders || !brush) {
		return;
	}
	new_hardness = p_value;
	const Vector<int> selected = _get_selected_points();
	if (selected.is_empty()) {
		return;
	}
	const Array before = brush->get_points_data();
	for (int id : selected) {
		brush->set_point_hardness(id, p_value);
	}
	_commit_points(TTR("Set Brush Point Hardness"), before, UndoRedo::MERGE_ENDS);
}

void QuickMeshBrush3DEditorPlugin::set_editing(bool p_editing) {
	editing = p_editing;
	hovering = false;
	if (edit_button) {
		edit_button->set_pressed_no_signal(editing);
	}
	if (brush) {
		Node3DEditor::get_singleton()->clear_subgizmo_selection(brush);
		brush->update_gizmos();
	}
	_update_toolbar();
}

void QuickMeshBrush3DEditorPlugin::_edit_toggled(bool p_on) {
	set_editing(p_on);
}

void QuickMeshBrush3DEditorPlugin::_tool_changed(int p_tool) {
	tool = Tool(p_tool);
	select_button->set_pressed_no_signal(tool == TOOL_SELECT);
	add_button->set_pressed_no_signal(tool == TOOL_ADD);
	hovering = false;
	if (brush) {
		brush->update_gizmos();
	}
}

void QuickMeshBrush3DEditorPlugin::_update_toolbar() {
	if (!toolbar) {
		return;
	}
	edit_tools->set_visible(editing && brush);
	const bool has_selection = !_get_selected_points().is_empty();
	reset_button->set_disabled(!has_selection);
	delete_button->set_disabled(!has_selection);
}

void QuickMeshBrush3DEditorPlugin::_update_theme() {
	edit_button->set_button_icon(toolbar->get_editor_theme_icon(SNAME("QuickMeshBrush3D")));
	select_button->set_button_icon(toolbar->get_editor_theme_icon(SNAME("ToolSelect")));
	add_button->set_button_icon(toolbar->get_editor_theme_icon(SNAME("EditorHandleAdd")));
	reset_button->set_button_icon(toolbar->get_editor_theme_icon(SNAME("Reload")));
	delete_button->set_button_icon(toolbar->get_editor_theme_icon(SNAME("Remove")));
}

void QuickMeshBrush3DEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_PROCESS: {
			if (!brush) {
				return;
			}
			// Show the first selected point's settings, and keep the counts current.
			const Vector<int> selected = _get_selected_points();
			if (selected != shown_selection) {
				shown_selection = selected;
				if (!selected.is_empty()) {
					updating_sliders = true;
					radius_slider->set_value(brush->get_point_radius(selected[0]));
					hardness_slider->set_value(brush->get_point_hardness(selected[0]));
					updating_sliders = false;
				}
				_update_toolbar();
			}
			info->set_text(vformat(TTR("%d points, %d triangles"), brush->get_point_count(), brush->get_triangle_count()));
		} break;
	}
}

EditorPlugin::AfterGUIInput QuickMeshBrush3DEditorPlugin::forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	if (!brush || !editing) {
		return EditorPlugin::AFTER_GUI_INPUT_PASS;
	}

	const Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MouseButton::LEFT && mb->is_pressed()) {
		const View3DController::NavigationScheme nav_scheme = (View3DController::NavigationScheme)EDITOR_GET("editors/3d/navigation/navigation_scheme").operator int();
		if ((nav_scheme == View3DController::NAV_SCHEME_MAYA || nav_scheme == View3DController::NAV_SCHEME_MODO) && mb->is_alt_pressed()) {
			return EditorPlugin::AFTER_GUI_INPUT_PASS;
		}
		// Add Point tool, or Ctrl+Click with the Select tool, puts a point on the surface.
		if (tool == TOOL_ADD || mb->is_command_or_control_pressed()) {
			Dictionary hit;
			if (_pick_surface(p_camera, mb->get_position(), hit)) {
				_add_point(hit);
				if (tool == TOOL_ADD && !mb->is_shift_pressed()) {
					_tool_changed(TOOL_SELECT); // Ready to move the new point with the gizmo.
				}
				return EditorPlugin::AFTER_GUI_INPUT_STOP;
			}
		}
		// Clicks pick points and drags box select them; other nodes are not picked while editing.
		return EditorPlugin::AFTER_GUI_INPUT_CUSTOM;
	}

	const Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid() && tool == TOOL_ADD) {
		Dictionary hit;
		const bool was_hovering = hovering;
		hovering = _pick_surface(p_camera, mm->get_position(), hit);
		if (hovering) {
			hover = Transform3D(Basis(), hit["position"]);
			const Vector3 normal = hit["normal"];
			Vector3 x = Vector3(1, 0, 0) - normal * normal.x;
			if (x.length_squared() < 0.01) {
				x = Vector3(0, 0, 1) - normal * normal.z;
			}
			x.normalize();
			hover.basis = Basis(x, normal, x.cross(normal).normalized());
		}
		if (hovering || was_hovering) {
			brush->update_gizmos();
		}
		return EditorPlugin::AFTER_GUI_INPUT_CUSTOM;
	}

	const Ref<InputEventKey> k = p_event;
	if (k.is_valid() && k->is_pressed() && !k->is_echo() && (k->get_keycode() == Key::KEY_DELETE || k->get_keycode() == Key::BACKSPACE)) {
		if (!_get_selected_points().is_empty()) {
			_delete_selected();
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		}
	}
	return EditorPlugin::AFTER_GUI_INPUT_CUSTOM;
}

void QuickMeshBrush3DEditorPlugin::edit(Object *p_object) {
	if (brush) {
		brush->update_gizmos();
	}
	brush = Object::cast_to<QuickMeshBrush3D>(p_object);
	hovering = false;
	shown_selection.clear();
	if (brush) {
		brush->update_gizmos();
	}
	_update_toolbar();
}

bool QuickMeshBrush3DEditorPlugin::handles(Object *p_object) const {
	return Object::cast_to<QuickMeshBrush3D>(p_object) != nullptr;
}

void QuickMeshBrush3DEditorPlugin::make_visible(bool p_visible) {
	toolbar->set_visible(p_visible);
	set_process(p_visible);
	if (!p_visible) {
		if (brush) {
			QuickMeshBrush3D *was = brush;
			brush = nullptr;
			was->update_gizmos();
		}
		hovering = false;
	}
}

QuickMeshBrush3DEditorPlugin::QuickMeshBrush3DEditorPlugin() {
	singleton = this;
	gizmo_plugin.instantiate();
	Node3DEditor::get_singleton()->add_gizmo_plugin(gizmo_plugin);

	toolbar = memnew(HBoxContainer);
	toolbar->hide();

	toolbar->add_child(memnew(VSeparator));

	edit_button = memnew(Button);
	edit_button->set_theme_type_variation(SceneStringName(FlatButton));
	edit_button->set_toggle_mode(true);
	edit_button->set_text(TTR("Edit Points"));
	edit_button->set_tooltip_text(TTR("Quick Mesh Brush edit mode: place points on the mesh and move, rotate or scale them with the gizmo to bend the surface around them.\nWhile it is on, clicks pick brush points instead of other nodes."));
	edit_button->connect(SceneStringName(toggled), callable_mp(this, &QuickMeshBrush3DEditorPlugin::_edit_toggled));
	toolbar->add_child(edit_button);

	edit_tools = memnew(HBoxContainer);
	edit_tools->hide();
	toolbar->add_child(edit_tools);

	select_button = memnew(Button);
	select_button->set_theme_type_variation(SceneStringName(FlatButton));
	select_button->set_toggle_mode(true);
	select_button->set_pressed(true);
	select_button->set_tooltip_text(TTR("Select Points") + "\n" + TTR("Click a point, Shift+Click to add to the selection, drag a box to select several.") + "\n" + TTR("Ctrl+Click on the mesh: add a point."));
	select_button->connect(SceneStringName(pressed), callable_mp(this, &QuickMeshBrush3DEditorPlugin::_tool_changed).bind(TOOL_SELECT));
	edit_tools->add_child(select_button);

	add_button = memnew(Button);
	add_button->set_theme_type_variation(SceneStringName(FlatButton));
	add_button->set_toggle_mode(true);
	add_button->set_tooltip_text(TTR("Add Point") + "\n" + TTR("Click on the mesh to place a point; it is then selected, ready to move.") + "\n" + TTR("Shift+Click: keep adding points."));
	add_button->connect(SceneStringName(pressed), callable_mp(this, &QuickMeshBrush3DEditorPlugin::_tool_changed).bind(TOOL_ADD));
	edit_tools->add_child(add_button);

	edit_tools->add_child(memnew(VSeparator));

	radius_slider = memnew(EditorSpinSlider);
	radius_slider->set_label(TTR("Radius"));
	radius_slider->set_min(0.01);
	radius_slider->set_max(20.0);
	radius_slider->set_step(0.01);
	radius_slider->set_allow_greater(true);
	radius_slider->set_suffix("m");
	radius_slider->set_value(new_radius);
	radius_slider->set_custom_minimum_size(Size2(135 * EDSCALE, 0));
	radius_slider->set_tooltip_text(TTR("How far around a point the surface bends. Sets the selected points, or the next new point."));
	radius_slider->connect(SceneStringName(value_changed), callable_mp(this, &QuickMeshBrush3DEditorPlugin::_radius_changed));
	edit_tools->add_child(radius_slider);

	hardness_slider = memnew(EditorSpinSlider);
	hardness_slider->set_label(TTR("Hardness"));
	hardness_slider->set_min(0.0);
	hardness_slider->set_max(1.0);
	hardness_slider->set_step(0.01);
	hardness_slider->set_value(new_hardness);
	hardness_slider->set_custom_minimum_size(Size2(135 * EDSCALE, 0));
	hardness_slider->set_tooltip_text(TTR("0 bends smoothly from the centre to the edge. Higher values keep a flat middle with a steeper edge, like the floor of a puddle."));
	hardness_slider->connect(SceneStringName(value_changed), callable_mp(this, &QuickMeshBrush3DEditorPlugin::_hardness_changed));
	edit_tools->add_child(hardness_slider);

	edit_tools->add_child(memnew(VSeparator));

	reset_button = memnew(Button);
	reset_button->set_theme_type_variation(SceneStringName(FlatButton));
	reset_button->set_tooltip_text(TTR("Reset Selected Points (flatten them back to where they were placed)"));
	reset_button->connect(SceneStringName(pressed), callable_mp(this, &QuickMeshBrush3DEditorPlugin::_reset_selected));
	edit_tools->add_child(reset_button);

	delete_button = memnew(Button);
	delete_button->set_theme_type_variation(SceneStringName(FlatButton));
	delete_button->set_tooltip_text(TTR("Delete Selected Points (Delete)"));
	delete_button->connect(SceneStringName(pressed), callable_mp(this, &QuickMeshBrush3DEditorPlugin::_delete_selected));
	edit_tools->add_child(delete_button);

	options = memnew(MenuButton);
	options->set_flat(false);
	options->set_theme_type_variation("FlatMenuButton");
	options->set_text(TTR("Brush"));
	PopupMenu *menu = options->get_popup();
	menu->add_item(TTR("Select All Points"), OPTION_SELECT_ALL);
	menu->add_item(TTR("Reset All Points"), OPTION_RESET_ALL);
	menu->add_item(TTR("Remove All Points"), OPTION_CLEAR_POINTS);
	menu->add_separator();
	menu->add_item(TTR("Create Collision (Kept Updated)"), OPTION_CREATE_COLLISION);
	menu->set_item_tooltip(menu->get_item_count() - 1, TTR("Adds a StaticBody3D with a trimesh CollisionShape3D and links it, so the collision follows every brush edit."));
	menu->add_item(TTR("Apply Edits to Source Mesh"), OPTION_APPLY_EDITS);
	menu->set_item_tooltip(menu->get_item_count() - 1, TTR("Makes the bent mesh the new source mesh and removes the points, to start a new round of edits on top."));
	menu->connect(SceneStringName(id_pressed), callable_mp(this, &QuickMeshBrush3DEditorPlugin::_option_pressed));
	edit_tools->add_child(options);

	info = memnew(Label);
	info->set_theme_type_variation("HeaderSmall");
	info->set_modulate(Color(1, 1, 1, 0.65));
	edit_tools->add_child(info);

	toolbar->connect(SceneStringName(theme_changed), callable_mp(this, &QuickMeshBrush3DEditorPlugin::_update_theme));
	Node3DEditor::get_singleton()->add_control_to_menu_panel(toolbar);
}
