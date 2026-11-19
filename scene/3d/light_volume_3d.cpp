/**************************************************************************/
/*  light_volume_3d.cpp                                                   */
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

#include "light_volume_3d.h"

#include "core/io/image.h"
#include "core/math/math_funcs.h"
#include "scene/3d/light_3d.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"

namespace {
// The shaft shader. Additive, double-sided shell: brightness peaks where
// the view looks through the middle of the beam (fresnel on the shell),
// fades along its length, is shaped across its width by the cookie, is
// broken up by two scrolling noise octaves (dust drifting in the light),
// and fades where scene geometry crosses it (depth fade), so a wall edge
// crossing the beam softens instead of slicing.
const char *SHAFT_SHADER = R"GLSL(
shader_type spatial;
render_mode blend_add, cull_disabled;

uniform vec4 shaft_color : source_color = vec4(1.0, 1.0, 1.0, 1.0);
uniform float intensity = 0.35;
uniform float edge_softness = 1.5;
uniform float fade_start = 0.05;
uniform float fade_end = 0.9;
uniform float shaft_length = 6.0;
uniform vec2 near_extent = vec2(0.4, 0.4);
uniform vec2 far_extent = vec2(1.6, 1.6);
uniform vec2 shear_offset = vec2(0.0, 0.0);
uniform sampler2D cookie_texture : filter_linear_mipmap, repeat_disable;
uniform float cookie_strength = 1.0;
uniform float noise_amount = 0.35;
uniform float noise_scale = 0.45;
uniform float noise_speed = 0.06;
uniform bool use_depth_fade = true;
uniform float depth_fade_distance = 0.75;

varying vec3 local_pos;
varying float axial;

void vertex() {
	local_pos = VERTEX;
	axial = clamp(-VERTEX.z / max(shaft_length, 0.001), 0.0, 1.0);
}

float hash13(vec3 p) {
	p = fract(p * 0.3183099 + vec3(0.11, 0.17, 0.13));
	p *= 17.0;
	return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float value_noise(vec3 p) {
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	float n000 = hash13(i);
	float n100 = hash13(i + vec3(1.0, 0.0, 0.0));
	float n010 = hash13(i + vec3(0.0, 1.0, 0.0));
	float n110 = hash13(i + vec3(1.0, 1.0, 0.0));
	float n001 = hash13(i + vec3(0.0, 0.0, 1.0));
	float n101 = hash13(i + vec3(1.0, 0.0, 1.0));
	float n011 = hash13(i + vec3(0.0, 1.0, 1.0));
	float n111 = hash13(i + vec3(1.0, 1.0, 1.0));
	return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
			mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

void fragment() {
	// Through the middle of the beam the shell faces the camera; at the
	// silhouette it turns away. That is what makes a shell read as a volume.
	float through = pow(abs(dot(NORMAL, VIEW)), max(edge_softness, 0.01));

	float fade_in = smoothstep(0.0, max(fade_start, 0.001) + 0.02, axial);
	float fade_out = 1.0 - smoothstep(max(fade_end - 0.3, 0.0), max(fade_end, 0.001), axial);
	float axial_fade = fade_in * fade_out;

	// Cookie: project the fragment's position across the shaft's width at
	// its depth, undoing the shear, so a window-frame or blinds texture
	// holds its shape all the way down the beam.
	vec2 half_extent = max(mix(near_extent, far_extent, axial), vec2(0.001));
	vec2 across = (local_pos.xy - shear_offset * axial) / (half_extent * 2.0);
	vec3 cookie_rgb = texture(cookie_texture, across + 0.5).rgb;
	float cookie_luma = dot(cookie_rgb, vec3(0.3333, 0.3333, 0.3333));
	float cookie_mask = mix(1.0, cookie_luma, cookie_strength);

	float drift = TIME * noise_speed * 10.0;
	float n1 = value_noise(local_pos * noise_scale + vec3(0.0, 0.0, drift));
	float n2 = value_noise(local_pos * noise_scale * 2.7 + vec3(4.7, 9.1, -drift * 1.7));
	float noise_factor = mix(1.0, clamp(n1 * 0.7 + n2 * 0.45, 0.0, 1.0), noise_amount);

	float depth_factor = 1.0;
	if (use_depth_fade) {
		float scene_raw = texture(DEPTH_TEXTURE, SCREEN_UV).x;
		vec4 scene_view = INV_PROJECTION_MATRIX * vec4(SCREEN_UV * 2.0 - 1.0, scene_raw, 1.0);
		scene_view.xyz /= scene_view.w;
		float scene_distance = -scene_view.z;
		float fragment_distance = -VERTEX.z;
		depth_factor = clamp((scene_distance - fragment_distance) / max(depth_fade_distance, 0.001), 0.0, 1.0);
	}

	float alpha = intensity * through * axial_fade * cookie_mask * noise_factor * depth_factor;
	ALBEDO = shaft_color.rgb * mix(vec3(1.0), cookie_rgb, cookie_strength);
	ALPHA = alpha;
}
)GLSL";
} // namespace

Vector2 LightVolume3D::get_near_extent() const {
	if (shape == SHAPE_BOX) {
		return size * 0.5;
	}
	return Vector2(start_radius, start_radius);
}

Vector2 LightVolume3D::get_far_extent() const {
	if (shape == SHAPE_BOX) {
		return size * 0.5;
	}
	return Vector2(end_radius, end_radius);
}

void LightVolume3D::_build_mesh() {
	const int radial_segments = 32;
	const bool is_box = (shape == SHAPE_BOX);
	const int ring_points = is_box ? 4 : radial_segments;

	Vector2 near_extent = get_near_extent();
	Vector2 far_extent = get_far_extent();
	Vector3 far_center = Vector3(shear.x, shear.y, -length);

	PackedVector3Array vertices;
	PackedVector3Array normals;
	PackedVector2Array uvs;
	PackedInt32Array indices;

	vertices.resize(ring_points * 2);
	normals.resize(ring_points * 2);
	uvs.resize(ring_points * 2);

	for (int i = 0; i < ring_points; i++) {
		real_t t = Math::TAU * real_t(i) / real_t(ring_points);
		Vector2 direction;
		Vector2 near_point;
		Vector2 far_point;
		Vector3 side_normal;
		if (is_box) {
			// Corners at 45, 135, 225, 315 degrees; the face normal comes
			// from the segment between this corner and the next.
			real_t corner = Math::PI * 0.25 + Math::PI * 0.5 * real_t(i);
			direction = Vector2(Math::cos(corner), Math::sin(corner)).normalized();
			near_point = Vector2((direction.x > 0 ? 1 : -1) * near_extent.x, (direction.y > 0 ? 1 : -1) * near_extent.y);
			far_point = Vector2((direction.x > 0 ? 1 : -1) * far_extent.x, (direction.y > 0 ? 1 : -1) * far_extent.y);
			real_t face = Math::PI * 0.5 * real_t(i);
			side_normal = Vector3(Math::cos(face), Math::sin(face), 0);
		} else {
			direction = Vector2(Math::cos(t), Math::sin(t));
			near_point = direction * near_extent.x;
			far_point = direction * far_extent.x;
			// Frustum side normal tilts with the radius change.
			real_t slope = (near_extent.x - far_extent.x) / MAX(length, 0.001);
			side_normal = Vector3(direction.x, direction.y, slope).normalized();
		}
		vertices[i] = Vector3(near_point.x, near_point.y, 0);
		vertices[i + ring_points] = far_center + Vector3(far_point.x, far_point.y, 0);
		normals[i] = side_normal;
		normals[i + ring_points] = side_normal;
		uvs[i] = Vector2(real_t(i) / real_t(ring_points), 0);
		uvs[i + ring_points] = Vector2(real_t(i) / real_t(ring_points), 1);
	}

	for (int i = 0; i < ring_points; i++) {
		int next = (i + 1) % ring_points;
		int n0 = i;
		int n1 = next;
		int f0 = i + ring_points;
		int f1 = next + ring_points;
		indices.push_back(n0);
		indices.push_back(f0);
		indices.push_back(n1);
		indices.push_back(n1);
		indices.push_back(f0);
		indices.push_back(f1);
	}

	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_INDEX] = indices;

