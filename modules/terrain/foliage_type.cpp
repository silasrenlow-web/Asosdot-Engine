/**************************************************************************/
/*  foliage_type.cpp                                                      */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#include "foliage_type.h"

#include "terrain_data.h"
#include "terrain_shaders.h"

#include "core/math/random_pcg.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

namespace {

Ref<Shader> shared_grass_shader;

Ref<Shader> get_grass_shader() {
	if (shared_grass_shader.is_null()) {
		shared_grass_shader.instantiate();
		shared_grass_shader->set_code(GRASS_SHADER_CODE);
	}
	return shared_grass_shader;
}

} // namespace

void FoliageType::free_shared() {
	shared_grass_shader.unref();
}

/* -------------------------------------------------------------------------- */
/*                                Built-in grass                              */
/* -------------------------------------------------------------------------- */

void FoliageType::_rebuild_builtin_mesh() const {
	builtin_mesh_dirty = false;
	if (builtin_mesh.is_null()) {
		builtin_mesh.instantiate();
	}
	builtin_mesh->clear_surfaces();

	PackedVector3Array vertices;
	PackedVector3Array normals;
	PackedVector2Array uvs;
	PackedInt32Array indices;

	// A tuft of tapered blades. The same seed every time, so every clump in a
	// field is the same mesh and only the instance transform varies.
	RandomPCG rng(0x6a55);
	const int segments = 3;
	const float spread = MAX(grass_width * 2.5f, 0.04f);
	for (int b = 0; b < grass_blades; b++) {
		const float angle = rng.randf() * Math::TAU;
		const float radius = Math::sqrt(rng.randf()) * spread;
		const float along = rng.randf() * Math::TAU;
		const Vector3 root(Math::cos(along) * radius, 0.0, Math::sin(along) * radius);
		const Vector3 across(Math::cos(angle), 0.0, Math::sin(angle));
		const Vector3 facing(-across.z, 0.0, across.x);
		const float height = grass_height * (0.65f + 0.35f * rng.randf());
		const float bend = height * (0.15f + 0.25f * rng.randf());

		const int base = vertices.size();
		for (int s = 0; s <= segments; s++) {
			const float t = float(s) / segments;
			const Vector3 centre = root + Vector3(0.0, height * t, 0.0) + facing * (bend * t * t);
			// The tangent of the bend, tilted back, gives a normal that lets the
			// blade catch light on its face.
			const Vector3 n = (facing - Vector3(0.0, 2.0f * bend * t / MAX(height, 0.001f), 0.0)).normalized();
			if (s == segments) {
				vertices.push_back(centre);
				normals.push_back(n);
				uvs.push_back(Vector2(0.5, 0.0));
			} else {
				const float half = grass_width * 0.5f * (1.0f - t);
				vertices.push_back(centre - across * half);
				vertices.push_back(centre + across * half);
				normals.push_back(n);
				normals.push_back(n);
				uvs.push_back(Vector2(0.0, 1.0 - t));
				uvs.push_back(Vector2(1.0, 1.0 - t));
			}
		}
		for (int s = 0; s < segments - 1; s++) {
			const int a = base + s * 2;
			indices.push_back(a);
			indices.push_back(a + 2);
			indices.push_back(a + 1);
			indices.push_back(a + 1);
			indices.push_back(a + 2);
			indices.push_back(a + 3);
		}
		const int last = base + (segments - 1) * 2;
		indices.push_back(last);
		indices.push_back(last + 2);
		indices.push_back(last + 1);
	}

	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_INDEX] = indices;
	builtin_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
}

void FoliageType::_update_builtin_material() const {
	if (builtin_material.is_null()) {
		builtin_material.instantiate();
		builtin_material->set_shader(get_grass_shader());
	}
	builtin_material->set_shader_parameter(SNAME("color_base"), grass_base_color);
	builtin_material->set_shader_parameter(SNAME("color_tip"), grass_tip_color);
	builtin_material->set_shader_parameter(SNAME("wind_strength"), wind_strength);
}

