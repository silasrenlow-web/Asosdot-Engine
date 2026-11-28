/**************************************************************************/
/*  eye_tracker.cpp                                                       */
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

#include "eye_tracker.h"

#include "core/config/project_settings.h"
#include "core/input/input.h"
#include "core/input/input_map.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/gui/control.h"
#include "scene/main/canvas_layer.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/font.h"
#include "servers/camera/camera_feed.h"
#include "servers/camera/camera_server.h"
#include "servers/display/display_server.h"
#include "servers/rendering/rendering_server.h"

EyeTracker *EyeTracker::singleton = nullptr;

namespace {

// Calibration: how long the eyes get to land on a dot, then how long it is
// measured for.
const double CALIBRATION_SETTLE = 0.75;
const double CALIBRATION_COLLECT = 1.0;
// A sample whose iris moved more than this (eye widths) since the last one is
// the eye jumping or the measurement slipping, not the eye resting on the dot.
const float CALIBRATION_JUMP = 0.05f;
// A webcam's view across, for working out how far away the player sits.
const float CAMERA_FIELD_OF_VIEW = Math::deg_to_rad(68.0f);
// The distance between the centres of a grown-up's eyes, in centimetres.
const float EYE_DISTANCE_CM = 6.3f;
// How long without a face before it counts as lost (a frame or two drop out).
const double FACE_LOST_AFTER = 0.4;
// A closed-and-open shorter than this is a blink; longer, the eyes were shut.
const double BLINK_LONGEST = 0.6;
// Uncalibrated, how long the average gaze takes to settle on the screen's middle.
const float AUTO_CENTRE_SECONDS = 12.0f;

float smoothing_alpha(float p_cutoff, float p_dt) {
	const float tau = 1.0f / (Math::TAU * p_cutoff);
	return 1.0f / (1.0f + tau / MAX(p_dt, 0.0001f));
}

} // namespace

float EyeTracker::OneEuro::filter(float p_value, float p_dt, float p_min_cutoff, float p_beta) {
	// The one euro filter (Casiez et al.): smooth when the gaze rests, quick
	// when it jumps.
	if (!primed) {
		value = p_value;
		derivative = 0.0f;
		primed = true;
		return value;
	}
	const float dx = (p_value - value) / MAX(p_dt, 0.0001f);
	derivative += (dx - derivative) * smoothing_alpha(1.0f, p_dt);
	const float cutoff = p_min_cutoff + p_beta * Math::abs(derivative);
	value += (p_value - value) * smoothing_alpha(cutoff, p_dt);
	return value;
}

void EyeTracker::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start", "source", "camera_feed_id"), &EyeTracker::start, DEFVAL(SOURCE_WEBCAM), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("stop"), &EyeTracker::stop);
	ClassDB::bind_method(D_METHOD("is_running"), &EyeTracker::is_running);
	ClassDB::bind_method(D_METHOD("get_status"), &EyeTracker::get_status);
	ClassDB::bind_method(D_METHOD("get_source"), &EyeTracker::get_source);
	ClassDB::bind_method(D_METHOD("get_camera_feed_id"), &EyeTracker::get_camera_feed_id);

	ClassDB::bind_method(D_METHOD("get_gaze_uv"), &EyeTracker::get_gaze_uv);
	ClassDB::bind_method(D_METHOD("get_gaze_position"), &EyeTracker::get_gaze_position);
	ClassDB::bind_method(D_METHOD("is_looking_at_screen"), &EyeTracker::is_looking_at_screen);
	ClassDB::bind_method(D_METHOD("is_face_detected"), &EyeTracker::is_face_detected);
	ClassDB::bind_method(D_METHOD("get_eye_openness", "eye"), &EyeTracker::get_eye_openness, DEFVAL(EYE_BOTH));
	ClassDB::bind_method(D_METHOD("are_eyes_closed"), &EyeTracker::are_eyes_closed);
	ClassDB::bind_method(D_METHOD("get_head_rotation"), &EyeTracker::get_head_rotation);
	ClassDB::bind_method(D_METHOD("get_confidence"), &EyeTracker::get_confidence);
	ClassDB::bind_method(D_METHOD("get_processing_time"), &EyeTracker::get_processing_time);
	ClassDB::bind_method(D_METHOD("get_tracking_rate"), &EyeTracker::get_tracking_rate);
	ClassDB::bind_method(D_METHOD("get_face_rect"), &EyeTracker::get_face_rect);

	ClassDB::bind_method(D_METHOD("start_calibration", "points", "check_accuracy"), &EyeTracker::start_calibration, DEFVAL(16), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("get_accuracy"), &EyeTracker::get_accuracy);
	ClassDB::bind_method(D_METHOD("cancel_calibration"), &EyeTracker::cancel_calibration);
	ClassDB::bind_method(D_METHOD("is_calibrating"), &EyeTracker::is_calibrating);
	ClassDB::bind_method(D_METHOD("is_calibrated"), &EyeTracker::is_calibrated);
	ClassDB::bind_method(D_METHOD("clear_calibration"), &EyeTracker::clear_calibration);
	ClassDB::bind_method(D_METHOD("get_calibration_data"), &EyeTracker::get_calibration_data);
	ClassDB::bind_method(D_METHOD("set_calibration_data", "data"), &EyeTracker::set_calibration_data);

	ClassDB::bind_method(D_METHOD("set_smoothing", "smoothing"), &EyeTracker::set_smoothing);
	ClassDB::bind_method(D_METHOD("get_smoothing"), &EyeTracker::get_smoothing);
	ClassDB::bind_method(D_METHOD("set_blink_threshold", "threshold"), &EyeTracker::set_blink_threshold);
	ClassDB::bind_method(D_METHOD("get_blink_threshold"), &EyeTracker::get_blink_threshold);
	ClassDB::bind_method(D_METHOD("set_screen_margin", "margin"), &EyeTracker::set_screen_margin);
	ClassDB::bind_method(D_METHOD("get_screen_margin"), &EyeTracker::get_screen_margin);
	ClassDB::bind_method(D_METHOD("set_mouse_fallback", "enabled"), &EyeTracker::set_mouse_fallback);
	ClassDB::bind_method(D_METHOD("is_mouse_fallback"), &EyeTracker::is_mouse_fallback);
	ClassDB::bind_method(D_METHOD("set_simulated_blink_action", "action"), &EyeTracker::set_simulated_blink_action);
	ClassDB::bind_method(D_METHOD("get_simulated_blink_action"), &EyeTracker::get_simulated_blink_action);
	ClassDB::bind_method(D_METHOD("set_debug_overlay", "enabled"), &EyeTracker::set_debug_overlay);
	ClassDB::bind_method(D_METHOD("is_debug_overlay"), &EyeTracker::is_debug_overlay);
	ClassDB::bind_method(D_METHOD("set_mirror_camera", "mirror"), &EyeTracker::set_mirror_camera);
	ClassDB::bind_method(D_METHOD("is_mirror_camera"), &EyeTracker::is_mirror_camera);

	ClassDB::bind_method(D_METHOD("analyze_image", "image"), &EyeTracker::analyze_image);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "smoothing", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_smoothing", "get_smoothing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "blink_threshold", PROPERTY_HINT_RANGE, "0.05,0.95,0.01"), "set_blink_threshold", "get_blink_threshold");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "screen_margin", PROPERTY_HINT_RANGE, "0,0.5,0.01"), "set_screen_margin", "get_screen_margin");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mouse_fallback"), "set_mouse_fallback", "is_mouse_fallback");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "simulated_blink_action"), "set_simulated_blink_action", "get_simulated_blink_action");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_overlay"), "set_debug_overlay", "is_debug_overlay");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mirror_camera"), "set_mirror_camera", "is_mirror_camera");

	ADD_SIGNAL(MethodInfo("status_changed", PropertyInfo(Variant::INT, "status", PROPERTY_HINT_ENUM, "Stopped,No Camera,Starting,Searching,Tracking,Simulated")));
	ADD_SIGNAL(MethodInfo("face_found"));
	ADD_SIGNAL(MethodInfo("face_lost"));
	ADD_SIGNAL(MethodInfo("blinked"));
	ADD_SIGNAL(MethodInfo("eyes_closed"));
	ADD_SIGNAL(MethodInfo("eyes_opened", PropertyInfo(Variant::FLOAT, "closed_time")));
	ADD_SIGNAL(MethodInfo("looked_away"));
	ADD_SIGNAL(MethodInfo("looked_back"));
	ADD_SIGNAL(MethodInfo("calibration_finished", PropertyInfo(Variant::BOOL, "success")));

	BIND_ENUM_CONSTANT(SOURCE_WEBCAM);
	BIND_ENUM_CONSTANT(SOURCE_MOUSE);
	BIND_ENUM_CONSTANT(SOURCE_SCREEN_CENTER);

	BIND_ENUM_CONSTANT(STATUS_STOPPED);
	BIND_ENUM_CONSTANT(STATUS_NO_CAMERA);
	BIND_ENUM_CONSTANT(STATUS_STARTING);
	BIND_ENUM_CONSTANT(STATUS_SEARCHING);
	BIND_ENUM_CONSTANT(STATUS_TRACKING);
	BIND_ENUM_CONSTANT(STATUS_SIMULATED);

	BIND_ENUM_CONSTANT(EYE_LEFT);
	BIND_ENUM_CONSTANT(EYE_RIGHT);
	BIND_ENUM_CONSTANT(EYE_BOTH);
}

