/**************************************************************************/
/*  mesh_paint_editor_plugin.cpp                                          */
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

#include "mesh_paint_editor_plugin.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/keyboard.h"
#include "core/os/os.h"
#include "editor/docks/editor_dock.h"
#include "editor/docks/editor_dock_manager.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/gui/editor_spin_slider.h"
#include "editor/inspector/editor_inspector.h"
#include "editor/inspector/editor_resource_picker.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/quick_mesh_brush_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/debugger/view_3d_controller.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/color_picker.h"
#include "scene/gui/item_list.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/option_button.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/separator.h"
#include "scene/gui/split_container.h"
#include "scene/main/viewport.h"
#include "servers/rendering/rendering_server.h"

static const char *PAINT_FILES_META = "_mesh_paint_files";

static bool point_in_triangle(const Vector2 &p_point, const Vector2 &p_a, const Vector2 &p_b, const Vector2 &p_c) {
	const real_t d1 = (p_point - p_b).cross(p_a - p_b);
	const real_t d2 = (p_point - p_c).cross(p_b - p_c);
	const real_t d3 = (p_point - p_a).cross(p_c - p_a);
	const bool has_negative = d1 < 0 || d2 < 0 || d3 < 0;
	const bool has_positive = d1 > 0 || d2 > 0 || d3 > 0;
	return !(has_negative && has_positive);
}

static EditorSpinSlider *make_slider(const String &p_label, double p_min, double p_max, double p_step, double p_value, const String &p_tooltip) {
	EditorSpinSlider *slider = memnew(EditorSpinSlider);
	slider->set_label(p_label);
	slider->set_min(p_min);
	slider->set_max(p_max);
	slider->set_step(p_step);
	slider->set_value(p_value);
	slider->set_tooltip_text(p_tooltip);
	slider->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	return slider;
}

static Label *make_header(const String &p_text) {
	Label *label = memnew(Label);
	label->set_text(p_text);
	label->set_theme_type_variation("HeaderSmall");
	return label;
}

// UV view.

MeshPaintUVEditor::MeshPaintUVEditor(MeshPaintEditorPlugin *p_plugin) {
	plugin = p_plugin;
	set_focus_mode(FOCUS_CLICK);
	set_clip_contents(true);
	set_custom_minimum_size(Size2(200, 200) * EDSCALE);
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);
}

Transform2D MeshPaintUVEditor::get_uv_to_screen() const {
	const real_t side = MAX(MIN(get_size().x, get_size().y) - 24 * EDSCALE, 32.0) * zoom;
	const Vector2 origin = get_size() * 0.5 + pan - Vector2(side, side) * 0.5;
	return Transform2D(Vector2(side, 0), Vector2(0, side), origin);
}

Vector2 MeshPaintUVEditor::uv_to_screen(const Vector2 &p_uv) const {
	return get_uv_to_screen().xform(p_uv);
}

Vector2 MeshPaintUVEditor::screen_to_uv(const Vector2 &p_screen) const {
	return get_uv_to_screen().affine_inverse().xform(p_screen);
}

void MeshPaintUVEditor::fit_view() {
	zoom = 1.0;
	pan = Vector2();
	queue_redraw();
}

int MeshPaintUVEditor::pick_face(const Vector2 &p_uv) const {
	const Ref<MeshPainter> &painter = plugin->get_painter();
	if (painter.is_null() || !painter->is_loaded()) {
		return -1;
	}
	// Selected faces win, so a click on a selected island over another one still drags it.
	int found = -1;
	for (int f = 0; f < painter->get_face_count(); f++) {
		if (point_in_triangle(p_uv, painter->get_face_uv(f, 0), painter->get_face_uv(f, 1), painter->get_face_uv(f, 2))) {
			if (painter->is_face_selected(f)) {
				return f;
			}
			if (found < 0) {
				found = f;
			}
		}
	}
	return found;
}

Vector2 MeshPaintUVEditor::get_selection_center() const {
	const Ref<MeshPainter> &painter = plugin->get_painter();
	Rect2 rect;
	bool first = true;
	for (const int f : painter->get_selected_faces()) {
		for (int c = 0; c < 3; c++) {
			const Vector2 uv = painter->get_face_uv(f, c);
			if (first) {
				rect = Rect2(uv, Vector2());
				first = false;
			} else {
				rect.expand_to(uv);
			}
		}
	}
	return rect.get_center();
}

void MeshPaintUVEditor::_begin_transform(Drag p_mode, const Vector2 &p_mouse) {
	const Ref<MeshPainter> &painter = plugin->get_painter();
	if (painter.is_null() || painter->get_selected_face_count() == 0) {
		return;
	}
	drag = p_mode;
	drag_start = p_mouse;
	drag_last = p_mouse;
	pivot = get_selection_center();
	applied = Transform2D();
	plugin->uv_transform_preview(); // Remembers the mesh to go back to.
	queue_redraw();
}

void MeshPaintUVEditor::_transform_to(const Transform2D &p_transform) {
	const Ref<MeshPainter> &painter = plugin->get_painter();
	const Transform2D delta = p_transform * applied.affine_inverse();
	painter->transform_faces_uv(painter->get_selected_faces(), delta);
	applied = p_transform;
	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	if (now - last_live_update > 60) {
		last_live_update = now;
		plugin->uv_transform_preview();
	}
	queue_redraw();
}

void MeshPaintUVEditor::_finish_transform(bool p_cancel) {
	const Drag was = drag;
	drag = DRAG_NONE;
	drag_from_click = false;
	if (p_cancel) {
		_transform_to(Transform2D());
		plugin->uv_transform_committed(Ref<Mesh>(), String());
	} else {
		plugin->uv_transform_committed(Ref<Mesh>(), was == DRAG_ROTATE ? TTR("Rotate UV Islands") : (was == DRAG_SCALE ? TTR("Scale UV Islands") : TTR("Move UV Islands")));
	}
	queue_redraw();
}

void MeshPaintUVEditor::_paint_at(const Vector2 &p_screen) {
	const Ref<Image> image = plugin->get_uv_paint_image(true);
	if (image.is_null()) {
		return;
	}
	const real_t side = get_uv_to_screen().get_scale().x;
	const Rect2i painted = plugin->get_painter()->paint_dab_uv(image, plugin->get_brush(), screen_to_uv(p_screen), plugin->get_radius_px() / MAX(side, (real_t)1.0));
	if (painted.size.x > 0) {
		plugin->uv_stroke_painted(image);
	}
}

void MeshPaintUVEditor::gui_input(const Ref<InputEvent> &p_event) {
	const Ref<MeshPainter> &painter = plugin->get_painter();
	if (painter.is_null() || !painter->is_loaded()) {
		return;
	}

	const Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid()) {
		const Vector2 mouse = mb->get_position();
		if (mb->is_pressed() && (mb->get_button_index() == MouseButton::WHEEL_UP || mb->get_button_index() == MouseButton::WHEEL_DOWN)) {
			const Vector2 anchor = screen_to_uv(mouse);
			zoom = CLAMP(zoom * (mb->get_button_index() == MouseButton::WHEEL_UP ? 1.15 : 1.0 / 1.15), 0.1, 64.0);
			pan += mouse - uv_to_screen(anchor);
			queue_redraw();
			accept_event();
			return;
		}
		if (mb->get_button_index() == MouseButton::MIDDLE) {
			drag = mb->is_pressed() ? DRAG_PAN : (drag == DRAG_PAN ? DRAG_NONE : drag);
			drag_last = mouse;
			accept_event();
			return;
		}
		if (is_transforming()) {
			if (mb->is_pressed() && mb->get_button_index() == MouseButton::RIGHT) {
				_finish_transform(true);
			} else if (mb->is_pressed() && mb->get_button_index() == MouseButton::LEFT && !drag_from_click) {
				_finish_transform(false);
			} else if (!mb->is_pressed() && mb->get_button_index() == MouseButton::LEFT && drag_from_click) {
				if (mouse.distance_to(drag_start) < 4 * EDSCALE) {
					_finish_transform(true); // It was a click.
				} else {
					_finish_transform(false);
				}
			}
			accept_event();
			return;
		}
		if (mb->get_button_index() != MouseButton::LEFT) {
			return;
		}
		grab_focus();
		if (mb->is_pressed()) {
			if (plugin->get_tool() == MeshPaintEditorPlugin::TOOL_PAINT) {
				if (plugin->get_uv_paint_image(true).is_valid()) {
					painter->begin_stroke();
					drag = DRAG_PAINT;
					drag_last = mouse;
					_paint_at(mouse);
				}
				accept_event();
				return;
			}
			const int face = pick_face(screen_to_uv(mouse));
			if (face >= 0) {
				if (!painter->is_face_selected(face) || mb->is_shift_pressed() || mb->is_command_or_control_pressed()) {
					const PackedInt32Array island = painter->get_island_faces(painter->get_face_island(face));
					if (!mb->is_shift_pressed() && !mb->is_command_or_control_pressed()) {
						painter->select_all(false);
					}
					for (const int f : island) {
						painter->set_face_selected(f, !mb->is_command_or_control_pressed());
					}
					plugin->uv_selection_changed();
				}
				if (!mb->is_command_or_control_pressed() && painter->get_selected_face_count() > 0) {
					// Click and drag moves what is selected.
					_begin_transform(DRAG_MOVE, mouse);
					drag_from_click = true;
				}
			} else {
				drag = DRAG_BOX;
				drag_start = mouse;
				drag_last = mouse;
				box_add = mb->is_shift_pressed();
				box_remove = mb->is_command_or_control_pressed();
			}
			accept_event();
		} else {
			if (drag == DRAG_PAINT) {
				drag = DRAG_NONE;
				plugin->uv_stroke_finished();
			} else if (drag == DRAG_BOX) {
				drag = DRAG_NONE;
				const Rect2 box = Rect2(drag_start, Vector2()).expand(mouse);
				if (!box_add && !box_remove) {
					painter->select_all(false);
				}
				PackedInt32Array hit;
				for (int f = 0; f < painter->get_face_count(); f++) {
					const Vector2 center = (painter->get_face_uv(f, 0) + painter->get_face_uv(f, 1) + painter->get_face_uv(f, 2)) / 3.0;
					if (box.has_point(uv_to_screen(center))) {
						hit.push_back(f);
					}
				}
				// Whole islands, unless Alt picks single faces.
				const PackedInt32Array faces = mb->is_alt_pressed() ? hit : PackedInt32Array();
				if (!mb->is_alt_pressed()) {
					for (const int island : painter->get_islands_of_faces(hit)) {
						for (const int f : painter->get_island_faces(island)) {
							painter->set_face_selected(f, !box_remove);
						}
					}
				}
				for (const int f : faces) {
					painter->set_face_selected(f, !box_remove);
				}
				plugin->uv_selection_changed();
				queue_redraw();
			}
			accept_event();
		}
		return;
	}

	const Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		const Vector2 mouse = mm->get_position();
		switch (drag) {
			case DRAG_PAN: {
				pan += mouse - drag_last;
				drag_last = mouse;
			} break;
			case DRAG_BOX: {
				drag_last = mouse;
			} break;
			case DRAG_PAINT: {
				const real_t spacing = MAX(plugin->get_radius_px() * 0.25, (real_t)1.0);
				Vector2 from = drag_last;
				const real_t distance = from.distance_to(mouse);
				if (distance >= spacing) {
					const int steps = MIN((int)(distance / spacing), 256);
					for (int i = 1; i <= steps; i++) {
						_paint_at(from.lerp(mouse, real_t(i) / steps));
					}
					drag_last = mouse;
				}
			} break;
			case DRAG_MOVE: {
				Vector2 delta = screen_to_uv(mouse) - screen_to_uv(drag_start);
				if (mm->is_command_or_control_pressed()) {
					delta = delta.snappedf(1.0 / 64.0);
				}
				_transform_to(Transform2D(0, delta));
			} break;
			case DRAG_ROTATE: {
				const Vector2 center = uv_to_screen(pivot);
				real_t angle = (mouse - center).angle() - (drag_start - center).angle();
				if (mm->is_command_or_control_pressed()) {
					angle = Math::snapped(angle, Math::deg_to_rad(15.0));
				}
				_transform_to(Transform2D(0, pivot) * Transform2D(angle, Vector2()) * Transform2D(0, -pivot));
			} break;
			case DRAG_SCALE: {
				const Vector2 center = uv_to_screen(pivot);
				real_t scale = (mouse - center).length() / MAX((drag_start - center).length(), (real_t)1.0);
				if (mm->is_command_or_control_pressed()) {
					scale = MAX(Math::snapped(scale, 0.1), 0.1);
				}
				_transform_to(Transform2D(0, pivot) * Transform2D(0, Size2(scale, scale), 0, Vector2()) * Transform2D(0, -pivot));
			} break;
			default:
				break;
		}
		drag_last = drag == DRAG_PAINT ? drag_last : mouse;
		queue_redraw();
		return;
	}

	const Ref<InputEventKey> k = p_event;
	if (k.is_valid() && k->is_pressed() && !k->is_echo()) {
		const Vector2 mouse = get_local_mouse_position();
		switch (k->get_keycode()) {
			case Key::G:
			case Key::R:
			case Key::S: {
				if (is_transforming() || k->is_command_or_control_pressed()) {
					return;
				}
				_begin_transform(k->get_keycode() == Key::G ? DRAG_MOVE : (k->get_keycode() == Key::R ? DRAG_ROTATE : DRAG_SCALE), mouse);
				accept_event();
			} break;
			case Key::ESCAPE: {
				if (is_transforming()) {
					_finish_transform(true);
					accept_event();
				}
			} break;
			case Key::ENTER:
			case Key::KP_ENTER: {
				if (is_transforming()) {
					_finish_transform(false);
					accept_event();
				}
			} break;
			case Key::F:
			case Key::HOME: {
				fit_view();
				accept_event();
			} break;
			case Key::A: {
				if (k->is_alt_pressed()) {
					painter->select_all(false);
				} else {
					painter->select_all(painter->get_selected_face_count() < painter->get_face_count());
				}
				plugin->uv_selection_changed();
				accept_event();
			} break;
			default:
				break;
		}
	}
}

