/**************************************************************************/
/*  terrain_brush.cpp                                                     */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#include "terrain_brush.h"

#include "../foliage_3d.h"
#include "../terrain_3d.h"

#include "core/math/random_pcg.h"
#include "core/templates/hashfuncs.h"

namespace {

struct HeightWindow {
	Rect2i rect;
	Vector<float> heights;
	Vector<uint8_t> valid;

	void read(const TerrainData *p_data, const Rect2i &p_rect) {
		rect = p_rect;
		heights.resize(rect.size.x * rect.size.y);
		valid.resize(rect.size.x * rect.size.y);
		p_data->read_heights(rect, heights.ptrw(), valid.ptrw());
	}
};

float hash_to_unit(uint32_t p_h) {
	return (p_h & 0xFFFFFF) / float(0xFFFFFF);
}

float lattice(int p_x, int p_z, uint32_t p_seed) {
	uint32_t h = hash_murmur3_one_32((uint32_t)p_x, p_seed);
	h = hash_murmur3_one_32((uint32_t)p_z, h);
	return hash_to_unit(hash_fmix32(h));
}

float value_noise(float p_x, float p_z, uint32_t p_seed) {
	const int ix = (int)Math::floor(p_x);
	const int iz = (int)Math::floor(p_z);
	float fx = p_x - ix;
	float fz = p_z - iz;
	fx = fx * fx * (3.0f - 2.0f * fx);
	fz = fz * fz * (3.0f - 2.0f * fz);
	const float a = Math::lerp(lattice(ix, iz, p_seed), lattice(ix + 1, iz, p_seed), fx);
	const float b = Math::lerp(lattice(ix, iz + 1, p_seed), lattice(ix + 1, iz + 1, p_seed), fx);
	return Math::lerp(a, b, fz);
}

// Brush weights have to sum to 255 exactly or the layers drift over many
// strokes; whatever rounding leaves over goes to the largest weight.
void quantize_weights(const float *p_weights, uint8_t *r_bytes) {
	int total = 0;
	int largest = 0;
	for (int i = 0; i < TerrainData::MAX_LAYERS; i++) {
		r_bytes[i] = (uint8_t)CLAMP((int)Math::round(p_weights[i] * 255.0f), 0, 255);
		total += r_bytes[i];
		largest = p_weights[i] > p_weights[largest] ? i : largest;
	}
	r_bytes[largest] = (uint8_t)CLAMP((int)r_bytes[largest] + (255 - total), 0, 255);
}

} // namespace

float TerrainBrush::falloff(float p_distance, float p_radius, float p_hardness) {
	if (p_distance >= p_radius) {
		return 0.0;
	}
	const float inner = p_radius * CLAMP(p_hardness, 0.0f, 0.999f);
	if (p_distance <= inner) {
		return 1.0;
	}
	const float t = (p_distance - inner) / (p_radius - inner);
	return 1.0f - t * t * (3.0f - 2.0f * t);
}

float TerrainBrush::fractal_noise(float p_x, float p_z, uint32_t p_seed) {
	float sum = 0.0;
	float amplitude = 0.5;
	float frequency = 1.0;
	float norm = 0.0;
	for (int octave = 0; octave < 5; octave++) {
		sum += value_noise(p_x * frequency, p_z * frequency, p_seed + octave * 1013) * amplitude;
		norm += amplitude;
		amplitude *= 0.5f;
		frequency *= 2.03f;
	}
	return sum / norm;
}

uint32_t TerrainBrush::maps_for(Tool p_tool) {
	switch (p_tool) {
		case TOOL_RAISE:
		case TOOL_SMOOTH:
		case TOOL_FLATTEN:
		case TOOL_ERODE:
		case TOOL_NOISE:
		case TOOL_TERRACE:
		case TOOL_RAMP:
			return TerrainData::MAP_HEIGHT;
		case TOOL_HOLES:
			return TerrainData::MAP_HOLES;
		case TOOL_PAINT:
			return TerrainData::MAP_SPLAT;
		case TOOL_COLOR:
		case TOOL_WETNESS:
			return TerrainData::MAP_COLOR;
		case TOOL_REGIONS:
			return TerrainData::MAP_ALL;
		default:
			return 0;
	}
}

