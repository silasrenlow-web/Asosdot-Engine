/**************************************************************************/
/*  realtime_gi.cpp                                                       */
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

#include "realtime_gi.h"

#include "core/object/callable_mp.h"
#include "core/templates/hashfuncs.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

using namespace RendererRD;

RealtimeGI *RealtimeGI::singleton = nullptr;

RealtimeGI::RealtimeGI() {
	singleton = this;
}

RealtimeGI::~RealtimeGI() {
	singleton = nullptr;
}

void RealtimeGI::init() {
	Vector<String> modes;
	modes.push_back("\n#define MODE_SKY_SH\n");
	modes.push_back("\n#define MODE_SKY_SH_ARRAY\n");
	modes.push_back("\n#define MODE_DIRECT\n");
	modes.push_back("\n#define MODE_RADIOSITY\n");
	modes.push_back("\n#define MODE_CLUSTER\n");
	modes.push_back("\n#define MODE_GATHER\n");
	modes.push_back("\n#define MODE_OUTPUT\n");
	modes.push_back("\n#define MODE_PROBES\n");
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}

	// The sky pass always binds a sky texture. Without a sky, it reads black
	// and lights with the flat colour instead.
	TextureStorage *texture_storage = TextureStorage::get_singleton();
	RID sampler = MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	for (int i = 0; i < 2; i++) {
		Vector<RD::Uniform> uniforms;
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.binding = 0;
		u.append_id(texture_storage->texture_rd_get_default(i == 0 ? TextureStorage::DEFAULT_RD_TEXTURE_BLACK : TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK));
		uniforms.push_back(u);
		RD::Uniform s;
		s.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		s.binding = 1;
		s.append_id(sampler);
		uniforms.push_back(s);
		default_sky_uniform_sets[i] = RD::get_singleton()->uniform_set_create(uniforms, shader.version_get_shader(shader_version, i == 0 ? MODE_SKY_SH : MODE_SKY_SH_ARRAY), 1);
	}
}

void RealtimeGI::free() {
	for (int i = 0; i < 2; i++) {
		if (default_sky_uniform_sets[i].is_valid() && RD::get_singleton()->uniform_set_is_valid(default_sky_uniform_sets[i])) {
			RD::get_singleton()->free_rid(default_sky_uniform_sets[i]);
		}
	}
	if (shader_version.is_valid()) {
		shader.version_free(shader_version);
	}
}

RID RealtimeGI::realtime_gi_allocate() {
	return realtime_gi_owner.allocate_rid();
}

void RealtimeGI::realtime_gi_initialize(RID p_rid) {
	realtime_gi_owner.initialize_rid(p_rid, Data());
	Data *gi = realtime_gi_owner.get_or_null(p_rid);

	// The lightmap lives as long as the GI does, so the scene culler can make
	// a lightmap instance from it as soon as the GI is placed in a scenario.
	LightStorage *light_storage = LightStorage::get_singleton();
	gi->lightmap = light_storage->lightmap_allocate();
	light_storage->lightmap_initialize(gi->lightmap);
	light_storage->lightmap_set_baked_exposure_normalization(gi->lightmap, 1.0);
}

void RealtimeGI::realtime_gi_free(RID p_rid) {
	Data *gi = realtime_gi_owner.get_or_null(p_rid);
	ERR_FAIL_NULL(gi);

	// Let the culler drop its lightmap instance before the lightmap goes.
	gi->dependency.deleted_notify(p_rid);

	_free_gpu(gi);
	LightStorage::get_singleton()->lightmap_free(gi->lightmap);
	realtime_gi_owner.free(p_rid);
}