EyeTracker::EyeTracker() {
	if (singleton == nullptr) {
		singleton = this;
	}
}

EyeTracker::~EyeTracker() {
	stop();
	if (singleton == this) {
		singleton = nullptr;
	}
}

// --- Starting and stopping ------------------------------------------------

Window *EyeTracker::_get_root() const {
	SceneTree *tree = SceneTree::get_singleton();
	return tree ? tree->get_root() : nullptr;
}

Ref<CameraFeed> EyeTracker::_pick_feed(int p_feed_id) const {
	CameraServer *server = CameraServer::get_singleton();
	if (!server) {
		return Ref<CameraFeed>();
	}
	if (p_feed_id >= 0) {
		return server->get_feed_by_id(p_feed_id);
	}
	// A camera facing the player first, then any.
	Ref<CameraFeed> any;
	for (int i = 0; i < server->get_feed_count(); i++) {
		Ref<CameraFeed> candidate = server->get_feed(i);
		if (candidate.is_null()) {
			continue;
		}
		if (candidate->get_position() == CameraFeed::FEED_FRONT) {
			return candidate;
		}
		if (any.is_null() && candidate->get_position() != CameraFeed::FEED_BACK) {
			any = candidate;
		}
	}
	return any;
}

Error EyeTracker::start(Source p_source, int p_camera_feed_id) {
	stop();
	source = p_source;
	running = true;

	SceneTree *tree = SceneTree::get_singleton();
	if (tree && !connected_to_tree) {
		tree->connect(SNAME("process_frame"), callable_mp(this, &EyeTracker::_process_frame));
		connected_to_tree = true;
	}

	if (source != SOURCE_WEBCAM) {
		_set_status(STATUS_SIMULATED);
		return OK;
	}

	if (!bool(GLOBAL_GET("eye_tracking/webcam/enabled"))) {
		WARN_PRINT("Eye tracking: the webcam is off for this project (Project Settings > Eye Tracking > Webcam > Enabled). Using the fallback instead.");
		_set_status(STATUS_NO_CAMERA);
		return ERR_UNAUTHORIZED;
	}

	Error err = face_tracker.initialize();
	if (err != OK) {
		_set_status(STATUS_NO_CAMERA);
		return err;
	}
	webcam_wanted = true;

	CameraServer *server = CameraServer::get_singleton();
	if (server) {
		server->set_monitoring_feeds(true);
	}
	feed = _pick_feed(p_camera_feed_id);
	if (feed.is_null()) {
		// Some platforms find cameras a moment later; _update_webcam keeps looking.
		_set_status(STATUS_NO_CAMERA);
		return ERR_UNAVAILABLE;
	}

	if (!feed->is_active()) {
		feed->set_active(true);
		if (!feed->is_active()) {
			feed.unref();
			_set_status(STATUS_NO_CAMERA);
			return ERR_CANT_OPEN;
		}
		opened_feed = true;
	}
	face_tracker.reset_tracking();
	thread_exit.clear();
	thread.start(_thread_func, this);
	_set_status(STATUS_STARTING);
	return OK;
}

