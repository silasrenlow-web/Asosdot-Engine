/**************************************************************************/
/*  gaze_target_3d.h                                                      */
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

#include "scene/3d/node_3d.h"

class Camera3D;

// Answers "is the player watching me?" for whatever it is placed on: in view
// of the current camera, near where the player looks (or anywhere on the
// screen), with open eyes, and optionally not hidden behind a wall. Without eye
// tracking running, the player counts as looking at the middle of the screen
// with open eyes, so a game works the same on machines without a webcam.
class GazeTarget3D : public Node3D {
	GDCLASS(GazeTarget3D, Node3D);

public:
	enum DetectionMode {
		DETECTION_GAZE, // Near the gaze point.
		DETECTION_ON_SCREEN, // Anywhere in view, as long as the eyes are open and on the screen.
	};

private:
	bool enabled = true;
	DetectionMode detection_mode = DETECTION_GAZE;
	float radius = 0.5;
	float gaze_margin = 0.12;
	bool require_line_of_sight = false;
	uint32_t collision_mask = 1;
	float unwatch_delay = 0.1;

	bool watched = false;
	bool on_screen = false;
	float gaze_distance = 1e10;
	double last_seen = -1e10;

	void _update();
	bool _has_line_of_sight(Camera3D *p_camera) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_enabled(bool p_enabled);
	bool is_enabled() const { return enabled; }
	void set_detection_mode(DetectionMode p_mode) { detection_mode = p_mode; }
	DetectionMode get_detection_mode() const { return detection_mode; }
	void set_radius(float p_radius);
	float get_radius() const { return radius; }
	void set_gaze_margin(float p_margin);
	float get_gaze_margin() const { return gaze_margin; }
	void set_require_line_of_sight(bool p_require) { require_line_of_sight = p_require; }
	bool is_requiring_line_of_sight() const { return require_line_of_sight; }
	void set_collision_mask(uint32_t p_mask) { collision_mask = p_mask; }
	uint32_t get_collision_mask() const { return collision_mask; }
	void set_unwatch_delay(float p_delay);
	float get_unwatch_delay() const { return unwatch_delay; }

	bool is_watched() const { return watched; }
	bool is_on_screen() const { return on_screen; }
	float get_gaze_distance() const { return gaze_distance; }

	GazeTarget3D();
};

VARIANT_ENUM_CAST(GazeTarget3D::DetectionMode);
