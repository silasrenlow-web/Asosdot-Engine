#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require

// Asosdot ray traced sun shadows.
//
// Traces visibility from each pixel toward the sun. Unlike a shadow map this
// has no bias to tune, no acne, no peter-panning and no cascade seams -- the
// ray either reaches the sun or it does not. Sampling a cone matching the
// sun's angular size gives physically correct contact hardening: the penumbra
// is tight where an occluder touches the receiver and widens with distance.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1) uniform sampler2D source_depth;
layout(r8, set = 0, binding = 2) uniform restrict writeonly image2D dest_shadow;

layout(push_constant, std430) uniform Params {
	mat4 inv_view_proj;
	vec4 cam_pos; // w unused
	vec4 sun; // xyz = direction toward the sun, w = angular radius in radians
	vec2 screen_size;
	uint ray_count;
	uint frame;
}
params;

const float PI2 = 6.28318530717958647692;

uint hash_u32(uint x) {
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

float rand_float(inout uint state) {
	state = hash_u32(state);
	return float(state) * (1.0 / 4294967296.0);
}

vec3 world_from_depth(vec2 uv, float depth) {
	vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
	vec4 p = params.inv_view_proj * ndc;
	return p.xyz / p.w;
}

// Uniformly sample a cone of half-angle `angle` around `dir`.
vec3 sample_cone(vec3 dir, float angle, float u1, float u2) {
	float cos_theta = mix(cos(angle), 1.0, u1);
	float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
	float phi = PI2 * u2;

	vec3 up = abs(dir.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(up, dir));
	vec3 bitangent = cross(dir, tangent);

	return normalize(tangent * (sin_theta * cos(phi)) + bitangent * (sin_theta * sin(phi)) + dir * cos_theta);
}

void main() {
	ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = ivec2(params.screen_size);
	if (coord.x >= size.x || coord.y >= size.y) {
		return;
	}

	vec2 texel = 1.0 / params.screen_size;
	vec2 uv = (vec2(coord) + 0.5) * texel;

	float depth = texture(source_depth, uv).r;
	if (depth <= 0.0) {
		// Sky: nothing to shadow.
		imageStore(dest_shadow, coord, vec4(1.0));
		return;
	}

	vec3 pos = world_from_depth(uv, depth);

	vec2 uv_dx = uv + vec2(texel.x, 0.0);
	vec2 uv_dy = uv + vec2(0.0, texel.y);
	vec3 pos_dx = world_from_depth(uv_dx, texture(source_depth, uv_dx).r);
	vec3 pos_dy = world_from_depth(uv_dy, texture(source_depth, uv_dy).r);

	vec3 normal = cross(pos_dx - pos, pos_dy - pos);
	float nl = length(normal);
	if (nl < 1e-8) {
		imageStore(dest_shadow, coord, vec4(1.0));
		return;
	}
	normal /= nl;
	if (dot(normal, params.cam_pos.xyz - pos) < 0.0) {
		normal = -normal;
	}

	vec3 sun_dir = normalize(params.sun.xyz);

	// Surfaces facing away from the sun are already unlit by N dot L. Writing
	// full visibility here avoids darkening them twice.
	float ndotl = dot(normal, sun_dir);
	if (ndotl <= 0.0) {
		imageStore(dest_shadow, coord, vec4(1.0));
		return;
	}

	// Offset along the normal, scaled at grazing angles where the reconstructed
	// position is least reliable.
	vec3 origin = pos + normal * mix(0.05, 0.01, ndotl);

	uint rng = hash_u32(uint(coord.x) * 73856093u ^ uint(coord.y) * 19349663u ^ params.frame * 83492791u);

	uint count = max(params.ray_count, 1u);
	float blocked = 0.0;

	for (uint i = 0u; i < count; i++) {
		vec3 dir = sun_dir;
		if (params.sun.w > 0.0001) {
			dir = sample_cone(sun_dir, params.sun.w, rand_float(rng), rand_float(rng));
		}

		rayQueryEXT rq;
		rayQueryInitializeEXT(rq, tlas,
				gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
				0xFF, origin, 0.005, dir, 500.0);
		while (rayQueryProceedEXT(rq)) {
		}

		if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
			blocked += 1.0;
		}
	}

	float visibility = 1.0 - (blocked / float(count));
	imageStore(dest_shadow, coord, vec4(clamp(visibility, 0.0, 1.0)));
}
