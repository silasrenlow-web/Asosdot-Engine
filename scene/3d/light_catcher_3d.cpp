/**************************************************************************/
/*  light_catcher_3d.cpp                                                  */
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

#include "light_catcher_3d.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/math/triangle_mesh.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/3d/physics/collision_object_3d.h"
#include "scene/main/viewport.h"
#include "servers/rendering/rendering_server.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/physics_3d/physics_server_3d.h"

// Below this a light is treated as not reaching a point at all.
static constexpr real_t CONTRIBUTION_EPSILON = 1e-5;
// Once lit, a catcher stays lit until it falls this far under the threshold,
// so a flickering light doesn't toggle it every frame.
static constexpr real_t HYSTERESIS = 0.8;

real_t LightCatcher3D::luminance(const Color &p_color) {
	return p_color.r * 0.2126 + p_color.g * 0.7152 + p_color.b * 0.0722;
}

// The renderer's omni falloff (scene_forward_lights_inc.glsl).
static real_t _omni_attenuation(real_t p_distance, real_t p_inv_range, real_t p_decay) {
	real_t nd = p_distance * p_inv_range;
	nd *= nd;
	nd *= nd;
	nd = MAX(1.0 - nd, 0.0);
	nd *= nd;
	return nd * Math::pow(MAX(p_distance, (real_t)0.0001), -p_decay);
}

void LightCatcher3D::_collect_exclusions(Node *p_from, HashSet<RID> &r_exclude) const {
	for (Node *n = p_from; n; n = n->get_parent()) {
		CollisionObject3D *body = Object::cast_to<CollisionObject3D>(n);
		if (body) {
			r_exclude.insert(body->get_rid());
		}
	}
}

bool LightCatcher3D::_blocked(const Vector3 &p_from, const Vector3 &p_to, Light3D *p_light, const HashSet<RID> &p_exclude) const {
	Ref<World3D> world = get_world_3d();
	if (world.is_null()) {
		return false;
	}

	Vector3 ray = p_to - p_from;
	const real_t length = ray.length();
	if (length <= occlusion_margin + SURFACE_OFFSET) {
		return false;
	}
	ray /= length;
	// Leave from just off the point, so a point on a surface (where a beam
	// lands on a wall) isn't shadowed by that surface itself.
	const Vector3 from = p_from + ray * SURFACE_OFFSET;
	const Vector3 to = p_from + ray * (length - occlusion_margin);

	if (occlusion_source != OCCLUSION_SOURCE_SHADOW_CASTERS) {
		PhysicsDirectSpaceState3D *space = world->get_direct_space_state();
		if (space) {
			PhysicsDirectSpaceState3D::RayParameters params;
			params.from = from;
			params.to = to;
			for (const RID &rid : p_exclude) {
				params.exclude.insert(rid);
			}
			// A lamp's own fitting usually has collision around the bulb; it
			// lights the room rather than shadowing it.
			_collect_exclusions(p_light, params.exclude);
			params.collision_mask = occlusion_mask;
			params.collide_with_areas = false;
			PhysicsDirectSpaceState3D::RayResult result;
			if (space->intersect_ray(params, result)) {
				return true;
			}
		}
	}
	if (occlusion_source != OCCLUSION_SOURCE_COLLIDERS) {
		return _shadow_casters_block(from, to, p_light, world->get_scenario());
	}
	return false;
}

