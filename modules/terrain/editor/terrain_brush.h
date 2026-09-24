/**************************************************************************/
/*  terrain_brush.h                                                       */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#pragma once

#include "core/math/color.h"
#include "core/math/rect2.h"
#include "core/math/rect2i.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/templates/local_vector.h"

class Foliage3D;
class Terrain3D;

// The terrain editor's tools, kept apart from the dock so they can be read and
// reasoned about on their own. Every tool works on a window of vertices around
// the brush: read it, change it, write it back.
class TerrainBrush {
public:
	enum Tool {
		TOOL_NONE,
		TOOL_RAISE,
		TOOL_SMOOTH,
		TOOL_FLATTEN,
		TOOL_ERODE,
		TOOL_NOISE,
		TOOL_TERRACE,
		TOOL_RAMP,
		TOOL_HOLES,
		TOOL_PAINT,
		TOOL_COLOR,
		TOOL_WETNESS,
		TOOL_GRASS,
		TOOL_OBJECTS,
		TOOL_REGIONS,
		TOOL_DIG, // Voxel terrain only.
		TOOL_MAX,
	};

	struct Settings {
		float radius = 6.0; // Metres.
		float strength = 0.5; // 0 to 1.
		float hardness = 0.4; // Fraction of the radius at full strength.
		bool invert = false;
		float flatten_height = 0.0; // Relative to the terrain's origin.
		float noise_scale = 12.0; // Metres per noise feature.
		float terrace_step = 2.0; // Metres between terraces.
		int layer = 0;
		Color color = Color(0.55, 0.42, 0.3);
		int foliage_type = 0;
	};

	static bool is_sculpt(Tool p_tool) { return p_tool >= TOOL_RAISE && p_tool <= TOOL_HOLES; }
	static bool is_foliage(Tool p_tool) { return p_tool == TOOL_GRASS || p_tool == TOOL_OBJECTS; }
	static uint32_t maps_for(Tool p_tool);
	static const char *get_tool_name(Tool p_tool);

	// The vertices a dab at p_local (metres, relative to the terrain's origin)
	// can change. Some tools read a margin around the brush but only write
	// inside it.
	static Rect2i get_dab_rect(const Terrain3D *p_terrain, Tool p_tool, const Vector2 &p_local, const Settings &p_settings);

	// One dab of a tool, applied over p_delta seconds.
	static void apply(Terrain3D *p_terrain, Foliage3D *p_foliage, Tool p_tool, const Vector2 &p_local, const Settings &p_settings, float p_delta, uint32_t p_seed);
	// The ramp tool works on a whole segment at once, on release.
	static void apply_ramp(Terrain3D *p_terrain, const Vector3 &p_from, const Vector3 &p_to, const Settings &p_settings);
	static Rect2i get_ramp_rect(const Terrain3D *p_terrain, const Vector3 &p_from, const Vector3 &p_to, const Settings &p_settings);

	// 0 to 1 across the brush: flat to the hardness radius, then eased to 0.
	static float falloff(float p_distance, float p_radius, float p_hardness);
	static float fractal_noise(float p_x, float p_z, uint32_t p_seed);
};
