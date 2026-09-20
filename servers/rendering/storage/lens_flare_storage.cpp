/**************************************************************************/
/*  lens_flare_storage.cpp                                                */
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

#include "lens_flare_storage.h"

RendererLensFlare *RendererLensFlare::singleton = nullptr;
const RendererLensFlare::LensFlare RendererLensFlare::default_flare = RendererLensFlare::LensFlare();

RendererLensFlare::RendererLensFlare() {
	singleton = this;
}

RendererLensFlare::~RendererLensFlare() {
	singleton = nullptr;
}

RID RendererLensFlare::lens_flare_allocate() {
	return lens_flare_owner.allocate_rid();
}

void RendererLensFlare::lens_flare_initialize(RID p_rid) {
	lens_flare_owner.initialize_rid(p_rid, LensFlare());
}

void RendererLensFlare::lens_flare_free(RID p_rid) {
	lens_flare_owner.free(p_rid);
}

void RendererLensFlare::lens_flare_set_element_count(RID p_flare, int p_count) {
	LensFlare *flare = lens_flare_owner.get_or_null(p_flare);
	ERR_FAIL_NULL(flare);
	ERR_FAIL_COND(p_count < 0);
	flare->elements.resize(p_count);
}

int RendererLensFlare::lens_flare_get_element_count(RID p_flare) const {
	const LensFlare *flare = lens_flare_owner.get_or_null(p_flare);
	ERR_FAIL_NULL_V(flare, 0);
	return flare->elements.size();
}

void RendererLensFlare::lens_flare_set_element_shape(RID p_flare, int p_index, RSE::LensFlareElementType p_type, RID p_texture, int p_sides, float p_falloff, float p_chromatic_aberration) {
	LensFlare *flare = lens_flare_owner.get_or_null(p_flare);
	ERR_FAIL_NULL(flare);
	ERR_FAIL_INDEX(p_index, (int)flare->elements.size());
	ERR_FAIL_INDEX(p_type, RSE::LENS_FLARE_ELEMENT_MAX);

	Element &element = flare->elements[p_index];
	element.type = p_type;
	element.texture = p_texture;
	element.sides = CLAMP(p_sides, 3, 32);
	element.falloff = MAX(p_falloff, 0.0f);
	element.chromatic_aberration = p_chromatic_aberration;
}

void RendererLensFlare::lens_flare_set_element_layout(RID p_flare, int p_index, float p_position, const Vector2 &p_size, float p_rotation, bool p_auto_rotate, int p_count, float p_spacing) {
	LensFlare *flare = lens_flare_owner.get_or_null(p_flare);
	ERR_FAIL_NULL(flare);
	ERR_FAIL_INDEX(p_index, (int)flare->elements.size());

	Element &element = flare->elements[p_index];
	element.position = p_position;
	element.size = p_size.maxf(0.0);
	element.rotation = p_rotation;
	element.auto_rotate = p_auto_rotate;
	element.count = MAX(p_count, 1);
	element.spacing = p_spacing;
}

void RendererLensFlare::lens_flare_set_element_color(RID p_flare, int p_index, const Color &p_color, float p_intensity, RSE::LensFlareBlendMode p_blend_mode, bool p_tint_by_source) {
	LensFlare *flare = lens_flare_owner.get_or_null(p_flare);
	ERR_FAIL_NULL(flare);
	ERR_FAIL_INDEX(p_index, (int)flare->elements.size());
	ERR_FAIL_INDEX(p_blend_mode, RSE::LENS_FLARE_BLEND_MAX);

	Element &element = flare->elements[p_index];
	element.color = p_color;
	element.intensity = MAX(p_intensity, 0.0f);
	element.blend_mode = p_blend_mode;
	element.tint_by_source = p_tint_by_source;
}

void RendererLensFlare::lens_flare_set_source(RID p_flare, float p_intensity, float p_scale, const Color &p_color, bool p_directional) {
	LensFlare *flare = lens_flare_owner.get_or_null(p_flare);
	ERR_FAIL_NULL(flare);

	flare->intensity = MAX(p_intensity, 0.0f);
	flare->scale = MAX(p_scale, 0.0f);
	flare->color = p_color;
	flare->directional = p_directional;
}

void RendererLensFlare::lens_flare_set_occlusion(RID p_flare, bool p_enabled, float p_radius) {
	LensFlare *flare = lens_flare_owner.get_or_null(p_flare);
	ERR_FAIL_NULL(flare);

	flare->occlusion_enabled = p_enabled;
	flare->occlusion_radius = MAX(p_radius, 0.0f);
}

void RendererLensFlare::lens_flare_set_fade(RID p_flare, float p_distance_begin, float p_distance_end, float p_screen_edge, float p_angle_limit, float p_angle_fade) {
	LensFlare *flare = lens_flare_owner.get_or_null(p_flare);
	ERR_FAIL_NULL(flare);

	flare->fade_distance_begin = MAX(p_distance_begin, 0.0f);
	flare->fade_distance_end = MAX(p_distance_end, 0.0f);
	flare->fade_screen_edge = CLAMP(p_screen_edge, 0.0f, 1.0f);
	flare->angle_limit = CLAMP(p_angle_limit, 0.0f, (float)Math::PI);
	flare->angle_fade = CLAMP(p_angle_fade, 0.0f, 1.0f);
}

const LocalVector<RendererLensFlare::Element> &RendererLensFlare::lens_flare_get_elements(RID p_flare) const {
	const LensFlare *flare = lens_flare_owner.get_or_null(p_flare);
	ERR_FAIL_NULL_V(flare, default_flare.elements);
	return flare->elements;
}

#define LENS_FLARE_GETTER(m_name, m_type, m_member) \
	m_type RendererLensFlare::m_name(RID p_flare) const { \
		const LensFlare *flare = lens_flare_owner.get_or_null(p_flare); \
		ERR_FAIL_NULL_V(flare, default_flare.m_member); \
		return flare->m_member; \
	}

LENS_FLARE_GETTER(lens_flare_get_intensity, float, intensity)
LENS_FLARE_GETTER(lens_flare_get_scale, float, scale)
LENS_FLARE_GETTER(lens_flare_get_color, Color, color)
LENS_FLARE_GETTER(lens_flare_is_directional, bool, directional)
LENS_FLARE_GETTER(lens_flare_get_occlusion_enabled, bool, occlusion_enabled)
LENS_FLARE_GETTER(lens_flare_get_occlusion_radius, float, occlusion_radius)
LENS_FLARE_GETTER(lens_flare_get_fade_distance_begin, float, fade_distance_begin)
LENS_FLARE_GETTER(lens_flare_get_fade_distance_end, float, fade_distance_end)
LENS_FLARE_GETTER(lens_flare_get_fade_screen_edge, float, fade_screen_edge)
LENS_FLARE_GETTER(lens_flare_get_angle_limit, float, angle_limit)
LENS_FLARE_GETTER(lens_flare_get_angle_fade, float, angle_fade)

#undef LENS_FLARE_GETTER
