/**************************************************************************/
/*  animation_mixer.cpp                                                   */
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

#include "animation_mixer.h"
#include "animation_mixer.compat.inc"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/string/string_name.h"
#include "scene/2d/audio_stream_player_2d.h"
#include "scene/animation/animation_player.h"
#include "scene/audio/audio_stream_player.h"
#include "scene/main/global_transmit.h"
#include "scene/resources/animation.h"
#include "servers/audio/audio_server.h"
#include "servers/audio/audio_stream.h"

#ifndef _3D_DISABLED
#include "scene/3d/audio_stream_player_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/3d/skeleton_3d.h"
#endif // _3D_DISABLED

#ifdef TOOLS_ENABLED
#include "editor/editor_undo_redo_manager.h"
#endif // TOOLS_ENABLED

bool AnimationMixer::_set(const StringName &p_name, const Variant &p_value) {
	String name = p_name;

#ifndef DISABLE_DEPRECATED
	if (name.begins_with("anims/")) {
		// Backwards compatibility with 3.x, add them to "default" library.
		String which = name.get_slicec('/', 1);

		Ref<Animation> anim = p_value;
		Ref<AnimationLibrary> al;
		if (!has_animation_library(StringName())) {
			al.instantiate();
			add_animation_library(StringName(), al);
		} else {
			al = get_animation_library(StringName());
		}
		al->add_animation(which, anim);
	} else if (name == "libraries") {
		Dictionary d = p_value;
		while (animation_libraries.size()) {
			remove_animation_library(animation_libraries[0].name);
		}
		for (const KeyValue<Variant, Variant> &kv : d) {
			Ref<AnimationLibrary> lib = kv.value;
			add_animation_library(kv.key, lib);
		}
		emit_signal(SNAME("animation_libraries_updated"));
	} else if (name.begins_with("libraries/")) {
		String which = name.get_slicec('/', 1);
		if (has_animation_library(which)) {
			remove_animation_library(which);
		}
		add_animation_library(which, p_value);
		emit_signal(SNAME("animation_libraries_updated"));
	} else {
		return false;
	}
#else
	if (name.begins_with("libraries/")) {
		String which = name.get_slicec('/', 1);
		if (has_animation_library(which)) {
			remove_animation_library(which);
		}
		add_animation_library(which, p_value);
		emit_signal(SNAME("animation_libraries_updated"));
	} else {
		return false;
	}
#endif // DISABLE_DEPRECATED

	return true;
}

bool AnimationMixer::_get(const StringName &p_name, Variant &r_ret) const {
	String name = p_name;

	if (name.begins_with("libraries/")) {
		String which = name.get_slicec('/', 1);
		if (has_animation_library(which)) {
			r_ret = get_animation_library(which);
		} else {
			return false;
		}
	} else {
		return false;
	}

	return true;
}

uint32_t AnimationMixer::_get_libraries_property_usage() const {
	return PROPERTY_USAGE_STORAGE;
}

void AnimationMixer::_get_property_list(List<PropertyInfo> *p_list) const {
	for (uint32_t i = 0; i < animation_libraries.size(); i++) {
		const String path = vformat("libraries/%s", animation_libraries[i].name);
		p_list->push_back(PropertyInfo(Variant::OBJECT, path, PROPERTY_HINT_RESOURCE_TYPE, AnimationLibrary::get_class_static(), _get_libraries_property_usage()));
	}
}

void AnimationMixer::_validate_property(PropertyInfo &p_property) const {
#ifdef TOOLS_ENABLED // `editing` is surrounded by TOOLS_ENABLED so this should also be.
	if (Engine::get_singleton()->is_editor_hint() && editing && (p_property.name == "active" || p_property.name == "deterministic" || p_property.name == "root_motion_track")) {
		p_property.usage |= PROPERTY_USAGE_READ_ONLY;
		return;
	}
#endif // TOOLS_ENABLED
	if (root_motion_track.is_empty() && p_property.name == "root_motion_local") {
		p_property.usage = PROPERTY_USAGE_NONE;
	}
}

/* -------------------------------------------- */
/* -- Data lists ------------------------------ */
/* -------------------------------------------- */

void AnimationMixer::_animation_set_cache_update() {
	// Relatively fast function to update all animations.
	animation_set_update_pass++;
	bool clear_cache_needed = false;

	// Update changed and add otherwise.
	for (const AnimationLibraryData &lib : animation_libraries) {
		for (const KeyValue<StringName, Ref<Animation>> &K : lib.library->animations) {
			StringName key = lib.name == StringName() ? K.key : StringName(String(lib.name) + "/" + String(K.key));
			AnimationData *ad = animation_set.getptr(key);

			if (!ad) {
				ad = &animation_set.insert(key, AnimationData())->value; // 2) Hash key and lookup again.
				ad->animation = K.value;
				ad->animation_library = lib.name;
				ad->last_update = animation_set_update_pass;
				cache_valid = false;
			} else {
				if (ad->last_update != animation_set_update_pass) {
					if (ad->animation != K.value || ad->animation_library != lib.name) {
						clear_cache_needed = true;
						ad->animation = K.value;
						ad->animation_library = lib.name;
					}

					ad->last_update = animation_set_update_pass;
				}
			}
		}
	}

	// Check removed.
	// TODO: Can we avoid this nonsense by iterating in reverse?
	LocalVector<StringName> to_erase;
	for (const KeyValue<StringName, AnimationData> &E : animation_set) {
		if (E.value.last_update != animation_set_update_pass) {
			// Was not updated, must be erased.
			to_erase.push_back(E.key);
			clear_cache_needed = true;
		}
	}

	for (const StringName &E : to_erase) {
		animation_set.erase(E);
	}

	if (clear_cache_needed) {
		// If something was modified or removed, caches need to be cleared.
		_clear_caches();
	}

	emit_signal(SNAME("animation_list_changed"));
}

void AnimationMixer::_animation_added(const StringName &p_name, const StringName &p_library) {
	_animation_set_cache_update();
}

void AnimationMixer::_animation_removed(const StringName &p_name, const StringName &p_library) {
	const StringName name = p_library == StringName() ? p_name : StringName(String(p_library) + "/" + String(p_name));

	if (!animation_set.has(name)) {
		return; // No need to update because not the one from the library being used.
	}

	_animation_set_cache_update();

	_remove_animation(name);
}

void AnimationMixer::_animation_renamed(const StringName &p_name, const StringName &p_to_name, const StringName &p_library) {
	const StringName from_name = p_library == StringName() ? p_name : StringName(String(p_library) + "/" + String(p_name));
	const StringName to_name = p_library == StringName() ? p_to_name : StringName(String(p_library) + "/" + String(p_to_name));

	if (!animation_set.has(from_name)) {
		return; // No need to update because not the one from the library being used.
	}
	_animation_set_cache_update();

	_rename_animation(from_name, to_name);
}

void AnimationMixer::_animation_changed(const StringName &p_name) {
	_clear_caches();
}

void AnimationMixer::_set_active(bool p_active) {
	//
}

void AnimationMixer::_remove_animation(const StringName &p_name) {
	//
}

void AnimationMixer::_rename_animation(const StringName &p_from_name, const StringName &p_to_name) {
	//
}

TypedArray<StringName> AnimationMixer::_get_animation_library_list() const {
	TypedArray<StringName> ret;
	for (const AnimationLibraryData &lib : animation_libraries) {
		ret.push_back(lib.name);
	}
	return ret;
}

void AnimationMixer::get_animation_library_list(LocalVector<StringName> *p_libraries) const {
	for (const AnimationLibraryData &lib : animation_libraries) {
		p_libraries->push_back(lib.name);
	}
}

Ref<AnimationLibrary> AnimationMixer::get_animation_library(const StringName &p_name) const {
	for (const AnimationLibraryData &lib : animation_libraries) {
		if (lib.name == p_name) {
			return lib.library;
		}
	}
	ERR_FAIL_V(Ref<AnimationLibrary>());
}

bool AnimationMixer::has_animation_library(const StringName &p_name) const {
	for (const AnimationLibraryData &lib : animation_libraries) {
		if (lib.name == p_name) {
			return true;
		}
	}

	return false;
}

const StringName &AnimationMixer::find_animation_library(const Ref<Animation> &p_animation) const {
	for (const KeyValue<StringName, AnimationData> &E : animation_set) {
		if (E.value.animation == p_animation) {
			return E.value.animation_library;
		}
	}
	const static StringName empty = StringName();
	return empty;
}

Error AnimationMixer::add_animation_library(const StringName &p_name, const Ref<AnimationLibrary> &p_animation_library) {
	ERR_FAIL_COND_V(p_animation_library.is_null(), ERR_INVALID_PARAMETER);
#ifdef DEBUG_ENABLED
	ERR_FAIL_COND_V_MSG(String(p_name).contains_char('/') || String(p_name).contains_char(':') || String(p_name).contains_char(',') || String(p_name).contains_char('['), ERR_INVALID_PARAMETER, "Invalid animation name: " + String(p_name) + ".");
#endif

	int insert_pos = 0;

	for (const AnimationLibraryData &lib : animation_libraries) {
		ERR_FAIL_COND_V_MSG(lib.name == p_name, ERR_ALREADY_EXISTS, "Can't add animation library twice with name: " + String(p_name));
		ERR_FAIL_COND_V_MSG(lib.library == p_animation_library, ERR_ALREADY_EXISTS, "Can't add animation library twice (adding as '" + p_name.operator String() + "', exists as '" + lib.name.operator String() + "'.");

		if (lib.name.operator String() >= p_name.operator String()) {
			break;
		}

		insert_pos++;
	}

	AnimationLibraryData ald;
	ald.name = p_name;
	ald.library = p_animation_library;

	animation_libraries.insert(insert_pos, ald);

	ald.library->connect(SNAME("animation_added"), callable_mp(this, &AnimationMixer::_animation_added).bind(p_name));
	ald.library->connect(SNAME("animation_removed"), callable_mp(this, &AnimationMixer::_animation_removed).bind(p_name));
	ald.library->connect(SNAME("animation_renamed"), callable_mp(this, &AnimationMixer::_animation_renamed).bind(p_name));
	ald.library->connect(SceneStringName(animation_changed), callable_mp(this, &AnimationMixer::_animation_changed));

	_animation_set_cache_update();

	notify_property_list_changed();

	return OK;
}

void AnimationMixer::remove_animation_library(const StringName &p_name) {
	int at_pos = -1;

	for (uint32_t i = 0; i < animation_libraries.size(); i++) {
		if (animation_libraries[i].name == p_name) {
			at_pos = i;
			break;
		}
	}

	ERR_FAIL_COND(at_pos == -1);

	animation_libraries[at_pos].library->disconnect(SNAME("animation_added"), callable_mp(this, &AnimationMixer::_animation_added));
	animation_libraries[at_pos].library->disconnect(SNAME("animation_removed"), callable_mp(this, &AnimationMixer::_animation_removed));
	animation_libraries[at_pos].library->disconnect(SNAME("animation_renamed"), callable_mp(this, &AnimationMixer::_animation_renamed));
	animation_libraries[at_pos].library->disconnect(SceneStringName(animation_changed), callable_mp(this, &AnimationMixer::_animation_changed));

	animation_libraries.remove_at(at_pos);
	_animation_set_cache_update();

	notify_property_list_changed();
}

void AnimationMixer::rename_animation_library(const StringName &p_name, const StringName &p_new_name) {
	if (p_name == p_new_name) {
		return;
	}
#ifdef DEBUG_ENABLED
	ERR_FAIL_COND_MSG(String(p_new_name).contains_char('/') || String(p_new_name).contains_char(':') || String(p_new_name).contains_char(',') || String(p_new_name).contains_char('['), "Invalid animation library name: " + String(p_new_name) + ".");
#endif

	bool found = false;
	for (AnimationLibraryData &lib : animation_libraries) {
		ERR_FAIL_COND_MSG(lib.name == p_new_name, "Can't rename animation library to another existing name: " + String(p_new_name) + ".");
		if (lib.name == p_name) {
			found = true;
			lib.name = p_new_name;
			// rename connections
			lib.library->disconnect(SNAME("animation_added"), callable_mp(this, &AnimationMixer::_animation_added));
			lib.library->disconnect(SNAME("animation_removed"), callable_mp(this, &AnimationMixer::_animation_removed));
			lib.library->disconnect(SNAME("animation_renamed"), callable_mp(this, &AnimationMixer::_animation_renamed));

			lib.library->connect(SNAME("animation_added"), callable_mp(this, &AnimationMixer::_animation_added).bind(p_new_name));
			lib.library->connect(SNAME("animation_removed"), callable_mp(this, &AnimationMixer::_animation_removed).bind(p_new_name));
			lib.library->connect(SNAME("animation_renamed"), callable_mp(this, &AnimationMixer::_animation_renamed).bind(p_new_name));

			for (const KeyValue<StringName, Ref<Animation>> &K : lib.library->animations) {
				StringName old_name = p_name == StringName() ? K.key : StringName(String(p_name) + "/" + String(K.key));
				StringName new_name = p_new_name == StringName() ? K.key : StringName(String(p_new_name) + "/" + String(K.key));
				_rename_animation(old_name, new_name);
			}
		}
	}

	ERR_FAIL_COND(!found);

	animation_libraries.sort(); // Must keep alphabetical order.

	_animation_set_cache_update(); // Update cache.

	notify_property_list_changed();
}

LocalVector<StringName> AnimationMixer::get_sorted_animation_list() const {
	LocalVector<StringName> animations;
	get_animation_list(&animations);
	animations.sort_custom<StringName::AlphCompare>();
	return animations;
}

void AnimationMixer::get_animation_list(LocalVector<StringName> *p_animations) const {
	p_animations->reserve(p_animations->size() + animation_set.size());
	for (const KeyValue<StringName, AnimationData> &E : animation_set) {
		p_animations->push_back(E.key);
	}
}

const Ref<Animation> &AnimationMixer::get_animation(const StringName &p_name) const {
	const Ref<Animation> &animation = get_animation_or_null(p_name);
	ERR_FAIL_COND_V_MSG(animation.is_null(), animation, vformat("Animation not found: \"%s\".", p_name));
	return animation;
}

const Ref<Animation> &AnimationMixer::get_animation_or_null(const StringName &p_name) const {
	const AnimationData *ad = animation_set.getptr(p_name);
	if (!ad) {
		const static Ref<Animation> empty = Ref<Animation>();
		return empty;
	}
	return ad->animation;
}

bool AnimationMixer::has_animation(const StringName &p_name) const {
	return animation_set.has(p_name);
}

StringName AnimationMixer::find_animation(const Ref<Animation> &p_animation) const {
	for (const KeyValue<StringName, AnimationData> &E : animation_set) {
		if (E.value.animation == p_animation) {
			return E.key;
		}
	}
	return StringName();
}

