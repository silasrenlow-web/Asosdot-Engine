/**************************************************************************/
/*  audio_stream_player_internal.cpp                                      */
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

#include "audio_stream_player_internal.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "servers/audio/audio_stream.h"

void AudioStreamPlayerInternal::_set_process(bool p_enabled) {
	if (physical) {
		node->set_physics_process_internal(p_enabled);
	} else {
		node->set_process_internal(p_enabled);
	}
}

void AudioStreamPlayerInternal::_update_stream_parameters() {
	if (stream.is_null()) {
		return;
	}

	List<AudioStream::Parameter> parameters;
	stream->get_parameter_list(&parameters);
	for (const AudioStream::Parameter &K : parameters) {
		const PropertyInfo &pi = K.property;
		StringName key = PARAM_PREFIX + pi.name;
		if (!playback_parameters.has(key)) {
			ParameterData pd;
			pd.path = pi.name;
			pd.value = K.default_value;
			playback_parameters.insert(key, pd);
		}
	}
}

void AudioStreamPlayerInternal::process() {
	Vector<Ref<AudioStreamPlayback>> playbacks_to_remove;
	for (Ref<AudioStreamPlayback> &playback : stream_playbacks) {
		if (playback.is_valid() && !_is_layer(playback) && !AudioServer::get_singleton()->is_playback_active(playback) && !AudioServer::get_singleton()->is_playback_paused(playback)) {
			playbacks_to_remove.push_back(playback);
		}
	}
	// Now go through and remove playbacks that have finished. Removing elements from a Vector in a range based for is asking for trouble.
	for (Ref<AudioStreamPlayback> &playback : playbacks_to_remove) {
		stream_playbacks.erase(playback);
	}
	const bool layer_stopped = _process_layered();
	if ((!playbacks_to_remove.is_empty() || layer_stopped) && stream_playbacks.is_empty()) {
		// This node is no longer actively playing audio.
		active.clear();
		_set_process(false);
	}
	if (!playbacks_to_remove.is_empty()) {
		node->emit_signal(SceneStringName(finished));
	}
}

// Reports layered sounds that ended by themselves, and lets the layer go once none are left.
bool AudioStreamPlayerInternal::_process_layered() {
	if (layer_playback.is_null() || layer_needs_start) {
		return false;
	}
	LocalVector<int64_t> ended;
	for (uint32_t i = 0; i < layered.size();) {
		if (layer_playback->is_stream_playing(layered[i].voice)) {
			i++;
		} else {
			ended.push_back(layered[i].id);
			layered.remove_at(i);
		}
	}
	bool stopped = false;
	if (layered.is_empty()) {
		AudioServer::get_singleton()->stop_playback_stream(layer_playback);
		stream_playbacks.erase(layer_playback);
		layer_playback.unref();
		stopped = true;
	}
	for (const int64_t id : ended) {
		node->emit_signal(SNAME("layered_finished"), id);
	}
	return stopped;
}

void AudioStreamPlayerInternal::ensure_playback_limit() {
	// Layered sounds share one playback of their own, which doesn't count toward max_polyphony.
	int own_playbacks = stream_playbacks.size() - (layer_playback.is_valid() ? 1 : 0);
	while (own_playbacks > max_polyphony) {
		const int oldest = _is_layer(stream_playbacks[0]) ? 1 : 0;
		AudioServer::get_singleton()->stop_playback_stream(stream_playbacks[oldest]);
		stream_playbacks.remove_at(oldest);
		own_playbacks--;
	}
}

void AudioStreamPlayerInternal::notification(int p_what) {
	switch (p_what) {
		case Node::NOTIFICATION_ENTER_TREE: {
			if (autoplay && !Engine::get_singleton()->is_editor_hint()) {
				play_callable.call(0.0);
			}
			set_stream_paused(!node->can_process());
		} break;

		case Node::NOTIFICATION_EXIT_TREE: {
			set_stream_paused(true);
		} break;

		case Node::NOTIFICATION_INTERNAL_PROCESS: {
			process();
		} break;

		case Node::NOTIFICATION_PREDELETE: {
			for (Ref<AudioStreamPlayback> &playback : stream_playbacks) {
				AudioServer::get_singleton()->stop_playback_stream(playback);
			}
			stream_playbacks.clear();
			layer_playback.unref();
			layered.clear();
		} break;

		case Node::NOTIFICATION_SUSPENDED:
		case Node::NOTIFICATION_PAUSED: {
			bool can_process = node->is_inside_tree() && node->can_process();
			if (!can_process) {
				// Node can't process so we start fading out to silence
				set_stream_paused(true);
			}
		} break;

		case Node::NOTIFICATION_UNSUSPENDED: {
			if (node->get_tree()->is_paused()) {
				break;
			}
			[[fallthrough]];
		}

		case Node::NOTIFICATION_UNPAUSED: {
			set_stream_paused(false);
		} break;
	}
}

