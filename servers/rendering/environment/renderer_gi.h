/**************************************************************************/
/*  renderer_gi.h                                                         */
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
#include "core/math/transform_3d.h"
#include "servers/rendering/rendering_server_enums.h"

#include <cstdint>

class RID;
struct AABB;
struct Vector2i;
struct Vector3;
struct Vector3i;

template <typename T>
class Vector;

class RendererGI {
public:
	virtual ~RendererGI() {}

	/* VOXEL GI API */

	virtual RID voxel_gi_allocate() = 0;
	virtual void voxel_gi_free(RID p_rid) = 0;
	virtual void voxel_gi_initialize(RID p_rid) = 0;

	virtual void voxel_gi_allocate_data(RID p_voxel_gi, const Transform3D &p_to_cell_xform, const AABB &p_aabb, const Vector3i &p_octree_size, const Vector<uint8_t> &p_octree_cells, const Vector<uint8_t> &p_data_cells, const Vector<uint8_t> &p_distance_field, const Vector<int> &p_level_counts) = 0;

	virtual AABB voxel_gi_get_bounds(RID p_voxel_gi) const = 0;
	virtual Vector3i voxel_gi_get_octree_size(RID p_voxel_gi) const = 0;
	virtual Vector<uint8_t> voxel_gi_get_octree_cells(RID p_voxel_gi) const = 0;
	virtual Vector<uint8_t> voxel_gi_get_data_cells(RID p_voxel_gi) const = 0;
	virtual Vector<uint8_t> voxel_gi_get_distance_field(RID p_voxel_gi) const = 0;

	virtual Vector<int> voxel_gi_get_level_counts(RID p_voxel_gi) const = 0;
	virtual Transform3D voxel_gi_get_to_cell_xform(RID p_voxel_gi) const = 0;

	virtual void voxel_gi_set_dynamic_range(RID p_voxel_gi, float p_range) = 0;
	virtual float voxel_gi_get_dynamic_range(RID p_voxel_gi) const = 0;

	virtual void voxel_gi_set_propagation(RID p_voxel_gi, float p_range) = 0;
	virtual float voxel_gi_get_propagation(RID p_voxel_gi) const = 0;

	virtual void voxel_gi_set_energy(RID p_voxel_gi, float p_energy) = 0;
	virtual float voxel_gi_get_energy(RID p_voxel_gi) const = 0;

	virtual void voxel_gi_set_baked_exposure_normalization(RID p_voxel_gi, float p_baked_exposure) = 0;
	virtual float voxel_gi_get_baked_exposure_normalization(RID p_voxel_gi) const = 0;

	virtual void voxel_gi_set_bias(RID p_voxel_gi, float p_bias) = 0;
	virtual float voxel_gi_get_bias(RID p_voxel_gi) const = 0;

	virtual void voxel_gi_set_normal_bias(RID p_voxel_gi, float p_range) = 0;
	virtual float voxel_gi_get_normal_bias(RID p_voxel_gi) const = 0;

	virtual void voxel_gi_set_interior(RID p_voxel_gi, bool p_enable) = 0;
	virtual bool voxel_gi_is_interior(RID p_voxel_gi) const = 0;

	virtual void voxel_gi_set_use_two_bounces(RID p_voxel_gi, bool p_enable) = 0;
	virtual bool voxel_gi_is_using_two_bounces(RID p_voxel_gi) const = 0;

	virtual uint32_t voxel_gi_get_version(RID p_probe) const = 0;

	virtual void sdfgi_reset() = 0;

	/* REALTIME GI API */

	// A light as the scene culler hands it to realtime GI each frame. Values
	// are the light's own settings; the renderer turns them into shading units
	// the same way it does for direct lighting, so bounce and direct agree.
	struct RealtimeGILight {
		RSE::LightType type = RSE::LIGHT_OMNI;
		Transform3D transform;
		Color color;
		float energy = 1.0;
		float intensity = 1.0;
		float indirect_energy = 1.0;
		float range = 5.0;
		float attenuation = 1.0;
		float spot_angle = 45.0;
		float spot_attenuation = 1.0;
		bool has_shadow = false;
		// Static lights are left out of direct lighting on lightmapped surfaces,
		// so realtime GI lights those surfaces itself.
		bool bake_static = false;
	};

	virtual RID realtime_gi_allocate() = 0;
	virtual void realtime_gi_free(RID p_rid) = 0;
	virtual void realtime_gi_initialize(RID p_rid) = 0;

	virtual void realtime_gi_set_surfaces(RID p_realtime_gi, const Vector2i &p_atlas_size, const Vector<float> &p_patches, const Vector<int32_t> &p_cluster_offsets, const Vector<int32_t> &p_texel_patches) = 0;
	virtual void realtime_gi_set_transport(RID p_realtime_gi, int p_ray_count, const Vector<int32_t> &p_offsets, const Vector<int32_t> &p_entries, const Vector<float> &p_sky_visibility) = 0;
	virtual void realtime_gi_set_geometry(RID p_realtime_gi, const AABB &p_bounds, const Vector<int32_t> &p_bvh, const Vector<float> &p_triangles) = 0;
	virtual void realtime_gi_set_probes(RID p_realtime_gi, const Vector<Vector3> &p_points, int p_ray_count, const Vector<int32_t> &p_rays, const Vector<int32_t> &p_tetrahedra, const Vector<int32_t> &p_bsp_tree) = 0;

	virtual void realtime_gi_set_energy(RID p_realtime_gi, float p_indirect_energy, float p_environment_energy, float p_albedo_boost, float p_emission_energy) = 0;
	virtual void realtime_gi_set_quality(RID p_realtime_gi, int p_bounces_per_frame, int p_shadow_samples, int p_probe_update_interval) = 0;
	virtual void realtime_gi_set_interior(RID p_realtime_gi, bool p_interior) = 0;

	virtual AABB realtime_gi_get_bounds(RID p_realtime_gi) const = 0;
	// The lightmap realtime GI writes into. Static surfaces use it as their
	// lightmap and dynamic objects read its probes.
	virtual RID realtime_gi_get_lightmap(RID p_realtime_gi) const = 0;

	// Runs this frame's lighting. Returns true when new probe lighting has
	// arrived, so the culler knows to recapture the objects it lights.
	virtual bool realtime_gi_update(RID p_realtime_gi, const Transform3D &p_transform, const Vector<RealtimeGILight> &p_lights, RID p_environment) = 0;
};
