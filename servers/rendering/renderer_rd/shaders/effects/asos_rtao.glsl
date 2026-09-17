#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require

// Asosdot ray traced ambient occlusion.
//
// Traces cosine weighted rays against the scene acceleration structure instead
// of marching the depth buffer. The practical difference is that occluders do
// not have to be on screen or in front of anything: screen space AO simply has
// no information about geometry it cannot see, while this queries real geometry.
//
// The surface normal is reconstructed from depth rather than read from the
// normal/roughness buffer, because that buffer is only allocated when other
// screen space effects ask for it. Depth alone keeps this pass self contained.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1) uniform sampler2D source_depth;
layout(r8, set = 0, binding = 2) uniform restrict writeonly image2D dest_ao;

layout(push_constant, std430) uniform Params {
	mat4 inv_view_proj;
	vec4 cam_pos; // w unused
	vec2 screen_size;
	float radius;
	float intensity;
	uint ray_count;
	uint frame;
	float pad0;
	float pad1;
}
params;

const float PI2 = 6.28318530717958647692;

// Cheap integer hash, used to decorrelate sampling per pixel and per frame.
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

// Godot uses reverse-Z, so NDC depth arrives in [0,1] with 0 at the far plane.
vec3 world_from_depth(vec2 uv, float depth) {
	vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
	vec4 p = params.inv_view_proj * ndc;
	return p.xyz / p.w;
}

// Build an orthonormal basis around n and sample the cosine weighted hemisphere.
vec3 sample_cosine_hemisphere(vec3 n, float u1, float u2) {
	float r = sqrt(u1);
	float phi = PI2 * u2;

	vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(up, n));
	vec3 bitangent = cross(n, tangent);

	return normalize(tangent * (r * cos(phi)) + bitangent * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
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
		// Far plane under reverse-Z: sky, nothing to occlude.
		imageStore(dest_ao, coord, vec4(1.0));
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
		// Degenerate derivative (depth discontinuity); leave the pixel unoccluded
		// rather than inventing a normal and tracing from garbage.
		imageStore(dest_ao, coord, vec4(1.0));
		return;
	}
	normal /= nl;

	// A flipped derivative would invert the hemisphere, so orient toward the eye.
	vec3 view_dir = params.cam_pos.xyz - pos;
	if (dot(normal, view_dir) < 0.0) {
		normal = -normal;
	}

	uint rng = hash_u32(uint(coord.x) * 73856093u ^ uint(coord.y) * 19349663u ^ params.frame * 83492791u);

	uint count = max(params.ray_count, 1u);
	float occlusion = 0.0;

	// Offset along the normal so the surface does not shadow itself.
	vec3 origin = pos + normal * 0.02;

	for (uint i = 0u; i < count; i++) {
		float u1 = rand_float(rng);
		float u2 = rand_float(rng);
		vec3 dir = sample_cosine_hemisphere(normal, u1, u2);

		rayQueryEXT rq;
		// Any hit is enough for occlusion, so stop at the first one.
		rayQueryInitializeEXT(rq, tlas,
				gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
				0xFF, origin, 0.005, dir, params.radius);
		while (rayQueryProceedEXT(rq)) {
		}

		if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
			occlusion += 1.0;
		}
	}

	float ao = 1.0 - (occlusion / float(count)) * params.intensity;
	imageStore(dest_ao, coord, vec4(clamp(ao, 0.0, 1.0)));
}