void MeshPaintUVEditor::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_MOUSE_EXIT: {
			queue_redraw();
		} break;
		case NOTIFICATION_DRAW: {
			const Color background = get_theme_color(SNAME("dark_color_2"), EditorStringName(Editor));
			draw_rect(Rect2(Vector2(), get_size()), background);
			const Ref<MeshPainter> &painter = plugin->get_painter();
			const Transform2D xf = get_uv_to_screen();
			const Rect2 square(xf.get_origin(), xf.get_scale());
			const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
			const int font_size = get_theme_font_size(SceneStringName(font_size), SNAME("Label"));

			// Checkers show where the texture is transparent.
			const int cells = 16;
			const Vector2 cell = square.size / cells;
			for (int y = 0; y < cells; y++) {
				for (int x = 0; x < cells; x++) {
					draw_rect(Rect2(square.position + cell * Vector2(x, y), cell), (x + y) % 2 ? Color(0.32, 0.32, 0.32) : Color(0.24, 0.24, 0.24));
				}
			}
			const Ref<Texture2D> texture = plugin->get_uv_background();
			if (texture.is_valid()) {
				draw_texture_rect(texture, square, false);
			}
			draw_rect(square, Color(1, 1, 1, 0.35), false);

			if (painter.is_null() || !painter->is_loaded()) {
				draw_string(font, Vector2(12, 24) * EDSCALE, TTR("Select a MeshInstance3D and turn on Paint."), HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(1, 1, 1, 0.7));
				return;
			}

			const int face_count = painter->get_face_count();
			const int active = plugin->get_active_slot();
			const PackedVector2Array uvs = painter->get_face_uvs();
			PackedVector2Array lines_other;
			PackedVector2Array lines_active;
			PackedVector2Array selected_points;
			PackedColorArray selected_colors;
			const Color accent = get_theme_color(SNAME("accent_color"), EditorStringName(Editor));
			const Color fill(accent.r, accent.g, accent.b, 0.35);
			lines_active.reserve(face_count * 6);
			for (int f = 0; f < face_count; f++) {
				const Vector2 a = xf.xform(uvs[f * 3 + 0]);
				const Vector2 b = xf.xform(uvs[f * 3 + 1]);
				const Vector2 c = xf.xform(uvs[f * 3 + 2]);
				PackedVector2Array &lines = painter->get_face_slot(f) == active ? lines_active : lines_other;
				lines.push_back(a);
				lines.push_back(b);
				lines.push_back(b);
				lines.push_back(c);
				lines.push_back(c);
				lines.push_back(a);
				if (painter->is_face_selected(f)) {
					selected_points.push_back(a);
					selected_points.push_back(b);
					selected_points.push_back(c);
					selected_colors.push_back(fill);
					selected_colors.push_back(fill);
					selected_colors.push_back(fill);
				}
			}
			if (!selected_points.is_empty()) {
				Vector<int> indices;
				indices.resize(selected_points.size());
				for (int i = 0; i < indices.size(); i++) {
					indices.write[i] = i;
				}
				RenderingServer::get_singleton()->canvas_item_add_triangle_array(get_canvas_item(), indices, selected_points, selected_colors);
			}
			if (!lines_other.is_empty()) {
				draw_multiline(lines_other, Color(1, 1, 1, 0.18));
			}
			if (!lines_active.is_empty()) {
				draw_multiline(lines_active, Color(1, 1, 1, 0.65));
			}
			if (!painter->has_uvs()) {
				draw_string(font, Vector2(12, 24) * EDSCALE, TTR("This mesh has no UVs yet: use UV > Smart Unwrap."), HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(1, 0.8, 0.4));
			}

			if (drag == DRAG_BOX) {
				const Rect2 box = Rect2(drag_start, Vector2()).expand(drag_last);
				draw_rect(box, Color(accent.r, accent.g, accent.b, 0.15));
				draw_rect(box, accent, false);
			}
			if (is_transforming()) {
				const String hint = drag_from_click ? TTR("Release to place. Ctrl: snap.") : TTR("Click or Enter to confirm, right click or Esc to cancel. Ctrl: snap.");
				draw_string(font, Vector2(12 * EDSCALE, get_size().y - 12 * EDSCALE), hint, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(1, 1, 1, 0.8));
				if (drag != DRAG_MOVE) {
					draw_line(uv_to_screen(pivot), get_local_mouse_position(), Color(1, 1, 1, 0.5), 1.0, true);
				}
			}
			if (plugin->get_tool() == MeshPaintEditorPlugin::TOOL_PAINT && get_global_rect().has_point(get_global_mouse_position())) {
				draw_arc(get_local_mouse_position(), plugin->get_radius_px(), 0, Math::TAU, 48, Color(1, 1, 1, 0.8), 1.0, true);
			}
		} break;
	}
}

// Plugin: painter and node.

bool MeshPaintEditorPlugin::_can_paint_node(MeshInstance3D *p_node) const {
	if (!p_node || p_node->get_mesh().is_null()) {
		return false;
	}
	// These rebuild their mesh from a source mesh, so painting it would be thrown away.
	if (Object::cast_to<QuickMeshBrush3D>(p_node) || p_node->is_class("EditableMeshInstance3D")) {
		return false;
	}
	return true;
}

Ref<Mesh> MeshPaintEditorPlugin::_make_pose_mesh() const {
	if (!node) {
		return Ref<Mesh>();
	}
	const Ref<ArrayMesh> mesh = node->get_mesh();
	if (mesh.is_null() || node->get_skin_reference().is_null() || mesh->get_surface_count() == 0) {
		return Ref<Mesh>();
	}
	for (int s = 0; s < mesh->get_surface_count(); s++) {
		const uint64_t format = mesh->surface_get_format(s);
		if (!(format & Mesh::ARRAY_FORMAT_BONES) || !(format & Mesh::ARRAY_FORMAT_WEIGHTS) || mesh->surface_get_primitive_type(s) != Mesh::PRIMITIVE_TRIANGLES) {
			return Ref<Mesh>();
		}
	}
	const RID skeleton = node->get_skin_reference()->get_skeleton();
	if (!skeleton.is_valid() || RenderingServer::get_singleton()->skeleton_get_bone_count(skeleton) <= 0) {
		return Ref<Mesh>();
	}
	// The brush follows the pose the model shows, not its rest pose.
	return node->bake_mesh_from_current_skeleton_pose();
}

void MeshPaintEditorPlugin::_sync_painter() {
	if (!node || !painting) {
		return;
	}
	const Ref<Mesh> mesh = node->get_mesh();
	if (mesh.is_null()) {
		return;
	}
	if (painter->is_loaded() && painter_node == node->get_instance_id() && (mesh == painter->get_committed_mesh() || mesh == painter->get_loaded_mesh())) {
		return;
	}
	const bool same_node = painter_node == node->get_instance_id();
	const PackedInt32Array selection = painter->get_selected_faces();
	const int face_count = painter->get_face_count();

	TypedArray<Material> overrides;
	for (int i = 0; i < node->get_surface_override_material_count(); i++) {
		overrides.push_back(node->get_surface_override_material(i));
	}
	painter->load_mesh(mesh, _make_pose_mesh(), overrides);
	painter_node = node->get_instance_id();
	if (same_node) {
		_restore_selection_after_reload(selection, face_count);
	}
	active_slot = CLAMP(active_slot, 0, MAX(painter->get_slot_count() - 1, 0));
	selection_dirty = true;
	_update_slot_list();
	_update_info();
	uv_editor->queue_redraw();
	if (!painter->has_uvs()) {
		_set_status(TTR("This mesh has no UVs, so there is nothing to paint on yet. Use UV > Smart Unwrap first."));
	}
}

