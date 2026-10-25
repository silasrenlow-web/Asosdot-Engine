/**************************************************************************/
/*  realtime_gi_editor_plugin.cpp                                         */
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

#include "realtime_gi_editor_plugin.h"

#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "scene/3d/realtime_gi.h"
#include "scene/gui/button.h"
#include "scene/main/scene_tree.h"
#include "servers/display/display_server.h"

EditorProgress *RealtimeGIEditorPlugin::tmp_progress = nullptr;

bool RealtimeGIEditorPlugin::bake_func_step(float p_progress, const String &p_description, void *, bool p_refresh) {
	if (!tmp_progress) {
		tmp_progress = memnew(EditorProgress("bake_realtime_gi", TTR("Bake Realtime GI"), 1000, true));
		ERR_FAIL_NULL_V(tmp_progress, false);
	}
	return tmp_progress->step(p_description, p_progress * 1000, p_refresh);
}

void RealtimeGIEditorPlugin::_bake() {
	if (!realtime_gi) {
		return;
	}
	Node *root = get_tree()->get_edited_scene_root();
	RealtimeGI::BakeError err = RealtimeGI::BAKE_ERROR_NO_SCENE_ROOT;
	const uint64_t time_started = OS::get_singleton()->get_ticks_msec();
	if (root) {
		err = realtime_gi->bake(root == realtime_gi ? realtime_gi : realtime_gi->get_parent(), String(), bake_func_step);
	}
	if (tmp_progress) {
		memdelete(tmp_progress);
		tmp_progress = nullptr;
	}

	switch (err) {
		case RealtimeGI::BAKE_ERROR_OK: {
			const int time_taken = OS::get_singleton()->get_ticks_msec() - time_started;
			Ref<RealtimeGIData> gi_data = realtime_gi->get_data();
			print_line(vformat("Baked realtime GI in %d.%02d s: %d patches, %d probes.", time_taken / 1000, (time_taken % 1000) / 10, gi_data.is_valid() ? gi_data->get_patch_count() : 0, gi_data.is_valid() ? gi_data->get_probe_count() : 0));
			DisplayServer::get_singleton()->window_request_attention();
		} break;
		case RealtimeGI::BAKE_ERROR_NO_SCENE_ROOT: {
			EditorNode::get_singleton()->show_warning(TTR("No editor scene root found."));
		} break;
		case RealtimeGI::BAKE_ERROR_NO_SAVE_PATH: {
			EditorNode::get_singleton()->show_warning(TTR("Save the scene first. The baked realtime GI is saved next to it."));
		} break;
		case RealtimeGI::BAKE_ERROR_NO_MESHES: {
			EditorNode::get_singleton()->show_warning(
					TTR("No meshes to light. Realtime GI lights static meshes that have UV2: set their Global Illumination mode to Static, and give them UV2.") +
					String::utf8("\n\n•  ") + TTR("For an imported scene, set Meshes > Light Baking to Static Lightmaps in the Import dock.") +
					String::utf8("\n•  ") + TTR("For a primitive mesh, check Add UV2 on the PrimitiveMesh resource.") +
					String::utf8("\n•  ") + TTR("For any other mesh, select its MeshInstance3D and choose Mesh > Unwrap UV2 for Lightmap/AO."));
		} break;
		case RealtimeGI::BAKE_ERROR_NO_RAYCASTER: {
			EditorNode::get_singleton()->show_warning(TTR("Realtime GI needs the raycast module (Embree) to bake, and this editor was built without it."));
		} break;
		case RealtimeGI::BAKE_ERROR_CANT_SAVE: {
			EditorNode::get_singleton()->show_warning(TTR("Couldn't save the baked realtime GI. Make sure the folder next to the scene is writable."));
		} break;
		default: {
		} break;
	}
}

void RealtimeGIEditorPlugin::edit(Object *p_object) {
	realtime_gi = Object::cast_to<RealtimeGI>(p_object);
}

bool RealtimeGIEditorPlugin::handles(Object *p_object) const {
	// A project class of the same name takes the name; this isn't it then.
	return Object::cast_to<RealtimeGI>(p_object) != nullptr;
}

void RealtimeGIEditorPlugin::make_visible(bool p_visible) {
	bake->set_visible(p_visible);
}

RealtimeGIEditorPlugin::RealtimeGIEditorPlugin() {
	bake = memnew(Button);
	bake->set_theme_type_variation(SceneStringName(FlatButton));
	bake->set_button_icon(EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("Bake"), EditorStringName(EditorIcons)));
	bake->set_text(TTR("Bake Realtime GI"));
	bake->set_tooltip_text(TTR("Works out how light travels between the static meshes. Lights can then move, turn and change while the bounced light follows them."));
	bake->hide();
	bake->connect(SceneStringName(pressed), callable_mp(this, &RealtimeGIEditorPlugin::_bake));
	add_control_to_container(CONTAINER_SPATIAL_EDITOR_MENU, bake);
}