Ref<AudioStreamPlayback> AudioStreamPlayerInternal::play_basic() {
	Ref<AudioStreamPlayback> stream_playback;
	if (stream.is_null()) {
		return stream_playback;
	}
	ERR_FAIL_COND_V_MSG(!node->is_inside_tree(), stream_playback, "Playback can only happen when a node is inside the scene tree");
	if (stream->is_monophonic() && is_playing()) {
		stop_callable.call();
	}
	stream_playback = stream->instantiate_playback();
	ERR_FAIL_COND_V_MSG(stream_playback.is_null(), stream_playback, "Failed to instantiate playback.");

	for (const KeyValue<StringName, ParameterData> &K : playback_parameters) {
		stream_playback->set_parameter(K.value.path, K.value.value);
	}

	// Sample handling.
	if (_is_sample()) {
		if (stream->can_be_sampled()) {
			stream_playback->set_is_sample(true);
			if (stream_playback->get_is_sample() && stream_playback->get_sample_playback().is_null()) {
				if (!AudioServer::get_singleton()->is_stream_registered_as_sample(stream)) {
					AudioServer::get_singleton()->register_stream_as_sample(stream);
				}
				Ref<AudioSamplePlayback> sample_playback;
				sample_playback.instantiate();
				sample_playback->stream = stream;
				sample_playback->pitch_scale = pitch_scale;
				stream_playback->set_sample_playback(sample_playback);
			}
		} else if (!stream->is_meta_stream()) {
			WARN_PRINT(vformat(R"(%s is trying to play a sample from a stream that cannot be sampled.)", node->get_path()));
		}
	}

	stream_playbacks.push_back(stream_playback);
	active.set();
	_set_process(true);
	return stream_playback;
}

void AudioStreamPlayerInternal::set_stream_paused(bool p_pause) {
	// TODO this does not have perfect recall, fix that maybe? If there are zero playbacks registered with the AudioServer, this bool isn't persisted.
	for (Ref<AudioStreamPlayback> &playback : stream_playbacks) {
		AudioServer::get_singleton()->set_playback_paused(playback, p_pause);
		if (_is_sample() && playback->get_sample_playback().is_valid()) {
			AudioServer::get_singleton()->set_sample_playback_pause(playback->get_sample_playback(), p_pause);
		}
	}
}

bool AudioStreamPlayerInternal::get_stream_paused() const {
	// There's currently no way to pause some playback streams but not others. Check the first and don't bother looking at the rest.
	if (!stream_playbacks.is_empty()) {
		return AudioServer::get_singleton()->is_playback_paused(stream_playbacks[0]);
	}
	return false;
}

