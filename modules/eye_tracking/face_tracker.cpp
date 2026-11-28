/**************************************************************************/
/*  face_tracker.cpp                                                      */
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

#include "face_tracker.h"

#include "eye_tracking_models.gen.h"

#include "core/object/worker_thread_pool.h"

namespace {

// BlazeFace (short range) looks at a 128 by 128 picture and answers for 896
// anchors: a 16 by 16 grid with 2 anchors each, then an 8 by 8 grid with 6.
const int FACE_INPUT = 128;
const int MESH_INPUT = 192;
const int IRIS_INPUT = 64;
const int MESH_POINTS = 468;
const float FACE_MIN_SCORE = 0.5f;
const float MESH_MIN_SCORE = 0.5f;
// While following a face, the face detector still looks every this many
// frames (and whenever the mesh is unsure), so a mesh that lost the face in a
// fast move is put right within a few frames.
const int DETECT_EVERY = 8;
const float MESH_SURE_SCORE = 0.9f;
// The face mesh's square is the face's size times this, as MediaPipe crops it.
const float MESH_SQUARE = 1.5f;
// The iris model's square is the eye's width (corner to corner) times this.
const float EYE_SQUARE = 2.3f;
// Without the mesh, the eye's square from the distance between the eyes.
const float EYE_SQUARE_FROM_DETECTION = 1.0f;

// Face mesh points used here (MediaPipe's numbering).
const int MESH_EYE_A_OUTER = 33;
const int MESH_EYE_A_INNER = 133;
const int MESH_EYE_B_INNER = 362;
const int MESH_EYE_B_OUTER = 263;
const int MESH_FOREHEAD = 10;
const int MESH_CHIN = 152;
const int MESH_SIDE_A = 234;
const int MESH_SIDE_B = 454;
const int MESH_NOSE_TIP = 1;
const int MESH_MOUTH = 13;

float sigmoid(float p_x) {
	return 1.0f / (1.0f + Math::exp(-CLAMP(p_x, -80.0f, 80.0f)));
}

} // namespace

Error FaceTracker::initialize() {
	if (ready) {
		return OK;
	}
	Error err = face_model.load(_eye_model_face_detection_short_range, _eye_model_face_detection_short_range_size);
	ERR_FAIL_COND_V_MSG(err != OK, err, "Eye tracking: the face model did not load.");
	err = mesh_model.load(_eye_model_face_landmark, _eye_model_face_landmark_size);
	ERR_FAIL_COND_V_MSG(err != OK, err, "Eye tracking: the face mesh model did not load.");
	for (TFLiteModel &iris_model : iris_models) {
		err = iris_model.load(_eye_model_iris_landmark, _eye_model_iris_landmark_size);
		ERR_FAIL_COND_V_MSG(err != OK, err, "Eye tracking: the iris model did not load.");
	}

	const Vector<int> face_in = face_model.get_input_shape(0);
	const Vector<int> mesh_in = mesh_model.get_input_shape(0);
	const Vector<int> iris_in = iris_models[0].get_input_shape(0);
	ERR_FAIL_COND_V(face_in.size() != 4 || face_in[1] != FACE_INPUT || face_in[3] != 3, ERR_INVALID_DATA);
	ERR_FAIL_COND_V(mesh_in.size() != 4 || mesh_in[1] != MESH_INPUT || mesh_in[3] != 3, ERR_INVALID_DATA);
	ERR_FAIL_COND_V(iris_in.size() != 4 || iris_in[1] != IRIS_INPUT || iris_in[3] != 3, ERR_INVALID_DATA);
	ERR_FAIL_COND_V(face_model.get_output_count() != 2 || iris_models[0].get_output_count() != 2 || mesh_model.get_output_count() != 2, ERR_INVALID_DATA);
	ERR_FAIL_COND_V(mesh_model.get_output_size(0) < MESH_POINTS * 3, ERR_INVALID_DATA);

	anchors.clear();
	const int layers[2][2] = { { 16, 2 }, { 8, 6 } };
	for (const auto &layer : layers) {
		const int grid = layer[0];
		for (int y = 0; y < grid; y++) {
			for (int x = 0; x < grid; x++) {
				for (int k = 0; k < layer[1]; k++) {
					anchors.push_back(Vector2((x + 0.5f) / grid, (y + 0.5f) / grid));
				}
			}
		}
	}
	ERR_FAIL_COND_V(int(anchors.size()) * 16 != face_model.get_output_size(0), ERR_INVALID_DATA);
	ready = true;
	return OK;
}

