/**************************************************************************/
/*  terrain_heightmap_io.h                                                */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#pragma once

#include "core/io/image.h"

// Reading and writing heightmaps in the formats terrain tools actually emit.
namespace TerrainHeightmapIO {

struct Loaded {
	Ref<Image> image; // FORMAT_RF.
	// True when the values are fractions of a height range, as integer
	// formats always are; false when they are already metres.
	bool normalized = true;
	int bits = 8;
	Vector2 range; // Minimum and maximum value found.
};

Error load(const String &p_path, Loaded &r_loaded, String &r_error);
// Writes .exr as metres, and .png (16-bit) or .r16 normalised to the image's
// own height range, which is returned so the heights can be restored.
Error save(const Ref<Image> &p_heights, const String &p_path, Vector2 &r_range, String &r_error);

} // namespace TerrainHeightmapIO
