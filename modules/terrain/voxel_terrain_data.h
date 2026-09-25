/**************************************************************************/
/*  voxel_terrain_data.h                                                  */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#pragma once

#include "terrain_layer.h"
#include "voxel_generator.h"

#include "core/io/resource.h"
#include "core/math/aabb.h"
#include "core/math/vector3i.h"
#include "core/os/rw_lock.h"
#include "core/templates/hash_map.h"

// Everything that makes one voxel terrain different from another: its
// generator, its layers, and the blocks of voxels that have been edited.
//
// Each voxel holds a signed distance to the surface, negative in the ground,
// and the layer it is made of. Only edited blocks are stored; everywhere else
// is asked of the generator, so digging one tunnel through a mountain stores
// the voxels around the tunnel and nothing else.
//
// Positions here are in voxels. Voxel (x, y, z) sits at
// Vector3(x, y, z) * voxel_size from the terrain's origin.
class VoxelTerrainData : public Resource {
	GDCLASS(VoxelTerrainData, Resource);

public:
	static constexpr int BLOCK_SIZE = 32;
	static constexpr int BLOCK_VOLUME = BLOCK_SIZE * BLOCK_SIZE * BLOCK_SIZE;
	static constexpr int MAX_LAYERS = 16;

	enum Operation {
		OPERATION_ADD,
		OPERATION_REMOVE,
		OPERATION_SMOOTH,
		OPERATION_FLATTEN,
		OPERATION_PAINT,
		OPERATION_ERASE_PAINT,
	};

	struct Block {
		// Distances in voxels, stored as 1/1024ths.
		Vector<int16_t> sdf;
		Vector<uint8_t> material;
	};

	_FORCE_INLINE_ static int floor_div(int p_a, int p_b) {
		return p_a >= 0 ? p_a / p_b : -((-p_a + p_b - 1) / p_b);
	}
	_FORCE_INLINE_ static Vector3i voxel_to_block(const Vector3i &p_voxel) {
		return Vector3i(floor_div(p_voxel.x, BLOCK_SIZE), floor_div(p_voxel.y, BLOCK_SIZE), floor_div(p_voxel.z, BLOCK_SIZE));
	}

private:
	float voxel_size = 1.0;
	Ref<VoxelGenerator> generator;
	Vector<Ref<TerrainLayer>> layers;
	HashMap<Vector3i, Block> blocks;
	// Meshing reads from worker threads while the editor or a game writes.
	mutable RWLock lock;

	void _mark_edited();
	void _generator_changed();
	void _layer_changed();
	void _emit_changed_area(const AABB &p_voxel_box);

	// Unlocked helpers; the caller holds the lock.
	void _read_unlocked(const Vector3i &p_origin, int p_step, const Vector3i &p_size, float *r_sdf, uint8_t *r_material) const;
	Block &_ensure_block(const Vector3i &p_block);
	void _remap_materials(const Vector<int> &p_old_to_new);

	// Storage.
	void _set_blocks(const Array &p_blocks);
	Array _get_blocks() const;
	void _set_layers(const Array &p_layers);
	Array _get_layers() const;

	// Undo support for the editor, per block. Restoring a block that did not
	// exist before removes it, handing it back to the generator.
	Dictionary _get_snapshot(const Vector3i &p_block) const;
	void _apply_snapshots(const Array &p_snapshots);
	Array _get_layers_snapshot() const;
	void _apply_layers_snapshot(const Array &p_layers, const Array &p_snapshots);

	TypedArray<Vector3i> _get_block_coords_bind() const;

protected:
	static void _bind_methods();
	bool _set(const StringName &p_name, const Variant &p_value);
	bool _get(const StringName &p_name, Variant &r_ret) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;

public:
	void set_voxel_size(float p_size);
	float get_voxel_size() const { return voxel_size; }
	void set_generator(const Ref<VoxelGenerator> &p_generator);
	Ref<VoxelGenerator> get_generator() const { return generator; }

	// Layers, as TerrainData has them: materials refer to them by index.
	int get_layer_count() const { return layers.size(); }
	Ref<TerrainLayer> get_layer(int p_index) const;
	void set_layer(int p_index, const Ref<TerrainLayer> &p_layer);
	int add_layer(const Ref<TerrainLayer> &p_layer);
	void remove_layer(int p_index);
	void move_layer(int p_from, int p_to);
	const Vector<Ref<TerrainLayer>> &get_layer_list() const { return layers; }

	// Edited blocks.
	int get_block_count() const;
	bool has_block(const Vector3i &p_block) const;
	void remove_block(const Vector3i &p_block);
	void clear();
	// The voxels covered by edited blocks, or an empty box when none are.
	AABB get_edited_bounds() const;
	bool has_edits_in(const AABB &p_voxel_box) const;
	// The lowest and highest voxel edited in the columns of a rectangle (x
	// and z). False when none of them has been edited.
	bool get_edited_span(const Rect2i &p_columns, int &r_lo, int &r_hi) const;

	// Voxels, edited or generated.
	float get_sdf(const Vector3i &p_voxel) const;
	int get_material(const Vector3i &p_voxel) const;
	void set_voxel(const Vector3i &p_voxel, float p_sdf, int p_material = -1);
	// Blended between the eight voxels around a point.
	float sample_sdf(const Vector3 &p_voxel_position) const;
	Vector3 sample_normal(const Vector3 &p_voxel_position) const;

	// Fills p_size samples along each axis, p_step voxels apart, starting at
	// p_origin, x fastest: edited voxels where there are some, the generator
	// elsewhere. Safe to call from any thread.
	void read(const Vector3i &p_origin, int p_step, const Vector3i &p_size, float *r_sdf, uint8_t *r_material) const;

	// Where ground meets the air above it, for foliage. Both are in voxels.
	//
	// find_top_surface() looks down each column of a rectangle, x fastest,
	// from p_top to p_bottom, for the highest ground open to the sky. Heights
	// are NaN where a column has none, and r_layers gets the layer just under
	// the surface.
	void find_top_surface(const Vector2i &p_origin, const Vector2i &p_size, int p_top, int p_bottom, float *r_heights, uint8_t *r_layers) const;
	// find_ground() finds the upward-facing ground in the column through
	// p_position nearest to its height, within p_range. Floors count, cave
	// ceilings do not.
	bool find_ground(const Vector3 &p_position, float p_range, float &r_height, int &r_layer) const;

	// One brush dab, in voxels. p_amount is how far the surface may move, in
	// voxels, or for smoothing, flattening and painting how much of the way
	// to go (1 for all of it). p_height is the flattening plane's height.
	// Ground that adding or flattening creates is made of p_layer, or of
	// whatever the voxels were before when it is -1.
	// Returns the voxels that changed.
	AABB edit_sphere(Operation p_operation, const Vector3 &p_center, float p_radius, float p_amount, float p_hardness = 1.0, int p_layer = -1, float p_height = 0.0, uint32_t p_seed = 0);
	// Blocks a dab of this size touches, for taking undo snapshots first.
	static AABB get_edit_bounds(Operation p_operation, const Vector3 &p_center, float p_radius);

	VoxelTerrainData();
};

VARIANT_ENUM_CAST(VoxelTerrainData::Operation);
