/**************************************************************************/
/*  foliage_type.h                                                        */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#pragma once

#include "core/io/resource.h"
#include "scene/resources/3d/shape_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/packed_scene.h"

// One kind of thing that grows on a terrain, and the rules for where.
//
// Grass is painted as a density and grown procedurally around the camera, so
// a field of it costs nothing to store. Objects (trees, rocks, fences, lamp
// posts) are placed individually and kept, and can be a mesh with an optional
// collision shape or a whole scene.
class FoliageType : public Resource {
	GDCLASS(FoliageType, Resource);

public:
	enum Kind {
		KIND_GRASS,
		KIND_OBJECT,
	};

private:
	Kind kind = KIND_GRASS;
	Ref<Mesh> mesh;
	Ref<PackedScene> scene;
	Ref<Material> material_override;

	float density = 8.0;
	float spacing = 3.0;
	float scale_min = 0.8;
	float scale_max = 1.2;
	bool random_rotation = true;
	float align_to_normal = 0.35;
	float random_tilt = 0.0;
	float height_offset = 0.0;
	float slope_min = 0.0;
	float slope_max = 50.0;
	uint32_t layer_mask = 0;
	float layer_threshold = 0.3;

	float visibility_range = 60.0;
	bool cast_shadow = false;
	float color_variation = 0.15;
	bool terrain_tint = true;

	Ref<Shape3D> collision_shape;

	float grass_height = 0.55;
	float grass_width = 0.05;
	int grass_blades = 6;
	Color grass_base_color = Color(0.14, 0.22, 0.06);
	Color grass_tip_color = Color(0.52, 0.6, 0.25);
	float wind_strength = 0.3;

	mutable Ref<ArrayMesh> builtin_mesh;
	mutable Ref<ShaderMaterial> builtin_material;
	mutable bool builtin_mesh_dirty = true;

	void _rebuild_builtin_mesh() const;
	void _update_builtin_material() const;

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	void set_kind(Kind p_kind);
	Kind get_kind() const { return kind; }
	void set_mesh(const Ref<Mesh> &p_mesh);
	Ref<Mesh> get_mesh() const { return mesh; }
	void set_scene(const Ref<PackedScene> &p_scene);
	Ref<PackedScene> get_scene() const { return scene; }
	void set_material_override(const Ref<Material> &p_material);
	Ref<Material> get_material_override() const { return material_override; }

	void set_density(float p_density);
	float get_density() const { return density; }
	void set_spacing(float p_spacing);
	float get_spacing() const { return spacing; }
	void set_scale_min(float p_scale);
	float get_scale_min() const { return scale_min; }
	void set_scale_max(float p_scale);
	float get_scale_max() const { return scale_max; }
	void set_random_rotation(bool p_enabled);
	bool is_random_rotation() const { return random_rotation; }
	void set_align_to_normal(float p_amount);
	float get_align_to_normal() const { return align_to_normal; }
	void set_random_tilt(float p_degrees);
	float get_random_tilt() const { return random_tilt; }
	void set_height_offset(float p_offset);
	float get_height_offset() const { return height_offset; }
	void set_slope_min(float p_degrees);
	float get_slope_min() const { return slope_min; }
	void set_slope_max(float p_degrees);
	float get_slope_max() const { return slope_max; }
	void set_layer_mask(uint32_t p_mask);
	uint32_t get_layer_mask() const { return layer_mask; }
	void set_layer_threshold(float p_threshold);
	float get_layer_threshold() const { return layer_threshold; }

	void set_visibility_range(float p_range);
	float get_visibility_range() const { return visibility_range; }
	void set_cast_shadow(bool p_enabled);
	bool is_cast_shadow() const { return cast_shadow; }
	void set_color_variation(float p_amount);
	float get_color_variation() const { return color_variation; }
	void set_terrain_tint(bool p_enabled);
	bool is_terrain_tint() const { return terrain_tint; }

	void set_collision_shape(const Ref<Shape3D> &p_shape);
	Ref<Shape3D> get_collision_shape() const { return collision_shape; }

	void set_grass_height(float p_height);
	float get_grass_height() const { return grass_height; }
	void set_grass_width(float p_width);
	float get_grass_width() const { return grass_width; }
	void set_grass_blades(int p_blades);
	int get_grass_blades() const { return grass_blades; }
	void set_grass_base_color(const Color &p_color);
	Color get_grass_base_color() const { return grass_base_color; }
	void set_grass_tip_color(const Color &p_color);
	Color get_grass_tip_color() const { return grass_tip_color; }
	void set_wind_strength(float p_strength);
	float get_wind_strength() const { return wind_strength; }

	// What actually gets drawn: the assigned mesh, or for grass without one the
	// built-in blades with their wind shader.
	bool uses_builtin_grass() const { return kind == KIND_GRASS && mesh.is_null(); }
	Ref<Mesh> get_render_mesh() const;
	Ref<Material> get_render_material() const;

	// Whether the rules allow growth at a spot. Slope is in degrees.
	bool accepts(float p_slope_degrees, const float *p_layer_weights, int p_layer_count) const;

	static void free_shared();
};

VARIANT_ENUM_CAST(FoliageType::Kind);
