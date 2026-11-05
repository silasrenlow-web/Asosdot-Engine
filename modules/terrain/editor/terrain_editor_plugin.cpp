/**************************************************************************/
/*  terrain_editor_plugin.cpp                                             */
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

#include "terrain_editor_plugin.h"

#include "../foliage_3d.h"
#include "../terrain_3d.h"
#include "../voxel_terrain_3d.h"
#include "terrain_3d_gizmo_plugin.h"
#include "terrain_create_dialog.h"
#include "terrain_heightmap_io.h"
#include "voxel_terrain_3d_gizmo_plugin.h"

#include "core/input/input.h"
#include "core/io/file_access.h"
#include "core/io/resource.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/math/random_pcg.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "editor/docks/editor_dock_manager.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/gui/editor_spin_slider.h"
#include "editor/inspector/editor_inspector.h"
#include "editor/inspector/editor_resource_preview.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/settings/editor_command_palette.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/color_picker.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/item_list.h"
#include "scene/gui/label.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/separator.h"
#include "scene/gui/spin_box.h"
#include "scene/gui/split_container.h"
#include "scene/gui/tab_container.h"
#include "scene/resources/image_texture.h"

#include <iterator>

namespace {

struct ToolInfo {
	TerrainBrush::Tool tool;
	const char *icon;
	const char *tooltip;
};

const ToolInfo SCULPT_TOOLS[] = {
	{ TerrainBrush::TOOL_RAISE, "TerrainRaise", TTRC("Raise ground. Hold Shift to lower it.") },
	{ TerrainBrush::TOOL_DIG, "TerrainDig", TTRC("Dig: carves caves, tunnels and overhangs out of voxel terrain. Hold Shift to fill instead.") },
	{ TerrainBrush::TOOL_SMOOTH, "TerrainSmooth", TTRC("Smooth: softens bumps and sharp edges.") },
	{ TerrainBrush::TOOL_FLATTEN, "TerrainFlatten", TTRC("Flatten: levels ground to the height where the stroke starts, or to a fixed height. Ctrl+Click picks the height.") },
	{ TerrainBrush::TOOL_ERODE, "TerrainErode", TTRC("Erode: rain runs downhill, carving gullies and leaving sediment where it slows.") },
	{ TerrainBrush::TOOL_NOISE, "TerrainNoise", TTRC("Noise: adds natural roughness. Hold Shift to subtract it.") },
	{ TerrainBrush::TOOL_TERRACE, "TerrainTerrace", TTRC("Terrace: steps the slope into ledges.") },
	{ TerrainBrush::TOOL_RAMP, "TerrainRamp", TTRC("Ramp: drag from one point to another for an even slope between them. Good for roads and paths.") },
	{ TerrainBrush::TOOL_HOLES, "TerrainHoles", TTRC("Holes: cut holes for caves, pits and basements. Hold Shift to fill them.") },
};

const ToolInfo PAINT_TOOLS[] = {
	{ TerrainBrush::TOOL_PAINT, "TerrainPaint", TTRC("Paint the selected layer. Hold Shift to erase it.") },
	{ TerrainBrush::TOOL_COLOR, "TerrainColor", TTRC("Paint a colour tint over the ground. Hold Shift to wash it out.") },
	{ TerrainBrush::TOOL_WETNESS, "TerrainWetness", TTRC("Paint wetness: mud, puddles, blood. Wet ground is darker and glossier. Hold Shift to dry it.") },
	{ TerrainBrush::TOOL_GRASS, "TerrainGrass", TTRC("Paint the selected grass. Hold Shift to remove it.") },
	{ TerrainBrush::TOOL_OBJECTS, "TerrainObjects", TTRC("Paint the selected objects: trees, rocks, props. Hold Shift to remove them.") },
	{ TerrainBrush::TOOL_REGIONS, "TerrainRegions", TTRC("Click to add a region of terrain. Shift+Click removes one.") },
};

Ref<Texture2D> make_swatch(const Color &p_color) {
	Ref<Image> image = Image::create_empty(32, 32, false, Image::FORMAT_RGBA8);
	image->fill(p_color);
	return ImageTexture::create_from_image(image);
}

String foliage_display_name(const Ref<FoliageType> &p_type, int p_index) {
	if (p_type.is_null()) {
		return vformat("#%d", p_index);
	}
	if (!p_type->get_name().is_empty()) {
		return p_type->get_name();
	}
	if (p_type->get_scene().is_valid() && !p_type->get_scene()->get_path().is_empty()) {
		return p_type->get_scene()->get_path().get_file().get_basename();
	}
	if (p_type->get_mesh().is_valid() && !p_type->get_mesh()->get_path().is_empty() && !p_type->get_mesh()->is_built_in()) {
		return p_type->get_mesh()->get_path().get_file().get_basename();
	}
	return p_type->get_kind() == FoliageType::KIND_GRASS ? vformat(TTR("Grass %d"), p_index + 1) : vformat(TTR("Object %d"), p_index + 1);
}

} // namespace

/* -------------------------------------------------------------------------- */
/*                                   Editing                                  */
/* -------------------------------------------------------------------------- */

Terrain3D *AsosdotTerrainEditor::_get_terrain() const {
	return ObjectDB::get_instance<Terrain3D>(terrain_id);
}

Foliage3D *AsosdotTerrainEditor::_get_foliage() const {
	return ObjectDB::get_instance<Foliage3D>(foliage_id);
}

Ref<TerrainData> AsosdotTerrainEditor::_get_data() const {
	const Terrain3D *terrain = _get_terrain();
	return terrain ? terrain->get_data() : Ref<TerrainData>();
}

VoxelTerrain3D *AsosdotTerrainEditor::_get_voxel() const {
	return ObjectDB::get_instance<VoxelTerrain3D>(voxel_id);
}

Ref<VoxelTerrainData> AsosdotTerrainEditor::_get_voxel_data() const {
	const VoxelTerrain3D *voxel = _get_voxel();
	return voxel ? voxel->get_data() : Ref<VoxelTerrainData>();
}

Node3D *AsosdotTerrainEditor::_get_edited_node() const {
	if (_is_voxel()) {
		return _get_voxel();
	}
	return _get_terrain();
}

int AsosdotTerrainEditor::LayerOwner::count() const {
	if (const TerrainData *t = Object::cast_to<TerrainData>(resource)) {
		return t->get_layer_count();
	}
	if (const VoxelTerrainData *v = Object::cast_to<VoxelTerrainData>(resource)) {
		return v->get_layer_count();
	}
	return 0;
}

Ref<TerrainLayer> AsosdotTerrainEditor::LayerOwner::get(int p_index) const {
	if (const TerrainData *t = Object::cast_to<TerrainData>(resource)) {
		return t->get_layer(p_index);
	}
	if (const VoxelTerrainData *v = Object::cast_to<VoxelTerrainData>(resource)) {
		return v->get_layer(p_index);
	}
	return Ref<TerrainLayer>();
}

AsosdotTerrainEditor::LayerOwner AsosdotTerrainEditor::_get_layer_owner() const {
	LayerOwner owner;
	if (_is_voxel()) {
		VoxelTerrain3D *voxel = _get_voxel();
		if (voxel && voxel->get_data().is_valid()) {
			owner.resource = voxel->get_data().ptr();
			owner.node = voxel;
		}
	} else {
		Terrain3D *terrain = _get_terrain();
		if (terrain && terrain->get_data().is_valid()) {
			owner.resource = terrain->get_data().ptr();
			owner.node = terrain;
		}
	}
	return owner;
}

void AsosdotTerrainEditor::edit(Object *p_object) {
	if (stroking) {
		_end_stroke();
	}
	Terrain3D *previous = _get_terrain();
	if (previous) {
		previous->set_brush_preview(false);
	}
	VoxelTerrain3D *previous_voxel = _get_voxel();
	if (previous_voxel) {
		previous_voxel->set_brush_preview(false);
	}

	terrain_id = ObjectID();
	foliage_id = ObjectID();
	voxel_id = ObjectID();
	VoxelTerrain3D *voxel = Object::cast_to<VoxelTerrain3D>(p_object);
	if (voxel) {
		voxel_id = voxel->get_instance_id();
	}
	Terrain3D *terrain = Object::cast_to<Terrain3D>(p_object);
	Foliage3D *foliage = Object::cast_to<Foliage3D>(p_object);
	if (foliage) {
		foliage_id = foliage->get_instance_id();
		Node *ground = foliage->get_node_or_null(foliage->get_terrain_path());
		terrain = Object::cast_to<Terrain3D>(ground);
		voxel = Object::cast_to<VoxelTerrain3D>(ground);
		if (voxel) {
			voxel_id = voxel->get_instance_id();
		}
	} else if (terrain || voxel) {
		Node *ground = terrain ? static_cast<Node *>(terrain) : static_cast<Node *>(voxel);
		for (int i = 0; i < ground->get_child_count(); i++) {
			foliage = Object::cast_to<Foliage3D>(ground->get_child(i));
			if (foliage) {
				foliage_id = foliage->get_instance_id();
				break;
			}
		}
	}
	if (terrain) {
		terrain_id = terrain->get_instance_id();
	}
	_watch();
	_refresh_layers();
	_refresh_foliage();
	_update_info();
	_update_menus();
	_update_tool_ui();
}

void AsosdotTerrainEditor::_watch() {
	const LayerOwner owner = _get_layer_owner();
	const ObjectID data_id = owner.resource ? owner.resource->get_instance_id() : ObjectID();
	if (data_id != watched_data) {
		const Callable changed = callable_mp(this, &AsosdotTerrainEditor::_data_changed);
		Object *old = ObjectDB::get_instance(watched_data);
		if (old) {
			for (const StringName &signal : { SNAME("layers_changed"), SNAME("regions_changed") }) {
				if (old->has_signal(signal) && old->is_connected(signal, changed)) {
					old->disconnect(signal, changed);
				}
			}
		}
		watched_data = data_id;
		if (owner.resource) {
			owner.resource->connect(SNAME("layers_changed"), changed);
			if (Object::cast_to<TerrainData>(owner.resource)) {
				owner.resource->connect(SNAME("regions_changed"), changed);
			}
		}
		_data_changed();
	}

	const Foliage3D *foliage = _get_foliage();
	const Ref<FoliageData> fd = foliage ? foliage->get_data() : Ref<FoliageData>();
	const ObjectID fd_id = fd.is_valid() ? fd->get_instance_id() : ObjectID();
	if (fd_id != watched_foliage_data) {
		FoliageData *old = ObjectDB::get_instance<FoliageData>(watched_foliage_data);
		if (old) {
			old->disconnect(SNAME("types_changed"), callable_mp(this, &AsosdotTerrainEditor::_refresh_foliage));
			old->disconnect(SNAME("type_changed"), callable_mp(this, &AsosdotTerrainEditor::_foliage_selected).bind(-2).unbind(1));
		}
		watched_foliage_data = fd_id;
		if (fd.is_valid()) {
			fd->connect(SNAME("types_changed"), callable_mp(this, &AsosdotTerrainEditor::_refresh_foliage));
			fd->connect(SNAME("type_changed"), callable_mp(this, &AsosdotTerrainEditor::_foliage_selected).bind(-2).unbind(1));
		}
		_refresh_foliage();
	}
}

void AsosdotTerrainEditor::_data_changed() {
	_refresh_layers();
	_update_info();
}

void AsosdotTerrainEditor::set_active(bool p_active) {
	active = p_active;
	if (!p_active) {
		if (stroking) {
			_end_stroke();
		}
		Terrain3D *terrain = _get_terrain();
		if (terrain) {
			terrain->set_brush_preview(false);
			terrain->set_brush_segment(false);
		}
		VoxelTerrain3D *voxel = _get_voxel();
		if (voxel) {
			voxel->set_brush_preview(false);
		}
		_show_transform_gizmo(true);
	} else {
		_show_transform_gizmo(tool == TerrainBrush::TOOL_NONE);
	}
}

void AsosdotTerrainEditor::_show_transform_gizmo(bool p_show) {
	// While a brush is out, the move gizmo in the middle of the terrain would
	// catch clicks meant for the ground.
	Dictionary state;
	state["transform_gizmo"] = p_show;
	for (uint32_t i = 0; i < Node3DEditor::VIEWPORTS_COUNT; i++) {
		Node3DEditor::get_singleton()->get_editor_viewport(i)->set_state(state);
	}
}

/* -------------------------------------------------------------------------- */
/*                                     UI                                     */
/* -------------------------------------------------------------------------- */

void AsosdotTerrainEditor::_tool_toggled(bool p_pressed, int p_tool) {
	if (stroking) {
		_end_stroke();
	}
	// Toggled rather than pressed, so selecting a tool from code (picking a
	// layer switches to Paint) goes through here too. Within the group, the old
	// tool is released before the new one is pressed.
	if (p_pressed) {
		tool = (TerrainBrush::Tool)p_tool;
	} else if (tool == (TerrainBrush::Tool)p_tool) {
		tool = TerrainBrush::TOOL_NONE;
	}
	_show_transform_gizmo(tool == TerrainBrush::TOOL_NONE);
	if (tool == TerrainBrush::TOOL_PAINT) {
		palette_tabs->set_current_tab(0);
	} else if (TerrainBrush::is_foliage(tool)) {
		palette_tabs->set_current_tab(1);
	}
	_update_tool_ui();
	_update_brush_preview();
}