void MeshPaintEditorPlugin::_restore_selection_after_reload(const PackedInt32Array &p_selected, int p_face_count) {
	if (painter->get_face_count() == p_face_count) {
		painter->set_selected_faces(p_selected);
	}
}

void MeshPaintEditorPlugin::_commit(const String &p_action) {
	ERR_FAIL_NULL(node);
	const Ref<Mesh> before = node->get_mesh();
	const Ref<ArrayMesh> after = painter->commit_mesh();
	ERR_FAIL_COND(after.is_null());

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(p_action, UndoRedo::MERGE_DISABLE, node);
	ur->add_do_property(node, "mesh", after);
	// Surface overrides were folded into the slots when the mesh was loaded.
	for (int i = 0; i < after->get_surface_count(); i++) {
		ur->add_do_method(node, "set_surface_override_material", i, Ref<Material>());
	}
	ur->add_do_method(this, "_refresh");
	ur->add_undo_property(node, "mesh", before);
	for (int i = 0; i < node->get_surface_override_material_count(); i++) {
		ur->add_undo_method(node, "set_surface_override_material", i, node->get_surface_override_material(i));
	}
	ur->add_undo_method(this, "_refresh");
	ur->commit_action();
}

void MeshPaintEditorPlugin::_refresh() {
	_sync_painter();
	selection_dirty = true;
	_update_slot_list();
	_update_info();
	uv_editor->queue_redraw();
}

// Paint layers.

int MeshPaintEditorPlugin::_find_layer(const Ref<Material> &p_material, MeshPainter::Channel p_channel) const {
	for (uint32_t i = 0; i < layers.size(); i++) {
		if (layers[i].material == p_material && layers[i].channel == p_channel) {
			return i;
		}
	}
	return -1;
}

String MeshPaintEditorPlugin::_default_folder() const {
	const Node *scene = EditorNode::get_singleton()->get_edited_scene();
	const String scene_path = scene ? scene->get_scene_file_path() : String();
	if (scene_path.is_empty()) {
		return "res://mesh_paint";
	}
	return scene_path.get_base_dir().path_join("mesh_paint");
}

String MeshPaintEditorPlugin::_make_layer_path(const Ref<BaseMaterial3D> &p_material, int p_slot, MeshPainter::Channel p_channel) const {
	String folder = folder_edit->get_text().strip_edges();
	if (folder.is_empty() || !folder.begins_with("res://")) {
		folder = _default_folder();
	}
	const Node *scene = EditorNode::get_singleton()->get_edited_scene();
	String base = scene && !scene->get_scene_file_path().is_empty() ? scene->get_scene_file_path().get_file().get_basename() + "_" : String();
	base += String(node->get_name()) + "_" + painter->get_slot_name(p_slot) + "_" + MeshPainter::get_channel_name(p_channel);
	base = base.validate_filename().replace(" ", "_").to_lower();
	// Never overwrite a file that is there already, or one another layer will write.
	auto taken = [&](const String &p_path) {
		if (FileAccess::exists(p_path)) {
			return true;
		}
		for (const PaintLayer &layer : layers) {
			if (layer.path == p_path) {
				return true;
			}
		}
		return false;
	};
	String path = folder.path_join(base + ".png");
	for (int n = 2; taken(path); n++) {
		path = folder.path_join(base + "_" + itos(n) + ".png");
	}
	return path;
}

Ref<BaseMaterial3D> MeshPaintEditorPlugin::_make_slot_material_local(int p_slot) {
	const Ref<Material> material = painter->get_slot_material(p_slot);
	if (material.is_valid() && Object::cast_to<BaseMaterial3D>(material.ptr()) == nullptr) {
		return Ref<BaseMaterial3D>();
	}
	const Node *scene = EditorNode::get_singleton()->get_edited_scene();
	const String scene_path = scene ? scene->get_scene_file_path() : String();
	const String path = material.is_valid() ? material->get_path() : String();
	if (material.is_valid() && (path.is_empty() || (!scene_path.is_empty() && path.begins_with(scene_path + "::")))) {
		return material;
	}
	// A material from a file (or from an imported model) is shared and would not keep the
	// paint: this mesh gets its own copy.
	Ref<BaseMaterial3D> copy;
	if (material.is_valid()) {
		copy = material->duplicate();
		copy_note = vformat(TTR("Painting uses a copy of \"%s\" kept in this mesh, so the original file is not changed."), path.get_slice("::", 0).get_file());
		_set_status(copy_note);
	} else {
		Ref<StandardMaterial3D> fresh;
		fresh.instantiate();
		copy = fresh;
	}
	painter->set_slot_material(p_slot, copy);
	_commit(TTR("Mesh Paint: Own Material for Painting"));
	return copy;
}

int MeshPaintEditorPlugin::_get_layer(int p_slot, MeshPainter::Channel p_channel, bool p_create) {
	if (painter.is_null() || p_slot < 0 || p_slot >= painter->get_slot_count()) {
		return -1;
	}
	Ref<BaseMaterial3D> material = painter->get_slot_material(p_slot);
	if (material.is_valid()) {
		const int index = _find_layer(material, p_channel);
		if (index >= 0) {
			const Ref<Texture2D> shown = MeshPainter::get_channel_texture(material, p_channel);
			PaintLayer &layer = layers[index];
			if (shown == layer.live || (shown.is_valid() && shown == layer.file_texture)) {
				return index;
			}
			// The texture was replaced (in the inspector, say): start again from what is there now.
			layers.remove_at(index);
		}
	}
	if (!p_create) {
		return -1;
	}
	if (painter->get_slot_material(p_slot).is_valid() && material.is_null()) {
		_set_status(TTR("Only StandardMaterial3D and ORMMaterial3D slots can be painted. Give this slot a StandardMaterial3D to paint it."));
		return -1;
	}
	if (!painter->has_uvs()) {
		_set_status(TTR("This mesh has no UVs yet: use UV > Smart Unwrap first."));
		return -1;
	}
	material = _make_slot_material_local(p_slot);
	if (material.is_null()) {
		return -1;
	}

	PaintLayer layer;
	layer.material = material;
	layer.channel = p_channel;
	const Ref<Texture2D> existing = MeshPainter::get_channel_texture(material, p_channel);
	const Dictionary files = material->get_meta(PAINT_FILES_META, Dictionary());
	const String owned = files.get(MeshPainter::get_channel_name(p_channel), String());
	if (existing.is_valid() && !owned.is_empty() && existing->get_path() == owned) {
		// Painted before: keep painting into the same file.
		layer.path = owned;
		layer.file_texture = existing;
	}
	layer.image = MeshPainter::make_channel_image(material, p_channel, Vector2i(resolution, resolution));
	ERR_FAIL_COND_V(layer.image.is_null(), -1);
	if (layer.path.is_empty()) {
		layer.path = _make_layer_path(material, p_slot, p_channel);
	}
	layer.live = ImageTexture::create_from_image(layer.image);
	layer.dirty = layer.file_texture.is_null();
	MeshPainter::set_channel_texture(material, p_channel, layer.live);
	layers.push_back(layer);
	if (layer.file_texture.is_null()) {
		const String note = vformat(TTR("New %s texture, %dx%d. It is saved with the scene as %s."), MeshPainter::get_channel_name(p_channel), layer.image->get_width(), layer.image->get_height(), layer.path);
		_set_status(copy_note.is_empty() ? note : copy_note + "\n" + note);
	}
	copy_note = String();
	return layers.size() - 1;
}

void MeshPaintEditorPlugin::_show_live(PaintLayer &p_layer) {
	if (MeshPainter::get_channel_texture(p_layer.material, p_layer.channel) != p_layer.live) {
		MeshPainter::set_channel_texture(p_layer.material, p_layer.channel, p_layer.live);
	}
}

void MeshPaintEditorPlugin::_flush_textures() {
	for (PaintLayer &layer : layers) {
		if (layer.upload) {
			layer.upload = false;
			layer.live->update(layer.image);
		}
	}
	textures_dirty = false;
}

Error MeshPaintEditorPlugin::_save_layers() {
	Vector<int> saved;
	for (uint32_t i = 0; i < layers.size(); i++) {
		PaintLayer &layer = layers[i];
		const bool showing_live = MeshPainter::get_channel_texture(layer.material, layer.channel) == layer.live;
		if (!showing_live) {
			continue;
		}
		if (layer.dirty || layer.file_texture.is_null()) {
			EditorFileSystem *efs = EditorFileSystem::get_singleton();
			const String dir = layer.path.get_base_dir();
			if (!efs->get_filesystem_path(dir)) {
				// Made through the FileSystem, so the new folder is known at once.
				const Error dir_err = efs->make_dir_recursive(dir);
				if (dir_err != OK && dir_err != ERR_ALREADY_EXISTS) {
					_set_status(vformat(TTR("Could not make the folder %s."), dir));
					continue;
				}
			}
			const Error err = layer.image->save_png(layer.path);
			if (err != OK) {
				_set_status(vformat(TTR("Could not save %s."), layer.path));
				continue;
			}
			if (!efs->get_filesystem_path(dir)) {
				// Made outside the editor while it ran: it shows up after the next scan.
				efs->scan_changes();
				_set_status(vformat(TTR("Saved %s; it is linked to the material on the next save, once the FileSystem has seen the folder."), layer.path));
				continue;
			}
			efs->update_file(layer.path);
			ResourceLoader::import(layer.path);
		}
		saved.push_back(i);
	}
	for (const int i : saved) {
		PaintLayer &layer = layers[i];
		const Ref<Texture2D> texture = ResourceLoader::load(layer.path, "Texture2D", ResourceFormatLoader::CACHE_MODE_REPLACE);
		if (texture.is_null()) {
			_set_status(vformat(TTR("Saved %s, but it could not be loaded back; the material keeps the painted copy."), layer.path));
			continue;
		}
		// The material uses the file from now on; painting again swaps the live copy in.
		MeshPainter::set_channel_texture(layer.material, layer.channel, texture);
		layer.file_texture = texture;
		layer.dirty = false;
		Dictionary files = layer.material->get_meta(PAINT_FILES_META, Dictionary());
		files[MeshPainter::get_channel_name(layer.channel)] = layer.path;
		layer.material->set_meta(PAINT_FILES_META, files);
	}
	if (!saved.is_empty()) {
		uv_editor->queue_redraw();
	}
	return OK;
}

void MeshPaintEditorPlugin::_set_image_region(const Ref<Image> &p_image, const Ref<Image> &p_region, const Vector2i &p_position) {
	ERR_FAIL_COND(p_image.is_null() || p_region.is_null());
	p_image->blit_rect(p_region, Rect2i(Point2i(), p_region->get_size()), p_position);
	for (PaintLayer &layer : layers) {
		if (layer.image == p_image) {
			_show_live(layer);
			layer.dirty = true;
			layer.upload = true;
			textures_dirty = true;
		}
	}
	uv_editor->queue_redraw();
}

