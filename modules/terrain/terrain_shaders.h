/**************************************************************************/
/*  terrain_shaders.h                                                     */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#pragma once

#include "core/string/ustring.h"

// Written in Godot's shading language rather than as an RD effect, so the
// terrain draws on Forward+, Mobile and Compatibility alike and every renderer
// feature (shadows, SDFGI, fog, decals) treats it as ordinary geometry.

inline constexpr const char *TERRAIN_SHADER_HEAD = R"GLSL(
shader_type spatial;
render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx;

// Layout of the terrain's region arrays.
uniform sampler2DArray heightmaps : filter_nearest, repeat_disable;
uniform sampler2DArray normalmaps : filter_nearest, repeat_disable;
uniform sampler2DArray splatmaps0 : filter_nearest, repeat_disable;
uniform sampler2DArray splatmaps1 : filter_nearest, repeat_disable;
uniform sampler2DArray splatmaps2 : filter_nearest, repeat_disable;
uniform sampler2DArray splatmaps3 : filter_nearest, repeat_disable;
uniform sampler2DArray colormaps : filter_nearest, repeat_disable;
uniform sampler2D region_map : filter_nearest, repeat_disable;
uniform ivec2 region_map_origin = ivec2(0);
uniform ivec2 region_map_size = ivec2(1);
uniform int region_size = 256;
uniform int chunk_size = 64;
uniform float vertex_spacing = 1.0;
uniform vec3 terrain_origin = vec3(0.0);
uniform int splat_map_count = 1;
uniform int layer_count = 0;

// Layers. The per layer uniforms are indexed by layer; the texture slices are
// separate because a layer without a texture takes no slice.
uniform sampler2DArray layer_albedo : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2DArray layer_normal : filter_linear_mipmap_anisotropic, repeat_enable;
uniform vec4 layer_tint[16]; // RGB tint, A roughness.
uniform vec4 layer_params[16]; // X 1 / tile size, Y normal strength, Z triplanar, W detiling.
uniform vec4 layer_textures[16]; // X albedo slice, Y normal slice, -1 for none.

// Shading.
uniform bool height_blending = true;
uniform float height_blend_sharpness = 0.25;
uniform bool auto_slope = false;
uniform int auto_slope_layer = 1;
uniform vec2 auto_slope_range = vec2(0.13, 0.25);
uniform float macro_variation = 0.2;
uniform float macro_variation_size = 64.0;
uniform float wetness_darkening = 0.55;

// Editor.
uniform int debug_view = 0;
uniform vec2 debug_height_range = vec2(0.0, 1.0);
uniform bool brush_enabled = false;
uniform vec3 brush_position = vec3(0.0);
uniform float brush_radius = 1.0;
uniform float brush_hardness = 0.5;
uniform vec4 brush_color : source_color = vec4(1.0);
uniform bool brush_segment = false;
uniform vec3 brush_segment_start = vec3(0.0);

// Deep enough to cover the worst crack a neighbouring level of detail can open,
// which the chunk's own height range bounds.
instance uniform float skirt_depth = 1.0;

int floor_div(int a, int b) {
	return a >= 0 ? a / b : -((-a + b - 1) / b);
}

ivec2 region_of(ivec2 v) {
	return ivec2(floor_div(v.x, region_size), floor_div(v.y, region_size));
}

int region_layer(ivec2 region) {
	ivec2 m = region - region_map_origin;
	if (m.x < 0 || m.y < 0 || m.x >= region_map_size.x || m.y >= region_map_size.y) {
		return -1;
	}
	return int(texelFetch(region_map, m, 0).r + 0.5) - 1;
}

// A vertex on the far edge of the last region has no region of its own and
// borrows from the one it closes off, the same as TerrainData does on the CPU.
bool resolve_vertex(ivec2 v, out ivec3 texel) {
	ivec2 vv = v;
	ivec2 r = region_of(vv);
	int layer = region_layer(r);
	if (layer < 0) {
		bool on_x = vv.x - r.x * region_size == 0;
		bool on_z = vv.y - r.y * region_size == 0;
		if (on_x) {
			ivec2 c = ivec2(vv.x - 1, vv.y);
			int l = region_layer(region_of(c));
			if (l >= 0) {
				vv = c;
				layer = l;
			}
		}
		if (layer < 0 && on_z) {
			ivec2 c = ivec2(vv.x, vv.y - 1);
			int l = region_layer(region_of(c));
			if (l >= 0) {
				vv = c;
				layer = l;
			}
		}
		if (layer < 0 && on_x && on_z) {
			ivec2 c = ivec2(vv.x - 1, vv.y - 1);
			int l = region_layer(region_of(c));
			if (l >= 0) {
				vv = c;
				layer = l;
			}
		}
		if (layer < 0) {
			texel = ivec3(0);
			return false;
		}
		r = region_of(vv);
	}
	texel = ivec3(vv - r * region_size, layer);
	return true;
}