void RealtimeGI::_free_gpu(Data *p_gi) {
	if (p_gi->lightmap.is_valid()) {
		LightStorage::get_singleton()->lightmap_set_textures(p_gi->lightmap, RID(), false);
	}
	if (p_gi->light_texture.is_valid()) {
		TextureStorage::get_singleton()->texture_free(p_gi->light_texture);
		p_gi->light_texture = RID();
	}

	RID *buffers[] = {
		&p_gi->patch_buffer,
		&p_gi->sky_visibility_buffer,
		&p_gi->cluster_offset_buffer,
		&p_gi->texel_patch_buffer,
		&p_gi->transport_offset_buffer,
		&p_gi->transport_entry_buffer,
		&p_gi->bvh_buffer,
		&p_gi->triangle_buffer,
		&p_gi->light_buffer,
		&p_gi->direct_buffer,
		&p_gi->radiosity_buffer,
		&p_gi->cluster_buffer,
		&p_gi->indirect_buffer,
		&p_gi->sky_sh_buffer,
		&p_gi->probe_ray_buffer,
		&p_gi->probe_sh_buffer,
		&p_gi->output_texture,
	};
	// Freeing a buffer frees the uniform sets that use it.
	for (RID *buffer : buffers) {
		if (buffer->is_valid()) {
			RD::get_singleton()->free_rid(*buffer);
			*buffer = RID();
		}
	}
	for (int i = 0; i < MODE_MAX; i++) {
		p_gi->uniform_sets[i] = RID();
	}
	p_gi->sky_uniform_set = RID();
	p_gi->sky_uniform_set_texture = RID();
	p_gi->light_capacity = 0;
	p_gi->uploaded = false;
}

template <typename T>
static RID _make_buffer(const Vector<T> &p_data, uint32_t p_min_size = 16) {
	uint32_t size = p_data.size() * sizeof(T);
	if (size < p_min_size) {
		Vector<uint8_t> zero;
		zero.resize(p_min_size);
		zero.fill(0);
		if (size) {
			memcpy(zero.ptrw(), p_data.ptr(), size);
		}
		return RD::get_singleton()->storage_buffer_create(p_min_size, zero);
	}
	return RD::get_singleton()->storage_buffer_create(size, p_data.span().template reinterpret<uint8_t>());
}

static RID _make_zero_buffer(uint32_t p_size) {
	Vector<uint8_t> zero;
	zero.resize(MAX(p_size, 16u));
	zero.fill(0);
	return RD::get_singleton()->storage_buffer_create(zero.size(), zero);
}