void AudioStreamPlayerInternal::validate_property(PropertyInfo &p_property) const {
	if (!Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	if (p_property.name == "bus") {
		String options;
		for (int i = 0; i < AudioServer::get_singleton()->get_bus_count(); i++) {
			if (i > 0) {
				options += ",";
			}
			String name = AudioServer::get_singleton()->get_bus_name(i);
			options += name;
		}

		p_property.hint_string = options;
	}
}

bool AudioStreamPlayerInternal::set(const StringName &p_name, const Variant &p_value) {
	ParameterData *pd = playback_parameters.getptr(p_name);
	if (!pd) {
		return false;
	}
	pd->value = p_value;
	for (Ref<AudioStreamPlayback> &playback : stream_playbacks) {
		playback->set_parameter(pd->path, pd->value);
	}
	return true;
}

bool AudioStreamPlayerInternal::get(const StringName &p_name, Variant &r_ret) const {
	const ParameterData *pd = playback_parameters.getptr(p_name);
	if (!pd) {
		return false;
	}
	r_ret = pd->value;
	return true;
}

void AudioStreamPlayerInternal::get_property_list(List<PropertyInfo> *p_list) const {
	if (stream.is_null()) {
		return;
	}
	List<AudioStream::Parameter> parameters;
	stream->get_parameter_list(&parameters);
	for (const AudioStream::Parameter &K : parameters) {
		PropertyInfo pi = K.property;
		pi.name = PARAM_PREFIX + pi.name;

		const ParameterData *pd = playback_parameters.getptr(pi.name);
		if (pd && pd->value == K.default_value) {
			pi.usage &= ~PROPERTY_USAGE_STORAGE;
		}

		p_list->push_back(pi);
	}
}

void AudioStreamPlayerInternal::set_stream(Ref<AudioStream> p_stream) {
	if (stream.is_valid()) {
		stream->disconnect(SNAME("parameter_list_changed"), callable_mp(this, &AudioStreamPlayerInternal::_update_stream_parameters));
	}
	stop_callable.call();
	stream = p_stream;
	_update_stream_parameters();
	if (stream.is_valid()) {
		stream->connect(SNAME("parameter_list_changed"), callable_mp(this, &AudioStreamPlayerInternal::_update_stream_parameters));
	}
	node->notify_property_list_changed();
}

void AudioStreamPlayerInternal::seek(float p_seconds) {
	if (is_playing()) {
		stop_callable.call();
		play_callable.call(p_seconds);
	}
}

void AudioStreamPlayerInternal::stop_basic() {
	// Only the player's own stream: layered sounds go on until stop_all_layered().
	for (int i = stream_playbacks.size() - 1; i >= 0; i--) {
		if (_is_layer(stream_playbacks[i])) {
			continue;
		}
		AudioServer::get_singleton()->stop_playback_stream(stream_playbacks[i]);
		stream_playbacks.remove_at(i);
	}

	if (stream_playbacks.is_empty()) {
		active.clear();
		_set_process(false);
	}
}

bool AudioStreamPlayerInternal::is_playing() const {
	for (const Ref<AudioStreamPlayback> &playback : stream_playbacks) {
		if (!_is_layer(playback) && AudioServer::get_singleton()->is_playback_active(playback)) {
			return true;
		}
	}
	return false;
}

float AudioStreamPlayerInternal::get_playback_position() {
	// Return the playback position of the most recently started playback stream.
	for (int i = stream_playbacks.size() - 1; i >= 0; i--) {
		if (!_is_layer(stream_playbacks[i])) {
			return AudioServer::get_singleton()->get_playback_position(stream_playbacks[i]);
		}
	}
	return 0;
}

void AudioStreamPlayerInternal::set_playing(bool p_enable) {
	if (p_enable) {
		play_callable.call(0.0);
	} else {
		stop_callable.call();
	}
}

bool AudioStreamPlayerInternal::is_active() const {
	return active.is_set();
}

void AudioStreamPlayerInternal::set_pitch_scale(float p_pitch_scale) {
	ERR_FAIL_COND(p_pitch_scale <= 0.0);
	pitch_scale = p_pitch_scale;

	for (Ref<AudioStreamPlayback> &playback : stream_playbacks) {
		AudioServer::get_singleton()->set_playback_pitch_scale(playback, pitch_scale);
	}
}

void AudioStreamPlayerInternal::set_max_polyphony(int p_max_polyphony) {
	if (p_max_polyphony > 0) {
		max_polyphony = p_max_polyphony;
	}
}

bool AudioStreamPlayerInternal::has_stream_playback() {
	return stream_playbacks.size() > (layer_playback.is_valid() ? 1 : 0);
}

Ref<AudioStreamPlayback> AudioStreamPlayerInternal::get_stream_playback() {
	for (int i = stream_playbacks.size() - 1; i >= 0; i--) {
		if (!_is_layer(stream_playbacks[i])) {
			return stream_playbacks[i];
		}
	}
	ERR_FAIL_V_MSG(Ref<AudioStreamPlayback>(), "Player is inactive. Call play() before requesting get_stream_playback().");
}

int64_t AudioStreamPlayerInternal::play_layered(const Ref<AudioStream> &p_stream, float p_from_pos, float p_volume_db, float p_pitch_scale) {
	ERR_FAIL_COND_V(p_stream.is_null(), AudioStreamPlaybackPolyphonic::INVALID_ID);
	ERR_FAIL_COND_V_MSG(!node->is_inside_tree(), AudioStreamPlaybackPolyphonic::INVALID_ID, "Playback can only happen when a node is inside the scene tree");
	ERR_FAIL_COND_V(p_pitch_scale <= 0.0, AudioStreamPlaybackPolyphonic::INVALID_ID);
	ERR_FAIL_COND_V_MSG(Math::is_nan(p_volume_db), AudioStreamPlaybackPolyphonic::INVALID_ID, "Volume can't be set to NaN.");
	if (layer_playback.is_null()) {
		if (layer_stream.is_null()) {
			layer_stream.instantiate();
			layer_stream->set_polyphony(MAX_LAYERED);
		}
		layer_playback = layer_stream->instantiate_playback();
		stream_playbacks.push_back(layer_playback);
		layer_needs_start = true;
		active.set();
		_set_process(true);
	}
	float sound_volume_db = p_volume_db;
	if (_is_sample() && p_stream->can_be_sampled()) {
		// A sample plays outside the mix, where the player's own volume can't reach it.
		sound_volume_db += volume_db;
	}
	const int64_t voice = layer_playback->play_stream(p_stream, p_from_pos, sound_volume_db, p_pitch_scale, playback_type, get_bus());
	ERR_FAIL_COND_V_MSG(voice == AudioStreamPlaybackPolyphonic::INVALID_ID, AudioStreamPlaybackPolyphonic::INVALID_ID, vformat("%s already plays %d layered sounds, the most it can.", node->get_path(), MAX_LAYERED));
	LayeredSound sound;
	sound.id = next_layered_id++;
	sound.voice = voice;
	layered.push_back(sound);
	return sound.id;
}

const AudioStreamPlayerInternal::LayeredSound *AudioStreamPlayerInternal::_find_layered(int64_t p_id) const {
	for (const LayeredSound &sound : layered) {
		if (sound.id == p_id) {
			return &sound;
		}
	}
	return nullptr;
}

void AudioStreamPlayerInternal::stop_layered(int64_t p_id) {
	for (uint32_t i = 0; i < layered.size(); i++) {
		if (layered[i].id == p_id) {
			layer_playback->stop_stream(layered[i].voice);
			layered.remove_at(i);
			return;
		}
	}
}

void AudioStreamPlayerInternal::stop_all_layered() {
	if (layer_playback.is_null()) {
		return;
	}
	AudioServer::get_singleton()->stop_playback_stream(layer_playback);
	stream_playbacks.erase(layer_playback);
	layer_playback.unref();
	layered.clear();
	layer_needs_start = false;
	if (stream_playbacks.is_empty()) {
		active.clear();
		_set_process(false);
	}
}

bool AudioStreamPlayerInternal::is_layered_playing(int64_t p_id) const {
	const LayeredSound *sound = _find_layered(p_id);
	return sound && layer_playback->is_stream_playing(sound->voice);
}

void AudioStreamPlayerInternal::set_layered_volume_db(int64_t p_id, float p_volume_db) {
	ERR_FAIL_COND_MSG(Math::is_nan(p_volume_db), "Volume can't be set to NaN.");
	if (const LayeredSound *sound = _find_layered(p_id)) {
		layer_playback->set_stream_volume(sound->voice, p_volume_db);
	}
}

void AudioStreamPlayerInternal::set_layered_pitch_scale(int64_t p_id, float p_pitch_scale) {
	ERR_FAIL_COND(p_pitch_scale <= 0.0);
	if (const LayeredSound *sound = _find_layered(p_id)) {
		layer_playback->set_stream_pitch_scale(sound->voice, p_pitch_scale);
	}
}

int AudioStreamPlayerInternal::get_layered_count() const {
	int count = 0;
	for (const LayeredSound &sound : layered) {
		if (layer_playback->is_stream_playing(sound.voice)) {
			count++;
		}
	}
	return count;
}

void AudioStreamPlayerInternal::set_playback_type(AudioServer::PlaybackType p_playback_type) {
	playback_type = p_playback_type;
}

AudioServer::PlaybackType AudioStreamPlayerInternal::get_playback_type() const {
	return playback_type;
}

StringName AudioStreamPlayerInternal::get_bus() const {
	const String bus_name = bus;
	for (int i = 0; i < AudioServer::get_singleton()->get_bus_count(); i++) {
		if (AudioServer::get_singleton()->get_bus_name(i) == bus_name) {
			return bus;
		}
	}
	return SceneStringName(Master);
}

AudioStreamPlayerInternal::AudioStreamPlayerInternal(Node *p_node, const Callable &p_play_callable, const Callable &p_stop_callable, bool p_physical) {
	node = p_node;
	play_callable = p_play_callable;
	stop_callable = p_stop_callable;
	physical = p_physical;
	bus = SceneStringName(Master);

	AudioServer::get_singleton()->connect("bus_layout_changed", callable_mp((Object *)node, &Object::notify_property_list_changed));
	AudioServer::get_singleton()->connect("bus_renamed", callable_mp((Object *)node, &Object::notify_property_list_changed).unbind(3));
}
