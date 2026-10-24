/**************************************************************************/
/*  editor_scene_exporter_blend_plugin.cpp                                */
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

#include "editor_scene_exporter_blend_plugin.h"

#include "../gltf_document.h"

#include "core/config/project_settings.h"
#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/gui/editor_toaster.h"
#include "editor/import/3d/resource_importer_scene.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/check_box.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/label.h"
#include "scene/gui/spin_box.h"
#include "scene/main/scene_tree.h"

namespace {

// Options are read from a JSON file, never interpolated into Python code or command-line arguments.
constexpr const char *BLENDER_EXPORT_SCRIPT = R"PY(
import bpy
import json
import math
import os
import sys
import tempfile

with open(sys.argv[sys.argv.index('--') + 1], encoding='utf-8') as request:
    options = json.load(request)
if bpy.app.version < (3, 0, 0):
    raise RuntimeError('Blender 3.0 or later is required.')
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.fps = options['fps']
scene.render.fps_base = 1.0
import_options = dict(filepath=options['glb_path'], import_pack_images=True)
properties = bpy.ops.import_scene.gltf.get_rna_type().properties
for name in ('export_import_convert_lighting_mode', 'convert_lighting_mode'):
    if name in properties:
        import_options[name] = 'COMPAT'
        break
if bpy.ops.import_scene.gltf(**import_options) != {'FINISHED'}:
    raise RuntimeError('Blender could not import the exported scene.')

# Keep the full playback range so Blender's default range does not truncate clips on reimport.
if bpy.data.actions:
    scene.frame_start = math.floor(min(action.frame_range[0] for action in bpy.data.actions))
    scene.frame_end = max(scene.frame_start + 1, math.ceil(max(action.frame_range[1] for action in bpy.data.actions)))
scene.frame_set(scene.frame_start)
for screen in bpy.data.screens:
    for area in screen.areas:
        if area.type == 'VIEW_3D':
            area.spaces.active.shading.type = 'MATERIAL'
bpy.ops.file.pack_all()

# Save beside the destination and replace it only after Blender has finished successfully.
# This also preserves an existing .blend file if conversion fails.
destination = options['blend_path']
handle, staged = tempfile.mkstemp(prefix='.asosdot-', suffix='.blend', dir=os.path.dirname(destination))
os.close(handle)
try:
    bpy.context.preferences.filepaths.save_version = 0
    if bpy.ops.wm.save_as_mainfile(filepath=staged, check_existing=False) != {'FINISHED'}:
        raise RuntimeError('Blender could not save the converted scene.')
    os.replace(staged, destination)
finally:
    if os.path.exists(staged):
        os.remove(staged)
)PY";

bool has_external_skeleton(Node *p_node, Node *p_root) {
	MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(p_node);
	if (mesh && !mesh->get_skeleton_path().is_empty()) {
		Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(mesh->get_node_or_null(mesh->get_skeleton_path()));
		if (skeleton && skeleton != p_root && !p_root->is_ancestor_of(skeleton)) {
			return true;
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		if (has_external_skeleton(p_node->get_child(i), p_root)) {
			return true;
		}
	}
	return false;
}

} // namespace

void AsosdotSceneExporterBlendPlugin::_popup_export_dialog(bool p_selected_branch) {
	if (task != WorkerThreadPool::INVALID_TASK_ID) {
		EditorNode::get_singleton()->show_warning(TTR("A Blender export is already in progress."));
		return;
	}
	Node *root = EditorNode::get_singleton()->get_tree()->get_edited_scene_root();
	if (!root) {
		EditorNode::get_singleton()->show_warning(TTR("Open a 3D scene before exporting to Blender."));
		return;
	}
	if (p_selected_branch) {
		List<Node *> selected = EditorNode::get_singleton()->get_editor_selection()->get_top_selected_node_list();
		if (selected.size() != 1) {
			EditorNode::get_singleton()->show_warning(TTR("Select one character or object branch to export. Include its Skeleton3D and AnimationPlayer children, or export the whole scene."));
			return;
		}
		root = selected.front()->get();
	}
	if (has_external_skeleton(root, root)) {
		EditorNode::get_singleton()->show_warning(TTR("This branch uses a skeleton outside the branch. Select the character's parent branch or export the whole scene to keep the rig."));
		return;
	}
	export_root = root->get_instance_id();
	file_dialog->set_current_file(String(root->get_name()).validate_filename() + ".blend");
	file_dialog->popup_file_dialog();
}

void AsosdotSceneExporterBlendPlugin::_file_selected(const String &p_path) {
	export_path = p_path;
	settings_dialog->popup_centered();
}