bool region_exists_at(ivec2 v) {
	return region_layer(region_of(v)) >= 0;
}

vec3 fetch_normal(ivec3 texel) {
	vec2 n = texelFetch(normalmaps, texel, 0).rg;
	return vec3(n.x, sqrt(max(1.0 - dot(n, n), 0.0)), n.y);
}

void vertex() {
	vec3 world = (MODEL_MATRIX * vec4(VERTEX.x, 0.0, VERTEX.z, 1.0)).xyz;
	ivec2 v = ivec2(round((world.xz - terrain_origin.xz) / vertex_spacing));

	ivec3 texel;
	bool valid = resolve_vertex(v, texel);
	float h = valid ? texelFetch(heightmaps, texel, 0).r : 0.0;
	valid = valid && !isnan(h);

	if (VERTEX.y < -0.5) {
		// A skirt hangs below the chunk's edge to hide the crack between two
		// levels of detail. At the edge of the terrain there is no neighbour to
		// crack against, and a skirt there would read as a wall.
		ivec2 local = ivec2(round(VERTEX.xz / vertex_spacing));
		int ox = local.x == 0 ? -1 : (local.x == chunk_size ? 1 : 0);
		int oz = local.y == 0 ? -1 : (local.y == chunk_size ? 1 : 0);
		bool beyond = (ox == 0 || region_exists_at(v + ivec2(ox, 0))) && (oz == 0 || region_exists_at(v + ivec2(0, oz)));
		h -= beyond ? skirt_depth : 0.0;
	}

	vec3 n = valid ? fetch_normal(texel) : vec3(0.0, 1.0, 0.0);
	NORMAL = n;
	TANGENT = normalize(vec3(1.0, 0.0, 0.0) - n * n.x);
	BINORMAL = normalize(cross(n, TANGENT));
	VERTEX.y = h;

	if (!valid) {
		// A NaN corner makes the GPU drop every triangle touching it, which is
		// what a hole is. It also keeps the hole out of the shadow maps.
		VERTEX = vec3(uintBitsToFloat(0x7fc00000u));
	}
}

)GLSL";

// Noise, detiling and normal mapping, shared by the heightmap and voxel
// shaders.
inline constexpr const char *TERRAIN_SHADER_HELPERS = R"GLSL(
float hash12(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float value_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), u.x), mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), u.x), u.y);
}