// Whether anything the renderer would draw into this light's shadow lies
// between two points: visible meshes that cast shadows, on the light's shadow
// caster mask. This is the geometry the shadow map is made of, so it agrees
// with the screen, at the cost of testing triangles instead of colliders.
bool LightCatcher3D::_shadow_casters_block(const Vector3 &p_from, const Vector3 &p_to, Light3D *p_light, RID p_scenario) const {
	const uint32_t caster_mask = p_light->get_shadow_caster_mask();
	const Vector<ObjectID> found = RS::get_singleton()->instances_cull_ray(p_from, p_to, p_scenario);

	// The catcher's own body (a player model) doesn't shadow itself.
	LocalVector<Node *> own_bodies;
	if (exclude_parent_bodies) {
		for (Node *n = get_parent(); n; n = n->get_parent()) {
			if (Object::cast_to<CollisionObject3D>(n)) {
				own_bodies.push_back(n);
			}
		}
	}

	auto segment_hits = [&](const Ref<Mesh> &p_mesh, const Transform3D &p_xform) -> bool {
		if (p_mesh.is_null()) {
			return false;
		}
		Ref<TriangleMesh> triangles = p_mesh->generate_triangle_mesh();
		if (triangles.is_null()) {
			return false;
		}
		const Transform3D inv = p_xform.affine_inverse();
		Vector3 point, normal;
		return triangles->intersect_segment(inv.xform(p_from), inv.xform(p_to), point, normal);
	};

	for (const ObjectID &id : found) {
		GeometryInstance3D *geometry = ObjectDB::get_instance<GeometryInstance3D>(id);
		if (!geometry || !geometry->is_visible_in_tree() || geometry->get_cast_shadows_setting() == GeometryInstance3D::SHADOW_CASTING_SETTING_OFF) {
			continue;
		}
		if ((geometry->get_layer_mask() & caster_mask) == 0) {
			continue;
		}
		bool own = false;
		for (Node *body : own_bodies) {
			if (body->is_ancestor_of(geometry)) {
				own = true;
				break;
			}
		}
		if (own) {
			continue;
		}

		if (MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(geometry)) {
			if (segment_hits(mesh_instance->get_mesh(), mesh_instance->get_global_transform())) {
				return true;
			}
		} else if (MultiMeshInstance3D *multi = Object::cast_to<MultiMeshInstance3D>(geometry)) {
			Ref<MultiMesh> multimesh = multi->get_multimesh();
			if (multimesh.is_null() || multimesh->get_transform_format() != MultiMesh::TRANSFORM_3D) {
				continue;
			}
			const Ref<Mesh> mesh = multimesh->get_mesh();
			if (mesh.is_null()) {
				continue;
			}
			const AABB mesh_aabb = mesh->get_aabb();
			const Transform3D base = multi->get_global_transform();
			const int count = multimesh->get_visible_instance_count() < 0 ? multimesh->get_instance_count() : multimesh->get_visible_instance_count();
			for (int i = 0; i < count; i++) {
				const Transform3D xform = base * multimesh->get_instance_transform(i);
				if (!xform.xform(mesh_aabb).intersects_segment(p_from, p_to)) {
					continue;
				}
				if (segment_hits(mesh, xform)) {
					return true;
				}
			}
		} else if (geometry->has_method(SNAME("get_meshes"))) {
			// CSG shapes: pairs of transform and mesh, relative to the node.
			const Array meshes = geometry->call(SNAME("get_meshes"));
			for (int i = 0; i + 1 < meshes.size(); i += 2) {
				if (segment_hits(meshes[i + 1], geometry->get_global_transform() * Transform3D(meshes[i]))) {
					return true;
				}
			}
		}
	}
	return false;
}

const Ref<Image> &LightCatcher3D::_projector_image(const Ref<Texture2D> &p_texture) const {
	// Reading a texture back can mean waiting on the GPU, so each one is read
	// once and kept, and read again every few seconds in case it changes.
	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	ProjectorImage *cached = projector_images.getptr(p_texture->get_instance_id());
	if (!cached || now - cached->read_at > PROJECTOR_REFRESH_MSEC) {
		Ref<Image> image = p_texture->get_image();
		if (image.is_valid()) {
			image = image->duplicate();
			if (image->is_compressed()) {
				image->decompress();
			}
			if (image->get_format() != Image::FORMAT_RGBA8) {
				image->convert(Image::FORMAT_RGBA8);
			}
		}
		ProjectorImage entry;
		entry.image = image;
		entry.read_at = now;
		projector_images.insert(p_texture->get_instance_id(), entry);
		cached = projector_images.getptr(p_texture->get_instance_id());
	}
	return cached->image;
}

static Color _sample_bilinear(const Ref<Image> &p_image, Vector2 p_uv) {
	const int w = p_image->get_width();
	const int h = p_image->get_height();
	const real_t x = CLAMP(p_uv.x, (real_t)0.0, (real_t)1.0) * w - 0.5;
	const real_t y = CLAMP(p_uv.y, (real_t)0.0, (real_t)1.0) * h - 0.5;
	const int x0 = CLAMP((int)Math::floor(x), 0, w - 1);
	const int y0 = CLAMP((int)Math::floor(y), 0, h - 1);
	const int x1 = MIN(x0 + 1, w - 1);
	const int y1 = MIN(y0 + 1, h - 1);
	const real_t fx = CLAMP(x - x0, (real_t)0.0, (real_t)1.0);
	const real_t fy = CLAMP(y - y0, (real_t)0.0, (real_t)1.0);
	const Color top = p_image->get_pixel(x0, y0).lerp(p_image->get_pixel(x1, y0), fx);
	const Color bottom = p_image->get_pixel(x0, y1).lerp(p_image->get_pixel(x1, y1), fx);
	return top.lerp(bottom, fy);
}

