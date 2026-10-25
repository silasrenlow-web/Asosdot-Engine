#[compute]

#version 450

#VERSION_DEFINES

// Realtime GI: lighting that moves with the lights over transport precomputed
// at bake time. Static surfaces are cut into patches of about half a metre,
// laid out in a lightmap atlas. The bake recorded, for every patch, which
// clusters of patches its hemisphere sees and how much of the sky. Each frame
// these passes light the patches directly, bounce that light once through the
// recorded transport, and write the result into the atlas as a lightmap, so
// repeated frames converge on every bounce while the lights keep moving.

#if defined(MODE_OUTPUT)
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#else
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
#endif

#include "../oct_inc.glsl"

struct Patch {
	vec4 position_radius; // xyz in the GI's space, w = radius of the patch's footprint
	vec4 normal;
	vec4 albedo;
	vec4 emission;
};

layout(set = 0, binding = 1, std430) restrict readonly buffer Patches {
	Patch data[];
}
patches;

// Nine spherical harmonics per patch: the sky the patch's hemisphere sees,
// weighted by the cosine, so its sky light is one dot product with the sky's
// own coefficients.
layout(set = 0, binding = 2, std430) restrict readonly buffer SkyVisibility {
	float data[];
}
sky_visibility;

// Patches are stored cluster by cluster, so a cluster is a range of them.
layout(set = 0, binding = 3, std430) restrict readonly buffer ClusterOffsets {
	uint data[];
}
cluster_offsets;

// The patch each atlas texel shows, or -1. Texels around a chart's edge show
// the nearest patch, so filtering never reads black.
layout(set = 0, binding = 4, std430) restrict readonly buffer TexelPatches {
	int data[];
}
texel_patches;

layout(set = 0, binding = 5, std430) restrict readonly buffer TransportOffsets {
	uint data[];
}
transport_offsets;

// Cluster index in the top 22 bits, the number of the patch's rays that
// landed on it in the low 10.
layout(set = 0, binding = 6, std430) restrict readonly buffer TransportEntries {
	uint data[];
}
transport_entries;

// Binary BVH, two uvec4 per node: min.xyz (float bits) and the right child or
// first triangle, then max.xyz and the triangle count (0 for an inner node,
// whose left child follows it).
layout(set = 0, binding = 7, std430) restrict readonly buffer BVH {
	uvec4 data[];
}
bvh;

layout(set = 0, binding = 8, std430) restrict readonly buffer Triangles {
	vec4 data[];
}
triangles;

#define LIGHT_TYPE_DIRECTIONAL 0
#define LIGHT_TYPE_OMNI 1
#define LIGHT_TYPE_SPOT 2

#define LIGHT_FLAG_SHADOW 1
#define LIGHT_FLAG_STATIC 2

struct Light {
	vec3 position;
	uint type;
	vec3 direction; // the way the light shines
	float inv_range;
	vec3 color; // linear colour times energy, in the units diffuse shading uses
	float attenuation;
	float cos_spot_angle;
	float inv_spot_attenuation;
	float indirect_energy;
	uint flags;
};

layout(set = 0, binding = 9, std430) restrict readonly buffer Lights {
	Light data[];
}
lights;

// Two per patch: direct light for bouncing (scaled by each light's indirect
// energy) and direct light from static lights for the lightmap itself.
layout(set = 0, binding = 10, std430) restrict buffer Direct {
	vec4 data[];
}
direct;

layout(set = 0, binding = 11, std430) restrict buffer Radiosity {
	vec4 data[];
}
radiosity;

layout(set = 0, binding = 12, std430) restrict buffer ClusterRadiance {
	vec4 data[];
}
cluster_radiance;

layout(set = 0, binding = 13, std430) restrict buffer Indirect {
	vec4 data[];
}
indirect;

layout(set = 0, binding = 14, std430) restrict buffer SkySH {
	vec4 data[];
}
sky_sh;

layout(set = 0, binding = 15, std430) restrict readonly buffer ProbeRays {
	int data[];
}
probe_rays;

layout(set = 0, binding = 16, std430) restrict writeonly buffer ProbeSH {
	vec4 data[];
}
probe_sh;

layout(rgba16f, set = 0, binding = 17) uniform restrict writeonly image2DArray output_light;