/* -------------------------------------------- */
/* -- General settings for animation ---------- */
/* -------------------------------------------- */

void AnimationMixer::_set_process(bool p_process, bool p_force) {
	if (processing == p_process && !p_force) {
		return;
	}

	switch (callback_mode_process) {
		case ANIMATION_CALLBACK_MODE_PROCESS_PHYSICS:
#ifdef TOOLS_ENABLED
			set_physics_process_internal(p_process && active && !editing);
#else
			set_physics_process_internal(p_process && active);
#endif // TOOLS_ENABLED
			break;
		case ANIMATION_CALLBACK_MODE_PROCESS_IDLE:
#ifdef TOOLS_ENABLED
			set_process_internal(p_process && active && !editing);
#else
			set_process_internal(p_process && active);
#endif // TOOLS_ENABLED
			break;
		case ANIMATION_CALLBACK_MODE_PROCESS_MANUAL:
			break;
	}

	processing = p_process;
}

void AnimationMixer::set_active(bool p_active) {
	if (active == p_active) {
		return;
	}

	active = p_active;
	_set_active(active);
	_set_process(processing, true);

	if (!active && is_inside_tree()) {
		_clear_caches();
	}
}

bool AnimationMixer::is_active() const {
	return active;
}

void AnimationMixer::set_root_node(const NodePath &p_path) {
	root_node = p_path;
	_clear_caches();
}

NodePath AnimationMixer::get_root_node() const {
	return root_node;
}

void AnimationMixer::set_deterministic(bool p_deterministic) {
	deterministic = p_deterministic;
	_clear_caches();
}

bool AnimationMixer::is_deterministic() const {
	return deterministic;
}

void AnimationMixer::set_callback_mode_process(AnimationCallbackModeProcess p_mode) {
	if (callback_mode_process == p_mode) {
		return;
	}

	bool was_active = is_active();
	if (was_active) {
		set_active(false);
	}

	callback_mode_process = p_mode;

	if (was_active) {
		set_active(true);
	}
}

AnimationMixer::AnimationCallbackModeProcess AnimationMixer::get_callback_mode_process() const {
	return callback_mode_process;
}

void AnimationMixer::set_callback_mode_method(AnimationCallbackModeMethod p_mode) {
	callback_mode_method = p_mode;
	emit_signal(SNAME("mixer_updated"));
}

AnimationMixer::AnimationCallbackModeMethod AnimationMixer::get_callback_mode_method() const {
	return callback_mode_method;
}

void AnimationMixer::set_callback_mode_discrete(AnimationCallbackModeDiscrete p_mode) {
	callback_mode_discrete = p_mode;
	_clear_caches();
	emit_signal(SNAME("mixer_updated"));
}

AnimationMixer::AnimationCallbackModeDiscrete AnimationMixer::get_callback_mode_discrete() const {
	return callback_mode_discrete;
}

void AnimationMixer::set_audio_max_polyphony(int p_audio_max_polyphony) {
	ERR_FAIL_COND(p_audio_max_polyphony < 0 || p_audio_max_polyphony > 128);
	audio_max_polyphony = p_audio_max_polyphony;
}

int AnimationMixer::get_audio_max_polyphony() const {
	return audio_max_polyphony;
}

#ifdef TOOLS_ENABLED
void AnimationMixer::set_editing(bool p_editing) {
	if (editing == p_editing) {
		return;
	}

	editing = p_editing;
	_set_process(processing, true);

	if (editing && is_inside_tree()) {
		_clear_caches();
	}

	notify_property_list_changed(); // To make active readonly.
}

bool AnimationMixer::is_editing() const {
	return editing;
}

void AnimationMixer::set_dummy(bool p_dummy) {
	dummy = p_dummy;
}

bool AnimationMixer::is_dummy() const {
	return dummy;
}
#endif // TOOLS_ENABLED

/* -------------------------------------------- */
/* -- Caches for blending --------------------- */
/* -------------------------------------------- */

void AnimationMixer::_clear_caches() {
	_init_root_motion_cache();
	_clear_audio_streams();
	_clear_playing_caches();
	for (KeyValue<Animation::TrackCacheID, TrackCache *> &K : track_cache) {
		memdelete(K.value);
	}
	track_cache.clear();
	animation_track_num_to_track_cache.clear();
	cache_valid = false;
	capture_cache.clear();

	emit_signal(SNAME("caches_cleared"));
}

void AnimationMixer::_clear_audio_streams() {
	// Stops the sounds animations started. The players' own streams play on.
	for (const Pair<ObjectID, int64_t> &sound : audio_sounds) {
		Object *player = ObjectDB::get_instance(sound.first);
		if (player) {
			player->call(SNAME("stop_layered"), sound.second);
		}
	}
	audio_sounds.clear();
	for (KeyValue<Animation::TrackCacheID, TrackCache *> &K : track_cache) {
		if (K.value->type == Animation::TYPE_AUDIO) {
			static_cast<TrackCacheAudio *>(K.value)->playing_streams.clear();
		}
#ifdef TOOLS_ENABLED
		// Stopping or pausing a preview in the editor also silences what its `playing` tracks
		// started. A running game keeps them playing, as in Godot.
		if (K.value->type == Animation::TYPE_VALUE && static_cast<TrackCacheValue *>(K.value)->is_audio_playing && Engine::get_singleton()->is_editor_hint()) {
			Object *player = ObjectDB::get_instance(K.value->object_id);
			if (player && player->call(SNAME("is_playing"))) {
				player->set(SNAME("playing"), false);
			}
		}
#endif // TOOLS_ENABLED
	}
}

void AnimationMixer::_stop_audio_sound(Object *p_player, int64_t p_index) {
	p_player->call(SNAME("stop_layered"), p_index);
	audio_sounds.erase(Pair<ObjectID, int64_t>(p_player->get_instance_id(), p_index));
}

void AnimationMixer::_play_audio_key(TrackCacheAudio *p_cache, Object *p_player, PlayingAudioTrackInfo &r_track_info, const Ref<Animation> &p_animation, int p_track, int p_key, double p_time, bool p_resume, bool p_latest) {
	Ref<AudioStream> stream = p_animation->audio_track_get_key_stream(p_track, p_key);
	if (stream.is_null()) {
		return;
	}
	double start_ofs = p_animation->audio_track_get_key_start_offset(p_track, p_key);
	const double end_ofs = p_animation->audio_track_get_key_end_offset(p_track, p_key);
	const real_t pitch_scale = p_animation->audio_track_get_key_pitch_scale(p_track, p_key);
	const real_t volume_db = p_animation->audio_track_get_key_volume_db(p_track, p_key);
	const double len = stream->get_length();

	if (p_resume) {
		// Resumed from where the key's sound has got to by now, if it still sounds; a stream that
		// doesn't know its length is only resumed for the latest key.
		const double since = MAX(0.0, p_time - p_animation->track_get_key_time(p_track, p_key));
		if (len > 0 ? Animation::is_greater_approx(since * pitch_scale, len - start_ofs - end_ofs) : !p_latest) {
			return;
		}
		start_ofs += since * pitch_scale;
	}

	// Past audio_max_polyphony, the oldest sound this player has from animations gives way.
	int sounding = 0;
	for (const KeyValue<Pair<ObjectID, int>, PlayingAudioTrackInfo> &L : p_cache->playing_streams) {
		sounding += L.value.stream_info.size();
	}
	while (sounding >= audio_max_polyphony && sounding > 0) {
		PlayingAudioTrackInfo *oldest_info = nullptr;
		uint64_t oldest = UINT64_MAX;
		for (KeyValue<Pair<ObjectID, int>, PlayingAudioTrackInfo> &L : p_cache->playing_streams) {
			for (const KeyValue<uint64_t, PlayingAudioStreamInfo> &M : L.value.stream_info) {
				if (M.key < oldest) {
					oldest = M.key;
					oldest_info = &L.value;
				}
			}
		}
		_stop_audio_sound(p_player, oldest_info->stream_info[oldest].index);
		oldest_info->stream_info.erase(oldest);
		sounding--;
	}

	const float blend_db = Math::linear_to_db(r_track_info.use_blend ? r_track_info.volume : 1.0);
	PlayingAudioStreamInfo pasi;
	pasi.index = p_player->call(SNAME("play_layered"), stream, start_ofs, volume_db + blend_db, pitch_scale);
	if (pasi.index < 0) {
		return;
	}
	audio_sounds.insert(Pair<ObjectID, int64_t>(p_player->get_instance_id(), pasi.index));
	pasi.start = p_time;
	pasi.volume_db = volume_db;
	if (len && Animation::is_greater_approx(end_ofs, 0)) { // Force an end at a time.
		pasi.len = (len - start_ofs - end_ofs) / pitch_scale;
	} else {
		pasi.len = 0;
	}
	r_track_info.stream_info[p_cache->started++] = pasi;
}

void AnimationMixer::_follow_audio_playhead(Object *p_player, PlayingAudioTrackInfo &r_track_info, const Ref<Animation> &p_animation, int p_track, double p_time) {
	// Sounds playing at their own pace give way to ones that sit under the playhead.
	for (const KeyValue<uint64_t, PlayingAudioStreamInfo> &M : r_track_info.stream_info) {
		_stop_audio_sound(p_player, M.value.index);
	}
	r_track_info.stream_info.clear();

	const float blend_db = Math::linear_to_db(r_track_info.use_blend ? r_track_info.volume : 1.0);
	LocalVector<int> under;
	const int key_count = p_animation->track_get_key_count(p_track);
	for (int k = 0; k < key_count; k++) {
		const double key_time = p_animation->track_get_key_time(p_track, k);
		if (Animation::is_greater_approx(key_time, p_time)) {
			break;
		}
		Ref<AudioStream> stream = p_animation->audio_track_get_key_stream(p_track, k);
		if (stream.is_null()) {
			continue;
		}
		const double len = stream->get_length();
		if (len <= 0.0) {
			continue; // A stream without a set length can't be laid out under the playhead.
		}
		const double start_ofs = p_animation->audio_track_get_key_start_offset(p_track, k);
		const double end_ofs = p_animation->audio_track_get_key_end_offset(p_track, k);
		const double position = start_ofs + MAX(0.0, p_time - key_time) * p_animation->audio_track_get_key_pitch_scale(p_track, k);
		if (position >= len - end_ofs) {
			continue; // Over by this point of the timeline.
		}
		under.push_back(k);

		ScrubbedAudioInfo *sound = r_track_info.scrubbed.getptr(k);
		if (sound && p_player->call(SNAME("is_layered_playing"), sound->index)) {
			sound->stream->set_target(position);
			continue;
		}
		Ref<AudioStreamScrub> scrub = AudioStreamScrub::create(stream);
		if (scrub.is_null()) {
			continue;
		}
		scrub->set_target(position);
		const real_t volume_db = p_animation->audio_track_get_key_volume_db(p_track, k);
		const int64_t index = p_player->call(SNAME("play_layered"), scrub, position, volume_db + blend_db, 1.0);
		if (index < 0) {
			continue;
		}
		audio_sounds.insert(Pair<ObjectID, int64_t>(p_player->get_instance_id(), index));
		ScrubbedAudioInfo info;
		info.index = index;
		info.stream = scrub;
		info.volume_db = volume_db;
		r_track_info.scrubbed.insert(k, info);
	}

	// Keys the playhead has left fall silent.
	LocalVector<int> left;
	for (const KeyValue<int, ScrubbedAudioInfo> &S : r_track_info.scrubbed) {
		if (!under.has(S.key)) {
			left.push_back(S.key);
		}
	}
	for (const int k : left) {
		_stop_audio_sound(p_player, r_track_info.scrubbed[k].index);
		r_track_info.scrubbed.erase(k);
	}
}

void AnimationMixer::_stop_scrubbed_audio(Object *p_player, PlayingAudioTrackInfo &r_track_info) {
	for (const KeyValue<int, ScrubbedAudioInfo> &S : r_track_info.scrubbed) {
		_stop_audio_sound(p_player, S.value.index);
	}
	r_track_info.scrubbed.clear();
}

void AnimationMixer::_process_camera_cut_track(TrackCacheValue *p_cache, const Ref<Animation> &p_animation, int p_track, double p_time, double p_delta, double p_start, double p_end, Animation::LoopedFlag p_looped_flag, bool p_seeked) {
	int key = -1;
	const int key_count = p_animation->track_get_key_count(p_track);
	while (key + 1 < key_count && !Animation::is_greater_approx(p_animation->track_get_key_time(p_track, key + 1), p_time)) {
		key++;
	}
	if (key < 0) {
		p_cache->camera_cut_value = Variant(); // Before the camera's first key, it's left as it is.
		return;
	}
	const Variant value = post_process_key_value(p_animation, p_track, p_animation->track_get_key_value(p_track, key), p_cache->object_id);
	// Applied when the shot changes, after a seek, and when a key is passed (so a key still takes the
	// camera back if something else made another camera current meanwhile).
	bool apply = p_seeked || value != p_cache->camera_cut_value;
	if (!apply) {
		LocalVector<int> passed;
		p_animation->track_get_key_indices_in_range(p_track, p_time, p_delta, p_start, p_end, &passed, p_looped_flag);
		apply = !passed.is_empty();
	}
	if (!apply) {
		return;
	}
	p_cache->camera_cut_value = value;
	Object *camera = ObjectDB::get_instance(p_cache->object_id);
	if (camera) {
		camera->set_indexed(p_cache->subpath, value);
	}
}

// In the editor, a stopped or paused AnimationPlayer only places the scene when it updates (after a
// key is edited, say); it never starts sounds.
bool AnimationMixer::_is_editor_preview_stopped() const {
#ifdef TOOLS_ENABLED
	if (Engine::get_singleton()->is_editor_hint()) {
		const AnimationPlayer *animation_player = Object::cast_to<AnimationPlayer>(this);
		return animation_player && !animation_player->is_playing();
	}
#endif // TOOLS_ENABLED
	return false;
}

// Whether the player's own stream sounds at p_time by its `playing` track, and where in the
// stream it is by then. The latest key at or before p_time decides (not the nearest, which may lie
// ahead), and an "on" key started the stream at its own time.
bool AnimationMixer::_audio_playing_position(TrackCacheValue *p_cache, Object *p_player, const Ref<Animation> &p_animation, int p_track, double p_time, int &r_key, double &r_position) {
	r_key = -1;
	const int key_count = p_animation->track_get_key_count(p_track);
	while (r_key + 1 < key_count && !Animation::is_greater_approx(p_animation->track_get_key_time(p_track, r_key + 1), p_time)) {
		r_key++;
	}
	if (r_key < 0) {
		return false; // Before the first key, the track hasn't started the stream.
	}
	if (!bool(post_process_key_value(p_animation, p_track, p_animation->track_get_key_value(p_track, r_key), p_cache->object_id))) {
		return false;
	}
	Ref<AudioStream> stream = p_player->get(SNAME("stream"));
	if (stream.is_null()) {
		return false;
	}
	r_position = MAX(0.0, p_time - p_animation->track_get_key_time(p_track, r_key)) * double(p_player->get(SNAME("pitch_scale")));
	const double length = stream->get_length();
	if (length > 0.0 && r_position >= length) {
		if (!stream->has_loop()) {
			return false; // It has played to its end by now.
		}
		r_position = Math::fmod(r_position, length);
	}
	return true;
}

