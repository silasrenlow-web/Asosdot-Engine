/**************************************************************************/
/*  gaze_model.cpp                                                        */
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

#include "gaze_model.h"

#include "core/math/math_funcs.h"

namespace {

const float FORMAT_VERSION = 3.0f;
// How far past the calibration's range a feature may go, as a part of that range.
const float RANGE_MARGIN = 0.35f;

// The least spread a feature is given when standardised: a feature that
// hardly moved during calibration (the head, when the player sat still) is
// not blown up into noise.
const float SPREAD_FLOOR[GazeModel::FEATURE_COUNT] = {
	1.0f, // bias
	0.004f, // iris x
	0.004f, // iris y
	0.004f, // upper lid
	0.01f, // yaw
	0.01f, // pitch
	0.005f, // face x
	0.005f, // face y
	0.002f, // face size
	0.0002f, // iris x squared
	0.0002f, // iris y squared
	0.0002f, // iris x * y
};

const float LAMBDAS[] = { 0.001f, 0.003f, 0.01f, 0.03f, 0.1f, 0.3f, 1.0f, 3.0f, 10.0f };

} // namespace

void GazeModel::make_features(const Measurement &p_measurement, float r_features[FEATURE_COUNT]) {
	const Vector2 iris = p_measurement.iris;
	r_features[FEATURE_BIAS] = 1.0f;
	r_features[FEATURE_IRIS_X] = iris.x;
	r_features[FEATURE_IRIS_Y] = iris.y;
	r_features[FEATURE_LID] = p_measurement.upper_lid;
	r_features[FEATURE_YAW] = p_measurement.yaw;
	r_features[FEATURE_PITCH] = p_measurement.pitch;
	r_features[FEATURE_FACE_X] = p_measurement.face.x;
	r_features[FEATURE_FACE_Y] = p_measurement.face.y;
	r_features[FEATURE_FACE_SIZE] = p_measurement.face_size;
	r_features[FEATURE_IRIS_XX] = iris.x * iris.x;
	r_features[FEATURE_IRIS_YY] = iris.y * iris.y;
	r_features[FEATURE_IRIS_XY] = iris.x * iris.y;
}

bool GazeModel::_solve(const LocalVector<float> &p_standard, const LocalVector<Vector2> &p_targets, const LocalVector<float> &p_sample_weights, int p_left_out_group, const LocalVector<int> &p_groups, float p_lambda, float r_x[FEATURE_COUNT], float r_y[FEATURE_COUNT]) {
	const int n = p_targets.size();
	double a[FEATURE_COUNT][FEATURE_COUNT] = {};
	double bx[FEATURE_COUNT] = {};
	double by[FEATURE_COUNT] = {};
	double total_weight = 0.0;
	for (int s = 0; s < n; s++) {
		if (p_groups[s] == p_left_out_group) {
			continue;
		}
		const float w = p_sample_weights[s];
		const float *row = p_standard.ptr() + s * FEATURE_COUNT;
		for (int i = 0; i < FEATURE_COUNT; i++) {
			for (int j = i; j < FEATURE_COUNT; j++) {
				a[i][j] += w * row[i] * row[j];
			}
			bx[i] += w * row[i] * p_targets[s].x;
			by[i] += w * row[i] * p_targets[s].y;
		}
		total_weight += w;
	}
	if (total_weight <= 0.0) {
		return false;
	}
	for (int i = 0; i < FEATURE_COUNT; i++) {
		for (int j = 0; j < i; j++) {
			a[i][j] = a[j][i];
		}
		if (i != FEATURE_BIAS) {
			a[i][i] += p_lambda * total_weight;
		}
	}

	// Gaussian elimination with partial pivoting, both right-hand sides at once.
	double m[FEATURE_COUNT][FEATURE_COUNT + 2];
	for (int i = 0; i < FEATURE_COUNT; i++) {
		for (int j = 0; j < FEATURE_COUNT; j++) {
			m[i][j] = a[i][j];
		}
		m[i][FEATURE_COUNT] = bx[i];
		m[i][FEATURE_COUNT + 1] = by[i];
	}
	for (int c = 0; c < FEATURE_COUNT; c++) {
		int pivot = c;
		for (int r = c + 1; r < FEATURE_COUNT; r++) {
			if (Math::abs(m[r][c]) > Math::abs(m[pivot][c])) {
				pivot = r;
			}
		}
		if (Math::abs(m[pivot][c]) < 1e-12) {
			return false;
		}
		for (int j = 0; j < FEATURE_COUNT + 2; j++) {
			SWAP(m[c][j], m[pivot][j]);
		}
		for (int r = 0; r < FEATURE_COUNT; r++) {
			if (r == c) {
				continue;
			}
			const double f = m[r][c] / m[c][c];
			for (int j = c; j < FEATURE_COUNT + 2; j++) {
				m[r][j] -= f * m[c][j];
			}
		}
	}
	for (int i = 0; i < FEATURE_COUNT; i++) {
		r_x[i] = float(m[i][FEATURE_COUNT] / m[i][i]);
		r_y[i] = float(m[i][FEATURE_COUNT + 1] / m[i][i]);
	}
	return true;
}