vec3 hue_color(float h) {
	vec3 k = clamp(abs(mod(h * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
	return mix(vec3(1.0), k, 0.75);
}

// Detiling moves every tile by one of eight random offsets, chosen by a slow
// noise and cross-faded, which breaks up repetition without rotating the
// normal map out from under the lighting.
vec2 detile_offset(vec2 uv, float amount, out vec2 other, out float blend) {
	float l = value_noise(uv * 0.21) * 8.0;
	float i = floor(l);
	blend = smoothstep(0.2, 0.8, l - i);
	other = sin(vec2(3.0, 7.0) * (i + 1.0)) * amount;
	return sin(vec2(3.0, 7.0) * i) * amount;
}

vec4 sample_slice(sampler2DArray tex, vec2 uv, int slice, vec2 gx, vec2 gy, float detile) {
	if (detile <= 0.0) {
		return textureGrad(tex, vec3(uv, float(slice)), gx, gy);
	}
	vec2 other;
	float blend;
	vec2 offset = detile_offset(uv, detile, other, blend);
	vec4 a = textureGrad(tex, vec3(uv + offset, float(slice)), gx, gy);
	vec4 b = textureGrad(tex, vec3(uv + other, float(slice)), gx, gy);
	return mix(a, b, blend);
}

vec3 unpack_normal(vec4 s, float strength) {
	vec2 xy = (s.xy * 2.0 - 1.0) * strength;
	return vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
}

// Perturbs a world space normal by a tangent space one, for a texture laid
// down along the given projection axes.
vec3 apply_normal(vec3 n, vec3 t_axis, vec3 b_axis, vec3 nt) {
	vec3 t = normalize(t_axis - n * dot(n, t_axis));
	vec3 b = normalize(b_axis - n * dot(n, b_axis));
	return normalize(t * nt.x + b * nt.y + n * nt.z);
}

)GLSL";

inline constexpr const char *TERRAIN_SHADER_TAIL = R"GLSL(
void sample_layer(int i, vec3 world, vec3 n, vec3 dpx, vec3 dpy, out vec4 albedo, out vec3 normal_ws) {
	vec4 p = layer_params[i];
	vec4 slices = layer_textures[i];
	int ai = int(slices.x);
	int ni = int(slices.y);
	float s = p.x;

	if (p.z > 0.5) {
		// Triplanar: three projections weighted by the normal, for cliffs that
		// a top down projection would smear.
		vec3 w = pow(abs(n), vec3(4.0));
		w /= w.x + w.y + w.z;
		vec4 ax = ai >= 0 ? sample_slice(layer_albedo, world.zy * s, ai, dpx.zy * s, dpy.zy * s, p.w) : vec4(1.0);
		vec4 ay = ai >= 0 ? sample_slice(layer_albedo, world.xz * s, ai, dpx.xz * s, dpy.xz * s, p.w) : vec4(1.0);
		vec4 az = ai >= 0 ? sample_slice(layer_albedo, world.xy * s, ai, dpx.xy * s, dpy.xy * s, p.w) : vec4(1.0);
		albedo = ax * w.x + ay * w.y + az * w.z;
		if (ni >= 0) {
			vec3 nx = apply_normal(n, vec3(0.0, 0.0, 1.0), vec3(0.0, -1.0, 0.0), unpack_normal(sample_slice(layer_normal, world.zy * s, ni, dpx.zy * s, dpy.zy * s, p.w), p.y));
			vec3 ny = apply_normal(n, vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, -1.0), unpack_normal(sample_slice(layer_normal, world.xz * s, ni, dpx.xz * s, dpy.xz * s, p.w), p.y));
			vec3 nz = apply_normal(n, vec3(1.0, 0.0, 0.0), vec3(0.0, -1.0, 0.0), unpack_normal(sample_slice(layer_normal, world.xy * s, ni, dpx.xy * s, dpy.xy * s, p.w), p.y));
			normal_ws = normalize(nx * w.x + ny * w.y + nz * w.z);
		} else {
			normal_ws = n;
		}
	} else {
		vec2 uv = world.xz * s;
		vec2 gx = dpx.xz * s;
		vec2 gy = dpy.xz * s;
		albedo = ai >= 0 ? sample_slice(layer_albedo, uv, ai, gx, gy, p.w) : vec4(1.0);
		normal_ws = ni >= 0 ? apply_normal(n, vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, -1.0), unpack_normal(sample_slice(layer_normal, uv, ni, gx, gy, p.w), p.y)) : n;
	}
	albedo.rgb *= layer_tint[i].rgb;
}