#if defined(MODE_SKY_SH)
layout(set = 1, binding = 0) uniform texture2D sky_radiance;
layout(set = 1, binding = 1) uniform sampler linear_sampler_mipmaps;
#elif defined(MODE_SKY_SH_ARRAY)
layout(set = 1, binding = 0) uniform texture2DArray sky_radiance;
layout(set = 1, binding = 1) uniform sampler linear_sampler_mipmaps;
#endif

#if defined(MODE_SKY_SH) || defined(MODE_SKY_SH_ARRAY)
shared vec3 partial[64][9];
#endif

#if defined(MODE_DIRECT)
// Tiled light culling: a workgroup's 64 patches are neighbours (they're
// stored chart by chart), so the group first finds the lights that can reach
// its bounds, and each patch then loops over those alone.
#define MAX_GROUP_LIGHTS 256
shared vec3 group_min[64];
shared vec3 group_max[64];
shared uint group_lights[MAX_GROUP_LIGHTS];
shared uint group_light_count;
#endif

#define SKY_MODE_COLOR 0
#define SKY_MODE_TEXTURE 1

layout(push_constant, std430) uniform Params {
	uint patch_count;
	uint cluster_count;
	uint light_count;
	uint atlas_width;

	uint transport_rays;
	uint shadow_samples;
	uint probe_count;
	uint probe_rays;

	float indirect_energy;
	float environment_energy;
	float albedo_boost;
	float emission_energy;

	vec3 sky_color; // the flat part of the sky light, or all of it in colour mode
	uint sky_mode;

	vec4 sky_orientation; // quaternion taking world directions into the sky's

	float sky_lod;
	float sky_mix; // how much of the light comes from the sky texture
	vec2 sky_border;

	uint atlas_height;
	uint direct_stride; // the direct pass updates every stride-th patch,
	uint direct_offset; // starting here, so large scenes spread it over frames
	uint pad0;
}
params;

#define M_PI 3.14159265359
#define GOLDEN_ANGLE 2.39996322972865332

// The same spherical Fibonacci set the bake used for the probes' rays.
vec3 fibonacci_direction(uint p_index, uint p_count) {
	float z = 1.0 - (2.0 * float(p_index) + 1.0) / float(p_count);
	float r = sqrt(max(0.0, 1.0 - z * z));
	float phi = GOLDEN_ANGLE * float(p_index);
	return vec3(cos(phi) * r, sin(phi) * r, z);
}

void sh_basis(vec3 d, out float c[9]) {
	c[0] = 0.282095;
	c[1] = 0.488603 * d.y;
	c[2] = 0.488603 * d.z;
	c[3] = 0.488603 * d.x;
	c[4] = 1.092548 * d.x * d.y;
	c[5] = 1.092548 * d.y * d.z;
	c[6] = 0.315392 * (3.0 * d.z * d.z - 1.0);
	c[7] = 1.092548 * d.x * d.z;
	c[8] = 0.546274 * (d.x * d.x - d.y * d.y);
}

vec3 sky_light(vec3 p_dir) {
	float c[9];
	sh_basis(p_dir, c);
	vec3 light = vec3(0.0);
	for (uint i = 0; i < 9; i++) {
		light += sky_sh.data[i].rgb * c[i];
	}
	return max(light, vec3(0.0));
}

#if defined(MODE_DIRECT)

bool ray_box(vec3 p_from, vec3 p_inv_dir, float p_max, vec3 p_min, vec3 p_max_bounds) {
	vec3 t0 = (p_min - p_from) * p_inv_dir;
	vec3 t1 = (p_max_bounds - p_from) * p_inv_dir;
	vec3 tmin = min(t0, t1);
	vec3 tmax = max(t0, t1);
	float enter = max(max(tmin.x, tmin.y), max(tmin.z, 0.0));
	float exit = min(min(tmax.x, tmax.y), min(tmax.z, p_max));
	return enter <= exit;
}