void AnimationMixer::_stop_audio_playing_follow(TrackCacheValue *p_cache, Object *p_player) {
	if (p_cache->playhead_sound >= 0) {
		_stop_audio_sound(p_player, p_cache->playhead_sound);
	}
	p_cache->playhead_sound = -1;
	p_cache->playhead_stream.unref();
	p_cache->playhead_key = -1;
}

void AnimationMixer::_process_audio_playing_track(TrackCacheValue *p_cache, const Ref<Animation> &p_animation, int p_track, double p_time, double p_delta, double p_start, double p_end, Animation::LoopedFlag p_looped_flag, bool p_seeked, bool p_backward, bool p_update_only) {
	Object *player = ObjectDB::get_instance(p_cache->object_id);
	if (!player) {
		return;
	}
	int key = -1;
	double position = 0.0;

	// Scrubbing in the editor, and playing backwards: the stream sits under the playhead and moves
	// with it, instead of the player restarting it each time a key is found.
	if ((p_update_only && audio_scrubbing) || (p_backward && !p_seeked && !p_update_only)) {
		if (player->call(SNAME("is_playing"))) {
			player->set(SNAME("playing"), false);
		}
		Ref<AudioStream> stream = player->get(SNAME("stream"));
		if (!_audio_playing_position(p_cache, player, p_animation, p_track, p_time, key, position) || stream->get_length() <= 0.0) {
			_stop_audio_playing_follow(p_cache, player);
			return;
		}
		if (p_cache->playhead_key == key && p_cache->playhead_stream.is_valid() && player->call(SNAME("is_layered_playing"), p_cache->playhead_sound)) {
			p_cache->playhead_stream->set_target(position);
			return;
		}
		_stop_audio_playing_follow(p_cache, player);
		Ref<AudioStreamScrub> scrub = AudioStreamScrub::create(stream);
		if (scrub.is_null()) {
			return;
		}
		scrub->set_target(position);
		const int64_t index = player->call(SNAME("play_layered"), scrub, position, 0.0, 1.0);
		if (index < 0) {
			return;
		}
		audio_sounds.insert(Pair<ObjectID, int64_t>(player->get_instance_id(), index));
		p_cache->playhead_sound = index;
		p_cache->playhead_stream = scrub;
		p_cache->playhead_key = key;
		return;
	}

	// Other seeks that only update the scene (onion skinning, stepping between keys) leave the
	// sound alone, rather than starting it from whichever key is nearest.
	if (p_update_only) {
		return;
	}

	const bool was_following = p_cache->playhead_sound >= 0;
	_stop_audio_playing_follow(p_cache, player);
	if (_is_editor_preview_stopped()) {
		return;
	}
	if (p_seeked || was_following) {
		// Pick up what sounds at this point of the timeline, partway into the stream.
		if (_audio_playing_position(p_cache, player, p_animation, p_track, p_time, key, position)) {
			player->call(SNAME("play"), position);
		} else if (player->call(SNAME("is_playing"))) {
			player->set(SNAME("playing"), false);
		}
		return;
	}

	// Playing forwards: each key passed switches the player, as in Godot.
	LocalVector<int> indices;
	p_animation->track_get_key_indices_in_range(p_track, p_time, p_delta, p_start, p_end, &indices, p_looped_flag);
	for (const int idx : indices) {
		player->set(SNAME("playing"), post_process_key_value(p_animation, p_track, p_animation->track_get_key_value(p_track, idx), p_cache->object_id));
	}
}

void AnimationMixer::_clear_playing_caches() {
	for (const TrackCache *E : playing_caches) {
		Object *t_obj = ObjectDB::get_instance(E->object_id);
		if (t_obj) {
			t_obj->call(SNAME("stop"), true);
		}
	}
	playing_caches.clear();
}

void AnimationMixer::_init_root_motion_cache() {
	root_motion_cache.loc = Vector3(0, 0, 0);
	root_motion_cache.rot = Quaternion(0, 0, 0, 1);
	root_motion_cache.scale = Vector3(1, 1, 1);
	root_motion_position = Vector3(0, 0, 0);
	root_motion_rotation = Quaternion(0, 0, 0, 1);
	root_motion_scale = Vector3(0, 0, 0);
	root_motion_position_accumulator = Vector3(0, 0, 0);
	root_motion_rotation_accumulator = Quaternion(0, 0, 0, 1);
	root_motion_scale_accumulator = Vector3(1, 1, 1);
}

void AnimationMixer::_create_track_num_to_track_cache_for_animation(const Ref<Animation> &p_animation) {
	if (animation_track_num_to_track_cache.has(p_animation)) {
		// In AnimationMixer::_update_caches, it retrieves all animations via AnimationMixer::get_animation_list
		// Since multiple AnimationLibraries can share the same Animation, it is possible that the cache is already created.
		return;
	}
	LocalVector<TrackCache *> &track_num_to_track_cache = animation_track_num_to_track_cache.insert_new(p_animation, LocalVector<TrackCache *>())->value;
	const LocalVector<Animation::Track *> &tracks = p_animation->get_tracks();

	track_num_to_track_cache.resize(tracks.size());
	for (uint32_t i = 0; i < tracks.size(); i++) {
		TrackCache **track_ptr = track_cache.getptr(tracks[i]->get_unique_id());
		if (track_ptr == nullptr) {
			track_num_to_track_cache[i] = nullptr;
		} else {
			track_num_to_track_cache[i] = *track_ptr;
		}
	}
}

bool AnimationMixer::_update_caches() {
	setup_pass++;
	if (unlikely(setup_pass == 0)) {
		setup_pass = 1;
	}

	root_motion_cache.loc = Vector3(0, 0, 0);
	root_motion_cache.rot = Quaternion(0, 0, 0, 1);
	root_motion_cache.scale = Vector3(1, 1, 1);

	LocalVector<StringName> sname_list;
	get_animation_list(&sname_list);

	bool check_path = GLOBAL_GET_CACHED(bool, "animation/warnings/check_invalid_track_paths");
	bool check_angle_interpolation = GLOBAL_GET_CACHED(bool, "animation/warnings/check_angle_interpolation_type_conflicting");

	Node *parent = get_node_or_null(root_node);
	if (!parent) {
		WARN_PRINT_ONCE(vformat("'%s' is an invalid root_node path, caches will not be built, please check the root_node assignment on: %s", root_node, get_path()));
		cache_valid = false;
		return false;
	}

#ifdef TOOLS_ENABLED
	String mixer_name = "AnimationMixer";
	const Node *owner = get_owner();
	if (owner) {
		const String scene_path = owner->get_scene_file_path();
		if (!scene_path.is_empty()) {
			mixer_name += vformat(" (at: %s)", scene_path.get_file());
		}
	}
#else
	const String mixer_name = "AnimationMixer";
#endif

	Ref<Animation> reset_anim;
	bool has_reset_anim = has_animation(SceneStringName(RESET));
	if (has_reset_anim) {
		reset_anim = get_animation(SceneStringName(RESET));
	}
	for (const StringName &E : sname_list) {
		const Ref<Animation> &anim = get_animation(E);
		for (int i = 0; i < anim->get_track_count(); i++) {
			if (!anim->track_is_enabled(i)) {
				continue;
			}
			NodePath path = anim->track_get_path(i);
			(void)path.hash(); // Make sure the cache is valid for faster comparison.

			const Animation::TrackCacheID &track_unique_id = anim->track_get_unique_id(i);
			Animation::TrackType track_src_type = anim->track_get_type(i);
			Animation::TrackType track_cache_type = Animation::get_cache_type(track_src_type);

			TrackCache *track = nullptr;
			if (TrackCache **p = track_cache.getptr(track_unique_id)) {
				track = *p;
			}

			// If not valid, delete track.
			if (track && (track->type != track_cache_type || ObjectDB::get_instance(track->object_id) == nullptr)) {
				playing_caches.erase(track);
				memdelete(track);
				track_cache.erase(track_unique_id);
				track = nullptr;
			}

			if (!track) {
				Ref<Resource> resource;
				Vector<StringName> leftover_path;

				Node *child = parent->get_node_and_resource(path, resource, leftover_path);
				if (!child) {
					if (check_path) {
						WARN_PRINT_ED(mixer_name + ": '" + String(E) + "', couldn't resolve track:  '" + String(path) + "'. This warning can be disabled in Project Settings.");
					}
					continue;
				}

				switch (track_src_type) {
					case Animation::TYPE_BEZIER:
					case Animation::TYPE_VALUE: {
						// If a value track without a key is cached first, the initial value cannot be determined.
						// It is a corner case, but which may cause problems with blending.
						if (anim->track_get_key_count(i) == 0) {
							WARN_VERBOSE(mixer_name + ": '" + String(E) + "', Value Track:  '" + String(path) + "' must have at least one key to cache for blending.");
							continue;
						}

						TrackCacheValue *track_value = memnew(TrackCacheValue);

						if (resource.is_valid()) {
							track_value->object_id = resource->get_instance_id();
						} else {
							track_value->object_id = child->get_instance_id();
						}

						track_value->is_using_angle = anim->track_get_interpolation_type(i) == Animation::INTERPOLATION_LINEAR_ANGLE || anim->track_get_interpolation_type(i) == Animation::INTERPOLATION_CUBIC_ANGLE;

						track_value->subpath = leftover_path;
						track_value->is_audio_playing = track_src_type == Animation::TYPE_VALUE && resource.is_null() && leftover_path.size() == 1 && leftover_path[0] == SNAME("playing") && child->has_method(SNAME("play_layered"));
						track_value->is_camera_cut = track_src_type == Animation::TYPE_VALUE && resource.is_null() && leftover_path.size() == 1 && leftover_path[0] == SNAME("current") && child->is_class("Camera3D");

						track = track_value;

						bool is_value = track_src_type == Animation::TYPE_VALUE;

						track_value->init_value = is_value ? anim->track_get_key_value(i, 0) : (anim->track_get_key_value(i, 0).operator Array())[0];
						track_value->init_value.zero();

						track_value->is_init = false;

						// Can't interpolate them, need to convert.
						track_value->is_variant_interpolatable = Animation::is_variant_interpolatable(track_value->init_value);

						// If there is a Reset Animation, it takes precedence by overwriting.
						if (has_reset_anim) {
							int rt = reset_anim->find_track(path, track_src_type);
							if (rt >= 0 && reset_anim->track_is_enabled(rt) && reset_anim->track_get_key_count(rt) > 0) {
								if (is_value) {
									track_value->init_value = reset_anim->track_get_key_value(rt, 0);
								} else {
									track_value->init_value = (reset_anim->track_get_key_value(rt, 0).operator Array())[0];
								}
							}
						}

						if (is_value && callback_mode_discrete == ANIMATION_CALLBACK_MODE_DISCRETE_FORCE_CONTINUOUS) {
							if (child) {
								PropertyInfo prop_info;
								ClassDB::get_property_info(child->get_class_name(), path.get_concatenated_subnames(), &prop_info);
								if (prop_info.hint == PROPERTY_HINT_ONESHOT) {
									WARN_PRINT_ED(vformat("%s: '%s', Value Track: '%s' is oneshot property, but will be continuously updated. Consider setting a value other than ANIMATION_CALLBACK_MODE_DISCRETE_FORCE_CONTINUOUS to AnimationMixer.callback_mode_dominant.", mixer_name, String(E), String(path)));
								}
							}
						}
					} break;
					case Animation::TYPE_POSITION_3D:
					case Animation::TYPE_ROTATION_3D:
					case Animation::TYPE_SCALE_3D: {
#ifndef _3D_DISABLED
						Node3D *node_3d = Object::cast_to<Node3D>(child);

						if (!node_3d) {
							ERR_PRINT(mixer_name + ": '" + String(E) + "', transform track does not point to Node3D:  '" + String(path) + "'.");
							continue;
						}

						TrackCacheTransform *track_xform = memnew(TrackCacheTransform);
						track_xform->type = Animation::TYPE_POSITION_3D;

						track_xform->bone_idx = -1;

						bool has_rest = false;
						Skeleton3D *sk = Object::cast_to<Skeleton3D>(node_3d);
						if (sk && path.get_subname_count() == 1) {
							track_xform->skeleton_id = sk->get_instance_id();
							int bone_idx = sk->find_bone(path.get_subname(0));
							if (bone_idx != -1) {
								has_rest = true;
								track_xform->bone_idx = bone_idx;
								Transform3D rest = sk->get_bone_rest(bone_idx);
								track_xform->init_loc = rest.origin;
								track_xform->init_rot = rest.basis.get_rotation_quaternion();
								track_xform->init_scale = rest.basis.get_scale();
							}
						}

						track_xform->object_id = node_3d->get_instance_id();

						track = track_xform;

						switch (track_src_type) {
							case Animation::TYPE_POSITION_3D: {
								track_xform->loc_used = true;
							} break;
							case Animation::TYPE_ROTATION_3D: {
								track_xform->rot_used = true;
							} break;
							case Animation::TYPE_SCALE_3D: {
								track_xform->scale_used = true;
							} break;
							default: {
							}
						}

						// For non Skeleton3D bone animation.
						if (has_reset_anim && !has_rest) {
							int rt = reset_anim->find_track(path, track_src_type);
							if (rt >= 0 && reset_anim->track_is_enabled(rt) && reset_anim->track_get_key_count(rt) > 0) {
								switch (track_src_type) {
									case Animation::TYPE_POSITION_3D: {
										track_xform->init_loc = reset_anim->track_get_key_value(rt, 0);
									} break;
									case Animation::TYPE_ROTATION_3D: {
										track_xform->init_rot = reset_anim->track_get_key_value(rt, 0);
									} break;
									case Animation::TYPE_SCALE_3D: {
										track_xform->init_scale = reset_anim->track_get_key_value(rt, 0);
									} break;
									default: {
									}
								}
							}
						}
#endif // _3D_DISABLED
					} break;
					case Animation::TYPE_BLEND_SHAPE: {
#ifndef _3D_DISABLED
						if (path.get_subname_count() != 1) {
							ERR_PRINT(mixer_name + ": '" + String(E) + "', blend shape track does not contain a blend shape subname:  '" + String(path) + "'.");
							continue;
						}
						MeshInstance3D *mesh_3d = Object::cast_to<MeshInstance3D>(child);

						if (!mesh_3d) {
							ERR_PRINT(mixer_name + ": '" + String(E) + "', blend shape track does not point to MeshInstance3D:  '" + String(path) + "'.");
							continue;
						}

						StringName blend_shape_name = path.get_subname(0);
						int blend_shape_idx = mesh_3d->find_blend_shape_by_name(blend_shape_name);
						if (blend_shape_idx == -1) {
							ERR_PRINT(mixer_name + ": '" + String(E) + "', blend shape track points to a non-existing name:  '" + String(blend_shape_name) + "'.");
							continue;
						}

						TrackCacheBlendShape *track_bshape = memnew(TrackCacheBlendShape);

						track_bshape->shape_index = blend_shape_idx;
						track_bshape->object_id = mesh_3d->get_instance_id();
						track = track_bshape;

						if (has_reset_anim) {
							int rt = reset_anim->find_track(path, track_src_type);
							if (rt >= 0 && reset_anim->track_is_enabled(rt) && reset_anim->track_get_key_count(rt) > 0) {
								track_bshape->init_value = reset_anim->track_get_key_value(rt, 0);
							}
						}
#endif
					} break;
					case Animation::TYPE_TRANSMIT: {
						// Asosdot: nothing to resolve; the cache only marks the track as playable.
						TrackCacheTransmit *track_transmit = memnew(TrackCacheTransmit);
						track_transmit->object_id = child->get_instance_id();
						track = track_transmit;
					} break;
					case Animation::TYPE_METHOD: {
						TrackCacheMethod *track_method = memnew(TrackCacheMethod);

						if (resource.is_valid()) {
							track_method->object_id = resource->get_instance_id();
						} else {
							track_method->object_id = child->get_instance_id();
						}

						track = track_method;

					} break;
					case Animation::TYPE_AUDIO: {
						if (!child->has_method(SNAME("play_layered"))) {
							WARN_PRINT_ED(mixer_name + ": '" + String(E) + "', Audio Track:  '" + String(path) + "' needs an AudioStreamPlayer, AudioStreamPlayer2D or AudioStreamPlayer3D.");
							continue;
						}
						TrackCacheAudio *track_audio = memnew(TrackCacheAudio);

						track_audio->object_id = child->get_instance_id();

						track = track_audio;

					} break;
					case Animation::TYPE_ANIMATION: {
						TrackCacheAnimation *track_animation = memnew(TrackCacheAnimation);

						track_animation->object_id = child->get_instance_id();

						track = track_animation;

					} break;
					default: {
						ERR_PRINT("Animation corrupted (invalid track type).");
						continue;
					}
				}
				track->path = path;
				track_cache[track_unique_id] = track;
			} else if (track_cache_type == Animation::TYPE_POSITION_3D) {
				TrackCacheTransform *track_xform = static_cast<TrackCacheTransform *>(track);
				if (track->setup_pass != setup_pass) {
					track_xform->loc_used = false;
					track_xform->rot_used = false;
					track_xform->scale_used = false;
				}
				switch (track_src_type) {
					case Animation::TYPE_POSITION_3D: {
						track_xform->loc_used = true;
					} break;
					case Animation::TYPE_ROTATION_3D: {
						track_xform->rot_used = true;
					} break;
					case Animation::TYPE_SCALE_3D: {
						track_xform->scale_used = true;
					} break;
					default: {
					}
				}
			} else if (track_cache_type == Animation::TYPE_VALUE) {
				TrackCacheValue *track_value = static_cast<TrackCacheValue *>(track);
				// If it has at least one angle interpolation, it also uses angle interpolation for blending.
				bool was_using_angle = track_value->is_using_angle;
				if (track_src_type == Animation::TYPE_VALUE) {
					if (track_value->init_value.is_string() && anim->value_track_get_update_mode(i) != Animation::UPDATE_DISCRETE) {
						WARN_PRINT_ONCE_ED(mixer_name + ": '" + String(E) + "', Value Track: '" + String(path) + "' blends String types. This is an experimental algorithm.");
					}
					track_value->is_using_angle = track_value->is_using_angle || anim->track_get_interpolation_type(i) == Animation::INTERPOLATION_LINEAR_ANGLE || anim->track_get_interpolation_type(i) == Animation::INTERPOLATION_CUBIC_ANGLE;
				}
				if (check_angle_interpolation && (was_using_angle != track_value->is_using_angle)) {
					WARN_PRINT_ED(mixer_name + ": '" + String(E) + "', Value Track: '" + String(path) + "' has different interpolation types for rotation between some animations which may be blended together. Blending prioritizes angle interpolation, so the blending result uses the shortest path referenced to the initial (RESET animation) value.");
				}
			}

			track->setup_pass = setup_pass;
		}
	}

	LocalVector<Animation::TrackCacheID> to_delete;

	for (const KeyValue<Animation::TrackCacheID, TrackCache *> &K : track_cache) {
		if (K.value->setup_pass != setup_pass) {
			to_delete.push_back(K.key);
		}
	}

	for (const Animation::TrackCacheID &unique_id : to_delete) {
		memdelete(track_cache[unique_id]);
		track_cache.erase(unique_id);
	}

	track_map.clear();

	int idx = 0;
	for (const KeyValue<Animation::TrackCacheID, TrackCache *> &K : track_cache) {
		track_map[K.value->path] = idx;
		idx++;
	}

	for (KeyValue<Animation::TrackCacheID, TrackCache *> &K : track_cache) {
		K.value->blend_idx = track_map[K.value->path];
	}

	track_map_version++;
	if (track_map_version == 0) {
		track_map_version = 1;
	}

	animation_track_num_to_track_cache.clear();
	for (const StringName &E : sname_list) {
		const Ref<Animation> &anim = get_animation(E);
		_create_track_num_to_track_cache_for_animation(anim);
	}

	track_count = idx;

	cache_valid = true;

	return true;
}

