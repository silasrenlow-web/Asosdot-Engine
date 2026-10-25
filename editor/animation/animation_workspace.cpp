/**************************************************************************/
/*  animation_workspace.cpp                                               */
/**************************************************************************/
/*  Asosdot: the Animation workspace, a main screen laid out for          */
/*  animating cutscenes and rigs: the 3D editor as the viewer, a library  */
/*  of animations, shots, sounds and poses beside it, the Inspector, and  */
/*  a tall timeline below, with recording, camera cuts and a pose library.*/
/**************************************************************************/

#include "animation_workspace.h"

#include "core/io/resource_loader.h"
#include "editor/animation/animation_camera_monitor.h"
#include "core/io/resource_saver.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/animation/animation_player_editor_plugin.h"
#include "editor/animation/animation_track_editor.h"
#include "editor/animation/animation_track_editor_plugins.h"
#include "editor/docks/editor_dock_manager.h"
#include "editor/docks/inspector_dock.h"
#include "editor/editor_data.h"
#include "editor/editor_interface.h"
#include "editor/editor_main_screen.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/inspector/editor_inspector.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/scene/3d/skeleton_3d_editor_plugin.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/animation/animation_player.h"
#include "scene/audio/audio_stream_player.h"
#include "scene/gui/flow_container.h"
#include "scene/gui/item_list.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/separator.h"
#include "scene/gui/slider.h"
#include "scene/gui/split_container.h"
#include "scene/gui/tab_container.h"
#include "scene/gui/tree.h"
#include "scene/resources/animation_library.h"
#include "servers/audio/audio_stream.h"

// How much of the editor's height the timeline gets while the workspace is open, and while it
// shows the Graph Editor, whose curves want the room.
static constexpr float TIMELINE_SHARE = 0.42f;
static constexpr float GRAPH_TIMELINE_SHARE = 0.62f;

String AsosdotAnimationWorkspace::format_time(double p_time) {
	const int minutes = int(p_time) / 60;
	return vformat("%02d:%05.2f", minutes, p_time - minutes * 60);
}

struct CameraCutByTime {
	_FORCE_INLINE_ bool operator()(const AsosdotAnimationWorkspace::CameraCut &p_a, const AsosdotAnimationWorkspace::CameraCut &p_b) const {
		return p_a.time < p_b.time;
	}
};

AnimationTrackEditor *AsosdotAnimationWorkspace::_get_track_editor() const {
	AnimationPlayerEditor *animation_editor = AnimationPlayerEditor::get_singleton();
	return animation_editor ? animation_editor->get_track_editor() : nullptr;
}

AnimationPlayer *AsosdotAnimationWorkspace::_get_player() const {
	AnimationPlayerEditor *animation_editor = AnimationPlayerEditor::get_singleton();
	return animation_editor ? animation_editor->get_player() : nullptr;
}

Ref<Animation> AsosdotAnimationWorkspace::_get_animation() const {
	AnimationTrackEditor *track_editor = _get_track_editor();
	return track_editor ? track_editor->get_current_animation() : Ref<Animation>();
}

Node *AsosdotAnimationWorkspace::_get_animation_root() const {
	AnimationTrackEditor *track_editor = _get_track_editor();
	return track_editor ? track_editor->get_root() : nullptr;
}

double AsosdotAnimationWorkspace::_get_playhead() const {
	AnimationPlayer *player = _get_player();
	if (player && player->is_valid()) {
		return player->get_current_animation_position();
	}
	AnimationPlayerEditor *animation_editor = AnimationPlayerEditor::get_singleton();
	return animation_editor ? animation_editor->timeline_position : 0.0;
}

void AsosdotAnimationWorkspace::_set_playhead(double p_time) {
	AnimationPlayerEditor *animation_editor = AnimationPlayerEditor::get_singleton();
	if (animation_editor && animation_editor->frame) {
		// Through the Animation dock's time field, which moves the timeline and the scene with it.
		animation_editor->frame->set_value(p_time);
	}
}

/// Toolbar.

void AsosdotAnimationWorkspace::_update_timecode() {
	const Ref<Animation> animation = _get_animation();
	if (animation.is_null()) {
		timecode->set_text("--:--.--");
		return;
	}
	const double time = _get_playhead();
	const double step = animation->get_step();
	if (step > 0.0) {
		timecode->set_text(vformat(TTR("%s  frame %d"), format_time(time), int(Math::round(time / step))));
	} else {
		timecode->set_text(format_time(time));
	}
}

void AsosdotAnimationWorkspace::_shot_toggled(bool p_pressed) {
	if (!active || !Node3DEditor::get_singleton()) {
		return;
	}
	// The viewer looks through the camera that's current at the playhead, which the camera cuts set.
	Node3DEditor::get_singleton()->get_editor_viewport(0)->set_cinematic_preview(p_pressed);
}

void AsosdotAnimationWorkspace::_monitor_toggled(bool p_pressed) {
	monitor->set_active(active && p_pressed);
}

void AsosdotAnimationWorkspace::_graph_toggled(bool p_pressed) {
	AnimationTrackEditor *track_editor = _get_track_editor();
	if (track_editor && track_editor->is_graph_editor_active() != p_pressed) {
		track_editor->toggle_graph_editor();
	}
	// Without an animation to show there's no Graph Editor to open.
	graph_button->set_pressed_no_signal(track_editor && track_editor->is_graph_editor_active());
}

void AsosdotAnimationWorkspace::_graph_editor_toggled(bool p_shown) {
	graph_button->set_pressed_no_signal(p_shown);
	if (active && p_shown) {
		_enlarge_timeline(GRAPH_TIMELINE_SHARE);
	}
}

void AsosdotAnimationWorkspace::_docks_toggled(bool p_pressed) {
	_apply_docks();
}

void AsosdotAnimationWorkspace::_apply_docks() {
	// The Inspector stays, for nodes and keys; the other side docks make way for the library.
	EditorDockManager::get_singleton()->set_side_docks_hidden(active && !docks_button->is_pressed(), InspectorDock::get_singleton());
}

void AsosdotAnimationWorkspace::_watch_player() {
	AnimationPlayer *player = _get_player();
	const ObjectID id = player ? player->get_instance_id() : ObjectID();
	if (id == watched_player) {
		return;
	}
	AnimationPlayer *previous = ObjectDB::get_instance<AnimationPlayer>(watched_player);
	if (previous && previous->is_connected(SNAME("mixer_applied"), callable_mp(this, &AsosdotAnimationWorkspace::_record_snapshot))) {
		previous->disconnect(SNAME("mixer_applied"), callable_mp(this, &AsosdotAnimationWorkspace::_record_snapshot));
	}
	watched_player = id;
	if (player) {
		// The playhead moving poses the scene without an edit; that isn't something to record.
		player->connect(SNAME("mixer_applied"), callable_mp(this, &AsosdotAnimationWorkspace::_record_snapshot));
	}
}

/// Animations.

void AsosdotAnimationWorkspace::_update_animations(bool p_force) {
	AnimationPlayer *player = _get_player();
	uint32_t hash = hash_murmur3_one_64(player ? uint64_t(player->get_instance_id()) : 0);
	const String filter = animation_search->get_text().strip_edges();
	hash = hash_murmur3_one_32(filter.hash(), hash);
	LocalVector<StringName> libraries;
	if (player) {
		player->get_animation_library_list(&libraries);
		for (const StringName &library_name : libraries) {
			Ref<AnimationLibrary> library = player->get_animation_library(library_name);
			LocalVector<StringName> names;
			library->get_animation_list(&names);
			for (const StringName &name : names) {
				hash = hash_murmur3_one_32(String(name).hash(), hash);
				const Ref<Animation> animation = library->get_animation(name);
				hash = hash_murmur3_one_double(animation->get_length(), hash);
				hash = hash_murmur3_one_32(animation->get_loop_mode(), hash);
			}
		}
	}
	const String current = player ? String(player->get_assigned_animation()) : String();
	if (!p_force && hash == listed_animations_hash && current == listed_current) {
		return;
	}
	listed_animations_hash = hash;
	listed_current = current;

	animation_tree->clear();
	TreeItem *root = animation_tree->create_item();
	if (!player) {
		TreeItem *hint = animation_tree->create_item(root);
		hint->set_text(0, TTR("Select an AnimationPlayer to list its animations."));
		hint->set_selectable(0, false);
		hint->set_custom_color(0, get_theme_color(SNAME("font_disabled_color"), EditorStringName(Editor)));
		return;
	}

	const Ref<Texture2D> icon = get_editor_theme_icon(SNAME("Animation"));
	const Ref<Texture2D> loop_icon = get_editor_theme_icon(SNAME("Loop"));
	for (const StringName &library_name : libraries) {
		Ref<AnimationLibrary> library = player->get_animation_library(library_name);
		TreeItem *parent = root;
		if (libraries.size() > 1 || library_name != StringName()) {
			parent = animation_tree->create_item(root);
			parent->set_text(0, library_name == StringName() ? TTR("[Global]") : String(library_name));
			parent->set_icon(0, get_editor_theme_icon(SNAME("AnimationLibrary")));
			parent->set_selectable(0, false);
		}
		LocalVector<StringName> names;
		library->get_animation_list(&names);
		for (const StringName &name : names) {
			const String full_name = library_name == StringName() ? String(name) : String(library_name) + "/" + String(name);
			if (!filter.is_empty() && !full_name.containsn(filter)) {
				continue;
			}
			const Ref<Animation> animation = library->get_animation(name);
			TreeItem *item = animation_tree->create_item(parent);
			item->set_text(0, name);
			item->set_icon(0, animation->get_loop_mode() == Animation::LOOP_NONE ? icon : loop_icon);
			item->set_suffix(0, vformat(TTR("%s s"), String::num(animation->get_length(), 2)));
			item->set_metadata(0, full_name);
			item->set_tooltip_text(0, full_name);
			if (full_name == current) {
				item->select(0);
				animation_tree->scroll_to_item(item);
			}
		}
	}
}