void AsosdotTerrainEditor::_update_tool_ui() {
	flatten_row->set_visible(tool == TerrainBrush::TOOL_FLATTEN || tool == TerrainBrush::TOOL_REGIONS);
	noise_slider->set_visible(tool == TerrainBrush::TOOL_NOISE);
	terrace_slider->set_visible(tool == TerrainBrush::TOOL_TERRACE);
	color_row->set_visible(tool == TerrainBrush::TOOL_COLOR);
	hardness_slider->set_visible(tool != TerrainBrush::TOOL_REGIONS);
	size_slider->set_visible(tool != TerrainBrush::TOOL_REGIONS);
	strength_slider->set_visible(tool != TerrainBrush::TOOL_REGIONS && tool != TerrainBrush::TOOL_HOLES);

	String hint;
	switch (tool) {
		case TerrainBrush::TOOL_NONE:
			hint = TTR("Pick a tool to start sculpting or painting.");
			break;
		case TerrainBrush::TOOL_REGIONS:
			hint = TTR("Click empty space to add a region at the height below. Shift+Click a region to remove it.");
			break;
		case TerrainBrush::TOOL_FLATTEN:
			hint = TTR("Ctrl+Click to pick a height. [ and ] resize the brush.");
			break;
		case TerrainBrush::TOOL_RAMP:
			hint = TTR("Drag from the bottom of the ramp to the top.");
			break;
		default:
			hint = TTR("Shift inverts. [ and ] resize the brush.");
			break;
	}
	hint_label->set_text(hint);

	// Voxel terrain gets the tools that work in three dimensions; the
	// heightmap-only ones are hidden rather than grayed out, since they will
	// never apply to it.
	const bool voxel = _is_voxel();
	const Terrain3D *terrain = _get_terrain();
	for (int i = 1; i < TerrainBrush::TOOL_MAX; i++) {
		if (!tool_buttons[i]) {
			continue;
		}
		const bool shown = voxel ? (i == TerrainBrush::TOOL_RAISE || i == TerrainBrush::TOOL_DIG || i == TerrainBrush::TOOL_SMOOTH || i == TerrainBrush::TOOL_FLATTEN || i == TerrainBrush::TOOL_PAINT || TerrainBrush::is_foliage((TerrainBrush::Tool)i)) : i != TerrainBrush::TOOL_DIG;
		tool_buttons[i]->set_visible(shown);
		if (voxel) {
			tool_buttons[i]->set_disabled(!_get_voxel());
		} else {
			tool_buttons[i]->set_disabled(!terrain || (i != TerrainBrush::TOOL_REGIONS && _get_data().is_null()));
		}
		if (!shown && tool == i) {
			tool_buttons[i]->set_pressed(false);
		}
	}
	if (tool_buttons[TerrainBrush::TOOL_RAISE]) {
		tool_buttons[TerrainBrush::TOOL_RAISE]->set_tooltip_text(voxel ? TTR("Add ground where the brush touches, in the selected layer. Hold Shift to take it away.") : TTR("Raise ground. Hold Shift to lower it."));
	}
}

void AsosdotTerrainEditor::_update_info() {
	if (_is_voxel()) {
		const Ref<VoxelTerrainData> vdata = _get_voxel_data();
		if (vdata.is_null()) {
			info_label->set_text(TTR("Empty. Use Terrain > New Voxel Terrain, or just start sculpting."));
			info_label->set_tooltip_text(String());
			return;
		}
		const String where = vdata->is_built_in() ? TTR("Stored in the scene") : vdata->get_path().get_file();
		info_label->set_text(vformat(TTR("Voxel terrain, %s m voxels\n%d edited blocks, %d layers\n%s"), String::num(vdata->get_voxel_size(), 2), vdata->get_block_count(), vdata->get_layer_count(), where));
		info_label->set_tooltip_text(vdata->is_built_in() ? TTR("The data is inside the scene file. The first edit moves it to its own file beside the scene, or use Terrain > Move Data to File.") : vdata->get_path());
		return;
	}
	const Terrain3D *terrain = _get_terrain();
	const Ref<TerrainData> tdata = _get_data();
	if (!terrain) {
		info_label->set_text(TTR("No terrain selected."));
		return;
	}
	if (tdata.is_null() || tdata->get_region_count() == 0) {
		info_label->set_text(TTR("Empty. Use Terrain > New Terrain... to create one."));
		return;
	}
	const Rect2i bounds = tdata->get_vertex_bounds();
	const float sp = terrain->get_vertex_spacing();
	String where = tdata->is_built_in() ? TTR("Stored in the scene") : tdata->get_path().get_file();
	info_label->set_text(vformat(TTR("%s x %s m\n%d regions, %d layers\n%s"), String::num(bounds.size.x * sp, 0), String::num(bounds.size.y * sp, 0), tdata->get_region_count(), tdata->get_layer_count(), where));
	info_label->set_tooltip_text(tdata->is_built_in() ? TTR("The data is inside the scene file. The first edit moves it to its own file beside the scene, or use Terrain > Move Data to File.") : tdata->get_path());
}

void AsosdotTerrainEditor::_settings_changed(double p_value) {
	settings.radius = size_slider->get_value();
	settings.strength = strength_slider->get_value();
	settings.hardness = hardness_slider->get_value();
	settings.flatten_height = flatten_height->get_value();
	settings.noise_scale = noise_slider->get_value();
	settings.terrace_step = terrace_slider->get_value();
	_update_brush_preview();
}

void AsosdotTerrainEditor::_color_changed(const Color &p_color) {
	settings.color = p_color;
	_update_brush_preview();
}

void AsosdotTerrainEditor::_flatten_fixed_toggled(bool p_pressed) {
	flatten_height->set_editable(p_pressed);
}

Color AsosdotTerrainEditor::_tool_color() const {
	const bool invert = Input::get_singleton()->is_key_pressed(Key::SHIFT);
	switch (tool) {
		case TerrainBrush::TOOL_RAISE:
			return invert ? Color(1.0, 0.35, 0.3) : Color(0.35, 1.0, 0.45);
		case TerrainBrush::TOOL_DIG:
			return invert ? Color(0.35, 1.0, 0.45) : Color(1.0, 0.45, 0.2);
		case TerrainBrush::TOOL_SMOOTH:
			return Color(0.4, 0.85, 1.0);
		case TerrainBrush::TOOL_FLATTEN:
			return Color(1.0, 0.85, 0.3);
		case TerrainBrush::TOOL_ERODE:
			return Color(1.0, 0.6, 0.25);
		case TerrainBrush::TOOL_NOISE:
			return Color(0.8, 0.5, 1.0);
		case TerrainBrush::TOOL_TERRACE:
			return Color(0.9, 0.75, 0.55);
		case TerrainBrush::TOOL_RAMP:
			return Color(1.0, 0.9, 0.5);
		case TerrainBrush::TOOL_HOLES:
			return invert ? Color(0.35, 1.0, 0.45) : Color(1.0, 0.2, 0.2);
		case TerrainBrush::TOOL_COLOR:
			return settings.color.lerp(Color(1, 1, 1), 0.2);
		case TerrainBrush::TOOL_WETNESS:
			return invert ? Color(0.9, 0.75, 0.45) : Color(0.3, 0.55, 1.0);
		case TerrainBrush::TOOL_GRASS:
			return invert ? Color(1.0, 0.45, 0.3) : Color(0.5, 1.0, 0.3);
		case TerrainBrush::TOOL_OBJECTS:
			return invert ? Color(1.0, 0.45, 0.3) : Color(0.8, 0.6, 0.35);
		case TerrainBrush::TOOL_REGIONS:
			return invert ? Color(1.0, 0.3, 0.3) : Color(1.0, 0.6, 0.1);
		default:
			return Color(1, 1, 1);
	}
}

void AsosdotTerrainEditor::_update_brush_preview() {
	const bool show = active && cursor_valid && tool != TerrainBrush::TOOL_NONE;
	VoxelTerrain3D *voxel = _get_voxel();
	if (voxel) {
		voxel->set_brush_preview(show, cursor, settings.radius, settings.hardness, _tool_color());
		return;
	}
	Terrain3D *terrain = _get_terrain();
	if (!terrain) {
		return;
	}
	float radius = settings.radius;
	float hardness = settings.hardness;
	if (tool == TerrainBrush::TOOL_REGIONS && terrain->get_data().is_valid()) {
		radius = terrain->get_data()->get_region_size() * terrain->get_vertex_spacing() * 0.5f;
		hardness = 1.0;
	}
	terrain->set_brush_preview(show, cursor, radius, hardness, _tool_color());
}

void AsosdotTerrainEditor::_show_message(const String &p_text) {
	message_dialog->set_text(p_text);
	message_dialog->popup_centered();
}

void AsosdotTerrainEditor::_update_menus() {
	// The two kinds of terrain have different things to do and to look at.
	const bool voxel = _is_voxel();
	PopupMenu *menu = terrain_menu->get_popup();
	menu->clear();
	if (voxel) {
		menu->add_item(TTR("New Voxel Terrain"), MENU_NEW);
		menu->set_item_tooltip(-1, TTR("Start over with flat ground and no edits. Its generator, in the Inspector, can add hills, caves or a heightmap to start from."));
		menu->add_separator();
		menu->add_item(TTR("Add Foliage3D"), MENU_ADD_FOLIAGE);
		menu->add_item(TTR("Rebuild"), MENU_REBUILD_COLLISION);
		menu->set_item_tooltip(-1, TTR("Mesh every block again, and its collision."));
		menu->add_item(TTR("Move Data to File"), MENU_MOVE_DATA);
		menu->set_item_tooltip(-1, TTR("Store the voxel data in its own compressed file beside the scene, instead of inside it. New voxel terrains do this already."));
	} else {
		menu->add_item(TTR("New Terrain..."), MENU_NEW);
		menu->add_item(TTR("Import Heightmap..."), MENU_IMPORT);
		menu->add_item(TTR("Export Heightmap..."), MENU_EXPORT);
		menu->add_separator();
		menu->add_item(TTR("Add Foliage3D"), MENU_ADD_FOLIAGE);
		menu->add_item(TTR("Rebuild Collision"), MENU_REBUILD_COLLISION);
		menu->add_item(TTR("Move Data to File"), MENU_MOVE_DATA);
		menu->set_item_tooltip(-1, TTR("Store the terrain and foliage data in their own compressed files beside the scene, instead of inside it. New terrains do this already."));
		menu->add_separator();
		menu->add_item(TTR("Convert to Voxel Terrain"), MENU_CONVERT_TO_VOXEL);
		menu->set_item_tooltip(-1, TTR("Add a VoxelTerrain3D that starts as this heightmap, with its layers and paint, so caves, tunnels and overhangs can be dug into it."));
	}

	PopupMenu *views = view_menu->get_popup();
	views->clear();
	int current = 0;
	if (voxel) {
		const char *names[] = { TTRC("Shaded"), TTRC("Normals"), TTRC("Layers"), TTRC("Level of Detail") };
		for (int i = 0; i < 4; i++) {
			views->add_radio_check_item(TTRGET(names[i]), i);
		}
		current = _get_voxel() ? (int)_get_voxel()->get_debug_view() : 0;
	} else {
		const char *names[] = { TTRC("Shaded"), TTRC("Height"), TTRC("Normals"), TTRC("Layers"), TTRC("Color"), TTRC("Wetness"), TTRC("Grid and Regions") };
		for (int i = 0; i < 7; i++) {
			views->add_radio_check_item(TTRGET(names[i]), i);
		}
		current = _get_terrain() ? (int)_get_terrain()->get_debug_view() : 0;
	}
	for (int i = 0; i < views->get_item_count(); i++) {
		views->set_item_checked(i, views->get_item_id(i) == current);
	}
}

void AsosdotTerrainEditor::_menu_option(int p_option) {
	VoxelTerrain3D *voxel = _get_voxel();
	if (voxel) {
		switch (p_option) {
			case MENU_NEW: {
				_new_voxel_data();
			} break;
			case MENU_REBUILD_COLLISION: {
				voxel->rebuild();
			} break;
			case MENU_ADD_FOLIAGE: {
				if (_check_ground()) {
					_ensure_foliage();
					_refresh_foliage();
				}
			} break;
			case MENU_MOVE_DATA: {
				if (voxel->get_data().is_valid() && voxel->get_data()->is_built_in()) {
					_ensure_external(voxel->get_data(), voxel, ".voxel.res");
				}
				Foliage3D *foliage = _get_foliage();
				if (foliage && foliage->get_data().is_valid() && foliage->get_data()->is_built_in()) {
					_ensure_external(foliage->get_data(), foliage, ".foliage.res");
				}
			} break;
		}
		return;
	}
	Terrain3D *terrain = _get_terrain();
	if (!terrain) {
		return;
	}
	switch (p_option) {
		case MENU_NEW: {
			create_dialog->popup_for(AsosdotTerrainCreateDialog::MODE_NEW, terrain->get_data(), terrain->get_vertex_spacing(), _default_data_path(terrain, ".terrain.res"));
		} break;
		case MENU_IMPORT: {
			create_dialog->popup_for(AsosdotTerrainCreateDialog::MODE_IMPORT, terrain->get_data(), terrain->get_vertex_spacing(), _default_data_path(terrain, ".terrain.res"));
		} break;
		case MENU_EXPORT: {
			if (_get_data().is_null() || _get_data()->get_region_count() == 0) {
				_show_message(TTR("There is no terrain to export."));
				return;
			}
			export_dialog->popup_file_dialog();
		} break;
		case MENU_REBUILD_COLLISION: {
			terrain->update_collision();
			terrain->update_gizmos();
		} break;
		case MENU_ADD_FOLIAGE: {
			_ensure_foliage();
			_refresh_foliage();
		} break;
		case MENU_MOVE_DATA: {
			if (_get_data().is_valid() && _get_data()->is_built_in()) {
				_ensure_external(_get_data(), terrain, ".terrain.res");
			}
			Foliage3D *foliage = _get_foliage();
			if (foliage && foliage->get_data().is_valid() && foliage->get_data()->is_built_in()) {
				_ensure_external(foliage->get_data(), foliage, ".foliage.res");
			}
		} break;
		case MENU_CONVERT_TO_VOXEL: {
			_convert_to_voxel();
		} break;
	}
}

