/**************************************************************************/
/*  terrain_material.cpp                                                  */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#include "terrain_material.h"

#include "terrain_data.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

void TerrainMaterial::set_height_blending(bool p_enabled) {
	height_blending = p_enabled;
	notify_property_list_changed();
	emit_changed();
}

void TerrainMaterial::set_height_blend_sharpness(float p_sharpness) {
	height_blend_sharpness = CLAMP(p_sharpness, 0.01f, 1.0f);
	emit_changed();
}

void TerrainMaterial::set_auto_slope(bool p_enabled) {
	auto_slope = p_enabled;
	notify_property_list_changed();
	emit_changed();
}

void TerrainMaterial::set_auto_slope_layer(int p_layer) {
	auto_slope_layer = CLAMP(p_layer, 0, TerrainData::MAX_LAYERS - 1);
	emit_changed();
}

void TerrainMaterial::set_auto_slope_angle(float p_degrees) {
	auto_slope_angle = CLAMP(p_degrees, 0.0f, 90.0f);
	emit_changed();
}

void TerrainMaterial::set_auto_slope_blend(float p_degrees) {
	auto_slope_blend = CLAMP(p_degrees, 0.1f, 45.0f);
	emit_changed();
}

void TerrainMaterial::set_macro_variation(float p_amount) {
	macro_variation = CLAMP(p_amount, 0.0f, 1.0f);
	emit_changed();
}

void TerrainMaterial::set_macro_variation_size(float p_size) {
	macro_variation_size = MAX(p_size, 1.0f);
	emit_changed();
}

void TerrainMaterial::set_wetness_darkening(float p_amount) {
	wetness_darkening = CLAMP(p_amount, 0.0f, 1.0f);
	emit_changed();
}

void TerrainMaterial::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name == "height_blend_sharpness" && !height_blending) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	if (p_property.name.begins_with("auto_slope_") && !auto_slope) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

void TerrainMaterial::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_height_blending", "enabled"), &TerrainMaterial::set_height_blending);
	ClassDB::bind_method(D_METHOD("is_height_blending"), &TerrainMaterial::is_height_blending);
	ClassDB::bind_method(D_METHOD("set_height_blend_sharpness", "sharpness"), &TerrainMaterial::set_height_blend_sharpness);
	ClassDB::bind_method(D_METHOD("get_height_blend_sharpness"), &TerrainMaterial::get_height_blend_sharpness);
	ClassDB::bind_method(D_METHOD("set_auto_slope", "enabled"), &TerrainMaterial::set_auto_slope);
	ClassDB::bind_method(D_METHOD("is_auto_slope"), &TerrainMaterial::is_auto_slope);
	ClassDB::bind_method(D_METHOD("set_auto_slope_layer", "layer"), &TerrainMaterial::set_auto_slope_layer);
	ClassDB::bind_method(D_METHOD("get_auto_slope_layer"), &TerrainMaterial::get_auto_slope_layer);
	ClassDB::bind_method(D_METHOD("set_auto_slope_angle", "degrees"), &TerrainMaterial::set_auto_slope_angle);
	ClassDB::bind_method(D_METHOD("get_auto_slope_angle"), &TerrainMaterial::get_auto_slope_angle);
	ClassDB::bind_method(D_METHOD("set_auto_slope_blend", "degrees"), &TerrainMaterial::set_auto_slope_blend);
	ClassDB::bind_method(D_METHOD("get_auto_slope_blend"), &TerrainMaterial::get_auto_slope_blend);
	ClassDB::bind_method(D_METHOD("set_macro_variation", "amount"), &TerrainMaterial::set_macro_variation);
	ClassDB::bind_method(D_METHOD("get_macro_variation"), &TerrainMaterial::get_macro_variation);
	ClassDB::bind_method(D_METHOD("set_macro_variation_size", "size"), &TerrainMaterial::set_macro_variation_size);
	ClassDB::bind_method(D_METHOD("get_macro_variation_size"), &TerrainMaterial::get_macro_variation_size);
	ClassDB::bind_method(D_METHOD("set_wetness_darkening", "amount"), &TerrainMaterial::set_wetness_darkening);
	ClassDB::bind_method(D_METHOD("get_wetness_darkening"), &TerrainMaterial::get_wetness_darkening);

	ADD_GROUP("Height Blending", "height_blend");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "height_blending"), "set_height_blending", "is_height_blending");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height_blend_sharpness", PROPERTY_HINT_RANGE, "0.01,1,0.01"), "set_height_blend_sharpness", "get_height_blend_sharpness");

	ADD_GROUP("Auto Slope", "auto_slope");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_slope"), "set_auto_slope", "is_auto_slope");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "auto_slope_layer", PROPERTY_HINT_RANGE, "0,15,1"), "set_auto_slope_layer", "get_auto_slope_layer");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "auto_slope_angle", PROPERTY_HINT_RANGE, "0,90,0.1,degrees"), "set_auto_slope_angle", "get_auto_slope_angle");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "auto_slope_blend", PROPERTY_HINT_RANGE, "0.1,45,0.1,degrees"), "set_auto_slope_blend", "get_auto_slope_blend");

	ADD_GROUP("Macro Variation", "macro_variation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "macro_variation", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_macro_variation", "get_macro_variation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "macro_variation_size", PROPERTY_HINT_RANGE, "1,1024,0.1,or_greater,suffix:m"), "set_macro_variation_size", "get_macro_variation_size");

	ADD_GROUP("Wetness", "wetness_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wetness_darkening", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_wetness_darkening", "get_wetness_darkening");
}
