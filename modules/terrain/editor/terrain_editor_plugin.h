/**************************************************************************/
/*  terrain_editor_plugin.h                                               */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#pragma once

#include "../foliage_type.h"
#include "../terrain_data.h"
#include "terrain_brush.h"

#include "editor/docks/editor_dock.h"
#include "editor/plugins/editor_plugin.h"

class AcceptDialog;
class Button;
class ButtonGroup;
class CheckBox;
class ColorPickerButton;
class EditorFileDialog;
class EditorSpinSlider;
class Foliage3D;
class HBoxContainer;
class ItemList;
class Label;
class MenuButton;
class SpinBox;
class Terrain3D;
class Terrain3DGizmoPlugin;
class TerrainCreateDialog;
class VBoxContainer;

// The Terrain dock: tools, brush settings and the layer and foliage palettes.
// It edits whichever Terrain3D or Foliage3D is selected; selecting a Foliage3D
// paints its terrain and selecting a Terrain3D paints its first Foliage3D.
class TerrainEditor : public EditorDock {
	GDCLASS(TerrainEditor, EditorDock);

	friend class TerrainEditorPlugin;

	enum MenuOption {
		MENU_NEW,
		MENU_IMPORT,
		MENU_EXPORT,
		MENU_REBUILD_COLLISION,
		MENU_ADD_FOLIAGE,
		MENU_MOVE_DATA,
	};

	ObjectID terrain_id;
	ObjectID foliage_id;

	TerrainBrush::Tool tool = TerrainBrush::TOOL_NONE;
	TerrainBrush::Settings settings;

	// Controls.
	MenuButton *terrain_menu = nullptr;
	MenuButton *view_menu = nullptr;
	Label *info_label = nullptr;
	Button *tool_buttons[TerrainBrush::TOOL_MAX] = {};
	Ref<ButtonGroup> tool_group;
	EditorSpinSlider *size_slider = nullptr;
	EditorSpinSlider *strength_slider = nullptr;
	EditorSpinSlider *hardness_slider = nullptr;
	HBoxContainer *flatten_row = nullptr;
	CheckBox *flatten_fixed = nullptr;
	SpinBox *flatten_height = nullptr;
	EditorSpinSlider *noise_slider = nullptr;
	EditorSpinSlider *terrace_slider = nullptr;
	HBoxContainer *color_row = nullptr;
	ColorPickerButton *color_picker = nullptr;
	Label *hint_label = nullptr;

	VBoxContainer *layer_panel = nullptr;
	ItemList *layer_list = nullptr;
	VBoxContainer *foliage_panel = nullptr;
	ItemList *foliage_list = nullptr;
	Label *foliage_empty = nullptr;

	TerrainCreateDialog *create_dialog = nullptr;
	EditorFileDialog *export_dialog = nullptr;
	EditorFileDialog *foliage_add_dialog = nullptr;
	AcceptDialog *message_dialog = nullptr;

	// Stroke.
	bool stroking = false;
	bool cursor_valid = false;
	Vector3 cursor;
	Vector3 last_dab;
	Vector3 ramp_start;
	uint32_t stroke_seed = 0;
	uint32_t dab_counter = 0;
	HashMap<Vector2i, Dictionary> stroke_before;
	uint32_t stroke_maps = 0;
	Dictionary foliage_before;
	int foliage_before_type = -1;
	ObjectID last_camera;
	bool active = false;

	// The data being listened to. Undo, a new terrain or the inspector can swap
	// the data out from under the dock, so it is re-checked every frame.
	ObjectID watched_data;
	ObjectID watched_foliage_data;
	void _watch();
	void _show_transform_gizmo(bool p_show);

	Terrain3D *_get_terrain() const;
	Foliage3D *_get_foliage() const;
	Ref<TerrainData> _get_data() const;

	void _tool_toggled(bool p_pressed, int p_tool);
	void _update_tool_ui();
	void _update_info();
	void _update_brush_preview();
	void _settings_changed(double p_value = 0.0);
	void _color_changed(const Color &p_color);
	void _flatten_fixed_toggled(bool p_pressed);
	void _menu_option(int p_option);
	void _view_option(int p_option);
	void _create_confirmed();
	void _export_selected(const String &p_path);
	void _show_message(const String &p_text);

	void _refresh_layers();
	void _layer_selected(int p_index);
	void _add_layer();
	void _remove_layer();
	void _edit_layer();
	void _add_layer_from(const Ref<Texture2D> &p_albedo);

	void _refresh_foliage();
	void _foliage_selected(int p_index);
	void _add_foliage_pressed();
	void _foliage_files_selected(const Vector<String> &p_paths);
	Ref<FoliageType> _make_foliage_type(const Ref<Resource> &p_resource, FoliageType::Kind p_kind, String &r_error) const;
	void _add_foliage_type(const Ref<FoliageType> &p_type);
	void _remove_foliage_type();
	void _edit_foliage_type();
	void _foliage_preview_ready(const String &p_path, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_small, int p_index);
	Foliage3D *_ensure_foliage();

	bool _can_drop_layer(const Point2 &p_point, const Variant &p_data) const;
	void _drop_layer(const Point2 &p_point, const Variant &p_data);
	bool _can_drop_foliage(const Point2 &p_point, const Variant &p_data) const;
	void _drop_foliage(const Point2 &p_point, const Variant &p_data);

	void _data_changed();

	// Terrain data is large, so it lives in its own binary file beside the
	// scene rather than inline in it. These pick that file and move data
	// there; the editor then saves it with the scene whenever it changes.
	String _default_data_path(Node *p_node, const String &p_suffix) const;
	bool _store_external(const Ref<Resource> &p_res, const String &p_path);
	void _ensure_external(const Ref<Resource> &p_res, Node *p_node, const String &p_suffix);

	bool _pick(Camera3D *p_camera, const Point2 &p_point, Vector3 &r_hit) const;
	void _snapshot(const Rect2i &p_rect, uint32_t p_maps);
	void _begin_stroke();
	void _dab(const Vector3 &p_at, float p_delta);
	void _end_stroke(bool p_cancel = false);
	Color _tool_color() const;

protected:
	void _notification(int p_what);

public:
	EditorPlugin::AfterGUIInput forward_input(Camera3D *p_camera, const Ref<InputEvent> &p_event);
	void draw_overlay(Control *p_overlay);
	void edit(Object *p_object);
	void set_active(bool p_active);

	TerrainEditor();
};

class TerrainEditorPlugin : public EditorPlugin {
	GDCLASS(TerrainEditorPlugin, EditorPlugin);

	TerrainEditor *terrain_editor = nullptr;
	Ref<Terrain3DGizmoPlugin> gizmo_plugin;

protected:
	void _notification(int p_what);

public:
	virtual String get_plugin_name() const override { return "Terrain3D"; }
	virtual bool handles(Object *p_object) const override;
	virtual void edit(Object *p_object) override;
	virtual void make_visible(bool p_visible) override;
	virtual EditorPlugin::AfterGUIInput forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) override;
	virtual void forward_3d_draw_over_viewport(Control *p_overlay) override;

	TerrainEditorPlugin();
};