void AsosdotTerrainEditor::_new_voxel_data() {
	VoxelTerrain3D *voxel = _get_voxel();
	if (!voxel) {
		return;
	}
	const Ref<VoxelTerrainData> old = voxel->get_data();
	Ref<VoxelTerrainData> vdata;
	vdata.instantiate();
	Ref<VoxelGenerator> generator;
	generator.instantiate();
	vdata->set_generator(generator);
	if (old.is_valid()) {
		// Starting over keeps the voxel size and the layers.
		vdata->set_voxel_size(old->get_voxel_size());
		vdata->call(SNAME("_set_layers"), old->call(SNAME("_get_layers")));
	}
	_store_external(vdata, _default_data_path(voxel, ".voxel.res"));

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("New Voxel Terrain"), UndoRedo::MERGE_DISABLE, voxel);
	ur->add_do_method(voxel, "set_data", vdata);
	ur->add_undo_method(voxel, "set_data", old);
	ur->add_do_reference(vdata.ptr());
	ur->commit_action();
	edit(voxel);
}

void AsosdotTerrainEditor::_convert_to_voxel() {
	Terrain3D *terrain = _get_terrain();
	const Ref<TerrainData> tdata = _get_data();
	Node *scene_root = EditorNode::get_singleton()->get_edited_scene();
	if (!terrain || tdata.is_null() || tdata->get_region_count() == 0 || !terrain->get_parent() || !scene_root) {
		_show_message(TTR("There is no heightmap terrain to convert."));
		return;
	}
	_ensure_external(tdata, terrain, ".terrain.res");

	// The heightmap stays as it is and becomes the voxel terrain's starting
	// shape, paint and all: the generator reads heights from it, the layers
	// are the same resources, and the voxels line up with its vertices.
	VoxelTerrain3D *voxel = memnew(VoxelTerrain3D);
	voxel->set_name("VoxelTerrain3D");
	voxel->set_transform(Transform3D(Basis(), terrain->get_transform().origin));
	voxel->set_terrain_material(terrain->get_terrain_material());
	Ref<VoxelGenerator> generator;
	generator.instantiate();
	generator->set_height_source(tdata);
	generator->set_height_source_spacing(terrain->get_vertex_spacing());
	// Beyond the heightmap's regions, the ground carries on at its lowest.
	generator->set_height(tdata->get_height_range().x);
	Ref<VoxelTerrainData> vdata;
	vdata.instantiate();
	vdata->set_voxel_size(terrain->get_vertex_spacing());
	vdata->set_generator(generator);
	vdata->call(SNAME("_set_layers"), tdata->call(SNAME("_get_layers")));
	voxel->set_data(vdata);
	_store_external(vdata, _default_data_path(voxel, ".voxel.res"));

	// The heightmap is hidden and stops colliding, so the two do not fight
	// where they overlap. It can be deleted once the voxel terrain is right;
	// its data file stays in use.
	Node *parent = terrain->get_parent();
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Convert to Voxel Terrain"), UndoRedo::MERGE_DISABLE, terrain);
	ur->add_do_method(parent, "add_child", voxel, true);
	ur->add_do_method(voxel, "set_owner", scene_root);
	ur->add_do_method(terrain, "set_visible", false);
	ur->add_do_method(terrain, "set_collision_enabled", false);
	ur->add_do_reference(voxel);

	// Its foliage moves across too. Grass is painted from above and needs no
	// change; an object's stored height, an offset above the heightmap,
	// becomes the height of the ground it stood on, which is what voxel
	// terrain keeps.
	for (int i = 0; i < terrain->get_child_count(); i++) {
		Foliage3D *foliage = Object::cast_to<Foliage3D>(terrain->get_child(i));
		if (!foliage || foliage->get_owner() != scene_root) {
			continue;
		}
		ur->add_do_method(foliage, "reparent", voxel, true);
		ur->add_undo_method(foliage, "reparent", terrain, true);
		const Ref<FoliageData> fd = foliage->get_data();
		for (int t = 0; fd.is_valid() && t < fd->get_type_count(); t++) {
			const PackedFloat32Array before = fd->get_instances(t);
			if (before.is_empty()) {
				continue;
			}
			PackedFloat32Array after = before;
			float *w = after.ptrw();
			for (int k = 0; k + FoliageData::INSTANCE_STRIDE <= after.size(); k += FoliageData::INSTANCE_STRIDE) {
				float h;
				if (tdata->get_height_interpolated(Vector2(w[k], w[k + 1]) / terrain->get_vertex_spacing(), h)) {
					w[k + 2] += h;
				}
			}
			ur->add_do_method(fd.ptr(), "set_instances", t, after);
			ur->add_undo_method(fd.ptr(), "set_instances", t, before);
		}
	}
	ur->add_undo_method(parent, "remove_child", voxel);
	ur->add_undo_method(terrain, "set_visible", terrain->is_visible());
	ur->add_undo_method(terrain, "set_collision_enabled", terrain->is_collision_enabled());
	ur->commit_action();
	EditorInterface::get_singleton()->edit_node(voxel);
}

void AsosdotTerrainEditor::_view_option(int p_option) {
	PopupMenu *popup = view_menu->get_popup();
	for (int i = 0; i < popup->get_item_count(); i++) {
		popup->set_item_checked(i, popup->get_item_id(i) == p_option);
	}
	if (VoxelTerrain3D *voxel = _get_voxel()) {
		voxel->set_debug_view((VoxelTerrain3D::DebugView)p_option);
	} else if (Terrain3D *terrain = _get_terrain()) {
		terrain->set_debug_view((Terrain3D::DebugView)p_option);
	}
}

void AsosdotTerrainEditor::_create_confirmed() {
	Terrain3D *terrain = _get_terrain();
	if (!terrain) {
		return;
	}
	float spacing = terrain->get_vertex_spacing();
	String error;
	const Ref<TerrainData> old = terrain->get_data();
	const Ref<TerrainData> created = create_dialog->build(old, spacing, error);
	if (created.is_null()) {
		_show_message(error.is_empty() ? TTR("The terrain could not be created.") : error);
		return;
	}
	String path = create_dialog->get_data_path();
	if (FileAccess::exists(path) || ResourceCache::has(path)) {
		// Never overwrite: the old data may still be on the undo stack.
		const String base = path.trim_suffix(".terrain.res").get_basename();
		for (int i = 2; FileAccess::exists(path) || ResourceCache::has(path); i++) {
			path = base + "_" + itos(i) + ".terrain.res";
		}
	}
	_store_external(created, path);

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(create_dialog->get_title(), UndoRedo::MERGE_DISABLE, terrain);
	ur->add_do_method(terrain, "set_vertex_spacing", spacing);
	ur->add_do_method(terrain, "set_data", created);
	ur->add_undo_method(terrain, "set_vertex_spacing", terrain->get_vertex_spacing());
	ur->add_undo_method(terrain, "set_data", old);
	ur->add_do_reference(created.ptr());
	ur->commit_action();
	edit(terrain);
}

void AsosdotTerrainEditor::_export_selected(const String &p_path) {
	const Ref<TerrainData> tdata = _get_data();
	if (tdata.is_null()) {
		return;
	}
	const Ref<Image> heights = tdata->export_height_image();
	Vector2 range;
	String error;
	if (TerrainHeightmapIO::save(heights, p_path, range, error) != OK) {
		_show_message(error);
		return;
	}
	if (p_path.get_extension().to_lower() == "exr") {
		_show_message(vformat(TTR("Exported %d x %d heights in meters."), heights->get_width(), heights->get_height()));
	} else {
		_show_message(vformat(TTR("Exported %d x %d heights. The file stores them as 0 to 1; to import it again, use a lowest height of %s m and a highest of %s m."), heights->get_width(), heights->get_height(), String::num(range.x, 3), String::num(range.y, 3)));
	}
}

String AsosdotTerrainEditor::_default_data_path(Node *p_node, const String &p_suffix) const {
	Node *scene_root = EditorNode::get_singleton()->get_edited_scene();
	const String scene_path = scene_root ? scene_root->get_scene_file_path() : String();
	const String node_name = String(p_node->get_name()).validate_filename().to_snake_case();
	// Beside the scene when it has been saved; otherwise at the project root,
	// named after the scene's root node the way Godot names new scene files.
	const String base = scene_path.is_empty()
			? "res://" + (scene_root ? String(scene_root->get_name()).validate_filename().to_snake_case() : String("level")) + "_" + node_name
			: scene_path.get_basename() + "_" + node_name;
	String path = base + p_suffix;
	for (int i = 2; FileAccess::exists(path) || ResourceCache::has(path); i++) {
		path = base + "_" + itos(i) + p_suffix;
	}
	return path;
}

bool AsosdotTerrainEditor::_store_external(const Ref<Resource> &p_res, const String &p_path) {
	if (p_res.is_null() || p_path.is_empty()) {
		return false;
	}
	const Error err = ResourceSaver::save(p_res, p_path, ResourceSaver::FLAG_COMPRESS);
	if (err != OK) {
		_show_message(vformat(TTR("Could not save terrain data to \"%s\"."), p_path));
		return false;
	}
	p_res->set_path(p_path, true);
	p_res->set_edited(false);
	EditorFileSystem::get_singleton()->update_file(p_path);
	return true;
}

void AsosdotTerrainEditor::_ensure_external(const Ref<Resource> &p_res, Node *p_node, const String &p_suffix) {
	if (p_res.is_null() || !p_res->is_built_in() || !p_node) {
		return;
	}
	if (_store_external(p_res, _default_data_path(p_node, p_suffix))) {
		// The scene now points at a file instead of holding the data, which
		// is a change to the scene that needs saving.
		EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
		ur->set_history_as_unsaved(ur->get_history_id_for_object(p_node));
		_update_info();
	}
}

/* -------------------------------------------------------------------------- */
/*                                   Layers                                   */
/* -------------------------------------------------------------------------- */

void AsosdotTerrainEditor::_refresh_layers() {
	layer_list->clear();
	const LayerOwner owner = _get_layer_owner();
	const int count = owner.count();
	layer_empty->set_visible(count == 0);
	if (!owner.resource) {
		layer_inspector->edit(nullptr);
		_update_layer_buttons();
		return;
	}
	for (int i = 0; i < count; i++) {
		const Ref<TerrainLayer> layer = owner.get(i);
		if (layer.is_null()) {
			continue;
		}
		Ref<Texture2D> icon = layer->get_albedo_texture();
		if (icon.is_null()) {
			icon = make_swatch(layer->get_albedo_color());
		}
		// The index is shown because foliage refers to layers by it.
		const int item = layer_list->add_item(vformat("%d  %s", i, layer->get_display_name()), icon);
		String tip = vformat(TTR("Layer %d: %s"), i, layer->get_display_name());
		if (!layer->get_path().is_empty() && !layer->is_built_in()) {
			tip += "\n" + layer->get_path();
		}
		layer_list->set_item_tooltip(item, tip);
	}
	if (count > 0) {
		settings.layer = CLAMP(settings.layer, 0, count - 1);
		layer_list->select(settings.layer);
	}
	_inspect_selected_layer();
	_update_layer_buttons();
}

void AsosdotTerrainEditor::_update_layer_buttons() {
	const int count = _get_layer_owner().count();
	const bool selected = count > 0 && settings.layer >= 0 && settings.layer < count;
	layer_duplicate_button->set_disabled(!selected || count >= TerrainData::MAX_LAYERS);
	layer_left_button->set_disabled(!selected || settings.layer == 0);
	layer_right_button->set_disabled(!selected || settings.layer >= count - 1);
	layer_save_button->set_disabled(!selected);
	layer_remove_button->set_disabled(!selected);
}

void AsosdotTerrainEditor::_inspect_selected_layer() {
	const LayerOwner owner = _get_layer_owner();
	TerrainLayer *layer = nullptr;
	if (settings.layer >= 0 && settings.layer < owner.count()) {
		layer = owner.get(settings.layer).ptr();
	}
	// edit() does nothing for the object already shown, so tweaking a slider,
	// which refreshes the list, does not reset the inspector under the pointer.
	layer_inspector->edit(layer);
}

