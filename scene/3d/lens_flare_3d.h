/**************************************************************************/
/*  lens_flare_3d.h                                                       */
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

#include "scene/3d/visual_instance_3d.h"
#include "scene/resources/lens_flare.h"

// A source of lens flare. Put one on a light, a headlamp, a window, anything
// bright enough to scatter inside a lens, and give it a LensFlare to draw.
//
// Which cameras pick it up is the camera's decision: Camera3D compares its
// lens_flare_cull_mask against this node's VisualInstance3D layers, separately
// from its ordinary cull_mask, so a flare can be hidden from one camera without
// hiding whatever is throwing it.
class LensFlare3D : public VisualInstance3D {
	GDCLASS(LensFlare3D, VisualInstance3D);

	Ref<LensFlare> flare;
	RID flare_rid;

	float intensity = 1.0;
	float flare_scale = 1.0;
	Color color = Color(1, 1, 1);
	bool directional = false;

	bool occlusion_enabled = true;
	float occlusion_radius = 0.25;
	float occlusion_angle = Math::deg_to_rad(1.0);

	float fade_distance_begin = 0.0;
	float fade_distance_end = 0.0;
	float fade_screen_edge = 0.2;
	float angle_limit = 0.0;
	float angle_fade = 0.2;

	void _flare_changed();
	void _update_elements();
	void _update_source();

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	virtual AABB get_aabb() const override;
	PackedStringArray get_configuration_warnings() const override;

	void set_flare(const Ref<LensFlare> &p_flare);
	Ref<LensFlare> get_flare() const;

	void set_intensity(float p_intensity);
	float get_intensity() const;

	void set_flare_scale(float p_scale);
	float get_flare_scale() const;

	void set_color(const Color &p_color);
	Color get_color() const;

	void set_directional(bool p_directional);
	bool is_directional() const;

	void set_occlusion_enabled(bool p_enabled);
	bool is_occlusion_enabled() const;

	void set_occlusion_radius(float p_radius);
	float get_occlusion_radius() const;

	void set_occlusion_angle(float p_radians);
	float get_occlusion_angle() const;

	void set_fade_distance_begin(float p_distance);
	float get_fade_distance_begin() const;

	void set_fade_distance_end(float p_distance);
	float get_fade_distance_end() const;

	void set_fade_screen_edge(float p_fade);
	float get_fade_screen_edge() const;

	void set_angle_limit(float p_radians);
	float get_angle_limit() const;

	void set_angle_fade(float p_fade);
	float get_angle_fade() const;

	LensFlare3D();
	~LensFlare3D();
};