void EyeTracker::_close_feed() {
	thread_exit.set();
	if (thread.is_started()) {
		thread.wait_to_finish();
	}
	if (feed.is_valid() && opened_feed) {
		feed->set_active(false);
	}
	opened_feed = false;
	feed.unref();
	{
		MutexLock lock(sample_mutex);
		latest = Sample();
	}
}

void EyeTracker::stop() {
	_close_feed();
	webcam_wanted = false;
	if (calibrating) {
		calibrating = false;
		calibration_points.clear();
	}
	running = false;
	consumed_serial = 0;
	latest_serial = 0;
	current = Sample();
	eyes_closed = false;
	looking = false;
	face_seen = false;
	filter_x = OneEuro();
	filter_y = OneEuro();
	gaze_uv = Vector2(0.5, 0.5);
	_push_gaze_to_viewport();
	_free_overlay();
	if (connected_to_tree) {
		SceneTree *tree = SceneTree::get_singleton();
		if (tree && tree->is_connected(SNAME("process_frame"), callable_mp(this, &EyeTracker::_process_frame))) {
			tree->disconnect(SNAME("process_frame"), callable_mp(this, &EyeTracker::_process_frame));
		}
		connected_to_tree = false;
	}
	_set_status(STATUS_STOPPED);
}

int EyeTracker::get_camera_feed_id() const {
	return feed.is_valid() ? feed->get_id() : -1;
}

// --- Camera thread --------------------------------------------------------

void EyeTracker::_thread_func(void *p_self) {
	static_cast<EyeTracker *>(p_self)->_thread_loop();
}

void EyeTracker::_thread_loop() {
	const Ref<CameraFeed> source_feed = feed;
	uint64_t last_frame = 0;
	Vector<uint8_t> data;
	while (!thread_exit.is_set()) {
		int width = 0;
		int height = 0;
		Image::Format format = Image::FORMAT_RGB8;
		uint64_t frame_serial = 0;
		if (source_feed.is_null() || !source_feed->get_frame_data(data, width, height, format, frame_serial) || frame_serial == last_frame) {
			OS::get_singleton()->delay_usec(3000);
			continue;
		}
		last_frame = frame_serial;

		const uint64_t begin = OS::get_singleton()->get_ticks_usec();
		const bool mirror = mirror_camera;
		Sample sample;
		FaceObservation &face = sample.observation;
		sample.face = face_tracker.process(data.ptr(), width, height, format, mirror, face);
		// Done with the picture.
		data = Vector<uint8_t>();

		sample.time = begin / 1000000.0;
		sample.process_ms = (OS::get_singleton()->get_ticks_usec() - begin) / 1000.0f;
		if (sample.face) {
			sample.score = face.score;
			sample.meshed = face.meshed;
			sample.eyes = face.eyes[0].valid && face.eyes[1].valid;
			if (sample.eyes) {
				// An eye seen more side-on (the head turned away from it) is
				// narrower in the picture and measured worse: it counts less.
				float weights[2];
				for (int e = 0; e < 2; e++) {
					weights[e] = face.eyes[e].corners[0].distance_squared_to(face.eyes[e].corners[1]) + 1.0f;
				}
				const float total = weights[0] + weights[1];
				sample.iris_offset = (face.eyes[0].iris_offset * weights[0] + face.eyes[1].iris_offset * weights[1]) / total;
				sample.upper_lid = (face.eyes[0].upper_lid * weights[0] + face.eyes[1].upper_lid * weights[1]) / total;
				// In a mirror, the player's left eye is on the left.
				const int left = mirror ? 0 : 1;
				sample.openness[0] = face.eyes[left].openness;
				sample.openness[1] = face.eyes[1 - left].openness;
			}
			sample.yaw = face.yaw;
			sample.pitch = face.pitch;
			sample.roll = face.roll;
			sample.face_centre = Vector2(face.head_centre.x / width, face.head_centre.y / height);
			sample.face_size = face.eye_distance / width;
		}

		MutexLock lock(sample_mutex);
		latest_serial++;
		sample.serial = latest_serial;
		latest = sample;
	}
}

// --- Main thread ----------------------------------------------------------

void EyeTracker::_set_status(Status p_status) {
	if (status == p_status) {
		return;
	}
	status = p_status;
	emit_signal(SNAME("status_changed"), status);
}

void EyeTracker::_features(const Sample &p_sample, float r_features[FEATURE_COUNT]) const {
	GazeModel::Measurement measurement;
	measurement.iris = p_sample.iris_offset;
	measurement.upper_lid = p_sample.upper_lid;
	measurement.yaw = p_sample.yaw;
	measurement.pitch = p_sample.pitch;
	measurement.face = p_sample.face_centre;
	measurement.face_size = p_sample.face_size;
	GazeModel::make_features(measurement, r_features);
}

Vector2 EyeTracker::_map_gaze(const float p_features[FEATURE_COUNT]) const {
	if (model.is_valid()) {
		return model.map(p_features);
	}
	// No calibration: how far the eyes and head have turned from where they
	// usually are, with gains from a laptop's screen at arm's length (an iris
	// moves about a quarter of the eye's width across the screen, the head
	// about half a radian). Rough, but it points the right way.
	const float *c = auto_centre;
	const float x = 0.5f + 4.0f * (p_features[GazeModel::FEATURE_IRIS_X] - c[GazeModel::FEATURE_IRIS_X]) + 1.2f * (p_features[GazeModel::FEATURE_YAW] - c[GazeModel::FEATURE_YAW]);
	const float y = 0.5f + 5.0f * (p_features[GazeModel::FEATURE_IRIS_Y] - c[GazeModel::FEATURE_IRIS_Y]) + 1.2f * (p_features[GazeModel::FEATURE_PITCH] - c[GazeModel::FEATURE_PITCH]);
	return Vector2(x, y);
}

