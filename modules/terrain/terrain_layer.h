/**************************************************************************/
/*  terrain_layer.h                                                       */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#pragma once

#include "core/io/resource.h"
#include "scene/resources/texture.h"

// One surface the terrain can be painted with. Shared freely between terrains:
// the per terrain part, how much of it lies where, lives in TerrainData.
class TerrainLayer : public Resource {
	GDCLASS(TerrainLayer, Resource);

	String surface_name;
	Ref<Texture2D> albedo_texture;
	Ref<Texture2D> normal_texture;
	Color albedo_color = Color(1, 1, 1);
	float roughness = 0.9;
	float normal_strength = 1.0;
	float tile_size = 4.0;
	bool triplanar = false;
	float detiling = 0.0;

	// Bumped whenever the pixels behind the layer change, so the terrain can
	// tell a new texture from a new tint and skip re-uploading its arrays.
	uint32_t texture_version = 0;

	void _texture_changed();

protected:
	static void _bind_methods();

public:
	void set_surface_name(const String &p_name);
	String get_surface_name() const { return surface_name; }

	void set_albedo_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_albedo_texture() const { return albedo_texture; }
	uint32_t get_texture_version() const { return texture_version; }
	void set_normal_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_normal_texture() const { return normal_texture; }

	void set_albedo_color(const Color &p_color);
	Color get_albedo_color() const { return albedo_color; }
	void set_roughness(float p_roughness);
	float get_roughness() const { return roughness; }
	void set_normal_strength(float p_strength);
	float get_normal_strength() const { return normal_strength; }
	void set_tile_size(float p_size);
	float get_tile_size() const { return tile_size; }
	void set_triplanar(bool p_enabled);
	bool is_triplanar() const { return triplanar; }
	void set_detiling(float p_amount);
	float get_detiling() const { return detiling; }

	// What the dock calls this layer: the surface name if one was given, then
	// the resource name, then the albedo texture's file name.
	String get_display_name() const;
};
