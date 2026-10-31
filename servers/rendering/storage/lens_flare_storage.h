/**************************************************************************/
/*  lens_flare_storage.h                                                  */
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

#include "core/math/color.h"
#include "core/math/vector2.h"
#include "core/variant/typed_array.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid_owner.h"
#include "servers/rendering/rendering_server_enums.h"

// Lens flares are plain CPU data: a list of elements to draw plus the placement
// parameters of the source they hang off. Nothing here owns a GPU resource, so
// this storage is shared by every renderer rather than reimplemented per backend,
// the way RendererCameraAttributes is.
class RendererLensFlare {
public:
	// One drawn piece of a flare. Everything a renderer needs to place and shade
	// a quad, with no back reference to the flare that owns it.
	struct Element {
		// Shape.
		RSE::LensFlareElementType type = RSE::LENS_FLARE_ELEMENT_GLOW;
		RID texture;
		int sides = 6;
		float falloff = 1.0;
		float chromatic_aberration = 0.0;

		// Placement along the axis that runs from the source through the centre
		// of the screen. 0 sits on the source, 1 on the centre, 2 opposite it.
		float position = 0.0;
		Vector2 size = Vector2(0.1, 0.1);
		float rotation = 0.0;
		bool auto_rotate = false;
		int count = 1;
		float spacing = 0.1;

		// Shading.
		Color color = Color(1, 1, 1, 1);
		float intensity = 1.0;
		RSE::LensFlareBlendMode blend_mode = RSE::LENS_FLARE_BLEND_ADD;
		bool tint_by_source = true;
	};

private:
	static RendererLensFlare *singleton;

	struct LensFlare {
		LocalVector<Element> elements;

		// Source parameters. These live on the flare rather than on the instance
		// because a LensFlare3D gives every node its own flare RID; sharing the
		// element list between nodes would mean a second, parallel per-instance
		// parameter API for no real saving, the element list being a few hundred
		// bytes at most.
		float intensity = 1.0;
		float scale = 1.0;
		Color color = Color(1, 1, 1, 1);
		bool directional = false;

		bool occlusion_enabled = true;
		float occlusion_radius = 0.5;
		// Instances the occlusion test looks past, as RIDs of scenario
		// instances. The scene culler turns them into world bounds each frame.
		LocalVector<RID> occlusion_exceptions;

		float fade_distance_begin = 0.0; // 0 disables distance fading.
		float fade_distance_end = 0.0;
		float fade_screen_edge = 0.2;
		float angle_limit = 0.0; // 0 disables angular fading.
		float angle_fade = 0.2;
	};

	mutable RID_Owner<LensFlare, true> lens_flare_owner;

	static const LensFlare default_flare;

public:
	static RendererLensFlare *get_singleton() { return singleton; }

	RendererLensFlare();
	~RendererLensFlare();

	RID lens_flare_allocate();
	void lens_flare_initialize(RID p_rid);
	void lens_flare_free(RID p_rid);

	bool owns_lens_flare(RID p_rid) const { return lens_flare_owner.owns(p_rid); }

	void lens_flare_set_element_count(RID p_flare, int p_count);
	int lens_flare_get_element_count(RID p_flare) const;

	// Grouped rather than one setter per field: an element has fifteen of them,
	// and callers always rewrite a whole element at once.
	void lens_flare_set_element_shape(RID p_flare, int p_index, RSE::LensFlareElementType p_type, RID p_texture, int p_sides, float p_falloff, float p_chromatic_aberration);
	void lens_flare_set_element_layout(RID p_flare, int p_index, float p_position, const Vector2 &p_size, float p_rotation, bool p_auto_rotate, int p_count, float p_spacing);
	void lens_flare_set_element_color(RID p_flare, int p_index, const Color &p_color, float p_intensity, RSE::LensFlareBlendMode p_blend_mode, bool p_tint_by_source);

	void lens_flare_set_source(RID p_flare, float p_intensity, float p_scale, const Color &p_color, bool p_directional);
	void lens_flare_set_occlusion(RID p_flare, bool p_enabled, float p_radius);
	void lens_flare_set_occlusion_exceptions(RID p_flare, const TypedArray<RID> &p_instances);
	const LocalVector<RID> &lens_flare_get_occlusion_exceptions(RID p_flare) const;
	void lens_flare_set_fade(RID p_flare, float p_distance_begin, float p_distance_end, float p_screen_edge, float p_angle_limit, float p_angle_fade);

	// Renderer side reads. These return references into the flare, so callers must
	// not hold them across anything that can free it.
	const LocalVector<Element> &lens_flare_get_elements(RID p_flare) const;

	float lens_flare_get_intensity(RID p_flare) const;
	float lens_flare_get_scale(RID p_flare) const;
	Color lens_flare_get_color(RID p_flare) const;
	bool lens_flare_is_directional(RID p_flare) const;

	bool lens_flare_get_occlusion_enabled(RID p_flare) const;
	float lens_flare_get_occlusion_radius(RID p_flare) const;

	float lens_flare_get_fade_distance_begin(RID p_flare) const;
	float lens_flare_get_fade_distance_end(RID p_flare) const;
	float lens_flare_get_fade_screen_edge(RID p_flare) const;
	float lens_flare_get_angle_limit(RID p_flare) const;
	float lens_flare_get_angle_fade(RID p_flare) const;
};
