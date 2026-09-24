/**************************************************************************/
/*  voxel_mesher.h                                                        */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#pragma once

#include "core/string/ustring.h"
#include "core/variant/variant.h"

// Marching cubes over one block of signed distances: negative inside the
// ground, positive in the air, with the surface where it crosses zero.
//
// The triangle table is not typed in from a paper. It is derived once from
// the cube itself: each face of a cell is cut where its corners change sign,
// the cuts are joined into loops around the cell, and each loop is fanned into
// triangles. A face with two opposite corners inside is always cut the same
// way, keeping those corners apart, and since that only depends on the face,
// the two cells that share it always agree and the surface has no holes.
//
// Everything here is plain data in and out, so blocks are meshed on worker
// threads.
class VoxelMesher {
public:
	static constexpr int CELLS = 32; // Cells along each edge of a block.
	static constexpr int POINTS = CELLS + 1; // Corners along each edge.
	static constexpr int SAMPLES = POINTS + 2; // With a margin of one each side, for normals.
	static constexpr int SAMPLE_COUNT = SAMPLES * SAMPLES * SAMPLES;
	static constexpr int MAX_LAYERS = 16;

	// Samples are indexed from -1 to POINTS on each axis.
	static _FORCE_INLINE_ int sample_index(int p_x, int p_y, int p_z) {
		return ((p_z + 1) * SAMPLES + (p_y + 1)) * SAMPLES + (p_x + 1);
	}

	struct Input {
		const float *sdf = nullptr; // SAMPLE_COUNT distances, in cells.
		const uint8_t *material = nullptr; // SAMPLE_COUNT layer indices.
		float cell_size = 1.0; // Metres per cell.
		// How far skirts hang, in metres, to hide the crack against a
		// neighbouring block drawn at another level of detail. Zero for none.
		float skirt = 0.0;
		bool collision = false;
		// Coarse levels of detail sample the ground several voxels apart, so
		// the sample a vertex takes its layer from can be deep under the
		// surface, in rock rather than grass. When given, this is asked for
		// the layer instead, with the vertex's position in cells and its
		// normal; returning -1 keeps the sample's layer.
		int (*layer_at)(void *p_userdata, const Vector3 &p_cell_position, const Vector3 &p_normal) = nullptr;
		void *layer_userdata = nullptr;
	};

	struct Output {
		PackedVector3Array vertices;
		PackedVector3Array normals;
		// Layer weights, four layers to each: 0-3, 4-7, 8-11 and 12-15.
		PackedByteArray weights[4];
		PackedInt32Array indices;
		// Indices before this are the surface; the rest are skirts.
		int surface_index_count = 0;
		// The surface alone, three vertices a triangle, when asked for.
		PackedVector3Array collision_faces;
	};

	static void build(const Input &p_input, Output &r_output);

	// Checks the derived table: every loop closes and every cut edge is used
	// by exactly two segments. Empty when all is well.
	static String validate_table();
};