	if (mesh.is_null()) {
		mesh.instantiate();
	} else {
		mesh->clear_surfaces();
	}
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);

	// The AABB covers both rings, including the sheared far end.
	Vector3 near_min = Vector3(-near_extent.x, -near_extent.y, 0);
	Vector3 near_max = Vector3(near_extent.x, near_extent.y, 0);
	Vector3 far_min = far_center + Vector3(-far_extent.x, -far_extent.y, 0);
	Vector3 far_max = far_center + Vector3(far_extent.x, far_extent.y, 0);
	aabb = AABB(near_min.min(far_min), Vector3());
	aabb.size = near_max.max(far_max) - aabb.position;
	mesh->set_custom_aabb(aabb);

	RenderingServer::get_singleton()->instance_set_base(get_instance(), mesh->get_rid());
	_update_material();
}

void LightVolume3D::_update_material() {
	if (material.is_null()) {
		return;
	}
	material->set_shader_parameter("shaft_color", color);
	material->set_shader_parameter("intensity", intensity);
	material->set_shader_parameter("edge_softness", edge_softness);
	material->set_shader_parameter("fade_start", fade_start);
	material->set_shader_parameter("fade_end", fade_end);
	material->set_shader_parameter("shaft_length", length);
	material->set_shader_parameter("near_extent", get_near_extent());
	material->set_shader_parameter("far_extent", get_far_extent());
	material->set_shader_parameter("shear_offset", shear);
	material->set_shader_parameter("cookie_texture", cookie.is_valid() ? Variant(cookie.ptr()) : Variant(white_texture.ptr()));
	material->set_shader_parameter("cookie_strength", cookie.is_valid() ? cookie_strength : 0.0);
	material->set_shader_parameter("noise_amount", noise_amount);
	material->set_shader_parameter("noise_scale", noise_scale);
	material->set_shader_parameter("noise_speed", noise_speed);
	material->set_shader_parameter("use_depth_fade", depth_fade);
	material->set_shader_parameter("depth_fade_distance", depth_fade_distance);
}

