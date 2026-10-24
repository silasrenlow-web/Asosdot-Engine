/**************************************************************************/
/*  audio_stream_scrub.h                                                  */
/**************************************************************************/
/*  Asosdot: audio that follows a moving position, like a tape under a    */
/*  playhead, for scrubbing and playing animations backwards.             */
/**************************************************************************/

#pragma once

#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "servers/audio/audio_stream.h"

#include <atomic>

// A stream decoded to frames at the audio server's mix rate, shared by every scrub of it.
class AudioScrubData : public RefCounted {
	GDCLASS(AudioScrubData, RefCounted);

public:
	LocalVector<AudioFrame> frames;
	double mix_rate = 44100.0;
	uint64_t last_used = 0;

	AudioFrame sample(double p_seconds) const;
	double get_length() const { return frames.is_empty() ? 0.0 : frames.size() / mix_rate; }
};

class AudioStreamScrub : public AudioStream {
	GDCLASS(AudioStreamScrub, AudioStream);

	friend class AudioStreamPlaybackScrub;

	// Longest stream decoded for scrubbing, and how much decoded audio is kept around.
	static constexpr double MAX_SECONDS = 600.0;
	static constexpr uint64_t MAX_CACHED_FRAMES = 64 * 1024 * 1024; // 512 MiB of stereo floats.

	static Mutex cache_mutex;
	static HashMap<ObjectID, Ref<AudioScrubData>> cache;
	static uint64_t use_counter;

	Ref<AudioScrubData> data;
	// The playhead as last set: where, how fast it was moving (seconds per second), and when.
	std::atomic<double> target{ 0.0 };
	std::atomic<double> target_speed{ 0.0 };
	std::atomic<uint64_t> target_usec{ 0 };

	static Ref<AudioScrubData> _get_data(const Ref<AudioStream> &p_source);

protected:
	static void _bind_methods() {}

public:
	// Null when the source can't be decoded ahead, such as a stream with no set length.
	static Ref<AudioStreamScrub> create(const Ref<AudioStream> &p_source);
	static void clear_cache();

	// Where in the source the sound should be, in seconds. The playback moves with it at the speed
	// it moves, forwards or backwards, and falls silent once it stops moving.
	void set_target(double p_seconds);
	// The target as last set, how fast it was moving, and when it was set (so a playback can tell a
	// new target from one it has already been following).
	void get_target(double &r_target, double &r_speed, uint64_t &r_set_usec) const;

	virtual Ref<AudioStreamPlayback> instantiate_playback() override;
	virtual String get_stream_name() const override { return "Scrub"; }
	virtual double get_length() const override { return data.is_valid() ? data->get_length() : 0.0; }
	virtual bool is_monophonic() const override { return false; }
};

class AudioStreamPlaybackScrub : public AudioStreamPlayback {
	GDCLASS(AudioStreamPlaybackScrub, AudioStreamPlayback);

	friend class AudioStreamScrub;

	// It moves at the target's speed and makes up any gap over FOLLOW seconds, never faster than
	// MAX_SPEED times normal speed, and eases speed changes over EASE seconds so they don't click.
	static constexpr double FOLLOW = 0.08;
	static constexpr double MAX_SPEED = 4.0;
	static constexpr double EASE = 0.03;
	// A target further than JUMP away is jumped to rather than raced towards.
	static constexpr double JUMP = 0.5;
	// Below this speed the sound fades, so a stopping playhead goes quiet instead of grinding.
	static constexpr double FADE_SPEED = 0.25;
	// Silent and still this long, it ends.
	static constexpr double IDLE_END = 1.0;
	// A target not moved for this long counts as still: a playhead held in place.
	static constexpr double STILL_AFTER = 0.12;

	Ref<AudioStreamScrub> scrub;
	Ref<AudioScrubData> data;
	bool active = false;
	double position = 0.0;
	double velocity = 0.0;
	double still_time = 0.0;
	// Audio mixed since the target last moved: the target is carried on at its speed by this much,
	// counted in mixed audio rather than wall time, as drivers mix in bursts ahead of playback.
	uint64_t followed_target_usec = 0;
	double since_target = 0.0;

protected:
	static void _bind_methods() {}

public:
	virtual void start(double p_from_pos = 0.0) override;
	virtual void stop() override;
	virtual bool is_playing() const override;
	virtual int get_loop_count() const override { return 0; }
	virtual double get_playback_position() const override { return position; }
	virtual void seek(double p_time) override { position = p_time; }
	virtual int mix(AudioFrame *p_buffer, float p_rate_scale, int p_frames) override;
};