void RealtimeGI::_upload(Data *p_gi) {
	_free_gpu(p_gi);
	p_gi->dirty = false;
	p_gi->serial++;
	p_gi->readback_pending = false;
	p_gi->settle_frames = 0;
	p_gi->state_hash = 0;

	p_gi->patch_count = p_gi->patches.size() / 16;
	p_gi->cluster_count = MAX(p_gi->cluster_offsets.size() - 1, 0);
	p_gi->probe_count = p_gi->probe_rays > 0 ? p_gi->probe_points.size() : 0;

	const Vector2i atlas = p_gi->atlas_size;
	bool valid = p_gi->patch_count > 0 && p_gi->cluster_count > 0 && atlas.x > 0 && atlas.y > 0 &&
			p_gi->texel_patches.size() == atlas.x * atlas.y &&
			p_gi->transport_offsets.size() == int(p_gi->patch_count) + 1 &&
			p_gi->sky_visibility.size() == int(p_gi->patch_count) * 9 &&
			p_gi->transport_rays > 0 && p_gi->bvh.size() >= 8;
	if (!valid) {
		// Data still arriving through the setters, or nothing baked yet.
		return;
	}
	if (p_gi->probe_count > 0 && p_gi->probe_ray_hits.size() != int(p_gi->probe_count) * p_gi->probe_rays) {
		p_gi->probe_count = 0;
	}

	p_gi->patch_buffer = _make_buffer(p_gi->patches);
	p_gi->sky_visibility_buffer = _make_buffer(p_gi->sky_visibility);
	p_gi->cluster_offset_buffer = _make_buffer(p_gi->cluster_offsets);
	p_gi->texel_patch_buffer = _make_buffer(p_gi->texel_patches);
	p_gi->transport_offset_buffer = _make_buffer(p_gi->transport_offsets);
	p_gi->transport_entry_buffer = _make_buffer(p_gi->transport_entries);
	p_gi->bvh_buffer = _make_buffer(p_gi->bvh);
	p_gi->triangle_buffer = _make_buffer(p_gi->triangles);
	p_gi->direct_buffer = _make_zero_buffer(p_gi->patch_count * 2 * 16);
	p_gi->radiosity_buffer = _make_zero_buffer(p_gi->patch_count * 16);
	p_gi->cluster_buffer = _make_zero_buffer(p_gi->cluster_count * 16);
	p_gi->indirect_buffer = _make_zero_buffer(p_gi->patch_count * 16);
	p_gi->sky_sh_buffer = _make_zero_buffer(9 * 16);
	p_gi->probe_ray_buffer = _make_buffer(p_gi->probe_ray_hits);
	p_gi->probe_sh_buffer = _make_zero_buffer(MAX(p_gi->probe_count, 1u) * 9 * 16);
	p_gi->light_capacity = 8;
	p_gi->light_buffer = _make_zero_buffer(p_gi->light_capacity * sizeof(LightData));

	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	tf.width = atlas.x;
	tf.height = atlas.y;
	// Lightmaps are sampled as arrays, and the texture storage only takes an
	// RD array texture with more than one layer. Only layer 0 is written.
	tf.array_layers = 2;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	p_gi->output_texture = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(p_gi->output_texture, "Realtime GI Lightmap");
	RD::get_singleton()->texture_clear(p_gi->output_texture, Color(0, 0, 0, 1), 0, 1, 0, 2);

	TextureStorage *texture_storage = TextureStorage::get_singleton();
	p_gi->light_texture = texture_storage->texture_allocate();
	texture_storage->texture_rd_initialize(p_gi->light_texture, p_gi->output_texture, RSE::TEXTURE_LAYERED_2D_ARRAY);

	LightStorage *light_storage = LightStorage::get_singleton();
	light_storage->lightmap_set_textures(p_gi->lightmap, p_gi->light_texture, false);
	light_storage->lightmap_set_probe_bounds(p_gi->lightmap, p_gi->bounds);
	light_storage->lightmap_set_probe_interior(p_gi->lightmap, p_gi->interior);

	// Probes start dark and fill in with the first readback a few frames on.
	PackedColorArray sh;
	if (p_gi->probe_count > 0) {
		sh.resize(p_gi->probe_count * 9);
		sh.fill(Color(0, 0, 0, 0));
		light_storage->lightmap_set_probe_capture_data(p_gi->lightmap, p_gi->probe_points, sh, p_gi->probe_tetrahedra, p_gi->probe_bsp);
	} else {
		light_storage->lightmap_set_probe_capture_data(p_gi->lightmap, PackedVector3Array(), PackedColorArray(), PackedInt32Array(), PackedInt32Array());
	}
	p_gi->probes_changed = true;

	p_gi->uploaded = true;
}

RID RealtimeGI::_get_uniform_set(Data *p_gi, Mode p_mode) {
	if (p_gi->uniform_sets[p_mode].is_valid() && RD::get_singleton()->uniform_set_is_valid(p_gi->uniform_sets[p_mode])) {
		return p_gi->uniform_sets[p_mode];
	}

	Vector<RD::Uniform> uniforms;
	const RID storage[] = {
		RID(), // binding 0 is unused
		p_gi->patch_buffer,
		p_gi->sky_visibility_buffer,
		p_gi->cluster_offset_buffer,
		p_gi->texel_patch_buffer,
		p_gi->transport_offset_buffer,
		p_gi->transport_entry_buffer,
		p_gi->bvh_buffer,
		p_gi->triangle_buffer,
		p_gi->light_buffer,
		p_gi->direct_buffer,
		p_gi->radiosity_buffer,
		p_gi->cluster_buffer,
		p_gi->indirect_buffer,
		p_gi->sky_sh_buffer,
		p_gi->probe_ray_buffer,
		p_gi->probe_sh_buffer,
	};
	for (uint32_t i = 1; i < sizeof(storage) / sizeof(storage[0]); i++) {
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = i;
		u.append_id(storage[i]);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 17;
		u.append_id(p_gi->output_texture);
		uniforms.push_back(u);
	}

	p_gi->uniform_sets[p_mode] = RD::get_singleton()->uniform_set_create(uniforms, shader.version_get_shader(shader_version, p_mode), 0);
	return p_gi->uniform_sets[p_mode];
}