// What a light's projector (cookie) texture lets through towards a point,
// mapped the way scene_forward_lights_inc.glsl maps it, as a linear colour.
Color LightCatcher3D::_projector_filter(Light3D *p_light, const Vector3 &p_point) const {
	const Ref<Texture2D> texture = p_light->get_projector();
	// The renderer projects a cookie through the light's shadow camera, so a
	// cookie only works on a light with shadows (see Light3D.light_projector).
	if (texture.is_null() || !p_light->has_shadow()) {
		return Color(1, 1, 1, 1);
	}
	const Ref<Image> &image = _projector_image(texture);
	if (image.is_null() || image->is_empty()) {
		return Color(1, 1, 1, 1);
	}
	const Vector3 local = p_light->get_global_transform().affine_inverse().xform(p_point);
	Vector2 uv;
	if (p_light->get_light_type() == RSE::LIGHT_SPOT) {
		// Through the spot's shadow camera: a perspective of the cone, the
		// image upright.
		if (local.z >= 0.0) {
			return Color(0, 0, 0, 1);
		}
		const real_t tan_angle = Math::tan(Math::deg_to_rad(p_light->get_param(Light3D::PARAM_SPOT_ANGLE)));
		const real_t depth = -local.z * MAX(tan_angle, (real_t)1e-4);
		uv = Vector2(0.5 + 0.5 * local.x / depth, 0.5 - 0.5 * local.y / depth);
	} else if (p_light->get_light_type() == RSE::LIGHT_OMNI) {
		// An omni cookie is a panorama. The atlas resamples it to the dual
		// paraboloid the shader reads, which comes out as this: the top row
		// straight down, the left edge towards -Z, turning through -X.
		// Measured against the screen with a cookie that encodes its own UV.
		const Vector3 v = local.normalized();
		uv = Vector2(Math::fposmod(Math::atan2(-v.x, -v.z), (real_t)Math::TAU) / Math::TAU, Math::acos(CLAMP(-v.y, (real_t)-1.0, (real_t)1.0)) / Math::PI);
	} else {
		return Color(1, 1, 1, 1);
	}
	const Color texel = _sample_bilinear(image, uv);
	const Color linear = texel.srgb_to_linear();
	return Color(linear.r * texel.a, linear.g * texel.a, linear.b * texel.a, 1.0);
}

// Lights fade out with distance from the camera, as the renderer fades them.
real_t LightCatcher3D::_distance_fade(Light3D *p_light, real_t &r_shadow_fade) const {
	r_shadow_fade = 1.0;
	if (!p_light->is_distance_fade_enabled() || p_light->get_light_type() == RSE::LIGHT_DIRECTIONAL || !is_inside_tree()) {
		return 1.0;
	}
	const Camera3D *camera = get_viewport()->get_camera_3d();
	if (!camera) {
		return 1.0;
	}
	const real_t distance = camera->get_global_position().distance_to(p_light->get_global_position());
	const real_t length = MAX(p_light->get_distance_fade_length(), (real_t)0.001);
	real_t fade = 1.0;
	if (distance > p_light->get_distance_fade_begin()) {
		fade = Math::smoothstep((real_t)0.0, (real_t)1.0, (real_t)(1.0 - (distance - p_light->get_distance_fade_begin()) / length));
	}
	if (distance > p_light->get_distance_fade_shadow()) {
		r_shadow_fade = Math::smoothstep((real_t)0.0, (real_t)1.0, (real_t)(1.0 - (distance - p_light->get_distance_fade_shadow()) / length));
	}
	return fade;
}

