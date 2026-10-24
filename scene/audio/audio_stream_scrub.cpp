/**************************************************************************/
/*  audio_stream_scrub.cpp                                                */
/**************************************************************************/
/*  Asosdot: audio that follows a moving position, like a tape under a    */
/*  playhead, for scrubbing and playing animations backwards.             */
/**************************************************************************/

#include "audio_stream_scrub.h"

#include "core/os/os.h"
#include "servers/audio/audio_server.h"

Mutex AudioStreamScrub::cache_mutex;
HashMap<ObjectID, Ref<AudioScrubData>> AudioStreamScrub::cache;
uint64_t AudioStreamScrub::use_counter = 0;

AudioFrame AudioScrubData::sample(double p_seconds) const {
	const double at = p_seconds * mix_rate;
	if (at < 0.0 || frames.size() < 2) {
		return AudioFrame(0, 0);
	}
	const int64_t index = int64_t(at);
	if (index >= int64_t(frames.size()) - 1) {
		return AudioFrame(0, 0);
	}
	const float blend = float(at - index);
	return frames[index] * (1.0f - blend) + frames[index + 1] * blend;
}

Ref<AudioScrubData> AudioStreamScrub::_get_data(const Ref<AudioStream> &p_source) {
	const ObjectID id = p_source->get_instance_id();
	{
		MutexLock lock(cache_mutex);
		if (Ref<AudioScrubData> *cached = cache.getptr(id)) {
			(*cached)->last_used = ++use_counter;
			return *cached;
		}
	}

	const double length = MIN(p_source->get_length(), MAX_SECONDS);
	if (length <= 0.0) {
		return Ref<AudioScrubData>();
	}
	Ref<AudioStreamPlayback> playback = p_source->instantiate_playback();
	if (playback.is_null()) {
		return Ref<AudioScrubData>();
	}

	// A playback mixed at rate 1 produces frames at the server's mix rate, as for the waveform
	// previews in the editor.
	Ref<AudioScrubData> data;
	data.instantiate();
	data->mix_rate = AudioServer::get_singleton()->get_mix_rate();
	const int64_t total = int64_t(length * data->mix_rate);
	data->frames.resize(total);
	playback->start(0.0);
	int64_t done = 0;
	while (done < total) {
		const int chunk = int(MIN(int64_t(4096), total - done));
		const int mixed = playback->mix(data->frames.ptr() + done, 1.0, chunk);
		done += MAX(mixed, 0);
		if (mixed < chunk) {
			break;
		}
	}
	playback->stop();
	data->frames.resize(done);

	MutexLock lock(cache_mutex);
	data->last_used = ++use_counter;
	cache.insert(id, data);
	// Forget streams that no longer exist, then the least recently scrubbed, until it fits.
	uint64_t cached_frames = 0;
	LocalVector<ObjectID> gone;
	for (const KeyValue<ObjectID, Ref<AudioScrubData>> &E : cache) {
		if (ObjectDB::get_instance(E.key) == nullptr) {
			gone.push_back(E.key);
		} else {
			cached_frames += E.value->frames.size();
		}
	}
	for (const ObjectID &key : gone) {
		cache.erase(key);
	}
	while (cached_frames > MAX_CACHED_FRAMES && cache.size() > 1) {
		ObjectID oldest;
		uint64_t oldest_use = UINT64_MAX;
		for (const KeyValue<ObjectID, Ref<AudioScrubData>> &E : cache) {
			if (E.key != id && E.value->last_used < oldest_use) {
				oldest_use = E.value->last_used;
				oldest = E.key;
			}
		}
		cached_frames -= cache[oldest]->frames.size();
		cache.erase(oldest);
	}
	return data;
}

Ref<AudioStreamScrub> AudioStreamScrub::create(const Ref<AudioStream> &p_source) {
	ERR_FAIL_COND_V(p_source.is_null(), Ref<AudioStreamScrub>());
	Ref<AudioScrubData> data = _get_data(p_source);
	if (data.is_null() || data->frames.is_empty()) {
		return Ref<AudioStreamScrub>();
	}
	Ref<AudioStreamScrub> scrub;
	scrub.instantiate();
	scrub->data = data;
	return scrub;
}