void fragment() {
	vec3 world = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
	// Derivatives are taken here, in uniform control flow, because the layer
	// sampling below happens inside branches that differ pixel to pixel.
	vec3 dpx = dFdx(world);
	vec3 dpy = dFdy(world);

	vec2 grid = (world.xz - terrain_origin.xz) / vertex_spacing;
	vec2 base = floor(grid);
	vec2 f = grid - base;
	ivec2 b = ivec2(base);
	ivec3 t00;
	ivec3 t10;
	ivec3 t01;
	ivec3 t11;
	bool o00 = resolve_vertex(b, t00);
	bool o10 = resolve_vertex(b + ivec2(1, 0), t10);
	bool o01 = resolve_vertex(b + ivec2(0, 1), t01);
	bool o11 = resolve_vertex(b + ivec2(1, 1), t11);
	if (!o00) {
		t00 = o10 ? t10 : (o01 ? t01 : t11);
	}
	if (!o10) {
		t10 = t00;
	}
	if (!o01) {
		t01 = t00;
	}
	if (!o11) {
		t11 = t00;
	}
	vec4 k = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);

	vec3 geo_n = normalize(fetch_normal(t00) * k.x + fetch_normal(t10) * k.y + fetch_normal(t01) * k.z + fetch_normal(t11) * k.w);

	float w[16];
	for (int i = 0; i < 16; i++) {
		w[i] = 0.0;
	}
	vec4 s0 = texelFetch(splatmaps0, t00, 0) * k.x + texelFetch(splatmaps0, t10, 0) * k.y + texelFetch(splatmaps0, t01, 0) * k.z + texelFetch(splatmaps0, t11, 0) * k.w;
	w[0] = s0.r;
	w[1] = s0.g;
	w[2] = s0.b;
	w[3] = s0.a;
	if (splat_map_count > 1) {
		vec4 s1 = texelFetch(splatmaps1, t00, 0) * k.x + texelFetch(splatmaps1, t10, 0) * k.y + texelFetch(splatmaps1, t01, 0) * k.z + texelFetch(splatmaps1, t11, 0) * k.w;
		w[4] = s1.r;
		w[5] = s1.g;
		w[6] = s1.b;
		w[7] = s1.a;
	}
	if (splat_map_count > 2) {
		vec4 s2 = texelFetch(splatmaps2, t00, 0) * k.x + texelFetch(splatmaps2, t10, 0) * k.y + texelFetch(splatmaps2, t01, 0) * k.z + texelFetch(splatmaps2, t11, 0) * k.w;
		w[8] = s2.r;
		w[9] = s2.g;
		w[10] = s2.b;
		w[11] = s2.a;
	}
	if (splat_map_count > 3) {
		vec4 s3 = texelFetch(splatmaps3, t00, 0) * k.x + texelFetch(splatmaps3, t10, 0) * k.y + texelFetch(splatmaps3, t01, 0) * k.z + texelFetch(splatmaps3, t11, 0) * k.w;
		w[12] = s3.r;
		w[13] = s3.g;
		w[14] = s3.b;
		w[15] = s3.a;
	}

	if (auto_slope && auto_slope_layer < layer_count) {
		float a = smoothstep(auto_slope_range.x, auto_slope_range.y, 1.0 - geo_n.y);
		for (int i = 0; i < 16; i++) {
			w[i] *= 1.0 - a;
		}
		w[auto_slope_layer] += a;
	}

	// Only the four strongest layers are sampled. More than four overlapping
	// meaningfully in one spot is rare, and it keeps the cost of a pixel flat
	// however many layers the terrain has.
	int ids[4] = int[4](0, 0, 0, 0);
	float ws[4] = float[4](0.0, 0.0, 0.0, 0.0);
	for (int i = 0; i < 16; i++) {
		if (i >= layer_count) {
			break;
		}
		float wi = w[i];
		if (wi > ws[3]) {
			int j = 3;
			while (j > 0 && wi > ws[j - 1]) {
				ws[j] = ws[j - 1];
				ids[j] = ids[j - 1];
				j--;
			}
			ws[j] = wi;
			ids[j] = i;
		}
	}

	vec3 albedo = vec3(0.42, 0.41, 0.38);
	float roughness = 0.9;
	vec3 n_ws = geo_n;
	vec4 blend = vec4(0.0);

	if (layer_count > 0) {
		vec4 alb[4];
		vec3 nrm[4];
		for (int j = 0; j < 4; j++) {
			alb[j] = vec4(0.0);
			nrm[j] = geo_n;
			if (ws[j] > 0.002) {
				sample_layer(ids[j], world, geo_n, dpx, dpy, alb[j], nrm[j]);
			}
		}

		blend = vec4(ws[0], ws[1], ws[2], ws[3]);
		if (height_blending) {
			// The layer whose height, lifted by its weight, stands tallest wins,
			// and everything within the sharpness band of it is mixed in. Gravel
			// then fills the gaps between stones instead of fading over them.
			vec4 hw = vec4(alb[0].a, alb[1].a, alb[2].a, alb[3].a) + blend;
			hw = mix(vec4(-1.0), hw, step(vec4(0.002), blend));
			float top = max(max(hw.x, hw.y), max(hw.z, hw.w)) - height_blend_sharpness;
			blend = max(hw - vec4(top), vec4(0.0));
		}
		blend /= max(blend.x + blend.y + blend.z + blend.w, 1e-5);

		albedo = alb[0].rgb * blend.x + alb[1].rgb * blend.y + alb[2].rgb * blend.z + alb[3].rgb * blend.w;
		roughness = layer_tint[ids[0]].a * blend.x + layer_tint[ids[1]].a * blend.y + layer_tint[ids[2]].a * blend.z + layer_tint[ids[3]].a * blend.w;
		n_ws = normalize(nrm[0] * blend.x + nrm[1] * blend.y + nrm[2] * blend.z + nrm[3] * blend.w);
	}

	float mv = value_noise(world.xz / macro_variation_size) * 0.65 + value_noise(world.xz / (macro_variation_size * 0.27)) * 0.35;
	albedo *= mix(1.0, mix(0.8, 1.16, mv), macro_variation);

	vec4 cm = texelFetch(colormaps, t00, 0) * k.x + texelFetch(colormaps, t10, 0) * k.y + texelFetch(colormaps, t01, 0) * k.z + texelFetch(colormaps, t11, 0) * k.w;
	albedo *= cm.rgb;
	float wet = cm.a;
	// Water darkens what it soaks, turns it glossy, and fills in the bumps.
	albedo *= mix(1.0, 1.0 - wetness_darkening, wet);
	roughness = mix(roughness, 0.06, wet);
	n_ws = normalize(mix(n_ws, geo_n, wet * 0.85));

	vec3 emission = vec3(0.0);
	float specular = 0.5;
	if (debug_view > 0 && debug_view < 6) {
		// Diagnostic views are flat colour: no gloss from wetness, no highlights.
		roughness = 1.0;
		specular = 0.0;
	}
	if (debug_view == 1) {
		float t = (world.y - terrain_origin.y - debug_height_range.x) / max(debug_height_range.y - debug_height_range.x, 0.001);
		albedo = vec3(clamp(t, 0.0, 1.0));
		n_ws = geo_n;
	} else if (debug_view == 2) {
		albedo = geo_n * 0.5 + 0.5;
		n_ws = vec3(0.0, 1.0, 0.0);
	} else if (debug_view == 3) {
		albedo = hue_color(float(ids[0]) * 0.618) * blend.x + hue_color(float(ids[1]) * 0.618) * blend.y + hue_color(float(ids[2]) * 0.618) * blend.z + hue_color(float(ids[3]) * 0.618) * blend.w;
		if (layer_count == 0) {
			albedo = hue_color(0.0);
		}
		n_ws = geo_n;
	} else if (debug_view == 4) {
		albedo = cm.rgb;
		n_ws = geo_n;
	} else if (debug_view == 5) {
		albedo = mix(vec3(0.85, 0.78, 0.62), vec3(0.08, 0.3, 0.95), wet);
		n_ws = geo_n;
	} else if (debug_view == 6) {
		vec2 cell = abs(fract(grid - 0.5) - 0.5) / max(fwidth(grid), vec2(1e-5));
		float line = 1.0 - min(min(cell.x, cell.y), 1.0);
		vec2 rgrid = grid / float(region_size);
		vec2 rcell = abs(fract(rgrid - 0.5) - 0.5) / max(fwidth(rgrid), vec2(1e-5));
		float rline = 1.0 - min(min(rcell.x, rcell.y) * 0.5, 1.0);
		albedo = mix(albedo, vec3(1.0), line * 0.35);
		emission += vec3(1.0, 0.55, 0.1) * rline;
	}

	if (brush_enabled) {
		float d;
		if (brush_segment) {
			vec2 a = brush_segment_start.xz;
			vec2 ab = brush_position.xz - a;
			float t = clamp(dot(world.xz - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
			d = distance(world.xz, a + ab * t);
		} else {
			d = distance(world.xz, brush_position.xz);
		}
		float px = max(fwidth(d), 1e-4);
		float ring = 1.0 - smoothstep(px * 0.5, px * 2.0, abs(d - brush_radius));
		float inner_radius = brush_radius * brush_hardness;
		float inner = brush_hardness < 0.98 ? (1.0 - smoothstep(px * 0.5, px * 2.0, abs(d - inner_radius))) * 0.5 : 0.0;
		float fill = (1.0 - smoothstep(inner_radius, brush_radius, d)) * step(d, brush_radius) * 0.08;
		// Emissive, so the brush stays visible in a scene lit for a horror game.
		emission += brush_color.rgb * (ring + inner + fill);
		albedo = mix(albedo, brush_color.rgb, ring * 0.6);
	}

	ALBEDO = albedo;
	ROUGHNESS = roughness;
	SPECULAR = specular;
	NORMAL = normalize((VIEW_MATRIX * vec4(n_ws, 0.0)).xyz);
	EMISSION = emission;
}
)GLSL";