RID RealtimeGI::_get_sky_uniform_set(Data *p_gi, RID p_radiance, bool p_array) {
	if (p_radiance.is_null()) {
		return default_sky_uniform_sets[0];
	}
	if (p_gi->sky_uniform_set.is_valid() && p_gi->sky_uniform_set_texture == p_radiance && RD::get_singleton()->uniform_set_is_valid(p_gi->sky_uniform_set)) {
		return p_gi->sky_uniform_set;
	}
	Vector<RD::Uniform> uniforms;
	RD::Uniform u;
	u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
	u.binding = 0;
	u.append_id(p_radiance);
	uniforms.push_back(u);
	RD::Uniform s;
	s.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
	s.binding = 1;
	s.append_id(MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));
	uniforms.push_back(s);
	p_gi->sky_uniform_set = RD::get_singleton()->uniform_set_create(uniforms, shader.version_get_shader(shader_version, p_array ? MODE_SKY_SH_ARRAY : MODE_SKY_SH), 1);
	p_gi->sky_uniform_set_texture = p_radiance;
	return p_gi->sky_uniform_set;
}

void RealtimeGI::_probes_read(const Vector<uint8_t> &p_data, RID p_rid, uint32_t p_serial) {
	RealtimeGI *self = singleton;
	if (!self) {
		return;
	}
	Data *gi = self->realtime_gi_owner.get_or_null(p_rid);
	if (!gi || gi->serial != p_serial) {
		return; // Freed or rebaked while the copy was in flight.
	}
	gi->readback_pending = false;
	if (p_data.size() < int(gi->probe_count * 9 * 16)) {
		return;
	}

	const float *values = reinterpret_cast<const float *>(p_data.ptr());
	PackedColorArray sh;
	sh.resize(gi->probe_count * 9);
	Color *w = sh.ptrw();
	for (uint32_t i = 0; i < gi->probe_count * 9; i++) {
		w[i] = Color(values[i * 4 + 0], values[i * 4 + 1], values[i * 4 + 2], 0.0);
	}
	LightStorage::get_singleton()->lightmap_set_probe_capture_data(gi->lightmap, gi->probe_points, sh, gi->probe_tetrahedra, gi->probe_bsp);
	gi->probes_changed = true;
}

void RealtimeGI::realtime_gi_set_surfaces(RID p_rid, const Vector2i &p_atlas_size, const Vector<float> &p_patches, const Vector<int32_t> &p_cluster_offsets, const Vector<int32_t> &p_texel_patches) {
	Data *gi = realtime_gi_owner.get_or_null(p_rid);
	ERR_FAIL_NULL(gi);
	ERR_FAIL_COND(p_patches.size() % 16 != 0);
	gi->atlas_size = p_atlas_size;
	gi->patches = p_patches;
	gi->cluster_offsets = p_cluster_offsets;
	gi->texel_patches = p_texel_patches;
	gi->dirty = true;
}