void AudioStreamScrub::clear_cache() {
	MutexLock lock(cache_mutex);
	cache.clear();
}

void AudioStreamScrub::set_target(double p_seconds) {
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	const uint64_t then = target_usec.load(std::memory_order_relaxed);
	const double previous = target.load(std::memory_order_relaxed);
	if (then != 0 && now <= then) {
		target.store(p_seconds, std::memory_order_relaxed);
		return; // Set twice within a microsecond: no time to measure a speed over.
	}
	double speed = 0.0;
	if (then != 0) {
		const double elapsed = (now - then) / 1000000.0;
		const double moved = p_seconds - previous;
		if (elapsed < AudioStreamPlaybackScrub::STILL_AFTER && Math::abs(moved) < 0.5) {
			// Smoothed, as mouse-driven playheads move in uneven steps.
			speed = Math::lerp(target_speed.load(std::memory_order_relaxed), moved / elapsed, 0.35);
		}
	}
	target_speed.store(speed, std::memory_order_relaxed);
	target.store(p_seconds, std::memory_order_relaxed);
	target_usec.store(now, std::memory_order_relaxed);
}

void AudioStreamScrub::get_target(double &r_target, double &r_speed, uint64_t &r_set_usec) const {
	r_set_usec = target_usec.load(std::memory_order_relaxed);
	r_target = target.load(std::memory_order_relaxed);
	r_speed = target_speed.load(std::memory_order_relaxed);
}

Ref<AudioStreamPlayback> AudioStreamScrub::instantiate_playback() {
	Ref<AudioStreamPlaybackScrub> playback;
	playback.instantiate();
	playback->scrub = Ref<AudioStreamScrub>(this);
	playback->data = data;
	return playback;
}

void AudioStreamPlaybackScrub::start(double p_from_pos) {
	position = p_from_pos;
	velocity = 0.0;
	still_time = 0.0;
	active = true;
}

void AudioStreamPlaybackScrub::stop() {
	active = false;
}

bool AudioStreamPlaybackScrub::is_playing() const {
	return active;
}

int AudioStreamPlaybackScrub::mix(AudioFrame *p_buffer, float p_rate_scale, int p_frames) {
	// The rate scale is ignored: the target already moves at the speed the sound should play.
	if (!active || data.is_null()) {
		return 0;
	}
	const double rate = data->mix_rate;
	const double block = p_frames / rate;
	double target = 0.0;
	double target_speed = 0.0;
	uint64_t set_usec = 0;
	scrub->get_target(target, target_speed, set_usec);
	if (set_usec != followed_target_usec) {
		followed_target_usec = set_usec;
		since_target = 0.0;
	}
	// Carried on at its speed while it's moving; held once it has been still a moment.
	double target_now = target;
	if (since_target < STILL_AFTER) {
		target_now += target_speed * since_target;
	} else {
		target_speed = 0.0;
	}
	since_target += block;
	double error = target_now - position;
	if (Math::abs(error) > JUMP) {
		position = target_now;
		velocity = target_speed;
		error = 0.0;
	}

	const double wanted = CLAMP(target_speed + error / FOLLOW, -MAX_SPEED, MAX_SPEED);
	const double from_velocity = velocity;
	velocity += (wanted - velocity) * MIN(1.0, block / EASE);
	for (int i = 0; i < p_frames; i++) {
		const double speed = from_velocity + (velocity - from_velocity) * double(i + 1) / p_frames;
		position += speed / rate;
		const float gain = float(CLAMP(Math::abs(speed) / FADE_SPEED, 0.0, 1.0));
		p_buffer[i] = data->sample(position) * gain;
	}
	position = CLAMP(position, 0.0, data->get_length());

	if (Math::abs(velocity) < 0.02 && Math::abs(error) < 0.005) {
		still_time += block;
		if (still_time > IDLE_END) {
			active = false;
			return 0;
		}
	} else {
		still_time = 0.0;
	}
	return p_frames;
}