void AsosdotAnimationWorkspace::_animation_selected() {
	TreeItem *item = animation_tree->get_selected();
	AnimationPlayerEditor *animation_editor = AnimationPlayerEditor::get_singleton();
	if (!item || !animation_editor || item->get_metadata(0).get_type() != Variant::STRING) {
		return;
	}
	const String name = item->get_metadata(0);
	if (name != listed_current) {
		listed_current = name;
		animation_editor->_select_anim_by_name(name);
	}
}

void AsosdotAnimationWorkspace::_animation_activated() {
	_animation_selected();
	AnimationPlayerEditor *animation_editor = AnimationPlayerEditor::get_singleton();
	if (animation_editor) {
		animation_editor->_play_from_pressed();
	}
}

void AsosdotAnimationWorkspace::_animation_tool(int p_tool) {
	AnimationPlayerEditor *animation_editor = AnimationPlayerEditor::get_singleton();
	if (!animation_editor || !_get_player()) {
		EditorNode::get_singleton()->show_warning(TTR("Select an AnimationPlayer in the scene first."));
		return;
	}
	animation_editor->_animation_tool_menu(p_tool);
}

/// Shots.

Vector<AsosdotAnimationWorkspace::CameraCut> AsosdotAnimationWorkspace::get_camera_cuts(const Ref<Animation> &p_animation, Node *p_root) {
	Vector<CameraCut> cuts;
	if (p_animation.is_null() || !p_root) {
		return cuts;
	}
	for (int i = 0; i < p_animation->get_track_count(); i++) {
		if (p_animation->track_get_type(i) != Animation::TYPE_VALUE) {
			continue;
		}
		const NodePath path = p_animation->track_get_path(i);
		if (path.get_subname_count() != 1 || path.get_subname(0) != SNAME("current")) {
			continue;
		}
		Camera3D *camera = Object::cast_to<Camera3D>(p_root->get_node_or_null(NodePath(path.get_concatenated_names())));
		if (!camera) {
			continue;
		}
		for (int k = 0; k < p_animation->track_get_key_count(i); k++) {
			if (bool(p_animation->track_get_key_value(i, k))) {
				CameraCut cut;
				cut.time = p_animation->track_get_key_time(i, k);
				cut.camera = camera;
				cuts.push_back(cut);
			}
		}
	}
	cuts.sort_custom<CameraCutByTime>();
	return cuts;
}

// The `current` track of every camera that has cuts, by camera.
static HashMap<Camera3D *, int> _get_cut_tracks(const Ref<Animation> &p_animation, Node *p_root) {
	HashMap<Camera3D *, int> tracks;
	for (int i = 0; i < p_animation->get_track_count(); i++) {
		if (p_animation->track_get_type(i) != Animation::TYPE_VALUE) {
			continue;
		}
		const NodePath path = p_animation->track_get_path(i);
		if (path.get_subname_count() != 1 || path.get_subname(0) != SNAME("current")) {
			continue;
		}
		Camera3D *camera = Object::cast_to<Camera3D>(p_root->get_node_or_null(NodePath(path.get_concatenated_names())));
		if (camera && !tracks.has(camera)) {
			tracks.insert(camera, i);
		}
	}
	return tracks;
}

void AsosdotAnimationWorkspace::cut_to_camera(const Ref<Animation> &p_animation, Node *p_root, Camera3D *p_camera, double p_time) {
	ERR_FAIL_COND(p_animation.is_null() || !p_root || !p_camera);
	HashMap<Camera3D *, int> tracks = _get_cut_tracks(p_animation, p_root);

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(vformat(TTR("Cut to Camera \"%s\""), p_camera->get_name()), UndoRedo::MERGE_DISABLE, p_animation.ptr());
	int new_track = p_animation->get_track_count();
	if (!tracks.has(p_camera)) {
		const NodePath path = String(p_root->get_path_to(p_camera)) + ":current";
		undo_redo->add_do_method(p_animation.ptr(), "add_track", Animation::TYPE_VALUE, -1);
		undo_redo->add_do_method(p_animation.ptr(), "track_set_path", new_track, path);
		undo_redo->add_do_method(p_animation.ptr(), "value_track_set_update_mode", new_track, Animation::UPDATE_DISCRETE);
		undo_redo->add_do_method(p_animation.ptr(), "track_set_interpolation_type", new_track, Animation::INTERPOLATION_NEAREST);
		undo_redo->add_undo_method(p_animation.ptr(), "remove_track", new_track);
		tracks.insert(p_camera, new_track);
	}
	for (const KeyValue<Camera3D *, int> &E : tracks) {
		const int track = E.value;
		const bool is_new = track >= p_animation->get_track_count();
		const bool live = E.key == p_camera;
		// Every camera with cuts is off from the start unless a cut makes it live, so a shot never
		// shows before it begins.
		if (!Math::is_zero_approx(p_time) && (is_new || p_animation->track_find_key(track, 0.0, Animation::FIND_MODE_APPROX) < 0)) {
			undo_redo->add_do_method(p_animation.ptr(), "track_insert_key", track, 0.0, false);
			if (!is_new) {
				undo_redo->add_undo_method(p_animation.ptr(), "track_remove_key_at_time", track, 0.0);
			}
		}
		const int existing = is_new ? -1 : p_animation->track_find_key(track, p_time, Animation::FIND_MODE_APPROX);
		if (existing >= 0) {
			undo_redo->add_do_method(p_animation.ptr(), "track_set_key_value", track, existing, live);
			undo_redo->add_undo_method(p_animation.ptr(), "track_set_key_value", track, existing, p_animation->track_get_key_value(track, existing));
		} else {
			undo_redo->add_do_method(p_animation.ptr(), "track_insert_key", track, p_time, live);
			if (!is_new) {
				undo_redo->add_undo_method(p_animation.ptr(), "track_remove_key_at_time", track, p_time);
			}
		}
	}
	undo_redo->commit_action();
}

void AsosdotAnimationWorkspace::remove_cut(const Ref<Animation> &p_animation, Node *p_root, double p_time) {
	ERR_FAIL_COND(p_animation.is_null() || !p_root);
	const HashMap<Camera3D *, int> tracks = _get_cut_tracks(p_animation, p_root);
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Remove Camera Cut"), UndoRedo::MERGE_DISABLE, p_animation.ptr());
	for (const KeyValue<Camera3D *, int> &E : tracks) {
		const int key = p_animation->track_find_key(E.value, p_time, Animation::FIND_MODE_APPROX);
		if (key < 0) {
			continue;
		}
		if (Math::is_zero_approx(p_time)) {
			// The start keeps a key, so cameras stay off until their first cut.
			undo_redo->add_do_method(p_animation.ptr(), "track_set_key_value", E.value, key, false);
			undo_redo->add_undo_method(p_animation.ptr(), "track_set_key_value", E.value, key, p_animation->track_get_key_value(E.value, key));
		} else {
			undo_redo->add_do_method(p_animation.ptr(), "track_remove_key_at_time", E.value, p_time);
			undo_redo->add_undo_method(p_animation.ptr(), "track_insert_key", E.value, p_animation->track_get_key_time(E.value, key), p_animation->track_get_key_value(E.value, key));
		}
	}
	undo_redo->commit_action();
}

