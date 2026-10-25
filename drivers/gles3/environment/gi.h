/**************************************************************************/
/*  gi.h                                                                  */
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

#ifdef GLES3_ENABLED

#include "core/math/aabb.h"
#include "core/templates/rid.h"
#include "servers/rendering/environment/renderer_gi.h"

namespace GLES3 {

class GI : public RendererGI {
public:
	/* VOXEL GI API */

	virtual RID voxel_gi_allocate() override;
	virtual void voxel_gi_free(RID p_rid) override;
	virtual void voxel_gi_initialize(RID p_rid) override;
	virtual void voxel_gi_allocate_data(RID p_voxel_gi, const Transform3D &p_to_cell_xform, const AABB &p_aabb, const Vector3i &p_octree_size, const Vector<uint8_t> &p_octree_cells, const Vector<uint8_t> &p_data_cells, const Vector<uint8_t> &p_distance_field, const Vector<int> &p_level_counts) override;

	virtual AABB voxel_gi_get_bounds(RID p_voxel_gi) const override;
	virtual Vector3i voxel_gi_get_octree_size(RID p_voxel_gi) const override;
	virtual Vector<uint8_t> voxel_gi_get_octree_cells(RID p_voxel_gi) const override;
	virtual Vector<uint8_t> voxel_gi_get_data_cells(RID p_voxel_gi) const override;
	virtual Vector<uint8_t> voxel_gi_get_distance_field(RID p_voxel_gi) const override;

	virtual Vector<int> voxel_gi_get_level_counts(RID p_voxel_gi) const override;
	virtual Transform3D voxel_gi_get_to_cell_xform(RID p_voxel_gi) const override;

	virtual void voxel_gi_set_dynamic_range(RID p_voxel_gi, float p_range) override;
	virtual float voxel_gi_get_dynamic_range(RID p_voxel_gi) const override;

	virtual void voxel_gi_set_propagation(RID p_voxel_gi, float p_range) override;
	virtual float voxel_gi_get_propagation(RID p_voxel_gi) const override;

	virtual void voxel_gi_set_energy(RID p_voxel_gi, float p_range) override;
	virtual float voxel_gi_get_energy(RID p_voxel_gi) const override;

	virtual void voxel_gi_set_baked_exposure_normalization(RID p_voxel_gi, float p_baked_exposure) override;
	virtual float voxel_gi_get_baked_exposure_normalization(RID p_voxel_gi) const override;

	virtual void voxel_gi_set_bias(RID p_voxel_gi, float p_range) override;
	virtual float voxel_gi_get_bias(RID p_voxel_gi) const override;

	virtual void voxel_gi_set_normal_bias(RID p_voxel_gi, float p_range) override;
	virtual float voxel_gi_get_normal_bias(RID p_voxel_gi) const override;

	virtual void voxel_gi_set_interior(RID p_voxel_gi, bool p_enable) override;
	virtual bool voxel_gi_is_interior(RID p_voxel_gi) const override;

	virtual void voxel_gi_set_use_two_bounces(RID p_voxel_gi, bool p_enable) override;
	virtual bool voxel_gi_is_using_two_bounces(RID p_voxel_gi) const override;

	virtual uint32_t voxel_gi_get_version(RID p_voxel_gi) const override;

	virtual void sdfgi_reset() override;

	/* REALTIME GI API */

	// Realtime GI needs compute shaders, which the Compatibility renderer lacks.

	virtual RID realtime_gi_allocate() override { return RID(); }
	virtual void realtime_gi_free(RID p_rid) override {}
	virtual void realtime_gi_initialize(RID p_rid) override {}
	virtual void realtime_gi_set_surfaces(RID p_realtime_gi, const Vector2i &p_atlas_size, const Vector<float> &p_patches, const Vector<int32_t> &p_cluster_offsets, const Vector<int32_t> &p_texel_patches) override {}
	virtual void realtime_gi_set_transport(RID p_realtime_gi, int p_ray_count, const Vector<int32_t> &p_offsets, const Vector<int32_t> &p_entries, const Vector<float> &p_sky_visibility) override {}
	virtual void realtime_gi_set_geometry(RID p_realtime_gi, const AABB &p_bounds, const Vector<int32_t> &p_bvh, const Vector<float> &p_triangles) override {}
	virtual void realtime_gi_set_probes(RID p_realtime_gi, const Vector<Vector3> &p_points, int p_ray_count, const Vector<int32_t> &p_rays, const Vector<int32_t> &p_tetrahedra, const Vector<int32_t> &p_bsp_tree) override {}
	virtual void realtime_gi_set_energy(RID p_realtime_gi, float p_indirect_energy, float p_environment_energy, float p_albedo_boost, float p_emission_energy) override {}
	virtual void realtime_gi_set_quality(RID p_realtime_gi, int p_bounces_per_frame, int p_shadow_samples, int p_probe_update_interval) override {}
	virtual void realtime_gi_set_interior(RID p_realtime_gi, bool p_interior) override {}
	virtual AABB realtime_gi_get_bounds(RID p_realtime_gi) const override { return AABB(); }
	virtual RID realtime_gi_get_lightmap(RID p_realtime_gi) const override { return RID(); }
	virtual bool realtime_gi_update(RID p_realtime_gi, const Transform3D &p_transform, const Vector<RealtimeGILight> &p_lights, RID p_environment) override { return false; }
};

}; // namespace GLES3

#endif // GLES3_ENABLED
