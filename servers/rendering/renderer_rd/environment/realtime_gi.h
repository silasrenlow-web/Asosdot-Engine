/**************************************************************************/
/*  realtime_gi.h                                                         */
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

#include "core/templates/local_vector.h"
#include "core/templates/rid_owner.h"
#include "servers/rendering/environment/renderer_gi.h"
#include "servers/rendering/renderer_rd/shaders/environment/realtime_gi.glsl.gen.h"
#include "servers/rendering/storage/utilities.h"

namespace RendererRD {

// Asosdot's realtime global illumination. The scene node bakes how light
// travels between static surfaces; this runs that transport on the GPU every
// frame with whatever lights the scene has now, and writes the result into a
// lightmap that static surfaces sample and probes that dynamic objects read.
class RealtimeGI {
public:
	enum Mode {
		MODE_SKY_SH,
		MODE_SKY_SH_ARRAY,
		MODE_DIRECT,
		MODE_RADIOSITY,
		MODE_CLUSTER,
		MODE_GATHER,
		MODE_OUTPUT,
		MODE_PROBES,
		MODE_MAX
	};

private:
	static RealtimeGI *singleton;

	struct PushConstant {
		uint32_t patch_count;
		uint32_t cluster_count;
		uint32_t light_count;
		uint32_t atlas_width;

		uint32_t transport_rays;
		uint32_t shadow_samples;
		uint32_t probe_count;
		uint32_t probe_rays;

		float indirect_energy;
		float environment_energy;
		float albedo_boost;
		float emission_energy;

		float sky_color[3];
		uint32_t sky_mode;

		float sky_orientation[4];

		float sky_lod;
		float sky_mix;
		float sky_border[2];

		uint32_t atlas_height;
		uint32_t direct_stride;
		uint32_t direct_offset;
		uint32_t pad;
	};

	static_assert(sizeof(PushConstant) <= 128, "Push constant must fit the guaranteed 128 byte minimum.");

	struct LightData {
		float position[3];
		uint32_t type;
		float direction[3];
		float inv_range;
		float color[3];
		float attenuation;
		float cos_spot_angle;
		float inv_spot_attenuation;
		float indirect_energy;
		uint32_t flags;
	};

	static_assert(sizeof(LightData) == 64, "LightData must match the shader's Light struct.");

	enum {
		LIGHT_FLAG_SHADOW = 1,
		LIGHT_FLAG_STATIC = 2,
	};

	// A scene settles once its lights stop changing. After this many frames
	// with nothing new, every bounce has converged and updates drop to one
	// frame in SETTLED_INTERVAL, enough to follow a slowly changing sky.
	static constexpr uint32_t SETTLE_FRAMES = 48;
	static constexpr uint32_t SETTLED_INTERVAL = 8;
	// Shadow rays are the costly part. Past this many patches, direct light
	// is updated for a share of the patches each frame, up to MAX_DIRECT_SPREAD
	// frames for all of them.
	static constexpr uint32_t DIRECT_PATCHES_PER_FRAME = 32768;
	static constexpr uint32_t MAX_DIRECT_SPREAD = 4;

	struct Data {
		// What the bake gave us. Kept so the buffers can be made again.
		Vector2i atlas_size;
		Vector<float> patches;
		Vector<int32_t> cluster_offsets;
		Vector<int32_t> texel_patches;

		int transport_rays = 0;
		Vector<int32_t> transport_offsets;
		Vector<int32_t> transport_entries;
		Vector<float> sky_visibility;

		AABB bounds;
		Vector<int32_t> bvh;
		Vector<float> triangles;

		Vector<Vector3> probe_points;
		int probe_rays = 0;
		Vector<int32_t> probe_ray_hits;
		Vector<int32_t> probe_tetrahedra;
		Vector<int32_t> probe_bsp;

		float indirect_energy = 1.0;
		float environment_energy = 1.0;
		float albedo_boost = 1.0;
		float emission_energy = 1.0;
		int bounces_per_frame = 1;
		int shadow_samples = 2;
		int probe_update_interval = 4;
		bool interior = false;