void AsosdotTerrainEditor::_layer_selected(int p_index) {
	settings.layer = p_index;
	_inspect_selected_layer();
	_update_layer_buttons();
	if (tool != TerrainBrush::TOOL_PAINT && tool_buttons[TerrainBrush::TOOL_PAINT] && !tool_buttons[TerrainBrush::TOOL_PAINT]->is_disabled()) {
		// Choosing a layer is choosing to paint with it.
		tool_buttons[TerrainBrush::TOOL_PAINT]->set_pressed(true);
	}
}

void AsosdotTerrainEditor::_layer_add_option(int p_option) {
	if (_is_voxel() && _get_voxel() && _get_voxel_data().is_null()) {
		_new_voxel_data();
	}
	if (!_get_layer_owner().resource) {
		_show_message(TTR("Create the terrain first: Terrain > New Terrain..."));
		return;
	}
	switch (p_option) {
		case LAYER_NEW: {
			Ref<TerrainLayer> layer;
			layer.instantiate();
			_add_layers({ layer });
		} break;
		case LAYER_FROM_TEXTURES: {
			layer_textures_dialog->popup_file_dialog();
		} break;
		case LAYER_LOAD: {
			layer_load_dialog->popup_file_dialog();
		} break;
	}
}

void AsosdotTerrainEditor::_add_layers(const Vector<Ref<TerrainLayer>> &p_layers) {
	const LayerOwner owner = _get_layer_owner();
	if (!owner.resource || p_layers.is_empty()) {
		return;
	}
	const int base = owner.count();
	const int room = TerrainData::MAX_LAYERS - base;
	if (room <= 0) {
		_show_message(vformat(TTR("A terrain can have at most %d layers."), TerrainData::MAX_LAYERS));
		return;
	}
	const int count = MIN(room, p_layers.size());
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(count == 1 ? TTR("Add Terrain Layer") : TTR("Add Terrain Layers"), UndoRedo::MERGE_DISABLE, owner.node);
	for (int i = 0; i < count; i++) {
		ur->add_do_method(owner.resource, "add_layer", p_layers[i]);
		ur->add_do_reference(p_layers[i].ptr());
	}
	for (int i = count - 1; i >= 0; i--) {
		ur->add_undo_method(owner.resource, "remove_layer", base + i);
	}
	ur->commit_action();
	settings.layer = base;
	_refresh_layers();
	if (count < p_layers.size()) {
		_show_message(vformat(TTR("Only %d of the layers were added: a terrain can have at most %d."), count, TerrainData::MAX_LAYERS));
	}
}

namespace {

// Texture sets name their maps with a suffix: rock_diff_2k, rock_nor_gl_2k,
// rock_rough_2k. Splitting the name into words and taking out the ones that
// name a map or a resolution leaves the material's name, which pairs a
// colour texture with its normal map.
enum TextureRole {
	ROLE_ALBEDO,
	ROLE_NORMAL,
	ROLE_OTHER,
};

TextureRole texture_role(const String &p_path, String &r_base) {
	static const char *normal_words[] = { "normal", "normals", "nrm", "norm", "nor", "nml", "gl", "dx", "opengl", "directx" };
	static const char *albedo_words[] = { "albedo", "diffuse", "diff", "basecolor", "base", "color", "colour", "col", "alb", "d", "a" };
	static const char *other_words[] = { "rough", "roughness", "rgh", "ao", "occlusion", "ambientocclusion", "arm", "orm", "height", "disp", "displacement", "bump", "metal", "metallic", "metalness", "spec", "specular", "gloss", "mask", "emission", "emissive", "opacity" };

	const String name = p_path.get_file().get_basename().to_lower();
	Vector<String> words;
	String word;
	for (int i = 0; i < name.length(); i++) {
		const char32_t c = name[i];
		if (c == '_' || c == '-' || c == ' ' || c == '.') {
			if (!word.is_empty()) {
				words.push_back(word);
			}
			word = String();
		} else {
			word += c;
		}
	}
	if (!word.is_empty()) {
		words.push_back(word);
	}

	const auto is_one_of = [](const String &p_word, const char *const *p_list, int p_count) {
		for (int i = 0; i < p_count; i++) {
			if (p_word == p_list[i]) {
				return true;
			}
		}
		return false;
	};
	const auto is_resolution = [](const String &p_word) {
		return p_word.length() <= 3 && p_word.ends_with("k") && p_word.trim_suffix("k").is_valid_int();
	};

	TextureRole role = ROLE_ALBEDO;
	Vector<String> kept;
	for (int i = 0; i < words.size(); i++) {
		const String &w = words[i];
		// Single letters only count as tags at the end of a name ("rock_n"),
		// where they cannot be part of the material's own name.
		const bool last = i == words.size() - 1 || (i == words.size() - 2 && is_resolution(words[i + 1]));
		if (is_one_of(w, normal_words, std::size(normal_words)) || (w == "n" && last)) {
			if (w != "gl" && w != "dx" && w != "opengl" && w != "directx") {
				role = ROLE_NORMAL;
			}
			continue;
		}
		if (is_one_of(w, other_words, std::size(other_words))) {
			role = role == ROLE_NORMAL ? ROLE_NORMAL : ROLE_OTHER;
			continue;
		}
		if (is_one_of(w, albedo_words, std::size(albedo_words)) && (w.length() > 1 || last)) {
			continue;
		}
		if (is_resolution(w)) {
			continue;
		}
		kept.push_back(w);
	}
	r_base = String("_").join(kept);
	if (r_base.is_empty()) {
		r_base = name;
	}
	return role;
}

} // namespace

void AsosdotTerrainEditor::_add_layers_from_files(const Vector<String> &p_paths) {
	// Group by material name first, so dropping a whole texture set makes one
	// layer with its colour and normal map rather than one layer per file.
	HashMap<String, String> albedo_for;
	HashMap<String, String> normal_for;
	Vector<String> order;
	Vector<Ref<TerrainLayer>> layers;
	PackedStringArray skipped;
	for (const String &path : p_paths) {
		const String type = ResourceLoader::get_resource_type(path);
		if (type == "TerrainLayer") {
			const Ref<TerrainLayer> layer = ResourceLoader::load(path);
			if (layer.is_valid()) {
				layers.push_back(layer);
			}
			continue;
		}
		if (!ClassDB::is_parent_class(type, "Texture2D")) {
			skipped.push_back(path.get_file());
			continue;
		}
		String base;
		const TextureRole role = texture_role(path, base);
		if (role == ROLE_OTHER) {
			skipped.push_back(path.get_file());
			continue;
		}
		if (!order.has(base)) {
			order.push_back(base);
		}
		(role == ROLE_NORMAL ? normal_for : albedo_for)[base] = path;
	}
	for (const String &base : order) {
		Ref<TerrainLayer> layer;
		layer.instantiate();
		if (albedo_for.has(base)) {
			layer->set_albedo_texture(ResourceLoader::load(albedo_for[base]));
		}
		if (normal_for.has(base)) {
			layer->set_normal_texture(ResourceLoader::load(normal_for[base]));
		}
		layer->set_surface_name(base.replace("_", " ").capitalize());
		layers.push_back(layer);
	}
	_add_layers(layers);
	if (!skipped.is_empty()) {
		_show_message(vformat(TTR("Terrain layers use a colour texture and a normal map, so these were not used:\n%s"), String("\n").join(skipped)));
	}
}

void AsosdotTerrainEditor::_layer_load_selected(const Vector<String> &p_paths) {
	_add_layers_from_files(p_paths);
}

void AsosdotTerrainEditor::_duplicate_layer() {
	const LayerOwner owner = _get_layer_owner();
	if (settings.layer < 0 || settings.layer >= owner.count()) {
		return;
	}
	const Ref<TerrainLayer> copy = owner.get(settings.layer)->duplicate();
	_add_layers({ copy });
}

void AsosdotTerrainEditor::_move_layer(int p_direction) {
	const LayerOwner owner = _get_layer_owner();
	if (!owner.resource) {
		return;
	}
	const int from = settings.layer;
	const int to = from + p_direction;
	if (from < 0 || to < 0 || to >= owner.count()) {
		return;
	}
	// A move is a pure reordering, so moving back undoes it exactly, painted
	// weights and all.
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Move Terrain Layer"), UndoRedo::MERGE_DISABLE, owner.node);
	ur->add_do_method(owner.resource, "move_layer", from, to);
	ur->add_undo_method(owner.resource, "move_layer", to, from);
	ur->commit_action();
	settings.layer = to;
	_refresh_layers();
}

void AsosdotTerrainEditor::_remove_layer() {
	const LayerOwner owner = _get_layer_owner();
	if (!owner.resource || settings.layer < 0 || settings.layer >= owner.count()) {
		return;
	}
	// Removing a layer rewrites every region's splat maps, or every block's
	// voxels, so the undo keeps a copy of all of them, not just the layers.
	const Array layers_before = owner.resource->call(SNAME("_get_layers"));
	const Array paint_before = owner.resource->call(SNAME("_get_layers_snapshot"));
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Remove Terrain Layer"), UndoRedo::MERGE_DISABLE, owner.node);
	ur->add_do_method(owner.resource, "remove_layer", settings.layer);
	ur->add_undo_method(owner.resource, "_apply_layers_snapshot", layers_before, paint_before);
	ur->commit_action();
	_refresh_layers();
}

void AsosdotTerrainEditor::_save_selected(bool p_foliage) {
	saving_resource.unref();
	if (p_foliage) {
		const Foliage3D *foliage = _get_foliage();
		const Ref<FoliageData> fd = foliage ? foliage->get_data() : Ref<FoliageData>();
		if (fd.is_valid() && settings.foliage_type >= 0 && settings.foliage_type < fd->get_type_count()) {
			saving_resource = fd->get_type(settings.foliage_type);
		}
	} else {
		const LayerOwner owner = _get_layer_owner();
		if (settings.layer >= 0 && settings.layer < owner.count()) {
			saving_resource = owner.get(settings.layer);
		}
	}
	if (saving_resource.is_null()) {
		return;
	}
	String hint = saving_resource->get_path();
	if (hint.is_empty() || saving_resource->is_built_in()) {
		const String name = p_foliage ? foliage_display_name(saving_resource, settings.foliage_type) : Ref<TerrainLayer>(saving_resource)->get_display_name();
		hint = name.validate_filename().to_snake_case() + ".tres";
	}
	resource_save_dialog->set_title(p_foliage ? TTR("Save Foliage Type") : TTR("Save Terrain Layer"));
	resource_save_dialog->set_current_path(hint);
	resource_save_dialog->popup_file_dialog();
}

void AsosdotTerrainEditor::_resource_save_selected(const String &p_path) {
	if (saving_resource.is_null()) {
		return;
	}
	// Saved to its own file, the layer or type can be shared by other
	// terrains and scenes; the data now refers to the file instead of holding
	// its own copy.
	if (ResourceSaver::save(saving_resource, p_path) != OK) {
		_show_message(vformat(TTR("Could not save to \"%s\"."), p_path));
		return;
	}
	saving_resource->set_path(p_path, true);
	EditorFileSystem::get_singleton()->update_file(p_path);
	const LayerOwner owner = _get_layer_owner();
	if (owner.resource) {
		owner.resource->set_edited(true);
	}
	const Foliage3D *foliage = _get_foliage();
	if (foliage && foliage->get_data().is_valid()) {
		foliage->get_data()->set_edited(true);
	}
	saving_resource.unref();
	_refresh_layers();
	_refresh_foliage();
}

bool AsosdotTerrainEditor::_can_drop_layer(const Point2 &p_point, const Variant &p_data) const {
	const Dictionary d = p_data;
	if (String(d.get("type", "")) != "files" || !_get_layer_owner().resource) {
		return false;
	}
	const Vector<String> files = d["files"];
	for (const String &file : files) {
		const String type = ResourceLoader::get_resource_type(file);
		if (ClassDB::is_parent_class(type, "Texture2D") || type == "TerrainLayer") {
			return true;
		}
	}
	return false;
}

void AsosdotTerrainEditor::_drop_layer(const Point2 &p_point, const Variant &p_data) {
	const Dictionary d = p_data;
	_add_layers_from_files(d["files"]);
}

/* -------------------------------------------------------------------------- */
/*                                   Foliage                                  */
/* -------------------------------------------------------------------------- */

bool AsosdotTerrainEditor::_check_ground() {
	if (_is_voxel()) {
		if (_get_voxel() && _get_voxel_data().is_null()) {
			_new_voxel_data();
		}
		return _get_voxel_data().is_valid();
	}
	if (!_get_terrain() || _get_data().is_null()) {
		_show_message(TTR("Create the terrain first: Terrain > New Terrain..."));
		return false;
	}
	return true;
}