void LightVolume3D::_apply_light(Light3D *p_light) {
	if (p_light == nullptr) {
		return;
	}
	color = p_light->get_color();
	if (auto_fit) {
		SpotLight3D *spot = Object::cast_to<SpotLight3D>(p_light);
		OmniLight3D *omni = Object::cast_to<OmniLight3D>(p_light);
		if (spot != nullptr) {
			length = MAX(spot->get_param(Light3D::PARAM_RANGE), 0.1);
			if (shape == SHAPE_CONE) {
				real_t angle_rad = Math::deg_to_rad(spot->get_param(Light3D::PARAM_SPOT_ANGLE));
				end_radius = Math::tan(angle_rad) * length;
			}
			_build_mesh();
		} else if (omni != nullptr) {
			length = MAX(omni->get_param(Light3D::PARAM_RANGE), 0.1);
			_build_mesh();
		}
	}
	// Energy scales the shaft on top of the user's intensity multiplier.
	material->set_shader_parameter("shaft_color", color);
	material->set_shader_parameter("intensity", intensity * p_light->get_param(Light3D::PARAM_ENERGY));
	update_configuration_warnings();
}

void LightVolume3D::_update_processing() {
	set_process(is_inside_tree() && follow_light && !source_light.is_empty());
}

void LightVolume3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_update_processing();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			set_process(false);
		} break;
		case NOTIFICATION_PROCESS: {
			if (!source_light.is_empty()) {
				_apply_light(Object::cast_to<Light3D>(get_node_or_null(source_light)));
			}
		} break;
	}
}

void LightVolume3D::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name == "size" && shape != SHAPE_BOX) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	if ((p_property.name == "start_radius" || p_property.name == "end_radius") && shape == SHAPE_BOX) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	if (p_property.name == "cookie_strength" && cookie.is_null()) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

PackedStringArray LightVolume3D::get_configuration_warnings() const {
	PackedStringArray warnings = VisualInstance3D::get_configuration_warnings();
	if (!source_light.is_empty()) {
		Node *node = get_node_or_null(source_light);
		if (node == nullptr) {
			warnings.push_back("Source light path does not point at a node. The shaft keeps its own colour and shape.");
		} else if (Object::cast_to<Light3D>(node) == nullptr) {
			warnings.push_back("Source light path points at something that is not a Light3D.");
		}
	}
	if (length <= 0.0) {
		warnings.push_back("Length is zero or negative: the shaft has no volume.");
	}
	return warnings;
}

AABB LightVolume3D::get_aabb() const {
	return aabb;
}

void LightVolume3D::set_shape(Shape p_shape) {
	if (shape == p_shape) {
		return;
	}
	shape = p_shape;
	_build_mesh();
	notify_property_list_changed();
}

