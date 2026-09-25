/**************************************************************************/
/*  voxel_generator.h                                                     */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#pragma once

#include "terrain_data.h"

#include "core/io/resource.h"
#include "core/math/aabb.h"
#include "core/math/vector3i.h"

class Noise;

// The ground a voxel terrain starts with, before anyone has dug into it.
// Everything not edited is worked out from this on demand, so a world as
// large as the view distance costs nothing to store.
//
// The ground is a surface height, from a flat level, noise, or a heightmap
// terrain, with optional caves carved out by 3D noise under it. Distances are
// in metres; the terrain divides by its voxel size.
class VoxelGenerator : public Resource {
	GDCLASS(VoxelGenerator, Resource);

	float height = 0.0;
	Ref<Noise> height_noise;
	float height_amplitude = 24.0;

	Ref<TerrainData> height_source;
	float height_source_spacing = 1.0;
	Vector3 height_source_offset;

	Ref<Noise> cave_noise;
	float cave_threshold = 0.3;
	float cave_min_depth = 6.0;
	float cave_max_depth = 64.0;

	int surface_layer = 0;
	int deep_layer = 0;
	float deep_layer_depth = 3.0;

	// The heightmap's range, which takes a pass over every height to find.
	mutable Vector2 source_range;
	mutable bool source_range_valid = false;

	float _cave_sdf(float p_depth, const Vector3 &p_world, float p_inv_voxel) const;
	void _noise_changed();
	void _source_changed();
	void _source_region_changed(const Vector2i &p_region, int p_maps);
	float _get_surface_height_bind(float p_x, float p_z) const { return get_surface_height(p_x, p_z); }

protected:
	static void _bind_methods();

public:
	// Distances are clamped to this many voxels either side of the surface;
	// beyond it only the sign matters.
	static constexpr float SDF_LIMIT = 31.0;

	void set_height(float p_height);
	float get_height() const { return height; }
	void set_height_noise(const Ref<Noise> &p_noise);
	Ref<Noise> get_height_noise() const;
	void set_height_amplitude(float p_amplitude);
	float get_height_amplitude() const { return height_amplitude; }

	void set_height_source(const Ref<TerrainData> &p_source);
	Ref<TerrainData> get_height_source() const { return height_source; }
	void set_height_source_spacing(float p_spacing);
	float get_height_source_spacing() const { return height_source_spacing; }
	void set_height_source_offset(const Vector3 &p_offset);
	Vector3 get_height_source_offset() const { return height_source_offset; }

	void set_cave_noise(const Ref<Noise> &p_noise);
	Ref<Noise> get_cave_noise() const;
	void set_cave_threshold(float p_threshold);
	float get_cave_threshold() const { return cave_threshold; }
	void set_cave_min_depth(float p_depth);
	float get_cave_min_depth() const { return cave_min_depth; }
	void set_cave_max_depth(float p_depth);
	float get_cave_max_depth() const { return cave_max_depth; }

	void set_surface_layer(int p_layer);
	int get_surface_layer() const { return surface_layer; }
	void set_deep_layer(int p_layer);
	int get_deep_layer() const { return deep_layer; }
	void set_deep_layer_depth(float p_depth);
	float get_deep_layer_depth() const { return deep_layer_depth; }

	// The height of the ground at a point, in metres, and the layer its
	// surface is made of.
	float get_surface_height(float p_x, float p_z, int *r_layer = nullptr) const;

	// Fills p_size samples along each axis, p_step voxels apart, starting at
	// voxel p_origin, x fastest. Safe to call from several threads at once.
	void generate(const Vector3i &p_origin, int p_step, const Vector3i &p_size, float p_voxel_size, float *r_sdf, uint8_t *r_material) const;
	float get_sdf(const Vector3 &p_voxel, float p_voxel_size) const;

	// The lowest and highest the surface can be, in metres, caves aside.
	void get_surface_bounds(float &r_lo, float &r_hi) const;

	// Whether the ground's surface can pass through a box of voxels. When it
	// cannot, r_solid says whether the box is all ground or all air.
	bool may_contain_surface(const AABB &p_voxel_box, float p_voxel_size, bool &r_solid) const;
};