void MeshPaintEditorPlugin::_record_stroke(const String &p_action) {
	const TypedArray<Image> images = painter->get_stroke_images();
	bool any = false;
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	for (int i = 0; i < images.size(); i++) {
		const Ref<Image> image = images[i];
		const Rect2i rect = painter->get_stroke_rect(image);
		if (rect.size.x <= 0 || rect.size.y <= 0) {
			continue;
		}
		if (!any) {
			ur->create_action(p_action, UndoRedo::MERGE_DISABLE, node);
			any = true;
		}
		const Ref<Image> before = painter->get_stroke_original(image)->get_region(rect);
		const Ref<Image> after = image->get_region(rect);
		ur->add_do_method(this, "_set_image_region", image, after, rect.position);
		ur->add_undo_method(this, "_set_image_region", image, before, rect.position);
	}
	if (any) {
		ur->commit_action(false);
	}
	painter->end_stroke();
}

// Tools.

bool MeshPaintEditorPlugin::_pick(Camera3D *p_camera, const Vector2 &p_point, Hit &r_hit) const {
	if (!node || !painter->is_loaded()) {
		return false;
	}
	const Transform3D global = node->get_global_transform();
	const Transform3D inverse = global.affine_inverse();
	const Vector3 from = inverse.xform(p_camera->project_ray_origin(p_point));
	const Vector3 dir = inverse.basis.xform(p_camera->project_ray_normal(p_point)).normalized();
	if (!painter->intersect_ray_face(from, dir, r_hit.face, r_hit.position, r_hit.normal)) {
		return false;
	}
	// The radius is set in screen pixels; turn it into mesh units at the hit's depth.
	const Vector3 world = global.xform(r_hit.position);
	const real_t depth = -p_camera->get_camera_transform().affine_inverse().xform(world).z;
	const Vector3 a = p_camera->project_position(p_point, depth);
	const Vector3 b = p_camera->project_position(p_point + Vector2(radius_px, 0), depth);
	const Vector3 scale = global.basis.get_scale().abs();
	r_hit.radius = a.distance_to(b) / MAX((scale.x + scale.y + scale.z) / 3.0, (real_t)0.000001);
	r_hit.eye = from;
	r_hit.up = inverse.basis.xform(p_camera->get_camera_transform().basis.get_column(1)).normalized();
	return true;
}

void MeshPaintEditorPlugin::_begin_stroke() {
	const Ref<Mesh> pose = _make_pose_mesh();
	if (pose.is_valid()) {
		painter->set_pose_mesh(pose);
	}
	painter->begin_stroke();
	stroking = true;
	stroke_layers.clear();
}

void MeshPaintEditorPlugin::_paint_at(Camera3D *p_camera, const Vector2 &p_point) {
	Hit hit;
	if (!_pick(p_camera, p_point, hit)) {
		return;
	}
	// Every slot under the brush gets painted in its own texture, unless one is locked.
	HashSet<int> slots;
	if (only_active_slot) {
		slots.insert(active_slot);
	} else {
		const real_t reach = brush->get_texture().is_valid() ? hit.radius * Math::SQRT2 : hit.radius;
		for (const int f : painter->get_faces_in_sphere(hit.position, reach)) {
			slots.insert(painter->get_face_slot(f));
		}
	}
	for (const int slot : slots) {
		const int index = _get_layer(slot, channel, true);
		if (index < 0) {
			continue;
		}
		PaintLayer &layer = layers[index];
		_show_live(layer);
		const Rect2i painted = painter->paint_dab(layer.image, brush, hit.position, hit.radius, hit.eye, hit.up, slot);
		if (painted.size.x > 0) {
			layer.dirty = true;
			layer.upload = true;
			textures_dirty = true;
		}
	}
}

void MeshPaintEditorPlugin::_end_stroke() {
	stroking = false;
	if (tool == TOOL_PAINT || tool == TOOL_FILL) {
		_record_stroke(tool == TOOL_FILL ? TTR("Mesh Paint: Fill") : TTR("Mesh Paint: Stroke"));
	}
	uv_editor->queue_redraw();
}

void MeshPaintEditorPlugin::_fill_at(int p_face) {
	// The selection when the face is in it, else the face's island.
	const PackedInt32Array faces = painter->is_face_selected(p_face) ? painter->get_selected_faces() : painter->get_island_faces(painter->get_face_island(p_face));
	HashMap<int, PackedInt32Array> by_slot;
	for (const int f : faces) {
		const int slot = painter->get_face_slot(f);
		if (!only_active_slot || slot == active_slot) {
			by_slot[slot].push_back(f);
		}
	}
	painter->begin_stroke();
	for (const KeyValue<int, PackedInt32Array> &E : by_slot) {
		const int index = _get_layer(E.key, channel, true);
		if (index < 0) {
			continue;
		}
		PaintLayer &layer = layers[index];
		_show_live(layer);
		(void)painter->fill_faces(layer.image, E.value, brush);
		layer.dirty = true;
		layer.upload = true;
		textures_dirty = true;
	}
	_record_stroke(TTR("Mesh Paint: Fill"));
	uv_editor->queue_redraw();
}

void MeshPaintEditorPlugin::_pick_color_at(const Hit &p_hit) {
	const int slot = painter->get_face_slot(p_hit.face);
	const int index = _get_layer(slot, channel, false);
	Color color;
	if (index >= 0) {
		color = painter->sample_image(layers[index].image, p_hit.face, painter->get_barycentric(p_hit.face, p_hit.position));
	} else {
		const Ref<BaseMaterial3D> material = painter->get_slot_material(slot);
		if (material.is_null()) {
			return;
		}
		const Ref<Image> image = MeshPainter::make_channel_image(material, channel, Vector2i(64, 64));
		color = painter->sample_image(image, p_hit.face, painter->get_barycentric(p_hit.face, p_hit.position));
	}
	updating_ui = true;
	if (channel == MeshPainter::CHANNEL_ROUGHNESS || channel == MeshPainter::CHANNEL_METALLIC) {
		value_slider->set_value(color.r);
	} else {
		color.a = 1.0;
		color_picker->set_pick_color(color);
	}
	updating_ui = false;
	_update_brush_color();
	_set_status(vformat(TTR("Picked %s."), channel == MeshPainter::CHANNEL_ROUGHNESS || channel == MeshPainter::CHANNEL_METALLIC ? String::num(color.r, 2) : "#" + color.to_html(false)));
}

void MeshPaintEditorPlugin::_select_at(Camera3D *p_camera, const Vector2 &p_point, bool p_subtract, bool p_click) {
	Hit hit;
	const bool has_hit = _pick(p_camera, p_point, hit);
	if (select_mode == SELECT_FACES) {
		if (!has_hit) {
			return;
		}
		for (const int f : painter->get_faces_in_sphere(hit.position, hit.radius, hit.eye, !brush->is_painting_backfaces())) {
			painter->set_face_selected(f, !p_subtract);
		}
	} else if (p_click) {
		if (!has_hit) {
			return;
		}
		const PackedInt32Array faces = select_mode == SELECT_ISLANDS ? painter->get_island_faces(painter->get_face_island(hit.face)) : painter->get_piece_faces(painter->get_face_piece(hit.face));
		for (const int f : faces) {
			painter->set_face_selected(f, !p_subtract);
		}
	}
	selection_dirty = true;
	uv_editor->queue_redraw();
	_update_info();
}

void MeshPaintEditorPlugin::_update_brush_color() {
	if (channel == MeshPainter::CHANNEL_ROUGHNESS || channel == MeshPainter::CHANNEL_METALLIC) {
		const float v = value_slider->get_value();
		brush->set_color(Color(v, v, v, 1));
	} else {
		brush->set_color(color_picker->get_pick_color());
	}
}

// Overlays.

void MeshPaintEditorPlugin::_create_overlays() {
	if (!node || !node->is_inside_tree() || node->get_world_3d().is_null()) {
		return;
	}
	_free_overlays();
	RenderingServer *rs = RenderingServer::get_singleton();
	const RID scenario = node->get_world_3d()->get_scenario();
	selection_mesh.instantiate();
	ring_mesh.instantiate();
	selection_instance = rs->instance_create2(selection_mesh->get_rid(), scenario);
	ring_instance = rs->instance_create2(ring_mesh->get_rid(), scenario);
	rs->instance_geometry_set_cast_shadows_setting(selection_instance, RSE::SHADOW_CASTING_SETTING_OFF);
	rs->instance_geometry_set_cast_shadows_setting(ring_instance, RSE::SHADOW_CASTING_SETTING_OFF);
	rs->instance_set_visible(ring_instance, false);
	selection_dirty = true;
}

void MeshPaintEditorPlugin::_free_overlays() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (selection_instance.is_valid()) {
		rs->free_rid(selection_instance);
		selection_instance = RID();
	}
	if (ring_instance.is_valid()) {
		rs->free_rid(ring_instance);
		ring_instance = RID();
	}
	selection_mesh.unref();
	ring_mesh.unref();
}

void MeshPaintEditorPlugin::_update_selection_overlay() {
	selection_dirty = false;
	if (selection_mesh.is_null()) {
		return;
	}
	selection_mesh->clear_surfaces();
	if (!painting || !node || !painter->is_loaded()) {
		return;
	}
	const real_t offset = painter->get_aabb().get_longest_axis_size() * 0.0008;
	const PackedVector3Array triangles = painter->get_faces_triangles(painter->get_selected_faces(), offset);
	if (triangles.is_empty()) {
		return;
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = triangles;
	selection_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	selection_mesh->surface_set_material(0, selection_material);
}

void MeshPaintEditorPlugin::_update_ring() {
	if (!ring_instance.is_valid()) {
		return;
	}
	const bool show = painting && hovering && node && (tool == TOOL_PAINT || (tool == TOOL_SELECT && select_mode == SELECT_FACES));
	RenderingServer::get_singleton()->instance_set_visible(ring_instance, show);
	if (!show) {
		return;
	}
	Vector3 x = hover_normal.cross(Vector3(0, 1, 0));
	if (x.length_squared() < 0.01) {
		x = hover_normal.cross(Vector3(1, 0, 0));
	}
	x.normalize();
	const Vector3 z = hover_normal.cross(x).normalized();
	const Vector3 center = hover_position + hover_normal * hover_radius * 0.02;
	PackedVector3Array points;
	const int segments = 48;
	for (int i = 0; i < segments; i++) {
		const real_t a0 = Math::TAU * i / segments;
		const real_t a1 = Math::TAU * (i + 1) / segments;
		points.push_back(center + (x * Math::cos(a0) + z * Math::sin(a0)) * hover_radius);
		points.push_back(center + (x * Math::cos(a1) + z * Math::sin(a1)) * hover_radius);
	}
	points.push_back(center);
	points.push_back(center + hover_normal * hover_radius * 0.35);
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = points;
	ring_mesh->clear_surfaces();
	ring_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_LINES, arrays);
	ring_mesh->surface_set_material(0, ring_material);
}

// UV view hooks.