const char *TerrainBrush::get_tool_name(Tool p_tool) {
	static const char *names[TOOL_MAX] = {
		"Select",
		"Raise / Lower",
		"Smooth",
		"Flatten",
		"Erode",
		"Noise",
		"Terrace",
		"Ramp",
		"Holes",
		"Paint Layer",
		"Paint Color",
		"Paint Wetness",
		"Paint Grass",
		"Paint Objects",
		"Regions",
		"Dig",
	};
	return (p_tool >= 0 && p_tool < TOOL_MAX) ? names[p_tool] : "";
}

void TerrainBrush::apply_foliage(Foliage3D *p_foliage, Tool p_tool, const Vector3 &p_local, const Settings &p_settings, float p_delta, uint32_t p_seed) {
	if (!p_foliage || p_foliage->get_data().is_null()) {
		return;
	}
	const Ref<FoliageData> fd = p_foliage->get_data();
	const int type = p_settings.foliage_type;
	if (type < 0 || type >= fd->get_type_count()) {
		return;
	}
	const Vector2 local2(p_local.x, p_local.z);
	const float sign = p_settings.invert ? -1.0f : 1.0f;

	if (p_tool == TOOL_GRASS) {
		// Density is painted from above, the same on either kind of ground.
		const float ts = fd->get_density_texel_size();
		const Vector2i t0 = fd->local_to_texel(local2 - Vector2(p_settings.radius, p_settings.radius));
		const Vector2i t1 = fd->local_to_texel(local2 + Vector2(p_settings.radius, p_settings.radius));
		const float rate = MIN(1.0f, p_settings.strength * p_delta * 3.0f);
		bool changed = false;
		for (int tz = t0.y; tz <= t1.y; tz++) {
			for (int tx = t0.x; tx <= t1.x; tx++) {
				const Vector2 at = (Vector2(tx, tz) + Vector2(0.5, 0.5)) * ts;
				const float f = falloff(at.distance_to(local2), p_settings.radius, p_settings.hardness);
				if (f <= 0.0f) {
					continue;
				}
				uint8_t *px = fd->density_texel(type, Vector2i(tx, tz), !p_settings.invert);
				if (!px) {
					continue;
				}
				const int v = CLAMP((int)Math::round((*px / 255.0f + sign * rate * f) * 255.0f), 0, 255);
				changed = changed || v != *px;
				*px = (uint8_t)v;
			}
		}
		if (changed) {
			fd->notify_density_changed(type, Rect2(local2 - Vector2(p_settings.radius, p_settings.radius), Vector2(p_settings.radius, p_settings.radius) * 2.0f).grow(ts * 2.0f));
		}
		return;
	}

	if (p_tool != TOOL_OBJECTS) {
		return;
	}
	const Ref<FoliageType> ft = fd->get_type(type);
	if (ft.is_null() || ft->get_kind() != FoliageType::KIND_OBJECT) {
		return;
	}
	// On voxel ground an object is placed on the ground nearest the brush,
	// which is a sphere there: a cave floor, a ledge, under an arch.
	const bool voxel = p_foliage->is_on_voxel_terrain();
	RandomPCG rng(p_seed);
	if (p_settings.invert) {
		const float probability = CLAMP(p_settings.strength * p_delta * 6.0f, 0.02f, 1.0f);
		if (voxel) {
			fd->remove_instances_in_sphere(type, p_local, p_settings.radius, probability, p_seed);
		} else {
			fd->remove_instances_in_radius(type, local2, p_settings.radius, probability, p_seed);
		}
		return;
	}
	const float spacing = ft->get_spacing();
	const float expected = Math::PI * p_settings.radius * p_settings.radius / (spacing * spacing) * p_settings.strength * p_delta * 1.5f;
	int wanted = (int)Math::floor(expected);
	if (rng.randf() < expected - wanted) {
		wanted++;
	}
	wanted = MIN(wanted, 64);
	LocalVector<Vector3> placed;
	for (int attempt = 0; attempt < wanted * 4 && (int)placed.size() < wanted; attempt++) {
		const float angle = rng.randf() * Math::TAU;
		const float dist = Math::sqrt(rng.randf()) * p_settings.radius;
		const Vector2 at = local2 + Vector2(Math::cos(angle), Math::sin(angle)) * dist;
		if (rng.randf() > falloff(dist, p_settings.radius, p_settings.hardness)) {
			continue;
		}
		const float yaw = rng.randf() * Math::TAU;
		const float scale = Math::lerp(ft->get_scale_min(), ft->get_scale_max(), rng.randf());
		const float variation = rng.randf();
		Transform3D xform;
		if (!p_foliage->compute_placement(type, at, yaw, scale, variation, voxel ? p_local.y : 0.0f, xform, true, voxel ? p_settings.radius + 2.0f : 0.0f)) {
			continue;
		}
		// A heightmap object stores an extra height above the ground (none
		// from the brush); a voxel one the height of the ground it stands on.
		const Vector3 position(at.x, voxel ? xform.origin.y - ft->get_height_offset() : 0.0f, at.y);
		bool crowded = voxel ? fd->has_instance_near_3d(type, position, spacing) : fd->has_instance_near(type, at, spacing);
		for (uint32_t i = 0; i < placed.size() && !crowded; i++) {
			crowded = voxel ? placed[i].distance_to(position) < spacing : Vector2(placed[i].x, placed[i].z).distance_to(at) < spacing;
		}
		if (crowded) {
			continue;
		}
		fd->add_instance(type, position, yaw, scale, variation);
		placed.push_back(position);
	}
}

