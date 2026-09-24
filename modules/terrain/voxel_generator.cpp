/**************************************************************************/
/*  voxel_generator.cpp                                                   */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#include "voxel_generator.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

#include "modules/noise/noise.h"

namespace {

// Caves fade in over this many metres at the top and bottom of their depth
// range, rather than stopping at a flat ceiling or floor.
constexpr float CAVE_FADE = 4.0;
// Cave noise is not a distance; this scales it into something close enough
// for the surface to be placed smoothly between samples.
constexpr float CAVE_SDF_SCALE = 16.0;

} // namespace

void VoxelGenerator::_noise_changed() {
	emit_changed();
}

void VoxelGenerator::_source_changed() {
	source_range_valid = false;
	emit_changed();
}

void VoxelGenerator::_source_region_changed(const Vector2i &p_region, int p_maps) {
	// Painting the heightmap after converting it is allowed, but the voxel
	// terrain only picks it up on the next rebuild; this keeps the bounds
	// honest in the meantime.
	source_range_valid = false;
}

void VoxelGenerator::set_height(float p_height) {
	height = p_height;
	emit_changed();
}

void VoxelGenerator::set_height_noise(const Ref<Noise> &p_noise) {
	if (height_noise.is_valid()) {
		height_noise->disconnect_changed(callable_mp(this, &VoxelGenerator::_noise_changed));
	}
	height_noise = p_noise;
	if (height_noise.is_valid()) {
		height_noise->connect_changed(callable_mp(this, &VoxelGenerator::_noise_changed));
	}
	emit_changed();
}

Ref<Noise> VoxelGenerator::get_height_noise() const {
	return height_noise;
}

void VoxelGenerator::set_height_amplitude(float p_amplitude) {
	height_amplitude = MAX(p_amplitude, 0.0f);
	emit_changed();
}

void VoxelGenerator::set_height_source(const Ref<TerrainData> &p_source) {
	if (height_source.is_valid()) {
		height_source->disconnect(SNAME("regions_changed"), callable_mp(this, &VoxelGenerator::_source_changed));
		height_source->disconnect(SNAME("region_changed"), callable_mp(this, &VoxelGenerator::_source_region_changed));
	}
	height_source = p_source;
	if (height_source.is_valid()) {
		height_source->connect(SNAME("regions_changed"), callable_mp(this, &VoxelGenerator::_source_changed));
		height_source->connect(SNAME("region_changed"), callable_mp(this, &VoxelGenerator::_source_region_changed));
	}
	_source_changed();
}

void VoxelGenerator::set_height_source_spacing(float p_spacing) {
	height_source_spacing = CLAMP(p_spacing, 0.05f, 64.0f);
	emit_changed();
}

void VoxelGenerator::set_height_source_offset(const Vector3 &p_offset) {
	height_source_offset = p_offset;
	emit_changed();
}

void VoxelGenerator::set_cave_noise(const Ref<Noise> &p_noise) {
	if (cave_noise.is_valid()) {
		cave_noise->disconnect_changed(callable_mp(this, &VoxelGenerator::_noise_changed));
	}
	cave_noise = p_noise;
	if (cave_noise.is_valid()) {
		cave_noise->connect_changed(callable_mp(this, &VoxelGenerator::_noise_changed));
	}
	emit_changed();
}

Ref<Noise> VoxelGenerator::get_cave_noise() const {
	return cave_noise;
}

void VoxelGenerator::set_cave_threshold(float p_threshold) {
	cave_threshold = CLAMP(p_threshold, -1.0f, 1.0f);
	emit_changed();
}

void VoxelGenerator::set_cave_min_depth(float p_depth) {
	cave_min_depth = MAX(p_depth, 0.0f);
	emit_changed();
}

void VoxelGenerator::set_cave_max_depth(float p_depth) {
	cave_max_depth = MAX(p_depth, 1.0f);
	emit_changed();
}

void VoxelGenerator::set_surface_layer(int p_layer) {
	surface_layer = CLAMP(p_layer, 0, TerrainData::MAX_LAYERS - 1);
	emit_changed();
}

void VoxelGenerator::set_deep_layer(int p_layer) {
	deep_layer = CLAMP(p_layer, 0, TerrainData::MAX_LAYERS - 1);
	emit_changed();
}

void VoxelGenerator::set_deep_layer_depth(float p_depth) {
	deep_layer_depth = MAX(p_depth, 0.0f);
	emit_changed();
}

float VoxelGenerator::get_surface_height(float p_x, float p_z, int *r_layer) const {
	float h = height;
	int layer = surface_layer;
	if (height_source.is_valid()) {
		const Vector2 v = Vector2(p_x - height_source_offset.x, p_z - height_source_offset.z) / height_source_spacing;
		float source_h;
		if (height_source->get_height_interpolated(v, source_h)) {
			h = source_h + height_source_offset.y;
			if (r_layer && height_source->get_layer_count() > 0) {
				// The heightmap's own paint carries over, dominant layer first.
				float w[TerrainData::MAX_LAYERS];
				height_source->get_layer_weights_interpolated(v, w);
				float best = -1.0;
				for (int i = 0; i < height_source->get_layer_count(); i++) {
					if (w[i] > best) {
						best = w[i];
						layer = i;
					}
				}
			}
		}
	}
	if (height_noise.is_valid()) {
		h += height_noise->get_noise_2d(p_x, p_z) * height_amplitude;
	}
	if (r_layer) {
		*r_layer = layer;
	}
	return h;
}

