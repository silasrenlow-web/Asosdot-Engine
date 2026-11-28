/**************************************************************************/
/*  gaze_target_3d.cpp                                                    */
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

#include "gaze_target_3d.h"

#include "eye_tracker.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/physics_3d/physics_server_3d.h"

void GazeTarget3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &GazeTarget3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &GazeTarget3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_detection_mode", "mode"), &GazeTarget3D::set_detection_mode);
	ClassDB::bind_method(D_METHOD("get_detection_mode"), &GazeTarget3D::get_detection_mode);
	ClassDB::bind_method(D_METHOD("set_radius", "radius"), &GazeTarget3D::set_radius);
	ClassDB::bind_method(D_METHOD("get_radius"), &GazeTarget3D::get_radius);
	ClassDB::bind_method(D_METHOD("set_gaze_margin", "margin"), &GazeTarget3D::set_gaze_margin);
	ClassDB::bind_method(D_METHOD("get_gaze_margin"), &GazeTarget3D::get_gaze_margin);
	ClassDB::bind_method(D_METHOD("set_require_line_of_sight", "require"), &GazeTarget3D::set_require_line_of_sight);
	ClassDB::bind_method(D_METHOD("is_requiring_line_of_sight"), &GazeTarget3D::is_requiring_line_of_sight);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &GazeTarget3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &GazeTarget3D::get_collision_mask);
	ClassDB::bind_method(D_METHOD("set_unwatch_delay", "seconds"), &GazeTarget3D::set_unwatch_delay);
	ClassDB::bind_method(D_METHOD("get_unwatch_delay"), &GazeTarget3D::get_unwatch_delay);

	ClassDB::bind_method(D_METHOD("is_watched"), &GazeTarget3D::is_watched);
	ClassDB::bind_method(D_METHOD("is_on_screen"), &GazeTarget3D::is_on_screen);
	ClassDB::bind_method(D_METHOD("get_gaze_distance"), &GazeTarget3D::get_gaze_distance);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "detection_mode", PROPERTY_HINT_ENUM, "Gaze,On Screen"), "set_detection_mode", "get_detection_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radius", PROPERTY_HINT_RANGE, "0.01,20,0.01,or_greater,suffix:m"), "set_radius", "get_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gaze_margin", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_gaze_margin", "get_gaze_margin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "unwatch_delay", PROPERTY_HINT_RANGE, "0,2,0.01,suffix:s"), "set_unwatch_delay", "get_unwatch_delay");
	ADD_GROUP("Line of Sight", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "require_line_of_sight"), "set_require_line_of_sight", "is_requiring_line_of_sight");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");

	ADD_SIGNAL(MethodInfo("watched"));
	ADD_SIGNAL(MethodInfo("unwatched"));

	BIND_ENUM_CONSTANT(DETECTION_GAZE);
	BIND_ENUM_CONSTANT(DETECTION_ON_SCREEN);
}

GazeTarget3D::GazeTarget3D() {
	set_notify_transform(false);
}

void GazeTarget3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_process_internal(true);
			}
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			_update();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			if (watched) {
				watched = false;
				emit_signal(SNAME("unwatched"));
			}
			on_screen = false;
		} break;
	}
}

void GazeTarget3D::set_enabled(bool p_enabled) {
	enabled = p_enabled;
	if (!enabled && watched) {
		watched = false;
		emit_signal(SNAME("unwatched"));
	}
}

void GazeTarget3D::set_radius(float p_radius) {
	radius = MAX(p_radius, 0.001f);
	update_gizmos();
}

void GazeTarget3D::set_gaze_margin(float p_margin) {
	gaze_margin = MAX(p_margin, 0.0f);
}

void GazeTarget3D::set_unwatch_delay(float p_delay) {
	unwatch_delay = MAX(p_delay, 0.0f);
}

bool GazeTarget3D::_has_line_of_sight(Camera3D *p_camera) const {
	Ref<World3D> world = get_world_3d();
	if (world.is_null()) {
		return true;
	}
	PhysicsDirectSpaceState3D *space = world->get_direct_space_state();
	if (!space) {
		return true;
	}
	const Vector3 from = p_camera->get_global_position();
	const Vector3 to = get_global_position();
	const float distance = from.distance_to(to);
	PhysicsDirectSpaceState3D::RayParameters ray;
	ray.from = from;
	ray.to = to;
	ray.collision_mask = collision_mask;
	PhysicsDirectSpaceState3D::RayResult hit;
	if (!space->intersect_ray(ray, hit)) {
		return true;
	}
	// Whatever is hit inside the target's radius is the target itself (its
	// own body), not something in front of it.
	return from.distance_to(hit.position) >= distance - radius;
}

void GazeTarget3D::_update() {
	const bool was_watched = watched;
	bool seen = false;
	on_screen = false;
	gaze_distance = 1e10;

	Viewport *viewport = get_viewport();
	Camera3D *camera = viewport ? viewport->get_camera_3d() : nullptr;
	if (enabled && camera && is_visible_in_tree()) {
		const Vector3 position = get_global_position();
		if (!camera->is_position_behind(position)) {
			const Rect2 visible = viewport->get_visible_rect();
			const Vector2 centre = camera->unproject_position(position);
			// The radius as the camera sees it, from a point beside the target.
			const Vector3 side = camera->get_global_transform().basis.get_column(0).normalized() * radius;
			const float screen_radius = centre.distance_to(camera->unproject_position(position + side));
			on_screen = visible.grow(screen_radius).has_point(centre);

			if (on_screen) {
				EyeTracker *tracker = EyeTracker::get_singleton();
				const Vector2 gaze_uv = tracker ? tracker->get_gaze_uv() : Vector2(0.5, 0.5);
				const Vector2 gaze = visible.position + gaze_uv * visible.size;
				gaze_distance = MAX(gaze.distance_to(centre) - screen_radius, 0.0f) / MAX(visible.size.y, 1.0f);

				const bool looking = !tracker || tracker->is_looking_at_screen();
				bool candidate = looking;
				if (detection_mode == DETECTION_GAZE) {
					candidate = candidate && gaze_distance <= gaze_margin;
				}
				if (candidate && require_line_of_sight) {
					candidate = _has_line_of_sight(camera);
				}
				seen = candidate;
			}
		}
	}

	const double now = OS::get_singleton()->get_ticks_usec() / 1000000.0;
	if (seen) {
		last_seen = now;
		watched = true;
	} else if (watched && now - last_seen >= unwatch_delay) {
		watched = false;
	}
	if (watched != was_watched) {
		emit_signal(watched ? SNAME("watched") : SNAME("unwatched"));
	}
}
