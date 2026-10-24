/**************************************************************************/
/*  render_editor_sequence.cpp                                            */
/**************************************************************************/
/*  Asosdot: shots and sound in the Render workspace.                      */
/**************************************************************************/

#include "render_editor_plugin.h"

#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/gui/editor_toaster.h"
#include "editor/inspector/editor_inspector.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/camera_3d.h"
#include "scene/audio/audio_stream_player.h"
#include "scene/gui/button.h"
#include "scene/gui/item_list.h"
#include "scene/gui/slider.h"
#include "scene/gui/split_container.h"
#include "scene/gui/tab_bar.h"
#include "scene/resources/font.h"
#include "servers/movie_writer/movie_writer.h"

String AsosdotRenderEditor::_shot_resource_key(const String &p_id) const {
	const Node *root = _get_scene_root();
	return String::num_uint64(root ? uint64_t(root->get_instance_id()) : 0) + "/" + p_id;
}

double AsosdotRenderEditor::_sequence_duration() const {
	double duration = 0.0;
	for (const SequenceShot &entry : shots) {
		duration += double(entry.settings->get_value("duration"));
	}
	return MAX(duration, 0.1);
}

double AsosdotRenderEditor::_shot_start(int p_index) const {
	double start = 0.0;
	for (int i = 0; i < MIN(p_index, shots.size()); i++) {
		start += double(shots[i].settings->get_value("duration"));
	}
	return start;
}

void AsosdotRenderEditor::_store_shot_view() {
	if (selected_shot < 0 || selected_shot >= shots.size()) {
		return;
	}
	SequenceShot &entry = shots.write[selected_shot];
	entry.target = orbit_target;
	entry.yaw = orbit_yaw;
	entry.pitch = orbit_pitch;
	entry.distance = orbit_distance;
	entry.framed = framed;
}

void AsosdotRenderEditor::_activate_shot(int p_index) {
	ERR_FAIL_INDEX(p_index, shots.size());
	if (p_index == selected_shot && shot == shots[p_index].settings) {
		return;
	}
	_cancel_pose_drag();
	_restore_pose();
	_store_shot_view();
	if (shot.is_valid()) {
		shot->disconnect_changed(callable_mp(this, &AsosdotRenderEditor::_shot_changed));
	}
	selected_shot = p_index;
	const SequenceShot &entry = shots[p_index];
	shot = entry.settings;
	shot_resources.insert(_shot_resource_key(entry.id), shot);
	orbit_target = entry.target;
	orbit_yaw = entry.yaw;
	orbit_pitch = entry.pitch;
	orbit_distance = entry.distance;
	framed = entry.framed;
	shot->connect_changed(callable_mp(this, &AsosdotRenderEditor::_shot_changed));
	if (active && !framed) {
		_frame_subject();
	}
	_update_studio();
	_update_sequence_ui();
	if (settings_selector->get_current_tab() == 0) {
		inspector->edit(shot.ptr());
	}
}

void AsosdotRenderEditor::_apply_sequence_time(double p_time) {
	if (!Math::is_finite(p_time) || shots.is_empty()) {
		return;
	}
	p_time = CLAMP(p_time, 0.0, _sequence_duration());
	double local = p_time;
	int index = 0;
	while (index < shots.size() - 1 && local >= double(shots[index].settings->get_value("duration")) - 0.000001) {
		local -= double(shots[index++].settings->get_value("duration"));
	}
	local = MAX(local, 0.0);
	camera_blend_active = false;
	const double blend = MIN(double(shots[index].settings->get_value("camera_blend")), double(shots[index].settings->get_value("duration")));
	if (active && index > 0 && blend > 0.0 && local < blend) {
		// Evaluate the outgoing shot at its end, including animated cameras.
		// Restore its pose before evaluating the incoming shot. This makes a
		// direct scrub into a blend identical to playing through it.
		evaluating_transition = true;
		_activate_shot(index - 1);
		_apply_time(shot->get_value("duration"));
		camera_blend_from = preview_camera->get_global_transform();
		camera_blend_fov = preview_camera->get_fov();
		camera_blend_size = preview_camera->get_size();
		camera_blend_projection = preview_camera->get_projection();
		camera_blend_aspect = preview_camera->get_keep_aspect_mode();
		evaluating_transition = false;
		const float weight = local / blend;
		camera_blend_weight = weight * weight * (3.0f - 2.0f * weight);
		camera_blend_active = true;
	}
	_activate_shot(index);
	sequence_time = p_time;
	if (active) {
		_apply_time(local);
	} else {
		time = local;
		_update_timeline();
	}
	_update_pose_bar();
}