Color FaceTracker::_sample(float p_x, float p_y) const {
	// Bilinear, clamped to the edge; colours 0 to 1.
	float x = CLAMP(p_x - 0.5f, 0.0f, float(frame_width - 1));
	const float y = CLAMP(p_y - 0.5f, 0.0f, float(frame_height - 1));
	if (frame_mirror) {
		x = float(frame_width - 1) - x;
	}
	const int x0 = int(x);
	const int y0 = int(y);
	const int x1 = MIN(x0 + 1, frame_width - 1);
	const int y1 = MIN(y0 + 1, frame_height - 1);
	const float fx = x - x0;
	const float fy = y - y0;
	const int c = frame_channels;
	const uint8_t *p00 = frame + (size_t(y0) * frame_width + x0) * c;
	const uint8_t *p10 = frame + (size_t(y0) * frame_width + x1) * c;
	const uint8_t *p01 = frame + (size_t(y1) * frame_width + x0) * c;
	const uint8_t *p11 = frame + (size_t(y1) * frame_width + x1) * c;
	float rgb[3];
	for (int k = 0; k < 3; k++) {
		const int ch = c >= 3 ? k : 0;
		const float top = p00[ch] + (p10[ch] - p00[ch]) * fx;
		const float bottom = p01[ch] + (p11[ch] - p01[ch]) * fx;
		rgb[k] = (top + (bottom - top) * fy) * (1.0f / 255.0f);
	}
	return Color(rgb[0], rgb[1], rgb[2]);
}

void FaceTracker::_crop(float *r_input, int p_size, const Vector2 &p_centre, float p_side, float p_angle, bool p_flip, int p_supersample, float p_scale, float p_bias) const {
	// A square of the frame, turned by p_angle, into the model's input: each
	// input pixel the average of a grid of samples over the area it covers.
	const Vector2 axis_x = Vector2(Math::cos(p_angle), Math::sin(p_angle));
	const Vector2 axis_y = Vector2(-axis_x.y, axis_x.x);
	const float pixel = p_side / p_size;
	const int n = MAX(p_supersample, 1);
	const float weight = p_scale / float(n * n);
	for (int y = 0; y < p_size; y++) {
		for (int x = 0; x < p_size; x++) {
			float r = 0, g = 0, b = 0;
			for (int sy = 0; sy < n; sy++) {
				const float v = (y + (sy + 0.5f) / n) - p_size * 0.5f;
				for (int sx = 0; sx < n; sx++) {
					float u = (x + (sx + 0.5f) / n) - p_size * 0.5f;
					if (p_flip) {
						u = -u;
					}
					const Vector2 at = p_centre + (axis_x * u + axis_y * v) * pixel;
					const Color c = _sample(at.x, at.y);
					r += c.r;
					g += c.g;
					b += c.b;
				}
			}
			float *p = r_input + (y * p_size + x) * 3;
			p[0] = r * weight + p_bias;
			p[1] = g * weight + p_bias;
			p[2] = b * weight + p_bias;
		}
	}
}