void AsosdotAnimationWorkspace::_update_scene_cameras() {
	scene_cameras.clear();
	Node *scene = EditorNode::get_singleton()->get_edited_scene();
	if (!scene) {
		return;
	}
	if (Object::cast_to<Camera3D>(scene)) {
		scene_cameras.push_back(scene->get_instance_id());
	}
	TypedArray<Node> found = scene->find_children("*", "Camera3D", true, false);
	for (int i = 0; i < found.size(); i++) {
		scene_cameras.push_back(Object::cast_to<Node>(found[i])->get_instance_id());
	}
}

void AsosdotAnimationWorkspace::cut_to_camera_at_playhead(Camera3D *p_camera) {
	const Ref<Animation> animation = _get_animation();
	if (animation.is_null() || !_get_animation_root()) {
		EditorNode::get_singleton()->show_warning(TTR("Pick an animation to cut in first."));
		return;
	}
	ERR_FAIL_NULL(p_camera);
	double time = _get_playhead();
	AnimationPlayer *player = _get_player();
	if (player && player->is_playing() && animation->get_step() > 0.0) {
		// A cut made while playing lands on the frame being shown, as one made with the playhead
		// stopped on that frame would.
		time = CLAMP(Math::snapped(time, double(animation->get_step())), 0.0, double(animation->get_length()));
	}
	cut_to_camera(animation, _get_animation_root(), p_camera, time);
	// Next frame: the lists are rebuilt from scratch, and a Tree can't lose the item it's signalling about.
	callable_mp(this, &AsosdotAnimationWorkspace::_update_shots).call_deferred(false);
}

void AsosdotAnimationWorkspace::_update_shots(bool p_force) {
	Node *scene = EditorNode::get_singleton()->get_edited_scene();
	LocalVector<Camera3D *> cameras;
	for (const ObjectID &id : scene_cameras) {
		Camera3D *camera = ObjectDB::get_instance<Camera3D>(id);
		if (camera && camera->is_inside_tree()) {
			cameras.push_back(camera);
		}
	}
	uint32_t cameras_hash = hash_murmur3_one_64(scene ? uint64_t(scene->get_instance_id()) : 0);
	for (Camera3D *camera : cameras) {
		cameras_hash = hash_murmur3_one_64(uint64_t(camera->get_instance_id()), cameras_hash);
		cameras_hash = hash_murmur3_one_32(String(camera->get_name()).hash(), cameras_hash);
	}

	const Ref<Animation> animation = _get_animation();
	const Vector<CameraCut> cuts = get_camera_cuts(animation, _get_animation_root());
	uint32_t cuts_hash = hash_murmur3_one_64(animation.is_valid() ? uint64_t(animation->get_instance_id()) : 0);
	for (const CameraCut &cut : cuts) {
		cuts_hash = hash_murmur3_one_double(cut.time, cuts_hash);
		cuts_hash = hash_murmur3_one_64(uint64_t(cut.camera->get_instance_id()), cuts_hash);
		cuts_hash = hash_murmur3_one_32(String(cut.camera->get_name()).hash(), cuts_hash);
	}

	if (p_force || cameras_hash != listed_cameras_hash) {
		listed_cameras_hash = cameras_hash;
		camera_tree->clear();
		TreeItem *root = camera_tree->create_item();
		const Ref<Texture2D> camera_icon = get_editor_theme_icon(SNAME("Camera3D"));
		const Ref<Texture2D> cut_icon = get_editor_theme_icon(SNAME("Key"));
		for (Camera3D *camera : cameras) {
			TreeItem *item = camera_tree->create_item(root);
			item->set_text(0, camera->get_name());
			item->set_icon(0, camera_icon);
			item->set_icon_modulate(0, AsosdotAnimationTrackEditShot::get_shot_color(camera));
			item->set_metadata(0, int64_t(camera->get_instance_id()));
			item->set_tooltip_text(0, String(scene->get_path_to(camera)));
			item->add_button(0, cut_icon, 0, false, TTR("Cut to this camera at the playhead."));
		}
		if (cameras.is_empty()) {
			TreeItem *hint = camera_tree->create_item(root);
			hint->set_text(0, TTR("Add a Camera3D to the scene to cut to it."));
			hint->set_selectable(0, false);
			hint->set_custom_color(0, get_theme_color(SNAME("font_disabled_color"), EditorStringName(Editor)));
		}
	}

	if (p_force || cuts_hash != listed_cuts_hash) {
		listed_cuts_hash = cuts_hash;
		cut_tree->clear();
		TreeItem *root = cut_tree->create_item();
		const Ref<Texture2D> remove_icon = get_editor_theme_icon(SNAME("Remove"));
		for (const CameraCut &cut : cuts) {
			TreeItem *item = cut_tree->create_item(root);
			item->set_text(0, format_time(cut.time) + "   " + String(cut.camera->get_name()));
			item->set_custom_color(0, AsosdotAnimationTrackEditShot::get_shot_color(cut.camera));
			item->set_metadata(0, cut.time);
			item->add_button(0, remove_icon, 0, false, TTR("Remove this cut."));
		}
		if (cuts.is_empty()) {
			TreeItem *hint = cut_tree->create_item(root);
			hint->set_text(0, TTR("No cuts yet. Move the playhead and press a camera's key button."));
			hint->set_selectable(0, false);
			hint->set_custom_color(0, get_theme_color(SNAME("font_disabled_color"), EditorStringName(Editor)));
		}
	}
}

void AsosdotAnimationWorkspace::_camera_activated() {
	TreeItem *item = camera_tree->get_selected();
	if (!item || item->get_metadata(0).get_type() != Variant::INT) {
		return;
	}
	Camera3D *camera = ObjectDB::get_instance<Camera3D>(ObjectID(uint64_t(int64_t(item->get_metadata(0)))));
	if (camera) {
		EditorSelection *selection = EditorNode::get_singleton()->get_editor_selection();
		selection->clear();
		selection->add_node(camera);
		EditorNode::get_singleton()->edit_node(camera);
	}
}

void AsosdotAnimationWorkspace::_camera_button(TreeItem *p_item, int p_column, int p_id, MouseButton p_button) {
	if (p_button != MouseButton::LEFT || p_item->get_metadata(0).get_type() != Variant::INT) {
		return;
	}
	Camera3D *camera = ObjectDB::get_instance<Camera3D>(ObjectID(uint64_t(int64_t(p_item->get_metadata(0)))));
	if (camera) {
		cut_to_camera_at_playhead(camera);
	}
}

void AsosdotAnimationWorkspace::_cut_selected() {
	TreeItem *item = cut_tree->get_selected();
	if (item && item->get_metadata(0).get_type() == Variant::FLOAT) {
		_set_playhead(item->get_metadata(0));
	}
}

void AsosdotAnimationWorkspace::_cut_button(TreeItem *p_item, int p_column, int p_id, MouseButton p_button) {
	if (p_button != MouseButton::LEFT || p_item->get_metadata(0).get_type() != Variant::FLOAT) {
		return;
	}
	remove_cut(_get_animation(), _get_animation_root(), p_item->get_metadata(0));
	callable_mp(this, &AsosdotAnimationWorkspace::_update_shots).call_deferred(false);
}

void AsosdotAnimationWorkspace::_cut_to_selected_camera() {
	if (_get_animation().is_null() || !_get_animation_root()) {
		EditorNode::get_singleton()->show_warning(TTR("Pick an animation to cut in first."));
		return;
	}
	Camera3D *camera = nullptr;
	for (Node *node : EditorNode::get_singleton()->get_editor_selection()->get_top_selected_node_list()) {
		camera = Object::cast_to<Camera3D>(node);
		if (camera) {
			break;
		}
	}
	if (!camera && camera_tree->get_selected() && camera_tree->get_selected()->get_metadata(0).get_type() == Variant::INT) {
		camera = ObjectDB::get_instance<Camera3D>(ObjectID(uint64_t(int64_t(camera_tree->get_selected()->get_metadata(0)))));
	}
	if (!camera) {
		EditorNode::get_singleton()->show_warning(TTR("Select the Camera3D to cut to, in the scene or the Shots list."));
		return;
	}
	cut_to_camera_at_playhead(camera);
}

/// Sounds.

static void _collect_sounds(EditorFileSystemDirectory *p_dir, LocalVector<String> &r_paths) {
	if (!p_dir) {
		return;
	}
	for (int i = 0; i < p_dir->get_file_count(); i++) {
		if (ClassDB::is_parent_class(p_dir->get_file_type(i), "AudioStream")) {
			r_paths.push_back(p_dir->get_file_path(i));
		}
	}
	for (int i = 0; i < p_dir->get_subdir_count(); i++) {
		_collect_sounds(p_dir->get_subdir(i), r_paths);
	}
}

