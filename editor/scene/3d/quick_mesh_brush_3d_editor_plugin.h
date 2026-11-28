/**************************************************************************/
/*  quick_mesh_brush_3d_editor_plugin.h                                   */
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

#include "core/object/undo_redo.h"
#include "editor/plugins/editor_plugin.h"
#include "editor/scene/3d/node_3d_editor_gizmos.h"
#include "scene/3d/quick_mesh_brush_3d.h"

class Button;
class EditorSpinSlider;
class HBoxContainer;
class Label;
class MenuButton;

// Asosdot: draws a Quick Mesh Brush's points and lets the 3D transform gizmo move, rotate
// and scale them as subgizmos.
class QuickMeshBrush3DGizmoPlugin : public EditorNode3DGizmoPlugin {
	GDCLASS(QuickMeshBrush3DGizmoPlugin, EditorNode3DGizmoPlugin);

public:
	bool has_gizmo(Node3D *p_spatial) override;
	String get_gizmo_name() const override;
	int get_priority() const override;
	void redraw(EditorNode3DGizmo *p_gizmo) override;

	int subgizmos_intersect_ray(const EditorNode3DGizmo *p_gizmo, Camera3D *p_camera, const Vector2 &p_point) const override;
	Vector<int> subgizmos_intersect_frustum(const EditorNode3DGizmo *p_gizmo, const Camera3D *p_camera, const Vector<Plane> &p_frustum) const override;
	bool wants_subgizmo_region_select(const EditorNode3DGizmo *p_gizmo) const override;
	Transform3D get_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id) const override;
	void set_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id, Transform3D p_transform) override;
	void commit_subgizmos(const EditorNode3DGizmo *p_gizmo, const Vector<int> &p_ids, const Vector<Transform3D> &p_restore, bool p_cancel = false) override;

	QuickMeshBrush3DGizmoPlugin();
};

class QuickMeshBrush3DEditorPlugin : public EditorPlugin {
	GDCLASS(QuickMeshBrush3DEditorPlugin, EditorPlugin);

	friend class QuickMeshBrush3DGizmoPlugin;

	enum Tool {
		TOOL_SELECT,
		TOOL_ADD,
	};

	enum Option {
		OPTION_SELECT_ALL,
		OPTION_RESET_ALL,
		OPTION_CLEAR_POINTS,
		OPTION_CREATE_COLLISION,
		OPTION_APPLY_EDITS,
	};

	Ref<QuickMeshBrush3DGizmoPlugin> gizmo_plugin;
	QuickMeshBrush3D *brush = nullptr;

	HBoxContainer *toolbar = nullptr;
	Button *edit_button = nullptr;
	HBoxContainer *edit_tools = nullptr;
	Button *select_button = nullptr;
	Button *add_button = nullptr;
	EditorSpinSlider *radius_slider = nullptr;
	EditorSpinSlider *hardness_slider = nullptr;
	Button *reset_button = nullptr;
	Button *delete_button = nullptr;
	MenuButton *options = nullptr;
	Label *info = nullptr;

	Tool tool = TOOL_SELECT;
	// Kept across selections, so going from one brush to another stays in edit mode.
	bool editing = false;
	real_t new_radius = 1.0;
	real_t new_hardness = 0.0;

	bool hovering = false;
	Transform3D hover;

	Vector<int> shown_selection;
	bool updating_sliders = false;

	void _update_theme();
	void _update_toolbar();
	void _edit_toggled(bool p_on);
	void _tool_changed(int p_tool);
	void _radius_changed(double p_value);
	void _hardness_changed(double p_value);
	void _option_pressed(int p_option);

	Vector<int> _get_selected_points() const;
	void _select_points(const Vector<int> &p_points);
	Ref<EditorNode3DGizmo> _get_gizmo() const;
	bool _pick_surface(Camera3D *p_camera, const Vector2 &p_point, Dictionary &r_hit) const;
	void _commit_points(const String &p_name, const Array &p_before, UndoRedo::MergeMode p_merge = UndoRedo::MERGE_DISABLE);
	void _add_point(const Dictionary &p_hit);
	void _reset_selected();
	void _delete_selected();
	void _create_collision();
	void _apply_edits();

protected:
	void _notification(int p_what);

public:
	inline static QuickMeshBrush3DEditorPlugin *singleton = nullptr;

	bool is_editing(const Node3D *p_node) const { return editing && brush && brush == p_node; }
	bool get_hover(const Node3D *p_node, Transform3D &r_hover, real_t &r_radius) const;
	void set_editing(bool p_editing);

	EditorPlugin::AfterGUIInput forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) override;
	String get_plugin_name() const override { return "QuickMeshBrush3D"; }
	void edit(Object *p_object) override;
	bool handles(Object *p_object) const override;
	void make_visible(bool p_visible) override;

	QuickMeshBrush3DEditorPlugin();
};