void AsosdotRenderEditor::_select_shot(int p_index) {
	if (job.active || p_index < 0 || p_index >= shots.size()) {
		return;
	}
	playing = false;
	_stop_audio();
	play_button->set_button_icon(get_editor_theme_icon(SNAME("Play")));
	_apply_sequence_time(_shot_start(p_index));
}

void AsosdotRenderEditor::_update_sequence_ui() {
	if (!shot_list) {
		return;
	}
	shot_list->clear();
	for (int i = 0; i < shots.size(); i++) {
		const Ref<AsosdotRenderShot> &settings = shots[i].settings;
		const String name = settings->get_value("shot_name");
		const String camera = _camera_label(settings);
		shot_list->add_item(vformat(U"%02d  %s · %s s", i + 1, name, String::num(settings->get_value("duration"), 2)));
		shot_list->set_item_tooltip(i, vformat(TTR("%s\nCamera: %s\nStarts at %s s"), name, camera, String::num(_shot_start(i), 2)));
	}
	shot_list->select(selected_shot);
	_update_workspace_labels();
	remove_shot_button->set_disabled(job.active || shots.size() <= 1);
	move_shot_up_button->set_disabled(job.active || selected_shot <= 0);
	move_shot_down_button->set_disabled(job.active || selected_shot >= shots.size() - 1);
	sequence_strip->queue_redraw();
}

void AsosdotRenderEditor::_settings_selected(int p_index) {
	inspector->edit(p_index == 0 ? shot.ptr() : sequence_output.ptr());
	_update_workspace_labels();
}

void AsosdotRenderEditor::_sequence_output_changed() {
	if (updating_sequence || job.active) {
		return;
	}
	_update_output_label();
	_update_timeline();
	_update_preview_audio(true);
}

void AsosdotRenderEditor::_commit_sequence_edit(const Dictionary &p_before, const String &p_name) {
	EditorUndoRedoManager *undo = EditorUndoRedoManager::get_singleton();
	undo->create_action(p_name, UndoRedo::MERGE_DISABLE, _get_scene_root(), false, false);
	undo->add_do_method(this, "set_state", get_state());
	undo->add_undo_method(this, "set_state", p_before);
	undo->commit_action();
}

void AsosdotRenderEditor::_add_shot(bool p_duplicate) {
	if (job.active || shots.size() >= 1024) {
		return;
	}
	const Dictionary before = get_state();
	_store_shot_view();
	SequenceShot entry = shots[selected_shot];
	entry.settings.instantiate();
	entry.id = String::num_uint64(entry.settings->get_instance_id());
	if (p_duplicate) {
		entry.settings->set_state(shot->get_state());
	}
	entry.settings->set_value("shot_name", p_duplicate ? String(shot->get_value("shot_name")) + TTR(" Copy") : vformat(TTR("Shot %d"), shots.size() + 1));
	entry.settings->set_meta(SNAME("__base_node_relative"), _get_scene_root());
	const int index = selected_shot + 1;
	shots.insert(index, entry);
	_select_shot(index);
	_commit_sequence_edit(before, p_duplicate ? TTR("Duplicate Render Shot") : TTR("Add Render Shot"));
}

void AsosdotRenderEditor::_remove_shot() {
	if (job.active || shots.size() <= 1) {
		return;
	}
	const Dictionary before = get_state();
	_cancel_pose_drag();
	_restore_pose();
	shot->disconnect_changed(callable_mp(this, &AsosdotRenderEditor::_shot_changed));
	shot->remove_meta(SNAME("__base_node_relative"));
	const int index = MIN(selected_shot, shots.size() - 2);
	shots.remove_at(selected_shot);
	selected_shot = -1;
	shot.unref();
	_select_shot(index);
	_commit_sequence_edit(before, TTR("Remove Render Shot"));
}