void AsosdotAnimationWorkspace::_update_sounds() {
	LocalVector<String> paths;
	_collect_sounds(EditorFileSystem::get_singleton()->get_filesystem(), paths);
	paths.sort_custom<FileNoCaseComparator>();
	const String filter = sound_search->get_text().strip_edges();

	sound_tree->clear();
	TreeItem *root = sound_tree->create_item();
	const Ref<Texture2D> add_icon = get_editor_theme_icon(SNAME("Add"));
	for (const String &path : paths) {
		if (!filter.is_empty() && !path.containsn(filter)) {
			continue;
		}
		TreeItem *item = sound_tree->create_item(root);
		item->set_text(0, path.get_file());
		item->set_suffix(0, path.get_base_dir().trim_prefix("res://"));
		item->set_icon(0, get_editor_theme_icon(SNAME("AudioStreamPlayer")));
		item->set_metadata(0, path);
		item->set_tooltip_text(0, path + "\n" + TTR("Double-click to listen, drag onto an audio track, or press + to add it at the playhead to the selected audio player."));
		item->add_button(0, add_icon, 0, false, TTR("Add at the playhead, on the selected AudioStreamPlayer's audio track."));
	}
	if (paths.is_empty()) {
		TreeItem *hint = sound_tree->create_item(root);
		hint->set_text(0, TTR("No sounds in the project yet."));
		hint->set_selectable(0, false);
		hint->set_custom_color(0, get_theme_color(SNAME("font_disabled_color"), EditorStringName(Editor)));
	}
}

void AsosdotAnimationWorkspace::_sound_activated() {
	TreeItem *item = sound_tree->get_selected();
	if (!item || item->get_metadata(0).get_type() != Variant::STRING) {
		return;
	}
	const String path = item->get_metadata(0);
	if (sound_preview->is_playing() && previewing_sound == path) {
		sound_preview->stop();
		return;
	}
	Ref<AudioStream> stream = ResourceLoader::load(path);
	if (stream.is_valid()) {
		previewing_sound = path;
		sound_preview->set_stream(stream);
		sound_preview->play();
	}
}

void AsosdotAnimationWorkspace::_sound_button(TreeItem *p_item, int p_column, int p_id, MouseButton p_button) {
	if (p_button == MouseButton::LEFT && p_item->get_metadata(0).get_type() == Variant::STRING) {
		_add_sound_at_playhead(p_item->get_metadata(0));
	}
}

Variant AsosdotAnimationWorkspace::_sound_drag_data(const Point2 &p_point) {
	TreeItem *item = sound_tree->get_item_at_position(p_point);
	if (!item || item->get_metadata(0).get_type() != Variant::STRING) {
		return Variant();
	}
	// The same as dragging from the FileSystem dock, which audio tracks accept.
	Vector<String> paths;
	paths.push_back(item->get_metadata(0));
	return EditorNode::get_singleton()->drag_files_and_dirs(paths, sound_tree);
}

void AsosdotAnimationWorkspace::_add_sound_at_playhead(const String &p_path) {
	const Ref<Animation> animation = _get_animation();
	Node *root = _get_animation_root();
	if (animation.is_null() || !root) {
		EditorNode::get_singleton()->show_warning(TTR("Pick an animation to add the sound to first."));
		return;
	}
	Node *player = nullptr;
	for (Node *node : EditorNode::get_singleton()->get_editor_selection()->get_top_selected_node_list()) {
		if (node->has_method(SNAME("play_layered"))) {
			player = node;
			break;
		}
	}
	if (!player) {
		EditorNode::get_singleton()->show_warning(TTR("Select the AudioStreamPlayer, AudioStreamPlayer2D or AudioStreamPlayer3D to play the sound."));
		return;
	}
	Ref<AudioStream> stream = ResourceLoader::load(p_path);
	ERR_FAIL_COND(stream.is_null());

	const NodePath path = root->get_path_to(player);
	int track = -1;
	for (int i = 0; i < animation->get_track_count(); i++) {
		if (animation->track_get_type(i) == Animation::TYPE_AUDIO && animation->track_get_path(i) == path) {
			track = i;
			break;
		}
	}
	double time = _get_playhead();
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Add Audio Clip"), UndoRedo::MERGE_DISABLE, animation.ptr());
	if (track < 0) {
		track = animation->get_track_count();
		undo_redo->add_do_method(animation.ptr(), "add_track", Animation::TYPE_AUDIO, -1);
		undo_redo->add_do_method(animation.ptr(), "track_set_path", track, path);
		undo_redo->add_undo_method(animation.ptr(), "remove_track", track);
	} else {
		// A key already there would be replaced, so the clip goes in just after it.
		while (animation->track_find_key(track, time, Animation::FIND_MODE_APPROX) >= 0) {
			time += 0.0001;
		}
		undo_redo->add_undo_method(animation.ptr(), "track_remove_key_at_time", track, time);
	}
	undo_redo->add_do_method(animation.ptr(), "audio_track_insert_key", track, time, stream);
	undo_redo->commit_action();
}

/// Poses.

Skeleton3D *AsosdotAnimationWorkspace::_get_selected_skeleton() const {
	for (Node *node : EditorNode::get_singleton()->get_editor_selection()->get_top_selected_node_list()) {
		if (Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(node)) {
			return skeleton;
		}
	}
	Skeleton3DEditor *skeleton_editor = Skeleton3DEditor::get_singleton();
	return skeleton_editor && skeleton_editor->is_visible_in_tree() ? skeleton_editor->get_skeleton() : nullptr;
}

Ref<Animation> AsosdotAnimationWorkspace::make_pose(Skeleton3D *p_skeleton, const Vector<int> &p_bones) {
	ERR_FAIL_NULL_V(p_skeleton, Ref<Animation>());
	Ref<Animation> pose;
	pose.instantiate();
	pose->set_length(0.1);
	const String skeleton_path = p_skeleton->get_name();
	for (const int bone : p_bones) {
		const NodePath path = skeleton_path + ":" + p_skeleton->get_bone_name(bone);
		const int position = pose->add_track(Animation::TYPE_POSITION_3D);
		pose->track_set_path(position, path);
		pose->position_track_insert_key(position, 0.0, p_skeleton->get_bone_pose_position(bone));
		const int rotation = pose->add_track(Animation::TYPE_ROTATION_3D);
		pose->track_set_path(rotation, path);
		pose->rotation_track_insert_key(rotation, 0.0, p_skeleton->get_bone_pose_rotation(bone));
		const int scale = pose->add_track(Animation::TYPE_SCALE_3D);
		pose->track_set_path(scale, path);
		pose->scale_track_insert_key(scale, 0.0, p_skeleton->get_bone_pose_scale(bone));
	}
	return pose;
}

void AsosdotAnimationWorkspace::apply_pose(Skeleton3D *p_skeleton, const Ref<Animation> &p_pose, float p_mix, int p_only_bone) {
	ERR_FAIL_NULL(p_skeleton);
	ERR_FAIL_COND(p_pose.is_null());
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Apply Pose"), UndoRedo::MERGE_DISABLE, p_skeleton);
	for (int i = 0; i < p_pose->get_track_count(); i++) {
		const NodePath path = p_pose->track_get_path(i);
		if (path.get_subname_count() != 1 || p_pose->track_get_key_count(i) == 0) {
			continue;
		}
		const int bone = p_skeleton->find_bone(path.get_subname(0));
		if (bone < 0 || (p_only_bone >= 0 && bone != p_only_bone)) {
			continue;
		}
		const Variant value = p_pose->track_get_key_value(i, 0);
		switch (p_pose->track_get_type(i)) {
			case Animation::TYPE_POSITION_3D: {
				const Vector3 current = p_skeleton->get_bone_pose_position(bone);
				undo_redo->add_do_method(p_skeleton, "set_bone_pose_position", bone, current.lerp(value, p_mix));
				undo_redo->add_undo_method(p_skeleton, "set_bone_pose_position", bone, current);
			} break;
			case Animation::TYPE_ROTATION_3D: {
				const Quaternion current = p_skeleton->get_bone_pose_rotation(bone);
				undo_redo->add_do_method(p_skeleton, "set_bone_pose_rotation", bone, current.slerp(value, p_mix));
				undo_redo->add_undo_method(p_skeleton, "set_bone_pose_rotation", bone, current);
			} break;
			case Animation::TYPE_SCALE_3D: {
				const Vector3 current = p_skeleton->get_bone_pose_scale(bone);
				undo_redo->add_do_method(p_skeleton, "set_bone_pose_scale", bone, current.lerp(value, p_mix));
				undo_redo->add_undo_method(p_skeleton, "set_bone_pose_scale", bone, current);
			} break;
			default:
				break;
		}
	}
	undo_redo->commit_action();
}

