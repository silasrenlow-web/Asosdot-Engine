/**************************************************************************/
/*  gaze_model.h                                                          */
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

#include "core/math/vector2.h"
#include "core/templates/local_vector.h"
#include "core/variant/variant.h"

// From what the camera measured of the eyes and head to a point on the
// screen. The features are the eyes' turn (iris offsets, upper lid), the
// head's turn and place, and the iris offsets squared (the eye is a ball, so
// the iris moves less towards the edges). A calibration fits them to the dots
// the player looked at with ridge regression, choosing how strongly to smooth
// the fit by leaving each dot out in turn and checking how well it is
// predicted by the others.
class GazeModel {
public:
	enum Feature {
		FEATURE_BIAS,
		FEATURE_IRIS_X,
		FEATURE_IRIS_Y,
		FEATURE_LID,
		FEATURE_YAW,
		FEATURE_PITCH,
		FEATURE_FACE_X,
		FEATURE_FACE_Y,
		FEATURE_FACE_SIZE,
		FEATURE_IRIS_XX,
		FEATURE_IRIS_YY,
		FEATURE_IRIS_XY,
		FEATURE_COUNT,
	};

	struct Measurement {
		Vector2 iris; // Both eyes' iris offsets, averaged, in eye widths.
		float upper_lid = 0.0;
		float yaw = 0.0; // Radians.
		float pitch = 0.0;
		Vector2 face; // 0 to 1 in the frame.
		float face_size = 0.0; // Eye distance over the frame's width.
	};

	static void make_features(const Measurement &p_measurement, float r_features[FEATURE_COUNT]);

	// p_rows holds FEATURE_COUNT floats per sample; p_groups says which dot each
	// sample was taken at (every dot counts the same, however many samples it
	// gave). Returns false when the samples cannot pin a fit down. r_cv_error is
	// the leave-one-dot-out error, in screen units (0 to 1).
	bool fit(const LocalVector<float> &p_rows, const LocalVector<Vector2> &p_targets, const LocalVector<int> &p_groups, float &r_cv_error);
	Vector2 map(const float p_features[FEATURE_COUNT]) const;

	bool is_valid() const { return valid; }
	void clear() { valid = false; }
	PackedFloat32Array save() const;
	bool load(const PackedFloat32Array &p_data);

private:
	bool valid = false;
	float mean[FEATURE_COUNT] = {};
	float scale[FEATURE_COUNT] = {};
	float weights_x[FEATURE_COUNT] = {};
	float weights_y[FEATURE_COUNT] = {};
	// The range each feature had during calibration: a measurement far outside
	// it (a slipped iris, an odd head turn) is held near it, not extrapolated.
	float lowest[FEATURE_COUNT] = {};
	float highest[FEATURE_COUNT] = {};

	static bool _solve(const LocalVector<float> &p_standard, const LocalVector<Vector2> &p_targets, const LocalVector<float> &p_sample_weights, int p_left_out_group, const LocalVector<int> &p_groups, float p_lambda, float r_x[FEATURE_COUNT], float r_y[FEATURE_COUNT]);
};