float VoxelGenerator::_cave_sdf(float p_depth, const Vector3 &p_world, float p_inv_voxel) const {
	if (p_depth < cave_min_depth - CAVE_FADE || p_depth > cave_max_depth + CAVE_FADE) {
		return -Math::INF;
	}
	float penalty = 0.0;
	if (p_depth < cave_min_depth) {
		penalty += (cave_min_depth - p_depth) / CAVE_FADE;
	}
	if (p_depth > cave_max_depth) {
		penalty += (p_depth - cave_max_depth) / CAVE_FADE;
	}
	// Positive where the noise clears the threshold: air, inside a cave.
	const float f = cave_noise->get_noise_3d(p_world.x, p_world.y, p_world.z) - cave_threshold - penalty;
	return f * CAVE_SDF_SCALE * p_inv_voxel;
}

void VoxelGenerator::generate(const Vector3i &p_origin, int p_step, const Vector3i &p_size, float p_voxel_size, float *r_sdf, uint8_t *r_material) const {
	const float inv_voxel = 1.0f / p_voxel_size;
	const bool caves = cave_noise.is_valid();
	for (int z = 0; z < p_size.z; z++) {
		const float wz = float(p_origin.z + z * p_step) * p_voxel_size;
		for (int x = 0; x < p_size.x; x++) {
			const float wx = float(p_origin.x + x * p_step) * p_voxel_size;
			int layer = surface_layer;
			const float h = get_surface_height(wx, wz, &layer);
			for (int y = 0; y < p_size.y; y++) {
				const float wy = float(p_origin.y + y * p_step) * p_voxel_size;
				const float depth = h - wy;
				float sdf = -depth * inv_voxel;
				uint8_t material = depth < deep_layer_depth ? layer : deep_layer;
				if (caves) {
					const float cave = _cave_sdf(depth, Vector3(wx, wy, wz), inv_voxel);
					if (cave > sdf) {
						sdf = cave;
						material = deep_layer;
					}
				}
				const int i = (z * p_size.y + y) * p_size.x + x;
				r_sdf[i] = CLAMP(sdf, -SDF_LIMIT, SDF_LIMIT);
				r_material[i] = material;
			}
		}
	}
}

float VoxelGenerator::get_sdf(const Vector3 &p_voxel, float p_voxel_size) const {
	const Vector3 w = p_voxel * p_voxel_size;
	const float depth = get_surface_height(w.x, w.z) - w.y;
	float sdf = -depth / p_voxel_size;
	if (cave_noise.is_valid()) {
		sdf = MAX(sdf, _cave_sdf(depth, w, 1.0f / p_voxel_size));
	}
	return CLAMP(sdf, -SDF_LIMIT, SDF_LIMIT);
}

bool VoxelGenerator::may_contain_surface(const AABB &p_voxel_box, float p_voxel_size, bool &r_solid) const {
	float lo = height;
	float hi = height;
	if (height_source.is_valid() && height_source->get_region_count() > 0) {
		// Outside the heightmap's regions the flat height applies, so the
		// range covers both.
		if (!source_range_valid) {
			source_range = height_source->get_height_range();
			source_range_valid = true;
		}
		const Vector2 range = source_range + Vector2(height_source_offset.y, height_source_offset.y);
		lo = MIN(lo, range.x);
		hi = MAX(hi, range.y);
	}
	if (height_noise.is_valid()) {
		// Fractal noise can stray a little past one.
		lo -= height_amplitude * 1.25f;
		hi += height_amplitude * 1.25f;
	}
	// A cell's worth of margin, since the surface is placed between samples.
	const float margin = p_voxel_size * 2.0f;
	const float y0 = p_voxel_box.position.y * p_voxel_size;
	const float y1 = p_voxel_box.get_end().y * p_voxel_size;
	if (y0 > hi + margin) {
		r_solid = false;
		return false;
	}
	const float solid_below = lo - (cave_noise.is_valid() ? cave_max_depth + CAVE_FADE : 0.0f) - margin;
	if (y1 < solid_below) {
		r_solid = true;
		return false;
	}
	return true;
}