Ref<Texture2D> MeshPaintEditorPlugin::get_uv_background() const {
	if (painter.is_null() || active_slot >= painter->get_slot_count()) {
		return Ref<Texture2D>();
	}
	const Ref<BaseMaterial3D> material = painter->get_slot_material(active_slot);
	if (material.is_null()) {
		return Ref<Texture2D>();
	}
	return MeshPainter::get_channel_texture(material, channel);
}

Ref<Image> MeshPaintEditorPlugin::get_uv_paint_image(bool p_create) {
	const int index = _get_layer(active_slot, channel, p_create);
	if (index < 0) {
		return Ref<Image>();
	}
	_show_live(layers[index]);
	return layers[index].image;
}

void MeshPaintEditorPlugin::uv_stroke_painted(const Ref<Image> &p_image) {
	for (PaintLayer &layer : layers) {
		if (layer.image == p_image) {
			layer.dirty = true;
			layer.upload = true;
			textures_dirty = true;
		}
	}
}

void MeshPaintEditorPlugin::uv_stroke_finished() {
	_record_stroke(TTR("Mesh Paint: Stroke in UV View"));
	uv_editor->queue_redraw();
}

void MeshPaintEditorPlugin::uv_selection_changed() {
	selection_dirty = true;
	_update_info();
	uv_editor->queue_redraw();
}

void MeshPaintEditorPlugin::uv_transform_preview() {
	if (!node) {
		return;
	}
	if (transform_before_mesh.is_null()) {
		transform_before_mesh = node->get_mesh();
		return;
	}
	// Live in the 3D view too, while the mesh is small enough to rebuild quickly.
	if (painter->get_face_count() <= 150000) {
		node->set_mesh(painter->commit_mesh(false));
	}
}

void MeshPaintEditorPlugin::uv_transform_committed(const Ref<Mesh> &p_before, const String &p_action) {
	const Ref<Mesh> before = transform_before_mesh;
	transform_before_mesh.unref();
	if (!node || before.is_null()) {
		return;
	}
	if (p_action.is_empty()) {
		// Cancelled: back to the mesh from before, and the painter with it.
		node->set_mesh(before);
		_sync_painter();
		selection_dirty = true;
		return;
	}
	const Ref<ArrayMesh> after = painter->commit_mesh();
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(p_action, UndoRedo::MERGE_DISABLE, node);
	ur->add_do_property(node, "mesh", after);
	ur->add_do_method(this, "_refresh");
	ur->add_undo_property(node, "mesh", before);
	ur->add_undo_method(this, "_refresh");
	ur->commit_action();
}

// UI callbacks.

void MeshPaintEditorPlugin::_set_status(const String &p_text) {
	if (status_label) {
		status_label->set_text(p_text);
	}
}

void MeshPaintEditorPlugin::_update_info() {
	if (!info_label) {
		return;
	}
	if (painter.is_null() || !painter->is_loaded()) {
		info_label->set_text(String());
		return;
	}
	info_label->set_text(vformat(TTR("%d faces, %d UV islands, %d selected"), painter->get_face_count(), painter->get_island_count(), painter->get_selected_face_count()));
}

void MeshPaintEditorPlugin::_update_slot_list() {
	if (!slot_list) {
		return;
	}
	updating_ui = true;
	slot_list->clear();
	if (painter.is_valid() && painter->is_loaded()) {
		for (int s = 0; s < painter->get_slot_count(); s++) {
			const Ref<Material> material = painter->get_slot_material(s);
			Color swatch(0.5, 0.5, 0.5);
			const Ref<BaseMaterial3D> base = material;
			if (base.is_valid()) {
				swatch = base->get_albedo();
			}
			Ref<Image> image = Image::create_empty(16, 16, false, Image::FORMAT_RGBA8);
			image->fill(Color(swatch.r, swatch.g, swatch.b, 1));
			const String kind = material.is_null() ? TTR("no material") : (material->get_path().is_empty() || material->get_path().contains("::") ? material->get_class() : material->get_path().get_file());
			slot_list->add_item(vformat("%s  (%d)  %s", painter->get_slot_name(s), painter->get_slot_face_count(s), kind), ImageTexture::create_from_image(image));
		}
		active_slot = CLAMP(active_slot, 0, MAX(painter->get_slot_count() - 1, 0));
		if (painter->get_slot_count() > 0) {
			slot_list->select(active_slot);
		}
	}
	const Ref<Material> material = painter.is_valid() && painter->is_loaded() && active_slot < painter->get_slot_count() ? painter->get_slot_material(active_slot) : Ref<Material>();
	material_picker->set_edited_resource(material);
	slot_name_edit->set_text(painter.is_valid() && painter->is_loaded() && active_slot < painter->get_slot_count() ? painter->get_slot_name(active_slot) : String());
	material_inspector->edit(material.ptr());
	updating_ui = false;
}

void MeshPaintEditorPlugin::_update_ui() {
	if (!dock) {
		return;
	}
	updating_ui = true;
	for (int i = 0; i < TOOL_MAX; i++) {
		tool_buttons[i]->set_pressed_no_signal(i == tool);
	}
	select_mode_option->set_visible(tool == TOOL_SELECT);
	const bool scalar = channel == MeshPainter::CHANNEL_ROUGHNESS || channel == MeshPainter::CHANNEL_METALLIC;
	color_picker->get_parent_control()->set_visible(!scalar);
	value_slider->set_visible(scalar);
	updating_ui = false;
}

void MeshPaintEditorPlugin::_update_theme() {
	if (!paint_button) {
		return;
	}
	paint_button->set_button_icon(paint_button->get_editor_theme_icon(SNAME("MeshPaint")));
	if (!dock) {
		return;
	}
	tool_buttons[TOOL_PAINT]->set_button_icon(dock->get_editor_theme_icon(SNAME("Paint")));
	tool_buttons[TOOL_FILL]->set_button_icon(dock->get_editor_theme_icon(SNAME("Bucket")));
	tool_buttons[TOOL_SELECT]->set_button_icon(dock->get_editor_theme_icon(SNAME("ToolSelect")));
	tool_buttons[TOOL_PICK]->set_button_icon(dock->get_editor_theme_icon(SNAME("ColorPick")));
	uv_menu->set_button_icon(dock->get_editor_theme_icon(SNAME("Uv")));
	save_button->set_button_icon(dock->get_editor_theme_icon(SNAME("Save")));
}

void MeshPaintEditorPlugin::_paint_toggled(bool p_on) {
	set_painting(p_on);
}

void MeshPaintEditorPlugin::_tool_selected(int p_tool) {
	tool = (Tool)p_tool;
	_update_ui();
	_update_ring();
	uv_editor->queue_redraw();
}

void MeshPaintEditorPlugin::_select_mode_selected(int p_mode) {
	select_mode = (SelectMode)p_mode;
	_update_ring();
}

void MeshPaintEditorPlugin::_channel_selected(int p_channel) {
	channel = (MeshPainter::Channel)p_channel;
	_update_brush_color();
	_update_ui();
	uv_editor->queue_redraw();
}

void MeshPaintEditorPlugin::_color_changed(const Color &p_color) {
	if (!updating_ui) {
		_update_brush_color();
	}
}

void MeshPaintEditorPlugin::_value_changed(double p_value) {
	if (!updating_ui) {
		_update_brush_color();
	}
}

void MeshPaintEditorPlugin::_radius_changed(double p_value) {
	radius_px = p_value;
	hover_radius = 0;
	uv_editor->queue_redraw();
}

void MeshPaintEditorPlugin::_strength_changed(double p_value) {
	brush->set_strength(p_value);
}

void MeshPaintEditorPlugin::_hardness_changed(double p_value) {
	brush->set_hardness(p_value);
}

void MeshPaintEditorPlugin::_blend_selected(int p_mode) {
	brush->set_blend_mode((MeshPaintBrush::BlendMode)p_mode);
}

void MeshPaintEditorPlugin::_brush_texture_changed(const Ref<Resource> &p_texture) {
	brush->set_texture(p_texture);
	if (brush->get_texture().is_valid() && brush->get_texture_image().is_null()) {
		_set_status(TTR("That texture's pixels could not be read, so the brush stays round."));
	}
}

void MeshPaintEditorPlugin::_angle_changed(double p_value) {
	brush->set_texture_angle(p_value);
}

void MeshPaintEditorPlugin::_brush_option_toggled(bool p_on, int p_which) {
	switch (p_which) {
		case 0:
			brush->set_occlusion(p_on);
			break;
		case 1:
			brush->set_paint_backfaces(p_on);
			break;
		case 2:
			only_active_slot = p_on;
			break;
	}
}

void MeshPaintEditorPlugin::_resolution_selected(int p_index) {
	resolution = resolution_option->get_item_id(p_index);
}

void MeshPaintEditorPlugin::_uv_option(int p_option) {
	if (!node || !painter->is_loaded()) {
		return;
	}
	const int padding = padding_slider->get_value();
	switch (p_option) {
		case UV_SMART_UNWRAP:
		case UV_UNWRAP_SELECTED: {
			const PackedInt32Array faces = p_option == UV_UNWRAP_SELECTED ? painter->get_selected_faces() : PackedInt32Array();
			if (p_option == UV_UNWRAP_SELECTED && faces.is_empty()) {
				_set_status(TTR("Select the faces to unwrap first (Select tool, or click islands in the UV view)."));
				return;
			}
			const uint64_t start = OS::get_singleton()->get_ticks_msec();
			const Error err = painter->smart_unwrap(faces, angle_limit_slider->get_value(), padding, resolution);
			if (err != OK) {
				_set_status(TTR("Unwrapping failed. See the Output panel for details."));
				return;
			}
			_commit(p_option == UV_UNWRAP_SELECTED ? TTR("Mesh Paint: Unwrap Selected Faces") : TTR("Mesh Paint: Smart Unwrap"));
			_set_status(vformat(TTR("Unwrapped into %d islands in %d ms. Textures painted before keep the old layout."), painter->get_island_count(), int(OS::get_singleton()->get_ticks_msec() - start)));
		} break;
		case UV_PACK:
		case UV_PACK_KEEP_SCALE: {
			if (!painter->has_uvs()) {
				_set_status(TTR("This mesh has no UVs to pack: use Smart Unwrap."));
				return;
			}
			if (painter->pack_islands(padding, resolution, true, p_option == UV_PACK) != OK) {
				_set_status(TTR("Packing failed. See the Output panel for details."));
				return;
			}
			_commit(TTR("Mesh Paint: Pack UV Islands"));
			_set_status(vformat(TTR("Packed %d islands."), painter->get_island_count()));
		} break;
		case UV_ROTATE_CW:
		case UV_ROTATE_CCW:
		case UV_FLIP_H:
		case UV_FLIP_V: {
			if (painter->get_selected_face_count() == 0) {
				_set_status(TTR("Select islands first."));
				return;
			}
			const Vector2 center = uv_editor->get_selection_center();
			Transform2D local;
			if (p_option == UV_ROTATE_CW || p_option == UV_ROTATE_CCW) {
				// UV y points down, so a positive angle turns clockwise on screen.
				local = Transform2D(p_option == UV_ROTATE_CW ? Math::PI * 0.5 : -Math::PI * 0.5, Vector2());
			} else {
				local = Transform2D(0, p_option == UV_FLIP_H ? Size2(-1, 1) : Size2(1, -1), 0, Vector2());
			}
			painter->transform_faces_uv(painter->get_selected_faces(), Transform2D(0, center) * local * Transform2D(0, -center));
			_commit(TTR("Mesh Paint: Turn UV Islands"));
		} break;
	}
	selection_dirty = true;
	_update_info();
	uv_editor->queue_redraw();
}