Rect2i TerrainBrush::get_dab_rect(const Terrain3D *p_terrain, Tool p_tool, const Vector2 &p_local, const Settings &p_settings) {
	const float sp = p_terrain->get_vertex_spacing();
	const Vector2 c = p_local / sp;
	const float r = p_settings.radius / sp;
	int margin = 1;
	if (p_tool == TOOL_ERODE) {
		margin = 6;
	}
	const Vector2i lo = Vector2i((int)Math::floor(c.x - r), (int)Math::floor(c.y - r)) - Vector2i(margin, margin);
	const Vector2i hi = Vector2i((int)Math::ceil(c.x + r), (int)Math::ceil(c.y + r)) + Vector2i(margin, margin);
	return Rect2i(lo, hi - lo + Vector2i(1, 1));
}

/* -------------------------------------------------------------------------- */
/*                                   Sculpt                                   */
/* -------------------------------------------------------------------------- */

static void erode(HeightWindow &r_win, const Vector2 &p_centre, float p_radius_v, const TerrainBrush::Settings &p_settings, float p_spacing, float p_delta, uint32_t p_seed) {
	// Hydraulic erosion: droplets roll downhill from inside the brush, picking
	// up sediment where they speed up and dropping it where they slow down,
	// which carves gullies and fans out deposits the way rain does.
	constexpr float INERTIA = 0.05;
	constexpr float CAPACITY = 4.0;
	constexpr float MIN_CAPACITY = 0.01;
	constexpr float DEPOSIT = 0.3;
	constexpr float ERODE = 0.3;
	constexpr float EVAPORATE = 0.02;
	constexpr float GRAVITY = 4.0;
	constexpr int MAX_STEPS = 40;

	const int w = r_win.rect.size.x;
	const int h = r_win.rect.size.y;
	float *heights = r_win.heights.ptrw();
	const uint8_t *valid = r_win.valid.ptr();
	const auto at = [&](int x, int z) -> float & { return heights[z * w + x]; };
	const auto ok = [&](int x, int z) { return x >= 0 && z >= 0 && x < w - 1 && z < h - 1 && valid[z * w + x] && valid[z * w + x + 1] && valid[(z + 1) * w + x] && valid[(z + 1) * w + x + 1]; };

	struct Tap {
		int x;
		int z;
		float w;
	};
	LocalVector<Tap> taps;
	float tap_sum = 0.0;
	for (int z = -2; z <= 2; z++) {
		for (int x = -2; x <= 2; x++) {
			const float weight = MAX(0.0f, 2.2f - Math::sqrt(float(x * x + z * z)));
			if (weight > 0.0f) {
				taps.push_back({ x, z, weight });
				tap_sum += weight;
			}
		}
	}
	for (Tap &t : taps) {
		t.w /= tap_sum;
	}

	const Vector2 centre = p_centre - Vector2(r_win.rect.position);
	const int droplets = CLAMP((int)(p_settings.strength * p_delta * Math::PI * p_radius_v * p_radius_v * 0.8f), 1, 4000);
	RandomPCG rng(p_seed);
	for (int d = 0; d < droplets; d++) {
		const float angle = rng.randf() * Math::TAU;
		const float dist = Math::sqrt(rng.randf()) * p_radius_v;
		float px = centre.x + Math::cos(angle) * dist;
		float pz = centre.y + Math::sin(angle) * dist;
		const float weight = TerrainBrush::falloff(dist * p_spacing, p_settings.radius, p_settings.hardness);
		float dx = 0.0;
		float dz = 0.0;
		float speed = 1.0;
		float water = 1.0;
		float sediment = 0.0;

		for (int step = 0; step < MAX_STEPS; step++) {
			const int ix = (int)Math::floor(px);
			const int iz = (int)Math::floor(pz);
			if (!ok(ix, iz)) {
				break;
			}
			const float ox = px - ix;
			const float oz = pz - iz;
			const float h00 = at(ix, iz);
			const float h10 = at(ix + 1, iz);
			const float h01 = at(ix, iz + 1);
			const float h11 = at(ix + 1, iz + 1);
			const float gx = (h10 - h00) * (1.0f - oz) + (h11 - h01) * oz;
			const float gz = (h01 - h00) * (1.0f - ox) + (h11 - h10) * ox;
			const float here = h00 * (1 - ox) * (1 - oz) + h10 * ox * (1 - oz) + h01 * (1 - ox) * oz + h11 * ox * oz;

			dx = dx * INERTIA - gx * (1.0f - INERTIA);
			dz = dz * INERTIA - gz * (1.0f - INERTIA);
			const float len = Math::sqrt(dx * dx + dz * dz);
			if (len < 1e-6f) {
				const float a = rng.randf() * Math::TAU;
				dx = Math::cos(a);
				dz = Math::sin(a);
			} else {
				dx /= len;
				dz /= len;
			}
			const float nx = px + dx;
			const float nz = pz + dz;
			const int nix = (int)Math::floor(nx);
			const int niz = (int)Math::floor(nz);
			if (!ok(nix, niz)) {
				break;
			}
			const float fx = nx - nix;
			const float fz = nz - niz;
			const float there = at(nix, niz) * (1 - fx) * (1 - fz) + at(nix + 1, niz) * fx * (1 - fz) + at(nix, niz + 1) * (1 - fx) * fz + at(nix + 1, niz + 1) * fx * fz;
			const float dh = there - here;

			const float capacity = MAX(-dh * speed * water * CAPACITY, MIN_CAPACITY);
			if (sediment > capacity || dh > 0.0f) {
				// Uphill, or carrying too much: drop some where it stands.
				const float amount = (dh > 0.0f ? MIN(dh, sediment) : (sediment - capacity) * DEPOSIT);
				sediment -= amount;
				const float deposit = amount * weight;
				at(ix, iz) += deposit * (1 - ox) * (1 - oz);
				at(ix + 1, iz) += deposit * ox * (1 - oz);
				at(ix, iz + 1) += deposit * (1 - ox) * oz;
				at(ix + 1, iz + 1) += deposit * ox * oz;
			} else {
				const float amount = MIN((capacity - sediment) * ERODE, -dh) * weight;
				for (const Tap &t : taps) {
					const int tx = ix + t.x;
					const int tz = iz + t.z;
					if (tx >= 0 && tz >= 0 && tx < w && tz < h && valid[tz * w + tx]) {
						at(tx, tz) -= amount * t.w;
					}
				}
				sediment += amount;
			}
			speed = Math::sqrt(MAX(0.0f, speed * speed - dh * GRAVITY));
			water *= 1.0f - EVAPORATE;
			px = nx;
			pz = nz;
		}
	}
}