Color LightCatcher3D::_light_at(Light3D *p_light, const Vector3 &p_point, const Vector3 &p_normal, bool p_occlude, const HashSet<RID> &p_exclude) const {
	const Transform3D xform = p_light->get_global_transform();
	const RSE::LightType type = p_light->get_light_type();

	// Energy as the renderer computes it, divided by pi as diffuse shading
	// does, so a white surface facing the light is lit by this much.
	const bool physical = GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/use_physical_light_units");
	real_t energy = p_light->get_param(Light3D::PARAM_ENERGY);
	if (physical) {
		energy *= p_light->get_param(Light3D::PARAM_INTENSITY);
		if (type == RSE::LIGHT_OMNI) {
			energy /= Math::PI * 4.0;
		} else if (type == RSE::LIGHT_SPOT) {
			energy /= Math::PI;
		} else if (type == RSE::LIGHT_AREA) {
			energy /= Math::PI * 2.0;
		}
	} else {
		energy *= Math::PI;
	}
	energy /= Math::PI;
	if (p_light->is_negative()) {
		energy = -energy;
	}
	if (Math::is_zero_approx(energy)) {
		return Color(0, 0, 0, 0);
	}
	Color color = p_light->get_color().srgb_to_linear();
	if (physical) {
		color *= p_light->get_correlated_color().srgb_to_linear();
	}

	Vector3 to_light;
	Vector3 target; // where an occluding ray goes
	real_t attenuation = 1.0;
	switch (type) {
		case RSE::LIGHT_DIRECTIONAL: {
			to_light = xform.basis.get_column(2).normalized();
			target = p_point + to_light * directional_distance;
		} break;
		case RSE::LIGHT_OMNI:
		case RSE::LIGHT_SPOT: {
			const Vector3 rel = xform.origin - p_point;
			const real_t distance = rel.length();
			const real_t range = MAX(p_light->get_param(Light3D::PARAM_RANGE), (real_t)0.001);
			if (distance >= range) {
				return Color(0, 0, 0, 0);
			}
			to_light = distance > 0.0 ? rel / distance : Vector3(0, 1, 0);
			attenuation = _omni_attenuation(distance, 1.0 / range, p_light->get_param(Light3D::PARAM_ATTENUATION));
			if (type == RSE::LIGHT_SPOT) {
				const Vector3 spot_dir = -xform.basis.get_column(2).normalized();
				const real_t cos_angle = Math::cos(Math::deg_to_rad(p_light->get_param(Light3D::PARAM_SPOT_ANGLE)));
				const real_t scos = MAX(-to_light.dot(spot_dir), cos_angle);
				const real_t rim = MAX((real_t)1e-4, (1.0 - scos) / MAX((real_t)1e-4, 1.0 - cos_angle));
				attenuation *= 1.0 - Math::pow(rim, (real_t)(1.0 / MAX(p_light->get_param(Light3D::PARAM_SPOT_ATTENUATION), (real_t)1e-4)));
			}
			target = xform.origin;
		} break;
		case RSE::LIGHT_AREA: {
			// A rectangle lighting the side it faces. Measured from its
			// nearest point, which is close to what the renderer's LTC gives
			// for anything not right up against it.
			const AreaLight3D *area = Object::cast_to<AreaLight3D>(p_light);
			const Vector2 size = area ? area->get_area_size() : Vector2(1, 1);
			const Vector3 facing = -xform.basis.get_column(2).normalized();
			const Vector3 axis_a = xform.basis.get_column(0).normalized();
			const Vector3 axis_b = xform.basis.get_column(1).normalized();
			const Vector3 rel = p_point - xform.origin;
			if (facing.dot(rel) <= 0.0) {
				return Color(0, 0, 0, 0);
			}
			const Vector3 closest = xform.origin + axis_a * CLAMP(rel.dot(axis_a), -size.x * 0.5f, size.x * 0.5f) + axis_b * CLAMP(rel.dot(axis_b), -size.y * 0.5f, size.y * 0.5f);
			const Vector3 to_closest = closest - p_point;
			const real_t distance = to_closest.length();
			const real_t range = MAX(p_light->get_param(Light3D::PARAM_RANGE), (real_t)0.001);
			if (distance >= range) {
				return Color(0, 0, 0, 0);
			}
			to_light = distance > 0.0 ? to_closest / distance : -facing;
			attenuation = _omni_attenuation(distance, 1.0 / range, p_light->get_param(Light3D::PARAM_ATTENUATION)) * MAX(facing.dot(-to_light), (real_t)0.0);
			if (area && !area->is_area_normalizing_energy()) {
				attenuation *= size.x * size.y;
			}
			target = closest;
		} break;
		default: {
			return Color(0, 0, 0, 0);
		}
	}

	real_t shadow_fade = 1.0;
	const real_t fade = _distance_fade(p_light, shadow_fade);
	const real_t n_dot_l = p_normal.is_zero_approx() ? 1.0 : MAX(p_normal.normalized().dot(to_light), (real_t)0.0);
	const real_t strength = energy * attenuation * n_dot_l * fade;
	if (Math::abs(strength) <= CONTRIBUTION_EPSILON) {
		return Color(0, 0, 0, 0);
	}
	Color contribution = color * strength;
	// A cookie shapes the light: a flashlight's dark ring lets nothing through.
	contribution *= _projector_filter(p_light, p_point);
	contribution.a = 1.0;
	if (luminance(contribution) == 0.0) {
		return Color(0, 0, 0, 0);
	}

	if (p_occlude && occlusion != OCCLUSION_DISABLED) {
		const bool test = occlusion == OCCLUSION_ALWAYS || p_light->has_shadow();
		if (test && _blocked(p_point, target, p_light, p_exclude)) {
			// Matching the screen, a shadow fades out with distance as it does.
			const real_t opacity = occlusion == OCCLUSION_SHADOWED_LIGHTS ? p_light->get_param(Light3D::PARAM_SHADOW_OPACITY) * shadow_fade : 1.0;
			contribution *= 1.0 - opacity;
			contribution.a = 1.0;
		}
	}
	return contribution;
}