bool FaceTracker::process(const uint8_t *p_data, int p_width, int p_height, Image::Format p_format, bool p_mirror, FaceObservation &r_face) {
	r_face = FaceObservation();
	ERR_FAIL_COND_V(!ready, false);
	ERR_FAIL_COND_V(p_data == nullptr || p_width < 16 || p_height < 16, false);
	switch (p_format) {
		case Image::FORMAT_RGB8:
			frame_channels = 3;
			break;
		case Image::FORMAT_RGBA8:
			frame_channels = 4;
			break;
		case Image::FORMAT_L8:
		case Image::FORMAT_R8:
			frame_channels = 1;
			break;
		default:
			ERR_FAIL_V_MSG(false, "Eye tracking: this camera's frame format is not supported.");
	}
	frame = p_data;
	frame_width = p_width;
	frame_height = p_height;
	frame_mirror = p_mirror;
	r_face.frame_size = Size2i(p_width, p_height);

	// Follow the face from last frame with the mesh alone; look for it again
	// with the face detector when it was lost.
	bool meshed = tracking && _run_mesh(track_centre, track_side, track_angle, r_face);
	if (meshed) {
		// The head moved quickly: the face is well off the middle of last
		// frame's square, and the mesh is less sure of it there. Measure again,
		// in a square around where it is now, so it catches up at once.
		Vector2 centre;
		float side = 0.0f;
		float angle = 0.0f;
		_square_from_mesh(r_face.mesh, centre, side, angle);
		const float moved = centre.distance_to(track_centre) / MAX(track_side, 1.0f);
		const float resized = Math::abs(side / MAX(track_side, 1.0f) - 1.0f);
		if (moved > 0.06f || resized > 0.15f || Math::abs(Math::angle_difference(angle, track_angle)) > 0.15f) {
			FaceObservation again = r_face;
			if (_run_mesh(centre, side, angle, again)) {
				r_face = again;
			}
		}
	}
	frames_since_detection++;
	if (meshed && (frames_since_detection >= DETECT_EVERY || r_face.score < MESH_SURE_SCORE)) {
		frames_since_detection = 0;
		FaceObservation detected;
		if (_detect_face(detected)) {
			Vector2 centre;
			float side = 0.0f;
			float angle = 0.0f;
			_square_from_mesh(r_face.mesh, centre, side, angle);
			const float face_size = side / MESH_SQUARE;
			if (detected.box.get_center().distance_to(centre) > face_size * 0.2f) {
				// Not where the mesh thinks it is: start again from the detector.
				const Vector2 eye_line = detected.keypoints[1] - detected.keypoints[0];
				FaceObservation again;
				again.frame_size = r_face.frame_size;
				if (_run_mesh(detected.box.get_center(), MAX(detected.box.size.x, detected.box.size.y) * MESH_SQUARE, Math::atan2(eye_line.y, eye_line.x), again)) {
					r_face = again;
				}
			}
		}
	}
	if (!meshed) {
		tracking = false;
		frames_since_detection = 0;
		FaceObservation detected = r_face;
		if (_detect_face(detected)) {
			const Vector2 eye_line = detected.keypoints[1] - detected.keypoints[0];
			const float angle = Math::atan2(eye_line.y, eye_line.x);
			const float side = MAX(detected.box.size.x, detected.box.size.y) * MESH_SQUARE;
			meshed = _run_mesh(detected.box.get_center(), side, angle, r_face);
			if (!meshed) {
				r_face = detected;
			}
		}
	}

	if (meshed) {
		const LocalVector<Vector3> &m = r_face.mesh;
		// The eyes' squares from their corners: tight, and turned with each eye.
		Vector2 corners[2][2] = {
			{ Vector2(m[MESH_EYE_A_OUTER].x, m[MESH_EYE_A_OUTER].y), Vector2(m[MESH_EYE_A_INNER].x, m[MESH_EYE_A_INNER].y) },
			{ Vector2(m[MESH_EYE_B_INNER].x, m[MESH_EYE_B_INNER].y), Vector2(m[MESH_EYE_B_OUTER].x, m[MESH_EYE_B_OUTER].y) },
		};
		for (auto &pair : corners) {
			if (pair[0].x > pair[1].x) {
				SWAP(pair[0], pair[1]);
			}
		}
		if ((corners[0][0] + corners[0][1]).x > (corners[1][0] + corners[1][1]).x) {
			SWAP(corners[0][0], corners[1][0]);
			SWAP(corners[0][1], corners[1][1]);
		}
		Vector2 centres[2];
		float sides[2];
		float angles[2];
		for (int e = 0; e < 2; e++) {
			const Vector2 line = corners[e][1] - corners[e][0];
			centres[e] = (corners[e][0] + corners[e][1]) * 0.5f;
			sides[e] = MAX(line.length() * EYE_SQUARE, 8.0f);
			angles[e] = Math::atan2(line.y, line.x);
		}
		_measure_eyes(centres, sides, angles, r_face);

		// Head pose from the mesh: across the face (eye corners and cheeks),
		// and up it (chin to forehead), in 3D.
		Vector3 across = (m[MESH_EYE_B_OUTER] - m[MESH_EYE_A_OUTER]) + (m[MESH_SIDE_B] - m[MESH_SIDE_A]);
		if (across.x < 0.0f) {
			across = -across;
		}
		const Vector3 up_raw = m[MESH_FOREHEAD] - m[MESH_CHIN];
		const Vector3 x_axis = across.normalized();
		const Vector3 z_axis = x_axis.cross(up_raw).normalized(); // Out of the face, towards the camera.
		const Vector3 y_axis = z_axis.cross(x_axis).normalized(); // Up the face.
		r_face.yaw = Math::atan2(z_axis.x, -z_axis.z);
		r_face.pitch = Math::atan2(z_axis.y, -z_axis.z);
		r_face.roll = Math::atan2(x_axis.y, x_axis.x);
		(void)y_axis;
		// Where the head is and how big it looks, from all of the mesh: steadier
		// than from the eyes, one of which may be half hidden by the nose.
		Vector2 mean;
		for (const Vector3 &p : m) {
			mean += Vector2(p.x, p.y);
		}
		mean /= float(m.size());
		r_face.head_centre = mean;
		// The outer eye corners are about 9 cm apart; as wide as the eyes' centres
		// (6.3 cm) would be, so the distance estimate stays the same.
		r_face.eye_distance = Vector2(m[MESH_EYE_A_OUTER].x, m[MESH_EYE_A_OUTER].y).distance_to(Vector2(m[MESH_EYE_B_OUTER].x, m[MESH_EYE_B_OUTER].y)) * (6.3f / 9.0f);

		// The detector's six points, from the mesh, for whoever reads them.
		r_face.keypoints[0] = centres[0];
		r_face.keypoints[1] = centres[1];
		r_face.keypoints[2] = Vector2(m[MESH_NOSE_TIP].x, m[MESH_NOSE_TIP].y);
		r_face.keypoints[3] = Vector2(m[MESH_MOUTH].x, m[MESH_MOUTH].y);
		const bool a_left = m[MESH_SIDE_A].x < m[MESH_SIDE_B].x;
		r_face.keypoints[4] = Vector2(m[a_left ? MESH_SIDE_A : MESH_SIDE_B].x, m[a_left ? MESH_SIDE_A : MESH_SIDE_B].y);
		r_face.keypoints[5] = Vector2(m[a_left ? MESH_SIDE_B : MESH_SIDE_A].x, m[a_left ? MESH_SIDE_B : MESH_SIDE_A].y);

		// Next frame's square.
		_square_from_mesh(m, track_centre, track_side, track_angle);
		tracking = r_face.eyes[0].valid || r_face.eyes[1].valid;
		r_face.box = Rect2(track_centre - Vector2(track_side, track_side) / (2.0f * MESH_SQUARE), Vector2(track_side, track_side) / MESH_SQUARE);
	} else if (r_face.found) {
		// The face detector found a face the mesh could not measure: the older,
		// rougher way, from its six points.
		const Vector2 eye_line = r_face.keypoints[1] - r_face.keypoints[0];
		const float eye_distance = eye_line.length();
		r_face.roll = Math::atan2(eye_line.y, eye_line.x);
		Vector2 centres[2] = { r_face.keypoints[0], r_face.keypoints[1] };
		const float side = eye_distance * EYE_SQUARE_FROM_DETECTION;
		float sides[2] = { side, side };
		float angles[2] = { r_face.roll, r_face.roll };
		_measure_eyes(centres, sides, angles, r_face);

		const Vector2 mid_eyes = (r_face.keypoints[0] + r_face.keypoints[1]) * 0.5f;
		const Vector2 nose = (r_face.keypoints[2] - mid_eyes).rotated(-r_face.roll);
		const Vector2 ear_left = (r_face.keypoints[4] - mid_eyes).rotated(-r_face.roll);
		const Vector2 ear_right = (r_face.keypoints[5] - mid_eyes).rotated(-r_face.roll);
		const float ear_distance = MAX(ear_right.x - ear_left.x, 1.0f);
		// Roughly radians for small turns.
		r_face.yaw = 2.0f * (nose.x - (ear_left.x + ear_right.x) * 0.5f) / ear_distance;
		r_face.pitch = 1.5f * (nose.y / MAX(eye_distance, 1.0f) - 0.55f);
		r_face.head_centre = mid_eyes;
		r_face.eye_distance = eye_distance;
	}
	frame = nullptr;
	return r_face.found;
}