void MeshPaintEditorPlugin::_selection_option(int p_option) {
	if (!painter->is_loaded()) {
		return;
	}
	switch (p_option) {
		case SELECTION_ALL:
			painter->select_all(true);
			break;
		case SELECTION_NONE:
			painter->select_all(false);
			break;
		case SELECTION_INVERT:
			painter->invert_selection();
			break;
		case SELECTION_GROW:
			painter->grow_selection();
			break;
		case SELECTION_SHRINK:
			painter->shrink_selection();
			break;
		case SELECTION_ISLANDS:
			painter->select_islands_of_selection();
			break;
		case SELECTION_PIECES:
			painter->select_pieces_of_selection();
			break;
		case SELECTION_SLOT:
			_select_slot_faces();
			return;
	}
	uv_selection_changed();
}

void MeshPaintEditorPlugin::_folder_browse() {
	folder_dialog->set_current_dir(folder_edit->get_text().is_empty() ? _default_folder() : folder_edit->get_text());
	folder_dialog->popup_file_dialog();
}

void MeshPaintEditorPlugin::_folder_selected(const String &p_dir) {
	folder_edit->set_text(p_dir);
}

void MeshPaintEditorPlugin::_save_pressed() {
	_save_layers();
	_set_status(TTR("Painted textures saved."));
}

void MeshPaintEditorPlugin::_slot_selected(int p_index) {
	if (updating_ui) {
		return;
	}
	active_slot = p_index;
	_update_slot_list();
	uv_editor->queue_redraw();
}

void MeshPaintEditorPlugin::_add_slot() {
	if (!node || !painter->is_loaded()) {
		return;
	}
	Ref<StandardMaterial3D> material;
	material.instantiate();
	// A distinct colour, so the new part shows up at once.
	const float hue = Math::fmod(0.13f + 0.618034f * painter->get_slot_count(), 1.0f);
	material->set_albedo(Color::from_hsv(hue, 0.45, 0.85));
	active_slot = painter->add_slot(material);
	if (painter->get_selected_face_count() > 0) {
		painter->set_faces_slot(painter->get_selected_faces(), active_slot);
		_commit(TTR("Mesh Paint: New Material for Selection"));
	}
	_update_slot_list();
	_update_info();
	_set_status(painter->get_selected_face_count() > 0 ? TTR("The selected faces got the new material. Pick your own material file in the slot's Material field, or edit this one below.") : TTR("New material slot. Select faces and press Assign to give them this material."));
}

void MeshPaintEditorPlugin::_remove_slot() {
	if (!node || painter->get_slot_count() <= 1) {
		_set_status(TTR("A mesh keeps at least one material."));
		return;
	}
	painter->remove_slot(active_slot);
	active_slot = MAX(active_slot - 1, 0);
	_commit(TTR("Mesh Paint: Remove Material Slot"));
	_update_slot_list();
}

void MeshPaintEditorPlugin::_assign_selection() {
	if (!node || painter->get_selected_face_count() == 0) {
		_set_status(TTR("Select faces first: Select tool in the 3D view, or islands in the UV view."));
		return;
	}
	painter->set_faces_slot(painter->get_selected_faces(), active_slot);
	_commit(TTR("Mesh Paint: Assign Material"));
	_update_slot_list();
	_set_status(vformat(TTR("%d faces now use \"%s\"."), painter->get_selected_face_count(), painter->get_slot_name(active_slot)));
}

void MeshPaintEditorPlugin::_select_slot_faces() {
	painter->set_selected_faces(painter->get_slot_faces(active_slot));
	uv_selection_changed();
}

void MeshPaintEditorPlugin::_slot_material_changed(const Ref<Resource> &p_material) {
	if (updating_ui || !node || active_slot >= painter->get_slot_count()) {
		return;
	}
	painter->set_slot_material(active_slot, p_material);
	_commit(TTR("Mesh Paint: Set Slot Material"));
	_update_slot_list();
	uv_editor->queue_redraw();
}

void MeshPaintEditorPlugin::_slot_name_submitted(const String &p_name) {
	if (updating_ui || !node || active_slot >= painter->get_slot_count() || p_name.strip_edges().is_empty()) {
		return;
	}
	painter->set_slot_name(active_slot, p_name.strip_edges());
	_commit(TTR("Mesh Paint: Rename Material Slot"));
	_update_slot_list();
}

void MeshPaintEditorPlugin::_material_property_edited(const String &p_property) {
	_update_slot_list();
	uv_editor->queue_redraw();
}

// Plugin.

void MeshPaintEditorPlugin::_check_saveable() {
	Node *root = EditorNode::get_singleton()->get_edited_scene();
	if (!node || !root) {
		return;
	}
	// A node inside an instanced scene only keeps changes when the instance's children are
	// editable (like any other change to such a node).
	for (Node *owner = node->get_owner(); owner && owner != root; owner = owner->get_owner()) {
		if (!root->is_editable_instance(owner)) {
			_set_status(vformat(TTR("\"%s\" is part of the instanced scene \"%s\". Turn on Editable Children for it (right click it in the Scene dock), or the painted mesh is not saved with this scene."), node->get_name(), owner->get_name()));
			return;
		}
	}
}

void MeshPaintEditorPlugin::set_painting(bool p_painting) {
	painting = p_painting && _can_paint_node(node);
	if (paint_button) {
		paint_button->set_pressed_no_signal(painting);
	}
	if (painting) {
		_sync_painter();
		_create_overlays();
		_check_saveable();
		dock->make_visible();
		set_process(true);
	} else {
		if (stroking) {
			_end_stroke();
		}
		hovering = false;
		_free_overlays();
		if (dock) {
			dock->close();
		}
	}
	_update_info();
	if (uv_editor) {
		uv_editor->queue_redraw();
	}
}

void MeshPaintEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_EXIT_TREE: {
			// Everything holding renderer resources goes now, while the renderer is still there.
			_free_overlays();
			layers.clear();
			painter->clear();
			brush->set_texture(Ref<Texture2D>());
			transform_before_mesh.unref();
			selection_material.unref();
			ring_material.unref();
			if (material_inspector) {
				material_inspector->edit(nullptr);
			}
		} break;
		case NOTIFICATION_PROCESS: {
			if (node && !ObjectDB::get_instance(node_id)) {
				node = nullptr;
				set_painting(false);
				return;
			}
			if (!painting || !node) {
				return;
			}
			_sync_painter();
			if (textures_dirty) {
				_flush_textures();
				uv_editor->queue_redraw();
			}
			if (selection_dirty) {
				_update_selection_overlay();
			}
			const Transform3D global = node->get_global_transform();
			RenderingServer *rs = RenderingServer::get_singleton();
			if (selection_instance.is_valid()) {
				rs->instance_set_transform(selection_instance, global);
			}
			if (ring_instance.is_valid()) {
				rs->instance_set_transform(ring_instance, global);
			}
		} break;
	}
}

EditorPlugin::AfterGUIInput MeshPaintEditorPlugin::forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	if (!painting || !node || painter.is_null() || !painter->is_loaded()) {
		return EditorPlugin::AFTER_GUI_INPUT_PASS;
	}

	const Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MouseButton::LEFT) {
		if (mb->is_pressed()) {
			const View3DController::NavigationScheme nav_scheme = (View3DController::NavigationScheme)EDITOR_GET("editors/3d/navigation/navigation_scheme").operator int();
			if ((nav_scheme == View3DController::NAV_SCHEME_MAYA || nav_scheme == View3DController::NAV_SCHEME_MODO) && mb->is_alt_pressed()) {
				return EditorPlugin::AFTER_GUI_INPUT_PASS;
			}
			Hit hit;
			switch (tool) {
				case TOOL_PAINT: {
					if (!painter->has_uvs()) {
						_set_status(TTR("This mesh has no UVs yet: use UV > Smart Unwrap first."));
						break;
					}
					_begin_stroke();
					_paint_at(p_camera, mb->get_position());
					last_dab_screen = mb->get_position();
				} break;
				case TOOL_FILL: {
					if (_pick(p_camera, mb->get_position(), hit)) {
						_fill_at(hit.face);
					}
				} break;
				case TOOL_PICK: {
					if (_pick(p_camera, mb->get_position(), hit)) {
						_pick_color_at(hit);
					}
				} break;
				case TOOL_SELECT: {
					stroke_subtract = mb->is_command_or_control_pressed();
					const bool has_hit = _pick(p_camera, mb->get_position(), hit);
					if (!mb->is_shift_pressed() && !stroke_subtract && (select_mode != SELECT_FACES || !has_hit)) {
						painter->select_all(false); // A plain click starts a new selection.
					}
					stroking = true;
					_select_at(p_camera, mb->get_position(), stroke_subtract, true);
				} break;
				default:
					break;
			}
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		}
		if (stroking) {
			if (tool == TOOL_SELECT) {
				stroking = false;
			} else {
				_end_stroke();
			}
		}
		return EditorPlugin::AFTER_GUI_INPUT_STOP;
	}

	const Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		Hit hit;
		hovering = _pick(p_camera, mm->get_position(), hit);
		if (hovering) {
			hover_position = hit.position;
			hover_normal = hit.normal;
			hover_radius = hit.radius;
		}
		_update_ring();
		if (stroking && tool == TOOL_PAINT) {
			const real_t spacing = MAX(radius_px * 0.25, (real_t)1.0);
			const Vector2 to = mm->get_position();
			const real_t distance = last_dab_screen.distance_to(to);
			if (distance >= spacing) {
				const int steps = MIN((int)(distance / spacing), 256);
				for (int i = 1; i <= steps; i++) {
					_paint_at(p_camera, last_dab_screen.lerp(to, real_t(i) / steps));
				}
				last_dab_screen = to;
			}
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		}
		if (stroking && tool == TOOL_SELECT && select_mode == SELECT_FACES) {
			_select_at(p_camera, mm->get_position(), stroke_subtract, false);
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		}
		return EditorPlugin::AFTER_GUI_INPUT_PASS;
	}

	const Ref<InputEventKey> k = p_event;
	if (k.is_valid() && k->is_pressed() && !k->is_command_or_control_pressed() && !k->is_alt_pressed()) {
		if (k->get_keycode() == Key::BRACKETLEFT || k->get_keycode() == Key::BRACKETRIGHT) {
			radius_slider->set_value(radius_px * (k->get_keycode() == Key::BRACKETLEFT ? 1.0 / 1.15 : 1.15));
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		}
	}
	return EditorPlugin::AFTER_GUI_INPUT_PASS;
}