/* -------------------------------------------- */
/* -- Blending processor ---------------------- */
/* -------------------------------------------- */

void AnimationMixer::_process_animation(double p_delta, bool p_update_only) {
	_blend_init();
	if (cache_valid && _blend_pre_process(p_delta, track_count, track_map)) {
		_blend_capture(p_delta);
		_blend_calc_total_weight();
		_blend_process(p_delta, p_update_only);
		clear_animation_instances();
		_blend_apply();
		_blend_post_process();
		emit_signal(SNAME("mixer_applied"));
	} else {
		clear_animation_instances();
	}
}

Variant AnimationMixer::_post_process_key_value(const Ref<Animation> &p_anim, int p_track, Variant &p_value, ObjectID p_object_id, int p_object_sub_idx) {
#ifndef _3D_DISABLED
	switch (p_anim->track_get_type(p_track)) {
		case Animation::TYPE_POSITION_3D: {
			if (p_object_sub_idx >= 0) {
				Skeleton3D *skel = ObjectDB::get_instance<Skeleton3D>(p_object_id);
				if (skel) {
					return Vector3(p_value) * skel->get_motion_scale();
				}
			}
			return p_value;
		} break;
		default: {
		} break;
	}
#endif // _3D_DISABLED
	return p_value;
}

Variant AnimationMixer::post_process_key_value(const Ref<Animation> &p_anim, int p_track, Variant p_value, ObjectID p_object_id, int p_object_sub_idx) {
	if (is_GDVIRTUAL_CALL_post_process_key_value) {
		Variant res;
		if (GDVIRTUAL_CALL(_post_process_key_value, p_anim, p_track, p_value, p_object_id, p_object_sub_idx, res)) {
			return res;
		}
		is_GDVIRTUAL_CALL_post_process_key_value = false;
	}
	return _post_process_key_value(p_anim, p_track, p_value, p_object_id, p_object_sub_idx);
}

void AnimationMixer::_blend_init() {
	// Check all tracks, see if they need modification.
	root_motion_position = Vector3(0, 0, 0);
	root_motion_rotation = Quaternion(0, 0, 0, 1);
	root_motion_scale = Vector3(0, 0, 0);
	root_motion_position_accumulator = Vector3(0, 0, 0);
	root_motion_rotation_accumulator = Quaternion(0, 0, 0, 1);
	root_motion_scale_accumulator = Vector3(1, 1, 1);

	if (!cache_valid) {
		if (!_update_caches()) {
			return;
		}
	}

	// Init all value/transform/blend/bezier tracks that track_cache has.
	for (const KeyValue<Animation::TrackCacheID, TrackCache *> &K : track_cache) {
		TrackCache *track = K.value;

		track->total_weight = 0.0;

		switch (track->type) {
			case Animation::TYPE_POSITION_3D: {
				TrackCacheTransform *t = static_cast<TrackCacheTransform *>(track);
				if (track->root_motion) {
					root_motion_cache.loc = Vector3(0, 0, 0);
					root_motion_cache.rot = Quaternion(0, 0, 0, 1);
					root_motion_cache.scale = Vector3(1, 1, 1);
				}
				t->loc = t->init_loc;
				t->rot = t->init_rot;
				t->scale = t->init_scale;
			} break;
			case Animation::TYPE_BLEND_SHAPE: {
				TrackCacheBlendShape *t = static_cast<TrackCacheBlendShape *>(track);
				t->value = t->init_value;
			} break;
			case Animation::TYPE_VALUE: {
				TrackCacheValue *t = static_cast<TrackCacheValue *>(track);
				t->value = Animation::cast_to_blendwise(t->init_value);
				t->element_size = t->init_value.is_string() ? (real_t)(t->init_value.operator String()).length() : 0;
				t->use_continuous = false;
				t->use_discrete = false;
			} break;
			case Animation::TYPE_AUDIO: {
				TrackCacheAudio *t = static_cast<TrackCacheAudio *>(track);
				for (KeyValue<Pair<ObjectID, int>, PlayingAudioTrackInfo> &L : t->playing_streams) {
					PlayingAudioTrackInfo &track_info = L.value;
					track_info.volume = 0.0;
				}
			} break;
			default: {
			} break;
		}
	}
}

bool AnimationMixer::_blend_pre_process(double p_delta, int p_track_count, const AHashMap<NodePath, int> &p_track_map) {
	return true;
}

void AnimationMixer::_blend_post_process() {
	//
}

void AnimationMixer::_blend_capture(double p_delta) {
	blend_capture(p_delta);
}

void AnimationMixer::blend_capture(double p_delta) {
	if (capture_cache.animation.is_null()) {
		return;
	}

	capture_cache.remain -= p_delta * capture_cache.step;
	if (Animation::is_less_or_equal_approx(capture_cache.remain, 0)) {
		if (capture_cache.animation.is_valid()) {
			animation_track_num_to_track_cache.erase(capture_cache.animation);
		}
		capture_cache.clear();
		return;
	}

	real_t weight = Tween::run_equation(capture_cache.trans_type, capture_cache.ease_type, capture_cache.remain, 0.0, 1.0, 1.0);

	// Blend with other animations.
	real_t inv = 1.0 - weight;
	for (AnimationInstance &ai : animation_instances) {
		ai.playback_info.weight *= inv;
	}

	// Build capture animation instance.
	PlaybackInfo pi;
	pi.weight = weight;

	AnimationInstance ai;
	ai.animation = capture_cache.animation;
	ai.playback_info = pi;

	animation_instances.push_back(ai);
}

void AnimationMixer::_blend_calc_total_weight() {
	for (const AnimationInstance &ai : animation_instances) {
		const Ref<Animation> &a = ai.animation;
		real_t weight = ai.playback_info.weight;
		if (Math::is_zero_approx(weight)) {
			continue;
		}
		Span<real_t> track_weights = ai.playback_info.track_weights != nullptr ? *ai.playback_info.track_weights : Span<real_t>();

		LocalVector<TrackCache *> *t_cache = animation_track_num_to_track_cache.getptr(a);
		ERR_CONTINUE_EDMSG(!t_cache, "No animation in cache.");
		LocalVector<TrackCache *> &track_num_to_track_cache = *t_cache;

		uint64_t pass_id = ++animation_instance_weight_pass_counter;
		// Handle wrap (slower but rare).
		if (unlikely(pass_id == 0)) {
			for (KeyValue<Animation::TrackCacheID, TrackCache *> &kv : track_cache) {
				if (kv.value) {
					kv.value->animation_instance_weight_applied_at = 0;
				}
			}
			animation_instance_weight_pass_counter = 1;
			pass_id = 1;
		}

		const LocalVector<Animation::Track *> &tracks = a->get_tracks();
		Animation::Track *const *tracks_ptr = tracks.ptr();
		int count = tracks.size();
		for (int i = 0; i < count; i++) {
			Animation::Track *animation_track = tracks_ptr[i];
			if (!animation_track->enabled) {
				continue;
			}
			TrackCache *track = track_num_to_track_cache[i];
			if (track == nullptr) {
				// No path, but avoid error spamming.
				continue;
			}

			// In some cases (e.g. TrackCacheTransform),
			// multiple Animation::Tracks (e.g. TYPE_POSITION_3D, TYPE_ROTATION_3D and TYPE_SCALE_3D)
			// can point to the same TrackCache instance.
			// So we need to make sure that the weight is added only once per AnimationInstance.
			if (track->animation_instance_weight_applied_at == pass_id) {
				continue;
			}

			int blend_idx = track->blend_idx;
			ERR_CONTINUE(blend_idx < 0 || blend_idx >= track_count);
			real_t blend;
			if (!track_weights.is_empty() && blend_idx < static_cast<int>(track_weights.size())) {
				blend = track_weights[blend_idx] * weight;
			} else {
				blend = weight;
			}
			track->total_weight += blend;
			track->animation_instance_weight_applied_at = pass_id;
		}
	}
}

