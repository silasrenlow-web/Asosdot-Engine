/**************************************************************************/
/*  light_catcher_3d.h                                                    */
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

#include "core/templates/hash_map.h"
#include "core/io/image.h"
#include "core/templates/hash_set.h"
#include "scene/resources/texture.h"
#include "scene/3d/node_3d.h"

class Light3D;

// Measures how brightly the scene's lights light a few points, the way the
// renderer lights them: the same energy, range, attenuation and spot cone
// formulas, with walls blocking light through physics raycasts. Put one on a
// player and enemies can tell whether they're standing in the light, or in a
// flashlight's beam.
class LightCatcher3D : public Node3D {
	GDCLASS(LightCatcher3D, Node3D);

public:
	enum Combine {
		COMBINE_MAXIMUM,
		COMBINE_AVERAGE,
	};

	enum Occlusion {
		OCCLUSION_ALWAYS,
		OCCLUSION_SHADOWED_LIGHTS,
		OCCLUSION_DISABLED,
	};

	enum OcclusionSource {
		OCCLUSION_SOURCE_COLLIDERS,
		OCCLUSION_SOURCE_SHADOW_CASTERS,
		OCCLUSION_SOURCE_BOTH,
	};

private:
	bool active = true;
	PackedVector3Array sample_points = { Vector3() };
	Combine combine = COMBINE_MAXIMUM;
	real_t threshold = 0.1;
	real_t response_time = 0.0;
	real_t update_interval = 0.0;
	uint32_t light_mask = 0xFFFFFFFF;

	Occlusion occlusion = OCCLUSION_ALWAYS;
	OcclusionSource occlusion_source = OCCLUSION_SOURCE_COLLIDERS;
	uint32_t occlusion_mask = 1;
	real_t occlusion_margin = 0.2;
	real_t directional_distance = 200.0;
	bool exclude_parent_bodies = true;

	// What the last update found.
	real_t raw_brightness = 0.0;
	real_t brightness = 0.0;
	Color light_color;
	bool lit = false;
	LocalVector<real_t> point_brightness;
	HashMap<ObjectID, real_t> contributions;
	HashSet<ObjectID> catching; // lights at or over the threshold on their own
	real_t time_to_update = 0.0;

	// Rays start this far off the point, so a point on a surface isn't
	// shadowed by the surface it's on.
	static constexpr real_t SURFACE_OFFSET = 0.02;
	static constexpr uint64_t PROJECTOR_REFRESH_MSEC = 2000;
	struct ProjectorImage {
		Ref<Image> image;
		uint64_t read_at = 0;
	};
	mutable HashMap<ObjectID, ProjectorImage> projector_images;

	void _update_processing();
	void _update(double p_delta);
	void _collect_exclusions(Node *p_from, HashSet<RID> &r_exclude) const;
	Color _light_at(Light3D *p_light, const Vector3 &p_point, const Vector3 &p_normal, bool p_occlude, const HashSet<RID> &p_exclude) const;
	bool _blocked(const Vector3 &p_from, const Vector3 &p_to, Light3D *p_light, const HashSet<RID> &p_exclude) const;
	bool _shadow_casters_block(const Vector3 &p_from, const Vector3 &p_to, Light3D *p_light, RID p_scenario) const;
	const Ref<Image> &_projector_image(const Ref<Texture2D> &p_texture) const;
	Color _projector_filter(Light3D *p_light, const Vector3 &p_point) const;
	real_t _distance_fade(Light3D *p_light, real_t &r_shadow_fade) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	static real_t luminance(const Color &p_color);

	void set_active(bool p_active);
	bool is_active() const;

	void set_sample_points(const PackedVector3Array &p_points);
	PackedVector3Array get_sample_points() const;

	void set_combine(Combine p_combine);
	Combine get_combine() const;

	void set_threshold(real_t p_threshold);
	real_t get_threshold() const;

	void set_response_time(real_t p_time);
	real_t get_response_time() const;

	void set_update_interval(real_t p_interval);
	real_t get_update_interval() const;

	void set_light_mask(uint32_t p_mask);
	uint32_t get_light_mask() const;

	void set_occlusion(Occlusion p_occlusion);
	Occlusion get_occlusion() const;

	void set_occlusion_source(OcclusionSource p_source);
	OcclusionSource get_occlusion_source() const;

	void set_occlusion_mask(uint32_t p_mask);
	uint32_t get_occlusion_mask() const;

	void set_occlusion_margin(real_t p_margin);
	real_t get_occlusion_margin() const;

	void set_directional_distance(real_t p_distance);
	real_t get_directional_distance() const;

	void set_exclude_parent_bodies(bool p_exclude);
	bool is_excluding_parent_bodies() const;

	real_t get_brightness() const;
	Color get_light_color() const;
	bool is_lit() const;
	real_t get_point_brightness(int p_point) const;
	TypedArray<Light3D> get_lights() const;
	real_t get_light_contribution(Light3D *p_light) const;
	Light3D *get_brightest_light() const;

	// How brightly the lights light a point right now. With a normal, the
	// point is a surface facing that way; without, it faces each light.
	real_t measure_point(const Vector3 &p_global_position, const Vector3 &p_normal = Vector3()) const;
	Color measure_point_color(const Vector3 &p_global_position, const Vector3 &p_normal = Vector3()) const;

	void update_now();

	PackedStringArray get_configuration_warnings() const override;

	LightCatcher3D();
};

VARIANT_ENUM_CAST(LightCatcher3D::Combine);
VARIANT_ENUM_CAST(LightCatcher3D::Occlusion);
VARIANT_ENUM_CAST(LightCatcher3D::OcclusionSource);