void FaceTracker::_square_from_mesh(const LocalVector<Vector3> &p_mesh, Vector2 &r_centre, float &r_side, float &r_angle) {
	// Around every point of the mesh, turned with the eye line.
	Vector3 across = p_mesh[MESH_EYE_B_OUTER] - p_mesh[MESH_EYE_A_OUTER];
	if (across.x < 0.0f) {
		across = -across;
	}
	r_angle = Math::atan2(across.y, across.x);
	const Vector2 axis_x = Vector2(Math::cos(r_angle), Math::sin(r_angle));
	const Vector2 axis_y = Vector2(-axis_x.y, axis_x.x);
	Vector2 low = Vector2(1e10, 1e10);
	Vector2 high = Vector2(-1e10, -1e10);
	for (const Vector3 &p : p_mesh) {
		const Vector2 q = Vector2(axis_x.dot(Vector2(p.x, p.y)), axis_y.dot(Vector2(p.x, p.y)));
		low = low.min(q);
		high = high.max(q);
	}
	const Vector2 mid = (low + high) * 0.5f;
	r_centre = axis_x * mid.x + axis_y * mid.y;
	r_side = MAX(high.x - low.x, high.y - low.y) * MESH_SQUARE;
}

bool FaceTracker::_run_mesh(const Vector2 &p_centre, float p_side, float p_angle, FaceObservation &r_face) {
	if (p_side < 16.0f) {
		return false;
	}
	const int supersample = CLAMP(int(Math::ceil(p_side / MESH_INPUT)), 1, 4);
	_crop(mesh_model.get_input(0), MESH_INPUT, p_centre, p_side, p_angle, false, supersample, 1.0f, 0.0f);
	mesh_model.run();
	const float score = sigmoid(mesh_model.get_output(1)[0]);
	if (score < MESH_MIN_SCORE) {
		return false;
	}

	const float *points = mesh_model.get_output(0);
	const Vector2 axis_x = Vector2(Math::cos(p_angle), Math::sin(p_angle));
	const Vector2 axis_y = Vector2(-axis_x.y, axis_x.x);
	const float scale = p_side / MESH_INPUT;
	r_face.mesh.resize(MESH_POINTS);
	for (int i = 0; i < MESH_POINTS; i++) {
		const float *p = points + i * 3;
		const Vector2 d = (Vector2(p[0], p[1]) - Vector2(MESH_INPUT * 0.5f, MESH_INPUT * 0.5f)) * scale;
		const Vector2 at = p_centre + axis_x * d.x + axis_y * d.y;
		r_face.mesh[i] = Vector3(at.x, at.y, p[2] * scale);
	}
	r_face.found = true;
	r_face.meshed = true;
	r_face.score = score;
	return true;
}