void TerrainBrush::apply(Terrain3D *p_terrain, Foliage3D *p_foliage, Tool p_tool, const Vector2 &p_local, const Settings &p_settings, float p_delta, uint32_t p_seed) {
	const Ref<TerrainData> data = p_terrain ? p_terrain->get_data() : Ref<TerrainData>();
	if (data.is_null()) {
		return;
	}
	const float sp = p_terrain->get_vertex_spacing();
	const Vector2 centre = p_local / sp;
	const float radius_v = p_settings.radius / sp;
	const float sign = p_settings.invert ? -1.0f : 1.0f;
	const Rect2i rect = get_dab_rect(p_terrain, p_tool, p_local, p_settings);
	const int w = rect.size.x;

	// Distance from the brush centre in metres, and how hard the brush bites.
	const auto weight_at = [&](int p_x, int p_z) {
		const Vector2 v = Vector2(rect.position.x + p_x, rect.position.y + p_z);
		return falloff(v.distance_to(centre) * sp, p_settings.radius, p_settings.hardness);
	};

	switch (p_tool) {
		case TOOL_RAISE:
		case TOOL_FLATTEN:
		case TOOL_NOISE:
		case TOOL_TERRACE: {
			HeightWindow win;
			win.read(data.ptr(), rect);
			float *h = win.heights.ptrw();
			// Big brushes move more ground per second, or large hills take an age.
			const float rate = p_settings.strength * (1.5f + p_settings.radius * 0.35f) * p_delta;
			const float blend = MIN(1.0f, p_settings.strength * p_delta * 10.0f);
			for (int z = 0; z < rect.size.y; z++) {
				for (int x = 0; x < w; x++) {
					const int i = z * w + x;
					if (!win.valid[i]) {
						continue;
					}
					const float f = weight_at(x, z);
					if (f <= 0.0f) {
						continue;
					}
					if (p_tool == TOOL_RAISE) {
						h[i] += sign * rate * f;
					} else if (p_tool == TOOL_FLATTEN) {
						h[i] = Math::lerp(h[i], p_settings.flatten_height, f * blend);
					} else if (p_tool == TOOL_NOISE) {
						const Vector2 m = Vector2(rect.position.x + x, rect.position.y + z) * sp / MAX(p_settings.noise_scale, 0.01f);
						const float n = fractal_noise(m.x, m.y, 0x71ad) * 2.0f - 1.0f;
						h[i] += sign * n * p_settings.noise_scale * 0.35f * p_settings.strength * p_delta * f;
					} else {
						const float step = MAX(p_settings.terrace_step, 0.01f);
						const float q = h[i] / step;
						const float fl = Math::floor(q);
						const float t = q - fl;
						const float edge = CLAMP((t - 0.3f) / 0.4f, 0.0f, 1.0f);
						const float target = (fl + edge * edge * (3.0f - 2.0f * edge)) * step;
						h[i] = Math::lerp(h[i], target, f * blend * 0.8f);
					}
				}
			}
			data->write_heights(rect, h);
		} break;

		case TOOL_SMOOTH: {
			HeightWindow win;
			win.read(data.ptr(), rect);
			const float *src = win.heights.ptr();
			Vector<float> out = win.heights;
			float *dst = out.ptrw();
			const float blend = MIN(1.0f, p_settings.strength * p_delta * 14.0f);
			for (int z = 1; z < rect.size.y - 1; z++) {
				for (int x = 1; x < w - 1; x++) {
					const int i = z * w + x;
					if (!win.valid[i]) {
						continue;
					}
					const float f = weight_at(x, z);
					if (f <= 0.0f) {
						continue;
					}
					float sum = 0.0;
					int count = 0;
					for (int dz = -1; dz <= 1; dz++) {
						for (int dx = -1; dx <= 1; dx++) {
							const int j = i + dz * w + dx;
							if (win.valid[j]) {
								sum += src[j];
								count++;
							}
						}
					}
					dst[i] = Math::lerp(src[i], sum / count, f * blend);
				}
			}
			data->write_heights(rect, dst);
		} break;

		case TOOL_ERODE: {
			HeightWindow win;
			win.read(data.ptr(), rect);
			erode(win, centre, radius_v, p_settings, sp, p_delta, p_seed);
			data->write_heights(rect, win.heights.ptr());
		} break;

		case TOOL_HOLES: {
			Vector<uint8_t> holes;
			holes.resize(w * rect.size.y);
			data->read_holes(rect, holes.ptrw());
			uint8_t *hw = holes.ptrw();
			bool changed = false;
			for (int z = 0; z < rect.size.y; z++) {
				for (int x = 0; x < w; x++) {
					// Holes are all or nothing, so the brush edge is its half
					// strength line.
					if (weight_at(x, z) >= 0.5f) {
						const uint8_t v = p_settings.invert ? 0 : 255;
						changed = changed || hw[z * w + x] != v;
						hw[z * w + x] = v;
					}
				}
			}
			if (changed) {
				data->write_holes(rect, hw);
			}
		} break;

		case TOOL_PAINT: {
			const int layer_count = data->get_layer_count();
			const int layer = p_settings.layer;
			if (layer < 0 || layer >= layer_count) {
				return;
			}
			// Where a layer is taken away something must replace it.
			const int fallback = layer == 0 ? 1 : 0;
			if (p_settings.invert && fallback >= layer_count) {
				return;
			}
			Vector<uint8_t> splat;
			splat.resize(w * rect.size.y * TerrainData::MAX_LAYERS);
			data->read_splat(rect, splat.ptrw());
			uint8_t *s = splat.ptrw();
			const float rate = MIN(1.0f, p_settings.strength * p_delta * 4.0f);
			for (int z = 0; z < rect.size.y; z++) {
				for (int x = 0; x < w; x++) {
					const float f = weight_at(x, z);
					if (f <= 0.0f) {
						continue;
					}
					uint8_t *px = s + (z * w + x) * TerrainData::MAX_LAYERS;
					float weights[TerrainData::MAX_LAYERS];
					for (int l = 0; l < TerrainData::MAX_LAYERS; l++) {
						weights[l] = px[l] / 255.0f;
					}
					const float before = weights[layer];
					const float after = CLAMP(before + sign * rate * f, 0.0f, 1.0f);
					if (after == before) {
						continue;
					}
					weights[layer] = after;
					float others = 0.0;
					for (int l = 0; l < TerrainData::MAX_LAYERS; l++) {
						others += l == layer ? 0.0f : weights[l];
					}
					const float rest = 1.0f - after;
					if (others > 1e-6f) {
						for (int l = 0; l < TerrainData::MAX_LAYERS; l++) {
							if (l != layer) {
								weights[l] *= rest / others;
							}
						}
					} else if (rest > 0.0f) {
						weights[fallback] = rest;
					}
					quantize_weights(weights, px);
				}
			}
			data->write_splat(rect, s);
		} break;

		case TOOL_COLOR:
		case TOOL_WETNESS: {
			Vector<Color> colors;
			colors.resize(w * rect.size.y);
			data->read_colors(rect, colors.ptrw());
			Color *c = colors.ptrw();
			const float rate = MIN(1.0f, p_settings.strength * p_delta * 4.0f);
			for (int z = 0; z < rect.size.y; z++) {
				for (int x = 0; x < w; x++) {
					const float f = weight_at(x, z);
					if (f <= 0.0f) {
						continue;
					}
					Color &px = c[z * w + x];
					if (p_tool == TOOL_COLOR) {
						// Inverted, the colour brush washes the tint back out.
						const Color target = p_settings.invert ? Color(1, 1, 1) : p_settings.color;
						px.r = Math::lerp(px.r, target.r, f * rate);
						px.g = Math::lerp(px.g, target.g, f * rate);
						px.b = Math::lerp(px.b, target.b, f * rate);
					} else {
						px.a = CLAMP(px.a + sign * rate * f, 0.0f, 1.0f);
					}
				}
			}
			data->write_colors(rect, c);
		} break;

		case TOOL_GRASS:
		case TOOL_OBJECTS: {
			apply_foliage(p_foliage, p_tool, Vector3(p_local.x, 0.0, p_local.y), p_settings, p_delta, p_seed);
		} break;

		case TOOL_REGIONS: {
			const Vector2i region = data->vertex_to_region(Vector2i((int)Math::floor(centre.x), (int)Math::floor(centre.y)));
			if (p_settings.invert) {
				data->remove_region(region);
			} else {
				data->add_region(region, p_settings.flatten_height);
			}
		} break;

		default:
			break;
	}
}