bool GazeModel::fit(const LocalVector<float> &p_rows, const LocalVector<Vector2> &p_targets, const LocalVector<int> &p_groups, float &r_cv_error) {
	const int n = p_targets.size();
	r_cv_error = 1.0f;
	ERR_FAIL_COND_V(int(p_rows.size()) != n * FEATURE_COUNT || int(p_groups.size()) != n, false);
	if (n < 10) {
		return false;
	}

	// Each dot weighs the same.
	int group_count = 0;
	for (int g : p_groups) {
		group_count = MAX(group_count, g + 1);
	}
	LocalVector<int> per_group;
	per_group.resize(group_count);
	for (int &count : per_group) {
		count = 0;
	}
	for (int g : p_groups) {
		per_group[g]++;
	}
	int used_groups = 0;
	for (int count : per_group) {
		used_groups += count > 0 ? 1 : 0;
	}
	if (used_groups < 5) {
		return false;
	}
	LocalVector<float> sample_weights;
	sample_weights.resize(n);
	for (int s = 0; s < n; s++) {
		sample_weights[s] = 1.0f / per_group[p_groups[s]];
	}

	// Standardise, by the weighted mean and spread.
	float new_mean[FEATURE_COUNT] = {};
	float new_scale[FEATURE_COUNT] = {};
	new_scale[FEATURE_BIAS] = 1.0f;
	for (int k = 1; k < FEATURE_COUNT; k++) {
		double sum = 0.0;
		double weight = 0.0;
		for (int s = 0; s < n; s++) {
			sum += sample_weights[s] * p_rows[s * FEATURE_COUNT + k];
			weight += sample_weights[s];
		}
		new_mean[k] = float(sum / weight);
		double variance = 0.0;
		for (int s = 0; s < n; s++) {
			const double d = p_rows[s * FEATURE_COUNT + k] - new_mean[k];
			variance += sample_weights[s] * d * d;
		}
		new_scale[k] = MAX(float(Math::sqrt(variance / weight)), SPREAD_FLOOR[k]);
	}
	LocalVector<float> standard;
	standard.resize(n * FEATURE_COUNT);
	for (int s = 0; s < n; s++) {
		for (int k = 0; k < FEATURE_COUNT; k++) {
			standard[s * FEATURE_COUNT + k] = k == FEATURE_BIAS ? 1.0f : (p_rows[s * FEATURE_COUNT + k] - new_mean[k]) / new_scale[k];
		}
	}

	// How strongly to hold the fit back: the one that predicts dots left out best.
	float best_lambda = 0.1f;
	float best_error = 1e10f;
	for (float lambda : LAMBDAS) {
		double error = 0.0;
		int tested = 0;
		for (int g = 0; g < group_count; g++) {
			if (per_group[g] == 0) {
				continue;
			}
			float wx[FEATURE_COUNT];
			float wy[FEATURE_COUNT];
			if (!_solve(standard, p_targets, sample_weights, g, p_groups, lambda, wx, wy)) {
				continue;
			}
			// The dot's predicted place: the mean of its samples' predictions.
			Vector2 predicted;
			Vector2 target;
			for (int s = 0; s < n; s++) {
				if (p_groups[s] != g) {
					continue;
				}
				const float *row = standard.ptr() + s * FEATURE_COUNT;
				for (int k = 0; k < FEATURE_COUNT; k++) {
					predicted.x += wx[k] * row[k];
					predicted.y += wy[k] * row[k];
				}
				target = p_targets[s];
			}
			predicted /= float(per_group[g]);
			error += predicted.distance_to(target);
			tested++;
		}
		if (tested == 0) {
			continue;
		}
		const float mean_error = float(error / tested);
		if (mean_error < best_error) {
			best_error = mean_error;
			best_lambda = lambda;
		}
	}
	if (best_error >= 1e9f) {
		return false;
	}

	float wx[FEATURE_COUNT];
	float wy[FEATURE_COUNT];
	if (!_solve(standard, p_targets, sample_weights, -1, p_groups, best_lambda, wx, wy)) {
		return false;
	}
	for (int k = 0; k < FEATURE_COUNT; k++) {
		mean[k] = new_mean[k];
		scale[k] = new_scale[k];
		weights_x[k] = wx[k];
		weights_y[k] = wy[k];
		lowest[k] = 1e10f;
		highest[k] = -1e10f;
		for (int s = 0; s < n; s++) {
			lowest[k] = MIN(lowest[k], p_rows[s * FEATURE_COUNT + k]);
			highest[k] = MAX(highest[k], p_rows[s * FEATURE_COUNT + k]);
		}
	}
	valid = true;
	r_cv_error = best_error;
	return true;
}