static bool _light_usable(const Light3D *p_light, const World3D *p_world, uint32_t p_mask) {
	if (!p_light->is_visible_in_tree() || (p_light->get_layer_mask() & p_mask) == 0) {
		return false;
	}
	if (Engine::get_singleton()->is_editor_hint()) {
		// The editor lights the viewport with a preview sun when a scene has
		// none; it isn't part of the game, so it doesn't count.
		const Node *edited = p_light->get_tree()->get_edited_scene_root();
		if (!edited || (p_light != edited && !edited->is_ancestor_of(p_light))) {
			return false;
		}
	} else if (p_light->is_editor_only()) {
		return false;
	}
	return p_light->get_world_3d().ptr() == p_world;
}

Color LightCatcher3D::measure_point_color(const Vector3 &p_global_position, const Vector3 &p_normal) const {
	ERR_FAIL_COND_V(!is_inside_tree(), Color(0, 0, 0, 0));
	Ref<World3D> world = get_world_3d();
	HashSet<RID> exclude;
	if (exclude_parent_bodies) {
		_collect_exclusions(const_cast<LightCatcher3D *>(this), exclude);
	}
	Color total(0, 0, 0, 0);
	for (Light3D *light : Light3D::get_lights_in_tree()) {
		if (_light_usable(light, world.ptr(), light_mask)) {
			total += _light_at(light, p_global_position, p_normal, true, exclude);
		}
	}
	total.a = 1.0;
	return total;
}

real_t LightCatcher3D::measure_point(const Vector3 &p_global_position, const Vector3 &p_normal) const {
	return MAX(luminance(measure_point_color(p_global_position, p_normal)), (real_t)0.0);
}

void LightCatcher3D::_update(double p_delta) {
	if (!is_inside_tree()) {
		return;
	}
	const bool editor = Engine::get_singleton()->is_editor_hint();
	Ref<World3D> world = get_world_3d();
	HashSet<RID> exclude;
	if (exclude_parent_bodies) {
		_collect_exclusions(this, exclude);
	}

	const int count = sample_points.size();
	point_brightness.resize(count);
	HashMap<ObjectID, real_t> found;
	real_t maximum = 0.0;
	real_t sum = 0.0;
	Color maximum_color(0, 0, 0, 0);
	Color sum_color(0, 0, 0, 0);
	const Transform3D xform = get_global_transform();

	for (int i = 0; i < count; i++) {
		const Vector3 point = xform.xform(sample_points[i]);
		Color total(0, 0, 0, 0);
		for (Light3D *light : Light3D::get_lights_in_tree()) {
			if (!_light_usable(light, world.ptr(), light_mask)) {
				continue;
			}
			const Color c = _light_at(light, point, Vector3(), true, exclude);
			const real_t l = luminance(c);
			if (Math::abs(l) > CONTRIBUTION_EPSILON) {
				total += c;
				real_t *existing = found.getptr(light->get_instance_id());
				if (existing) {
					*existing = combine == COMBINE_MAXIMUM ? MAX(*existing, l) : *existing + l / count;
				} else {
					found.insert(light->get_instance_id(), combine == COMBINE_MAXIMUM ? l : l / count);
				}
			}
		}
		const real_t l = MAX(luminance(total), (real_t)0.0);
		point_brightness[i] = l;
		sum += l;
		sum_color += total;
		if (i == 0 || l > maximum) {
			maximum = l;
			maximum_color = total;
		}
	}

	if (count > 0) {
		raw_brightness = combine == COMBINE_MAXIMUM ? maximum : sum / count;
		light_color = combine == COMBINE_MAXIMUM ? maximum_color : sum_color / count;
	} else {
		raw_brightness = 0.0;
		light_color = Color(0, 0, 0, 0);
	}
	light_color.a = 1.0;

	if (response_time > 0.0 && !editor) {
		brightness = Math::lerp(brightness, raw_brightness, (real_t)(1.0 - Math::exp(-p_delta / response_time)));
	} else {
		brightness = raw_brightness;
	}
	contributions = found;

	// Which lights reach the threshold on their own: a flashlight landing on
	// the catcher, a lamp it walks under.
	HashSet<ObjectID> now_catching;
	for (const KeyValue<ObjectID, real_t> &E : contributions) {
		if (E.value >= threshold) {
			now_catching.insert(E.key);
		}
	}

	if (editor) {
		catching = now_catching;
		lit = brightness >= threshold;
		update_gizmos();
		return;
	}

	LocalVector<ObjectID> exited;
	for (const ObjectID &id : catching) {
		if (!now_catching.has(id)) {
			exited.push_back(id);
		}
	}
	LocalVector<ObjectID> entered;
	for (const ObjectID &id : now_catching) {
		if (!catching.has(id)) {
			entered.push_back(id);
		}
	}
	catching = now_catching;
	for (const ObjectID &id : exited) {
		if (Object *light = ObjectDB::get_instance(id)) {
			emit_signal(SNAME("light_exited"), light);
		}
	}
	for (const ObjectID &id : entered) {
		if (Object *light = ObjectDB::get_instance(id)) {
			emit_signal(SNAME("light_entered"), light);
		}
	}

	const bool was_lit = lit;
	if (!lit && brightness >= threshold) {
		lit = true;
	} else if (lit && brightness < threshold * HYSTERESIS) {
		lit = false;
	}
	if (lit != was_lit) {
		emit_signal(SNAME("lit_changed"), lit);
	}
}