Foliage3D *AsosdotTerrainEditor::_ensure_foliage() {
	Foliage3D *foliage = _get_foliage();
	Node3D *terrain = _get_edited_node();
	if (foliage || !terrain) {
		if (foliage && foliage->get_data().is_null()) {
			Ref<FoliageData> fd;
			fd.instantiate();
			_store_external(fd, _default_data_path(foliage, ".foliage.res"));
			EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
			ur->create_action(TTR("Create Foliage Data"), UndoRedo::MERGE_DISABLE, foliage);
			ur->add_do_method(foliage, "set_data", fd);
			ur->add_undo_method(foliage, "set_data", Ref<FoliageData>());
			ur->add_do_reference(fd.ptr());
			ur->commit_action();
		}
		return foliage;
	}
	Node *scene_root = EditorNode::get_singleton()->get_edited_scene();
	foliage = memnew(Foliage3D);
	foliage->set_name("Foliage3D");
	Ref<FoliageData> fd;
	fd.instantiate();
	_store_external(fd, _default_data_path(foliage, ".foliage.res"));
	foliage->set_data(fd);

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Add Foliage3D"), UndoRedo::MERGE_DISABLE, terrain);
	ur->add_do_method(terrain, "add_child", foliage, true);
	ur->add_do_method(foliage, "set_owner", scene_root);
	ur->add_do_reference(foliage);
	ur->add_undo_method(terrain, "remove_child", foliage);
	ur->commit_action();
	foliage_id = foliage->get_instance_id();
	_watch();
	return foliage;
}

void AsosdotTerrainEditor::_refresh_foliage() {
	foliage_list->clear();
	const Foliage3D *foliage = _get_foliage();
	const Ref<FoliageData> fd = foliage ? foliage->get_data() : Ref<FoliageData>();
	foliage_empty->set_visible(fd.is_null() || fd->get_type_count() == 0);
	const bool any = fd.is_valid() && fd->get_type_count() > 0;
	foliage_duplicate_button->set_disabled(!any);
	foliage_save_button->set_disabled(!any);
	foliage_remove_button->set_disabled(!any);
	if (fd.is_null()) {
		foliage_inspector->edit(nullptr);
		return;
	}
	for (int i = 0; i < fd->get_type_count(); i++) {
		const Ref<FoliageType> type = fd->get_type(i);
		const bool grass = type.is_valid() && type->get_kind() == FoliageType::KIND_GRASS;
		const int item = foliage_list->add_item(foliage_display_name(type, i), get_editor_theme_icon(grass ? SNAME("TerrainGrass") : SNAME("TerrainObjects")));
		String tip = grass ? vformat(TTR("Grass, %s blades/m² at full density"), String::num(type->get_density(), 1)) : vformat(TTR("Object, %d placed"), fd->get_instance_count(i));
		foliage_list->set_item_tooltip(item, tip);
		if (type.is_valid()) {
			Ref<Resource> preview_source = type->get_scene().is_valid() ? Ref<Resource>(type->get_scene()) : Ref<Resource>(type->get_mesh());
			if (preview_source.is_valid()) {
				EditorResourcePreview::get_singleton()->queue_edited_resource_preview(preview_source, callable_mp(this, &AsosdotTerrainEditor::_foliage_preview_ready).bind(i));
			}
		}
	}
	if (fd->get_type_count() > 0) {
		settings.foliage_type = CLAMP(settings.foliage_type, 0, fd->get_type_count() - 1);
		foliage_list->select(settings.foliage_type);
	}
	_inspect_selected_foliage();
}

void AsosdotTerrainEditor::_inspect_selected_foliage() {
	const Foliage3D *foliage = _get_foliage();
	const Ref<FoliageData> fd = foliage ? foliage->get_data() : Ref<FoliageData>();
	FoliageType *type = nullptr;
	if (fd.is_valid() && settings.foliage_type >= 0 && settings.foliage_type < fd->get_type_count()) {
		type = fd->get_type(settings.foliage_type).ptr();
	}
	foliage_inspector->edit(type);
}

void AsosdotTerrainEditor::_duplicate_foliage_type() {
	const Foliage3D *foliage = _get_foliage();
	const Ref<FoliageData> fd = foliage ? foliage->get_data() : Ref<FoliageData>();
	if (fd.is_null() || settings.foliage_type < 0 || settings.foliage_type >= fd->get_type_count()) {
		return;
	}
	_add_foliage_type(fd->get_type(settings.foliage_type)->duplicate());
}

void AsosdotTerrainEditor::_foliage_preview_ready(const String &p_path, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_small, int p_index) {
	if (p_preview.is_valid() && p_index < foliage_list->get_item_count()) {
		foliage_list->set_item_icon(p_index, p_preview);
	}
}

void AsosdotTerrainEditor::_foliage_selected(int p_index) {
	if (p_index == -2) {
		// A type's settings changed; its name or preview may have too.
		_refresh_foliage();
		return;
	}
	settings.foliage_type = p_index;
	_inspect_selected_foliage();
	const Foliage3D *foliage = _get_foliage();
	const Ref<FoliageData> fd = foliage ? foliage->get_data() : Ref<FoliageData>();
	if (fd.is_null() || p_index < 0 || p_index >= fd->get_type_count()) {
		return;
	}
	const Ref<FoliageType> type = fd->get_type(p_index);
	const TerrainBrush::Tool wanted = (type.is_valid() && type->get_kind() == FoliageType::KIND_OBJECT) ? TerrainBrush::TOOL_OBJECTS : TerrainBrush::TOOL_GRASS;
	if (tool != wanted && tool_buttons[wanted] && !tool_buttons[wanted]->is_disabled()) {
		tool_buttons[wanted]->set_pressed(true);
	}
}

void AsosdotTerrainEditor::_add_foliage_type(const Ref<FoliageType> &p_type) {
	if (!_check_ground()) {
		return;
	}
	Foliage3D *foliage = _ensure_foliage();
	if (!foliage) {
		return;
	}
	const Ref<FoliageData> fd = foliage->get_data();
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Add Foliage Type"), UndoRedo::MERGE_DISABLE, foliage);
	ur->add_do_method(fd.ptr(), "add_type", p_type);
	ur->add_undo_method(fd.ptr(), "remove_type", fd->get_type_count());
	ur->add_do_reference(p_type.ptr());
	ur->commit_action();
	settings.foliage_type = fd->get_type_count() - 1;
	_refresh_foliage();
	_foliage_selected(settings.foliage_type);
}

Ref<FoliageType> AsosdotTerrainEditor::_make_foliage_type(const Ref<Resource> &p_resource, FoliageType::Kind p_kind, String &r_error) const {
	// A saved type already says what it is.
	const Ref<FoliageType> saved = p_resource;
	if (saved.is_valid()) {
		return saved;
	}

	Ref<Mesh> mesh = p_resource;
	const Ref<PackedScene> scene = p_resource;
	if (mesh.is_null() && scene.is_null()) {
		r_error = vformat(TTR("\"%s\" is not a mesh, a scene or a FoliageType."), p_resource.is_valid() ? p_resource->get_path().get_file() : String());
		return Ref<FoliageType>();
	}
	if (mesh.is_null() && p_kind == FoliageType::KIND_GRASS) {
		// Grass is drawn as a MultiMesh, so it takes a mesh; a scene such as
		// an imported .glb gives up the first one it holds.
		Node *instance = scene->instantiate();
		if (instance) {
			TypedArray<Node> found = instance->find_children("*", "MeshInstance3D", true, false);
			MeshInstance3D *first = Object::cast_to<MeshInstance3D>(instance);
			if (!first && !found.is_empty()) {
				first = Object::cast_to<MeshInstance3D>(found[0]);
			}
			if (first) {
				mesh = first->get_mesh();
			}
			memdelete(instance);
		}
		if (mesh.is_null()) {
			r_error = vformat(TTR("\"%s\" has no mesh in it to grow as grass."), scene->get_path().get_file());
			return Ref<FoliageType>();
		}
	}

	Ref<FoliageType> type;
	type.instantiate();
	type->set_kind(p_kind);
	if (mesh.is_valid()) {
		type->set_mesh(mesh);
	} else {
		type->set_scene(scene);
	}
	if (p_kind == FoliageType::KIND_OBJECT) {
		// Trees stand up straight and are seen from far off; grass leans with
		// the ground and is only grown near the camera.
		type->set_align_to_normal(0.0);
		type->set_cast_shadow(true);
		type->set_visibility_range(300.0);
		type->set_slope_max(40.0);
	}
	return type;
}

void AsosdotTerrainEditor::_add_foliage_pressed() {
	if (!_check_ground()) {
		return;
	}
	foliage_add_dialog->popup_file_dialog();
}

void AsosdotTerrainEditor::_foliage_files_selected(const Vector<String> &p_paths) {
	const Dictionary options = foliage_add_dialog->get_selected_options();
	const FoliageType::Kind kind = int(options.get(TTR("Paint As"), 0)) == 1 ? FoliageType::KIND_GRASS : FoliageType::KIND_OBJECT;
	PackedStringArray errors;
	Ref<FoliageType> last;
	for (const String &path : p_paths) {
		String error;
		const Ref<FoliageType> type = _make_foliage_type(ResourceLoader::load(path), kind, error);
		if (type.is_null()) {
			errors.push_back(error.is_empty() ? vformat(TTR("Could not load \"%s\"."), path.get_file()) : error);
			continue;
		}
		_add_foliage_type(type);
		last = type;
	}
	if (!errors.is_empty()) {
		_show_message(String("\n").join(errors));
	}
}

void AsosdotTerrainEditor::_remove_foliage_type() {
	Foliage3D *foliage = _get_foliage();
	const Ref<FoliageData> fd = foliage ? foliage->get_data() : Ref<FoliageData>();
	const Vector<int> selected = foliage_list->get_selected_items();
	if (fd.is_null() || selected.is_empty()) {
		return;
	}
	const Array types_before = fd->call(SNAME("_get_types"));
	const Array data_before = fd->call(SNAME("_get_all_snapshot"));
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Remove Foliage Type"), UndoRedo::MERGE_DISABLE, foliage);
	ur->add_do_method(fd.ptr(), "remove_type", selected[0]);
	ur->add_undo_method(fd.ptr(), "_apply_all_snapshot", types_before, data_before);
	ur->commit_action();
	_refresh_foliage();
}

bool AsosdotTerrainEditor::_can_drop_foliage(const Point2 &p_point, const Variant &p_data) const {
	const Dictionary d = p_data;
	if (String(d.get("type", "")) != "files" || !_get_edited_node()) {
		return false;
	}
	const Vector<String> files = d["files"];
	for (const String &file : files) {
		const String type = ResourceLoader::get_resource_type(file);
		if (!ClassDB::is_parent_class(type, "Mesh") && type != "PackedScene" && type != "FoliageType") {
			return false;
		}
	}
	return !files.is_empty();
}

void AsosdotTerrainEditor::_drop_foliage(const Point2 &p_point, const Variant &p_data) {
	const Dictionary d = p_data;
	const Vector<String> files = d["files"];
	for (const String &file : files) {
		// Dropped meshes and scenes are placed as objects, since there is no
		// dialog to ask; switch Kind to Grass in the Inspector to paint one as
		// grass instead, or use Add... to choose up front.
		String error;
		const Ref<FoliageType> type = _make_foliage_type(ResourceLoader::load(file), FoliageType::KIND_OBJECT, error);
		if (type.is_null()) {
			_show_message(error);
			continue;
		}
		_add_foliage_type(type);
	}
}

/* -------------------------------------------------------------------------- */
/*                                   Strokes                                  */
/* -------------------------------------------------------------------------- */

bool AsosdotTerrainEditor::_pick(Camera3D *p_camera, const Point2 &p_point, Vector3 &r_hit) const {
	const Vector3 from = p_camera->project_ray_origin(p_point);
	const Vector3 dir = p_camera->project_ray_normal(p_point);
	if (const VoxelTerrain3D *voxel = _get_voxel()) {
		return voxel->get_data().is_valid() && voxel->intersect_ray_fast(from, dir, p_camera->get_far(), r_hit);
	}
	const Terrain3D *terrain = _get_terrain();
	if (!terrain) {
		return false;
	}
	if (terrain->intersect_ray_fast(from, dir, p_camera->get_far(), r_hit)) {
		return true;
	}
	if (tool == TerrainBrush::TOOL_REGIONS) {
		// Adding a region means pointing at empty space, so fall back to the
		// plane the new region would sit on.
		const Plane plane(Vector3(0, 1, 0), terrain->get_global_position().y + settings.flatten_height);
		Vector3 hit;
		if (plane.intersects_ray(from, dir, &hit)) {
			r_hit = hit;
			return true;
		}
	}
	return false;
}

void AsosdotTerrainEditor::_snapshot(const Rect2i &p_rect, uint32_t p_maps) {
	const Ref<TerrainData> tdata = _get_data();
	if (tdata.is_null()) {
		return;
	}
	const Vector2i r0 = tdata->vertex_to_region(p_rect.position);
	const Vector2i r1 = tdata->vertex_to_region(p_rect.get_end() - Vector2i(1, 1));
	for (int rz = r0.y; rz <= r1.y; rz++) {
		for (int rx = r0.x; rx <= r1.x; rx++) {
			const Vector2i coord(rx, rz);
			if (stroke_before.has(coord) || (!tdata->has_region(coord) && p_maps != TerrainData::MAP_ALL)) {
				continue;
			}
			stroke_before.insert(coord, tdata->call(SNAME("_get_snapshot"), coord, (int)p_maps));
		}
	}
	stroke_maps |= p_maps;
}