void AsosdotRenderEditor::_move_shot(int p_direction) {
	const int index = selected_shot + p_direction;
	if (job.active || index < 0 || index >= shots.size()) {
		return;
	}
	const Dictionary before = get_state();
	_store_shot_view();
	SWAP(shots.write[selected_shot], shots.write[index]);
	selected_shot = index;
	_select_shot(index);
	_commit_sequence_edit(before, TTR("Reorder Render Shots"));
}

void AsosdotRenderEditor::_set_sequence_editing(bool p_enabled) {
	for (Button *button : { add_shot_button, duplicate_shot_button, remove_shot_button, move_shot_up_button, move_shot_down_button, play_button, sequence_start_button, sequence_end_button, previous_frame_button, next_frame_button, previous_key_button, next_key_button, frame_button, pose_button, render_image_button, render_animation_button }) {
		button->set_disabled(!p_enabled);
	}
	inspector->set_read_only(!p_enabled);
	pose_bar->set_mouse_filter(p_enabled ? Control::MOUSE_FILTER_PASS : Control::MOUSE_FILTER_IGNORE);
	_update_sequence_ui();
}

Dictionary AsosdotRenderEditor::get_state() const {
	Dictionary state;
	Array entries;
	for (int i = 0; i < shots.size(); i++) {
		const SequenceShot &entry = shots[i];
		const bool current = i == selected_shot;
		Dictionary item;
		item["shot"] = entry.settings->get_state();
		item["shot_id"] = entry.id;
		item["orbit_target"] = current ? orbit_target : entry.target;
		item["orbit_yaw"] = current ? orbit_yaw : entry.yaw;
		item["orbit_pitch"] = current ? orbit_pitch : entry.pitch;
		item["orbit_distance"] = current ? orbit_distance : entry.distance;
		item["framed"] = current ? framed : entry.framed;
		entries.push_back(item);
	}
	state["sequence_version"] = 1;
	state["shots"] = entries;
	state["output"] = sequence_output->get_state();
	state["selected_shot"] = selected_shot;
	state["sequence_time"] = sequence_time;
	state["pose_mode"] = pose_mode;
	state["pose_tool"] = pose_tool;
	state["auto_key"] = auto_key;
	Dictionary layout;
	layout["preview_split"] = (preview_focus ? preview_split_before_focus : preview_split->get_split_offset()) / EDSCALE;
	layout["sequence_split"] = sequence_split->get_split_offset() / EDSCALE;
	layout["workspace_split"] = (preview_focus ? workspace_split_before_focus : workspace_split->get_split_offset()) / EDSCALE;
	layout["properties_tab"] = settings_selector->get_current_tab();
	layout["workspace_focus"] = workspace_focus;
	layout["preview_focus"] = preview_focus;
	layout["composition_guides"] = composition_guides;
	state["layout"] = layout;
	return state;
}