void LightCatcher3D::update_now() {
	time_to_update = 0.0;
	_update(0.0);
}

void LightCatcher3D::_update_processing() {
	const bool run = active && is_inside_tree();
	if (Engine::get_singleton()->is_editor_hint()) {
		// In the editor it shows what it would catch on its gizmo.
		set_process_internal(run);
		set_physics_process_internal(false);
	} else {
		// Physics frames, so it agrees with the raycasts it makes.
		set_physics_process_internal(run);
		set_process_internal(false);
	}
}

void LightCatcher3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			time_to_update = 0.0;
			_update_processing();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_update_processing();
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			// The editor redraws the gizmo a few times a second, which is
			// plenty to see lights being placed and moved.
			time_to_update -= get_process_delta_time();
			if (time_to_update <= 0.0) {
				time_to_update = 0.2;
				_update(0.2);
			}
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			const double delta = get_physics_process_delta_time();
			time_to_update -= delta;
			if (time_to_update <= 0.0) {
				const double elapsed = MAX(update_interval, delta);
				time_to_update = update_interval;
				_update(elapsed);
			}
		} break;
	}
}

void LightCatcher3D::set_active(bool p_active) {
	active = p_active;
	_update_processing();
}

bool LightCatcher3D::is_active() const {
	return active;
}

void LightCatcher3D::set_sample_points(const PackedVector3Array &p_points) {
	sample_points = p_points;
	update_gizmos();
	update_configuration_warnings();
}

PackedVector3Array LightCatcher3D::get_sample_points() const {
	return sample_points;
}

void LightCatcher3D::set_combine(Combine p_combine) {
	combine = p_combine;
}

LightCatcher3D::Combine LightCatcher3D::get_combine() const {
	return combine;
}

void LightCatcher3D::set_threshold(real_t p_threshold) {
	threshold = MAX(p_threshold, (real_t)0.0);
}

real_t LightCatcher3D::get_threshold() const {
	return threshold;
}

void LightCatcher3D::set_response_time(real_t p_time) {
	response_time = MAX(p_time, (real_t)0.0);
}

real_t LightCatcher3D::get_response_time() const {
	return response_time;
}

void LightCatcher3D::set_update_interval(real_t p_interval) {
	update_interval = MAX(p_interval, (real_t)0.0);
}

real_t LightCatcher3D::get_update_interval() const {
	return update_interval;
}

void LightCatcher3D::set_light_mask(uint32_t p_mask) {
	light_mask = p_mask;
}

uint32_t LightCatcher3D::get_light_mask() const {
	return light_mask;
}

void LightCatcher3D::set_occlusion(Occlusion p_occlusion) {
	occlusion = p_occlusion;
	notify_property_list_changed();
	update_configuration_warnings();
}

LightCatcher3D::Occlusion LightCatcher3D::get_occlusion() const {
	return occlusion;
}

void LightCatcher3D::set_occlusion_source(OcclusionSource p_source) {
	occlusion_source = p_source;
}

LightCatcher3D::OcclusionSource LightCatcher3D::get_occlusion_source() const {
	return occlusion_source;
}

void LightCatcher3D::set_occlusion_mask(uint32_t p_mask) {
	occlusion_mask = p_mask;
	update_configuration_warnings();
}

uint32_t LightCatcher3D::get_occlusion_mask() const {
	return occlusion_mask;
}

void LightCatcher3D::set_occlusion_margin(real_t p_margin) {
	occlusion_margin = MAX(p_margin, (real_t)0.0);
}

real_t LightCatcher3D::get_occlusion_margin() const {
	return occlusion_margin;
}

void LightCatcher3D::set_directional_distance(real_t p_distance) {
	directional_distance = MAX(p_distance, (real_t)0.01);
}

real_t LightCatcher3D::get_directional_distance() const {
	return directional_distance;
}

void LightCatcher3D::set_exclude_parent_bodies(bool p_exclude) {
	exclude_parent_bodies = p_exclude;
}

bool LightCatcher3D::is_excluding_parent_bodies() const {
	return exclude_parent_bodies;
}

real_t LightCatcher3D::get_brightness() const {
	return brightness;
}

Color LightCatcher3D::get_light_color() const {
	return light_color;
}