void AsosdotAnimationWorkspace::_update_poses() {
	pose_list->clear();
	if (pose_library.is_null()) {
		pose_library_label->set_text(TTR("No pose library. Open one, or make a new one, to save poses in."));
	} else {
		pose_library_label->set_text(pose_library_path);
		LocalVector<StringName> names;
		pose_library->get_animation_list(&names);
		for (const StringName &name : names) {
			pose_list->add_item(name, get_editor_theme_icon(SNAME("Skeleton3D")));
		}
	}
	Skeleton3D *skeleton = _get_selected_skeleton();
	pose_hint->set_text(skeleton ? vformat(TTR("Posing \"%s\"."), skeleton->get_name()) : TTR("Select a Skeleton3D to save or apply poses."));
}

void AsosdotAnimationWorkspace::_pose_library_open() {
	pose_file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILE);
	pose_file_dialog->set_title(TTR("Open Pose Library"));
	pose_file_dialog->popup_file_dialog();
}

void AsosdotAnimationWorkspace::_pose_library_new() {
	pose_file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	pose_file_dialog->set_title(TTR("New Pose Library"));
	pose_file_dialog->popup_file_dialog();
}

void AsosdotAnimationWorkspace::_pose_library_chosen(const String &p_path) {
	Ref<AnimationLibrary> library;
	if (ResourceLoader::exists(p_path)) {
		library = ResourceLoader::load(p_path);
		if (library.is_null()) {
			EditorNode::get_singleton()->show_warning(TTR("That file isn't an animation library."));
			return;
		}
	} else {
		library.instantiate();
		const Error err = ResourceSaver::save(library, p_path);
		ERR_FAIL_COND_MSG(err != OK, vformat("Couldn't save the pose library to \"%s\".", p_path));
		library->set_path(p_path);
	}
	pose_library = library;
	pose_library_path = p_path;
	EditorSettings::get_singleton()->set_project_metadata("animation_workspace", "pose_library", p_path);
	_update_poses();
}

void AsosdotAnimationWorkspace::_save_pose_requested() {
	if (pose_library.is_null()) {
		_pose_library_new();
		return;
	}
	if (!_get_selected_skeleton()) {
		EditorNode::get_singleton()->show_warning(TTR("Select the Skeleton3D whose pose to save."));
		return;
	}
	pose_name->set_text(vformat(TTR("Pose %d"), pose_library->get_animation_list_size() + 1));
	pose_name_dialog->popup_centered(Size2(300, 0) * EDSCALE);
	pose_name->select_all();
	pose_name->grab_focus();
}

void AsosdotAnimationWorkspace::_save_pose_confirmed() {
	Skeleton3D *skeleton = _get_selected_skeleton();
	const String name = pose_name->get_text().strip_edges().validate_node_name();
	if (!skeleton || pose_library.is_null() || name.is_empty()) {
		return;
	}
	Vector<int> bones;
	for (int i = 0; i < skeleton->get_bone_count(); i++) {
		bones.push_back(i);
	}
	if (pose_library->has_animation(name)) {
		pose_library->remove_animation(name);
	}
	pose_library->add_animation(name, make_pose(skeleton, bones));
	const Error err = ResourceSaver::save(pose_library, pose_library_path);
	ERR_FAIL_COND_MSG(err != OK, vformat("Couldn't save the pose library to \"%s\".", pose_library_path));
	_update_poses();
}

void AsosdotAnimationWorkspace::_apply_pose(bool p_selected_bone_only) {
	Skeleton3D *skeleton = _get_selected_skeleton();
	if (!skeleton) {
		EditorNode::get_singleton()->show_warning(TTR("Select the Skeleton3D to pose."));
		return;
	}
	const PackedInt32Array selected = pose_list->get_selected_items();
	if (pose_library.is_null() || selected.is_empty()) {
		return;
	}
	int only_bone = -1;
	if (p_selected_bone_only) {
		Skeleton3DEditor *skeleton_editor = Skeleton3DEditor::get_singleton();
		only_bone = skeleton_editor && skeleton_editor->get_skeleton() == skeleton ? skeleton_editor->get_selected_bone() : -1;
		if (only_bone < 0) {
			EditorNode::get_singleton()->show_warning(TTR("Select a bone in the Skeleton3D first."));
			return;
		}
	}
	apply_pose(skeleton, pose_library->get_animation(pose_list->get_item_text(selected[0])), pose_mix->get_value() / 100.0, only_bone);
}

void AsosdotAnimationWorkspace::_delete_pose() {
	const PackedInt32Array selected = pose_list->get_selected_items();
	if (pose_library.is_null() || selected.is_empty()) {
		return;
	}
	pose_library->remove_animation(pose_list->get_item_text(selected[0]));
	ResourceSaver::save(pose_library, pose_library_path);
	_update_poses();
}

/// Recording.

bool AsosdotAnimationWorkspace::is_recording() const {
	return active && record_button->is_pressed();
}

uint64_t AsosdotAnimationWorkspace::get_scene_history_version() {
	UndoRedo *undo_redo = EditorUndoRedoManager::get_singleton()->get_history_undo_redo(EditorNode::get_editor_data().get_current_edited_scene_history_id());
	return undo_redo ? undo_redo->get_version() : 0;
}

void AsosdotAnimationWorkspace::_record_toggled(bool p_pressed) {
	record_button->set_self_modulate(p_pressed ? Color(1.0, 0.35, 0.35) : Color(1, 1, 1));
	recorded_properties.clear();
	_take_record_snapshot();
	recorded_version = get_scene_history_version();
}

void AsosdotAnimationWorkspace::_record_snapshot() {
	if (record_flush_queued) {
		return; // An edit waits to be compared with the snapshot as it was before it.
	}
	_take_record_snapshot();
}

void AsosdotAnimationWorkspace::_take_record_snapshot() {
	recorded.clear();
	if (!is_recording()) {
		return;
	}
	for (Node *node : EditorNode::get_singleton()->get_editor_selection()->get_full_selected_node_list()) {
		Node3D *node_3d = Object::cast_to<Node3D>(node);
		if (!node_3d) {
			continue;
		}
		RecordedNode state;
		state.position = node_3d->get_position();
		state.rotation = node_3d->get_quaternion();
		state.scale = node_3d->get_scale();
		if (Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(node_3d)) {
			for (int i = 0; i < skeleton->get_bone_count(); i++) {
				state.bone_positions.push_back(skeleton->get_bone_pose_position(i));
				state.bone_rotations.push_back(skeleton->get_bone_pose_rotation(i));
				state.bone_scales.push_back(skeleton->get_bone_pose_scale(i));
			}
		}
		recorded.insert(node_3d->get_instance_id(), state);
	}
}

void AsosdotAnimationWorkspace::_record_version_changed() {
	if (!is_recording()) {
		return;
	}
	const uint64_t version = get_scene_history_version();
	if (record_inserting) {
		recorded_version = version; // The keys recording itself inserts.
		return;
	}
	if (version == recorded_version) {
		return; // Another history changed, or nothing was done.
	}
	const bool edited = version > recorded_version;
	recorded_version = version;
	if (!edited) {
		// An undo: what's in the scene now is the starting point again, not something to key.
		recorded_properties.clear();
		callable_mp(this, &AsosdotAnimationWorkspace::_record_snapshot).call_deferred();
		return;
	}
	if (!record_flush_queued) {
		record_flush_queued = true;
		// After the edit's action has finished committing.
		callable_mp(this, &AsosdotAnimationWorkspace::_record_flush).call_deferred();
	}
}

void AsosdotAnimationWorkspace::_record_property_edited(const String &p_property) {
	if (!is_recording()) {
		return;
	}
	Node *node = Object::cast_to<Node>(InspectorDock::get_inspector_singleton()->get_edited_object());
	if (!node) {
		return;
	}
	// Transforms and bone poses are compared after the edit, which also catches gizmo moves.
	static const char *compared[] = { "position", "rotation", "rotation_degrees", "quaternion", "basis", "scale", "transform", "global_position", "global_rotation", "global_rotation_degrees", "global_basis", "global_transform", nullptr };
	if (Object::cast_to<Node3D>(node)) {
		for (int i = 0; compared[i]; i++) {
			if (p_property == compared[i]) {
				return;
			}
		}
	}
	if (p_property.begins_with("bones/")) {
		return;
	}
	recorded_properties.insert(Pair<ObjectID, StringName>(node->get_instance_id(), StringName(p_property)));
}

