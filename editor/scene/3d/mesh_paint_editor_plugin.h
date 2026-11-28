/**************************************************************************/
/*  mesh_paint_editor_plugin.h                                            */
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

#include "editor/plugins/editor_plugin.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/gui/control.h"
#include "scene/resources/3d/mesh_painter.h"
#include "scene/resources/image_texture.h"

class Button;
class CheckBox;
class ColorPickerButton;
class EditorDock;
class EditorFileDialog;
class EditorInspector;
class EditorResourcePicker;
class EditorSpinSlider;
class HBoxContainer;
class ItemList;
class Label;
class LineEdit;
class MenuButton;
class OptionButton;
class VBoxContainer;
class MeshPaintEditorPlugin;

// Asosdot: the Mesh Paint tool's UV view. Shows the active slot's texture under the UV layout;
// islands are picked, box selected, moved (drag or G), rotated (R) and scaled (S) here, and the
// Paint tool paints straight into the texture.
class MeshPaintUVEditor : public Control {
	GDCLASS(MeshPaintUVEditor, Control);

	enum Drag {
		DRAG_NONE,
		DRAG_PAN,
		DRAG_BOX,
		DRAG_MOVE,
		DRAG_ROTATE,
		DRAG_SCALE,
		DRAG_PAINT,
	};

	MeshPaintEditorPlugin *plugin = nullptr;
	real_t zoom = 1.0;
	Vector2 pan;
	bool view_fitted = false;

	Drag drag = DRAG_NONE;
	bool drag_from_click = false; // Move started by dragging an island, confirmed on release.
	Vector2 drag_start; // Screen.
	Vector2 drag_last; // Screen.
	Vector2 pivot; // UV.
	Transform2D applied; // UV transform applied so far by the current move/rotate/scale.
	bool box_add = false;
	bool box_remove = false;
	Vector2 last_paint_uv;
	uint64_t last_live_update = 0;

	void _transform_to(const Transform2D &p_transform);
	void _finish_transform(bool p_cancel);
	void _begin_transform(Drag p_mode, const Vector2 &p_mouse);
	void _paint_at(const Vector2 &p_screen);

protected:
	void _notification(int p_what);
	virtual void gui_input(const Ref<InputEvent> &p_event) override;

public:
	Transform2D get_uv_to_screen() const;
	Vector2 uv_to_screen(const Vector2 &p_uv) const;
	Vector2 screen_to_uv(const Vector2 &p_screen) const;
	int pick_face(const Vector2 &p_uv) const;
	Vector2 get_selection_center() const;
	void fit_view();
	bool is_transforming() const { return drag == DRAG_MOVE || drag == DRAG_ROTATE || drag == DRAG_SCALE; }

	MeshPaintUVEditor(MeshPaintEditorPlugin *p_plugin);
};

// Asosdot: Mesh Paint. A built-in mode for any MeshInstance3D: paint textures on the model in
// the 3D view or in the UV view, smart unwrap and pack UVs, pick and edit UV islands, and give
// parts of the mesh their own materials. The result is an ordinary ArrayMesh with one surface
// per material and painted PNGs next to the scene, so it costs nothing extra in the game.
class MeshPaintEditorPlugin : public EditorPlugin {
	GDCLASS(MeshPaintEditorPlugin, EditorPlugin);

	friend class MeshPaintUVEditor;

public:
	enum Tool {
		TOOL_PAINT,
		TOOL_FILL,
		TOOL_SELECT,
		TOOL_PICK,
		TOOL_MAX,
	};

	enum SelectMode {
		SELECT_FACES,
		SELECT_ISLANDS,
		SELECT_PIECES,
	};

private:
	enum SelectionOption {
		SELECTION_ALL,
		SELECTION_NONE,
		SELECTION_INVERT,
		SELECTION_GROW,
		SELECTION_SHRINK,
		SELECTION_ISLANDS,
		SELECTION_PIECES,
		SELECTION_SLOT,
	};

	enum UVOption {
		UV_SMART_UNWRAP,
		UV_UNWRAP_SELECTED,
		UV_PACK,
		UV_PACK_KEEP_SCALE,
		UV_ROTATE_CW,
		UV_ROTATE_CCW,
		UV_FLIP_H,
		UV_FLIP_V,
	};