inline constexpr const char *GRASS_SHADER_CODE = R"GLSL(
shader_type spatial;
render_mode cull_disabled, diffuse_burley, specular_schlick_ggx, world_vertex_coords;

uniform vec4 color_base : source_color = vec4(0.14, 0.22, 0.06, 1.0);
uniform vec4 color_tip : source_color = vec4(0.52, 0.6, 0.25, 1.0);
uniform float wind_strength = 0.3;
uniform float wind_speed = 1.0;
uniform vec2 wind_direction = vec2(1.0, 0.35);

// Past this the blades shrink into the ground rather than popping out.
instance uniform float fade_distance = 60.0;

varying float v_height;
varying vec3 v_tint;
varying vec3 v_normal;

void vertex() {
	// The blade's UV runs from 1 at its root to 0 at its tip.
	float h = clamp(1.0 - UV.y, 0.0, 1.0);
	vec3 root = MODEL_MATRIX[3].xyz;

	vec2 dir = normalize(wind_direction);
	float t = TIME * wind_speed;
	float gust = sin(dot(root.xz, dir) * 0.35 - t * 1.7) * 0.5 + 0.5;
	float flutter = sin(dot(root.xz, vec2(1.3, 0.7)) * 2.1 + t * 4.3) * 0.25;
	float sway = (gust * 0.8 + flutter) * wind_strength * h * h;
	VERTEX.xz += dir * sway;
	VERTEX.y -= abs(sway) * 0.35 * h;

	float dist = distance(root, CAMERA_POSITION_WORLD);
	float keep = 1.0 - smoothstep(fade_distance * 0.7, fade_distance, dist);
	VERTEX = mix(root, VERTEX, keep);

	// Lit mostly like the ground the tuft grows from, whose normal comes in
	// with the instance, with a little of the blade's own facing for variety.
	vec3 ground = INSTANCE_CUSTOM.xyz;
	if (dot(ground, ground) < 0.25) {
		ground = vec3(0.0, 1.0, 0.0);
	}
	v_normal = normalize(mix(NORMAL, normalize(ground), 0.8));
	NORMAL = v_normal;
	v_height = h;
	v_tint = COLOR.rgb;
}