void AsosdotTerrainEditor::_begin_stroke() {
	if (_is_voxel()) {
		VoxelTerrain3D *voxel = _get_voxel();
		if (!voxel) {
			return;
		}
		if (voxel->get_data().is_null()) {
			_new_voxel_data();
		}
		_ensure_external(voxel->get_data(), voxel, ".voxel.res");
		if (TerrainBrush::is_foliage(tool) && _get_foliage()) {
			_ensure_external(_get_foliage()->get_data(), _get_foliage(), ".foliage.res");
		}
		stroking = true;
		voxel_stroke_before.clear();
		foliage_before = Dictionary();
		foliage_before_type = -1;
		stroke_seed = Math::rand();
		dab_counter = 0;
		if (tool == TerrainBrush::TOOL_FLATTEN && !flatten_fixed->is_pressed()) {
			settings.flatten_height = cursor.y - voxel->get_global_position().y;
			flatten_height->set_value_no_signal(settings.flatten_height);
		}
		last_dab = cursor;
		_voxel_dab(cursor, 1.0 / 60.0);
		return;
	}
	Terrain3D *terrain = _get_terrain();
	if (!terrain || (_get_data().is_null() && tool != TerrainBrush::TOOL_REGIONS)) {
		return;
	}
	if (_get_data().is_null()) {
		// Regions on a terrain with no data: give it some first.
		Ref<TerrainData> tdata;
		tdata.instantiate();
		_store_external(tdata, _default_data_path(terrain, ".terrain.res"));
		EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
		ur->create_action(TTR("Create Terrain Data"), UndoRedo::MERGE_DISABLE, terrain);
		ur->add_do_method(terrain, "set_data", tdata);
		ur->add_undo_method(terrain, "set_data", Ref<TerrainData>());
		ur->add_do_reference(tdata.ptr());
		ur->commit_action();
		_watch();
	}
	_ensure_external(_get_data(), terrain, ".terrain.res");
	if (TerrainBrush::is_foliage(tool) && _get_foliage()) {
		_ensure_external(_get_foliage()->get_data(), _get_foliage(), ".foliage.res");
	}
	stroking = true;
	stroke_before.clear();
	stroke_maps = 0;
	foliage_before = Dictionary();
	foliage_before_type = -1;
	stroke_seed = Math::rand();
	dab_counter = 0;
	terrain->set_collision_update_immediate(false);

	const Vector3 origin = terrain->get_global_position();
	if (tool == TerrainBrush::TOOL_FLATTEN && !flatten_fixed->is_pressed()) {
		settings.flatten_height = cursor.y - origin.y;
		flatten_height->set_value_no_signal(settings.flatten_height);
	}
	if (tool == TerrainBrush::TOOL_RAMP) {
		ramp_start = cursor;
		terrain->set_brush_segment(true, ramp_start);
		return;
	}
	if (tool == TerrainBrush::TOOL_REGIONS) {
		settings.invert = Input::get_singleton()->is_key_pressed(Key::SHIFT);
		const Ref<TerrainData> tdata = _get_data();
		const Vector2 local = Vector2(cursor.x - origin.x, cursor.z - origin.z) / terrain->get_vertex_spacing();
		const Vector2i vertex((int)Math::floor(local.x), (int)Math::floor(local.y));
		_snapshot(Rect2i(vertex, Size2i(1, 1)), TerrainData::MAP_ALL);
		TerrainBrush::apply(terrain, _get_foliage(), tool, Vector2(cursor.x - origin.x, cursor.z - origin.z), settings, 0.0, stroke_seed);
		_end_stroke();
		return;
	}
	last_dab = cursor;
	_dab(cursor, 1.0 / 60.0);
}

void AsosdotTerrainEditor::_dab(const Vector3 &p_at, float p_delta) {
	if (_is_voxel()) {
		_voxel_dab(p_at, p_delta);
		return;
	}
	Terrain3D *terrain = _get_terrain();
	const Ref<TerrainData> tdata = _get_data();
	if (!terrain || tdata.is_null()) {
		return;
	}
	settings.invert = Input::get_singleton()->is_key_pressed(Key::SHIFT);
	const Vector3 origin = terrain->get_global_position();
	const Vector2 local(p_at.x - origin.x, p_at.z - origin.z);

	Foliage3D *foliage = nullptr;
	if (TerrainBrush::is_foliage(tool)) {
		foliage = _get_foliage();
		const Ref<FoliageData> fd = foliage ? foliage->get_data() : Ref<FoliageData>();
		if (fd.is_null() || settings.foliage_type < 0 || settings.foliage_type >= fd->get_type_count()) {
			return;
		}
		if (foliage_before_type < 0) {
			foliage_before_type = settings.foliage_type;
			foliage_before = fd->call(SNAME("_get_type_snapshot"), foliage_before_type);
		}
	} else {
		_snapshot(TerrainBrush::get_dab_rect(terrain, tool, local, settings), TerrainBrush::maps_for(tool));
	}
	TerrainBrush::apply(terrain, foliage, tool, local, settings, p_delta, stroke_seed + dab_counter++ * 7919);
}

void AsosdotTerrainEditor::_end_stroke(bool p_cancel) {
	if (!stroking) {
		return;
	}
	stroking = false;
	if (_is_voxel()) {
		_end_voxel_stroke(p_cancel);
		return;
	}
	Terrain3D *terrain = _get_terrain();
	const Ref<TerrainData> tdata = _get_data();
	if (!terrain || tdata.is_null()) {
		stroke_before.clear();
		return;
	}

	const Vector3 origin = terrain->get_global_position();
	if (tool == TerrainBrush::TOOL_RAMP) {
		terrain->set_brush_segment(false);
		if (!p_cancel && ramp_start.distance_to(cursor) > terrain->get_vertex_spacing()) {
			const Vector3 from = ramp_start - origin;
			const Vector3 to = cursor - origin;
			_snapshot(TerrainBrush::get_ramp_rect(terrain, from, to, settings), TerrainData::MAP_HEIGHT);
			TerrainBrush::apply_ramp(terrain, from, to, settings);
		}
	}

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	if (!stroke_before.is_empty()) {
		Array before;
		Array after;
		for (const KeyValue<Vector2i, Dictionary> &E : stroke_before) {
			before.push_back(E.value);
			after.push_back(tdata->call(SNAME("_get_snapshot"), E.key, (int)E.value["maps"]));
		}
		if (p_cancel) {
			tdata->call(SNAME("_apply_snapshots"), before);
		} else {
			ur->create_action(vformat(TTR("Terrain: %s"), TTRGET(TerrainBrush::get_tool_name(tool))), UndoRedo::MERGE_DISABLE, terrain);
			ur->add_do_method(tdata.ptr(), "_apply_snapshots", after);
			ur->add_undo_method(tdata.ptr(), "_apply_snapshots", before);
			ur->commit_action(false);
#ifdef TOOLS_ENABLED
			tdata->set_edited(true);
#endif
		}
	}
	stroke_before.clear();
	_end_foliage_stroke(p_cancel);

	// Collision was held back during the stroke; let it catch up now.
	terrain->set_collision_update_immediate(true);
	_update_info();
}

void AsosdotTerrainEditor::_voxel_snapshot(const AABB &p_voxel_box) {
	const Ref<VoxelTerrainData> vdata = _get_voxel_data();
	if (vdata.is_null()) {
		return;
	}
	const Vector3i b0 = VoxelTerrainData::voxel_to_block(Vector3i(p_voxel_box.position.floor()));
	const Vector3i b1 = VoxelTerrainData::voxel_to_block(Vector3i(p_voxel_box.get_end().ceil()));
	for (int z = b0.z; z <= b1.z; z++) {
		for (int y = b0.y; y <= b1.y; y++) {
			for (int x = b0.x; x <= b1.x; x++) {
				const Vector3i block(x, y, z);
				if (!voxel_stroke_before.has(block)) {
					voxel_stroke_before.insert(block, vdata->call(SNAME("_get_snapshot"), block));
				}
			}
		}
	}
}

void AsosdotTerrainEditor::_voxel_dab(const Vector3 &p_at, float p_delta) {
	VoxelTerrain3D *voxel = _get_voxel();
	const Ref<VoxelTerrainData> vdata = _get_voxel_data();
	if (!voxel || vdata.is_null()) {
		return;
	}
	const bool invert = Input::get_singleton()->is_key_pressed(Key::SHIFT);
	const float vs = vdata->get_voxel_size();
	const Vector3 center = voxel->global_to_voxel(p_at);
	const float radius = MAX(settings.radius / vs, 0.75f);
	const int layer = settings.layer < vdata->get_layer_count() ? settings.layer : -1;
	// Adding and digging move the surface by up to this many voxels a
	// second, faster for a bigger brush so it fills in the same time.
	const float move = settings.strength * p_delta * MAX(radius, 2.0f) * 3.0f;
	const float blend = MIN(settings.strength * p_delta * 10.0f, 1.0f);

	if (TerrainBrush::is_foliage(tool)) {
		Foliage3D *foliage = _get_foliage();
		const Ref<FoliageData> fd = foliage ? foliage->get_data() : Ref<FoliageData>();
		if (fd.is_null() || settings.foliage_type < 0 || settings.foliage_type >= fd->get_type_count()) {
			return;
		}
		if (foliage_before_type < 0) {
			foliage_before_type = settings.foliage_type;
			foliage_before = fd->call(SNAME("_get_type_snapshot"), foliage_before_type);
		}
		settings.invert = invert;
		TerrainBrush::apply_foliage(foliage, tool, p_at - voxel->get_global_position(), settings, p_delta, stroke_seed + dab_counter++ * 7919);
		return;
	}

	VoxelTerrainData::Operation operation;
	float amount = move;
	int use_layer = -1;
	float height = 0.0;
	switch (tool) {
		case TerrainBrush::TOOL_RAISE:
		case TerrainBrush::TOOL_DIG: {
			const bool adding = (tool == TerrainBrush::TOOL_RAISE) != invert;
			operation = adding ? VoxelTerrainData::OPERATION_ADD : VoxelTerrainData::OPERATION_REMOVE;
			use_layer = adding ? layer : -1;
		} break;
		case TerrainBrush::TOOL_SMOOTH: {
			operation = VoxelTerrainData::OPERATION_SMOOTH;
			amount = blend;
		} break;
		case TerrainBrush::TOOL_FLATTEN: {
			operation = VoxelTerrainData::OPERATION_FLATTEN;
			amount = blend;
			height = settings.flatten_height / vs;
		} break;
		case TerrainBrush::TOOL_PAINT: {
			if (layer < 0) {
				return;
			}
			operation = invert ? VoxelTerrainData::OPERATION_ERASE_PAINT : VoxelTerrainData::OPERATION_PAINT;
			amount = blend;
			use_layer = layer;
		} break;
		default:
			return;
	}
	_voxel_snapshot(VoxelTerrainData::get_edit_bounds(operation, center, radius));
	(void)vdata->edit_sphere(operation, center, radius, amount, settings.hardness, use_layer, height, stroke_seed + dab_counter++ * 7919);
}

void AsosdotTerrainEditor::_end_voxel_stroke(bool p_cancel) {
	VoxelTerrain3D *voxel = _get_voxel();
	const Ref<VoxelTerrainData> vdata = _get_voxel_data();
	if (voxel && vdata.is_valid() && !voxel_stroke_before.is_empty()) {
		Array before;
		Array after;
		for (const KeyValue<Vector3i, Dictionary> &E : voxel_stroke_before) {
			before.push_back(E.value);
			after.push_back(vdata->call(SNAME("_get_snapshot"), E.key));
		}
		if (p_cancel) {
			vdata->call(SNAME("_apply_snapshots"), before);
		} else {
			EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
			ur->create_action(vformat(TTR("Voxel Terrain: %s"), TTRGET(TerrainBrush::get_tool_name(tool))), UndoRedo::MERGE_DISABLE, voxel);
			ur->add_do_method(vdata.ptr(), "_apply_snapshots", after);
			ur->add_undo_method(vdata.ptr(), "_apply_snapshots", before);
			ur->commit_action(false);
			vdata->set_edited(true);
		}
	}
	voxel_stroke_before.clear();
	_end_foliage_stroke(p_cancel);
	_update_info();
}

void AsosdotTerrainEditor::_end_foliage_stroke(bool p_cancel) {
	Foliage3D *foliage = _get_foliage();
	if (foliage_before_type >= 0 && foliage && foliage->get_data().is_valid()) {
		const Ref<FoliageData> fd = foliage->get_data();
		const Dictionary after = fd->call(SNAME("_get_type_snapshot"), foliage_before_type);
		if (p_cancel) {
			fd->call(SNAME("_apply_type_snapshot"), foliage_before_type, foliage_before);
		} else {
			EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
			ur->create_action(vformat(TTR("Terrain: %s"), TTRGET(TerrainBrush::get_tool_name(tool))), UndoRedo::MERGE_DISABLE, foliage);
			ur->add_do_method(fd.ptr(), "_apply_type_snapshot", foliage_before_type, after);
			ur->add_undo_method(fd.ptr(), "_apply_type_snapshot", foliage_before_type, foliage_before);
			ur->commit_action(false);
			fd->set_edited(true);
		}
	}
	foliage_before = Dictionary();
	foliage_before_type = -1;
}

/* -------------------------------------------------------------------------- */
/*                                    Input                                   */
/* -------------------------------------------------------------------------- */