	// One painted texture: a material channel with a live image while painting, saved as a
	// PNG that the material then uses.
	struct PaintLayer {
		Ref<BaseMaterial3D> material;
		MeshPainter::Channel channel = MeshPainter::CHANNEL_ALBEDO;
		Ref<Image> image;
		Ref<ImageTexture> live;
		Ref<Texture2D> file_texture;
		String path;
		bool dirty = false; // Painted since it was last saved.
		bool upload = false; // The live texture needs the image again.
	};

	MeshInstance3D *node = nullptr;
	ObjectID node_id;
	ObjectID painter_node; // The node the painter last loaded.
	bool painting = false;
	Ref<MeshPainter> painter;
	Ref<MeshPaintBrush> brush;
	LocalVector<PaintLayer> layers;

	Tool tool = TOOL_PAINT;
	SelectMode select_mode = SELECT_FACES;
	MeshPainter::Channel channel = MeshPainter::CHANNEL_ALBEDO;
	int active_slot = 0;
	real_t radius_px = 40.0;
	int resolution = 2048;
	bool only_active_slot = false;

	// Stroke state.
	bool stroking = false;
	bool stroke_subtract = false;
	Vector2 last_dab_screen;
	HashSet<int> stroke_layers;
	bool textures_dirty = false;
	bool selection_dirty = false;
	Ref<Mesh> transform_before_mesh; // The mesh to go back to while UV islands are dragged.

	// Viewport overlays (selection and brush ring), drawn with the RenderingServer.
	RID selection_instance;
	RID ring_instance;
	Ref<ArrayMesh> selection_mesh;
	Ref<ArrayMesh> ring_mesh;
	Ref<StandardMaterial3D> selection_material;
	Ref<StandardMaterial3D> ring_material;
	bool hovering = false;
	Vector3 hover_position;
	Vector3 hover_normal;
	real_t hover_radius = 0.0;

	// UI.
	Button *paint_button = nullptr;
	EditorDock *dock = nullptr;
	Button *tool_buttons[TOOL_MAX] = {};
	OptionButton *select_mode_option = nullptr;
	OptionButton *channel_option = nullptr;
	ColorPickerButton *color_picker = nullptr;
	EditorSpinSlider *value_slider = nullptr;
	EditorSpinSlider *radius_slider = nullptr;
	EditorSpinSlider *strength_slider = nullptr;
	EditorSpinSlider *hardness_slider = nullptr;
	OptionButton *blend_option = nullptr;
	EditorResourcePicker *brush_texture_picker = nullptr;
	EditorSpinSlider *angle_slider = nullptr;
	CheckBox *occlusion_check = nullptr;
	CheckBox *backfaces_check = nullptr;
	CheckBox *active_slot_check = nullptr;
	OptionButton *resolution_option = nullptr;
	EditorSpinSlider *angle_limit_slider = nullptr;
	EditorSpinSlider *padding_slider = nullptr;
	MenuButton *uv_menu = nullptr;
	MenuButton *selection_menu = nullptr;
	LineEdit *folder_edit = nullptr;
	EditorFileDialog *folder_dialog = nullptr;
	Button *save_button = nullptr;
	Label *info_label = nullptr;
	Label *status_label = nullptr;
	MeshPaintUVEditor *uv_editor = nullptr;
	ItemList *slot_list = nullptr;
	EditorResourcePicker *material_picker = nullptr;
	LineEdit *slot_name_edit = nullptr;
	EditorInspector *material_inspector = nullptr;
	bool updating_ui = false;
	String copy_note; // Shown with the next status, so it is not lost.

	// Painter <-> node.
	bool _can_paint_node(MeshInstance3D *p_node) const;
	void _sync_painter();
	Ref<Mesh> _make_pose_mesh() const;
	void _commit(const String &p_action);
	void _restore_selection_after_reload(const PackedInt32Array &p_selected, int p_face_count);