void fragment() {
	// Set explicitly rather than left to the interpolated normal: blades are
	// double sided, and for every blade seen from behind the engine flips its
	// normal, which here would point into the ground and turn half the field
	// black in full sun.
	NORMAL = normalize((VIEW_MATRIX * vec4(v_normal, 0.0)).xyz);
	vec3 c = mix(color_base.rgb, color_tip.rgb, v_height) * v_tint;
	ALBEDO = c;
	ROUGHNESS = 0.85;
	SPECULAR = 0.25;
	// A little darker toward the roots, where the tuft shades itself.
	AO = mix(0.6, 1.0, v_height);
	AO_LIGHT_AFFECT = 0.3;
	BACKLIGHT = c * 0.3;
}
)GLSL";

inline constexpr const char *VOXEL_SHADER_HEAD = R"GLSL(
shader_type spatial;
render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx;

// Layers, laid out as for the heightmap terrain.
uniform int layer_count = 0;
uniform sampler2DArray layer_albedo : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2DArray layer_normal : filter_linear_mipmap_anisotropic, repeat_enable;
uniform vec4 layer_tint[16]; // RGB tint, A roughness.
uniform vec4 layer_params[16]; // X 1 / tile size, Y normal strength, Z triplanar, W detiling.
uniform vec4 layer_textures[16]; // X albedo slice, Y normal slice, -1 for none.

// Shading.
uniform bool height_blending = true;
uniform float height_blend_sharpness = 0.25;
uniform bool auto_slope = false;
uniform int auto_slope_layer = 1;
uniform vec2 auto_slope_range = vec2(0.13, 0.25);
uniform float macro_variation = 0.2;
uniform float macro_variation_size = 64.0;

// Editor.
uniform int debug_view = 0;
uniform bool brush_enabled = false;
uniform vec3 brush_position = vec3(0.0);
uniform float brush_radius = 1.0;
uniform float brush_hardness = 0.5;
uniform vec4 brush_color : source_color = vec4(1.0);

instance uniform float lod_level = 0.0;

// The mesher gives every vertex a weight for each of the sixteen layers, four
// to a custom channel; interpolated across a triangle they blend one voxel's
// layer into the next.
varying vec4 weights0;
varying vec4 weights1;
varying vec4 weights2;
varying vec4 weights3;
varying vec3 world_position;
varying vec3 world_normal;