void VoxelGenerator::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_height", "height"), &VoxelGenerator::set_height);
	ClassDB::bind_method(D_METHOD("get_height"), &VoxelGenerator::get_height);
	ClassDB::bind_method(D_METHOD("set_height_noise", "noise"), &VoxelGenerator::set_height_noise);
	ClassDB::bind_method(D_METHOD("get_height_noise"), &VoxelGenerator::get_height_noise);
	ClassDB::bind_method(D_METHOD("set_height_amplitude", "amplitude"), &VoxelGenerator::set_height_amplitude);
	ClassDB::bind_method(D_METHOD("get_height_amplitude"), &VoxelGenerator::get_height_amplitude);
	ClassDB::bind_method(D_METHOD("set_height_source", "source"), &VoxelGenerator::set_height_source);
	ClassDB::bind_method(D_METHOD("get_height_source"), &VoxelGenerator::get_height_source);
	ClassDB::bind_method(D_METHOD("set_height_source_spacing", "spacing"), &VoxelGenerator::set_height_source_spacing);
	ClassDB::bind_method(D_METHOD("get_height_source_spacing"), &VoxelGenerator::get_height_source_spacing);
	ClassDB::bind_method(D_METHOD("set_height_source_offset", "offset"), &VoxelGenerator::set_height_source_offset);
	ClassDB::bind_method(D_METHOD("get_height_source_offset"), &VoxelGenerator::get_height_source_offset);
	ClassDB::bind_method(D_METHOD("set_cave_noise", "noise"), &VoxelGenerator::set_cave_noise);
	ClassDB::bind_method(D_METHOD("get_cave_noise"), &VoxelGenerator::get_cave_noise);
	ClassDB::bind_method(D_METHOD("set_cave_threshold", "threshold"), &VoxelGenerator::set_cave_threshold);
	ClassDB::bind_method(D_METHOD("get_cave_threshold"), &VoxelGenerator::get_cave_threshold);
	ClassDB::bind_method(D_METHOD("set_cave_min_depth", "depth"), &VoxelGenerator::set_cave_min_depth);
	ClassDB::bind_method(D_METHOD("get_cave_min_depth"), &VoxelGenerator::get_cave_min_depth);
	ClassDB::bind_method(D_METHOD("set_cave_max_depth", "depth"), &VoxelGenerator::set_cave_max_depth);
	ClassDB::bind_method(D_METHOD("get_cave_max_depth"), &VoxelGenerator::get_cave_max_depth);
	ClassDB::bind_method(D_METHOD("set_surface_layer", "layer"), &VoxelGenerator::set_surface_layer);
	ClassDB::bind_method(D_METHOD("get_surface_layer"), &VoxelGenerator::get_surface_layer);
	ClassDB::bind_method(D_METHOD("set_deep_layer", "layer"), &VoxelGenerator::set_deep_layer);
	ClassDB::bind_method(D_METHOD("get_deep_layer"), &VoxelGenerator::get_deep_layer);
	ClassDB::bind_method(D_METHOD("set_deep_layer_depth", "depth"), &VoxelGenerator::set_deep_layer_depth);
	ClassDB::bind_method(D_METHOD("get_deep_layer_depth"), &VoxelGenerator::get_deep_layer_depth);
	ClassDB::bind_method(D_METHOD("get_surface_height", "x", "z"), &VoxelGenerator::_get_surface_height_bind);

	ADD_GROUP("Surface", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height", PROPERTY_HINT_RANGE, "-1000,1000,0.01,or_less,or_greater,suffix:m"), "set_height", "get_height");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "height_noise", PROPERTY_HINT_RESOURCE_TYPE, "Noise"), "set_height_noise", "get_height_noise");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height_amplitude", PROPERTY_HINT_RANGE, "0,500,0.01,or_greater,suffix:m"), "set_height_amplitude", "get_height_amplitude");
	ADD_GROUP("Height Source", "height_source_");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "height_source", PROPERTY_HINT_RESOURCE_TYPE, "TerrainData"), "set_height_source", "get_height_source");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height_source_spacing", PROPERTY_HINT_RANGE, "0.05,64,0.01,or_greater,suffix:m"), "set_height_source_spacing", "get_height_source_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "height_source_offset", PROPERTY_HINT_NONE, "suffix:m"), "set_height_source_offset", "get_height_source_offset");
	ADD_GROUP("Caves", "cave_");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "cave_noise", PROPERTY_HINT_RESOURCE_TYPE, "Noise"), "set_cave_noise", "get_cave_noise");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cave_threshold", PROPERTY_HINT_RANGE, "-1,1,0.01"), "set_cave_threshold", "get_cave_threshold");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cave_min_depth", PROPERTY_HINT_RANGE, "0,200,0.1,or_greater,suffix:m"), "set_cave_min_depth", "get_cave_min_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cave_max_depth", PROPERTY_HINT_RANGE, "1,1000,0.1,or_greater,suffix:m"), "set_cave_max_depth", "get_cave_max_depth");
	ADD_GROUP("Layers", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "surface_layer", PROPERTY_HINT_RANGE, "0,15,1"), "set_surface_layer", "get_surface_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "deep_layer", PROPERTY_HINT_RANGE, "0,15,1"), "set_deep_layer", "get_deep_layer");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "deep_layer_depth", PROPERTY_HINT_RANGE, "0,100,0.1,or_greater,suffix:m"), "set_deep_layer_depth", "get_deep_layer_depth");
}