void AsosdotSceneExporterBlendPlugin::_export_scene() {
	if (task != WorkerThreadPool::INVALID_TASK_ID) {
		return;
	}
	Node *root = Object::cast_to<Node>(ObjectDB::get_instance(export_root));
	Node *scene_root = EditorNode::get_singleton()->get_tree()->get_edited_scene_root();
	if (!root || !scene_root || (root != scene_root && !scene_root->is_ancestor_of(root))) {
		EditorNode::get_singleton()->show_warning(TTR("The scene or selected branch changed. Start the Blender export again."));
		return;
	}
	String blender_path = EDITOR_GET("filesystem/import/blender/blender_path");
	if (blender_path.is_empty() || !FileAccess::exists(blender_path)) {
		EditorNode::get_singleton()->show_warning(TTR("Set the Blender executable in Editor Settings > Filesystem > Import > Blender > Blender Path, then export again."));
		return;
	}
	if (!export_path.has_extension("blend")) {
		EditorNode::get_singleton()->show_warning(TTR("Choose a file with the .blend extension."));
		return;
	}
	Error err;
	job.temp_dir = DirAccess::create_temp("asosdot-blender", false, &err);
	if (err != OK) {
		EditorNode::get_singleton()->show_warning(TTR("Could not create temporary files for the Blender export."));
		return;
	}
	job.blender_path = blender_path;
	job.glb_path = job.temp_dir->get_current_dir().path_join("scene.glb");
	job.blend_path = ProjectSettings::get_singleton()->globalize_path(export_path);
#ifdef WINDOWS_ENABLED
	// Blender treats paths beginning with // as relative to the .blend file.
	if (job.blend_path.is_network_share_path()) {
		job.blend_path = "\\\\" + job.blend_path.substr(2);
	}
#endif
	job.fps = (int)bake_fps->get_value();
	job.open = open_in_blender->is_pressed();
	job.result = OK;
	job.output = String();

	Ref<GLTFDocument> document;
	document.instantiate();
	document->set_root_node_mode(GLTFDocument::ROOT_NODE_MODE_KEEP_ROOT);
	document->set_visibility_mode(GLTFDocument::VISIBILITY_MODE_EXCLUDE);
	document->set_image_format("PNG");
	Ref<GLTFState> state;
	state.instantiate();
	state->set_bake_fps(job.fps);
	state->set_create_animations(export_animations->is_pressed());
	err = document->append_from_scene(root, state, EditorSceneFormatImporter::IMPORT_USE_NAMED_SKIN_BINDS);
	if (err == OK) {
		err = document->write_to_filesystem(state, job.glb_path);
	}
	if (err != OK) {
		job.temp_dir.unref();
		EditorNode::get_singleton()->show_warning(vformat(TTR("Could not export the scene for Blender (error %d)."), err));
		return;
	}
	EditorToaster::get_singleton()->popup_str(TTR("Exporting scene to Blender..."));
	task = WorkerThreadPool::get_singleton()->add_native_task(_export_blend, &job, false, "Blender scene export");
	set_process(true);
}

void AsosdotSceneExporterBlendPlugin::_export_blend(void *p_userdata) {
	ExportJob *export_job = static_cast<ExportJob *>(p_userdata);
	Dictionary options;
	options["glb_path"] = export_job->glb_path;
	options["blend_path"] = export_job->blend_path;
	options["fps"] = export_job->fps;
	const String script_path = export_job->glb_path.get_base_dir().path_join("export.py");
	const String options_path = export_job->glb_path.get_base_dir().path_join("request.json");
	Ref<FileAccess> script_file = FileAccess::open(script_path, FileAccess::WRITE, &export_job->result);
	if (export_job->result != OK) {
		return;
	}
	script_file->store_string(BLENDER_EXPORT_SCRIPT);
	export_job->result = script_file->get_error();
	script_file.unref();
	if (export_job->result != OK) {
		return;
	}
	Ref<FileAccess> options_file = FileAccess::open(options_path, FileAccess::WRITE, &export_job->result);
	if (export_job->result != OK) {
		return;
	}
	options_file->store_string(JSON::stringify(options));
	export_job->result = options_file->get_error();
	options_file.unref();
	if (export_job->result != OK) {
		return;
	}
	List<String> args;
	args.push_back("--background");
	args.push_back("--factory-startup");
	args.push_back("--disable-autoexec");
	args.push_back("--python-exit-code");
	args.push_back("1");
	args.push_back("--python");
	args.push_back(script_path);
	args.push_back("--");
	args.push_back(options_path);
	int exit_code = -1;
	export_job->result = OS::get_singleton()->execute(export_job->blender_path, args, &export_job->output, &exit_code, true);
	if (export_job->result == OK && (exit_code != 0 || !FileAccess::exists(export_job->blend_path))) {
		export_job->result = FAILED;
	}
}