void vertex() {
	weights0 = CUSTOM0;
	weights1 = CUSTOM1;
	weights2 = CUSTOM2;
	weights3 = CUSTOM3;
	world_position = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	world_normal = normalize(mat3(MODEL_MATRIX) * NORMAL);
}
)GLSL";

inline constexpr const char *VOXEL_SHADER_TAIL = R"GLSL(
// Voxel ground faces every way, from floors to cave ceilings, so every layer
// is projected along all three axes and blended by the normal. A projection
// that hardly shows is skipped.
void sample_layer(int i, vec3 world, vec3 n, vec3 dpx, vec3 dpy, out vec4 albedo, out vec3 normal_ws) {
	vec4 p = layer_params[i];
	vec4 slices = layer_textures[i];
	int ai = int(slices.x);
	int ni = int(slices.y);
	float s = p.x;
	vec3 w = pow(abs(n), vec3(4.0));
	w /= w.x + w.y + w.z;

	albedo = vec4(0.0);
	vec3 nsum = vec3(0.0);
	float kept = 0.0;
	if (w.x > 0.02) {
		albedo += (ai >= 0 ? sample_slice(layer_albedo, world.zy * s, ai, dpx.zy * s, dpy.zy * s, p.w) : vec4(1.0)) * w.x;
		nsum += (ni >= 0 ? apply_normal(n, vec3(0.0, 0.0, 1.0), vec3(0.0, -1.0, 0.0), unpack_normal(sample_slice(layer_normal, world.zy * s, ni, dpx.zy * s, dpy.zy * s, p.w), p.y)) : n) * w.x;
		kept += w.x;
	}
	if (w.y > 0.02) {
		albedo += (ai >= 0 ? sample_slice(layer_albedo, world.xz * s, ai, dpx.xz * s, dpy.xz * s, p.w) : vec4(1.0)) * w.y;
		nsum += (ni >= 0 ? apply_normal(n, vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, -1.0), unpack_normal(sample_slice(layer_normal, world.xz * s, ni, dpx.xz * s, dpy.xz * s, p.w), p.y)) : n) * w.y;
		kept += w.y;
	}
	if (w.z > 0.02) {
		albedo += (ai >= 0 ? sample_slice(layer_albedo, world.xy * s, ai, dpx.xy * s, dpy.xy * s, p.w) : vec4(1.0)) * w.z;
		nsum += (ni >= 0 ? apply_normal(n, vec3(1.0, 0.0, 0.0), vec3(0.0, -1.0, 0.0), unpack_normal(sample_slice(layer_normal, world.xy * s, ni, dpx.xy * s, dpy.xy * s, p.w), p.y)) : n) * w.z;
		kept += w.z;
	}
	albedo /= max(kept, 1e-5);
	normal_ws = normalize(nsum);
	albedo.rgb *= layer_tint[i].rgb;
}