LightVolume3D::Shape LightVolume3D::get_shape() const {
	return shape;
}

void LightVolume3D::set_length(real_t p_length) {
	length = MAX(p_length, 0.0);
	_build_mesh();
}

real_t LightVolume3D::get_length() const {
	return length;
}

void LightVolume3D::set_size(const Vector2 &p_size) {
	size = Vector2(MAX(p_size.x, 0.0), MAX(p_size.y, 0.0));
	_build_mesh();
}

Vector2 LightVolume3D::get_size() const {
	return size;
}

void LightVolume3D::set_start_radius(real_t p_radius) {
	start_radius = MAX(p_radius, 0.0);
	_build_mesh();
}

real_t LightVolume3D::get_start_radius() const {
	return start_radius;
}

void LightVolume3D::set_end_radius(real_t p_radius) {
	end_radius = MAX(p_radius, 0.0);
	_build_mesh();
}

real_t LightVolume3D::get_end_radius() const {
	return end_radius;
}

void LightVolume3D::set_shear(const Vector2 &p_shear) {
	shear = p_shear;
	_build_mesh();
}

Vector2 LightVolume3D::get_shear() const {
	return shear;
}

void LightVolume3D::set_color(const Color &p_color) {
	color = p_color;
	_update_material();
}

Color LightVolume3D::get_color() const {
	return color;
}

void LightVolume3D::set_intensity(real_t p_intensity) {
	intensity = MAX(p_intensity, 0.0);
	_update_material();
}

real_t LightVolume3D::get_intensity() const {
	return intensity;
}

void LightVolume3D::set_edge_softness(real_t p_softness) {
	edge_softness = CLAMP(p_softness, 0.05, 8.0);
	_update_material();
}

real_t LightVolume3D::get_edge_softness() const {
	return edge_softness;
}

void LightVolume3D::set_fade_start(real_t p_fade) {
	fade_start = CLAMP(p_fade, 0.0, 1.0);
	_update_material();
}

real_t LightVolume3D::get_fade_start() const {
	return fade_start;
}

void LightVolume3D::set_fade_end(real_t p_fade) {
	fade_end = CLAMP(p_fade, 0.0, 1.0);
	_update_material();
}

real_t LightVolume3D::get_fade_end() const {
	return fade_end;
}

void LightVolume3D::set_cookie(const Ref<Texture2D> &p_cookie) {
	cookie = p_cookie;
	_update_material();
	notify_property_list_changed();
}

Ref<Texture2D> LightVolume3D::get_cookie() const {
	return cookie;
}

void LightVolume3D::set_cookie_strength(real_t p_strength) {
	cookie_strength = CLAMP(p_strength, 0.0, 1.0);
	_update_material();
}

real_t LightVolume3D::get_cookie_strength() const {
	return cookie_strength;
}

void LightVolume3D::set_noise_amount(real_t p_amount) {
	noise_amount = CLAMP(p_amount, 0.0, 1.0);
	_update_material();
}

real_t LightVolume3D::get_noise_amount() const {
	return noise_amount;
}

void LightVolume3D::set_noise_scale(real_t p_scale) {
	noise_scale = MAX(p_scale, 0.01);
	_update_material();
}

real_t LightVolume3D::get_noise_scale() const {
	return noise_scale;
}

void LightVolume3D::set_noise_speed(real_t p_speed) {
	noise_speed = p_speed;
	_update_material();
}

real_t LightVolume3D::get_noise_speed() const {
	return noise_speed;
}

void LightVolume3D::set_depth_fade(bool p_enabled) {
	depth_fade = p_enabled;
	_update_material();
}

bool LightVolume3D::get_depth_fade() const {
	return depth_fade;
}

void LightVolume3D::set_depth_fade_distance(real_t p_distance) {
	depth_fade_distance = MAX(p_distance, 0.01);
	_update_material();
}

real_t LightVolume3D::get_depth_fade_distance() const {
	return depth_fade_distance;
}

void LightVolume3D::set_source_light(const NodePath &p_path) {
	source_light = p_path;
	_update_processing();
	if (source_light.is_empty()) {
		_update_material(); // Restore the shaft's own colour and intensity.
	}
	if (follow_light && is_inside_tree() && !source_light.is_empty()) {
		_apply_light(Object::cast_to<Light3D>(get_node_or_null(source_light)));
	}
	update_configuration_warnings();
}