EditorPlugin::AfterGUIInput AsosdotTerrainEditor::forward_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	Node3D *terrain = _get_edited_node();
	if (!terrain || tool == TerrainBrush::TOOL_NONE || !active) {
		return EditorPlugin::AFTER_GUI_INPUT_PASS;
	}
	// Captured means freelook; the brush has no business there.
	if (Input::get_singleton()->get_mouse_mode() == Input::MouseMode::MOUSE_MODE_CAPTURED) {
		return EditorPlugin::AFTER_GUI_INPUT_PASS;
	}
	last_camera = p_camera->get_instance_id();

	const Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed() && !key->is_echo()) {
		if (key->get_keycode() == Key::BRACKETLEFT) {
			size_slider->set_value(size_slider->get_value() / 1.2);
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		}
		if (key->get_keycode() == Key::BRACKETRIGHT) {
			size_slider->set_value(size_slider->get_value() * 1.2);
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		}
		if (key->get_keycode() == Key::ESCAPE && stroking) {
			_end_stroke(true);
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		}
		if (key->get_keycode() == Key::SHIFT) {
			_update_brush_preview();
		}
	}
	if (key.is_valid() && !key->is_pressed() && key->get_keycode() == Key::SHIFT) {
		_update_brush_preview();
	}

	const Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_valid()) {
		cursor_valid = _pick(p_camera, motion->get_position(), cursor);
		_update_brush_preview();
		return stroking ? EditorPlugin::AFTER_GUI_INPUT_STOP : EditorPlugin::AFTER_GUI_INPUT_PASS;
	}

	const Ref<InputEventMouseButton> button = p_event;
	if (button.is_valid() && button->get_button_index() == MouseButton::LEFT) {
		if (button->is_pressed()) {
			cursor_valid = _pick(p_camera, button->get_position(), cursor);
			if (cursor_valid) {
				if (tool == TerrainBrush::TOOL_FLATTEN && button->is_command_or_control_pressed()) {
					settings.flatten_height = cursor.y - terrain->get_global_position().y;
					flatten_height->set_value_no_signal(settings.flatten_height);
					flatten_fixed->set_pressed(true);
				} else {
					_begin_stroke();
				}
			}
		} else if (stroking) {
			_end_stroke();
		}
		// With a brush out, a click belongs to the brush even over empty space,
		// or it would select something else and put the brush away.
		return EditorPlugin::AFTER_GUI_INPUT_STOP;
	}
	return EditorPlugin::AFTER_GUI_INPUT_PASS;
}

void AsosdotTerrainEditor::draw_overlay(Control *p_overlay) {
	if (tool == TerrainBrush::TOOL_NONE || !_get_edited_node()) {
		return;
	}
	String text = TTRGET(TerrainBrush::get_tool_name(tool));
	if (_is_voxel() && tool == TerrainBrush::TOOL_RAISE) {
		text = TTR("Add");
	}
	if (tool != TerrainBrush::TOOL_REGIONS) {
		text += vformat("  |  %s m  |  %d%%", String::num(settings.radius, 1), (int)Math::round(settings.strength * 100.0));
	}
	const LayerOwner owner = _get_layer_owner();
	if ((tool == TerrainBrush::TOOL_PAINT || (_is_voxel() && tool == TerrainBrush::TOOL_RAISE)) && settings.layer < owner.count()) {
		text += "  |  " + owner.get(settings.layer)->get_display_name();
	}
	if (tool == TerrainBrush::TOOL_FLATTEN) {
		text += vformat("  |  %s m", String::num(settings.flatten_height, 2));
	}
	const Ref<Font> font = p_overlay->get_theme_font(SceneStringName(font), SNAME("Label"));
	const int font_size = p_overlay->get_theme_font_size(SceneStringName(font_size), SNAME("Label"));
	const Point2 pos(20 * EDSCALE, p_overlay->get_size().y - 20 * EDSCALE);
	p_overlay->draw_string(font, pos + Point2(1, 1), text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(0, 0, 0, 0.8));
	p_overlay->draw_string(font, pos, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(1, 1, 1));
}

void AsosdotTerrainEditor::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			for (Button *b : icon_buttons) {
				b->set_button_icon(get_editor_theme_icon(b->get_meta(SNAME("_icon"))));
			}
			for (const ToolInfo &info : SCULPT_TOOLS) {
				tool_buttons[info.tool]->set_button_icon(get_editor_theme_icon(info.icon));
			}
			for (const ToolInfo &info : PAINT_TOOLS) {
				tool_buttons[info.tool]->set_button_icon(get_editor_theme_icon(info.icon));
			}
		} break;

		case NOTIFICATION_PROCESS: {
			_watch();
			if (!stroking || !cursor_valid || tool == TerrainBrush::TOOL_RAMP || tool == TerrainBrush::TOOL_REGIONS) {
				break;
			}
			float spacing = 0.0;
			if (const Terrain3D *terrain = _get_terrain()) {
				spacing = terrain->get_vertex_spacing();
			} else if (_get_voxel_data().is_valid()) {
				spacing = _get_voxel_data()->get_voxel_size();
			} else {
				break;
			}
			// Dabs are laid down every frame while the button is held, and
			// filled in along the path when the pointer moves faster than the
			// brush can keep up, so a quick stroke leaves no gaps.
			const float delta = get_process_delta_time();
			const float step = MAX(settings.radius * 0.3f, spacing);
			const float dist = last_dab.distance_to(cursor);
			if (dist > step) {
				const int count = MIN((int)Math::ceil(dist / step), 16);
				for (int i = 1; i <= count; i++) {
					_dab(last_dab.lerp(cursor, float(i) / count), delta / count);
				}
			} else {
				_dab(cursor, delta);
			}
			last_dab = cursor;
		} break;
	}
}

/* -------------------------------------------------------------------------- */
/*                                Construction                                */
/* -------------------------------------------------------------------------- */

