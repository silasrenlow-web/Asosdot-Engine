/**************************************************************************/
/*  lens_flare_3d.cpp                                                     */
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

#include "lens_flare_3d.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "servers/rendering/rendering_server.h"

void LensFlare3D::_flare_changed() {
	_update_elements();
}

void LensFlare3D::_update_elements() {
	RenderingServer *rs = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rs);

	// With no flare of its own the node draws the engine's default one, so
	// dropping a LensFlare3D into a scene shows a flare straight away. Building
	// it here rather than in a constructor keeps an instantiated resource out of
	// the class's default property value, which every node would then share.
	Ref<LensFlare> source = flare.is_valid() ? flare : LensFlare::create_default();

	const Vector<Ref<LensFlareElement>> &list = source->get_element_list();
	rs->lens_flare_set_element_count(flare_rid, list.size());

	for (int i = 0; i < list.size(); i++) {
		const Ref<LensFlareElement> &element = list[i];
		if (element.is_null()) {
			// An empty slot in the array keeps its index, so the ones after it do
			// not shuffle while it is being filled in. A zero size draws nothing.
			rs->lens_flare_set_element_layout(flare_rid, i, 0.0, Vector2(), 0.0, false, 1, 0.0);
			continue;
		}

		RID texture;
		if (element->get_texture().is_valid()) {
			texture = element->get_texture()->get_rid();
		}

		rs->lens_flare_set_element_shape(flare_rid, i, (RSE::LensFlareElementType)element->get_type(), texture, element->get_sides(), element->get_falloff(), element->get_chromatic_aberration());
		rs->lens_flare_set_element_layout(flare_rid, i, element->get_position(), element->get_size(), element->get_rotation(), element->is_auto_rotating(), element->get_count(), element->get_spacing());
		rs->lens_flare_set_element_color(flare_rid, i, element->get_color(), element->get_intensity(), (RSE::LensFlareBlendMode)element->get_blend_mode(), element->is_tinted_by_source());
	}
}

void LensFlare3D::_update_source() {
	RenderingServer *rs = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rs);

	rs->lens_flare_set_source(flare_rid, intensity, flare_scale, color, directional);
	rs->lens_flare_set_occlusion(flare_rid, occlusion_enabled, occlusion_radius);

	// A directional source is infinitely far away and has no cone, so neither
	// fade means anything for it.
	const float distance_begin = directional ? 0.0 : fade_distance_begin;
	const float distance_end = directional ? 0.0 : fade_distance_end;
	const float limit = directional ? 0.0 : angle_limit;
	rs->lens_flare_set_fade(flare_rid, distance_begin, distance_end, fade_screen_edge, limit, angle_fade);
}

AABB LensFlare3D::get_aabb() const {
	// A flare is a point. The box is only here so the node reports something
	// sensible to the editor and to get_aabb() callers; the renderer never
	// bounds checks a flare, it projects it and lets the screen do the culling.
	return AABB(Vector3(-0.25, -0.25, -0.25), Vector3(0.5, 0.5, 0.5));
}

PackedStringArray LensFlare3D::get_configuration_warnings() const {
	PackedStringArray warnings = VisualInstance3D::get_configuration_warnings();

	if (OS::get_singleton()->get_current_rendering_method() == "gl_compatibility") {
		warnings.push_back(RTR("Lens flares are only drawn by the Forward+ and Mobile renderers."));
	}

	if (flare.is_valid() && flare->get_element_count() == 0) {
		warnings.push_back(RTR("This node's LensFlare has no elements, so nothing is drawn. LensFlare.create_default() returns a filled in one to start from."));
	}

	return warnings;
}

void LensFlare3D::set_flare(const Ref<LensFlare> &p_flare) {
	if (flare == p_flare) {
		return;
	}

	if (flare.is_valid()) {
		flare->disconnect_changed(callable_mp(this, &LensFlare3D::_flare_changed));
	}

	flare = p_flare;

	if (flare.is_valid()) {
		flare->connect_changed(callable_mp(this, &LensFlare3D::_flare_changed));
	}

	_update_elements();
	update_configuration_warnings();
}