bool LightCatcher3D::is_lit() const {
	return lit;
}

real_t LightCatcher3D::get_point_brightness(int p_point) const {
	ERR_FAIL_INDEX_V(p_point, sample_points.size(), 0.0);
	// Not measured yet.
	return p_point < (int)point_brightness.size() ? point_brightness[p_point] : 0.0;
}

TypedArray<Light3D> LightCatcher3D::get_lights() const {
	TypedArray<Light3D> lights;
	for (const KeyValue<ObjectID, real_t> &E : contributions) {
		if (Light3D *light = ObjectDB::get_instance<Light3D>(E.key)) {
			lights.push_back(light);
		}
	}
	return lights;
}

real_t LightCatcher3D::get_light_contribution(Light3D *p_light) const {
	ERR_FAIL_NULL_V(p_light, 0.0);
	const real_t *value = contributions.getptr(p_light->get_instance_id());
	return value ? *value : 0.0;
}

Light3D *LightCatcher3D::get_brightest_light() const {
	Light3D *best = nullptr;
	real_t best_value = 0.0;
	for (const KeyValue<ObjectID, real_t> &E : contributions) {
		if (E.value > best_value) {
			if (Light3D *light = ObjectDB::get_instance<Light3D>(E.key)) {
				best = light;
				best_value = E.value;
			}
		}
	}
	return best;
}

PackedStringArray LightCatcher3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	if (sample_points.is_empty()) {
		warnings.push_back(RTR("A LightCatcher3D needs at least one sample point to measure light at."));
	}
	if (occlusion != OCCLUSION_DISABLED && occlusion_source != OCCLUSION_SOURCE_SHADOW_CASTERS && occlusion_mask == 0) {
		warnings.push_back(RTR("Occlusion is on but the occlusion mask is empty, so walls won't block any light."));
	}
	return warnings;
}