bool FaceTracker::_detect_face(FaceObservation &r_face) {
	// The whole frame, letterboxed into the model's square: each input pixel
	// is the average of a 3 by 3 grid of samples over the area it covers.
	const float side = float(MAX(frame_width, frame_height));
	const float pad_x = (side - frame_width) * 0.5f;
	const float pad_y = (side - frame_height) * 0.5f;
	const float step = side / FACE_INPUT;
	float *input = face_model.get_input(0);
	for (int y = 0; y < FACE_INPUT; y++) {
		for (int x = 0; x < FACE_INPUT; x++) {
			float r = 0, g = 0, b = 0;
			int n = 0;
			for (int sy = 0; sy < 3; sy++) {
				const float fy = (y + (sy + 0.5f) / 3.0f) * step - pad_y;
				for (int sx = 0; sx < 3; sx++) {
					const float fx = (x + (sx + 0.5f) / 3.0f) * step - pad_x;
					if (fx < 0 || fy < 0 || fx >= frame_width || fy >= frame_height) {
						continue;
					}
					const Color c = _sample(fx, fy);
					r += c.r;
					g += c.g;
					b += c.b;
					n++;
				}
			}
			float *p = input + (y * FACE_INPUT + x) * 3;
			// The model wants -1 to 1; the black bars are -1 to it.
			if (n > 0) {
				p[0] = r / n * 2.0f - 1.0f;
				p[1] = g / n * 2.0f - 1.0f;
				p[2] = b / n * 2.0f - 1.0f;
			} else {
				p[0] = p[1] = p[2] = -1.0f;
			}
		}
	}
	face_model.run();

	const float *regressors = face_model.get_output(0);
	const float *scores = face_model.get_output(1);
	int best = -1;
	float best_score = FACE_MIN_SCORE;
	for (uint32_t i = 0; i < anchors.size(); i++) {
		const float s = sigmoid(scores[i]);
		if (s > best_score) {
			best_score = s;
			best = int(i);
		}
	}
	if (best < 0) {
		return false;
	}

	// The best anchor and those overlapping it, averaged by score (the way
	// MediaPipe's weighted non-maximum suppression does), for a steadier box.
	auto decode_box = [&](int p_index) {
		const float *r = regressors + p_index * 16;
		const Vector2 centre = Vector2(r[0], r[1]) / float(FACE_INPUT) + anchors[p_index];
		const Vector2 size = Vector2(r[2], r[3]) / float(FACE_INPUT);
		return Rect2(centre - size * 0.5f, size);
	};
	const Rect2 best_box = decode_box(best);
	float total = 0.0f;
	Vector2 box_position, box_size;
	Vector2 points[6];
	for (uint32_t i = 0; i < anchors.size(); i++) {
		const float s = sigmoid(scores[i]);
		if (s < FACE_MIN_SCORE) {
			continue;
		}
		const Rect2 box = decode_box(i);
		const Rect2 overlap = box.intersection(best_box);
		const float union_area = box.get_area() + best_box.get_area() - overlap.get_area();
		if (union_area <= 0.0f || overlap.get_area() / union_area < 0.3f) {
			continue;
		}
		const float *r = regressors + i * 16;
		total += s;
		box_position += box.position * s;
		box_size += box.size * s;
		for (int k = 0; k < 6; k++) {
			points[k] += (Vector2(r[4 + k * 2], r[5 + k * 2]) / float(FACE_INPUT) + anchors[i]) * s;
		}
	}
	if (total <= 0.0f) {
		return false;
	}

	const Vector2 pad(pad_x, pad_y);
	r_face.found = true;
	r_face.score = best_score;
	r_face.box = Rect2(box_position / total * side - pad, box_size / total * side);
	for (int k = 0; k < 6; k++) {
		r_face.keypoints[k] = points[k] / total * side - pad;
	}
	return true;
}