Ref<LensFlare> LensFlare3D::get_flare() const {
	return flare;
}

void LensFlare3D::set_intensity(float p_intensity) {
	intensity = MAX(p_intensity, 0.0f);
	_update_source();
}

float LensFlare3D::get_intensity() const {
	return intensity;
}

void LensFlare3D::set_flare_scale(float p_scale) {
	flare_scale = MAX(p_scale, 0.0f);
	_update_source();
}

float LensFlare3D::get_flare_scale() const {
	return flare_scale;
}

void LensFlare3D::set_color(const Color &p_color) {
	color = p_color;
	_update_source();
}

Color LensFlare3D::get_color() const {
	return color;
}

void LensFlare3D::set_directional(bool p_directional) {
	directional = p_directional;
	_update_source();
	notify_property_list_changed();
}

bool LensFlare3D::is_directional() const {
	return directional;
}

void LensFlare3D::set_occlusion_enabled(bool p_enabled) {
	occlusion_enabled = p_enabled;
	_update_source();
	notify_property_list_changed();
}

bool LensFlare3D::is_occlusion_enabled() const {
	return occlusion_enabled;
}

void LensFlare3D::set_occlusion_radius(float p_radius) {
	occlusion_radius = MAX(p_radius, 0.0f);
	_update_source();
}

float LensFlare3D::get_occlusion_radius() const {
	return occlusion_radius;
}

void LensFlare3D::set_fade_distance_begin(float p_distance) {
	fade_distance_begin = MAX(p_distance, 0.0f);
	_update_source();
}

float LensFlare3D::get_fade_distance_begin() const {
	return fade_distance_begin;
}

void LensFlare3D::set_fade_distance_end(float p_distance) {
	fade_distance_end = MAX(p_distance, 0.0f);
	_update_source();
}

float LensFlare3D::get_fade_distance_end() const {
	return fade_distance_end;
}

void LensFlare3D::set_fade_screen_edge(float p_fade) {
	fade_screen_edge = CLAMP(p_fade, 0.0f, 1.0f);
	_update_source();
}

float LensFlare3D::get_fade_screen_edge() const {
	return fade_screen_edge;
}

void LensFlare3D::set_angle_limit(float p_radians) {
	angle_limit = CLAMP(p_radians, 0.0f, (float)Math::PI);
	_update_source();
	notify_property_list_changed();
}

float LensFlare3D::get_angle_limit() const {
	return angle_limit;
}

void LensFlare3D::set_angle_fade(float p_fade) {
	angle_fade = CLAMP(p_fade, 0.0f, 1.0f);
	_update_source();
}

float LensFlare3D::get_angle_fade() const {
	return angle_fade;
}

void LensFlare3D::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name == "occlusion_radius" && !occlusion_enabled) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}

	// Distance and angle both need a source that is somewhere, and a directional
	// one is only a direction.
	if (directional && (p_property.name == "fade_distance_begin" || p_property.name == "fade_distance_end" || p_property.name == "angle_limit" || p_property.name == "angle_fade")) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}

	if (p_property.name == "angle_fade" && angle_limit <= 0.0) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

