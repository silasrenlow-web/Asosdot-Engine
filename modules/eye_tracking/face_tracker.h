/**************************************************************************/
/*  face_tracker.h                                                        */
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

#include "tflite_model.h"

#include "core/io/image.h"
#include "core/math/rect2.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"

// What one camera frame shows of a face and its eyes. All positions are in
// pixels of the frame as a mirror shows it (the user's right on the right), so
// "looking right" means right on the screen too.
struct EyeObservation {
	bool valid = false;
	Vector2 iris; // Centre of the iris.
	float iris_radius = 0.0;
	Vector2 corners[2]; // Left and right corner of the eye, in the frame.
	Vector2 contour[16]; // The eyelids: 0-8 lower lid corner to corner, 9-15 upper.
	// Measured in the eye's own square, turned like the eye, in eye widths:
	// where the iris sits from the eye's centre (x right, y down), how far the
	// lids are apart, and where the upper lid is (it follows the eye up and down).
	Vector2 iris_offset;
	float openness = 0.0;
	float upper_lid = 0.0;
};

struct FaceObservation {
	bool found = false;
	float score = 0.0;
	// True when the face mesh measured the face; false when only the face
	// detector did (then the head angles are rough).
	bool meshed = false;
	Rect2 box;
	// Eye, eye, nose tip, mouth, ear, ear (left to right as the frame shows them).
	Vector2 keypoints[6];
	// The 468 face mesh points (x, y in frame pixels, z depth in the same units,
	// smaller is nearer). Empty without the mesh.
	LocalVector<Vector3> mesh;
	// Head turn in radians: yaw (turned right is positive, as the frame shows
	// it), pitch (down is positive) and roll (the eye line's tilt).
	float yaw = 0.0;
	float pitch = 0.0;
	float roll = 0.0;
	// Where the head is: the point between the eyes, and the eye distance, in
	// frame pixels.
	Vector2 head_centre;
	float eye_distance = 0.0;
	EyeObservation eyes[2]; // The eye on the frame's left, then right.
	Size2i frame_size;
};

// Finds a face and measures its eyes in camera frames, with MediaPipe's
// models: BlazeFace to find a face, the face mesh to follow it from frame to
// frame and give the head's pose and the eyes' corners, and the iris model on
// a tight square around each eye. Frames never leave this object: it keeps no
// copy of them, only the models' working memory.
class FaceTracker {
	TFLiteModel face_model;
	TFLiteModel mesh_model;
	TFLiteModel iris_models[2]; // One per eye, so both eyes run at once.
	LocalVector<Vector2> anchors;
	bool ready = false;

	// The frame being processed, as handed in.
	const uint8_t *frame = nullptr;
	int frame_width = 0;
	int frame_height = 0;
	int frame_channels = 3;
	bool frame_mirror = true;

	// Where the face was last frame, for the face mesh's square this frame.
	bool tracking = false;
	Vector2 track_centre;
	float track_side = 0.0;
	float track_angle = 0.0;
	int frames_since_detection = 0;

	struct EyeJob {
		FaceTracker *tracker = nullptr;
		int eye = 0;
		Vector2 centre;
		float side = 0.0;
		float angle = 0.0;
		EyeObservation *result = nullptr;
	};

	Color _sample(float p_x, float p_y) const;
	void _crop(float *r_input, int p_size, const Vector2 &p_centre, float p_side, float p_angle, bool p_flip, int p_supersample, float p_scale, float p_bias) const;
	bool _detect_face(FaceObservation &r_face);
	bool _run_mesh(const Vector2 &p_centre, float p_side, float p_angle, FaceObservation &r_face);
	static void _square_from_mesh(const LocalVector<Vector3> &p_mesh, Vector2 &r_centre, float &r_side, float &r_angle);
	void _measure_eye(TFLiteModel &p_model, int p_eye, const Vector2 &p_centre, float p_side, float p_angle, EyeObservation &r_eye) const;
	static void _eye_task(void *p_job);
	void _measure_eyes(const Vector2 p_centres[2], const float p_sides[2], const float p_angles[2], FaceObservation &r_face);

public:
	Error initialize();
	bool is_ready() const { return ready; }

	// Accepts RGB8, RGBA8, L8 and R8 frames. With p_mirror the frame is read
	// mirrored, which is how a front camera should be shown to the person in it.
	bool process(const uint8_t *p_data, int p_width, int p_height, Image::Format p_format, bool p_mirror, FaceObservation &r_face);
	void reset_tracking() { tracking = false; }
};
