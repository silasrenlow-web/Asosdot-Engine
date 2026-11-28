/**************************************************************************/
/*  eye_tracker.h                                                         */
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

#pragma once

#include "face_tracker.h"
#include "gaze_model.h"

#include "core/object/object.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/typed_array.h"

class CameraFeed;
class CanvasLayer;
class Control;
class Window;

// Where the player is looking, whether their eyes are open, and whether they
// are looking at the screen at all, from a webcam or, without one, from the
// mouse or the screen's centre.
//
// Privacy: camera frames are read in memory on a thread of this object and
// dropped once measured. Nothing is written to disk and nothing is sent
// anywhere. The webcam is only opened by start(), and only when the project
// allows it (eye_tracking/webcam/enabled). Calibration keeps a few numbers,
// never pictures; get_calibration_data() hands them to the game to keep if it
// wants to.
class EyeTracker : public Object {
	GDCLASS(EyeTracker, Object);

public:
	enum Source {
		SOURCE_WEBCAM,
		SOURCE_MOUSE,
		SOURCE_SCREEN_CENTER,
	};

	enum Status {
		STATUS_STOPPED,
		STATUS_NO_CAMERA,
		STATUS_STARTING,
		STATUS_SEARCHING,
		STATUS_TRACKING,
		STATUS_SIMULATED,
	};

	enum Eye {
		EYE_LEFT,
		EYE_RIGHT,
		EYE_BOTH,
	};

private:
	static EyeTracker *singleton;

	// What the camera thread hands the main thread for each frame it measured.
	struct Sample {
		uint64_t serial = 0;
		double time = 0.0;
		bool face = false;
		bool eyes = false;
		float score = 0.0;
		bool meshed = false;
		Vector2 iris_offset; // Both eyes, averaged.
		float upper_lid = 0.0; // Both eyes, averaged.
		float openness[2] = { 0, 0 };
		float yaw = 0.0; // Radians.
		float pitch = 0.0;
		Vector2 face_centre; // 0 to 1 in the frame.
		float face_size = 0.0; // Eye distance over the frame's width.
		float roll = 0.0;
		FaceObservation observation; // For the debug view.
		float process_ms = 0.0;
	};

	enum {
		FEATURE_COUNT = GazeModel::FEATURE_COUNT,
	};

	struct OneEuro {
		float value = 0.0;
		float derivative = 0.0;
		bool primed = false;
		float filter(float p_value, float p_dt, float p_min_cutoff, float p_beta);
	};

	// Settings.
	Source source = SOURCE_SCREEN_CENTER;
	float smoothing = 0.5;
	float blink_threshold = 0.5;
	float screen_margin = 0.15;
	bool mouse_fallback = true;
	StringName simulated_blink_action;
	bool debug_overlay = false;
	bool mirror_camera = true;

	// Camera thread.
	Ref<CameraFeed> feed;
	bool opened_feed = false;
	bool webcam_wanted = false; // Allowed by the project and the models loaded.
	Thread thread;
	SafeFlag thread_exit;
	mutable Mutex sample_mutex;
	Sample latest;
	uint64_t latest_serial = 0;
	FaceTracker face_tracker;
	static void _thread_func(void *p_self);
	void _thread_loop();

	// Main thread state.
	bool running = false;
	Status status = STATUS_STOPPED;
	uint64_t consumed_serial = 0;
	Sample current;
	double last_face_time = -100.0;
	Vector2 gaze_uv = Vector2(0.5, 0.5);
	Vector2 raw_gaze_uv = Vector2(0.5, 0.5);
	OneEuro filter_x;
	OneEuro filter_y;
	double last_filter_time = 0.0;
	float tracking_rate = 0.0; // Frames measured a second.
	Vector2 last_iris; // For telling a fixation from a jump of the eyes.
	float openness[2] = { 1, 1 };
	float open_baseline[2] = { 0.25, 0.25 };
	bool baseline_primed = false;
	bool eyes_closed = false;
	double closed_since = 0.0;
	bool looking = false;
	bool face_seen = false;
	// Uncalibrated, the tracker assumes the player looks at the middle of the
	// screen on average and slowly learns where that is.
	float auto_centre[FEATURE_COUNT] = {};
	bool auto_centre_primed = false;