void EyeTracker::_process_frame() {
	if (!running) {
		return;
	}
	const double now = OS::get_singleton()->get_ticks_usec() / 1000000.0;

	const bool was_closed = eyes_closed;
	const bool was_looking = looking;
	const bool had_face = face_seen;

	if (source == SOURCE_WEBCAM && webcam_wanted) {
		// Also keeps looking for a camera while there is none.
		_update_webcam(now);
	}
	if (source != SOURCE_WEBCAM || status == STATUS_NO_CAMERA) {
		_update_simulated(now);
	}

	if (calibrating) {
		_update_calibration(now);
	}

	if (face_seen != had_face) {
		emit_signal(face_seen ? SNAME("face_found") : SNAME("face_lost"));
	}
	if (eyes_closed != was_closed) {
		if (eyes_closed) {
			closed_since = now;
			emit_signal(SNAME("eyes_closed"));
		} else {
			const double closed_time = now - closed_since;
			emit_signal(SNAME("eyes_opened"), closed_time);
			if (closed_time <= BLINK_LONGEST) {
				emit_signal(SNAME("blinked"));
			}
		}
	}
	if (looking != was_looking) {
		emit_signal(looking ? SNAME("looked_back") : SNAME("looked_away"));
	}

	_push_gaze_to_viewport();
	if (debug_overlay || calibrating) {
		_ensure_overlay();
	}
	if (overlay_layer && ObjectDB::get_instance(overlay_id) != overlay_layer) {
		// Freed with the scene tree it was in.
		overlay_layer = nullptr;
		debug_control = nullptr;
		calibration_control = nullptr;
	}
	if (debug_control) {
		debug_control->set_visible(debug_overlay);
		debug_control->queue_redraw();
	}
	if (calibration_control) {
		calibration_control->set_visible(calibrating);
		calibration_control->queue_redraw();
	}
}

void EyeTracker::_update_webcam(double p_now) {
	if (feed.is_null()) {
		// No camera yet: look again now and then (plugged in, or found late).
		static double last_look = 0.0;
		if (p_now - last_look < 1.0) {
			return;
		}
		last_look = p_now;
		feed = _pick_feed(-1);
		if (feed.is_null()) {
			return;
		}
		if (!feed->is_active()) {
			feed->set_active(true);
			if (!feed->is_active()) {
				feed.unref();
				return;
			}
			opened_feed = true;
		}
		face_tracker.reset_tracking();
		thread_exit.clear();
		thread.start(_thread_func, this);
		_set_status(STATUS_STARTING);
		return;
	}

	Sample sample;
	bool new_frame = false;
	{
		MutexLock lock(sample_mutex);
		if (latest.serial != consumed_serial) {
			sample = latest;
			new_frame = true;
		}
	}
	if (!new_frame) {
		// A camera that stops sending (unplugged, taken by another program) must
		// not leave the last face standing. Outside the lock: signal handlers may
		// stop the tracker, which waits for the camera thread.
		if (face_seen && p_now - last_face_time > FACE_LOST_AFTER * 3.0) {
			face_seen = false;
			looking = false;
			_set_status(STATUS_SEARCHING);
		}
		return;
	}
	consumed_serial = sample.serial;
	const float dt = current.time > 0.0 ? float(CLAMP(sample.time - current.time, 0.001, 0.5)) : 1.0f / 30.0f;
	current = sample;
	tracking_rate = tracking_rate <= 0.0f ? 1.0f / dt : Math::lerp(tracking_rate, 1.0f / dt, 0.05f);

	if (!sample.face || !sample.eyes) {
		if (p_now - last_face_time > FACE_LOST_AFTER) {
			face_seen = false;
			looking = false;
			_set_status(STATUS_SEARCHING);
		}
		return;
	}
	last_face_time = p_now;
	face_seen = true;
	_set_status(STATUS_TRACKING);

	// How open each eye is, against how open it usually is when open: the
	// baseline follows wide-open eyes quickly and narrow ones slowly.
	for (int e = 0; e < 2; e++) {
		const float o = sample.openness[e];
		if (!baseline_primed) {
			open_baseline[e] = MAX(o, 0.15f);
		} else if (o > open_baseline[e]) {
			open_baseline[e] += (o - open_baseline[e]) * 0.2f;
		} else {
			open_baseline[e] += (o - open_baseline[e]) * MIN(dt * 0.05f, 1.0f);
		}
		open_baseline[e] = MAX(open_baseline[e], 0.08f);
		openness[e] = CLAMP(o / open_baseline[e], 0.0f, 1.0f);
	}
	baseline_primed = true;
	const float both = (openness[0] + openness[1]) * 0.5f;
	if (eyes_closed) {
		eyes_closed = both < blink_threshold + 0.1f;
	} else {
		eyes_closed = both < blink_threshold;
	}

	if (!eyes_closed) {
		// The iris is hidden or squashed while the lids close; keep the last gaze.
		float features[FEATURE_COUNT];
		_features(sample, features);
		if (!auto_centre_primed) {
			for (int k = 0; k < FEATURE_COUNT; k++) {
				auto_centre[k] = features[k];
			}
			auto_centre_primed = true;
		} else {
			const float a = MIN(dt / AUTO_CENTRE_SECONDS, 1.0f);
			for (int k = 0; k < FEATURE_COUNT; k++) {
				auto_centre[k] += (features[k] - auto_centre[k]) * a;
			}
		}

		raw_gaze_uv = _map_gaze(features);
		const float min_cutoff = Math::lerp(4.0f, 0.3f, smoothing);
		const float beta = Math::lerp(4.0f, 0.6f, smoothing);
		gaze_uv.x = filter_x.filter(raw_gaze_uv.x, dt, min_cutoff, beta);
		gaze_uv.y = filter_y.filter(raw_gaze_uv.y, dt, min_cutoff, beta);

		// Only samples where the eyes rest: one that jumped from the last is the
		// eye still moving to the dot, or a measurement that slipped.
		const bool steady = last_iris.distance_to(sample.iris_offset) < CALIBRATION_JUMP;
		last_iris = sample.iris_offset;
		if (calibrating && steady && calibration_index < int(calibration_points.size())) {
			const double t = p_now - calibration_point_start;
			if (t >= CALIBRATION_SETTLE && t < CALIBRATION_SETTLE + CALIBRATION_COLLECT) {
				CalibrationPoint &point = calibration_points[calibration_index];
				for (int k = 0; k < FEATURE_COUNT; k++) {
					point.rows.push_back(features[k]);
				}
			}
		}
	}

	const float m = screen_margin;
	looking = !eyes_closed && gaze_uv.x >= -m && gaze_uv.x <= 1.0f + m && gaze_uv.y >= -m && gaze_uv.y <= 1.0f + m;
}