void RealtimeGI::realtime_gi_set_transport(RID p_rid, int p_ray_count, const Vector<int32_t> &p_offsets, const Vector<int32_t> &p_entries, const Vector<float> &p_sky_visibility) {
	Data *gi = realtime_gi_owner.get_or_null(p_rid);
	ERR_FAIL_NULL(gi);
	ERR_FAIL_COND(p_ray_count < 0 || p_ray_count > 1023);
	gi->transport_rays = p_ray_count;
	gi->transport_offsets = p_offsets;
	gi->transport_entries = p_entries;
	gi->sky_visibility = p_sky_visibility;
	gi->dirty = true;
}

void RealtimeGI::realtime_gi_set_geometry(RID p_rid, const AABB &p_bounds, const Vector<int32_t> &p_bvh, const Vector<float> &p_triangles) {
	Data *gi = realtime_gi_owner.get_or_null(p_rid);
	ERR_FAIL_NULL(gi);
	ERR_FAIL_COND(p_bvh.size() % 8 != 0);
	ERR_FAIL_COND(p_triangles.size() % 12 != 0);
	gi->bounds = p_bounds;
	gi->bvh = p_bvh;
	gi->triangles = p_triangles;
	gi->dirty = true;
	gi->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_AABB);
}

void RealtimeGI::realtime_gi_set_probes(RID p_rid, const Vector<Vector3> &p_points, int p_ray_count, const Vector<int32_t> &p_rays, const Vector<int32_t> &p_tetrahedra, const Vector<int32_t> &p_bsp_tree) {
	Data *gi = realtime_gi_owner.get_or_null(p_rid);
	ERR_FAIL_NULL(gi);
	ERR_FAIL_COND(p_ray_count < 0);
	gi->probe_points = p_points;
	gi->probe_rays = p_ray_count;
	gi->probe_ray_hits = p_rays;
	gi->probe_tetrahedra = p_tetrahedra;
	gi->probe_bsp = p_bsp_tree;
	gi->dirty = true;
}

void RealtimeGI::realtime_gi_set_energy(RID p_rid, float p_indirect_energy, float p_environment_energy, float p_albedo_boost, float p_emission_energy) {
	Data *gi = realtime_gi_owner.get_or_null(p_rid);
	ERR_FAIL_NULL(gi);
	gi->indirect_energy = p_indirect_energy;
	gi->environment_energy = p_environment_energy;
	gi->albedo_boost = p_albedo_boost;
	gi->emission_energy = p_emission_energy;
}

void RealtimeGI::realtime_gi_set_quality(RID p_rid, int p_bounces_per_frame, int p_shadow_samples, int p_probe_update_interval) {
	Data *gi = realtime_gi_owner.get_or_null(p_rid);
	ERR_FAIL_NULL(gi);
	gi->bounces_per_frame = CLAMP(p_bounces_per_frame, 1, 8);
	gi->shadow_samples = CLAMP(p_shadow_samples, 1, 16);
	gi->probe_update_interval = MAX(p_probe_update_interval, 1);
}

void RealtimeGI::realtime_gi_set_interior(RID p_rid, bool p_interior) {
	Data *gi = realtime_gi_owner.get_or_null(p_rid);
	ERR_FAIL_NULL(gi);
	gi->interior = p_interior;
	LightStorage::get_singleton()->lightmap_set_probe_interior(gi->lightmap, p_interior);
}

AABB RealtimeGI::realtime_gi_get_bounds(RID p_rid) const {
	const Data *gi = realtime_gi_owner.get_or_null(p_rid);
	ERR_FAIL_NULL_V(gi, AABB());
	return gi->bounds;
}

RID RealtimeGI::realtime_gi_get_lightmap(RID p_rid) const {
	const Data *gi = realtime_gi_owner.get_or_null(p_rid);
	ERR_FAIL_NULL_V(gi, RID());
	return gi->lightmap;
}

Dependency *RealtimeGI::realtime_gi_get_dependency(RID p_rid) const {
	Data *gi = realtime_gi_owner.get_or_null(p_rid);
	ERR_FAIL_NULL_V(gi, nullptr);
	return &gi->dependency;
}