Rect2i TerrainBrush::get_ramp_rect(const Terrain3D *p_terrain, const Vector3 &p_from, const Vector3 &p_to, const Settings &p_settings) {
	const float sp = p_terrain->get_vertex_spacing();
	const Vector2 a = Vector2(p_from.x, p_from.z) / sp;
	const Vector2 b = Vector2(p_to.x, p_to.z) / sp;
	const float r = p_settings.radius / sp;
	const Vector2i lo = Vector2i((int)Math::floor(MIN(a.x, b.x) - r), (int)Math::floor(MIN(a.y, b.y) - r)) - Vector2i(1, 1);
	const Vector2i hi = Vector2i((int)Math::ceil(MAX(a.x, b.x) + r), (int)Math::ceil(MAX(a.y, b.y) + r)) + Vector2i(1, 1);
	return Rect2i(lo, hi - lo + Vector2i(1, 1));
}

void TerrainBrush::apply_ramp(Terrain3D *p_terrain, const Vector3 &p_from, const Vector3 &p_to, const Settings &p_settings) {
	// A straight, evenly sloped strip from where the stroke started to where it
	// ended, feathered out to the sides: roads, ramps, paths up a hill.
	const Ref<TerrainData> data = p_terrain ? p_terrain->get_data() : Ref<TerrainData>();
	if (data.is_null()) {
		return;
	}
	const float sp = p_terrain->get_vertex_spacing();
	const Rect2i rect = get_ramp_rect(p_terrain, p_from, p_to, p_settings);
	HeightWindow win;
	win.read(data.ptr(), rect);
	float *h = win.heights.ptrw();
	const Vector2 a(p_from.x, p_from.z);
	const Vector2 ab = Vector2(p_to.x, p_to.z) - a;
	const float len2 = MAX(ab.length_squared(), 1e-6f);
	const float amount = CLAMP(p_settings.strength * 2.0f, 0.0f, 1.0f);
	for (int z = 0; z < rect.size.y; z++) {
		for (int x = 0; x < rect.size.x; x++) {
			const int i = z * rect.size.x + x;
			if (!win.valid[i]) {
				continue;
			}
			const Vector2 p = Vector2(rect.position.x + x, rect.position.y + z) * sp;
			const float t = CLAMP((p - a).dot(ab) / len2, 0.0f, 1.0f);
			const float f = falloff(p.distance_to(a + ab * t), p_settings.radius, p_settings.hardness);
			if (f > 0.0f) {
				const float target = float(Math::lerp(p_from.y, p_to.y, real_t(t)));
				h[i] = Math::lerp(h[i], target, f * amount);
			}
		}
	}
	data->write_heights(rect, h);
}