void FaceTracker::_eye_task(void *p_job) {
	EyeJob *job = static_cast<EyeJob *>(p_job);
	job->tracker->_measure_eye(job->tracker->iris_models[job->eye], job->eye, job->centre, job->side, job->angle, *job->result);
}

void FaceTracker::_measure_eyes(const Vector2 p_centres[2], const float p_sides[2], const float p_angles[2], FaceObservation &r_face) {
	// Both eyes at once: the second on a worker thread, each with its own copy
	// of the iris model.
	EyeJob job;
	job.tracker = this;
	job.eye = 1;
	job.centre = p_centres[1];
	job.side = p_sides[1];
	job.angle = p_angles[1];
	job.result = &r_face.eyes[1];
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	WorkerThreadPool::TaskID task = pool ? pool->add_native_task(_eye_task, &job, true, "Eye tracking: iris") : WorkerThreadPool::INVALID_TASK_ID;
	_measure_eye(iris_models[0], 0, p_centres[0], p_sides[0], p_angles[0], r_face.eyes[0]);
	if (task != WorkerThreadPool::INVALID_TASK_ID) {
		pool->wait_for_task_completion(task);
	} else {
		_eye_task(&job);
	}
}

void FaceTracker::_measure_eye(TFLiteModel &p_model, int p_eye, const Vector2 &p_centre, float p_side, float p_angle, EyeObservation &r_eye) const {
	r_eye = EyeObservation();
	// The iris model knows eyes as the frame's left eye looks; the other is
	// read mirrored.
	const bool flip = p_eye == 1;
	const int supersample = CLAMP(int(Math::ceil(p_side / IRIS_INPUT)) + 1, 2, 4);
	_crop(p_model.get_input(0), IRIS_INPUT, p_centre, p_side, p_angle, flip, supersample, 1.0f, 0.0f);
	p_model.run();

	const float *contour = p_model.get_output(0); // 71 points of x, y, z.
	const float *iris = p_model.get_output(1); // 5 points: centre, then the edge.
	if (p_model.get_output_size(0) < 71 * 3 || p_model.get_output_size(1) < 15) {
		return;
	}

	const Vector2 axis_x = Vector2(Math::cos(p_angle), Math::sin(p_angle));
	const Vector2 axis_y = Vector2(-axis_x.y, axis_x.x);
	const float pixel = p_side / IRIS_INPUT;
	// In the square's own pixels, unmirrored: x right, y down, turned with the eye.
	auto local = [&](const float *p_point) {
		float x = p_point[0];
		if (flip) {
			x = IRIS_INPUT - x;
		}
		return Vector2(x, p_point[1]);
	};
	auto to_frame = [&](const Vector2 &p_local) {
		const Vector2 d = p_local - Vector2(IRIS_INPUT * 0.5f, IRIS_INPUT * 0.5f);
		return p_centre + (axis_x * d.x + axis_y * d.y) * pixel;
	};

	Vector2 points[16];
	for (int k = 0; k < 16; k++) {
		points[k] = local(contour + k * 3);
		r_eye.contour[k] = to_frame(points[k]);
	}
	// Points 0 and 8 are the corners; which is left depends on the mirroring.
	Vector2 corner_left = points[0];
	Vector2 corner_right = points[8];
	if (corner_left.x > corner_right.x) {
		SWAP(corner_left, corner_right);
	}
	const float width = corner_left.distance_to(corner_right);
	if (width < 2.0f) {
		return;
	}
	const Vector2 iris_centre = local(iris);
	const Vector2 eye_centre = (corner_left + corner_right) * 0.5f;
	r_eye.iris_offset = (iris_centre - eye_centre) / width;

	// The lids at the middle of the eye: 3-5 on the lower lid, 11-13 on the upper.
	const float lower = (points[3].y + points[4].y + points[5].y) / 3.0f;
	const float upper = (points[11].y + points[12].y + points[13].y) / 3.0f;
	r_eye.openness = MAX(lower - upper, 0.0f) / width;
	r_eye.upper_lid = (upper - eye_centre.y) / width;

	r_eye.iris = to_frame(iris_centre);
	r_eye.iris_radius = local(iris).distance_to(local(iris + 3)) * pixel;
	r_eye.corners[0] = to_frame(corner_left);
	r_eye.corners[1] = to_frame(corner_right);
	r_eye.valid = true;
}