void EyeTracker::_update_simulated(double p_now) {
	Window *root = _get_root();
	face_seen = true;
	if (source == SOURCE_SCREEN_CENTER || !mouse_fallback || !root) {
		gaze_uv = Vector2(0.5, 0.5);
	} else {
		const Rect2 visible = root->get_visible_rect();
		const Vector2 mouse = root->get_mouse_position();
		if (visible.size.x > 0 && visible.size.y > 0) {
			gaze_uv = (mouse - visible.position) / visible.size;
		}
	}
	bool closed = false;
	if (simulated_blink_action != StringName() && InputMap::get_singleton()->has_action(simulated_blink_action)) {
		closed = Input::get_singleton()->is_action_pressed(simulated_blink_action);
	}
	eyes_closed = closed;
	openness[0] = openness[1] = closed ? 0.0f : 1.0f;
	const float m = screen_margin;
	looking = !eyes_closed && gaze_uv.x >= -m && gaze_uv.x <= 1.0f + m && gaze_uv.y >= -m && gaze_uv.y <= 1.0f + m;
}

void EyeTracker::_push_gaze_to_viewport() {
	RenderingServer *rs = RenderingServer::get_singleton();
	Window *root = _get_root();
	if (rs && root) {
		rs->viewport_set_gaze_point(root->get_viewport_rid(), gaze_uv);
	}
}

// --- Reading the state ----------------------------------------------------

Vector2 EyeTracker::get_gaze_position() const {
	Window *root = _get_root();
	if (!root) {
		return Vector2();
	}
	const Rect2 visible = root->get_visible_rect();
	return visible.position + gaze_uv * visible.size;
}

bool EyeTracker::is_looking_at_screen() const {
	// Not running, the player is taken to look at the screen with open eyes,
	// so games work the same without eye tracking.
	return !running || looking;
}

bool EyeTracker::is_face_detected() const {
	return running && face_seen;
}

float EyeTracker::get_eye_openness(Eye p_eye) const {
	if (!running) {
		return 1.0f;
	}
	switch (p_eye) {
		case EYE_LEFT:
			return openness[0];
		case EYE_RIGHT:
			return openness[1];
		default:
			return (openness[0] + openness[1]) * 0.5f;
	}
}

bool EyeTracker::are_eyes_closed() const {
	return running && eyes_closed;
}

Vector3 EyeTracker::get_head_rotation() const {
	if (!running || source != SOURCE_WEBCAM || !face_seen) {
		return Vector3();
	}
	const float yaw = Math::rad_to_deg(current.yaw);
	const float pitch = Math::rad_to_deg(current.pitch);
	const float roll = Math::rad_to_deg(current.roll);
	// As a Node3D's rotation: x up, y to the player's left, z tilting left.
	return Vector3(-pitch, -yaw, -roll);
}

Rect2 EyeTracker::get_face_rect() const {
	// In the camera picture as a mirror shows it, 0 to 1 across and down.
	const FaceObservation &face = current.observation;
	if (!running || !face_seen || !face.found || face.frame_size.x <= 0) {
		return Rect2();
	}
	const Vector2 size = Vector2(face.frame_size);
	return Rect2(face.box.position / size, face.box.size / size);
}

float EyeTracker::get_confidence() const {
	if (!running) {
		return 0.0f;
	}
	if (source != SOURCE_WEBCAM || status == STATUS_NO_CAMERA) {
		return 1.0f;
	}
	return face_seen ? current.score : 0.0f;
}

// --- Calibration ------------------------------------------------------------

void EyeTracker::start_calibration(int p_points, bool p_check_accuracy) {
	if (!running || source != SOURCE_WEBCAM || status == STATUS_NO_CAMERA) {
		WARN_PRINT("Eye tracking: calibration needs the webcam running (start(EyeTracker.SOURCE_WEBCAM)).");
		call_deferred(SNAME("emit_signal"), SNAME("calibration_finished"), false);
		return;
	}
	// Dots in a grid over the screen, a little in from its edges, row after row
	// back and forth so the eyes never cross the whole screen at once.
	LocalVector<Vector2> targets;
	if (p_points <= 5) {
		for (const Vector2 &v : { Vector2(0.5, 0.5), Vector2(0.1, 0.1), Vector2(0.9, 0.1), Vector2(0.9, 0.9), Vector2(0.1, 0.9) }) {
			targets.push_back(v);
		}
	} else {
		const int per_side = p_points <= 9 ? 3 : 4;
		for (int row = 0; row < per_side; row++) {
			for (int i = 0; i < per_side; i++) {
				const int column = row % 2 == 0 ? i : per_side - 1 - i;
				targets.push_back(Vector2(Math::lerp(0.07f, 0.93f, float(column) / (per_side - 1)), Math::lerp(0.08f, 0.92f, float(row) / (per_side - 1))));
			}
		}
	}
	calibration_points.clear();
	for (const Vector2 &target : targets) {
		CalibrationPoint point;
		point.target = target;
		calibration_points.push_back(point);
	}
	if (p_check_accuracy) {
		// Places the calibration did not use.
		for (const Vector2 &v : { Vector2(0.5, 0.5), Vector2(0.28, 0.3), Vector2(0.72, 0.3), Vector2(0.72, 0.7), Vector2(0.28, 0.7) }) {
			CalibrationPoint point;
			point.target = v;
			point.check = true;
			calibration_points.push_back(point);
		}
	}
	accuracy = Dictionary();
	calibration_index = 0;
	calibration_point_start = OS::get_singleton()->get_ticks_usec() / 1000000.0;
	calibrating = true;
}

void EyeTracker::cancel_calibration() {
	if (!calibrating) {
		return;
	}
	calibrating = false;
	calibration_points.clear();
	emit_signal(SNAME("calibration_finished"), false);
}