void AsosdotAnimationWorkspace::_record_flush() {
	record_flush_queued = false;
	AnimationTrackEditor *track_editor = _get_track_editor();
	Node *root = _get_animation_root();
	if (!is_recording() || !track_editor || !root || _get_animation().is_null() || !track_editor->has_keying() || track_editor->is_read_only()) {
		recorded_properties.clear();
		_take_record_snapshot();
		return;
	}

	// What changed since the snapshot, gathered first so nothing is queued (and no empty action is
	// made) when the edit touched nothing recorded.
	struct TransformKey {
		Node3D *node = nullptr;
		String sub;
		Animation::TrackType type = Animation::TYPE_POSITION_3D;
		Variant value;
	};
	LocalVector<TransformKey> transform_keys;
	for (const KeyValue<ObjectID, RecordedNode> &E : recorded) {
		Node3D *node = ObjectDB::get_instance<Node3D>(E.key);
		if (!node || (node != root && !root->is_ancestor_of(node))) {
			continue;
		}
		const RecordedNode &was = E.value;
		if (!node->get_position().is_equal_approx(was.position)) {
			transform_keys.push_back({ node, String(), Animation::TYPE_POSITION_3D, node->get_position() });
		}
		if (!node->get_quaternion().is_equal_approx(was.rotation)) {
			transform_keys.push_back({ node, String(), Animation::TYPE_ROTATION_3D, node->get_quaternion() });
		}
		if (!node->get_scale().is_equal_approx(was.scale)) {
			transform_keys.push_back({ node, String(), Animation::TYPE_SCALE_3D, node->get_scale() });
		}
		Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(node);
		if (!skeleton || int(was.bone_rotations.size()) != skeleton->get_bone_count()) {
			continue;
		}
		for (int i = 0; i < skeleton->get_bone_count(); i++) {
			const String bone = skeleton->get_bone_name(i);
			if (!skeleton->get_bone_pose_position(i).is_equal_approx(was.bone_positions[i])) {
				transform_keys.push_back({ skeleton, bone, Animation::TYPE_POSITION_3D, skeleton->get_bone_pose_position(i) });
			}
			if (!skeleton->get_bone_pose_rotation(i).is_equal_approx(was.bone_rotations[i])) {
				transform_keys.push_back({ skeleton, bone, Animation::TYPE_ROTATION_3D, skeleton->get_bone_pose_rotation(i) });
			}
			if (!skeleton->get_bone_pose_scale(i).is_equal_approx(was.bone_scales[i])) {
				transform_keys.push_back({ skeleton, bone, Animation::TYPE_SCALE_3D, skeleton->get_bone_pose_scale(i) });
			}
		}
	}
	LocalVector<Pair<Node *, StringName>> value_keys;
	for (const Pair<ObjectID, StringName> &property : recorded_properties) {
		Node *node = ObjectDB::get_instance<Node>(property.first);
		if (node && (node == root || root->is_ancestor_of(node))) {
			value_keys.push_back(Pair<Node *, StringName>(node, property.second));
		}
	}
	recorded_properties.clear();

	if (!transform_keys.is_empty() || !value_keys.is_empty()) {
		track_editor->make_insert_queue();
		for (const TransformKey &key : transform_keys) {
			track_editor->insert_transform_key(key.node, key.sub, key.type, key.value);
		}
		for (const Pair<Node *, StringName> &key : value_keys) {
			track_editor->insert_node_value_key(key.first, key.second, false, false);
		}
		// Recording makes the tracks it needs without asking each time.
		record_inserting = true;
		track_editor->commit_insert_queue(false);
		record_inserting = false;
	}
	_take_record_snapshot();
	recorded_version = get_scene_history_version();
}

/// Workspace.

void AsosdotAnimationWorkspace::set_active(bool p_active) {
	if (active == p_active) {
		return;
	}
	active = p_active;
	toolbar->set_visible(active);
	set_visible(active);
	set_process(active);

	if (active) {
		// The timeline is the Animation dock in the bottom panel, made tall.
		AnimationPlayerEditor::get_singleton()->make_visible();
		saved_center_split = EditorNode::get_center_split()->get_split_offset();
		saved_center_split_valid = true;
		AnimationTrackEditor *track_editor = _get_track_editor();
		if (track_editor && !track_editor->is_connected(SNAME("graph_editor_toggled"), callable_mp(this, &AsosdotAnimationWorkspace::_graph_editor_toggled))) {
			track_editor->connect(SNAME("graph_editor_toggled"), callable_mp(this, &AsosdotAnimationWorkspace::_graph_editor_toggled));
		}
		const bool graph = track_editor && track_editor->is_graph_editor_active();
		graph_button->set_pressed_no_signal(graph);
		callable_mp(this, &AsosdotAnimationWorkspace::_enlarge_timeline).call_deferred(graph ? GRAPH_TIMELINE_SHARE : TIMELINE_SHARE);
		_apply_docks();
		if (shot_button->is_pressed()) {
			_shot_toggled(true);
		}
		_update_scene_cameras();
		monitor->set_active(monitor_button->is_pressed());
		_update_animations(true);
		_update_shots(true);
		_update_sounds();
		_update_poses();
		_watch_player();
		_take_record_snapshot();
		recorded_version = get_scene_history_version();
	} else {
		EditorDockManager::get_singleton()->set_side_docks_hidden(false);
		if (saved_center_split_valid) {
			EditorNode::get_singleton()->set_center_split_offset(saved_center_split);
			saved_center_split_valid = false;
		}
		if (shot_button->is_pressed() && Node3DEditor::get_singleton()) {
			Node3DEditor::get_singleton()->get_editor_viewport(0)->set_cinematic_preview(false);
		}
		monitor->set_active(false);
		sound_preview->stop();
	}
}

void AsosdotAnimationWorkspace::_enlarge_timeline(float p_share) {
	DockSplitContainer *center = EditorNode::get_center_split();
	if (!active || center->get_child_count() < 2) {
		return;
	}
	Control *bottom = Object::cast_to<Control>(center->get_child(center->get_child_count() - 1));
	if (!bottom || !bottom->is_visible()) {
		return;
	}
	const float wanted = center->get_size().y * p_share;
	if (bottom->get_size().y < wanted) {
		center->set_split_offset(center->get_split_offset() - int(wanted - bottom->get_size().y));
	}
}

Dictionary AsosdotAnimationWorkspace::get_layout() const {
	Dictionary layout;
	layout["tab"] = tabs->get_current_tab();
	layout["shot"] = shot_button->is_pressed();
	layout["monitor"] = monitor_button->is_pressed();
	layout["camera_monitor"] = monitor->get_layout();
	layout["docks"] = docks_button->is_pressed();
	layout["pose_mix"] = pose_mix->get_value();
	return layout;
}

void AsosdotAnimationWorkspace::set_layout(const Dictionary &p_layout) {
	tabs->set_current_tab(CLAMP(int(p_layout.get("tab", 0)), 0, tabs->get_tab_count() - 1));
	shot_button->set_pressed_no_signal(p_layout.get("shot", false));
	monitor_button->set_pressed_no_signal(p_layout.get("monitor", true));
	monitor->set_layout(p_layout.get("camera_monitor", Dictionary()));
	docks_button->set_pressed_no_signal(p_layout.get("docks", false));
	pose_mix->set_value(p_layout.get("pose_mix", 100.0));
	if (active) {
		_apply_docks();
		_shot_toggled(shot_button->is_pressed());
		monitor->set_active(monitor_button->is_pressed());
	}
}