void MeshPaintEditorPlugin::edit(Object *p_object) {
	MeshInstance3D *next = Object::cast_to<MeshInstance3D>(p_object);
	if (next == node) {
		return;
	}
	if (stroking) {
		_end_stroke();
	}
	_free_overlays();
	node = next;
	node_id = node ? node->get_instance_id() : ObjectID();
	hovering = false;
	if (paint_button) {
		paint_button->set_disabled(!_can_paint_node(node));
		paint_button->set_tooltip_text(_can_paint_node(node) ? TTR("Mesh Paint: paint textures on this mesh, unwrap and edit its UV islands, and give parts of it their own materials.\nWhile it is on, clicks in the 3D view paint or select faces instead of picking nodes.") : TTR("Mesh Paint works on a MeshInstance3D with a mesh. A Quick Mesh Brush or editable (CSG) mesh rebuilds its mesh, so convert it to a MeshInstance3D first."));
	}
	if (painting && _can_paint_node(node)) {
		set_painting(true);
	} else if (painting && !node) {
		// Keep the mode for the next mesh picked, but nothing to show now.
		painter->clear();
		dock->close();
	}
}

bool MeshPaintEditorPlugin::handles(Object *p_object) const {
	return Object::cast_to<MeshInstance3D>(p_object) != nullptr;
}

void MeshPaintEditorPlugin::make_visible(bool p_visible) {
	if (paint_button) {
		paint_button->set_visible(p_visible);
	}
	set_process(p_visible);
	if (!p_visible) {
		if (stroking) {
			_end_stroke();
		}
		hovering = false;
		_free_overlays();
		if (dock) {
			dock->close();
		}
	} else if (painting && _can_paint_node(node)) {
		set_painting(true);
	}
}

void MeshPaintEditorPlugin::apply_changes() {
	// Runs before a scene is saved: the PNGs are written and the materials pointed at them, so
	// the scene never embeds the live paint textures.
	_save_layers();
}