Ref<Mesh> FoliageType::get_render_mesh() const {
	if (mesh.is_valid()) {
		return mesh;
	}
	if (kind != KIND_GRASS) {
		return Ref<Mesh>();
	}
	if (builtin_mesh_dirty || builtin_mesh.is_null()) {
		_rebuild_builtin_mesh();
	}
	return builtin_mesh;
}

Ref<Material> FoliageType::get_render_material() const {
	if (material_override.is_valid()) {
		return material_override;
	}
	if (uses_builtin_grass()) {
		_update_builtin_material();
		return builtin_material;
	}
	return Ref<Material>();
}

bool FoliageType::accepts(float p_slope_degrees, const float *p_layer_weights, int p_layer_count) const {
	if (p_slope_degrees < slope_min || p_slope_degrees > slope_max) {
		return false;
	}
	if (layer_mask == 0) {
		return true;
	}
	float allowed = 0.0;
	for (int i = 0; i < MIN(p_layer_count, TerrainData::MAX_LAYERS); i++) {
		if (layer_mask & (1u << i)) {
			allowed += p_layer_weights[i];
		}
	}
	return allowed >= layer_threshold;
}

/* -------------------------------------------------------------------------- */
/*                                  Properties                                */
/* -------------------------------------------------------------------------- */

void FoliageType::set_kind(Kind p_kind) {
	kind = p_kind;
	notify_property_list_changed();
	emit_changed();
}

void FoliageType::set_mesh(const Ref<Mesh> &p_mesh) {
	mesh = p_mesh;
	notify_property_list_changed();
	emit_changed();
}

void FoliageType::set_scene(const Ref<PackedScene> &p_scene) {
	scene = p_scene;
	notify_property_list_changed();
	emit_changed();
}

void FoliageType::set_material_override(const Ref<Material> &p_material) {
	material_override = p_material;
	emit_changed();
}

void FoliageType::set_density(float p_density) {
	density = CLAMP(p_density, 0.01f, 200.0f);
	emit_changed();
}

void FoliageType::set_spacing(float p_spacing) {
	spacing = MAX(p_spacing, 0.05f);
	emit_changed();
}

void FoliageType::set_scale_min(float p_scale) {
	scale_min = MAX(p_scale, 0.001f);
	emit_changed();
}

void FoliageType::set_scale_max(float p_scale) {
	scale_max = MAX(p_scale, 0.001f);
	emit_changed();
}

void FoliageType::set_random_rotation(bool p_enabled) {
	random_rotation = p_enabled;
	emit_changed();
}

void FoliageType::set_align_to_normal(float p_amount) {
	align_to_normal = CLAMP(p_amount, 0.0f, 1.0f);
	emit_changed();
}

void FoliageType::set_random_tilt(float p_degrees) {
	random_tilt = CLAMP(p_degrees, 0.0f, 90.0f);
	emit_changed();
}

void FoliageType::set_height_offset(float p_offset) {
	height_offset = p_offset;
	emit_changed();
}

void FoliageType::set_slope_min(float p_degrees) {
	slope_min = CLAMP(p_degrees, 0.0f, 90.0f);
	emit_changed();
}

void FoliageType::set_slope_max(float p_degrees) {
	slope_max = CLAMP(p_degrees, 0.0f, 90.0f);
	emit_changed();
}

void FoliageType::set_layer_mask(uint32_t p_mask) {
	layer_mask = p_mask & ((1u << TerrainData::MAX_LAYERS) - 1);
	notify_property_list_changed();
	emit_changed();
}

void FoliageType::set_layer_threshold(float p_threshold) {
	layer_threshold = CLAMP(p_threshold, 0.0f, 1.0f);
	emit_changed();
}

void FoliageType::set_visibility_range(float p_range) {
	visibility_range = MAX(p_range, 0.0f);
	emit_changed();
}

void FoliageType::set_cast_shadow(bool p_enabled) {
	cast_shadow = p_enabled;
	emit_changed();
}