bool ray_triangle(vec3 p_from, vec3 p_dir, float p_max, vec3 p0, vec3 p1, vec3 p2) {
	vec3 e1 = p1 - p0;
	vec3 e2 = p2 - p0;
	vec3 pv = cross(p_dir, e2);
	float det = dot(e1, pv);
	if (abs(det) < 1e-9) {
		return false;
	}
	float inv_det = 1.0 / det;
	vec3 tv = p_from - p0;
	float u = dot(tv, pv) * inv_det;
	if (u < 0.0 || u > 1.0) {
		return false;
	}
	vec3 qv = cross(tv, e1);
	float v = dot(p_dir, qv) * inv_det;
	if (v < 0.0 || u + v > 1.0) {
		return false;
	}
	float t = dot(e2, qv) * inv_det;
	return t > 0.0 && t < p_max;
}

// True when anything lies between p_from and p_max along p_dir.
bool occluded(vec3 p_from, vec3 p_dir, float p_max) {
	vec3 inv_dir = 1.0 / mix(p_dir, vec3(1e-12), equal(p_dir, vec3(0.0)));
	uint stack[32];
	uint stack_size = 0;
	uint node = 0;
	while (true) {
		uvec4 a = bvh.data[node * 2 + 0];
		uvec4 b = bvh.data[node * 2 + 1];
		if (ray_box(p_from, inv_dir, p_max, uintBitsToFloat(a.xyz), uintBitsToFloat(b.xyz))) {
			if (b.w > 0) {
				for (uint i = 0; i < b.w; i++) {
					uint t = (a.w + i) * 3;
					if (ray_triangle(p_from, p_dir, p_max, triangles.data[t].xyz, triangles.data[t + 1].xyz, triangles.data[t + 2].xyz)) {
						return true;
					}
				}
			} else {
				if (stack_size < 32) {
					stack[stack_size++] = a.w;
				}
				node = node + 1;
				continue;
			}
		}
		if (stack_size == 0) {
			break;
		}
		node = stack[--stack_size];
	}
	return false;
}

float get_omni_attenuation(float p_distance, float p_inv_range, float p_decay) {
	float nd = p_distance * p_inv_range;
	nd *= nd;
	nd *= nd;
	nd = max(1.0 - nd, 0.0);
	nd *= nd;
	return nd * pow(max(p_distance, 0.0001), -p_decay);
}

#endif