void AnimationMixer::_blend_process(double p_delta, bool p_update_only) {
	// Apply value/transform/blend/bezier blends to track caches and execute method/audio/animation tracks.
#ifdef TOOLS_ENABLED
	bool can_call = is_inside_tree() && !Engine::get_singleton()->is_editor_hint();
#endif // TOOLS_ENABLED
	for (const AnimationInstance &ai : animation_instances) {
		const Ref<Animation> &a = ai.animation;
		double time = ai.playback_info.time;
		double delta = ai.playback_info.delta;
		double start = ai.playback_info.start;
		double end = ai.playback_info.end;
		bool seeked = ai.playback_info.seeked;
		Animation::LoopedFlag looped_flag = ai.playback_info.looped_flag;
		bool is_external_seeking = ai.playback_info.is_external_seeking;
		real_t weight = ai.playback_info.weight;
		bool backward = std::signbit(delta); // This flag is used by the root motion calculates or detecting the end of audio stream.
		bool seeked_backward = std::signbit(p_delta);
#ifndef _3D_DISABLED
		bool calc_root = !seeked || is_external_seeking;
#endif // _3D_DISABLED
		LocalVector<TrackCache *> *t_cache = animation_track_num_to_track_cache.getptr(a);
		ERR_CONTINUE_EDMSG(!t_cache, "No animation in cache.");
		LocalVector<TrackCache *> &track_num_to_track_cache = *t_cache;

		const LocalVector<Animation::Track *> &tracks = a->get_tracks();
		Animation::Track *const *tracks_ptr = tracks.ptr();
		double a_length = a->get_length();
		int count = tracks.size();
		for (int i = 0; i < count; i++) {
			const Animation::Track *animation_track = tracks_ptr[i];
			if (!animation_track->enabled) {
				continue;
			}
			TrackCache *track = track_num_to_track_cache[i];
			if (track == nullptr) {
				continue; // No path, but avoid error spamming.
			}
			int blend_idx = track->blend_idx;
			ERR_CONTINUE(blend_idx < 0 || blend_idx >= track_count);
			real_t blend;
			Span<real_t> track_weights = ai.playback_info.track_weights != nullptr ? *ai.playback_info.track_weights : Span<real_t>();
			if (!track_weights.is_empty() && blend_idx < static_cast<int>(track_weights.size())) {
				blend = track_weights[blend_idx] * weight;
			} else {
				blend = weight;
			}

			if (!deterministic) {
				// If non-deterministic, do normalization.
				// It would be better to make this if statement outside the for loop, but come here since too much code...
				if (Math::is_zero_approx(track->total_weight)) {
					continue;
				}
				blend = blend / track->total_weight;
			}
			Animation::TrackType ttype = animation_track->type;
			track->root_motion = root_motion_track == animation_track->path;
			switch (ttype) {
				case Animation::TYPE_POSITION_3D: {
#ifndef _3D_DISABLED
					if (Math::is_zero_approx(blend)) {
						continue; // Nothing to blend.
					}
					TrackCacheTransform *t = static_cast<TrackCacheTransform *>(track);
					if (track->root_motion && calc_root) {
						int rot_track = -1;
						if (root_motion_local) {
							rot_track = a->find_track(a->track_get_path(i), Animation::TYPE_ROTATION_3D);
						}
						double prev_time = time - delta;
						if (!backward) {
							if (Animation::is_less_approx(prev_time, start)) {
								switch (a->get_loop_mode()) {
									case Animation::LOOP_NONE: {
										prev_time = start;
									} break;
									case Animation::LOOP_LINEAR: {
										prev_time = Math::fposmod(prev_time - start, end - start) + start;
									} break;
									case Animation::LOOP_PINGPONG: {
										prev_time = Math::pingpong(prev_time - start, end - start) + start;
									} break;
									default:
										break;
								}
							}
						} else {
							if (Animation::is_greater_approx(prev_time, end)) {
								switch (a->get_loop_mode()) {
									case Animation::LOOP_NONE: {
										prev_time = end;
									} break;
									case Animation::LOOP_LINEAR: {
										prev_time = Math::fposmod(prev_time - start, end - start) + start;
									} break;
									case Animation::LOOP_PINGPONG: {
										prev_time = Math::pingpong(prev_time - start, end - start) + start;
									} break;
									default:
										break;
								}
							}
						}
						if (rot_track >= 0) {
							Vector3 loc[2];
							Quaternion rot;
							if (!backward) {
								if (Animation::is_greater_approx(prev_time, time)) {
									Error err = a->try_position_track_interpolate(i, prev_time, &loc[0]);
									if (err != OK) {
										continue;
									}
									loc[0] = post_process_key_value(a, i, loc[0], t->object_id, t->bone_idx);
									a->try_position_track_interpolate(i, end, &loc[1]);
									loc[1] = post_process_key_value(a, i, loc[1], t->object_id, t->bone_idx);

									a->try_rotation_track_interpolate(rot_track, end, &rot);
									rot = post_process_key_value(a, rot_track, rot, t->object_id, t->bone_idx);

									root_motion_cache.loc += rot.xform_inv(loc[1] - loc[0]) * blend;
									prev_time = start;
								}
							} else {
								if (Animation::is_less_approx(prev_time, time)) {
									Error err = a->try_position_track_interpolate(i, prev_time, &loc[0]);
									if (err != OK) {
										continue;
									}
									loc[0] = post_process_key_value(a, i, loc[0], t->object_id, t->bone_idx);
									a->try_position_track_interpolate(i, start, &loc[1]);
									loc[1] = post_process_key_value(a, i, loc[1], t->object_id, t->bone_idx);

									a->try_rotation_track_interpolate(rot_track, start, &rot);
									rot = post_process_key_value(a, rot_track, rot, t->object_id, t->bone_idx);

									root_motion_cache.loc += rot.xform_inv(loc[1] - loc[0]) * blend;
									prev_time = end;
								}
							}
							Error err = a->try_position_track_interpolate(i, prev_time, &loc[0]);
							if (err != OK) {
								continue;
							}
							loc[0] = post_process_key_value(a, i, loc[0], t->object_id, t->bone_idx);
							a->try_position_track_interpolate(i, time, &loc[1]);
							loc[1] = post_process_key_value(a, i, loc[1], t->object_id, t->bone_idx);

							a->try_rotation_track_interpolate(rot_track, time, &rot);
							rot = post_process_key_value(a, rot_track, rot, t->object_id, t->bone_idx);

							root_motion_cache.loc += rot.xform_inv(loc[1] - loc[0]) * blend;
							prev_time = !backward ? start : end;
						} else {
							Vector3 loc[2];
							if (!backward) {
								if (Animation::is_greater_approx(prev_time, time)) {
									Error err = a->try_position_track_interpolate(i, prev_time, &loc[0]);
									if (err != OK) {
										continue;
									}
									loc[0] = post_process_key_value(a, i, loc[0], t->object_id, t->bone_idx);
									a->try_position_track_interpolate(i, end, &loc[1]);
									loc[1] = post_process_key_value(a, i, loc[1], t->object_id, t->bone_idx);
									root_motion_cache.loc += (loc[1] - loc[0]) * blend;
									prev_time = start;
								}
							} else {
								if (Animation::is_less_approx(prev_time, time)) {
									Error err = a->try_position_track_interpolate(i, prev_time, &loc[0]);
									if (err != OK) {
										continue;
									}
									loc[0] = post_process_key_value(a, i, loc[0], t->object_id, t->bone_idx);
									a->try_position_track_interpolate(i, start, &loc[1]);
									loc[1] = post_process_key_value(a, i, loc[1], t->object_id, t->bone_idx);
									root_motion_cache.loc += (loc[1] - loc[0]) * blend;
									prev_time = end;
								}
							}
							Error err = a->try_position_track_interpolate(i, prev_time, &loc[0]);
							if (err != OK) {
								continue;
							}
							loc[0] = post_process_key_value(a, i, loc[0], t->object_id, t->bone_idx);
							a->try_position_track_interpolate(i, time, &loc[1]);
							loc[1] = post_process_key_value(a, i, loc[1], t->object_id, t->bone_idx);
							root_motion_cache.loc += (loc[1] - loc[0]) * blend;
							prev_time = !backward ? start : end;
						}
					}
					{
						Vector3 loc;
						Error err = a->try_position_track_interpolate(i, time, &loc);
						if (err != OK) {
							continue;
						}
						loc = post_process_key_value(a, i, loc, t->object_id, t->bone_idx);
						t->loc += (loc - t->init_loc) * blend;
					}
#endif // _3D_DISABLED
				} break;
				case Animation::TYPE_ROTATION_3D: {
#ifndef _3D_DISABLED
					if (Math::is_zero_approx(blend)) {
						continue; // Nothing to blend.
					}
					TrackCacheTransform *t = static_cast<TrackCacheTransform *>(track);
					if (track->root_motion && calc_root) {
						double prev_time = time - delta;
						if (!backward) {
							if (Animation::is_less_approx(prev_time, start)) {
								switch (a->get_loop_mode()) {
									case Animation::LOOP_NONE: {
										prev_time = start;
									} break;
									case Animation::LOOP_LINEAR: {
										prev_time = Math::fposmod(prev_time - start, end - start) + start;
									} break;
									case Animation::LOOP_PINGPONG: {
										prev_time = Math::pingpong(prev_time - start, end - start) + start;
									} break;
									default:
										break;
								}
							}
						} else {
							if (Animation::is_greater_approx(prev_time, end)) {
								switch (a->get_loop_mode()) {
									case Animation::LOOP_NONE: {
										prev_time = end;
									} break;
									case Animation::LOOP_LINEAR: {
										prev_time = Math::fposmod(prev_time - start, end - start) + start;
									} break;
									case Animation::LOOP_PINGPONG: {
										prev_time = Math::pingpong(prev_time - start, end - start) + start;
									} break;
									default:
										break;
								}
							}
						}
						Quaternion rot[2];
						if (!backward) {
							if (Animation::is_greater_approx(prev_time, time)) {
								Error err = a->try_rotation_track_interpolate(i, prev_time, &rot[0]);
								if (err != OK) {
									continue;
								}
								rot[0] = post_process_key_value(a, i, rot[0], t->object_id, t->bone_idx);
								a->try_rotation_track_interpolate(i, end, &rot[1]);
								rot[1] = post_process_key_value(a, i, rot[1], t->object_id, t->bone_idx);
								root_motion_cache.rot = Animation::interpolate_via_rest(root_motion_cache.rot, rot[1], blend, rot[0]);
								prev_time = start;
							}
						} else {
							if (Animation::is_less_approx(prev_time, time)) {
								Error err = a->try_rotation_track_interpolate(i, prev_time, &rot[0]);
								if (err != OK) {
									continue;
								}
								rot[0] = post_process_key_value(a, i, rot[0], t->object_id, t->bone_idx);
								a->try_rotation_track_interpolate(i, start, &rot[1]);
								rot[1] = post_process_key_value(a, i, rot[1], t->object_id, t->bone_idx);
								root_motion_cache.rot = Animation::interpolate_via_rest(root_motion_cache.rot, rot[1], blend, rot[0]);
								prev_time = end;
							}
						}
						Error err = a->try_rotation_track_interpolate(i, prev_time, &rot[0]);
						if (err != OK) {
							continue;
						}
						rot[0] = post_process_key_value(a, i, rot[0], t->object_id, t->bone_idx);
						a->try_rotation_track_interpolate(i, time, &rot[1]);
						rot[1] = post_process_key_value(a, i, rot[1], t->object_id, t->bone_idx);
						root_motion_cache.rot = Animation::interpolate_via_rest(root_motion_cache.rot, rot[1], blend, rot[0]);
						prev_time = !backward ? start : end;
					}
					{
						Quaternion rot;
						Error err = a->try_rotation_track_interpolate(i, time, &rot);
						if (err != OK) {
							continue;
						}
						rot = post_process_key_value(a, i, rot, t->object_id, t->bone_idx);
						t->rot = Animation::interpolate_via_rest(t->rot, rot, blend, t->init_rot);
					}
#endif // _3D_DISABLED
				} break;
				case Animation::TYPE_SCALE_3D: {
#ifndef _3D_DISABLED
					if (Math::is_zero_approx(blend)) {
						continue; // Nothing to blend.
					}
					TrackCacheTransform *t = static_cast<TrackCacheTransform *>(track);
					if (track->root_motion && calc_root) {
						double prev_time = time - delta;
						if (!backward) {
							if (Animation::is_less_approx(prev_time, start)) {
								switch (a->get_loop_mode()) {
									case Animation::LOOP_NONE: {
										prev_time = start;
									} break;
									case Animation::LOOP_LINEAR: {
										prev_time = Math::fposmod(prev_time - start, end - start) + start;
									} break;
									case Animation::LOOP_PINGPONG: {
										prev_time = Math::pingpong(prev_time - start, end - start) + start;
									} break;
									default:
										break;
								}
							}
						} else {
							if (Animation::is_greater_approx(prev_time, end)) {
								switch (a->get_loop_mode()) {
									case Animation::LOOP_NONE: {
										prev_time = end;
									} break;
									case Animation::LOOP_LINEAR: {
										prev_time = Math::fposmod(prev_time - start, end - start) + start;
									} break;
									case Animation::LOOP_PINGPONG: {
										prev_time = Math::pingpong(prev_time - start, end - start) + start;
									} break;
									default:
										break;
								}
							}
						}
						Vector3 scale[2];
						if (!backward) {
							if (Animation::is_greater_approx(prev_time, time)) {
								Error err = a->try_scale_track_interpolate(i, prev_time, &scale[0]);
								if (err != OK) {
									continue;
								}
								scale[0] = post_process_key_value(a, i, scale[0], t->object_id, t->bone_idx);
								a->try_scale_track_interpolate(i, end, &scale[1]);
								scale[1] = post_process_key_value(a, i, scale[1], t->object_id, t->bone_idx);
								root_motion_cache.scale += (scale[1] - scale[0]) * blend;
								prev_time = start;
							}
						} else {
							if (Animation::is_less_approx(prev_time, time)) {
								Error err = a->try_scale_track_interpolate(i, prev_time, &scale[0]);
								if (err != OK) {
									continue;
								}
								scale[0] = post_process_key_value(a, i, scale[0], t->object_id, t->bone_idx);
								a->try_scale_track_interpolate(i, start, &scale[1]);
								scale[1] = post_process_key_value(a, i, scale[1], t->object_id, t->bone_idx);
								root_motion_cache.scale += (scale[1] - scale[0]) * blend;
								prev_time = end;
							}
						}
						Error err = a->try_scale_track_interpolate(i, prev_time, &scale[0]);
						if (err != OK) {
							continue;
						}
						scale[0] = post_process_key_value(a, i, scale[0], t->object_id, t->bone_idx);
						a->try_scale_track_interpolate(i, time, &scale[1]);
						scale[1] = post_process_key_value(a, i, scale[1], t->object_id, t->bone_idx);
						root_motion_cache.scale += (scale[1] - scale[0]) * blend;
						prev_time = !backward ? start : end;
					}
					{
						Vector3 scale;
						Error err = a->try_scale_track_interpolate(i, time, &scale);
						if (err != OK) {
							continue;
						}
						scale = post_process_key_value(a, i, scale, t->object_id, t->bone_idx);
						t->scale += (scale - t->init_scale) * blend;
					}
#endif // _3D_DISABLED
				} break;
				case Animation::TYPE_BLEND_SHAPE: {
#ifndef _3D_DISABLED
					if (Math::is_zero_approx(blend)) {
						continue; // Nothing to blend.
					}
					TrackCacheBlendShape *t = static_cast<TrackCacheBlendShape *>(track);
					float value;
					Error err = a->try_blend_shape_track_interpolate(i, time, &value);
					//ERR_CONTINUE(err!=OK); //used for testing, should be removed
					if (err != OK) {
						continue;
					}
					value = post_process_key_value(a, i, value, t->object_id, t->shape_index);
					t->value += (value - t->init_value) * blend;
#endif // _3D_DISABLED
				} break;
				case Animation::TYPE_BEZIER:
				case Animation::TYPE_VALUE: {
					if (Math::is_zero_approx(blend)) {
						continue; // Nothing to blend.
					}
					TrackCacheValue *t = static_cast<TrackCacheValue *>(track);
					if (t->is_audio_playing) {
						_process_audio_playing_track(t, a, i, time, delta, start, end, looped_flag, seeked, backward, p_update_only);
						continue;
					}
					if (t->is_camera_cut) {
						_process_camera_cut_track(t, a, i, time, delta, start, end, looped_flag, seeked);
						continue;
					}
					bool is_value = ttype == Animation::TYPE_VALUE;
					bool is_discrete = is_value && a->value_track_get_update_mode(i) == Animation::UPDATE_DISCRETE;
					bool force_continuous = callback_mode_discrete == ANIMATION_CALLBACK_MODE_DISCRETE_FORCE_CONTINUOUS;
					if (!is_discrete || force_continuous) {
						t->use_continuous = true;

						Variant value;
						if (t->is_variant_interpolatable) {
							value = is_value ? a->value_track_interpolate(i, time, is_discrete && force_continuous ? backward : false) : Variant(a->bezier_track_interpolate(i, time));
							value = post_process_key_value(a, i, value, t->object_id);
							if (value == Variant()) {
								continue;
							}
						} else {
							// Discrete track sets the value in the current _blend_process() function,
							// but Force Continuous track does not set the value here because the value must be set in the _blend_apply() function later.
							int idx = a->track_find_key(i, time, Animation::FIND_MODE_NEAREST, false, backward);
							if (idx < 0) {
								continue;
							}
							value = a->track_get_key_value(i, idx);
							value = post_process_key_value(a, i, value, t->object_id);
							if (value == Variant()) {
								continue;
							}
							t->value = value;
							continue;
						}

						// Special case for angle interpolation.
						if (t->is_using_angle) {
							// For blending consistency, it prevents rotation of more than 180 degrees from init_value.
							// This is the same with Quaternion blending.
							t->value = Animation::interpolate_via_rest((double)t->value, (double)value, blend, (double)t->init_value);
						} else {
							value = Animation::cast_to_blendwise(value);
							if (t->init_value.is_array()) {
								t->element_size = MAX(t->element_size.operator int(), (value.operator Array()).size());
							} else if (t->init_value.is_string()) {
								real_t length = Animation::subtract_variant((real_t)(value.operator Array()).size(), (real_t)(t->init_value.operator String()).length());
								t->element_size = Animation::blend_variant(t->element_size, length, blend);
							}
							value = Animation::subtract_variant(value, Animation::cast_to_blendwise(t->init_value));
							t->value = Animation::blend_variant(t->value, value, blend);
						}
					} else {
						if (seeked) {
							int idx = a->track_find_key(i, time, is_external_seeking ? Animation::FIND_MODE_NEAREST : Animation::FIND_MODE_EXACT, false, seeked_backward);
							if (idx < 0) {
								continue;
							}
							t->use_discrete = true;
							Variant value = a->track_get_key_value(i, idx);
							value = post_process_key_value(a, i, value, t->object_id);
							Object *t_obj = ObjectDB::get_instance(t->object_id);
							if (t_obj) {
								t_obj->set_indexed(t->subpath, value);
							}
						} else {
							LocalVector<int> indices;
							a->track_get_key_indices_in_range(i, time, delta, start, end, &indices, looped_flag);
							for (int &F : indices) {
								t->use_discrete = true;
								Variant value = a->track_get_key_value(i, F);
								value = post_process_key_value(a, i, value, t->object_id);
								Object *t_obj = ObjectDB::get_instance(t->object_id);
								if (t_obj) {
									t_obj->set_indexed(t->subpath, value);
								}
							}
						}
					}
				} break;
				case Animation::TYPE_METHOD: {
#ifdef TOOLS_ENABLED
					if (!can_call) {
						continue;
					}
#endif // TOOLS_ENABLED
					if (p_update_only || Math::is_zero_approx(blend)) {
						continue;
					}
					TrackCacheMethod *t = static_cast<TrackCacheMethod *>(track);
					if (seeked) {
						int idx = a->track_find_key(i, time, is_external_seeking ? Animation::FIND_MODE_NEAREST : Animation::FIND_MODE_EXACT, true);
						if (idx < 0) {
							continue;
						}
						StringName method = a->method_track_get_name(i, idx);
						Vector<Variant> params = a->method_track_get_params(i, idx);
						_call_object(t->object_id, method, params, callback_mode_method == ANIMATION_CALLBACK_MODE_METHOD_DEFERRED);
					} else {
						LocalVector<int> indices;
						a->track_get_key_indices_in_range(i, time, delta, start, end, &indices, looped_flag);
						for (int &F : indices) {
							StringName method = a->method_track_get_name(i, F);
							Vector<Variant> params = a->method_track_get_params(i, F);
							_call_object(t->object_id, method, params, callback_mode_method == ANIMATION_CALLBACK_MODE_METHOD_DEFERRED);
						}
					}
				} break;
				case Animation::TYPE_TRANSMIT: {
#ifdef TOOLS_ENABLED
					if (!can_call) {
						continue;
					}
#endif // TOOLS_ENABLED
					if (p_update_only || Math::is_zero_approx(blend) || !GlobalTransmit::get_singleton()) {
						continue;
					}
					const bool deferred = callback_mode_method == ANIMATION_CALLBACK_MODE_METHOD_DEFERRED;
					if (seeked) {
						int idx = a->track_find_key(i, time, is_external_seeking ? Animation::FIND_MODE_NEAREST : Animation::FIND_MODE_EXACT, true);
						if (idx < 0) {
							continue;
						}
						_transmit(a->transmit_track_get_channel(i, idx), a->transmit_track_get_arguments(i, idx), deferred);
					} else {
						LocalVector<int> indices;
						a->track_get_key_indices_in_range(i, time, delta, start, end, &indices, looped_flag);
						for (int &F : indices) {
							_transmit(a->transmit_track_get_channel(i, F), a->transmit_track_get_arguments(i, F), deferred);
						}
					}
				} break;
				case Animation::TYPE_AUDIO: {
					// The end of audio should be observed even if the blend value is 0, build up the information and store to the cache for that.
					TrackCacheAudio *t = static_cast<TrackCacheAudio *>(track);
					Object *t_obj = ObjectDB::get_instance(t->object_id);
					Node *asp = t_obj ? Object::cast_to<Node>(t_obj) : nullptr;
					if (!t_obj || !asp) {
						t->playing_streams.clear();
						continue;
					}
					PlayingAudioTrackInfo &track_info = t->playing_streams[Pair<ObjectID, int>(a->get_instance_id(), i)];
					track_info.length = a_length;
					track_info.time = time;
					track_info.volume += blend;
					track_info.loop = a->get_loop_mode() != Animation::LOOP_NONE;
					track_info.backward = backward;
					track_info.use_blend = a->audio_track_is_use_blend(i);

					// Asosdot: while the editor scrubs, and while playing backwards, sounds sit
					// under the playhead and move with it, like audio in a video editor.
					if ((p_update_only && audio_scrubbing) || (backward && !seeked && !p_update_only)) {
						_follow_audio_playhead(t_obj, track_info, a, i, time);
						continue;
					}

					// Main process to fire key is started from here.
					if (p_update_only) {
						continue;
					}
					// Resume what sounds under the playhead after a seek, or after following the
					// playhead, when sounds go back to playing forwards at their own pace.
					const bool resume = seeked || !track_info.scrubbed.is_empty();
					_stop_scrubbed_audio(t_obj, track_info);
					if (_is_editor_preview_stopped()) {
						continue;
					}
					if (resume) {
						// Every key still sounding at the new time resumes from where it would be by now.
						// What this track played before the seek is let go first, so nothing doubles up.
						for (const KeyValue<uint64_t, PlayingAudioStreamInfo> &M : track_info.stream_info) {
							_stop_audio_sound(t_obj, M.value.index);
						}
						track_info.stream_info.clear();
						int latest = -1;
						const int key_count = a->track_get_key_count(i);
						while (latest + 1 < key_count && !Animation::is_greater_approx(a->track_get_key_time(i, latest + 1), time)) {
							latest++;
						}
						for (int k = 0; k <= latest; k++) {
							_play_audio_key(t, t_obj, track_info, a, i, k, time, true, k == latest);
						}
					} else {
						// Every key passed since the last update plays, however many fall in one frame.
						LocalVector<int> to_play;
						a->track_get_key_indices_in_range(i, time, delta, start, end, &to_play, looped_flag);
						for (const int idx : to_play) {
							_play_audio_key(t, t_obj, track_info, a, i, idx, time, false, false);
						}
					}
				} break;
				case Animation::TYPE_ANIMATION: {
					if (Math::is_zero_approx(blend)) {
						continue;
					}
					TrackCacheAnimation *t = static_cast<TrackCacheAnimation *>(track);
					Object *t_obj = ObjectDB::get_instance(t->object_id);
					if (!t_obj) {
						continue;
					}
					AnimationPlayer *player2 = Object::cast_to<AnimationPlayer>(t_obj);
					if (!player2) {
						continue;
					}
					// TODO: Make it possible to embed section info in animation track keys.
					if (seeked) {
						// Seek.
						int idx = a->track_find_key(i, time, Animation::FIND_MODE_NEAREST, true);
						if (idx < 0) {
							continue;
						}
						double pos = a->track_get_key_time(i, idx);
						StringName anim_name = a->animation_track_get_key_animation(i, idx);
						const Ref<Animation> &anim = player2->get_animation_or_null(anim_name);
						if (anim_name == SNAME("[stop]") || anim.is_null()) {
							continue;
						}
						double at_anim_pos = start;
						switch (anim->get_loop_mode()) {
							case Animation::LOOP_NONE: {
								if (!is_external_seeking && ((!backward && Animation::is_greater_or_equal_approx(time, pos + end)) || (backward && Animation::is_less_or_equal_approx(time, pos + start)))) {
									continue; // Do nothing if current time is outside of length when started.
								}
								at_anim_pos = MIN(end, time - pos); // Seek to end.
							} break;
							case Animation::LOOP_LINEAR: {
								at_anim_pos = Math::fposmod(time - pos - start, end - start) + start; // Seek to loop.
							} break;
							case Animation::LOOP_PINGPONG: {
								at_anim_pos = Math::pingpong(time - pos - start, end - start) + start;
							} break;
							default:
								break;
						}
						if (player2->is_playing() || !is_external_seeking) {
							player2->seek(at_anim_pos, false, p_update_only);
							player2->play(anim_name);
							t->playing = true;
							playing_caches.insert(t);
						} else {
							player2->set_assigned_animation(anim_name);
							player2->seek(at_anim_pos, true, p_update_only);
						}
					} else {
						// Find stuff to play.
						LocalVector<int> to_play;
						a->track_get_key_indices_in_range(i, time, delta, start, end, &to_play, looped_flag);
						if (to_play.size()) {
							int idx = to_play[to_play.size() - 1];
							StringName anim_name = a->animation_track_get_key_animation(i, idx);
							if (anim_name == SNAME("[stop]") || !player2->has_animation(anim_name)) {
								if (playing_caches.has(t)) {
									playing_caches.erase(t);
									player2->stop();
									t->playing = false;
								}
							} else {
								player2->play(anim_name);
								t->playing = true;
								playing_caches.insert(t);
							}
						}
					}
				} break;
			}
		}
	}
	is_GDVIRTUAL_CALL_post_process_key_value = true;
}