void LightCatcher3D::_validate_property(PropertyInfo &p_property) const {
	if (occlusion == OCCLUSION_DISABLED && (p_property.name == "occlusion_source" || p_property.name == "occlusion_mask" || p_property.name == "occlusion_margin" || p_property.name == "directional_distance" || p_property.name == "exclude_parent_bodies")) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

void LightCatcher3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_active", "active"), &LightCatcher3D::set_active);
	ClassDB::bind_method(D_METHOD("is_active"), &LightCatcher3D::is_active);
	ClassDB::bind_method(D_METHOD("set_sample_points", "points"), &LightCatcher3D::set_sample_points);
	ClassDB::bind_method(D_METHOD("get_sample_points"), &LightCatcher3D::get_sample_points);
	ClassDB::bind_method(D_METHOD("set_combine", "combine"), &LightCatcher3D::set_combine);
	ClassDB::bind_method(D_METHOD("get_combine"), &LightCatcher3D::get_combine);
	ClassDB::bind_method(D_METHOD("set_threshold", "threshold"), &LightCatcher3D::set_threshold);
	ClassDB::bind_method(D_METHOD("get_threshold"), &LightCatcher3D::get_threshold);
	ClassDB::bind_method(D_METHOD("set_response_time", "seconds"), &LightCatcher3D::set_response_time);
	ClassDB::bind_method(D_METHOD("get_response_time"), &LightCatcher3D::get_response_time);
	ClassDB::bind_method(D_METHOD("set_update_interval", "seconds"), &LightCatcher3D::set_update_interval);
	ClassDB::bind_method(D_METHOD("get_update_interval"), &LightCatcher3D::get_update_interval);
	ClassDB::bind_method(D_METHOD("set_light_mask", "mask"), &LightCatcher3D::set_light_mask);
	ClassDB::bind_method(D_METHOD("get_light_mask"), &LightCatcher3D::get_light_mask);
	ClassDB::bind_method(D_METHOD("set_occlusion", "occlusion"), &LightCatcher3D::set_occlusion);
	ClassDB::bind_method(D_METHOD("get_occlusion"), &LightCatcher3D::get_occlusion);
	ClassDB::bind_method(D_METHOD("set_occlusion_source", "source"), &LightCatcher3D::set_occlusion_source);
	ClassDB::bind_method(D_METHOD("get_occlusion_source"), &LightCatcher3D::get_occlusion_source);
	ClassDB::bind_method(D_METHOD("set_occlusion_mask", "mask"), &LightCatcher3D::set_occlusion_mask);
	ClassDB::bind_method(D_METHOD("get_occlusion_mask"), &LightCatcher3D::get_occlusion_mask);
	ClassDB::bind_method(D_METHOD("set_occlusion_margin", "margin"), &LightCatcher3D::set_occlusion_margin);
	ClassDB::bind_method(D_METHOD("get_occlusion_margin"), &LightCatcher3D::get_occlusion_margin);
	ClassDB::bind_method(D_METHOD("set_directional_distance", "distance"), &LightCatcher3D::set_directional_distance);
	ClassDB::bind_method(D_METHOD("get_directional_distance"), &LightCatcher3D::get_directional_distance);
	ClassDB::bind_method(D_METHOD("set_exclude_parent_bodies", "exclude"), &LightCatcher3D::set_exclude_parent_bodies);
	ClassDB::bind_method(D_METHOD("is_excluding_parent_bodies"), &LightCatcher3D::is_excluding_parent_bodies);

	ClassDB::bind_method(D_METHOD("get_brightness"), &LightCatcher3D::get_brightness);
	ClassDB::bind_method(D_METHOD("get_light_color"), &LightCatcher3D::get_light_color);
	ClassDB::bind_method(D_METHOD("is_lit"), &LightCatcher3D::is_lit);
	ClassDB::bind_method(D_METHOD("get_point_brightness", "point"), &LightCatcher3D::get_point_brightness);
	ClassDB::bind_method(D_METHOD("get_lights"), &LightCatcher3D::get_lights);
	ClassDB::bind_method(D_METHOD("get_light_contribution", "light"), &LightCatcher3D::get_light_contribution);
	ClassDB::bind_method(D_METHOD("get_brightest_light"), &LightCatcher3D::get_brightest_light);
	ClassDB::bind_method(D_METHOD("measure_point", "global_position", "normal"), &LightCatcher3D::measure_point, DEFVAL(Vector3()));
	ClassDB::bind_method(D_METHOD("measure_point_color", "global_position", "normal"), &LightCatcher3D::measure_point_color, DEFVAL(Vector3()));
	ClassDB::bind_method(D_METHOD("update_now"), &LightCatcher3D::update_now);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "active"), "set_active", "is_active");
	ADD_GROUP("Sampling", "");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR3_ARRAY, "sample_points"), "set_sample_points", "get_sample_points");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "combine", PROPERTY_HINT_ENUM, "Maximum,Average"), "set_combine", "get_combine");
	ADD_GROUP("Detection", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "threshold", PROPERTY_HINT_RANGE, "0,4,0.001,or_greater"), "set_threshold", "get_threshold");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "response_time", PROPERTY_HINT_RANGE, "0,2,0.01,or_greater,suffix:s"), "set_response_time", "get_response_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "update_interval", PROPERTY_HINT_RANGE, "0,1,0.01,or_greater,suffix:s"), "set_update_interval", "get_update_interval");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "light_mask", PROPERTY_HINT_LAYERS_3D_RENDER), "set_light_mask", "get_light_mask");
	ADD_GROUP("Occlusion", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "occlusion", PROPERTY_HINT_ENUM, "Always,Shadowed Lights,Disabled"), "set_occlusion", "get_occlusion");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "occlusion_source", PROPERTY_HINT_ENUM, "Colliders,Shadow Casters,Both"), "set_occlusion_source", "get_occlusion_source");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "occlusion_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_occlusion_mask", "get_occlusion_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "occlusion_margin", PROPERTY_HINT_RANGE, "0,2,0.01,or_greater,suffix:m"), "set_occlusion_margin", "get_occlusion_margin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "directional_distance", PROPERTY_HINT_RANGE, "1,1000,1,or_greater,suffix:m"), "set_directional_distance", "get_directional_distance");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "exclude_parent_bodies"), "set_exclude_parent_bodies", "is_excluding_parent_bodies");

	ADD_SIGNAL(MethodInfo("lit_changed", PropertyInfo(Variant::BOOL, "lit")));
	ADD_SIGNAL(MethodInfo("light_entered", PropertyInfo(Variant::OBJECT, "light", PROPERTY_HINT_RESOURCE_TYPE, "Light3D")));
	ADD_SIGNAL(MethodInfo("light_exited", PropertyInfo(Variant::OBJECT, "light", PROPERTY_HINT_RESOURCE_TYPE, "Light3D")));

	BIND_ENUM_CONSTANT(COMBINE_MAXIMUM);
	BIND_ENUM_CONSTANT(COMBINE_AVERAGE);
	BIND_ENUM_CONSTANT(OCCLUSION_ALWAYS);
	BIND_ENUM_CONSTANT(OCCLUSION_SHADOWED_LIGHTS);
	BIND_ENUM_CONSTANT(OCCLUSION_DISABLED);
	BIND_ENUM_CONSTANT(OCCLUSION_SOURCE_COLLIDERS);
	BIND_ENUM_CONSTANT(OCCLUSION_SOURCE_SHADOW_CASTERS);
	BIND_ENUM_CONSTANT(OCCLUSION_SOURCE_BOTH);
}

LightCatcher3D::LightCatcher3D() {
	set_notify_transform(false);
}
