/**************************************************************************/
/*  terrain_material.h                                                    */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#pragma once

#include "core/io/resource.h"

// How a terrain's layers are combined into a surface. The layers themselves
// belong to TerrainData; this is the part that is a matter of taste, and so is
// worth sharing between every terrain in a game.
class TerrainMaterial : public Resource {
	GDCLASS(TerrainMaterial, Resource);

	bool height_blending = true;
	float height_blend_sharpness = 0.25;

	bool auto_slope = false;
	int auto_slope_layer = 1;
	float auto_slope_angle = 35.0;
	float auto_slope_blend = 10.0;

	float macro_variation = 0.2;
	float macro_variation_size = 64.0;

	float wetness_darkening = 0.55;

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	void set_height_blending(bool p_enabled);
	bool is_height_blending() const { return height_blending; }
	void set_height_blend_sharpness(float p_sharpness);
	float get_height_blend_sharpness() const { return height_blend_sharpness; }

	void set_auto_slope(bool p_enabled);
	bool is_auto_slope() const { return auto_slope; }
	void set_auto_slope_layer(int p_layer);
	int get_auto_slope_layer() const { return auto_slope_layer; }
	void set_auto_slope_angle(float p_degrees);
	float get_auto_slope_angle() const { return auto_slope_angle; }
	void set_auto_slope_blend(float p_degrees);
	float get_auto_slope_blend() const { return auto_slope_blend; }

	void set_macro_variation(float p_amount);
	float get_macro_variation() const { return macro_variation; }
	void set_macro_variation_size(float p_size);
	float get_macro_variation_size() const { return macro_variation_size; }

	void set_wetness_darkening(float p_amount);
	float get_wetness_darkening() const { return wetness_darkening; }
};