void AnimationMixer::_blend_apply() {
	// Finally, set the tracks.
	for (const KeyValue<Animation::TrackCacheID, TrackCache *> &K : track_cache) {
		TrackCache *track = K.value;
		bool is_zero_amount = Math::is_zero_approx(track->total_weight);
		if (!deterministic && is_zero_amount) {
			continue;
		}
		switch (track->type) {
			case Animation::TYPE_POSITION_3D: {
#ifndef _3D_DISABLED
				TrackCacheTransform *t = static_cast<TrackCacheTransform *>(track);

				if (t->root_motion) {
					root_motion_position = root_motion_cache.loc;
					root_motion_rotation = root_motion_cache.rot;
					root_motion_scale = root_motion_cache.scale - Vector3(1, 1, 1);
					root_motion_position_accumulator = t->loc;
					root_motion_rotation_accumulator = t->rot;
					root_motion_scale_accumulator = t->scale;
				} else if (t->skeleton_id.is_valid() && t->bone_idx >= 0) {
					Skeleton3D *t_skeleton = ObjectDB::get_instance<Skeleton3D>(t->skeleton_id);
					if (!t_skeleton) {
						return;
					}

					// TODO: Once https://github.com/godotengine/godot/pull/113441 makes it in
					// Use set_bone_pose_components when loc_used, rot_used, and scale_used are all true.

					if (t->loc_used) {
						t_skeleton->set_bone_pose_position(t->bone_idx, t->loc);
					}
					if (t->rot_used) {
						t_skeleton->set_bone_pose_rotation(t->bone_idx, t->rot);
					}
					if (t->scale_used) {
						t_skeleton->set_bone_pose_scale(t->bone_idx, t->scale);
					}

				} else if (!t->skeleton_id.is_valid()) {
					Node3D *t_node_3d = ObjectDB::get_instance<Node3D>(t->object_id);
					if (!t_node_3d) {
						return;
					}
					if (t->loc_used && t->rot_used && t->scale_used) {
						Transform3D transform = Transform3D(Basis(t->rot).scaled(t->scale), t->loc);
						t_node_3d->set_transform(transform);
					} else {
						if (t->loc_used) {
							t_node_3d->set_position(t->loc);
						}
						if (t->rot_used) {
							t_node_3d->set_rotation(t->rot.get_euler());
						}
						if (t->scale_used) {
							t_node_3d->set_scale(t->scale);
						}
					}
				}
#endif // _3D_DISABLED
			} break;
			case Animation::TYPE_BLEND_SHAPE: {
#ifndef _3D_DISABLED
				TrackCacheBlendShape *t = static_cast<TrackCacheBlendShape *>(track);

				MeshInstance3D *t_mesh_3d = ObjectDB::get_instance<MeshInstance3D>(t->object_id);
				if (t_mesh_3d) {
					t_mesh_3d->set_blend_shape_value(t->shape_index, t->value);
				}
#endif // _3D_DISABLED
			} break;
			case Animation::TYPE_VALUE: {
				TrackCacheValue *t = static_cast<TrackCacheValue *>(track);

				if (callback_mode_discrete == ANIMATION_CALLBACK_MODE_DISCRETE_FORCE_CONTINUOUS) {
					t->is_init = false; // Always update in Force Continuous.
				} else if (!t->use_continuous && (t->use_discrete || !deterministic)) {
					t->is_init = true; // If there is no continuous value and only disctere value is applied or just started, don't RESET.
				}

				if ((t->is_init && (is_zero_amount || !t->use_continuous)) ||
						(callback_mode_discrete != ANIMATION_CALLBACK_MODE_DISCRETE_FORCE_CONTINUOUS &&
								!is_zero_amount &&
								callback_mode_discrete == ANIMATION_CALLBACK_MODE_DISCRETE_DOMINANT &&
								t->use_discrete)) {
					break; // Don't overwrite the value set by UPDATE_DISCRETE.
				}

				if (callback_mode_discrete != ANIMATION_CALLBACK_MODE_DISCRETE_FORCE_CONTINUOUS) {
					t->is_init = !t->use_continuous; // If there is no Continuous in non-Force Continuous type, it means RESET.
				}

				// Trim unused elements if init array/string is not blended.
				if (t->value.is_array()) {
					int actual_blended_size = (int)Math::round(Math::abs(t->element_size.operator real_t()));
					if (actual_blended_size < (t->value.operator Array()).size()) {
						real_t abs_weight = Math::abs(track->total_weight);
						if (abs_weight >= 1.0) {
							(t->value.operator Array()).resize(actual_blended_size);
						} else if (t->init_value.is_string()) {
							(t->value.operator Array()).resize(Animation::interpolate_variant((t->init_value.operator String()).length(), actual_blended_size, abs_weight));
						}
					}
				}

				Object *t_obj = ObjectDB::get_instance(t->object_id);
				if (t_obj) {
					t_obj->set_indexed(t->subpath, Animation::cast_from_blendwise(t->value, t->init_value.get_type()));
				}

			} break;
			case Animation::TYPE_AUDIO: {
				TrackCacheAudio *t = static_cast<TrackCacheAudio *>(track);
				Object *t_obj = ObjectDB::get_instance(t->object_id);
				if (!t_obj) {
					t->playing_streams.clear();
					break;
				}

				// Audio ending process.
				LocalVector<Pair<ObjectID, int>> erase_maps;
				for (KeyValue<Pair<ObjectID, int>, PlayingAudioTrackInfo> &L : t->playing_streams) {
					PlayingAudioTrackInfo &track_info = L.value;
					float db = Math::linear_to_db(track_info.use_blend ? track_info.volume : 1.0);
					LocalVector<uint64_t> erase_streams;
					AHashMap<uint64_t, PlayingAudioStreamInfo> &map = track_info.stream_info;
					for (const KeyValue<uint64_t, PlayingAudioStreamInfo> &M : map) {
						PlayingAudioStreamInfo pasi = M.value;

						bool stop = false;
						if (!t_obj->call(SNAME("is_layered_playing"), pasi.index)) {
							stop = true;
						}
						if (!track_info.loop) {
							if (!track_info.backward) {
								if (Animation::is_less_approx(track_info.time, pasi.start)) {
									stop = true;
								}
							} else if (track_info.backward) {
								if (Animation::is_greater_approx(track_info.time, pasi.start)) {
									stop = true;
								}
							}
						}
						if (Animation::is_greater_approx(pasi.len, 0)) {
							double len = 0.0;
							if (!track_info.backward) {
								len = Animation::is_greater_approx(pasi.start, track_info.time) ? (track_info.length - pasi.start) + track_info.time : track_info.time - pasi.start;
							} else {
								len = Animation::is_less_approx(pasi.start, track_info.time) ? (track_info.length - track_info.time) + pasi.start : pasi.start - track_info.time;
							}
							if (Animation::is_greater_approx(len, pasi.len)) {
								stop = true;
							}
						}
						if (stop) {
							// Time to stop.
							_stop_audio_sound(t_obj, pasi.index);
							erase_streams.push_back(M.key);
						} else {
							t_obj->call(SNAME("set_layered_volume_db"), pasi.index, pasi.volume_db + db);
						}
					}
					for (uint32_t erase_idx = 0; erase_idx < erase_streams.size(); erase_idx++) {
						map.erase(erase_streams[erase_idx]);
					}

					// Sounds under the playhead follow the blend too, and drop out once they end by
					// themselves (they do after the playhead has been still a moment).
					LocalVector<int> ended_scrubs;
					for (const KeyValue<int, ScrubbedAudioInfo> &S : track_info.scrubbed) {
						if (t_obj->call(SNAME("is_layered_playing"), S.value.index)) {
							t_obj->call(SNAME("set_layered_volume_db"), S.value.index, S.value.volume_db + db);
						} else {
							_stop_audio_sound(t_obj, S.value.index);
							ended_scrubs.push_back(S.key);
						}
					}
					for (const int k : ended_scrubs) {
						track_info.scrubbed.erase(k);
					}

					if (map.is_empty() && track_info.scrubbed.is_empty()) {
						erase_maps.push_back(L.key);
					}
				}
				for (uint32_t erase_idx = 0; erase_idx < erase_maps.size(); erase_idx++) {
					t->playing_streams.erase(erase_maps[erase_idx]);
				}
			} break;
			default: {
			} // The rest don't matter.
		}
	}
}