void fragment() {
	vec3 world = world_position;
	// Derivatives are taken here, in uniform control flow, because the layer
	// sampling below happens inside branches that differ pixel to pixel.
	vec3 dpx = dFdx(world);
	vec3 dpy = dFdy(world);
	vec3 geo_n = normalize(world_normal);

	float w[16];
	w[0] = weights0.x;
	w[1] = weights0.y;
	w[2] = weights0.z;
	w[3] = weights0.w;
	w[4] = weights1.x;
	w[5] = weights1.y;
	w[6] = weights1.z;
	w[7] = weights1.w;
	w[8] = weights2.x;
	w[9] = weights2.y;
	w[10] = weights2.z;
	w[11] = weights2.w;
	w[12] = weights3.x;
	w[13] = weights3.y;
	w[14] = weights3.z;
	w[15] = weights3.w;

	if (auto_slope && auto_slope_layer < layer_count) {
		float a = smoothstep(auto_slope_range.x, auto_slope_range.y, 1.0 - geo_n.y);
		for (int i = 0; i < 16; i++) {
			w[i] *= 1.0 - a;
		}
		w[auto_slope_layer] += a;
	}

	// Only the four strongest layers are sampled.
	int ids[4] = int[4](0, 0, 0, 0);
	float ws[4] = float[4](0.0, 0.0, 0.0, 0.0);
	for (int i = 0; i < 16; i++) {
		if (i >= layer_count) {
			break;
		}
		float wi = w[i];
		if (wi > ws[3]) {
			int j = 3;
			while (j > 0 && wi > ws[j - 1]) {
				ws[j] = ws[j - 1];
				ids[j] = ids[j - 1];
				j--;
			}
			ws[j] = wi;
			ids[j] = i;
		}
	}

	vec3 albedo = vec3(0.42, 0.41, 0.38);
	float roughness = 0.9;
	vec3 n_ws = geo_n;
	vec4 blend = vec4(0.0);

	if (layer_count > 0) {
		vec4 alb[4];
		vec3 nrm[4];
		for (int j = 0; j < 4; j++) {
			alb[j] = vec4(0.0);
			nrm[j] = geo_n;
			if (ws[j] > 0.002) {
				sample_layer(ids[j], world, geo_n, dpx, dpy, alb[j], nrm[j]);
			}
		}

		blend = vec4(ws[0], ws[1], ws[2], ws[3]);
		if (height_blending) {
			vec4 hw = vec4(alb[0].a, alb[1].a, alb[2].a, alb[3].a) + blend;
			hw = mix(vec4(-1.0), hw, step(vec4(0.002), blend));
			float top = max(max(hw.x, hw.y), max(hw.z, hw.w)) - height_blend_sharpness;
			blend = max(hw - vec4(top), vec4(0.0));
		}
		blend /= max(blend.x + blend.y + blend.z + blend.w, 1e-5);

		albedo = alb[0].rgb * blend.x + alb[1].rgb * blend.y + alb[2].rgb * blend.z + alb[3].rgb * blend.w;
		roughness = layer_tint[ids[0]].a * blend.x + layer_tint[ids[1]].a * blend.y + layer_tint[ids[2]].a * blend.z + layer_tint[ids[3]].a * blend.w;
		n_ws = normalize(nrm[0] * blend.x + nrm[1] * blend.y + nrm[2] * blend.z + nrm[3] * blend.w);
	}

	float mv = value_noise(world.xz / macro_variation_size + world.y * 0.013) * 0.65 + value_noise(world.xz / (macro_variation_size * 0.27)) * 0.35;
	albedo *= mix(1.0, mix(0.8, 1.16, mv), macro_variation);

	vec3 emission = vec3(0.0);
	float specular = 0.5;
	if (debug_view > 0) {
		// Diagnostic views are flat colour.
		roughness = 1.0;
		specular = 0.0;
		n_ws = geo_n;
	}
	if (debug_view == 1) {
		albedo = geo_n * 0.5 + 0.5;
		n_ws = vec3(0.0, 1.0, 0.0);
	} else if (debug_view == 2) {
		albedo = hue_color(float(ids[0]) * 0.618) * blend.x + hue_color(float(ids[1]) * 0.618) * blend.y + hue_color(float(ids[2]) * 0.618) * blend.z + hue_color(float(ids[3]) * 0.618) * blend.w;
		if (layer_count == 0) {
			albedo = hue_color(0.0);
		}
	} else if (debug_view == 3) {
		albedo = hue_color(lod_level * 0.17 + 0.55);
	}

	if (brush_enabled) {
		// Where the brush's sphere meets the ground, drawn as a ring, with a
		// fainter one at the edge of full strength.
		float d = distance(world, brush_position);
		float px = max(fwidth(d), 1e-4);
		float ring = 1.0 - smoothstep(px * 0.5, px * 2.0, abs(d - brush_radius));
		float inner_radius = brush_radius * brush_hardness;
		float inner = brush_hardness < 0.98 ? (1.0 - smoothstep(px * 0.5, px * 2.0, abs(d - inner_radius))) * 0.5 : 0.0;
		float fill = (1.0 - smoothstep(inner_radius, brush_radius, d)) * step(d, brush_radius) * 0.08;
		// Emissive, so the brush stays visible in a scene lit for a horror game.
		emission += brush_color.rgb * (ring + inner + fill);
		albedo = mix(albedo, brush_color.rgb, ring * 0.6);
	}

	ALBEDO = albedo;
	ROUGHNESS = roughness;
	SPECULAR = specular;
	NORMAL = normalize((VIEW_MATRIX * vec4(n_ws, 0.0)).xyz);
	EMISSION = emission;
}
)GLSL";

inline String terrain_shader_code() {
	return String(TERRAIN_SHADER_HEAD) + TERRAIN_SHADER_HELPERS + TERRAIN_SHADER_TAIL;
}

inline String voxel_shader_code() {
	return String(VOXEL_SHADER_HEAD) + TERRAIN_SHADER_HELPERS + VOXEL_SHADER_TAIL;
}