void MeshPaintEditorPlugin::_build_dock() {
	dock = memnew(EditorDock);
	dock->set_name(TTRC("Mesh Paint"));
	dock->set_icon_name("MeshPaint");
	dock->set_default_slot(EditorDock::DOCK_SLOT_BOTTOM);
	dock->set_available_layouts(EditorDock::DOCK_LAYOUT_HORIZONTAL | EditorDock::DOCK_LAYOUT_FLOATING);
	dock->set_global(false);
	dock->set_transient(true);
	dock->set_custom_minimum_size(Size2(0, 300) * EDSCALE);

	HSplitContainer *split = memnew(HSplitContainer);
	dock->add_child(split);

	// Left: tools and brush.
	ScrollContainer *left_scroll = memnew(ScrollContainer);
	left_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	left_scroll->set_custom_minimum_size(Size2(250, 0) * EDSCALE);
	split->add_child(left_scroll);
	VBoxContainer *left = memnew(VBoxContainer);
	left->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	left_scroll->add_child(left);

	HBoxContainer *tools = memnew(HBoxContainer);
	left->add_child(tools);
	const char *tool_tips[TOOL_MAX] = {
		TTRC("Paint: drag on the model, or in the UV view. [ and ] change the size."),
		TTRC("Fill: click an island (or the selection, when you click inside it) to fill it with the brush colour."),
		TTRC("Select faces: drag to brush faces in, Ctrl to brush them out, Shift to add. Islands and Pieces modes select a whole part per click."),
		TTRC("Pick: click the model to take its colour (or value) for the brush."),
	};
	for (int i = 0; i < TOOL_MAX; i++) {
		Button *button = memnew(Button);
		button->set_theme_type_variation(SceneStringName(FlatButton));
		button->set_toggle_mode(true);
		button->set_tooltip_text(TTRGET(tool_tips[i]));
		button->connect(SceneStringName(pressed), callable_mp(this, &MeshPaintEditorPlugin::_tool_selected).bind(i));
		tools->add_child(button);
		tool_buttons[i] = button;
	}
	select_mode_option = memnew(OptionButton);
	select_mode_option->add_item(TTR("Faces (Brush)"), SELECT_FACES);
	select_mode_option->add_item(TTR("UV Islands"), SELECT_ISLANDS);
	select_mode_option->add_item(TTR("Pieces (Linked)"), SELECT_PIECES);
	select_mode_option->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	select_mode_option->connect(SceneStringName(item_selected), callable_mp(this, &MeshPaintEditorPlugin::_select_mode_selected));
	tools->add_child(select_mode_option);

	left->add_child(make_header(TTR("Brush")));
	channel_option = memnew(OptionButton);
	channel_option->add_item(TTR("Albedo (Color)"), MeshPainter::CHANNEL_ALBEDO);
	channel_option->add_item(TTR("Roughness"), MeshPainter::CHANNEL_ROUGHNESS);
	channel_option->add_item(TTR("Metallic"), MeshPainter::CHANNEL_METALLIC);
	channel_option->add_item(TTR("Emission"), MeshPainter::CHANNEL_EMISSION);
	channel_option->set_tooltip_text(TTR("Which texture of the material is painted. Each slot gets its own PNG per channel."));
	channel_option->connect(SceneStringName(item_selected), callable_mp(this, &MeshPaintEditorPlugin::_channel_selected));
	left->add_child(channel_option);

	HBoxContainer *color_row = memnew(HBoxContainer);
	left->add_child(color_row);
	Label *color_label = memnew(Label);
	color_label->set_text(TTR("Color"));
	color_row->add_child(color_label);
	color_picker = memnew(ColorPickerButton);
	color_picker->set_pick_color(Color(0.8, 0.2, 0.2));
	color_picker->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	color_picker->set_custom_minimum_size(Size2(0, 24) * EDSCALE);
	color_picker->connect("color_changed", callable_mp(this, &MeshPaintEditorPlugin::_color_changed));
	color_row->add_child(color_picker);
	value_slider = make_slider(TTR("Value"), 0, 1, 0.01, 0.5, TTR("The roughness or metallic value to paint."));
	value_slider->connect(SceneStringName(value_changed), callable_mp(this, &MeshPaintEditorPlugin::_value_changed));
	left->add_child(value_slider);

	radius_slider = make_slider(TTR("Radius"), 1, 500, 1, radius_px, TTR("Brush radius in screen pixels ([ and ] in the 3D view)."));
	radius_slider->set_suffix("px");
	radius_slider->set_allow_greater(true);
	radius_slider->connect(SceneStringName(value_changed), callable_mp(this, &MeshPaintEditorPlugin::_radius_changed));
	left->add_child(radius_slider);
	strength_slider = make_slider(TTR("Strength"), 0, 1, 0.01, 1.0, TTR("How much one stroke covers. Strokes never build up over themselves; paint again to add more."));
	strength_slider->connect(SceneStringName(value_changed), callable_mp(this, &MeshPaintEditorPlugin::_strength_changed));
	left->add_child(strength_slider);
	hardness_slider = make_slider(TTR("Hardness"), 0, 1, 0.01, 0.5, TTR("0 fades from the centre, 1 has a crisp edge. Use 1 for stamping textures."));
	hardness_slider->connect(SceneStringName(value_changed), callable_mp(this, &MeshPaintEditorPlugin::_hardness_changed));
	left->add_child(hardness_slider);

	blend_option = memnew(OptionButton);
	const char *blends[] = { TTRC("Mix"), TTRC("Add"), TTRC("Multiply"), TTRC("Lighten"), TTRC("Darken"), TTRC("Erase (Alpha)"), TTRC("Blur") };
	for (int i = 0; i < MeshPaintBrush::BLEND_MAX; i++) {
		blend_option->add_item(TTRGET(blends[i]), i);
	}
	blend_option->connect(SceneStringName(item_selected), callable_mp(this, &MeshPaintEditorPlugin::_blend_selected));
	left->add_child(blend_option);

	Label *texture_label = memnew(Label);
	texture_label->set_text(TTR("Brush Texture (stamp / decal)"));
	left->add_child(texture_label);
	brush_texture_picker = memnew(EditorResourcePicker);
	brush_texture_picker->set_base_type("Texture2D");
	brush_texture_picker->set_tooltip_text(TTR("Your own image as the brush: it is stamped on the model facing the camera (logos, patterns, cloth prints). Its alpha is the brush shape."));
	brush_texture_picker->connect("resource_changed", callable_mp(this, &MeshPaintEditorPlugin::_brush_texture_changed));
	left->add_child(brush_texture_picker);
	angle_slider = make_slider(TTR("Angle"), -180, 180, 1, 0, TTR("Turns the brush texture."));
	angle_slider->set_suffix(U"°");
	angle_slider->connect(SceneStringName(value_changed), callable_mp(this, &MeshPaintEditorPlugin::_angle_changed));
	left->add_child(angle_slider);

	occlusion_check = memnew(CheckBox);
	occlusion_check->set_text(TTR("Only Visible Surfaces"));
	occlusion_check->set_tooltip_text(TTR("Don't paint parts hidden behind other parts of the mesh (an arm in front of the body)."));
	occlusion_check->set_pressed(true);
	occlusion_check->connect(SceneStringName(toggled), callable_mp(this, &MeshPaintEditorPlugin::_brush_option_toggled).bind(0));
	left->add_child(occlusion_check);
	backfaces_check = memnew(CheckBox);
	backfaces_check->set_text(TTR("Paint Back Faces"));
	backfaces_check->connect(SceneStringName(toggled), callable_mp(this, &MeshPaintEditorPlugin::_brush_option_toggled).bind(1));
	left->add_child(backfaces_check);
	active_slot_check = memnew(CheckBox);
	active_slot_check->set_text(TTR("Only the Active Material"));
	active_slot_check->set_tooltip_text(TTR("Paint only faces of the material selected on the right, like masking tape at the edges of a part."));
	active_slot_check->connect(SceneStringName(toggled), callable_mp(this, &MeshPaintEditorPlugin::_brush_option_toggled).bind(2));
	left->add_child(active_slot_check);

	left->add_child(make_header(TTR("UVs and Selection")));
	HBoxContainer *menus = memnew(HBoxContainer);
	left->add_child(menus);
	uv_menu = memnew(MenuButton);
	uv_menu->set_text(TTR("UV"));
	uv_menu->set_flat(false);
	uv_menu->set_theme_type_variation("FlatMenuButton");
	PopupMenu *uv_popup = uv_menu->get_popup();
	uv_popup->add_item(TTR("Smart Unwrap"), UV_SMART_UNWRAP);
	uv_popup->set_item_tooltip(uv_popup->get_item_count() - 1, TTR("New UVs for the whole mesh: islands split at sharp angles and between materials, then packed in the square."));
	uv_popup->add_item(TTR("Unwrap Selected Faces"), UV_UNWRAP_SELECTED);
	uv_popup->set_item_tooltip(uv_popup->get_item_count() - 1, TTR("Only the selected faces get new UVs, as their own islands; the rest keeps its layout and everything is packed again."));
	uv_popup->add_separator();
	uv_popup->add_item(TTR("Pack Islands"), UV_PACK);
	uv_popup->set_item_tooltip(uv_popup->get_item_count() - 1, TTR("Places the islands again, all at the same texel density."));
	uv_popup->add_item(TTR("Pack Islands (Keep Sizes)"), UV_PACK_KEEP_SCALE);
	uv_popup->set_item_tooltip(uv_popup->get_item_count() - 1, TTR("Places the islands again but keeps how big you made each one."));
	uv_popup->add_separator();
	uv_popup->add_item(TTR("Rotate Selected 90° Clockwise"), UV_ROTATE_CW);
	uv_popup->add_item(TTR("Rotate Selected 90° Counter-Clockwise"), UV_ROTATE_CCW);
	uv_popup->add_item(TTR("Flip Selected Horizontally"), UV_FLIP_H);
	uv_popup->add_item(TTR("Flip Selected Vertically"), UV_FLIP_V);
	uv_popup->connect(SceneStringName(id_pressed), callable_mp(this, &MeshPaintEditorPlugin::_uv_option));
	menus->add_child(uv_menu);
	selection_menu = memnew(MenuButton);
	selection_menu->set_text(TTR("Select"));
	selection_menu->set_flat(false);
	selection_menu->set_theme_type_variation("FlatMenuButton");
	PopupMenu *selection_popup = selection_menu->get_popup();
	selection_popup->add_item(TTR("All"), SELECTION_ALL);
	selection_popup->add_item(TTR("None"), SELECTION_NONE);
	selection_popup->add_item(TTR("Invert"), SELECTION_INVERT);
	selection_popup->add_separator();
	selection_popup->add_item(TTR("Grow"), SELECTION_GROW);
	selection_popup->add_item(TTR("Shrink"), SELECTION_SHRINK);
	selection_popup->add_item(TTR("Whole UV Islands"), SELECTION_ISLANDS);
	selection_popup->add_item(TTR("Whole Pieces (Linked)"), SELECTION_PIECES);
	selection_popup->add_item(TTR("Faces of the Active Material"), SELECTION_SLOT);
	selection_popup->connect(SceneStringName(id_pressed), callable_mp(this, &MeshPaintEditorPlugin::_selection_option));
	menus->add_child(selection_menu);

	angle_limit_slider = make_slider(TTR("Angle Limit"), 5, 89, 1, 66, TTR("Smart Unwrap: faces bent more than this from their island start a new island."));
	angle_limit_slider->set_suffix(U"°");
	left->add_child(angle_limit_slider);
	padding_slider = make_slider(TTR("Island Margin"), 0, 32, 1, 4, TTR("Texels between islands, so paint never bleeds into a neighbour."));
	padding_slider->set_suffix("px");
	left->add_child(padding_slider);
	HBoxContainer *resolution_row = memnew(HBoxContainer);
	left->add_child(resolution_row);
	Label *resolution_label = memnew(Label);
	resolution_label->set_text(TTR("Texture Size"));
	resolution_row->add_child(resolution_label);
	resolution_option = memnew(OptionButton);
	resolution_option->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	resolution_option->set_tooltip_text(TTR("Size of new painted textures, and what unwrap margins are measured against."));
	for (const int size : { 512, 1024, 2048, 4096, 8192 }) {
		resolution_option->add_item(vformat("%dx%d", size, size), size);
	}
	resolution_option->select(resolution_option->get_item_index(resolution));
	resolution_option->connect(SceneStringName(item_selected), callable_mp(this, &MeshPaintEditorPlugin::_resolution_selected));
	resolution_row->add_child(resolution_option);

	left->add_child(make_header(TTR("Textures")));
	HBoxContainer *folder_row = memnew(HBoxContainer);
	left->add_child(folder_row);
	folder_edit = memnew(LineEdit);
	folder_edit->set_placeholder(TTR("Folder (default: mesh_paint next to the scene)"));
	folder_edit->set_tooltip_text(TTR("Where new painted PNGs are saved."));
	folder_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	folder_row->add_child(folder_edit);
	Button *browse = memnew(Button);
	browse->set_text("...");
	browse->connect(SceneStringName(pressed), callable_mp(this, &MeshPaintEditorPlugin::_folder_browse));
	folder_row->add_child(browse);
	save_button = memnew(Button);
	save_button->set_text(TTR("Save Painted Textures"));
	save_button->set_tooltip_text(TTR("Writes the painted PNGs now. Saving the scene does this too."));
	save_button->connect(SceneStringName(pressed), callable_mp(this, &MeshPaintEditorPlugin::_save_pressed));
	left->add_child(save_button);
	info_label = memnew(Label);
	info_label->set_modulate(Color(1, 1, 1, 0.65));
	left->add_child(info_label);
	status_label = memnew(Label);
	status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	status_label->set_custom_minimum_size(Size2(200, 0) * EDSCALE);
	status_label->set_modulate(Color(1, 1, 1, 0.8));
	left->add_child(status_label);

	folder_dialog = memnew(EditorFileDialog);
	folder_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_DIR);
	folder_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
	folder_dialog->connect("dir_selected", callable_mp(this, &MeshPaintEditorPlugin::_folder_selected));
	dock->add_child(folder_dialog);

	// Middle: the UV view.
	HSplitContainer *split2 = memnew(HSplitContainer);
	split2->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	split->add_child(split2);
	uv_editor = memnew(MeshPaintUVEditor(this));
	uv_editor->set_tooltip_text(TTR("UV view. Click an island to select it, drag to move it, Shift adds, Ctrl removes, drag in empty space to box select (Alt: single faces).\nG move, R rotate, S scale, A select all, F fit. Wheel zooms, middle mouse pans. With the Paint tool, paint here directly."));
	split2->add_child(uv_editor);

	// Right: material slots.
	VBoxContainer *right = memnew(VBoxContainer);
	right->set_custom_minimum_size(Size2(300, 0) * EDSCALE);
	split2->add_child(right);
	right->add_child(make_header(TTR("Materials (one per part)")));
	slot_list = memnew(ItemList);
	slot_list->set_custom_minimum_size(Size2(0, 90) * EDSCALE);
	slot_list->connect(SceneStringName(item_selected), callable_mp(this, &MeshPaintEditorPlugin::_slot_selected));
	right->add_child(slot_list);
	HBoxContainer *slot_buttons = memnew(HBoxContainer);
	right->add_child(slot_buttons);
	Button *add = memnew(Button);
	add->set_text(TTR("New"));
	add->set_tooltip_text(TTR("New material slot. With faces selected, they get it straight away."));
	add->connect(SceneStringName(pressed), callable_mp(this, &MeshPaintEditorPlugin::_add_slot));
	slot_buttons->add_child(add);
	Button *assign = memnew(Button);
	assign->set_text(TTR("Assign"));
	assign->set_tooltip_text(TTR("Give the selected faces this material."));
	assign->connect(SceneStringName(pressed), callable_mp(this, &MeshPaintEditorPlugin::_assign_selection));
	slot_buttons->add_child(assign);
	Button *select = memnew(Button);
	select->set_text(TTR("Select"));
	select->set_tooltip_text(TTR("Select the faces that use this material."));
	select->connect(SceneStringName(pressed), callable_mp(this, &MeshPaintEditorPlugin::_select_slot_faces));
	slot_buttons->add_child(select);
	Button *remove = memnew(Button);
	remove->set_text(TTR("Remove"));
	remove->set_tooltip_text(TTR("Remove this slot; its faces go to another one."));
	remove->connect(SceneStringName(pressed), callable_mp(this, &MeshPaintEditorPlugin::_remove_slot));
	slot_buttons->add_child(remove);

	slot_name_edit = memnew(LineEdit);
	slot_name_edit->set_placeholder(TTR("Slot name"));
	slot_name_edit->connect(SceneStringName(text_submitted), callable_mp(this, &MeshPaintEditorPlugin::_slot_name_submitted));
	right->add_child(slot_name_edit);
	material_picker = memnew(EditorResourcePicker);
	material_picker->set_base_type("Material");
	material_picker->set_tooltip_text(TTR("This slot's material: drop your own material file here, or make a new StandardMaterial3D."));
	material_picker->connect("resource_changed", callable_mp(this, &MeshPaintEditorPlugin::_slot_material_changed));
	right->add_child(material_picker);
	material_inspector = memnew(EditorInspector);
	material_inspector->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	material_inspector->set_use_doc_hints(true);
	material_inspector->set_theme_type_variation(SNAME("EditorInspectorForeground"));
	material_inspector->connect(SNAME("property_edited"), callable_mp(this, &MeshPaintEditorPlugin::_material_property_edited));
	right->add_child(material_inspector);

	EditorDockManager::get_singleton()->add_dock(dock);
	dock->close();
	dock->connect(SceneStringName(theme_changed), callable_mp(this, &MeshPaintEditorPlugin::_update_theme));
	_update_ui();
}

void MeshPaintEditorPlugin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_set_image_region", "image", "region", "position"), &MeshPaintEditorPlugin::_set_image_region);
	ClassDB::bind_method(D_METHOD("_refresh"), &MeshPaintEditorPlugin::_refresh);
}

MeshPaintEditorPlugin::MeshPaintEditorPlugin() {
	singleton = this;
	painter.instantiate();
	brush.instantiate();
	brush->set_color(Color(0.8, 0.2, 0.2));

	selection_material.instantiate();
	selection_material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	selection_material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	selection_material->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	selection_material->set_albedo(Color(1.0, 0.55, 0.1, 0.35));
	selection_material->set_render_priority(Material::RENDER_PRIORITY_MAX);
	selection_material->set_flag(BaseMaterial3D::FLAG_DISABLE_FOG, true);
	ring_material.instantiate();
	ring_material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	ring_material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	ring_material->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
	ring_material->set_flag(BaseMaterial3D::FLAG_DISABLE_FOG, true);
	ring_material->set_albedo(Color(1, 1, 1, 0.9));
	ring_material->set_render_priority(Material::RENDER_PRIORITY_MAX);

	paint_button = memnew(Button);
	paint_button->set_theme_type_variation(SceneStringName(FlatButton));
	paint_button->set_toggle_mode(true);
	paint_button->set_text(TTR("Paint"));
	paint_button->hide();
	paint_button->connect(SceneStringName(toggled), callable_mp(this, &MeshPaintEditorPlugin::_paint_toggled));
	paint_button->connect(SceneStringName(theme_changed), callable_mp(this, &MeshPaintEditorPlugin::_update_theme));
	Node3DEditor::get_singleton()->add_control_to_menu_panel(paint_button);

	_build_dock();
}

MeshPaintEditorPlugin::~MeshPaintEditorPlugin() {
	_free_overlays();
	if (singleton == this) {
		singleton = nullptr;
	}
}