void AnimationMixer::_transmit(const Variant &p_channel, const Array &p_arguments, bool p_deferred) {
	if (p_deferred) {
		callable_mp(GlobalTransmit::get_singleton(), &GlobalTransmit::transmit_array).call_deferred(p_channel, p_arguments);
	} else {
		GlobalTransmit::get_singleton()->transmit_array(p_channel, p_arguments);
	}
}

void AnimationMixer::_call_object(ObjectID p_object_id, const StringName &p_method, const Vector<Variant> &p_params, bool p_deferred) {
	// Separate function to use alloca() more efficiently
	const Variant **argptrs = (const Variant **)alloca(sizeof(Variant *) * p_params.size());
	const Variant *args = p_params.ptr();
	uint32_t argcount = p_params.size();
	for (uint32_t i = 0; i < argcount; i++) {
		argptrs[i] = &args[i];
	}
	Object *t_obj = ObjectDB::get_instance(p_object_id);
	if (!t_obj) {
		return;
	}
	if (p_deferred) {
		Callable(t_obj, p_method).call_deferredp(argptrs, argcount);
	} else {
		Callable::CallError ce;
		t_obj->callp(p_method, argptrs, argcount, ce);
	}
}

void AnimationMixer::make_animation_instance(const StringName &p_name, const PlaybackInfo &p_playback_info) {
	const Ref<Animation> &animation = get_animation_or_null(p_name);
	ERR_FAIL_COND(animation.is_null());

	AnimationInstance ai;
	ai.animation = animation;
	ai.playback_info = p_playback_info;

	animation_instances.push_back(std::move(ai));
}

void AnimationMixer::clear_animation_instances() {
	animation_instances.clear();
}

void AnimationMixer::advance(double p_time) {
	_process_animation(p_time);
}

void AnimationMixer::clear_caches() {
	_clear_caches();
}

/* -------------------------------------------- */
/* -- Root motion ----------------------------- */
/* -------------------------------------------- */

void AnimationMixer::set_root_motion_track(const NodePath &p_track) {
	root_motion_track = p_track;
	notify_property_list_changed();
}

NodePath AnimationMixer::get_root_motion_track() const {
	return root_motion_track;
}

void AnimationMixer::set_root_motion_local(bool p_enabled) {
	root_motion_local = p_enabled;
}

bool AnimationMixer::is_root_motion_local() const {
	return root_motion_local;
}

Vector3 AnimationMixer::get_root_motion_position() const {
	return root_motion_position;
}

Quaternion AnimationMixer::get_root_motion_rotation() const {
	return root_motion_rotation;
}

Vector3 AnimationMixer::get_root_motion_scale() const {
	return root_motion_scale;
}

Vector3 AnimationMixer::get_root_motion_position_accumulator() const {
	return root_motion_position_accumulator;
}

Quaternion AnimationMixer::get_root_motion_rotation_accumulator() const {
	return root_motion_rotation_accumulator;
}

Vector3 AnimationMixer::get_root_motion_scale_accumulator() const {
	return root_motion_scale_accumulator;
}

/* -------------------------------------------- */
/* -- Reset on save --------------------------- */
/* -------------------------------------------- */

void AnimationMixer::set_reset_on_save_enabled(bool p_enabled) {
	reset_on_save = p_enabled;
}

bool AnimationMixer::is_reset_on_save_enabled() const {
	return reset_on_save;
}

bool AnimationMixer::can_apply_reset() const {
	return has_animation(SceneStringName(RESET));
}

void AnimationMixer::_build_backup_track_cache() {
	for (const KeyValue<Animation::TrackCacheID, TrackCache *> &K : track_cache) {
		TrackCache *track = K.value;
		track->total_weight = 1.0;
		switch (track->type) {
			case Animation::TYPE_POSITION_3D: {
#ifndef _3D_DISABLED
				TrackCacheTransform *t = static_cast<TrackCacheTransform *>(track);
				if (t->root_motion) {
					// Do nothing.
				} else if (t->skeleton_id.is_valid() && t->bone_idx >= 0) {
					Skeleton3D *t_skeleton = ObjectDB::get_instance<Skeleton3D>(t->skeleton_id);
					if (!t_skeleton) {
						return;
					}
					if (t->loc_used) {
						t->loc = t_skeleton->get_bone_pose_position(t->bone_idx);
					}
					if (t->rot_used) {
						t->rot = t_skeleton->get_bone_pose_rotation(t->bone_idx);
					}
					if (t->scale_used) {
						t->scale = t_skeleton->get_bone_pose_scale(t->bone_idx);
					}
				} else if (!t->skeleton_id.is_valid()) {
					Node3D *t_node_3d = ObjectDB::get_instance<Node3D>(t->object_id);
					if (!t_node_3d) {
						return;
					}
					if (t->loc_used) {
						t->loc = t_node_3d->get_position();
					}
					if (t->rot_used) {
						t->rot = t_node_3d->get_quaternion();
					}
					if (t->scale_used) {
						t->scale = t_node_3d->get_scale();
					}
				}
#endif // _3D_DISABLED
			} break;
			case Animation::TYPE_BLEND_SHAPE: {
#ifndef _3D_DISABLED
				TrackCacheBlendShape *t = static_cast<TrackCacheBlendShape *>(track);
				MeshInstance3D *t_mesh_3d = ObjectDB::get_instance<MeshInstance3D>(t->object_id);
				if (t_mesh_3d) {
					t->value = t_mesh_3d->get_blend_shape_value(t->shape_index);
				}
#endif // _3D_DISABLED
			} break;
			case Animation::TYPE_VALUE: {
				TrackCacheValue *t = static_cast<TrackCacheValue *>(track);
				Object *t_obj = ObjectDB::get_instance(t->object_id);
				if (t_obj) {
					t->value = Animation::cast_to_blendwise(t_obj->get_indexed(t->subpath));
				}
				t->use_continuous = true;
				t->use_discrete = false;
				if (t->init_value.is_array()) {
					t->element_size = MAX(t->element_size.operator int(), (t->value.operator Array()).size());
				} else if (t->init_value.is_string()) {
					t->element_size = (real_t)(t->value.operator Array()).size();
				}
			} break;
			// Audio tracks play layered sounds and never take over the player's own stream, so
			// there's nothing of theirs to clear from a backup.
			default: {
			} // The rest don't matter.
		}
	}
}

Ref<AnimatedValuesBackup> AnimationMixer::make_backup() {
	Ref<AnimatedValuesBackup> backup;
	backup.instantiate();

	Ref<Animation> reset_anim = animation_set[SceneStringName(RESET)].animation;
	ERR_FAIL_COND_V(reset_anim.is_null(), Ref<AnimatedValuesBackup>());

	_blend_init();
	PlaybackInfo pi;
	pi.time = 0;
	pi.delta = 0;
	pi.start = 0;
	pi.end = reset_anim->get_length();
	pi.seeked = true;
	pi.weight = 1.0;
	make_animation_instance(SceneStringName(RESET), pi);
	_build_backup_track_cache();

	backup->set_data(AHashMap<Animation::TrackCacheID, TrackCache *, HashHasher>(track_cache));
	clear_animation_instances();

	return backup;
}

void AnimationMixer::reset() {
	ERR_FAIL_COND(!can_apply_reset());

	Ref<Animation> reset_anim = animation_set[SceneStringName(RESET)].animation;
	ERR_FAIL_COND(reset_anim.is_null());

	Node *root_node_object = get_node_or_null(root_node);
	ERR_FAIL_NULL(root_node_object);

	AnimationPlayer *aux_player = memnew(AnimationPlayer);
	root_node_object->add_child(aux_player);
	Ref<AnimationLibrary> al;
	al.instantiate();
	al->add_animation(SceneStringName(RESET), reset_anim);
	aux_player->set_reset_on_save_enabled(false);
	aux_player->set_root_node(aux_player->get_path_to(root_node_object));
	aux_player->add_animation_library("", al);
	aux_player->set_assigned_animation(SceneStringName(RESET));
	aux_player->seek(0.0f, true);
	aux_player->queue_free();
}

void AnimationMixer::restore(const Ref<AnimatedValuesBackup> &p_backup) {
	ERR_FAIL_COND(p_backup.is_null());
	track_cache = p_backup->get_data();
	_blend_apply();
	track_cache = AHashMap<Animation::TrackCacheID, AnimationMixer::TrackCache *, HashHasher>();
	cache_valid = false;
}

#ifdef TOOLS_ENABLED
Ref<AnimatedValuesBackup> AnimationMixer::apply_reset(bool p_user_initiated) {
	if (!p_user_initiated && dummy) {
		return Ref<AnimatedValuesBackup>();
	}
	ERR_FAIL_COND_V(!can_apply_reset(), Ref<AnimatedValuesBackup>());

	Ref<Animation> reset_anim = animation_set[SceneStringName(RESET)].animation;
	ERR_FAIL_COND_V(reset_anim.is_null(), Ref<AnimatedValuesBackup>());

	Ref<AnimatedValuesBackup> backup_current = make_backup();
	if (p_user_initiated) {
		EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
		ur->create_action(TTR("Animation Apply Reset"));
		ur->add_do_method(this, "_reset");
		ur->add_undo_method(this, "_restore", backup_current);
		ur->commit_action();
	} else {
		reset();
	}

	return backup_current;
}
#endif // TOOLS_ENABLED

/* -------------------------------------------- */
/* -- Capture feature ------------------------- */
/* -------------------------------------------- */