void LensFlare3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_flare", "flare"), &LensFlare3D::set_flare);
	ClassDB::bind_method(D_METHOD("get_flare"), &LensFlare3D::get_flare);
	ClassDB::bind_method(D_METHOD("set_intensity", "intensity"), &LensFlare3D::set_intensity);
	ClassDB::bind_method(D_METHOD("get_intensity"), &LensFlare3D::get_intensity);
	ClassDB::bind_method(D_METHOD("set_flare_scale", "scale"), &LensFlare3D::set_flare_scale);
	ClassDB::bind_method(D_METHOD("get_flare_scale"), &LensFlare3D::get_flare_scale);
	ClassDB::bind_method(D_METHOD("set_color", "color"), &LensFlare3D::set_color);
	ClassDB::bind_method(D_METHOD("get_color"), &LensFlare3D::get_color);
	ClassDB::bind_method(D_METHOD("set_directional", "directional"), &LensFlare3D::set_directional);
	ClassDB::bind_method(D_METHOD("is_directional"), &LensFlare3D::is_directional);

	ClassDB::bind_method(D_METHOD("set_occlusion_enabled", "enabled"), &LensFlare3D::set_occlusion_enabled);
	ClassDB::bind_method(D_METHOD("is_occlusion_enabled"), &LensFlare3D::is_occlusion_enabled);
	ClassDB::bind_method(D_METHOD("set_occlusion_radius", "radius"), &LensFlare3D::set_occlusion_radius);
	ClassDB::bind_method(D_METHOD("get_occlusion_radius"), &LensFlare3D::get_occlusion_radius);

	ClassDB::bind_method(D_METHOD("set_fade_distance_begin", "distance"), &LensFlare3D::set_fade_distance_begin);
	ClassDB::bind_method(D_METHOD("get_fade_distance_begin"), &LensFlare3D::get_fade_distance_begin);
	ClassDB::bind_method(D_METHOD("set_fade_distance_end", "distance"), &LensFlare3D::set_fade_distance_end);
	ClassDB::bind_method(D_METHOD("get_fade_distance_end"), &LensFlare3D::get_fade_distance_end);
	ClassDB::bind_method(D_METHOD("set_fade_screen_edge", "fade"), &LensFlare3D::set_fade_screen_edge);
	ClassDB::bind_method(D_METHOD("get_fade_screen_edge"), &LensFlare3D::get_fade_screen_edge);
	ClassDB::bind_method(D_METHOD("set_angle_limit", "radians"), &LensFlare3D::set_angle_limit);
	ClassDB::bind_method(D_METHOD("get_angle_limit"), &LensFlare3D::get_angle_limit);
	ClassDB::bind_method(D_METHOD("set_angle_fade", "fade"), &LensFlare3D::set_angle_fade);
	ClassDB::bind_method(D_METHOD("get_angle_fade"), &LensFlare3D::get_angle_fade);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "flare", PROPERTY_HINT_RESOURCE_TYPE, LensFlare::get_class_static()), "set_flare", "get_flare");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "intensity", PROPERTY_HINT_RANGE, "0,16,0.001,or_greater"), "set_intensity", "get_intensity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flare_scale", PROPERTY_HINT_RANGE, "0,8,0.001,or_greater"), "set_flare_scale", "get_flare_scale");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_color", "get_color");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "directional"), "set_directional", "is_directional");

	ADD_GROUP("Occlusion", "occlusion_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "occlusion_enabled"), "set_occlusion_enabled", "is_occlusion_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "occlusion_radius", PROPERTY_HINT_RANGE, "0,8,0.001,or_greater,suffix:m"), "set_occlusion_radius", "get_occlusion_radius");

	ADD_GROUP("Fade", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fade_distance_begin", PROPERTY_HINT_RANGE, "0,4096,0.01,or_greater,suffix:m"), "set_fade_distance_begin", "get_fade_distance_begin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fade_distance_end", PROPERTY_HINT_RANGE, "0,4096,0.01,or_greater,suffix:m"), "set_fade_distance_end", "get_fade_distance_end");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fade_screen_edge", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_fade_screen_edge", "get_fade_screen_edge");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "angle_limit", PROPERTY_HINT_RANGE, "0,180,0.1,radians_as_degrees"), "set_angle_limit", "get_angle_limit");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "angle_fade", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_angle_fade", "get_angle_fade");
}

LensFlare3D::LensFlare3D() {
	RenderingServer *rs = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rs);

	flare_rid = rs->lens_flare_create();
	set_base(flare_rid);

	_update_source();
	// Pushes the engine default, since there is no flare of our own yet.
	_update_elements();
}

LensFlare3D::~LensFlare3D() {
	RenderingServer *rs = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rs);

	// Detach before freeing, or the instance spends the rest of its teardown
	// holding a RID that no longer resolves.
	set_base(RID());
	rs->free_rid(flare_rid);
}