	// Paint layers.
	int _find_layer(const Ref<Material> &p_material, MeshPainter::Channel p_channel) const;
	int _get_layer(int p_slot, MeshPainter::Channel p_channel, bool p_create);
	Ref<BaseMaterial3D> _make_slot_material_local(int p_slot);
	String _default_folder() const;
	String _make_layer_path(const Ref<BaseMaterial3D> &p_material, int p_slot, MeshPainter::Channel p_channel) const;
	void _show_live(PaintLayer &p_layer);
	void _flush_textures();
	Error _save_layers();
	void _set_image_region(const Ref<Image> &p_image, const Ref<Image> &p_region, const Vector2i &p_position);
	void _record_stroke(const String &p_action);

	// Tools.
	struct Hit {
		int face = -1;
		Vector3 position; // Mesh local space, like everything below.
		Vector3 normal;
		real_t radius = 0.0; // The brush radius at this depth.
		Vector3 eye;
		Vector3 up;
	};
	bool _pick(Camera3D *p_camera, const Vector2 &p_point, Hit &r_hit) const;
	void _begin_stroke();
	void _paint_at(Camera3D *p_camera, const Vector2 &p_point);
	void _end_stroke();
	void _fill_at(int p_face);
	void _pick_color_at(const Hit &p_hit);
	void _select_at(Camera3D *p_camera, const Vector2 &p_point, bool p_subtract, bool p_click);
	void _update_brush_color();

	// Overlays.
	void _create_overlays();
	void _free_overlays();
	void _update_selection_overlay();
	void _update_ring();

	// UI.
	void _build_dock();
	void _update_ui();
	void _update_slot_list();
	void _update_info();
	void _set_status(const String &p_text);
	void _check_saveable();
	void _update_theme();
	void _paint_toggled(bool p_on);
	void _tool_selected(int p_tool);
	void _select_mode_selected(int p_mode);
	void _channel_selected(int p_channel);
	void _color_changed(const Color &p_color);
	void _value_changed(double p_value);
	void _radius_changed(double p_value);
	void _strength_changed(double p_value);
	void _hardness_changed(double p_value);
	void _blend_selected(int p_mode);
	void _brush_texture_changed(const Ref<Resource> &p_texture);
	void _angle_changed(double p_value);
	void _brush_option_toggled(bool p_on, int p_which);
	void _resolution_selected(int p_index);
	void _uv_option(int p_option);
	void _selection_option(int p_option);
	void _folder_browse();
	void _folder_selected(const String &p_dir);
	void _save_pressed();
	void _slot_selected(int p_index);
	void _add_slot();
	void _remove_slot();
	void _assign_selection();
	void _select_slot_faces();
	void _slot_material_changed(const Ref<Resource> &p_material);
	void _slot_name_submitted(const String &p_name);
	void _material_property_edited(const String &p_property);
	void _refresh();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	inline static MeshPaintEditorPlugin *singleton = nullptr;

	MeshInstance3D *get_node() const { return node; }
	const Ref<MeshPainter> &get_painter() const { return painter; }
	const Ref<MeshPaintBrush> &get_brush() const { return brush; }
	Tool get_tool() const { return tool; }
	int get_active_slot() const { return active_slot; }
	real_t get_radius_px() const { return radius_px; }
	// The image the UV view shows and paints: the active slot's layer for the channel.
	Ref<Texture2D> get_uv_background() const;
	Ref<Image> get_uv_paint_image(bool p_create);
	void uv_stroke_painted(const Ref<Image> &p_image);
	void uv_stroke_finished();
	void uv_selection_changed();
	void uv_transform_committed(const Ref<Mesh> &p_before, const String &p_action);
	void uv_transform_preview();

	bool is_painting() const { return painting; }
	void set_painting(bool p_painting);

	virtual EditorPlugin::AfterGUIInput forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) override;
	virtual String get_plugin_name() const override { return "MeshPaint"; }
	virtual void edit(Object *p_object) override;
	virtual bool handles(Object *p_object) const override;
	virtual void make_visible(bool p_visible) override;
	virtual void apply_changes() override;

	MeshPaintEditorPlugin();
	~MeshPaintEditorPlugin();
};