Vector2 GazeModel::map(const float p_features[FEATURE_COUNT]) const {
	Vector2 result;
	for (int k = 0; k < FEATURE_COUNT; k++) {
		const float margin = (highest[k] - lowest[k]) * RANGE_MARGIN;
		const float value = CLAMP(p_features[k], lowest[k] - margin, highest[k] + margin);
		const float v = k == FEATURE_BIAS ? 1.0f : (value - mean[k]) / scale[k];
		result.x += weights_x[k] * v;
		result.y += weights_y[k] * v;
	}
	// Somewhere about the screen, whatever the measurement.
	return result.clamp(Vector2(-0.5, -0.5), Vector2(1.5, 1.5));
}

PackedFloat32Array GazeModel::save() const {
	PackedFloat32Array data;
	if (!valid) {
		return data;
	}
	data.push_back(FORMAT_VERSION);
	data.push_back(FEATURE_COUNT);
	for (const float *block : { mean, scale, weights_x, weights_y, lowest, highest }) {
		for (int k = 0; k < FEATURE_COUNT; k++) {
			data.push_back(block[k]);
		}
	}
	return data;
}

bool GazeModel::load(const PackedFloat32Array &p_data) {
	if (p_data.size() != 2 + FEATURE_COUNT * 6 || p_data[0] != FORMAT_VERSION || int(p_data[1]) != FEATURE_COUNT) {
		return false;
	}
	const float *d = p_data.ptr() + 2;
	for (int k = 0; k < FEATURE_COUNT; k++) {
		mean[k] = d[k];
		scale[k] = MAX(d[FEATURE_COUNT + k], 1e-6f);
		weights_x[k] = d[FEATURE_COUNT * 2 + k];
		weights_y[k] = d[FEATURE_COUNT * 3 + k];
		lowest[k] = d[FEATURE_COUNT * 4 + k];
		highest[k] = d[FEATURE_COUNT * 5 + k];
	}
	valid = true;
	return true;
}