void AsosdotRenderEditor::set_state(const Dictionary &p_state) {
	if (job.active) {
		return;
	}
	updating_sequence = true;
	playing = false;
	_stop_audio();
	_cancel_pose_drag();
	_restore_pose();
	if (shot.is_valid()) {
		shot->disconnect_changed(callable_mp(this, &AsosdotRenderEditor::_shot_changed));
	}
	for (const SequenceShot &entry : shots) {
		shot_resources.insert(_shot_resource_key(entry.id), entry.settings);
		entry.settings->remove_meta(SNAME("__base_node_relative"));
	}
	shots.clear();
	shot.unref();
	Array entries = p_state.get("shots", Array());
	if (entries.is_empty()) {
		// Existing scenes retain their single shot and its output settings.
		entries.push_back(p_state);
	}
	HashSet<String> ids;
	for (int i = 0; i < MIN(entries.size(), 1024); i++) {
		if (entries[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary item = entries[i];
		SequenceShot entry;
		entry.id = item.get("shot_id", String());
		const Ref<AsosdotRenderShot> *existing = shot_resources.getptr(_shot_resource_key(entry.id));
		if (!entry.id.is_empty() && !ids.has(entry.id) && existing) {
			entry.settings = *existing;
		} else {
			entry.settings.instantiate();
			if (entry.id.is_empty() || ids.has(entry.id)) {
				entry.id = String::num_uint64(entry.settings->get_instance_id());
			}
		}
		ids.insert(entry.id);
		shot_resources.insert(_shot_resource_key(entry.id), entry.settings);
		entry.settings->set_state(item.get("shot", Dictionary()));
		entry.settings->set_meta(SNAME("__base_node_relative"), _get_scene_root());
		entry.target = item.get("orbit_target", Vector3());
		if (!entry.target.is_finite()) {
			entry.target = Vector3();
		}
		entry.yaw = item.get("orbit_yaw", 0.6);
		entry.pitch = item.get("orbit_pitch", -0.2);
		entry.distance = item.get("orbit_distance", 4.0);
		entry.yaw = Math::is_finite(entry.yaw) ? entry.yaw : 0.6f;
		entry.pitch = Math::is_finite(entry.pitch) ? CLAMP(entry.pitch, -1.5f, 1.5f) : -0.2f;
		entry.distance = Math::is_finite(entry.distance) ? CLAMP(entry.distance, 0.01f, 100000.0f) : 4.0f;
		entry.framed = item.get("framed", false);
		shots.push_back(entry);
	}
	if (shots.is_empty()) {
		SequenceShot entry;
		entry.settings.instantiate();
		entry.id = String::num_uint64(entry.settings->get_instance_id());
		shots.push_back(entry);
	}
	sequence_output->set_state(p_state.get("output", p_state.get("shot", Dictionary())));
	selected_shot = -1;
	_activate_shot(CLAMP(int(p_state.get("selected_shot", 0)), 0, shots.size() - 1));
	updating_sequence = false;
	_set_pose_mode(p_state.get("pose_mode", false));
	_set_pose_tool(p_state.get("pose_tool", POSE_TOOL_ROTATE));
	_set_auto_key(p_state.get("auto_key", false));
	const Dictionary layout = p_state.get("layout", Dictionary());
	_set_preview_focus(false);
	preview_split->set_split_offset(CLAMP(int(layout.get("preview_split", 0)), -2000, 2000) * EDSCALE);
	sequence_split->set_split_offset(CLAMP(int(layout.get("sequence_split", 0)), -2000, 2000) * EDSCALE);
	workspace_split->set_split_offset(CLAMP(int(layout.get("workspace_split", 0)), -2000, 2000) * EDSCALE);
	settings_selector->set_current_tab(CLAMP(int(layout.get("properties_tab", 0)), 0, 1));
	_settings_selected(settings_selector->get_current_tab());
	_set_composition_guides(layout.get("composition_guides", false));
	_set_preview_focus(layout.get("preview_focus", false));
	_set_workspace_focus(layout.get("workspace_focus", false));
	play_button->set_button_icon(get_editor_theme_icon(SNAME("Play")));
	const double at = p_state.get("sequence_time", _shot_start(selected_shot));
	_apply_sequence_time(Math::is_finite(at) ? at : 0.0);
	_update_output_label();
}

void AsosdotRenderEditor::_stop_audio() {
	if (music_player) {
		music_player->stop();
		dialogue_player->stop();
	}
}

void AsosdotRenderEditor::_update_preview_audio(bool p_seek) {
	if (!playing || !active || job.active) {
		_stop_audio();
		return;
	}
	auto update = [&](AudioStreamPlayer *p_player, const Ref<AsosdotRenderShot> &p_settings, const String &p_prefix, double p_position, String &r_path, bool p_restart) {
		const String path = p_settings->get_value(p_prefix);
		if (path != r_path) {
			p_player->stop();
			r_path = path;
			p_player->set_stream(path.is_empty() ? Ref<AudioStream>() : Ref<AudioStream>(ResourceLoader::load(path, "AudioStream")));
			p_restart = true;
		}
		const Ref<AudioStream> stream = p_player->get_stream();
		const double offset = p_settings->get_value(p_prefix + "_offset");
		if (stream.is_null() || p_position < 0.0 || p_position + offset >= stream->get_length()) {
			p_player->stop();
			return;
		}
		p_player->set_volume_db(p_settings->get_value(p_prefix + "_volume_db"));
		if (p_restart || !p_player->is_playing()) {
			p_player->play(p_position + offset);
		}
	};
	update(music_player, sequence_output, "music", sequence_time - double(sequence_output->get_value("music_start")), preview_music_path, p_seek);
	update(dialogue_player, shot, "dialogue", time, preview_dialogue_path, p_seek || preview_dialogue_shot != selected_shot);
	preview_dialogue_shot = selected_shot;
}

void AsosdotRenderEditor::_prepare_render_audio() {
	render_audio.clear();
	rendered_audio_samples = 0;
	auto add = [&](const Ref<AsosdotRenderShot> &p_settings, const String &p_prefix, double p_start, double p_end) {
		const String path = p_settings->get_value(p_prefix);
		if (path.is_empty()) {
			return;
		}
		const Ref<AudioStream> stream = ResourceLoader::load(path, "AudioStream");
		if (stream.is_null()) {
			EditorToaster::get_singleton()->popup_str(vformat(TTR("Could not load audio: %s"), path), EditorToaster::SEVERITY_WARNING);
			return;
		}
		AudioCue cue;
		cue.offset = p_settings->get_value(p_prefix + "_offset");
		cue.start = p_start;
		cue.end = MIN(p_end, p_start + MAX(0.0, stream->get_length() - cue.offset));
		cue.gain = Math::db_to_linear(float(p_settings->get_value(p_prefix + "_volume_db")));
		cue.playback = stream->instantiate_playback();
		if (cue.playback.is_valid() && cue.end > cue.start) {
			render_audio.push_back(cue);
		}
	};
	add(sequence_output, "music", sequence_output->get_value("music_start"), _sequence_duration());
	double start = 0.0;
	for (const SequenceShot &entry : shots) {
		const double end = start + double(entry.settings->get_value("duration"));
		add(entry.settings, "dialogue", start, end);
		start = end;
	}
}

Vector<int32_t> AsosdotRenderEditor::_mix_render_audio() {
	// The encoder runs on another thread and may be several frames behind.
	// Derive this block from the render frame rather than encoder progress.
	const uint64_t next_sample = job.writer->get_frame_audio_sample_offset_at(job.frame + 1);
	const int sample_count = next_sample - rendered_audio_samples;
	const int channels = job.writer->get_frame_audio_channels();
	const double rate = job.writer->get_frame_audio_mix_rate();
	const int64_t first_sample = rendered_audio_samples;
	rendered_audio_samples = next_sample;
	Vector<AudioFrame> mixed;
	mixed.resize(sample_count);
	memset(mixed.ptrw(), 0, sample_count * sizeof(AudioFrame));
	for (int i = 0; i < render_audio.size(); i++) {
		AudioCue &cue = render_audio.write[i];
		const int64_t first = MAX(first_sample, int64_t(Math::ceil(cue.start * rate)));
		const int64_t last = MIN(first_sample + sample_count, int64_t(Math::ceil(cue.end * rate)));
		if (last <= first) {
			continue;
		}
		if (!cue.started) {
			cue.playback->start(cue.offset + MAX(0.0, first / rate - cue.start));
			cue.started = true;
		}
		Vector<AudioFrame> buffer;
		buffer.resize(last - first);
		memset(buffer.ptrw(), 0, buffer.size() * sizeof(AudioFrame));
		// Playback resamplers target the live audio device. Compensate so
		// slow offline rendering still encodes at the movie's sample rate.
		const float scale = AudioServer::get_singleton()->get_mix_rate() / rate / MAX(AudioServer::get_singleton()->get_playback_speed_scale(), 0.0001f);
		cue.playback->mix(buffer.ptrw(), scale, buffer.size());
		for (int j = 0; j < buffer.size(); j++) {
			mixed.write[first - first_sample + j] += buffer[j] * cue.gain;
		}
	}
	Vector<int32_t> result;
	result.resize(sample_count * channels);
	memset(result.ptrw(), 0, result.size() * sizeof(int32_t));
	for (int i = 0; i < sample_count; i++) {
		result.write[i * channels] = int32_t(CLAMP(double(mixed[i].left), -1.0, 1.0) * 2147483647.0);
		result.write[i * channels + 1] = int32_t(CLAMP(double(mixed[i].right), -1.0, 1.0) * 2147483647.0);
	}
	return result;
}

void AsosdotRenderEditor::_render_sequence(const String &p_path) {
	if (active && !p_path.is_empty()) {
		_start_job(_chosen_output_path(p_path), true);
	}
}