void main() {
#if defined(MODE_SKY_SH) || defined(MODE_SKY_SH_ARRAY)

	// Project the sky onto nine spherical harmonics, from 256 directions
	// spread evenly over the sphere. One workgroup does it all.
	const uint SAMPLES = 256;
	vec3 accum[9];
	for (uint i = 0; i < 9; i++) {
		accum[i] = vec3(0.0);
	}
	for (uint s = gl_LocalInvocationID.x; s < SAMPLES; s += 64) {
		vec3 dir = fibonacci_direction(s, SAMPLES);
		vec3 light = params.sky_color;
		if (params.sky_mode == SKY_MODE_TEXTURE) {
			vec4 q = params.sky_orientation;
			vec3 sky_dir = dir + 2.0 * cross(q.xyz, cross(q.xyz, dir) + q.w * dir);
			vec2 uv = vec3_to_oct_with_border(sky_dir, params.sky_border);
#if defined(MODE_SKY_SH_ARRAY)
			vec3 sky = textureLod(sampler2DArray(sky_radiance, linear_sampler_mipmaps), vec3(uv, params.sky_lod), 0.0).rgb;
#else
			vec3 sky = textureLod(sampler2D(sky_radiance, linear_sampler_mipmaps), uv, params.sky_lod).rgb;
#endif
			light = mix(params.sky_color, sky, params.sky_mix);
		}
		float c[9];
		sh_basis(dir, c);
		for (uint i = 0; i < 9; i++) {
			accum[i] += light * c[i];
		}
	}

	// Reduce through shared memory.
	for (uint i = 0; i < 9; i++) {
		partial[gl_LocalInvocationID.x][i] = accum[i];
	}
	barrier();
	for (uint stride = 32; stride > 0; stride >>= 1) {
		if (gl_LocalInvocationID.x < stride) {
			for (uint i = 0; i < 9; i++) {
				partial[gl_LocalInvocationID.x][i] += partial[gl_LocalInvocationID.x + stride][i];
			}
		}
		barrier();
	}
	if (gl_LocalInvocationID.x == 0) {
		for (uint i = 0; i < 9; i++) {
			sky_sh.data[i] = vec4(partial[0][i] * (4.0 * M_PI / float(SAMPLES)) * params.environment_energy, 0.0);
		}
	}

#elif defined(MODE_DIRECT)

	uint patch_index = gl_GlobalInvocationID.x * params.direct_stride + params.direct_offset;
	uint local = gl_LocalInvocationID.x;
	bool in_range = patch_index < params.patch_count;

	Patch p = patches.data[min(patch_index, params.patch_count - 1)];
	vec3 normal = p.normal.xyz;
	float radius = p.position_radius.w;

	// The group's bounds, by parallel reduction.
	group_min[local] = in_range ? p.position_radius.xyz - radius : vec3(1e20);
	group_max[local] = in_range ? p.position_radius.xyz + radius : vec3(-1e20);
	if (local == 0) {
		group_light_count = 0;
	}
	barrier();
	for (uint stride = 32; stride > 0; stride >>= 1) {
		if (local < stride) {
			group_min[local] = min(group_min[local], group_min[local + stride]);
			group_max[local] = max(group_max[local], group_max[local + stride]);
		}
		barrier();
	}
	vec3 group_center = (group_min[0] + group_max[0]) * 0.5;
	float group_radius = length(group_max[0] - group_min[0]) * 0.5;
	for (uint l = local; l < params.light_count; l += 64) {
		Light light = lights.data[l];
		bool reaches = light.type == LIGHT_TYPE_DIRECTIONAL || distance(light.position, group_center) < group_radius + 1.0 / light.inv_range;
		if (reaches) {
			uint slot = atomicAdd(group_light_count, 1);
			if (slot < MAX_GROUP_LIGHTS) {
				group_lights[slot] = l;
			}
		}
	}
	barrier();
	if (!in_range) {
		return;
	}
	uint group_count = min(group_light_count, uint(MAX_GROUP_LIGHTS));

	// Shadow rays leave from points spread over the patch, so a shadow edge
	// crossing it shades it partly instead of all or nothing.
	vec3 tangent = normalize(abs(normal.y) < 0.99 ? cross(normal, vec3(0.0, 1.0, 0.0)) : cross(normal, vec3(1.0, 0.0, 0.0)));
	vec3 bitangent = cross(normal, tangent);
	float bias = max(0.02, radius * 0.1);

	vec3 bounce_light = vec3(0.0);
	vec3 static_light = vec3(0.0);

	for (uint i = 0; i < group_count; i++) {
		Light light = lights.data[group_lights[i]];

		vec3 light_dir;
		float attenuation = 1.0;
		float max_dist = 1e20;

		if (light.type == LIGHT_TYPE_DIRECTIONAL) {
			light_dir = -light.direction;
		} else {
			vec3 rel = light.position - p.position_radius.xyz;
			float dist = length(rel);
			if (dist * light.inv_range >= 1.0) {
				continue;
			}
			light_dir = rel / max(dist, 1e-6);
			attenuation = get_omni_attenuation(dist, light.inv_range, light.attenuation);
			if (light.type == LIGHT_TYPE_SPOT) {
				float scos = max(dot(-light_dir, light.direction), light.cos_spot_angle);
				float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - light.cos_spot_angle));
				attenuation *= 1.0 - pow(spot_rim, light.inv_spot_attenuation);
			}
			max_dist = dist;
		}

		float n_dot_l = dot(normal, light_dir);
		if (n_dot_l <= 0.0 || attenuation <= 0.0) {
			continue;
		}

		float visibility = 1.0;
		if (bool(light.flags & LIGHT_FLAG_SHADOW)) {
			uint hits = 0;
			for (uint s = 0; s < params.shadow_samples; s++) {
				vec2 offset = vec2(0.0);
				if (params.shadow_samples > 1) {
					float r = sqrt((float(s) + 0.5) / float(params.shadow_samples)) * 0.7;
					float phi = GOLDEN_ANGLE * float(s);
					offset = vec2(cos(phi), sin(phi)) * r * radius;
				}
				vec3 from = p.position_radius.xyz + tangent * offset.x + bitangent * offset.y + normal * bias;
				vec3 to_light = light.type == LIGHT_TYPE_DIRECTIONAL ? light_dir : normalize(light.position - from);
				float dist = light.type == LIGHT_TYPE_DIRECTIONAL ? 1e20 : length(light.position - from) - bias;
				if (occluded(from, to_light, dist)) {
					hits++;
				}
			}
			visibility = 1.0 - float(hits) / float(params.shadow_samples);
		}

		vec3 light_value = light.color * (n_dot_l * attenuation * visibility);
		bounce_light += light_value * light.indirect_energy;
		if (bool(light.flags & LIGHT_FLAG_STATIC)) {
			static_light += light_value;
		}
	}

	direct.data[patch_index * 2 + 0] = vec4(bounce_light, 0.0);
	direct.data[patch_index * 2 + 1] = vec4(static_light, 0.0);