void AsosdotSceneExporterBlendPlugin::_notification(int p_what) {
	if (p_what != NOTIFICATION_PROCESS || task == WorkerThreadPool::INVALID_TASK_ID || !WorkerThreadPool::get_singleton()->is_task_completed(task)) {
		return;
	}
	WorkerThreadPool::get_singleton()->wait_for_task_completion(task);
	task = WorkerThreadPool::INVALID_TASK_ID;
	set_process(false);
	job.temp_dir.unref();
	if (job.result != OK) {
		ERR_PRINT(vformat("Blender export failed (error %d):\n%s", job.result, job.output));
		EditorNode::get_singleton()->show_warning(TTR("Blender could not export the scene. See the Output panel for details."));
		return;
	}
	EditorFileSystem::get_singleton()->scan_changes();
	EditorToaster::get_singleton()->popup_str(vformat(TTR("Exported Blender scene: %s"), job.blend_path));
	if (job.open) {
		List<String> args;
		args.push_back(job.blend_path);
		Error err = OS::get_singleton()->create_process(job.blender_path, args);
		if (err != OK) {
			EditorNode::get_singleton()->show_warning(vformat(TTR("The scene was exported, but Blender could not be opened (error %d)."), err));
		}
	}
}

AsosdotSceneExporterBlendPlugin::AsosdotSceneExporterBlendPlugin() {
	file_dialog = memnew(EditorFileDialog);
	file_dialog->set_title(TTR("Export Scene to Blender"));
	file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	file_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	file_dialog->add_filter("*.blend", TTR("Blender Scene"));
	file_dialog->connect("file_selected", callable_mp(this, &AsosdotSceneExporterBlendPlugin::_file_selected));
	EditorNode::get_singleton()->get_gui_base()->add_child(file_dialog);

	settings_dialog = memnew(ConfirmationDialog);
	settings_dialog->set_title(TTR("Blender Export Settings"));
	settings_dialog->set_ok_button_text(TTR("Export"));
	settings_dialog->connect("confirmed", callable_mp(this, &AsosdotSceneExporterBlendPlugin::_export_scene));
	EditorNode::get_singleton()->get_gui_base()->add_child(settings_dialog);
	VBoxContainer *settings = memnew(VBoxContainer);
	settings->set_custom_minimum_size(Size2(480, 0) * EDSCALE);
	settings_dialog->add_child(settings);
	Label *description = memnew(Label);
	description->set_text(TTR("Exports visible meshes, PBR materials, packed textures, rigs, cameras and lights. Custom shaders, environments, lens flares and rendering effects need to be recreated in Blender."));
	description->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	settings->add_child(description);
	export_animations = memnew(CheckBox);
	export_animations->set_text(TTR("Include Animations"));
	export_animations->set_pressed(true);
	settings->add_child(export_animations);
	HBoxContainer *fps_row = memnew(HBoxContainer);
	settings->add_child(fps_row);
	Label *fps_label = memnew(Label);
	fps_label->set_text(TTR("Animation FPS:"));
	fps_row->add_child(fps_label);
	bake_fps = memnew(SpinBox);
	bake_fps->set_min(1);
	bake_fps->set_max(240);
	bake_fps->set_step(1);
	bake_fps->set_value(30);
	fps_row->add_child(bake_fps);
	open_in_blender = memnew(CheckBox);
	open_in_blender->set_text(TTR("Open in Blender After Export"));
	settings->add_child(open_in_blender);
	Label *round_trip = memnew(Label);
	round_trip->set_text(TTR("To bring edits back, save the .blend file inside your project, or export glTF 2.0 (.glb) from Blender. Use Principled BSDF materials with image textures; bake procedural materials to images first."));
	round_trip->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	settings->add_child(round_trip);

	PopupMenu *menu = get_export_as_menu();
	int idx = menu->get_item_count();
	menu->add_item(TTR("Blender Scene..."));
	menu->set_item_metadata(idx, callable_mp(this, &AsosdotSceneExporterBlendPlugin::_popup_export_dialog).bind(false));
	idx = menu->get_item_count();
	menu->add_item(TTR("Selected Branch to Blender..."));
	menu->set_item_metadata(idx, callable_mp(this, &AsosdotSceneExporterBlendPlugin::_popup_export_dialog).bind(true));
	set_process(false);
}

AsosdotSceneExporterBlendPlugin::~AsosdotSceneExporterBlendPlugin() {
	if (task != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(task);
	}
}