	// Calibration: dots to look at, then a few more to measure how well it
	// worked. Each dot keeps the samples taken while the eyes rested on it.
	GazeModel model;
	struct CalibrationPoint {
		Vector2 target;
		LocalVector<float> rows; // FEATURE_COUNT floats per sample.
		bool check = false; // One of the accuracy check's dots.
	};
	bool calibrating = false;
	int calibration_index = 0;
	double calibration_point_start = 0.0;
	LocalVector<CalibrationPoint> calibration_points;
	Dictionary accuracy;
	void _finish_calibration();
	void _measure_accuracy();

	// Overlays, made when first needed.
	CanvasLayer *overlay_layer = nullptr;
	ObjectID overlay_id; // The tree frees the overlay when the game quits.
	Control *debug_control = nullptr;
	Control *calibration_control = nullptr;
	bool connected_to_tree = false;

	void _features(const Sample &p_sample, float r_features[FEATURE_COUNT]) const;
	Vector2 _map_gaze(const float p_features[FEATURE_COUNT]) const;
	void _set_status(Status p_status);
	void _process_frame();
	void _update_webcam(double p_now);
	void _update_simulated(double p_now);
	void _update_calibration(double p_now);
	bool _fit_calibration();
	void _ensure_overlay();
	void _free_overlay();
	void _draw_debug();
	void _draw_calibration();
	void _push_gaze_to_viewport();
	Window *_get_root() const;
	Ref<CameraFeed> _pick_feed(int p_feed_id) const;
	void _close_feed();

protected:
	static void _bind_methods();

public:
	static EyeTracker *get_singleton() { return singleton; }

	Error start(Source p_source = SOURCE_WEBCAM, int p_camera_feed_id = -1);
	void stop();
	bool is_running() const { return running; }
	Status get_status() const { return status; }
	Source get_source() const { return source; }
	int get_camera_feed_id() const;

	Vector2 get_gaze_uv() const { return gaze_uv; }
	Vector2 get_gaze_position() const;
	bool is_looking_at_screen() const;
	bool is_face_detected() const;
	float get_eye_openness(Eye p_eye = EYE_BOTH) const;
	bool are_eyes_closed() const;
	Vector3 get_head_rotation() const;
	float get_confidence() const;
	float get_processing_time() const { return current.process_ms; }
	float get_tracking_rate() const { return tracking_rate; }
	Rect2 get_face_rect() const;

	void start_calibration(int p_points = 16, bool p_check_accuracy = true);
	void cancel_calibration();
	bool is_calibrating() const { return calibrating; }
	bool is_calibrated() const { return model.is_valid(); }
	Dictionary get_accuracy() const { return accuracy; }
	void clear_calibration();
	PackedFloat32Array get_calibration_data() const;
	bool set_calibration_data(const PackedFloat32Array &p_data);

	void set_smoothing(float p_smoothing);
	float get_smoothing() const { return smoothing; }
	void set_blink_threshold(float p_threshold);
	float get_blink_threshold() const { return blink_threshold; }
	void set_screen_margin(float p_margin);
	float get_screen_margin() const { return screen_margin; }
	void set_mouse_fallback(bool p_enabled) { mouse_fallback = p_enabled; }
	bool is_mouse_fallback() const { return mouse_fallback; }
	void set_simulated_blink_action(const StringName &p_action) { simulated_blink_action = p_action; }
	StringName get_simulated_blink_action() const { return simulated_blink_action; }
	void set_debug_overlay(bool p_enabled);
	bool is_debug_overlay() const { return debug_overlay; }
	void set_mirror_camera(bool p_mirror) { mirror_camera = p_mirror; }
	bool is_mirror_camera() const { return mirror_camera; }

	// For tests and tools: measures one still picture with the same models,
	// no webcam needed. The result holds the face box, eye points, iris
	// centres, openness and head turn.
	Dictionary analyze_image(const Ref<Image> &p_image);

	EyeTracker();
	~EyeTracker();
};

VARIANT_ENUM_CAST(EyeTracker::Source);
VARIANT_ENUM_CAST(EyeTracker::Status);
VARIANT_ENUM_CAST(EyeTracker::Eye);