bool RealtimeGI::realtime_gi_update(RID p_rid, const Transform3D &p_transform, const Vector<RendererGI::RealtimeGILight> &p_lights, RID p_environment) {
	Data *gi = realtime_gi_owner.get_or_null(p_rid);
	ERR_FAIL_NULL_V(gi, false);

	if (gi->dirty) {
		_upload(gi);
	}
	bool probes_changed = gi->probes_changed;
	gi->probes_changed = false;
	if (!gi->uploaded) {
		return probes_changed;
	}

	RendererSceneRenderRD *scene_render = RendererSceneRenderRD::get_singleton();
	const bool physical_units = scene_render->is_using_physical_light_units();

	// Lights, in the GI's own space.
	Transform3D to_local = p_transform.affine_inverse();
	LocalVector<LightData> light_data;
	light_data.reserve(p_lights.size());
	for (const RendererGI::RealtimeGILight &light : p_lights) {
		if (light.type == RSE::LIGHT_AREA) {
			continue; // Not yet.
		}
		float energy = light.energy;
		if (physical_units) {
			energy *= light.intensity;
			if (light.type == RSE::LIGHT_OMNI) {
				energy *= 1.0 / (Math::PI * 4.0);
			} else if (light.type == RSE::LIGHT_SPOT) {
				energy *= 1.0 / Math::PI;
			}
		} else {
			energy *= Math::PI;
		}
		// Diffuse shading divides by pi; fold it in here.
		energy /= Math::PI;
		Color color = light.color.srgb_to_linear() * energy;

		LightData ld = {};
		Vector3 position = to_local.xform(light.transform.origin);
		Vector3 direction = to_local.basis.xform(light.transform.basis.xform(Vector3(0, 0, -1))).normalized();
		ld.position[0] = position.x;
		ld.position[1] = position.y;
		ld.position[2] = position.z;
		ld.direction[0] = direction.x;
		ld.direction[1] = direction.y;
		ld.direction[2] = direction.z;
		ld.type = light.type == RSE::LIGHT_DIRECTIONAL ? 0 : (light.type == RSE::LIGHT_OMNI ? 1 : 2);
		ld.inv_range = 1.0 / MAX(0.001f, light.range);
		ld.color[0] = color.r;
		ld.color[1] = color.g;
		ld.color[2] = color.b;
		ld.attenuation = light.attenuation;
		ld.cos_spot_angle = Math::cos(Math::deg_to_rad(light.spot_angle));
		ld.inv_spot_attenuation = 1.0 / MAX(0.0001f, light.spot_attenuation);
		ld.indirect_energy = light.indirect_energy;
		ld.flags = (light.has_shadow ? LIGHT_FLAG_SHADOW : 0) | (light.bake_static ? LIGHT_FLAG_STATIC : 0);
		light_data.push_back(ld);
	}

	PushConstant pc = {};
	pc.patch_count = gi->patch_count;
	pc.cluster_count = gi->cluster_count;
	pc.light_count = light_data.size();
	pc.atlas_width = gi->atlas_size.x;
	pc.atlas_height = gi->atlas_size.y;
	pc.direct_stride = 1;
	pc.transport_rays = gi->transport_rays;
	pc.shadow_samples = gi->shadow_samples;
	pc.probe_count = gi->probe_count;
	pc.probe_rays = gi->probe_rays;
	pc.indirect_energy = gi->indirect_energy;
	pc.environment_energy = gi->environment_energy;
	pc.albedo_boost = gi->albedo_boost;
	pc.emission_energy = gi->emission_energy;
	pc.sky_orientation[3] = 1.0;

	// Sky light, chosen the way the scene's own ambient light is, so a
	// surface keeps the same ambient light when realtime GI takes it over.
	Color flat;
	RID radiance;
	bool radiance_array = false;
	float sky_energy = 1.0;
	if (p_environment.is_valid()) {
		RSE::EnvironmentBG bg = scene_render->environment_get_background(p_environment);
		RSE::EnvironmentAmbientSource source = scene_render->environment_get_ambient_source(p_environment);
		float bg_energy = scene_render->environment_get_bg_energy_multiplier(p_environment);
		if (source == RSE::ENV_AMBIENT_SOURCE_BG && (bg == RSE::ENV_BG_CLEAR_COLOR || bg == RSE::ENV_BG_COLOR)) {
			Color c = bg == RSE::ENV_BG_CLEAR_COLOR ? RSG::texture_storage->get_default_clear_color() : scene_render->environment_get_bg_color(p_environment);
			flat = c.srgb_to_linear() * bg_energy;
		} else {
			bool use_sky = (source == RSE::ENV_AMBIENT_SOURCE_BG && bg == RSE::ENV_BG_SKY) || source == RSE::ENV_AMBIENT_SOURCE_SKY;
			if (use_sky || source == RSE::ENV_AMBIENT_SOURCE_COLOR) {
				flat = scene_render->environment_get_ambient_light(p_environment).srgb_to_linear() * scene_render->environment_get_ambient_light_energy(p_environment);
			}
			if (use_sky) {
				SkyRD *sky_rd = scene_render->get_sky();
				SkyRD::Sky *sky = sky_rd->get_sky(scene_render->environment_get_sky(p_environment));
				if (sky && sky->radiance.is_valid()) {
					radiance = sky->radiance;
					radiance_array = sky_rd->sky_use_octmap_array;
					sky_energy = bg_energy;
					pc.sky_mode = 1;
					pc.sky_mix = scene_render->environment_get_ambient_sky_contribution(p_environment);
					pc.sky_lod = MAX(sky_rd->roughness_layers - 1, 0);
					pc.sky_border[0] = sky->uv_border_size;
					pc.sky_border[1] = 1.0 - sky->uv_border_size * 2.0;
					Quaternion q = scene_render->environment_get_sky_orientation(p_environment).get_quaternion().inverse();
					pc.sky_orientation[0] = q.x;
					pc.sky_orientation[1] = q.y;
					pc.sky_orientation[2] = q.z;
					pc.sky_orientation[3] = q.w;
				}
			}
		}
	} else {
		flat = RSG::texture_storage->get_default_clear_color().srgb_to_linear();
	}
	if (sky_energy < 0.0001) {
		// A sky with no energy adds nothing; keep only the flat part of the mix.
		flat *= 1.0 - pc.sky_mix;
		pc.sky_mode = 0;
		radiance = RID();
		sky_energy = 1.0;
	}
	// The texture part of the mix carries the background energy; the flat
	// part already includes its own. Dividing it out here lets the shader
	// scale both by one factor.
	pc.sky_color[0] = flat.r / sky_energy;
	pc.sky_color[1] = flat.g / sky_energy;
	pc.sky_color[2] = flat.b / sky_energy;
	pc.environment_energy *= sky_energy;

	// Nothing to do once the lighting has converged and nothing changed.
	const uint32_t light_hash = hash_murmur3_one_32(gi->shadow_samples, hash_murmur3_buffer(light_data.ptr(), light_data.size() * sizeof(LightData)));
	uint64_t hash = hash_murmur3_buffer(&pc, sizeof(PushConstant), light_hash);
	hash = hash_murmur3_one_64(radiance.get_id(), hash);
	hash = hash_murmur3_one_32(gi->bounces_per_frame, hash);
	if (hash != gi->state_hash) {
		gi->state_hash = hash;
		gi->settle_frames = 0;
	}
	gi->frame++;
	gi->frames_since_probes++;
	bool settled = gi->settle_frames >= SETTLE_FRAMES;
	if (settled && (gi->frame % SETTLED_INTERVAL) != 0) {
		return probes_changed;
	}
	gi->settle_frames++;
	gi->probes_dirty = true;

	RD *rd = RD::get_singleton();

	if (light_data.size() > gi->light_capacity) {
		rd->free_rid(gi->light_buffer);
		while (gi->light_capacity < light_data.size()) {
			gi->light_capacity *= 2;
		}
		gi->light_buffer = _make_zero_buffer(gi->light_capacity * sizeof(LightData));
		for (int i = 0; i < MODE_MAX; i++) {
			gi->uniform_sets[i] = RID();
		}
	}
	if (light_data.size()) {
		rd->buffer_update(gi->light_buffer, 0, light_data.size() * sizeof(LightData), light_data.ptr());
	}

	rd->draw_command_begin_label("Realtime GI");
	// Each pass gets its own compute list so the Visual Profiler can time
	// it; the lists end in a barrier anyway, so this costs nothing.
	auto dispatch = [&](Mode p_mode, const char *p_name, uint32_t p_x, uint32_t p_y = 1) {
		RENDER_TIMESTAMP(p_name);
		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, pipelines[p_mode]);
		rd->compute_list_bind_uniform_set(cl, _get_uniform_set(gi, p_mode), 0);
		if (p_mode == MODE_SKY_SH || p_mode == MODE_SKY_SH_ARRAY) {
			rd->compute_list_bind_uniform_set(cl, _get_sky_uniform_set(gi, radiance, radiance_array), 1);
		}
		rd->compute_list_set_push_constant(cl, &pc, sizeof(PushConstant));
		rd->compute_list_dispatch_threads(cl, p_x, p_y, 1);
		rd->compute_list_end();
	};

	dispatch(radiance.is_valid() && radiance_array ? MODE_SKY_SH_ARRAY : MODE_SKY_SH, "Realtime GI Sky", 64);
	// Direct light only changes when a light does; the bounce keeps
	// converging either way. Large scenes relight a share of their patches
	// each frame, so every patch catches up within a few frames.
	const uint32_t spread = CLAMP((gi->patch_count + DIRECT_PATCHES_PER_FRAME - 1) / DIRECT_PATCHES_PER_FRAME, 1u, MAX_DIRECT_SPREAD);
	if (light_hash != gi->light_hash || gi->settle_frames <= 1) {
		gi->light_hash = light_hash;
		gi->direct_frames_left = spread;
	}
	if (gi->direct_frames_left > 0) {
		gi->direct_frames_left--;
		pc.direct_stride = spread;
		pc.direct_offset = gi->frame % spread;
		dispatch(MODE_DIRECT, "Realtime GI Direct", (gi->patch_count + spread - 1 - pc.direct_offset) / spread);
		pc.direct_stride = 1;
		pc.direct_offset = 0;
	}
	for (int i = 0; i < gi->bounces_per_frame; i++) {
		dispatch(MODE_RADIOSITY, "Realtime GI Radiosity", gi->patch_count);
		dispatch(MODE_CLUSTER, "Realtime GI Clusters", gi->cluster_count);
		dispatch(MODE_GATHER, "Realtime GI Gather", gi->patch_count);
	}
	dispatch(MODE_OUTPUT, "Realtime GI Lightmap", gi->atlas_size.x, gi->atlas_size.y);

	bool read_probes = false;
	if (gi->probe_count > 0 && gi->probes_dirty && !gi->readback_pending && gi->frames_since_probes >= uint32_t(gi->probe_update_interval)) {
		dispatch(MODE_PROBES, "Realtime GI Probes", gi->probe_count);
		read_probes = true;
	}
	RENDER_TIMESTAMP("Realtime GI Done");
	rd->draw_command_end_label();

	if (read_probes) {
		gi->frames_since_probes = 0;
		gi->probes_dirty = false;
		gi->readback_pending = true;
		rd->buffer_get_data_async(gi->probe_sh_buffer, callable_mp_static(&RealtimeGI::_probes_read).bind(p_rid, gi->serial), 0, gi->probe_count * 9 * 16);
	}

	return probes_changed;
}