NodePath LightVolume3D::get_source_light() const {
	return source_light;
}

void LightVolume3D::set_follow_light(bool p_follow) {
	follow_light = p_follow;
	_update_processing();
	if (!follow_light) {
		_update_material(); // Restore the shaft's own colour and intensity.
	}
}

bool LightVolume3D::get_follow_light() const {
	return follow_light;
}

void LightVolume3D::set_auto_fit(bool p_fit) {
	auto_fit = p_fit;
	if (auto_fit && follow_light && is_inside_tree() && !source_light.is_empty()) {
		_apply_light(Object::cast_to<Light3D>(get_node_or_null(source_light)));
	}
}

bool LightVolume3D::get_auto_fit() const {
	return auto_fit;
}

void LightVolume3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_shape", "shape"), &LightVolume3D::set_shape);
	ClassDB::bind_method(D_METHOD("get_shape"), &LightVolume3D::get_shape);
	ClassDB::bind_method(D_METHOD("set_length", "length"), &LightVolume3D::set_length);
	ClassDB::bind_method(D_METHOD("get_length"), &LightVolume3D::get_length);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &LightVolume3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &LightVolume3D::get_size);
	ClassDB::bind_method(D_METHOD("set_start_radius", "start_radius"), &LightVolume3D::set_start_radius);
	ClassDB::bind_method(D_METHOD("get_start_radius"), &LightVolume3D::get_start_radius);
	ClassDB::bind_method(D_METHOD("set_end_radius", "end_radius"), &LightVolume3D::set_end_radius);
	ClassDB::bind_method(D_METHOD("get_end_radius"), &LightVolume3D::get_end_radius);
	ClassDB::bind_method(D_METHOD("set_shear", "shear"), &LightVolume3D::set_shear);
	ClassDB::bind_method(D_METHOD("get_shear"), &LightVolume3D::get_shear);
	ClassDB::bind_method(D_METHOD("set_color", "color"), &LightVolume3D::set_color);
	ClassDB::bind_method(D_METHOD("get_color"), &LightVolume3D::get_color);
	ClassDB::bind_method(D_METHOD("set_intensity", "intensity"), &LightVolume3D::set_intensity);
	ClassDB::bind_method(D_METHOD("get_intensity"), &LightVolume3D::get_intensity);
	ClassDB::bind_method(D_METHOD("set_edge_softness", "edge_softness"), &LightVolume3D::set_edge_softness);
	ClassDB::bind_method(D_METHOD("get_edge_softness"), &LightVolume3D::get_edge_softness);
	ClassDB::bind_method(D_METHOD("set_fade_start", "fade_start"), &LightVolume3D::set_fade_start);
	ClassDB::bind_method(D_METHOD("get_fade_start"), &LightVolume3D::get_fade_start);
	ClassDB::bind_method(D_METHOD("set_fade_end", "fade_end"), &LightVolume3D::set_fade_end);
	ClassDB::bind_method(D_METHOD("get_fade_end"), &LightVolume3D::get_fade_end);
	ClassDB::bind_method(D_METHOD("set_cookie", "cookie"), &LightVolume3D::set_cookie);
	ClassDB::bind_method(D_METHOD("get_cookie"), &LightVolume3D::get_cookie);
	ClassDB::bind_method(D_METHOD("set_cookie_strength", "cookie_strength"), &LightVolume3D::set_cookie_strength);
	ClassDB::bind_method(D_METHOD("get_cookie_strength"), &LightVolume3D::get_cookie_strength);
	ClassDB::bind_method(D_METHOD("set_noise_amount", "noise_amount"), &LightVolume3D::set_noise_amount);
	ClassDB::bind_method(D_METHOD("get_noise_amount"), &LightVolume3D::get_noise_amount);
	ClassDB::bind_method(D_METHOD("set_noise_scale", "noise_scale"), &LightVolume3D::set_noise_scale);
	ClassDB::bind_method(D_METHOD("get_noise_scale"), &LightVolume3D::get_noise_scale);
	ClassDB::bind_method(D_METHOD("set_noise_speed", "noise_speed"), &LightVolume3D::set_noise_speed);
	ClassDB::bind_method(D_METHOD("get_noise_speed"), &LightVolume3D::get_noise_speed);
	ClassDB::bind_method(D_METHOD("set_depth_fade", "depth_fade"), &LightVolume3D::set_depth_fade);
	ClassDB::bind_method(D_METHOD("get_depth_fade"), &LightVolume3D::get_depth_fade);
	ClassDB::bind_method(D_METHOD("set_depth_fade_distance", "depth_fade_distance"), &LightVolume3D::set_depth_fade_distance);
	ClassDB::bind_method(D_METHOD("get_depth_fade_distance"), &LightVolume3D::get_depth_fade_distance);
	ClassDB::bind_method(D_METHOD("set_source_light", "source_light"), &LightVolume3D::set_source_light);
	ClassDB::bind_method(D_METHOD("get_source_light"), &LightVolume3D::get_source_light);
	ClassDB::bind_method(D_METHOD("set_follow_light", "follow_light"), &LightVolume3D::set_follow_light);
	ClassDB::bind_method(D_METHOD("get_follow_light"), &LightVolume3D::get_follow_light);
	ClassDB::bind_method(D_METHOD("set_auto_fit", "auto_fit"), &LightVolume3D::set_auto_fit);
	ClassDB::bind_method(D_METHOD("get_auto_fit"), &LightVolume3D::get_auto_fit);
	ClassDB::bind_method(D_METHOD("get_mesh"), &LightVolume3D::get_mesh);
	ClassDB::bind_method(D_METHOD("get_near_extent"), &LightVolume3D::get_near_extent);
	ClassDB::bind_method(D_METHOD("get_far_extent"), &LightVolume3D::get_far_extent);

	ADD_GROUP("Shape", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shape", PROPERTY_HINT_ENUM, "Box,Cone,Cylinder"), "set_shape", "get_shape");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "length", PROPERTY_HINT_RANGE, "0,64,0.01,or_greater,suffix:m"), "set_length", "get_length");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "size"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "start_radius", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater,suffix:m"), "set_start_radius", "get_start_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "end_radius", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater,suffix:m"), "set_end_radius", "get_end_radius");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "shear"), "set_shear", "get_shear");

	ADD_GROUP("Shaft", "");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_color", "get_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "intensity", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"), "set_intensity", "get_intensity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "edge_softness", PROPERTY_HINT_RANGE, "0.05,8,0.05"), "set_edge_softness", "get_edge_softness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fade_start", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_fade_start", "get_fade_start");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fade_end", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_fade_end", "get_fade_end");

	ADD_GROUP("Cookie", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "cookie", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_cookie", "get_cookie");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cookie_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_cookie_strength", "get_cookie_strength");

	ADD_GROUP("Noise", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "noise_amount", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_noise_amount", "get_noise_amount");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "noise_scale", PROPERTY_HINT_RANGE, "0.01,4,0.01"), "set_noise_scale", "get_noise_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "noise_speed", PROPERTY_HINT_RANGE, "-1,1,0.005"), "set_noise_speed", "get_noise_speed");

	ADD_GROUP("Depth Fade", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "depth_fade"), "set_depth_fade", "get_depth_fade");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "depth_fade_distance", PROPERTY_HINT_RANGE, "0.01,4,0.01,suffix:m"), "set_depth_fade_distance", "get_depth_fade_distance");

	ADD_GROUP("Source Light", "");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "source_light", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Light3D"), "set_source_light", "get_source_light");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "follow_light"), "set_follow_light", "get_follow_light");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_fit"), "set_auto_fit", "get_auto_fit");

	BIND_ENUM_CONSTANT(SHAPE_BOX);
	BIND_ENUM_CONSTANT(SHAPE_CONE);
	BIND_ENUM_CONSTANT(SHAPE_CYLINDER);
}

LightVolume3D::LightVolume3D() {
	Ref<Image> white_image = Image::create_empty(1, 1, false, Image::FORMAT_RGBA8);
	white_image->fill(Color(1, 1, 1));
	white_texture = ImageTexture::create_from_image(white_image);

	shader.instantiate();
	shader->set_code(SHAFT_SHADER);
	material.instantiate();
	material->set_shader(shader);

	_build_mesh();
	RenderingServer::get_singleton()->instance_geometry_set_material_override(get_instance(), material->get_rid());
	// Shafts should not be hidden by their own bounding box when the
	// camera is close, and they never write depth (additive shader).
	set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
}

LightVolume3D::~LightVolume3D() {
}