AsosdotTerrainEditor::AsosdotTerrainEditor() {
	set_name(TTRC("Terrain"));
	set_icon_name("TerrainDock");
	set_dock_shortcut(ED_SHORTCUT_AND_COMMAND("bottom_panels/toggle_terrain_bottom_panel", TTRC("Toggle Terrain Dock")));
	set_default_slot(EditorDock::DOCK_SLOT_BOTTOM);
	set_available_layouts(EditorDock::DOCK_LAYOUT_HORIZONTAL | EditorDock::DOCK_LAYOUT_FLOATING);
	set_global(false);
	set_transient(true);
	set_custom_minimum_size(Size2(0, 280 * EDSCALE));
	set_theme_type_variation("NoBorderBottomPanel");

	HBoxContainer *main = memnew(HBoxContainer);
	main->add_theme_constant_override("separation", 10 * EDSCALE);
	add_child(main);

	// Terrain menu and summary.
	VBoxContainer *left = memnew(VBoxContainer);
	left->set_custom_minimum_size(Size2(150, 0) * EDSCALE);
	main->add_child(left);
	HBoxContainer *menus = memnew(HBoxContainer);
	left->add_child(menus);
	terrain_menu = memnew(MenuButton);
	terrain_menu->set_text(TTR("Terrain"));
	terrain_menu->set_flat(false);
	terrain_menu->set_theme_type_variation("FlatMenuButton");
	terrain_menu->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &AsosdotTerrainEditor::_menu_option));
	menus->add_child(terrain_menu);
	view_menu = memnew(MenuButton);
	view_menu->set_text(TTR("View"));
	view_menu->set_theme_type_variation("FlatMenuButton");
	view_menu->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &AsosdotTerrainEditor::_view_option));
	menus->add_child(view_menu);
	_update_menus();
	info_label = memnew(Label);
	info_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	info_label->set_custom_minimum_size(Size2(150, 0) * EDSCALE);
	info_label->set_modulate(Color(1, 1, 1, 0.75));
	left->add_child(info_label);

	main->add_child(memnew(VSeparator));

	// Tools.
	tool_group.instantiate();
	tool_group->set_allow_unpress(true);
	VBoxContainer *tools = memnew(VBoxContainer);
	main->add_child(tools);
	const auto add_tools = [&](const String &p_title, const ToolInfo *p_infos, int p_count) {
		Label *title = memnew(Label(p_title));
		title->set_theme_type_variation("HeaderSmall");
		tools->add_child(title);
		GridContainer *grid = memnew(GridContainer);
		grid->set_columns(4);
		tools->add_child(grid);
		for (int i = 0; i < p_count; i++) {
			Button *b = memnew(Button);
			b->set_toggle_mode(true);
			b->set_button_group(tool_group);
			b->set_theme_type_variation(SceneStringName(FlatButton));
			b->set_tooltip_text(TTRGET(p_infos[i].tooltip));
			b->set_accessibility_name(TTRGET(TerrainBrush::get_tool_name(p_infos[i].tool)));
			b->connect(SceneStringName(toggled), callable_mp(this, &AsosdotTerrainEditor::_tool_toggled).bind((int)p_infos[i].tool));
			grid->add_child(b);
			tool_buttons[p_infos[i].tool] = b;
		}
	};
	add_tools(TTR("Sculpt"), SCULPT_TOOLS, std::size(SCULPT_TOOLS));
	add_tools(TTR("Paint"), PAINT_TOOLS, std::size(PAINT_TOOLS));

	main->add_child(memnew(VSeparator));

	// Brush settings.
	VBoxContainer *brush = memnew(VBoxContainer);
	brush->set_custom_minimum_size(Size2(240, 0) * EDSCALE);
	main->add_child(brush);
	const auto make_slider = [&](const String &p_label, double p_min, double p_max, double p_step, double p_value, const String &p_suffix, bool p_exp) {
		EditorSpinSlider *s = memnew(EditorSpinSlider);
		s->set_label(p_label);
		s->set_min(p_min);
		s->set_max(p_max);
		s->set_step(p_step);
		s->set_value(p_value);
		s->set_suffix(p_suffix);
		s->set_exp_ratio(p_exp);
		s->set_allow_greater(true);
		s->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		s->connect(SceneStringName(value_changed), callable_mp(this, &AsosdotTerrainEditor::_settings_changed));
		brush->add_child(s);
		return s;
	};
	size_slider = make_slider(TTR("Size"), 0.25, 500.0, 0.05, settings.radius, "m", true);
	strength_slider = make_slider(TTR("Strength"), 0.01, 1.0, 0.01, settings.strength, "", false);
	strength_slider->set_allow_greater(false);
	hardness_slider = make_slider(TTR("Hardness"), 0.0, 1.0, 0.01, settings.hardness, "", false);
	hardness_slider->set_allow_greater(false);
	noise_slider = make_slider(TTR("Noise Size"), 0.5, 500.0, 0.1, settings.noise_scale, "m", true);
	terrace_slider = make_slider(TTR("Step"), 0.05, 100.0, 0.05, settings.terrace_step, "m", true);

	flatten_row = memnew(HBoxContainer);
	brush->add_child(flatten_row);
	flatten_fixed = memnew(CheckBox);
	flatten_fixed->set_text(TTR("Fixed Height"));
	flatten_fixed->set_tooltip_text(TTR("Off: flatten to the height where the stroke starts. On: always flatten to this height."));
	flatten_fixed->connect(SceneStringName(toggled), callable_mp(this, &AsosdotTerrainEditor::_flatten_fixed_toggled));
	flatten_row->add_child(flatten_fixed);
	flatten_height = memnew(SpinBox);
	flatten_height->set_min(-100000);
	flatten_height->set_max(100000);
	flatten_height->set_step(0.01);
	flatten_height->set_suffix("m");
	flatten_height->set_editable(false);
	flatten_height->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	flatten_height->connect(SceneStringName(value_changed), callable_mp(this, &AsosdotTerrainEditor::_settings_changed));
	flatten_row->add_child(flatten_height);

	color_row = memnew(HBoxContainer);
	brush->add_child(color_row);
	color_row->add_child(memnew(Label(TTR("Color"))));
	color_picker = memnew(ColorPickerButton);
	color_picker->set_pick_color(settings.color);
	color_picker->set_edit_alpha(false);
	color_picker->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	color_picker->connect("color_changed", callable_mp(this, &AsosdotTerrainEditor::_color_changed));
	color_row->add_child(color_picker);

	hint_label = memnew(Label);
	hint_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	hint_label->set_modulate(Color(1, 1, 1, 0.6));
	hint_label->set_custom_minimum_size(Size2(240, 0) * EDSCALE);
	brush->add_child(hint_label);

	main->add_child(memnew(VSeparator));

	// Layers and foliage.
	palette_tabs = memnew(TabContainer);
	palette_tabs->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	palette_tabs->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	palette_tabs->set_theme_type_variation("TabContainerOdd");
	main->add_child(palette_tabs);

	const auto add_icon_button = [&](HBoxContainer *p_bar, const StringName &p_icon, const String &p_tooltip, const Callable &p_callable) {
		Button *b = memnew(Button);
		b->set_theme_type_variation(SceneStringName(FlatButton));
		b->set_tooltip_text(p_tooltip);
		b->set_accessibility_name(p_tooltip);
		b->set_meta(SNAME("_icon"), p_icon);
		b->connect(SceneStringName(pressed), p_callable);
		p_bar->add_child(b);
		icon_buttons.push_back(b);
		return b;
	};
	const auto make_inspector = []() {
		EditorInspector *inspector = memnew(EditorInspector);
		inspector->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		inspector->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		inspector->set_custom_minimum_size(Size2(240, 0) * EDSCALE);
		inspector->set_use_doc_hints(true);
		inspector->set_hide_script(true);
		inspector->set_hide_metadata(true);
		inspector->set_autoclear(true);
		return inspector;
	};
	const auto make_list = []() {
		ItemList *list = memnew(ItemList);
		list->set_max_columns(0);
		list->set_icon_mode(ItemList::ICON_MODE_TOP);
		list->set_fixed_icon_size(Size2(56, 56) * EDSCALE);
		list->set_fixed_column_width(88 * EDSCALE);
		list->set_max_text_lines(2);
		list->set_same_column_width(true);
		list->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		list->set_custom_minimum_size(Size2(160, 0) * EDSCALE);
		return list;
	};

	HSplitContainer *layer_split = memnew(HSplitContainer);
	layer_split->set_name(TTR("Layers"));
	palette_tabs->add_child(layer_split);
	VBoxContainer *layer_side = memnew(VBoxContainer);
	layer_side->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	layer_split->add_child(layer_side);
	HBoxContainer *layer_bar = memnew(HBoxContainer);
	layer_side->add_child(layer_bar);
	MenuButton *layer_add = memnew(MenuButton);
	layer_add->set_text(TTR("Add"));
	layer_add->set_flat(false);
	layer_add->set_theme_type_variation("FlatMenuButton");
	layer_add->set_tooltip_text(TTR("Add layers. Dragging textures from the FileSystem dock into the list works too, and pairs each colour texture with its normal map by name."));
	layer_add->get_popup()->add_item(TTR("New Layer"), LAYER_NEW);
	layer_add->get_popup()->add_item(TTR("New Layers from Textures..."), LAYER_FROM_TEXTURES);
	layer_add->get_popup()->add_item(TTR("Load Layer..."), LAYER_LOAD);
	layer_add->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &AsosdotTerrainEditor::_layer_add_option));
	layer_bar->add_child(layer_add);
	layer_duplicate_button = add_icon_button(layer_bar, SNAME("Duplicate"), TTR("Duplicate the selected layer."), callable_mp(this, &AsosdotTerrainEditor::_duplicate_layer));
	layer_left_button = add_icon_button(layer_bar, SNAME("ArrowLeft"), TTR("Move the selected layer earlier. Its painted areas move with it."), callable_mp(this, &AsosdotTerrainEditor::_move_layer).bind(-1));
	layer_right_button = add_icon_button(layer_bar, SNAME("ArrowRight"), TTR("Move the selected layer later. Its painted areas move with it."), callable_mp(this, &AsosdotTerrainEditor::_move_layer).bind(1));
	layer_save_button = add_icon_button(layer_bar, SNAME("Save"), TTR("Save the selected layer to a file, to share it with other terrains."), callable_mp(this, &AsosdotTerrainEditor::_save_selected).bind(false));
	layer_remove_button = add_icon_button(layer_bar, SNAME("Remove"), TTR("Remove the selected layer. Where it was painted, the others take over."), callable_mp(this, &AsosdotTerrainEditor::_remove_layer));
	layer_list = make_list();
	layer_list->connect(SceneStringName(item_selected), callable_mp(this, &AsosdotTerrainEditor::_layer_selected));
	layer_list->set_drag_forwarding(Callable(), callable_mp(this, &AsosdotTerrainEditor::_can_drop_layer), callable_mp(this, &AsosdotTerrainEditor::_drop_layer));
	layer_side->add_child(layer_list);
	layer_empty = memnew(Label);
	layer_empty->set_text(TTR("No layers yet. Add one, or drag textures here from the FileSystem dock."));
	layer_empty->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	layer_empty->set_modulate(Color(1, 1, 1, 0.6));
	layer_side->add_child(layer_empty);
	layer_inspector = make_inspector();
	layer_split->add_child(layer_inspector);

	HSplitContainer *foliage_split = memnew(HSplitContainer);
	foliage_split->set_name(TTR("Foliage"));
	palette_tabs->add_child(foliage_split);
	VBoxContainer *foliage_side = memnew(VBoxContainer);
	foliage_side->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	foliage_split->add_child(foliage_side);
	HBoxContainer *foliage_bar = memnew(HBoxContainer);
	foliage_side->add_child(foliage_bar);
	Button *foliage_add = memnew(Button);
	foliage_add->set_text(TTR("Add..."));
	foliage_add->set_theme_type_variation(SceneStringName(FlatButton));
	foliage_add->set_tooltip_text(TTR("Add grass or objects from your own meshes and scenes, choosing whether each is painted as grass or placed as objects. You can also drag them from the FileSystem dock into the list."));
	foliage_add->connect(SceneStringName(pressed), callable_mp(this, &AsosdotTerrainEditor::_add_foliage_pressed));
	foliage_bar->add_child(foliage_add);
	foliage_duplicate_button = add_icon_button(foliage_bar, SNAME("Duplicate"), TTR("Duplicate the selected type, without what was painted with it."), callable_mp(this, &AsosdotTerrainEditor::_duplicate_foliage_type));
	foliage_save_button = add_icon_button(foliage_bar, SNAME("Save"), TTR("Save the selected type to a file, to share it with other terrains."), callable_mp(this, &AsosdotTerrainEditor::_save_selected).bind(true));
	foliage_remove_button = add_icon_button(foliage_bar, SNAME("Remove"), TTR("Remove the selected type and everything painted with it."), callable_mp(this, &AsosdotTerrainEditor::_remove_foliage_type));
	foliage_list = make_list();
	foliage_list->connect(SceneStringName(item_selected), callable_mp(this, &AsosdotTerrainEditor::_foliage_selected));
	foliage_list->set_drag_forwarding(Callable(), callable_mp(this, &AsosdotTerrainEditor::_can_drop_foliage), callable_mp(this, &AsosdotTerrainEditor::_drop_foliage));
	foliage_side->add_child(foliage_list);
	foliage_empty = memnew(Label);
	foliage_empty->set_text(TTR("No foliage yet. Use Add... to bring in a mesh or scene as grass or objects, or drag one here from the FileSystem dock."));
	foliage_empty->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	foliage_empty->set_modulate(Color(1, 1, 1, 0.6));
	foliage_side->add_child(foliage_empty);
	foliage_inspector = make_inspector();
	foliage_split->add_child(foliage_inspector);

	// Dialogs.
	create_dialog = memnew(AsosdotTerrainCreateDialog);
	create_dialog->connect(SceneStringName(confirmed), callable_mp(this, &AsosdotTerrainEditor::_create_confirmed));
	add_child(create_dialog);

	{
		List<String> texture_extensions;
		ResourceLoader::get_recognized_extensions_for_type("Texture2D", &texture_extensions);
		PackedStringArray patterns;
		for (const String &extension : texture_extensions) {
			if (!patterns.has("*." + extension)) {
				patterns.push_back("*." + extension);
			}
		}
		layer_textures_dialog = memnew(EditorFileDialog);
		layer_textures_dialog->set_title(TTR("New Layers from Textures"));
		layer_textures_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILES);
		layer_textures_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
		layer_textures_dialog->add_filter(String(",").join(patterns), TTR("Textures"));
		layer_textures_dialog->connect("files_selected", callable_mp(this, &AsosdotTerrainEditor::_add_layers_from_files));
		add_child(layer_textures_dialog);
	}
	layer_load_dialog = memnew(EditorFileDialog);
	layer_load_dialog->set_title(TTR("Load Terrain Layers"));
	layer_load_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILES);
	layer_load_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
	layer_load_dialog->add_filter("*.tres,*.res", TTR("Terrain Layers"));
	layer_load_dialog->connect("files_selected", callable_mp(this, &AsosdotTerrainEditor::_layer_load_selected));
	add_child(layer_load_dialog);
	resource_save_dialog = memnew(EditorFileDialog);
	resource_save_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	resource_save_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
	resource_save_dialog->add_filter("*.tres", TTR("Text Resource"));
	resource_save_dialog->add_filter("*.res", TTR("Binary Resource"));
	resource_save_dialog->connect("file_selected", callable_mp(this, &AsosdotTerrainEditor::_resource_save_selected));
	add_child(resource_save_dialog);

	foliage_add_dialog = memnew(EditorFileDialog);
	foliage_add_dialog->set_title(TTR("Add Foliage"));
	foliage_add_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILES);
	foliage_add_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
	{
		List<String> extensions;
		ResourceLoader::get_recognized_extensions_for_type("Mesh", &extensions);
		ResourceLoader::get_recognized_extensions_for_type("PackedScene", &extensions);
		ResourceLoader::get_recognized_extensions_for_type("FoliageType", &extensions);
		PackedStringArray patterns;
		for (const String &extension : extensions) {
			if (!patterns.has("*." + extension)) {
				patterns.push_back("*." + extension);
			}
		}
		foliage_add_dialog->add_filter(String(",").join(patterns), TTR("Meshes, Scenes and Foliage Types"));
	}
	foliage_add_dialog->add_option(TTR("Paint As"), { TTR("Objects, placed one by one"), TTR("Grass, painted as a density") }, 0);
	foliage_add_dialog->connect("files_selected", callable_mp(this, &AsosdotTerrainEditor::_foliage_files_selected));
	add_child(foliage_add_dialog);

	export_dialog = memnew(EditorFileDialog);
	export_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	export_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	export_dialog->add_filter("*.exr", TTR("OpenEXR (meters)"));
	export_dialog->add_filter("*.png", TTR("PNG, 16-bit"));
	export_dialog->add_filter("*.r16", TTR("Raw 16-bit"));
	export_dialog->connect("file_selected", callable_mp(this, &AsosdotTerrainEditor::_export_selected));
	add_child(export_dialog);

	message_dialog = memnew(AcceptDialog);
	message_dialog->set_title(TTR("Terrain"));
	add_child(message_dialog);

	_update_tool_ui();
}

/* -------------------------------------------------------------------------- */
/*                                   Plugin                                   */
/* -------------------------------------------------------------------------- */

bool AsosdotTerrainEditorPlugin::handles(Object *p_object) const {
	if (!terrain_editor) {
		return false;
	}
	return Object::cast_to<Terrain3D>(p_object) || Object::cast_to<Foliage3D>(p_object) || Object::cast_to<VoxelTerrain3D>(p_object);
}

void AsosdotTerrainEditorPlugin::edit(Object *p_object) {
	// The editor still tidies its plugins up after the dock has gone.
	if (!terrain_editor) {
		return;
	}
	terrain_editor->edit(p_object);
}

void AsosdotTerrainEditorPlugin::make_visible(bool p_visible) {
	if (!terrain_editor) {
		return;
	}
	if (p_visible) {
		terrain_editor->make_visible();
	} else {
		terrain_editor->close();
	}
	terrain_editor->set_active(p_visible);
}

EditorPlugin::AfterGUIInput AsosdotTerrainEditorPlugin::forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	if (!terrain_editor) {
		return EditorPlugin::AFTER_GUI_INPUT_PASS;
	}
	const EditorPlugin::AfterGUIInput result = terrain_editor->forward_input(p_camera, p_event);
	if (terrain_editor->tool != TerrainBrush::TOOL_NONE) {
		update_overlays();
	}
	return result;
}

void AsosdotTerrainEditorPlugin::forward_3d_draw_over_viewport(Control *p_overlay) {
	if (terrain_editor) {
		terrain_editor->draw_overlay(p_overlay);
	}
}

void AsosdotTerrainEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (ClassDB::is_asosdot_class_group_yielded("terrain")) {
				// The project has its own class of one of the terrain's names, as with the Terrain3D extension.
				break;
			}
			terrain_editor = memnew(AsosdotTerrainEditor);
			EditorDockManager::get_singleton()->add_dock(terrain_editor);
			terrain_editor->close();
			terrain_editor->set_process(true);
			set_process(true);
		} break;

		case NOTIFICATION_EXIT_TREE: {
			if (!terrain_editor) {
				break;
			}
			EditorDockManager::get_singleton()->remove_dock(terrain_editor);
			memdelete_notnull(terrain_editor);
			terrain_editor = nullptr;
		} break;

		case NOTIFICATION_PROCESS: {
			// Grass grows around a camera, and while editing that is whichever
			// viewport was used last.
			Node3DEditor *editor = Node3DEditor::get_singleton();
			if (!editor || !terrain_editor) {
				break;
			}
			Camera3D *camera = terrain_editor ? ObjectDB::get_instance<Camera3D>(terrain_editor->last_camera) : nullptr;
			if (!camera) {
				camera = editor->get_editor_viewport(0)->get_camera_3d();
			}
			if (camera && camera->is_inside_tree()) {
				// Grass grows, and voxel terrain picks its detail, around it.
				Foliage3D::set_editor_focus(camera->get_global_position());
				VoxelTerrain3D::set_editor_viewer(camera->get_global_position());
			}
		} break;
	}
}

AsosdotTerrainEditorPlugin::AsosdotTerrainEditorPlugin() {
	if (ClassDB::is_asosdot_class_group_yielded("terrain")) {
		return;
	}
	gizmo_plugin.instantiate();
	Node3DEditor::get_singleton()->add_gizmo_plugin(gizmo_plugin);
	voxel_gizmo_plugin.instantiate();
	Node3DEditor::get_singleton()->add_gizmo_plugin(voxel_gizmo_plugin);
}