void AsosdotAnimationWorkspace::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			// An edit is committed with "history_changed"; undo and redo send "version_changed".
			EditorUndoRedoManager::get_singleton()->connect(SNAME("history_changed"), callable_mp(this, &AsosdotAnimationWorkspace::_record_version_changed));
			EditorUndoRedoManager::get_singleton()->connect(SNAME("version_changed"), callable_mp(this, &AsosdotAnimationWorkspace::_record_version_changed));
			EditorNode::get_singleton()->get_editor_selection()->connect(SNAME("selection_changed"), callable_mp(this, &AsosdotAnimationWorkspace::_record_snapshot));
			EditorNode::get_singleton()->get_editor_selection()->connect(SNAME("selection_changed"), callable_mp(this, &AsosdotAnimationWorkspace::_update_poses));
			InspectorDock::get_inspector_singleton()->connect(SNAME("property_edited"), callable_mp(this, &AsosdotAnimationWorkspace::_record_property_edited));
			EditorFileSystem::get_singleton()->connect(SNAME("filesystem_changed"), callable_mp(this, &AsosdotAnimationWorkspace::_update_sounds));

			const String saved_library = EditorSettings::get_singleton()->get_project_metadata("animation_workspace", "pose_library", String());
			if (!saved_library.is_empty() && ResourceLoader::exists(saved_library)) {
				pose_library = ResourceLoader::load(saved_library);
				pose_library_path = pose_library.is_valid() ? saved_library : String();
			}
		} break;

		case NOTIFICATION_THEME_CHANGED: {
			record_button->set_button_icon(get_editor_theme_icon(SNAME("AutoKey")));
			shot_button->set_button_icon(get_editor_theme_icon(SNAME("Camera3D")));
			monitor_button->set_button_icon(get_editor_theme_icon(SNAME("Panels2Alt")));
			graph_button->set_button_icon(get_editor_theme_icon(SNAME("Curve")));
			cut_button->set_button_icon(get_editor_theme_icon(SNAME("Key")));
			animation_search->set_right_icon(get_editor_theme_icon(SNAME("Search")));
			sound_search->set_right_icon(get_editor_theme_icon(SNAME("Search")));
		} break;

		case NOTIFICATION_PROCESS: {
			_update_timecode();
			_watch_player();
			// Lists follow the scene and animation as they change; cheap enough to check each frame.
			_update_scene_cameras();
			_update_animations();
			_update_shots();
		} break;
	}
}

void AsosdotAnimationWorkspace::_bind_methods() {
}

static Button *_make_tool_button(const String &p_text, const String &p_tooltip) {
	Button *button = memnew(Button);
	button->set_text(p_text);
	button->set_tooltip_text(p_tooltip);
	button->set_theme_type_variation("FlatButton");
	return button;
}

AsosdotAnimationWorkspace::AsosdotAnimationWorkspace() {
	set_name(TTRC("Animation Library"));
	set_custom_minimum_size(Size2(260 * EDSCALE, 0));

	// The toolbar, shown in the 3D editor's menu bar.
	toolbar = memnew(HBoxContainer);
	toolbar->add_child(memnew(VSeparator));
	record_button = _make_tool_button(TTR("Record"), TTR("While on, moving, rotating and scaling things in the viewer, posing bones and editing properties in the Inspector puts keys at the playhead."));
	record_button->set_toggle_mode(true);
	record_button->connect(SceneStringName(toggled), callable_mp(this, &AsosdotAnimationWorkspace::_record_toggled));
	toolbar->add_child(record_button);
	shot_button = _make_tool_button(TTR("Shot"), TTR("Look through the camera that's live at the playhead, as the camera cuts switch between them."));
	shot_button->set_toggle_mode(true);
	shot_button->connect(SceneStringName(toggled), callable_mp(this, &AsosdotAnimationWorkspace::_shot_toggled));
	toolbar->add_child(shot_button);
	monitor_button = _make_tool_button(TTR("Monitor"), TTR("Show the camera monitor beside the viewer: the live shot or any camera, with frame guides, and every camera in a strip to cut between."));
	monitor_button->set_toggle_mode(true);
	monitor_button->set_pressed(true);
	monitor_button->connect(SceneStringName(toggled), callable_mp(this, &AsosdotAnimationWorkspace::_monitor_toggled));
	toolbar->add_child(monitor_button);
	graph_button = _make_tool_button(TTR("Graph"), TTR("Show the Graph Editor in the timeline: every animated number as a curve, edited as in Blender (Ctrl+Tab)."));
	graph_button->set_toggle_mode(true);
	graph_button->connect(SceneStringName(toggled), callable_mp(this, &AsosdotAnimationWorkspace::_graph_toggled));
	toolbar->add_child(graph_button);
	cut_button = _make_tool_button(TTR("Cut"), TTR("Cut to the selected camera at the playhead."));
	cut_button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotAnimationWorkspace::_cut_to_selected_camera));
	toolbar->add_child(cut_button);
	docks_button = _make_tool_button(TTR("Docks"), TTR("Show the editor's other side docks in this workspace too."));
	docks_button->set_toggle_mode(true);
	docks_button->connect(SceneStringName(toggled), callable_mp(this, &AsosdotAnimationWorkspace::_docks_toggled));
	toolbar->add_child(docks_button);
	toolbar->add_child(memnew(VSeparator));
	timecode = memnew(Label);
	timecode->set_custom_minimum_size(Size2(130 * EDSCALE, 0));
	timecode->set_tooltip_text(TTR("The playhead's time, and its frame at the animation's step."));
	timecode->set_mouse_filter(MOUSE_FILTER_PASS);
	toolbar->add_child(timecode);
	toolbar->hide();

	tabs = memnew(TabContainer);
	tabs->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(tabs);

	// Animations.
	VBoxContainer *animations = memnew(VBoxContainer);
	animations->set_name(TTRC("Animations"));
	tabs->add_child(animations);
	animation_search = memnew(LineEdit);
	animation_search->set_placeholder(TTR("Filter Animations"));
	animation_search->set_clear_button_enabled(true);
	animation_search->connect(SceneStringName(text_changed), callable_mp(this, &AsosdotAnimationWorkspace::_update_animations).bind(true).unbind(1));
	animations->add_child(animation_search);
	animation_tree = memnew(Tree);
	animation_tree->set_v_size_flags(SIZE_EXPAND_FILL);
	animation_tree->set_hide_root(true);
	animation_tree->set_tooltip_text(TTR("Click to edit an animation; double-click to play it from the start."));
	animation_tree->connect(SceneStringName(item_selected), callable_mp(this, &AsosdotAnimationWorkspace::_animation_selected));
	animation_tree->connect("item_activated", callable_mp(this, &AsosdotAnimationWorkspace::_animation_activated));
	animations->add_child(animation_tree);
	HFlowContainer *animation_buttons = memnew(HFlowContainer);
	animations->add_child(animation_buttons);
	struct AnimationButton {
		const char *text;
		int tool;
	};
	const AnimationButton animation_tools[] = {
		{ TTRC("New"), AnimationPlayerEditor::TOOL_NEW_ANIM },
		{ TTRC("Duplicate"), AnimationPlayerEditor::TOOL_DUPLICATE_ANIM },
		{ TTRC("Rename"), AnimationPlayerEditor::TOOL_RENAME_ANIM },
		{ TTRC("Delete"), AnimationPlayerEditor::TOOL_REMOVE_ANIM },
	};
	for (const AnimationButton &tool : animation_tools) {
		Button *button = memnew(Button);
		button->set_text(TTRGET(tool.text));
		button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotAnimationWorkspace::_animation_tool).bind(tool.tool));
		animation_buttons->add_child(button);
	}

	// Shots.
	VSplitContainer *shots = memnew(VSplitContainer);
	shots->set_name(TTRC("Shots"));
	tabs->add_child(shots);
	VBoxContainer *cameras_box = memnew(VBoxContainer);
	cameras_box->set_v_size_flags(SIZE_EXPAND_FILL);
	shots->add_child(cameras_box);
	Label *cameras_label = memnew(Label(TTR("Cameras")));
	cameras_label->set_theme_type_variation("HeaderSmall");
	cameras_box->add_child(cameras_label);
	camera_tree = memnew(Tree);
	camera_tree->set_v_size_flags(SIZE_EXPAND_FILL);
	camera_tree->set_hide_root(true);
	camera_tree->set_tooltip_text(TTR("Double-click a camera to select it. Its key button cuts to it at the playhead."));
	camera_tree->connect("item_activated", callable_mp(this, &AsosdotAnimationWorkspace::_camera_activated));
	camera_tree->connect("button_clicked", callable_mp(this, &AsosdotAnimationWorkspace::_camera_button));
	cameras_box->add_child(camera_tree);
	VBoxContainer *cuts_box = memnew(VBoxContainer);
	cuts_box->set_v_size_flags(SIZE_EXPAND_FILL);
	shots->add_child(cuts_box);
	Label *cuts_label = memnew(Label(TTR("Cuts in this animation")));
	cuts_label->set_theme_type_variation("HeaderSmall");
	cuts_box->add_child(cuts_label);
	cut_tree = memnew(Tree);
	cut_tree->set_v_size_flags(SIZE_EXPAND_FILL);
	cut_tree->set_hide_root(true);
	cut_tree->set_tooltip_text(TTR("Click a cut to move the playhead to it."));
	cut_tree->connect(SceneStringName(item_selected), callable_mp(this, &AsosdotAnimationWorkspace::_cut_selected));
	cut_tree->connect("button_clicked", callable_mp(this, &AsosdotAnimationWorkspace::_cut_button));
	cuts_box->add_child(cut_tree);

	// Sounds.
	VBoxContainer *sounds = memnew(VBoxContainer);
	sounds->set_name(TTRC("Sounds"));
	tabs->add_child(sounds);
	sound_search = memnew(LineEdit);
	sound_search->set_placeholder(TTR("Filter Sounds"));
	sound_search->set_clear_button_enabled(true);
	sound_search->connect(SceneStringName(text_changed), callable_mp(this, &AsosdotAnimationWorkspace::_update_sounds).unbind(1));
	sounds->add_child(sound_search);
	sound_tree = memnew(Tree);
	sound_tree->set_v_size_flags(SIZE_EXPAND_FILL);
	sound_tree->set_hide_root(true);
	sound_tree->connect("item_activated", callable_mp(this, &AsosdotAnimationWorkspace::_sound_activated));
	sound_tree->connect("button_clicked", callable_mp(this, &AsosdotAnimationWorkspace::_sound_button));
	sound_tree->set_drag_forwarding(callable_mp(this, &AsosdotAnimationWorkspace::_sound_drag_data), Callable(), Callable());
	sounds->add_child(sound_tree);
	sound_preview = memnew(AudioStreamPlayer);
	add_child(sound_preview);

	// Poses.
	VBoxContainer *poses = memnew(VBoxContainer);
	poses->set_name(TTRC("Poses"));
	tabs->add_child(poses);
	HBoxContainer *library_row = memnew(HBoxContainer);
	poses->add_child(library_row);
	Button *open_library = memnew(Button);
	open_library->set_text(TTR("Open..."));
	open_library->set_tooltip_text(TTR("Open a pose library: an AnimationLibrary file whose animations are poses."));
	open_library->connect(SceneStringName(pressed), callable_mp(this, &AsosdotAnimationWorkspace::_pose_library_open));
	library_row->add_child(open_library);
	Button *new_library = memnew(Button);
	new_library->set_text(TTR("New..."));
	new_library->set_tooltip_text(TTR("Make a new pose library file."));
	new_library->connect(SceneStringName(pressed), callable_mp(this, &AsosdotAnimationWorkspace::_pose_library_new));
	library_row->add_child(new_library);
	pose_library_label = memnew(Label);
	pose_library_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	pose_library_label->set_custom_minimum_size(Size2(1, 0));
	poses->add_child(pose_library_label);
	pose_list = memnew(ItemList);
	pose_list->set_v_size_flags(SIZE_EXPAND_FILL);
	pose_list->connect("item_activated", callable_mp(this, &AsosdotAnimationWorkspace::_apply_pose).bind(false).unbind(1));
	poses->add_child(pose_list);
	HBoxContainer *mix_row = memnew(HBoxContainer);
	poses->add_child(mix_row);
	Label *mix_label = memnew(Label(TTR("Mix")));
	mix_row->add_child(mix_label);
	pose_mix = memnew(HSlider);
	pose_mix->set_min(0);
	pose_mix->set_max(100);
	pose_mix->set_step(1);
	pose_mix->set_value(100);
	pose_mix->set_h_size_flags(SIZE_EXPAND_FILL);
	pose_mix->set_tooltip_text(TTR("How far toward the pose to move, from the current pose (0%) to the saved one (100%)."));
	mix_row->add_child(pose_mix);
	HFlowContainer *pose_buttons = memnew(HFlowContainer);
	poses->add_child(pose_buttons);
	Button *save_pose = memnew(Button);
	save_pose->set_text(TTR("Save Pose"));
	save_pose->set_tooltip_text(TTR("Save the selected Skeleton3D's pose in the library."));
	save_pose->connect(SceneStringName(pressed), callable_mp(this, &AsosdotAnimationWorkspace::_save_pose_requested));
	pose_buttons->add_child(save_pose);
	Button *apply = memnew(Button);
	apply->set_text(TTR("Apply"));
	apply->set_tooltip_text(TTR("Pose the selected Skeleton3D. With Record on, the pose is keyed at the playhead."));
	apply->connect(SceneStringName(pressed), callable_mp(this, &AsosdotAnimationWorkspace::_apply_pose).bind(false));
	pose_buttons->add_child(apply);
	Button *apply_bone = memnew(Button);
	apply_bone->set_text(TTR("Apply to Bone"));
	apply_bone->set_tooltip_text(TTR("Pose only the bone selected in the Skeleton3D."));
	apply_bone->connect(SceneStringName(pressed), callable_mp(this, &AsosdotAnimationWorkspace::_apply_pose).bind(true));
	pose_buttons->add_child(apply_bone);
	Button *delete_pose = memnew(Button);
	delete_pose->set_text(TTR("Delete"));
	delete_pose->connect(SceneStringName(pressed), callable_mp(this, &AsosdotAnimationWorkspace::_delete_pose));
	pose_buttons->add_child(delete_pose);
	pose_hint = memnew(Label);
	pose_hint->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	pose_hint->set_custom_minimum_size(Size2(1, 0));
	poses->add_child(pose_hint);

	pose_file_dialog = memnew(EditorFileDialog);
	pose_file_dialog->add_filter("*.tres", TTR("Text Resource"));
	pose_file_dialog->add_filter("*.res", TTR("Binary Resource"));
	pose_file_dialog->connect("file_selected", callable_mp(this, &AsosdotAnimationWorkspace::_pose_library_chosen));
	add_child(pose_file_dialog);

	pose_name_dialog = memnew(ConfirmationDialog);
	pose_name_dialog->set_title(TTR("Save Pose"));
	pose_name = memnew(LineEdit);
	pose_name_dialog->add_child(pose_name);
	pose_name_dialog->register_text_enter(pose_name);
	pose_name_dialog->connect(SceneStringName(confirmed), callable_mp(this, &AsosdotAnimationWorkspace::_save_pose_confirmed));
	add_child(pose_name_dialog);

	monitor = memnew(AsosdotCameraMonitor(this));

	hide();
}