#elif defined(MODE_RADIOSITY)

	uint patch_index = gl_GlobalInvocationID.x;
	if (patch_index >= params.patch_count) {
		return;
	}

	Patch p = patches.data[patch_index];
	vec3 albedo = min(p.albedo.rgb * params.albedo_boost, vec3(0.98));
	vec3 irradiance = direct.data[patch_index * 2].rgb + indirect.data[patch_index].rgb;
	radiosity.data[patch_index] = vec4(albedo * irradiance + p.emission.rgb * params.emission_energy, 1.0);

#elif defined(MODE_CLUSTER)

	uint cluster_index = gl_GlobalInvocationID.x;
	if (cluster_index >= params.cluster_count) {
		return;
	}

	uint from = cluster_offsets.data[cluster_index];
	uint to = cluster_offsets.data[cluster_index + 1];
	vec3 sum = vec3(0.0);
	for (uint i = from; i < to; i++) {
		sum += radiosity.data[i].rgb;
	}
	cluster_radiance.data[cluster_index] = vec4(sum / float(max(to - from, 1u)), 1.0);

#elif defined(MODE_GATHER)

	uint patch_index = gl_GlobalInvocationID.x;
	if (patch_index >= params.patch_count) {
		return;
	}

	uint from = transport_offsets.data[patch_index];
	uint to = transport_offsets.data[patch_index + 1];
	vec3 light = vec3(0.0);
	for (uint i = from; i < to; i++) {
		uint entry = transport_entries.data[i];
		light += cluster_radiance.data[entry >> 10].rgb * float(entry & 0x3FF);
	}
	light /= float(params.transport_rays);

	for (uint i = 0; i < 9; i++) {
		light += sky_sh.data[i].rgb * sky_visibility.data[patch_index * 9 + i];
	}

	indirect.data[patch_index] = vec4(max(light, vec3(0.0)), 1.0);

#elif defined(MODE_OUTPUT)

	ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
	if (texel.x >= int(params.atlas_width) || texel.y >= int(params.atlas_height)) {
		return;
	}

	int patch_index = texel_patches.data[texel.y * int(params.atlas_width) + texel.x];
	vec3 light = vec3(0.0);
	if (patch_index >= 0) {
		light = indirect.data[patch_index].rgb * params.indirect_energy + direct.data[patch_index * 2 + 1].rgb;
	}
	imageStore(output_light, ivec3(texel, 0), vec4(light, 1.0));

#elif defined(MODE_PROBES)

	uint probe_index = gl_GlobalInvocationID.x;
	if (probe_index >= params.probe_count) {
		return;
	}

	vec3 accum[9];
	for (uint i = 0; i < 9; i++) {
		accum[i] = vec3(0.0);
	}

	for (uint r = 0; r < params.probe_rays; r++) {
		int hit = probe_rays.data[probe_index * params.probe_rays + r];
		vec3 dir = fibonacci_direction(r, params.probe_rays);
		vec3 light;
		if (hit >= 0) {
			light = cluster_radiance.data[hit].rgb;
		} else if (hit == -1) {
			light = sky_light(dir);
		} else {
			continue;
		}
		float c[9];
		sh_basis(dir, c);
		for (uint i = 0; i < 9; i++) {
			accum[i] += light * c[i];
		}
	}

	// The same scale LightmapGI gives its probes.
	float scale = 4.0 / float(params.probe_rays) * params.indirect_energy;
	for (uint i = 0; i < 9; i++) {
		probe_sh.data[probe_index * 9 + i] = vec4(accum[i] * scale, 0.0);
	}

#endif
}