void EyeTracker::_update_calibration(double p_now) {
	if (Input::get_singleton()->is_action_just_pressed(SNAME("ui_cancel"))) {
		cancel_calibration();
		return;
	}
	if (p_now - calibration_point_start < CALIBRATION_SETTLE + CALIBRATION_COLLECT) {
		return;
	}
	calibration_index++;
	calibration_point_start = p_now;
	if (calibration_index < int(calibration_points.size()) && !calibration_points[calibration_index].check) {
		return;
	}
	if (calibration_index < int(calibration_points.size()) && calibration_points[calibration_index].check && !calibration_points[calibration_index - 1].check) {
		// The calibration dots are done: fit, then check it on the others.
		_finish_calibration();
		if (!model.is_valid()) {
			calibrating = false;
			calibration_points.clear();
			emit_signal(SNAME("calibration_finished"), false);
		}
		return;
	}
	if (calibration_index < int(calibration_points.size())) {
		return;
	}
	// The last dot.
	calibrating = false;
	bool any_check = false;
	for (const CalibrationPoint &point : calibration_points) {
		any_check = any_check || point.check;
	}
	if (any_check) {
		_measure_accuracy();
	} else {
		_finish_calibration();
	}
	calibration_points.clear();
	filter_x = OneEuro();
	filter_y = OneEuro();
	emit_signal(SNAME("calibration_finished"), model.is_valid());
}

void EyeTracker::_finish_calibration() {
	// Each dot's samples, without the ones far from the rest of that dot's (a
	// glance away, a slipped measurement): kept are those within three times
	// the median distance of the median.
	LocalVector<float> rows;
	LocalVector<Vector2> targets;
	LocalVector<int> groups;
	int dots = 0;
	for (uint32_t d = 0; d < calibration_points.size(); d++) {
		const CalibrationPoint &point = calibration_points[d];
		if (point.check) {
			continue;
		}
		const int n = point.rows.size() / FEATURE_COUNT;
		if (n < 5) {
			continue;
		}
		LocalVector<float> xs;
		LocalVector<float> ys;
		for (int s = 0; s < n; s++) {
			xs.push_back(point.rows[s * FEATURE_COUNT + GazeModel::FEATURE_IRIS_X]);
			ys.push_back(point.rows[s * FEATURE_COUNT + GazeModel::FEATURE_IRIS_Y]);
		}
		xs.sort();
		ys.sort();
		const Vector2 median = Vector2(xs[n / 2], ys[n / 2]);
		LocalVector<float> distances;
		for (int s = 0; s < n; s++) {
			distances.push_back(median.distance_to(Vector2(point.rows[s * FEATURE_COUNT + GazeModel::FEATURE_IRIS_X], point.rows[s * FEATURE_COUNT + GazeModel::FEATURE_IRIS_Y])));
		}
		LocalVector<float> sorted;
		sorted = distances;
		sorted.sort();
		const float limit = MAX(sorted[n / 2] * 3.0f, 0.01f);
		int kept = 0;
		for (int s = 0; s < n; s++) {
			if (distances[s] > limit) {
				continue;
			}
			for (int k = 0; k < FEATURE_COUNT; k++) {
				rows.push_back(point.rows[s * FEATURE_COUNT + k]);
			}
			targets.push_back(point.target);
			groups.push_back(d);
			kept++;
		}
		dots += kept > 0 ? 1 : 0;
	}
	if (dots < 5) {
		WARN_PRINT(vformat("Eye tracking: calibration saw the eyes at only %d of the dots; keeping the old one.", dots));
		return;
	}
	GazeModel fitted;
	float cv_error = 1.0f;
	if (!fitted.fit(rows, targets, groups, cv_error)) {
		WARN_PRINT("Eye tracking: calibration could not be fitted; keeping the old one.");
		return;
	}
	model = fitted;
	accuracy = Dictionary();
	accuracy["measured"] = false;
	accuracy["fit_error"] = cv_error;
}

void EyeTracker::_measure_accuracy() {
	// How far the calibrated gaze lands from dots it was not fitted on, and how
	// much it wanders while the eyes rest, in centimetres on the screen and in
	// degrees of the eye's turn.
	if (!model.is_valid()) {
		return;
	}
	Size2 screen_cm = Size2(34.5, 19.4); // A 15.6 inch screen, when the system cannot say.
	DisplayServer *display = DisplayServer::get_singleton();
	if (display) {
		const int dpi = display->screen_get_dpi();
		const Size2i window = display->window_get_size();
		if (dpi > 0 && window.x > 0) {
			screen_cm = Size2(window) / float(dpi) * 2.54f;
		}
	}
	// How far away the player sits, from how far apart their eyes look.
	float distance_cm = 60.0f;
	if (current.face_size > 0.01f) {
		distance_cm = CLAMP(EYE_DISTANCE_CM / (current.face_size * 2.0f * Math::tan(CAMERA_FIELD_OF_VIEW * 0.5f)), 25.0f, 150.0f);
	}

	float error_sum = 0.0f;
	float jitter_sum = 0.0f;
	int measured = 0;
	for (const CalibrationPoint &point : calibration_points) {
		if (!point.check) {
			continue;
		}
		const int n = point.rows.size() / FEATURE_COUNT;
		if (n < 5) {
			continue;
		}
		LocalVector<float> xs;
		LocalVector<float> ys;
		LocalVector<Vector2> predictions;
		for (int s = 0; s < n; s++) {
			const Vector2 p = model.map(point.rows.ptr() + s * FEATURE_COUNT);
			predictions.push_back(p);
			xs.push_back(p.x);
			ys.push_back(p.y);
		}
		xs.sort();
		ys.sort();
		const Vector2 median = Vector2(xs[n / 2], ys[n / 2]);
		error_sum += ((median - point.target) * screen_cm).length();
		float spread = 0.0f;
		for (const Vector2 &p : predictions) {
			spread += ((p - median) * screen_cm).length_squared();
		}
		jitter_sum += Math::sqrt(spread / n);
		measured++;
	}
	const Variant fit_error = accuracy.get("fit_error", Variant());
	accuracy = Dictionary();
	accuracy["measured"] = measured > 0;
	if (fit_error.get_type() != Variant::NIL) {
		accuracy["fit_error"] = fit_error;
	}
	if (measured == 0) {
		return;
	}
	const float error_cm = error_sum / measured;
	accuracy["error_cm"] = error_cm;
	accuracy["error_degrees"] = Math::rad_to_deg(Math::atan(error_cm / distance_cm));
	accuracy["jitter_cm"] = jitter_sum / measured;
	accuracy["distance_cm"] = distance_cm;
	accuracy["screen_cm"] = Vector2(screen_cm);
	accuracy["dots"] = measured;
}