		uint32_t patch_count = 0;
		uint32_t cluster_count = 0;
		uint32_t probe_count = 0;

		bool dirty = true;
		bool uploaded = false;

		RID patch_buffer;
		RID sky_visibility_buffer;
		RID cluster_offset_buffer;
		RID texel_patch_buffer;
		RID transport_offset_buffer;
		RID transport_entry_buffer;
		RID bvh_buffer;
		RID triangle_buffer;
		RID light_buffer;
		uint32_t light_capacity = 0;
		RID direct_buffer;
		RID radiosity_buffer;
		RID cluster_buffer;
		RID indirect_buffer;
		RID sky_sh_buffer;
		RID probe_ray_buffer;
		RID probe_sh_buffer;

		RID output_texture; // RD
		RID light_texture; // The RS texture the lightmap holds.
		RID lightmap;

		RID uniform_sets[MODE_MAX];
		RID sky_uniform_set;
		RID sky_uniform_set_texture;

		uint64_t state_hash = 0;
		uint32_t light_hash = 0;
		uint32_t direct_frames_left = 0;
		uint32_t settle_frames = 0;
		uint32_t frame = 0;
		uint32_t frames_since_probes = 0;
		bool probes_dirty = false;
		bool readback_pending = false;
		bool probes_changed = false;
		uint32_t serial = 0; // Bumped on every upload, so a stale readback is dropped.

		Dependency dependency;
	};

	mutable RID_Owner<Data, true> realtime_gi_owner;

	RealtimeGiShaderRD shader;
	RID shader_version;
	RID pipelines[MODE_MAX];
	RID default_sky_uniform_sets[2]; // texture2D, texture2DArray

	void _free_gpu(Data *p_gi);
	void _upload(Data *p_gi);
	RID _get_uniform_set(Data *p_gi, Mode p_mode);
	RID _get_sky_uniform_set(Data *p_gi, RID p_radiance, bool p_array);
	static void _probes_read(const Vector<uint8_t> &p_data, RID p_rid, uint32_t p_serial);

public:
	static RealtimeGI *get_singleton() { return singleton; }

	RealtimeGI();
	~RealtimeGI();

	void init();
	void free();

	bool owns_realtime_gi(RID p_rid) { return realtime_gi_owner.owns(p_rid); }

	RID realtime_gi_allocate();
	void realtime_gi_initialize(RID p_rid);
	void realtime_gi_free(RID p_rid);

	void realtime_gi_set_surfaces(RID p_rid, const Vector2i &p_atlas_size, const Vector<float> &p_patches, const Vector<int32_t> &p_cluster_offsets, const Vector<int32_t> &p_texel_patches);
	void realtime_gi_set_transport(RID p_rid, int p_ray_count, const Vector<int32_t> &p_offsets, const Vector<int32_t> &p_entries, const Vector<float> &p_sky_visibility);
	void realtime_gi_set_geometry(RID p_rid, const AABB &p_bounds, const Vector<int32_t> &p_bvh, const Vector<float> &p_triangles);
	void realtime_gi_set_probes(RID p_rid, const Vector<Vector3> &p_points, int p_ray_count, const Vector<int32_t> &p_rays, const Vector<int32_t> &p_tetrahedra, const Vector<int32_t> &p_bsp_tree);
	void realtime_gi_set_energy(RID p_rid, float p_indirect_energy, float p_environment_energy, float p_albedo_boost, float p_emission_energy);
	void realtime_gi_set_quality(RID p_rid, int p_bounces_per_frame, int p_shadow_samples, int p_probe_update_interval);
	void realtime_gi_set_interior(RID p_rid, bool p_interior);

	AABB realtime_gi_get_bounds(RID p_rid) const;
	RID realtime_gi_get_lightmap(RID p_rid) const;
	Dependency *realtime_gi_get_dependency(RID p_rid) const;

	bool realtime_gi_update(RID p_rid, const Transform3D &p_transform, const Vector<RendererGI::RealtimeGILight> &p_lights, RID p_environment);
};

} // namespace RendererRD