void FoliageType::set_color_variation(float p_amount) {
	color_variation = CLAMP(p_amount, 0.0f, 1.0f);
	emit_changed();
}

void FoliageType::set_terrain_tint(bool p_enabled) {
	terrain_tint = p_enabled;
	emit_changed();
}

void FoliageType::set_collision_shape(const Ref<Shape3D> &p_shape) {
	collision_shape = p_shape;
	emit_changed();
}

void FoliageType::set_grass_height(float p_height) {
	grass_height = CLAMP(p_height, 0.02f, 10.0f);
	builtin_mesh_dirty = true;
	emit_changed();
}

void FoliageType::set_grass_width(float p_width) {
	grass_width = CLAMP(p_width, 0.002f, 1.0f);
	builtin_mesh_dirty = true;
	emit_changed();
}

void FoliageType::set_grass_blades(int p_blades) {
	grass_blades = CLAMP(p_blades, 1, 64);
	builtin_mesh_dirty = true;
	emit_changed();
}

void FoliageType::set_grass_base_color(const Color &p_color) {
	grass_base_color = p_color;
	if (builtin_material.is_valid()) {
		_update_builtin_material();
	}
	emit_changed();
}

void FoliageType::set_grass_tip_color(const Color &p_color) {
	grass_tip_color = p_color;
	if (builtin_material.is_valid()) {
		_update_builtin_material();
	}
	emit_changed();
}

void FoliageType::set_wind_strength(float p_strength) {
	wind_strength = CLAMP(p_strength, 0.0f, 4.0f);
	if (builtin_material.is_valid()) {
		_update_builtin_material();
	}
	emit_changed();
}