void EyeTracker::clear_calibration() {
	model.clear();
	accuracy = Dictionary();
}

PackedFloat32Array EyeTracker::get_calibration_data() const {
	return model.save();
}

bool EyeTracker::set_calibration_data(const PackedFloat32Array &p_data) {
	GazeModel loaded;
	// Data from an older version of the tracker does not fit this one: calibrate again.
	if (!loaded.load(p_data)) {
		return false;
	}
	model = loaded;
	return true;
}

// --- Settings ---------------------------------------------------------------

void EyeTracker::set_smoothing(float p_smoothing) {
	smoothing = CLAMP(p_smoothing, 0.0f, 1.0f);
}

void EyeTracker::set_blink_threshold(float p_threshold) {
	blink_threshold = CLAMP(p_threshold, 0.05f, 0.95f);
}

void EyeTracker::set_screen_margin(float p_margin) {
	screen_margin = CLAMP(p_margin, 0.0f, 0.5f);
}

void EyeTracker::set_debug_overlay(bool p_enabled) {
	debug_overlay = p_enabled;
	if (debug_control) {
		debug_control->set_visible(p_enabled);
	}
}

// --- Overlays ---------------------------------------------------------------

void EyeTracker::_ensure_overlay() {
	if (overlay_layer) {
		return;
	}
	Window *root = _get_root();
	if (!root) {
		return;
	}
	overlay_layer = memnew(CanvasLayer);
	overlay_id = overlay_layer->get_instance_id();
	overlay_layer->set_name("EyeTrackerOverlay");
	overlay_layer->set_layer(1000);

	calibration_control = memnew(Control);
	calibration_control->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	calibration_control->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	calibration_control->connect(SNAME("draw"), callable_mp(this, &EyeTracker::_draw_calibration));
	overlay_layer->add_child(calibration_control);

	debug_control = memnew(Control);
	debug_control->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	debug_control->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	debug_control->connect(SNAME("draw"), callable_mp(this, &EyeTracker::_draw_debug));
	overlay_layer->add_child(debug_control);

	root->add_child(overlay_layer, false, Node::INTERNAL_MODE_BACK);
}

void EyeTracker::_free_overlay() {
	if (overlay_layer && ObjectDB::get_instance(overlay_id) == overlay_layer) {
		if (overlay_layer->get_parent()) {
			overlay_layer->get_parent()->remove_child(overlay_layer);
		}
		memdelete(overlay_layer);
	}
	overlay_id = ObjectID();
	overlay_layer = nullptr;
	debug_control = nullptr;
	calibration_control = nullptr;
}

void EyeTracker::_draw_calibration() {
	if (!calibrating || !calibration_control || calibration_index >= int(calibration_points.size())) {
		return;
	}
	const Size2 size = calibration_control->get_size();
	calibration_control->draw_rect(Rect2(Point2(), size), Color(0.03, 0.03, 0.04, 0.92));

	const double now = OS::get_singleton()->get_ticks_usec() / 1000000.0;
	const double t = now - calibration_point_start;
	const Vector2 at = calibration_points[calibration_index].target * size;
	const float unit = MIN(size.x, size.y);
	if (t < CALIBRATION_SETTLE) {
		// A ring closing in on the dot, so the eyes go to it.
		const float k = float(t / CALIBRATION_SETTLE);
		calibration_control->draw_arc(at, Math::lerp(unit * 0.08f, unit * 0.015f, k), 0, Math::TAU, 48, Color(1, 1, 1, 0.8), 2.0f, true);
		calibration_control->draw_circle(at, unit * 0.008f, Color(1, 1, 1));
	} else {
		const float k = float((t - CALIBRATION_SETTLE) / CALIBRATION_COLLECT);
		calibration_control->draw_circle(at, unit * 0.012f, Color(1, 0.35, 0.3));
		calibration_control->draw_arc(at, unit * 0.022f, -Math::PI * 0.5f, -Math::PI * 0.5f + Math::TAU * k, 48, Color(1, 0.35, 0.3), 3.0f, true);
	}

	Ref<Font> font = calibration_control->get_theme_default_font();
	if (font.is_valid()) {
		const int font_size = MAX(14, int(unit * 0.025f));
		int dots = 0;
		int checks = 0;
		for (const CalibrationPoint &point : calibration_points) {
			(point.check ? checks : dots)++;
		}
		const String text = calibration_points[calibration_index].check
				? vformat("Checking accuracy (%d of %d). Look at the dot.", calibration_index - dots + 1, checks)
				: vformat("Look at the dot (%d of %d). Keep your head still. Esc cancels.", calibration_index + 1, dots);
		calibration_control->draw_string(font, Point2(0, size.y * 0.5f + unit * 0.12f), text, HORIZONTAL_ALIGNMENT_CENTER, size.x, font_size, Color(1, 1, 1, 0.75));
	}
}