/// Plugin.

AsosdotAnimationWorkspacePlugin *AsosdotAnimationWorkspacePlugin::singleton = nullptr;

EditorPlugin *AsosdotAnimationWorkspacePlugin::_get_node_3d_plugin() {
	if (!node_3d_plugin) {
		node_3d_plugin = EditorNode::get_singleton()->get_editor_main_screen()->get_plugin_by_name("3D");
	}
	return node_3d_plugin;
}

bool AsosdotAnimationWorkspacePlugin::hosts_main_screen_of(const EditorPlugin *p_plugin) const {
	return workspace->is_active() && p_plugin && p_plugin == node_3d_plugin;
}

void AsosdotAnimationWorkspacePlugin::make_visible(bool p_visible) {
	EditorPlugin *viewer = _get_node_3d_plugin();
	ERR_FAIL_NULL(viewer);
	if (p_visible) {
		// The 3D editor stays where it is and shows in this screen's place, with the library in
		// its left panel; nothing is moved between parents.
		viewer->make_visible(true);
		workspace->set_active(true);
		// Whatever is selected now is edited in the viewer, as the 3D screen would.
		Node *selected = nullptr;
		const List<Node *> top = EditorNode::get_singleton()->get_editor_selection()->get_top_selected_node_list();
		if (!top.is_empty()) {
			selected = top.front()->get();
		}
		viewer->edit(Object::cast_to<Node3D>(selected));
	} else {
		workspace->set_active(false);
		viewer->make_visible(false);
	}
}

void AsosdotAnimationWorkspacePlugin::edit(Object *p_object) {
	if (workspace->is_active() && _get_node_3d_plugin()) {
		node_3d_plugin->edit(p_object);
	}
}

void AsosdotAnimationWorkspacePlugin::set_window_layout(Ref<ConfigFile> p_layout) {
	if (p_layout->has_section_key("AnimationWorkspace", "layout")) {
		workspace->set_layout(p_layout->get_value("AnimationWorkspace", "layout"));
	}
}

void AsosdotAnimationWorkspacePlugin::get_window_layout(Ref<ConfigFile> p_layout) {
	p_layout->set_value("AnimationWorkspace", "layout", workspace->get_layout());
}

AsosdotAnimationWorkspacePlugin::AsosdotAnimationWorkspacePlugin() {
	singleton = this;
	workspace = memnew(AsosdotAnimationWorkspace);
	add_control_to_container(CONTAINER_SPATIAL_EDITOR_SIDE_LEFT, workspace);
	// The monitor sits right of the viewer, sharing the width with it, side by side.
	add_control_to_container(CONTAINER_SPATIAL_EDITOR_SIDE_RIGHT, workspace->get_monitor());
	add_control_to_container(CONTAINER_SPATIAL_EDITOR_MENU, workspace->toolbar);
}

AsosdotAnimationWorkspacePlugin::~AsosdotAnimationWorkspacePlugin() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