void FoliageType::_validate_property(PropertyInfo &p_property) const {
	const bool grass = kind == KIND_GRASS;
	if (!grass && (p_property.name == "density" || p_property.name == "terrain_tint")) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	if (grass && (p_property.name == "spacing" || p_property.name == "scene" || p_property.name == "collision_shape")) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	if (p_property.name.begins_with("grass_") || p_property.name == "wind_strength") {
		if (!uses_builtin_grass()) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	}
	if (p_property.name == "layer_threshold" && layer_mask == 0) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	if (scene.is_valid() && !grass && (p_property.name == "mesh" || p_property.name == "material_override" || p_property.name == "collision_shape")) {
		// A scene brings its own meshes, materials and collision.
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

void FoliageType::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_kind", "kind"), &FoliageType::set_kind);
	ClassDB::bind_method(D_METHOD("get_kind"), &FoliageType::get_kind);
	ClassDB::bind_method(D_METHOD("set_mesh", "mesh"), &FoliageType::set_mesh);
	ClassDB::bind_method(D_METHOD("get_mesh"), &FoliageType::get_mesh);
	ClassDB::bind_method(D_METHOD("set_scene", "scene"), &FoliageType::set_scene);
	ClassDB::bind_method(D_METHOD("get_scene"), &FoliageType::get_scene);
	ClassDB::bind_method(D_METHOD("set_material_override", "material"), &FoliageType::set_material_override);
	ClassDB::bind_method(D_METHOD("get_material_override"), &FoliageType::get_material_override);
	ClassDB::bind_method(D_METHOD("set_density", "density"), &FoliageType::set_density);
	ClassDB::bind_method(D_METHOD("get_density"), &FoliageType::get_density);
	ClassDB::bind_method(D_METHOD("set_spacing", "spacing"), &FoliageType::set_spacing);
	ClassDB::bind_method(D_METHOD("get_spacing"), &FoliageType::get_spacing);
	ClassDB::bind_method(D_METHOD("set_scale_min", "scale"), &FoliageType::set_scale_min);
	ClassDB::bind_method(D_METHOD("get_scale_min"), &FoliageType::get_scale_min);
	ClassDB::bind_method(D_METHOD("set_scale_max", "scale"), &FoliageType::set_scale_max);
	ClassDB::bind_method(D_METHOD("get_scale_max"), &FoliageType::get_scale_max);
	ClassDB::bind_method(D_METHOD("set_random_rotation", "enabled"), &FoliageType::set_random_rotation);
	ClassDB::bind_method(D_METHOD("is_random_rotation"), &FoliageType::is_random_rotation);
	ClassDB::bind_method(D_METHOD("set_align_to_normal", "amount"), &FoliageType::set_align_to_normal);
	ClassDB::bind_method(D_METHOD("get_align_to_normal"), &FoliageType::get_align_to_normal);
	ClassDB::bind_method(D_METHOD("set_random_tilt", "degrees"), &FoliageType::set_random_tilt);
	ClassDB::bind_method(D_METHOD("get_random_tilt"), &FoliageType::get_random_tilt);
	ClassDB::bind_method(D_METHOD("set_height_offset", "offset"), &FoliageType::set_height_offset);
	ClassDB::bind_method(D_METHOD("get_height_offset"), &FoliageType::get_height_offset);
	ClassDB::bind_method(D_METHOD("set_slope_min", "degrees"), &FoliageType::set_slope_min);
	ClassDB::bind_method(D_METHOD("get_slope_min"), &FoliageType::get_slope_min);
	ClassDB::bind_method(D_METHOD("set_slope_max", "degrees"), &FoliageType::set_slope_max);
	ClassDB::bind_method(D_METHOD("get_slope_max"), &FoliageType::get_slope_max);
	ClassDB::bind_method(D_METHOD("set_layer_mask", "mask"), &FoliageType::set_layer_mask);
	ClassDB::bind_method(D_METHOD("get_layer_mask"), &FoliageType::get_layer_mask);
	ClassDB::bind_method(D_METHOD("set_layer_threshold", "threshold"), &FoliageType::set_layer_threshold);
	ClassDB::bind_method(D_METHOD("get_layer_threshold"), &FoliageType::get_layer_threshold);
	ClassDB::bind_method(D_METHOD("set_visibility_range", "range"), &FoliageType::set_visibility_range);
	ClassDB::bind_method(D_METHOD("get_visibility_range"), &FoliageType::get_visibility_range);
	ClassDB::bind_method(D_METHOD("set_cast_shadow", "enabled"), &FoliageType::set_cast_shadow);
	ClassDB::bind_method(D_METHOD("is_cast_shadow"), &FoliageType::is_cast_shadow);
	ClassDB::bind_method(D_METHOD("set_color_variation", "amount"), &FoliageType::set_color_variation);
	ClassDB::bind_method(D_METHOD("get_color_variation"), &FoliageType::get_color_variation);
	ClassDB::bind_method(D_METHOD("set_terrain_tint", "enabled"), &FoliageType::set_terrain_tint);
	ClassDB::bind_method(D_METHOD("is_terrain_tint"), &FoliageType::is_terrain_tint);
	ClassDB::bind_method(D_METHOD("set_collision_shape", "shape"), &FoliageType::set_collision_shape);
	ClassDB::bind_method(D_METHOD("get_collision_shape"), &FoliageType::get_collision_shape);
	ClassDB::bind_method(D_METHOD("set_grass_height", "height"), &FoliageType::set_grass_height);
	ClassDB::bind_method(D_METHOD("get_grass_height"), &FoliageType::get_grass_height);
	ClassDB::bind_method(D_METHOD("set_grass_width", "width"), &FoliageType::set_grass_width);
	ClassDB::bind_method(D_METHOD("get_grass_width"), &FoliageType::get_grass_width);
	ClassDB::bind_method(D_METHOD("set_grass_blades", "blades"), &FoliageType::set_grass_blades);
	ClassDB::bind_method(D_METHOD("get_grass_blades"), &FoliageType::get_grass_blades);
	ClassDB::bind_method(D_METHOD("set_grass_base_color", "color"), &FoliageType::set_grass_base_color);
	ClassDB::bind_method(D_METHOD("get_grass_base_color"), &FoliageType::get_grass_base_color);
	ClassDB::bind_method(D_METHOD("set_grass_tip_color", "color"), &FoliageType::set_grass_tip_color);
	ClassDB::bind_method(D_METHOD("get_grass_tip_color"), &FoliageType::get_grass_tip_color);
	ClassDB::bind_method(D_METHOD("set_wind_strength", "strength"), &FoliageType::set_wind_strength);
	ClassDB::bind_method(D_METHOD("get_wind_strength"), &FoliageType::get_wind_strength);
	ClassDB::bind_method(D_METHOD("get_render_mesh"), &FoliageType::get_render_mesh);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "kind", PROPERTY_HINT_ENUM, "Grass,Object"), "set_kind", "get_kind");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "mesh", PROPERTY_HINT_RESOURCE_TYPE, "Mesh"), "set_mesh", "get_mesh");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "scene", PROPERTY_HINT_RESOURCE_TYPE, "PackedScene"), "set_scene", "get_scene");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material_override", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material_override", "get_material_override");

	ADD_GROUP("Placement", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "density", PROPERTY_HINT_RANGE, "0.01,200,0.01,suffix:/m²"), "set_density", "get_density");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spacing", PROPERTY_HINT_RANGE, "0.05,100,0.01,or_greater,suffix:m"), "set_spacing", "get_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scale_min", PROPERTY_HINT_RANGE, "0.001,10,0.001,or_greater"), "set_scale_min", "get_scale_min");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scale_max", PROPERTY_HINT_RANGE, "0.001,10,0.001,or_greater"), "set_scale_max", "get_scale_max");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "random_rotation"), "set_random_rotation", "is_random_rotation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "align_to_normal", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_align_to_normal", "get_align_to_normal");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "random_tilt", PROPERTY_HINT_RANGE, "0,90,0.1,degrees"), "set_random_tilt", "get_random_tilt");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height_offset", PROPERTY_HINT_RANGE, "-10,10,0.01,or_less,or_greater,suffix:m"), "set_height_offset", "get_height_offset");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "slope_min", PROPERTY_HINT_RANGE, "0,90,0.1,degrees"), "set_slope_min", "get_slope_min");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "slope_max", PROPERTY_HINT_RANGE, "0,90,0.1,degrees"), "set_slope_max", "get_slope_max");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "layer_mask", PROPERTY_HINT_FLAGS, "Layer 0,Layer 1,Layer 2,Layer 3,Layer 4,Layer 5,Layer 6,Layer 7,Layer 8,Layer 9,Layer 10,Layer 11,Layer 12,Layer 13,Layer 14,Layer 15"), "set_layer_mask", "get_layer_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "layer_threshold", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_layer_threshold", "get_layer_threshold");

	ADD_GROUP("Rendering", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "visibility_range", PROPERTY_HINT_RANGE, "0,4096,0.1,or_greater,suffix:m"), "set_visibility_range", "get_visibility_range");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cast_shadow"), "set_cast_shadow", "is_cast_shadow");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "color_variation", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_color_variation", "get_color_variation");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "terrain_tint"), "set_terrain_tint", "is_terrain_tint");

	ADD_GROUP("Collision", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "collision_shape", PROPERTY_HINT_RESOURCE_TYPE, "Shape3D"), "set_collision_shape", "get_collision_shape");

	ADD_GROUP("Built-in Grass", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "grass_height", PROPERTY_HINT_RANGE, "0.02,10,0.01,suffix:m"), "set_grass_height", "get_grass_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "grass_width", PROPERTY_HINT_RANGE, "0.002,1,0.001,suffix:m"), "set_grass_width", "get_grass_width");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "grass_blades", PROPERTY_HINT_RANGE, "1,64,1"), "set_grass_blades", "get_grass_blades");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "grass_base_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_grass_base_color", "get_grass_base_color");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "grass_tip_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_grass_tip_color", "get_grass_tip_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_strength", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_wind_strength", "get_wind_strength");

	BIND_ENUM_CONSTANT(KIND_GRASS);
	BIND_ENUM_CONSTANT(KIND_OBJECT);
}
