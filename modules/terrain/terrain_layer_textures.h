/**************************************************************************/
/*  terrain_layer_textures.h                                              */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#pragma once

#include "terrain_layer.h"
#include "terrain_material.h"

#include "core/templates/rid.h"

// A terrain's layers as the GPU sees them: every colour texture in one
// texture array, every normal map in another, and the per layer uniforms
// that say which slice belongs to which layer. Heightmap and voxel terrain
// draw their layers the same way, so they share this.
class TerrainLayerTextures {
public:
	static constexpr int MAX_LAYERS = 16;

private:
	RID albedo_array;
	RID normal_array;
	// Which layers and texture versions the arrays were built from.
	Vector<uint64_t> key;
	int albedo_slices[MAX_LAYERS];
	int normal_slices[MAX_LAYERS];
	bool built = false;
	bool converted = false;

	static Vector<uint64_t> _make_key(const Vector<Ref<TerrainLayer>> &p_layers);

public:
	// Builds the arrays again if the layers' textures changed since the last
	// call, or always with p_force. Returns whether it did.
	bool update(const Vector<Ref<TerrainLayer>> &p_layers, bool p_force = false);
	void clear();

	// True when the textures disagreed in size, format or mipmaps and had to
	// be converted to uncompressed RGBA8 to share an array.
	bool is_converted() const { return converted; }

	// layer_count, layer_albedo, layer_normal, layer_tint, layer_params and
	// layer_textures.
	void set_layer_params(RID p_material, const Vector<Ref<TerrainLayer>> &p_layers) const;
	// The TerrainMaterial's blending, auto slope and variation uniforms.
	static void set_material_params(RID p_material, const Ref<TerrainMaterial> &p_terrain_material);

	TerrainLayerTextures();
	~TerrainLayerTextures();
};