void AnimationMixer::capture(const StringName &p_name, double p_duration, Tween::TransitionType p_trans_type, Tween::EaseType p_ease_type) {
	ERR_FAIL_COND(!active);
	const Ref<Animation> &reference_animation = get_animation_or_null(p_name);
	ERR_FAIL_COND(reference_animation.is_null());
	ERR_FAIL_COND(p_duration <= 0);

	if (!cache_valid) {
		_update_caches(); // Need to retrieve object id.
	}

	capture_cache.remain = 1.0;
	capture_cache.step = 1.0 / p_duration;
	capture_cache.trans_type = p_trans_type;
	capture_cache.ease_type = p_ease_type;
	if (capture_cache.animation.is_valid()) {
		animation_track_num_to_track_cache.erase(capture_cache.animation);
	}
	capture_cache.animation.instantiate();

	bool is_valid = false;
	for (int i = 0; i < reference_animation->get_track_count(); i++) {
		if (!reference_animation->track_is_enabled(i)) {
			continue;
		}
		if (reference_animation->track_get_type(i) == Animation::TYPE_VALUE && reference_animation->value_track_get_update_mode(i) == Animation::UPDATE_CAPTURE) {
			TrackCacheValue *t = static_cast<TrackCacheValue *>(track_cache[reference_animation->track_get_unique_id(i)]);
			Object *t_obj = ObjectDB::get_instance(t->object_id);
			if (t_obj) {
				Variant value = t_obj->get_indexed(t->subpath);
				int inserted_idx = capture_cache.animation->add_track(Animation::TYPE_VALUE);
				capture_cache.animation->track_set_path(inserted_idx, reference_animation->track_get_path(i));
				capture_cache.animation->track_insert_key(inserted_idx, 0, value);
				capture_cache.animation->value_track_set_update_mode(inserted_idx, Animation::UPDATE_CONTINUOUS);
				capture_cache.animation->track_set_interpolation_type(inserted_idx, Animation::INTERPOLATION_LINEAR);
				is_valid = true;
			}
		}
	}
	if (!is_valid) {
		capture_cache.clear();
	} else {
		_create_track_num_to_track_cache_for_animation(capture_cache.animation);
	}
}

/* -------------------------------------------- */
/* -- General functions ----------------------- */
/* -------------------------------------------- */

void AnimationMixer::_node_removed(Node *p_node) {
	_clear_caches();
}

void AnimationMixer::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (!processing) {
				set_physics_process_internal(false);
				set_process_internal(false);
			}
			_clear_caches();
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			if (active && callback_mode_process == ANIMATION_CALLBACK_MODE_PROCESS_IDLE) {
				_process_animation(get_process_delta_time());
			}
		} break;

		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			if (active && callback_mode_process == ANIMATION_CALLBACK_MODE_PROCESS_PHYSICS) {
				_process_animation(get_physics_process_delta_time());
			}
		} break;

		case NOTIFICATION_EXIT_TREE: {
			_clear_caches();
		} break;
	}
}

#ifdef TOOLS_ENABLED
void AnimationMixer::get_argument_options(const StringName &p_function, int p_idx, List<String> *r_options) const {
	const String pf = p_function;
	if (p_idx == 0) {
		if (pf == "get_animation" || pf == "has_animation") {
			for (const StringName &name : get_sorted_animation_list()) {
				r_options->push_back(String(name).quote());
			}
		} else if (pf == "get_animation_library" || pf == "has_animation_library" || pf == "remove_animation_library" || pf == "rename_animation_library") {
			LocalVector<StringName> al;
			get_animation_library_list(&al);
			for (const StringName &name : al) {
				r_options->push_back(String(name).quote());
			}
		}
	}
	Node::get_argument_options(p_function, p_idx, r_options);
}
#endif

void AnimationMixer::_bind_methods() {
	/* ---- Data lists ---- */
	ClassDB::bind_method(D_METHOD("add_animation_library", "name", "library"), &AnimationMixer::add_animation_library);
	ClassDB::bind_method(D_METHOD("remove_animation_library", "name"), &AnimationMixer::remove_animation_library);
	ClassDB::bind_method(D_METHOD("rename_animation_library", "name", "newname"), &AnimationMixer::rename_animation_library);
	ClassDB::bind_method(D_METHOD("has_animation_library", "name"), &AnimationMixer::has_animation_library);
	ClassDB::bind_method(D_METHOD("get_animation_library", "name"), &AnimationMixer::get_animation_library);
	ClassDB::bind_method(D_METHOD("get_animation_library_list"), &AnimationMixer::_get_animation_library_list);

	ClassDB::bind_method(D_METHOD("has_animation", "name"), &AnimationMixer::has_animation);
	ClassDB::bind_method(D_METHOD("get_animation", "name"), &AnimationMixer::get_animation);
	ClassDB::bind_method(D_METHOD("get_animation_list"), &AnimationMixer::_get_animation_list);

	/* ---- General settings for animation ---- */
	ClassDB::bind_method(D_METHOD("set_active", "active"), &AnimationMixer::set_active);
	ClassDB::bind_method(D_METHOD("is_active"), &AnimationMixer::is_active);

	ClassDB::bind_method(D_METHOD("set_deterministic", "deterministic"), &AnimationMixer::set_deterministic);
	ClassDB::bind_method(D_METHOD("is_deterministic"), &AnimationMixer::is_deterministic);

	ClassDB::bind_method(D_METHOD("set_root_node", "path"), &AnimationMixer::set_root_node);
	ClassDB::bind_method(D_METHOD("get_root_node"), &AnimationMixer::get_root_node);

	ClassDB::bind_method(D_METHOD("set_callback_mode_process", "mode"), &AnimationMixer::set_callback_mode_process);
	ClassDB::bind_method(D_METHOD("get_callback_mode_process"), &AnimationMixer::get_callback_mode_process);

	ClassDB::bind_method(D_METHOD("set_callback_mode_method", "mode"), &AnimationMixer::set_callback_mode_method);
	ClassDB::bind_method(D_METHOD("get_callback_mode_method"), &AnimationMixer::get_callback_mode_method);

	ClassDB::bind_method(D_METHOD("set_callback_mode_discrete", "mode"), &AnimationMixer::set_callback_mode_discrete);
	ClassDB::bind_method(D_METHOD("get_callback_mode_discrete"), &AnimationMixer::get_callback_mode_discrete);

	/* ---- Audio ---- */
	// Hidden: the editor sets it around timeline drags; tests and tools can do the same.
	ClassDB::bind_method(D_METHOD("_set_audio_scrubbing", "scrubbing"), &AnimationMixer::set_audio_scrubbing);
	ClassDB::bind_method(D_METHOD("set_audio_max_polyphony", "max_polyphony"), &AnimationMixer::set_audio_max_polyphony);
	ClassDB::bind_method(D_METHOD("get_audio_max_polyphony"), &AnimationMixer::get_audio_max_polyphony);

	/* ---- Root motion accumulator for Skeleton3D ---- */
	ClassDB::bind_method(D_METHOD("set_root_motion_track", "path"), &AnimationMixer::set_root_motion_track);
	ClassDB::bind_method(D_METHOD("get_root_motion_track"), &AnimationMixer::get_root_motion_track);
	ClassDB::bind_method(D_METHOD("set_root_motion_local", "enabled"), &AnimationMixer::set_root_motion_local);
	ClassDB::bind_method(D_METHOD("is_root_motion_local"), &AnimationMixer::is_root_motion_local);

	ClassDB::bind_method(D_METHOD("get_root_motion_position"), &AnimationMixer::get_root_motion_position);
	ClassDB::bind_method(D_METHOD("get_root_motion_rotation"), &AnimationMixer::get_root_motion_rotation);
	ClassDB::bind_method(D_METHOD("get_root_motion_scale"), &AnimationMixer::get_root_motion_scale);
	ClassDB::bind_method(D_METHOD("get_root_motion_position_accumulator"), &AnimationMixer::get_root_motion_position_accumulator);
	ClassDB::bind_method(D_METHOD("get_root_motion_rotation_accumulator"), &AnimationMixer::get_root_motion_rotation_accumulator);
	ClassDB::bind_method(D_METHOD("get_root_motion_scale_accumulator"), &AnimationMixer::get_root_motion_scale_accumulator);

	/* ---- Blending processor ---- */
	ClassDB::bind_method(D_METHOD("clear_caches"), &AnimationMixer::clear_caches);
	ClassDB::bind_method(D_METHOD("advance", "delta"), &AnimationMixer::advance);
	GDVIRTUAL_BIND(_post_process_key_value, "animation", "track", "value", "object_id", "object_sub_idx");

	/* ---- Capture feature ---- */
	ClassDB::bind_method(D_METHOD("capture", "name", "duration", "trans_type", "ease_type"), &AnimationMixer::capture, DEFVAL(Tween::TRANS_LINEAR), DEFVAL(Tween::EASE_IN));

	/* ---- Reset on save ---- */
	ClassDB::bind_method(D_METHOD("set_reset_on_save_enabled", "enabled"), &AnimationMixer::set_reset_on_save_enabled);
	ClassDB::bind_method(D_METHOD("is_reset_on_save_enabled"), &AnimationMixer::is_reset_on_save_enabled);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "active"), "set_active", "is_active");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "deterministic"), "set_deterministic", "is_deterministic");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "reset_on_save", PROPERTY_HINT_NONE, ""), "set_reset_on_save_enabled", "is_reset_on_save_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "root_node"), "set_root_node", "get_root_node");

	ADD_GROUP("Root Motion", "root_motion_");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "root_motion_track"), "set_root_motion_track", "get_root_motion_track");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "root_motion_local"), "set_root_motion_local", "is_root_motion_local");

	ADD_GROUP("Audio", "audio_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "audio_max_polyphony", PROPERTY_HINT_RANGE, "1,127,1"), "set_audio_max_polyphony", "get_audio_max_polyphony");

	ADD_GROUP("Callback Mode", "callback_mode_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "callback_mode_process", PROPERTY_HINT_ENUM, "Physics,Idle,Manual"), "set_callback_mode_process", "get_callback_mode_process");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "callback_mode_method", PROPERTY_HINT_ENUM, "Deferred,Immediate"), "set_callback_mode_method", "get_callback_mode_method");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "callback_mode_discrete", PROPERTY_HINT_ENUM, "Dominant,Recessive,Force Continuous"), "set_callback_mode_discrete", "get_callback_mode_discrete");

	BIND_ENUM_CONSTANT(ANIMATION_CALLBACK_MODE_PROCESS_PHYSICS);
	BIND_ENUM_CONSTANT(ANIMATION_CALLBACK_MODE_PROCESS_IDLE);
	BIND_ENUM_CONSTANT(ANIMATION_CALLBACK_MODE_PROCESS_MANUAL);

	BIND_ENUM_CONSTANT(ANIMATION_CALLBACK_MODE_METHOD_DEFERRED);
	BIND_ENUM_CONSTANT(ANIMATION_CALLBACK_MODE_METHOD_IMMEDIATE);

	BIND_ENUM_CONSTANT(ANIMATION_CALLBACK_MODE_DISCRETE_DOMINANT);
	BIND_ENUM_CONSTANT(ANIMATION_CALLBACK_MODE_DISCRETE_RECESSIVE);
	BIND_ENUM_CONSTANT(ANIMATION_CALLBACK_MODE_DISCRETE_FORCE_CONTINUOUS);

	ADD_SIGNAL(MethodInfo("animation_list_changed"));
	ADD_SIGNAL(MethodInfo("animation_libraries_updated"));
	ADD_SIGNAL(MethodInfo("animation_finished", PropertyInfo(Variant::STRING_NAME, "anim_name")));
	ADD_SIGNAL(MethodInfo("animation_started", PropertyInfo(Variant::STRING_NAME, "anim_name")));
	ADD_SIGNAL(MethodInfo("caches_cleared"));
	ADD_SIGNAL(MethodInfo("mixer_applied"));
	ADD_SIGNAL(MethodInfo("mixer_updated")); // For updating dummy player.

	ClassDB::bind_method(D_METHOD("_reset"), &AnimationMixer::reset);
	ClassDB::bind_method(D_METHOD("_restore", "backup"), &AnimationMixer::restore);
}

AnimationMixer::AnimationMixer() {
	root_node = NodePath("..");
}

AnimationMixer::~AnimationMixer() {
}

void AnimatedValuesBackup::set_data(const AHashMap<Animation::TrackCacheID, AnimationMixer::TrackCache *, HashHasher> &p_data) {
	clear_data();

	for (const KeyValue<Animation::TrackCacheID, AnimationMixer::TrackCache *> &E : p_data) {
		AnimationMixer::TrackCache *track = get_cache_copy(E.value);
		if (!track) {
			continue; // Some types of tracks do not get a copy and must be ignored.
		}

		data.insert(E.key, track);
	}
}

AHashMap<Animation::TrackCacheID, AnimationMixer::TrackCache *, HashHasher> AnimatedValuesBackup::get_data() const {
	AHashMap<Animation::TrackCacheID, AnimationMixer::TrackCache *, HashHasher> ret;
	for (const KeyValue<Animation::TrackCacheID, AnimationMixer::TrackCache *> &E : data) {
		AnimationMixer::TrackCache *track = get_cache_copy(E.value);
		ERR_CONTINUE(!track); // Backup shouldn't contain tracks that cannot be copied, this is a mistake.

		ret.insert(E.key, track);
	}
	return ret;
}

void AnimatedValuesBackup::clear_data() {
	for (KeyValue<Animation::TrackCacheID, AnimationMixer::TrackCache *> &K : data) {
		memdelete(K.value);
	}
	data.clear();
}

AnimationMixer::TrackCache *AnimatedValuesBackup::get_cache_copy(AnimationMixer::TrackCache *p_cache) const {
	switch (p_cache->type) {
		case Animation::TYPE_BEZIER:
		case Animation::TYPE_VALUE: {
			AnimationMixer::TrackCacheValue *src = static_cast<AnimationMixer::TrackCacheValue *>(p_cache);
			AnimationMixer::TrackCacheValue *tc = memnew(AnimationMixer::TrackCacheValue(*src));
			return tc;
		}

		case Animation::TYPE_POSITION_3D:
		case Animation::TYPE_ROTATION_3D:
		case Animation::TYPE_SCALE_3D: {
			AnimationMixer::TrackCacheTransform *src = static_cast<AnimationMixer::TrackCacheTransform *>(p_cache);
			AnimationMixer::TrackCacheTransform *tc = memnew(AnimationMixer::TrackCacheTransform(*src));
			return tc;
		}

		case Animation::TYPE_BLEND_SHAPE: {
			AnimationMixer::TrackCacheBlendShape *src = static_cast<AnimationMixer::TrackCacheBlendShape *>(p_cache);
			AnimationMixer::TrackCacheBlendShape *tc = memnew(AnimationMixer::TrackCacheBlendShape(*src));
			return tc;
		}

		case Animation::TYPE_AUDIO: {
			AnimationMixer::TrackCacheAudio *src = static_cast<AnimationMixer::TrackCacheAudio *>(p_cache);
			AnimationMixer::TrackCacheAudio *tc = memnew(AnimationMixer::TrackCacheAudio(*src));
			return tc;
		}

		case Animation::TYPE_TRANSMIT: {
			AnimationMixer::TrackCacheTransmit *src = static_cast<AnimationMixer::TrackCacheTransmit *>(p_cache);
			return memnew(AnimationMixer::TrackCacheTransmit(*src));
		}

		case Animation::TYPE_METHOD:
		case Animation::TYPE_ANIMATION: {
			// Nothing to do here.
		} break;
	}
	return nullptr;
}