void EyeTracker::_draw_debug() {
	if (!debug_overlay || !debug_control) {
		return;
	}
	const Size2 size = debug_control->get_size();
	const float unit = MIN(size.x, size.y);

	// The gaze, and how close to it counts as "looked at" for GazeTarget3D.
	const Vector2 at = gaze_uv * size;
	const Color gaze_colour = eyes_closed ? Color(0.5, 0.5, 0.5, 0.8) : (looking ? Color(0.3, 1, 0.5, 0.9) : Color(1, 0.4, 0.3, 0.9));
	debug_control->draw_arc(at, unit * 0.03f, 0, Math::TAU, 32, gaze_colour, 2.0f, true);
	debug_control->draw_circle(at, 3.0f, gaze_colour);

	// The camera picture with what was found on it, top left.
	const float preview_width = MIN(320.0f, size.x * 0.3f);
	Rect2 preview;
	if (source == SOURCE_WEBCAM && feed.is_valid() && current.observation.frame_size.x > 0) {
		const Size2i frame = current.observation.frame_size;
		preview = Rect2(Point2(10, 10), Size2(preview_width, preview_width * frame.y / frame.x));
		// Drawn straight from the feed's texture, mirrored like the measurements.
		if (mirror_camera) {
			debug_control->draw_set_transform(Vector2(preview.position.x + preview.size.x, preview.position.y), 0.0, Vector2(-1, 1));
		} else {
			debug_control->draw_set_transform(preview.position);
		}
		RenderingServer::get_singleton()->canvas_item_add_texture_rect(debug_control->get_canvas_item(), Rect2(Vector2(), preview.size), feed->get_texture(CameraServer::FEED_RGBA_IMAGE));
		debug_control->draw_set_transform(Vector2());
		const Vector2 scale = preview.size / Vector2(frame);
		auto to_preview = [&](const Vector2 &p) { return preview.position + p * scale; };
		const FaceObservation &face = current.observation;
		if (face.found) {
			debug_control->draw_rect(Rect2(to_preview(face.box.position), face.box.size * scale), Color(0.3, 1, 0.5, 0.8), false, 1.0f);
			for (const Vector3 &p : face.mesh) {
				debug_control->draw_rect(Rect2(to_preview(Vector2(p.x, p.y)) - Vector2(0.5, 0.5), Vector2(1, 1)), Color(0.4, 1, 0.6, 0.55));
			}
			for (int e = 0; e < 2; e++) {
				const EyeObservation &eye = face.eyes[e];
				if (!eye.valid) {
					continue;
				}
				for (int k = 0; k < 16; k++) {
					debug_control->draw_circle(to_preview(eye.contour[k]), 1.0f, Color(0.3, 0.9, 1));
				}
				debug_control->draw_arc(to_preview(eye.iris), MAX(eye.iris_radius * scale.x, 1.5f), 0, Math::TAU, 16, Color(1, 0.3, 0.3), 1.0f);
			}
		}
	} else {
		preview = Rect2(Point2(10, 10), Size2(preview_width, 0));
	}

	Ref<Font> font = debug_control->get_theme_default_font();
	if (font.is_null()) {
		return;
	}
	static const char *status_names[] = { "Stopped", "No camera", "Starting", "Searching for a face", "Tracking", "Simulated" };
	static const char *source_names[] = { "Webcam", "Mouse", "Screen centre" };
	const int font_size = 14;
	Point2 line = Point2(10, preview.position.y + preview.size.y + 20);
	auto print = [&](const String &p_text) {
		debug_control->draw_string(font, line + Vector2(1, 1), p_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(0, 0, 0, 0.8));
		debug_control->draw_string(font, line, p_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(1, 1, 1));
		line.y += font_size + 4;
	};
	print(vformat("Eye tracker: %s (%s)%s", status_names[status], source_names[source], model.is_valid() ? ", calibrated" : ""));
	print(vformat("Gaze %.2f, %.2f  %s", gaze_uv.x, gaze_uv.y, looking ? "looking at the screen" : "looking away"));
	print(vformat("Eyes %s  left %d%%  right %d%%", eyes_closed ? "CLOSED" : "open", int(openness[0] * 100), int(openness[1] * 100)));
	if (source == SOURCE_WEBCAM && feed.is_valid()) {
		const Vector3 head = get_head_rotation();
		print(vformat("Head %d, %d, %d deg  |  %.1f ms a frame, %d a second%s", int(head.x), int(head.y), int(head.z), current.process_ms, int(tracking_rate), current.meshed ? "" : "  (no face mesh)"));
		if (accuracy.get("measured", false)) {
			print(vformat("Accuracy: %.1f cm (%.1f degrees) off, %.1f cm jitter", float(accuracy["error_cm"]), float(accuracy["error_degrees"]), float(accuracy["jitter_cm"])));
		}
	}
}

// --- Still pictures -----------------------------------------------------------

Dictionary EyeTracker::analyze_image(const Ref<Image> &p_image) {
	Dictionary result;
	result["found"] = false;
	ERR_FAIL_COND_V(p_image.is_null() || p_image->is_empty(), result);
	Ref<Image> image = p_image->duplicate();
	if (image->is_compressed()) {
		image->decompress();
	}
	image->convert(Image::FORMAT_RGB8);

	// Its own tracker: the camera thread's may be busy.
	FaceTracker tracker;
	ERR_FAIL_COND_V(tracker.initialize() != OK, result);
	FaceObservation face;
	const uint64_t begin = OS::get_singleton()->get_ticks_usec();
	const Vector<uint8_t> data = image->get_data();
	const bool found = tracker.process(data.ptr(), image->get_width(), image->get_height(), Image::FORMAT_RGB8, false, face);
	result["time_ms"] = (OS::get_singleton()->get_ticks_usec() - begin) / 1000.0;
	result["found"] = found;
	if (!found) {
		return result;
	}
	result["score"] = face.score;
	result["meshed"] = face.meshed;
	result["box"] = face.box;
	PackedVector3Array mesh;
	for (const Vector3 &p : face.mesh) {
		mesh.push_back(p);
	}
	result["mesh"] = mesh;
	PackedVector2Array keypoints;
	for (int k = 0; k < 6; k++) {
		keypoints.push_back(face.keypoints[k]);
	}
	result["keypoints"] = keypoints;
	result["yaw"] = face.yaw;
	result["pitch"] = face.pitch;
	result["roll"] = face.roll;
	Array eyes;
	for (int e = 0; e < 2; e++) {
		const EyeObservation &eye = face.eyes[e];
		Dictionary d;
		d["valid"] = eye.valid;
		d["iris"] = eye.iris;
		d["iris_radius"] = eye.iris_radius;
		d["iris_offset"] = eye.iris_offset;
		d["openness"] = eye.openness;
		d["upper_lid"] = eye.upper_lid;
		PackedVector2Array corners;
		corners.push_back(eye.corners[0]);
		corners.push_back(eye.corners[1]);
		d["corners"] = corners;
		PackedVector2Array contour;
		for (int k = 0; k < 16; k++) {
			contour.push_back(eye.contour[k]);
		}
		d["contour"] = contour;
		eyes.push_back(d);
	}
	result["eyes"] = eyes;
	return result;
}
