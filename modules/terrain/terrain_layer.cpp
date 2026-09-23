/**************************************************************************/
/*  terrain_layer.cpp                                                     */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#include "terrain_layer.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

void TerrainLayer::_texture_changed() {
	// A reimported texture changes size or format underneath us, and the
	// terrain has to rebuild its texture arrays to match.
	emit_changed();
}

void TerrainLayer::set_surface_name(const String &p_name) {
	surface_name = p_name;
	emit_changed();
}

void TerrainLayer::set_albedo_texture(const Ref<Texture2D> &p_texture) {
	if (albedo_texture == p_texture) {
		return;
	}
	if (albedo_texture.is_valid()) {
		albedo_texture->disconnect_changed(callable_mp(this, &TerrainLayer::_texture_changed));
	}
	albedo_texture = p_texture;
	if (albedo_texture.is_valid()) {
		albedo_texture->connect_changed(callable_mp(this, &TerrainLayer::_texture_changed));
	}
	emit_changed();
}

void TerrainLayer::set_normal_texture(const Ref<Texture2D> &p_texture) {
	if (normal_texture == p_texture) {
		return;
	}
	if (normal_texture.is_valid()) {
		normal_texture->disconnect_changed(callable_mp(this, &TerrainLayer::_texture_changed));
	}
	normal_texture = p_texture;
	if (normal_texture.is_valid()) {
		normal_texture->connect_changed(callable_mp(this, &TerrainLayer::_texture_changed));
	}
	emit_changed();
}

void TerrainLayer::set_albedo_color(const Color &p_color) {
	albedo_color = p_color;
	emit_changed();
}

void TerrainLayer::set_roughness(float p_roughness) {
	roughness = CLAMP(p_roughness, 0.0f, 1.0f);
	emit_changed();
}

void TerrainLayer::set_normal_strength(float p_strength) {
	normal_strength = CLAMP(p_strength, 0.0f, 4.0f);
	emit_changed();
}

void TerrainLayer::set_tile_size(float p_size) {
	tile_size = MAX(p_size, 0.01f);
	emit_changed();
}

void TerrainLayer::set_triplanar(bool p_enabled) {
	triplanar = p_enabled;
	emit_changed();
}

void TerrainLayer::set_detiling(float p_amount) {
	detiling = CLAMP(p_amount, 0.0f, 1.0f);
	emit_changed();
}

String TerrainLayer::get_display_name() const {
	if (!surface_name.is_empty()) {
		return surface_name;
	}
	if (!get_name().is_empty()) {
		return get_name();
	}
	if (albedo_texture.is_valid() && !albedo_texture->get_path().is_empty()) {
		return albedo_texture->get_path().get_file().get_basename();
	}
	return RTR("Unnamed Layer");
}

void TerrainLayer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_surface_name", "name"), &TerrainLayer::set_surface_name);
	ClassDB::bind_method(D_METHOD("get_surface_name"), &TerrainLayer::get_surface_name);
	ClassDB::bind_method(D_METHOD("set_albedo_texture", "texture"), &TerrainLayer::set_albedo_texture);
	ClassDB::bind_method(D_METHOD("get_albedo_texture"), &TerrainLayer::get_albedo_texture);
	ClassDB::bind_method(D_METHOD("set_normal_texture", "texture"), &TerrainLayer::set_normal_texture);
	ClassDB::bind_method(D_METHOD("get_normal_texture"), &TerrainLayer::get_normal_texture);
	ClassDB::bind_method(D_METHOD("set_albedo_color", "color"), &TerrainLayer::set_albedo_color);
	ClassDB::bind_method(D_METHOD("get_albedo_color"), &TerrainLayer::get_albedo_color);
	ClassDB::bind_method(D_METHOD("set_roughness", "roughness"), &TerrainLayer::set_roughness);
	ClassDB::bind_method(D_METHOD("get_roughness"), &TerrainLayer::get_roughness);
	ClassDB::bind_method(D_METHOD("set_normal_strength", "strength"), &TerrainLayer::set_normal_strength);
	ClassDB::bind_method(D_METHOD("get_normal_strength"), &TerrainLayer::get_normal_strength);
	ClassDB::bind_method(D_METHOD("set_tile_size", "size"), &TerrainLayer::set_tile_size);
	ClassDB::bind_method(D_METHOD("get_tile_size"), &TerrainLayer::get_tile_size);
	ClassDB::bind_method(D_METHOD("set_triplanar", "enabled"), &TerrainLayer::set_triplanar);
	ClassDB::bind_method(D_METHOD("is_triplanar"), &TerrainLayer::is_triplanar);
	ClassDB::bind_method(D_METHOD("set_detiling", "amount"), &TerrainLayer::set_detiling);
	ClassDB::bind_method(D_METHOD("get_detiling"), &TerrainLayer::get_detiling);
	ClassDB::bind_method(D_METHOD("get_display_name"), &TerrainLayer::get_display_name);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "surface_name"), "set_surface_name", "get_surface_name");

	ADD_GROUP("Albedo", "albedo_");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "albedo_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_albedo_texture", "get_albedo_texture");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "albedo_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_albedo_color", "get_albedo_color");

	ADD_GROUP("Normal", "normal_");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "normal_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_normal_texture", "get_normal_texture");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "normal_strength", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_normal_strength", "get_normal_strength");

	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roughness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_roughness", "get_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tile_size", PROPERTY_HINT_RANGE, "0.01,256,0.01,or_greater,suffix:m"), "set_tile_size", "get_tile_size");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "triplanar"), "set_triplanar", "is_triplanar");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detiling", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_detiling", "get_detiling");
}
