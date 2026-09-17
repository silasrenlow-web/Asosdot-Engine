#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require
#extension GL_EXT_ray_tracing_position_fetch : require

// Asosdot one bounce ray traced global illumination.
//
// For each pixel, traces cosine weighted rays into the scene. Where a ray hits
// geometry, the hit is shaded: the triangle's vertices are read straight out of
// the acceleration structure (position fetch) to get a real geometric normal,
// the instance's flat albedo is looked up, and a shadow ray decides whether the
// sun reaches it. The result is light that has bounced off real geometry and
// carries its colour -- a red wall tints a white floor red.
//
// Limitations, deliberate for now: one bounce, flat per-instance albedo with no
// textures, and rays that miss contribute nothing (the sky is already handled
// by Godot's own ambient term, so adding it here would double count).

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1) uniform sampler2D source_depth;
layout(rgba16f, set = 0, binding = 2) uniform restrict writeonly image2D dest_gi;

struct AsosInstance {
	vec4 albedo;
	// x = index offset, y = vertex offset, z = albedo texture slot
	// (0xFFFFFFFF when untextured), w = flags.
	uvec4 params;
};

layout(set = 0, binding = 3, std430) restrict readonly buffer InstanceData {
	AsosInstance data[];
}
instance_data;

// Shading attributes for every traceable surface, concatenated. One vec4 per
// vertex: uv.xy and the octahedral normal pair.
layout(set = 0, binding = 4, std430) restrict readonly buffer ShadingVertices {
	vec4 data[];
}
shading_vertices;

layout(set = 0, binding = 5, std430) restrict readonly buffer ShadingIndices {
	uint data[];
}
shading_indices;

layout(push_constant, std430) uniform Params {
	mat4 inv_view_proj;
	vec4 cam_pos; // w unused
	vec4 sun_dir; // xyz toward the sun, w = sun energy
	vec4 sun_color; // rgb, w = gi intensity
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

// Matches Godot's own vertex normal decode, so no re-encoding was needed
// when the attributes were extracted.
vec3 oct_to_vec3(vec2 e) {
	vec3 v = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
	float t = max(-v.z, 0.0);
	v.xy += t * -sign(v.xy);
	return normalize(v);
}

vec3 world_from_depth(vec2 uv, float depth) {
	vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
	vec4 p = params.inv_view_proj * ndc;
	return p.xyz / p.w;
}

vec3 sample_cosine_hemisphere(vec3 n, float u1, float u2) {
	float r = sqrt(u1);
	float phi = PI2 * u2;

	vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(up, n));
	vec3 bitangent = cross(n, tangent);

	return normalize(tangent * (r * cos(phi)) + bitangent * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
}

// True when anything blocks the straight line from p toward the sun.
bool sun_blocked(vec3 p, vec3 n) {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas,
			gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
			0xFF, p + n * 0.02, 0.005, normalize(params.sun_dir.xyz), 500.0);
	while (rayQueryProceedEXT(rq)) {
	}
	return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
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
		imageStore(dest_gi, coord, vec4(0.0));
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
		imageStore(dest_gi, coord, vec4(0.0));
		return;
	}
	normal /= nl;
	if (dot(normal, params.cam_pos.xyz - pos) < 0.0) {
		normal = -normal;
	}

	vec3 sun_dir = normalize(params.sun_dir.xyz);
	vec3 sun_radiance = params.sun_color.rgb * params.sun_dir.w;

	uint rng = hash_u32(uint(coord.x) * 73856093u ^ uint(coord.y) * 19349663u ^ params.frame * 83492791u);

	uint count = max(params.ray_count, 1u);
	vec3 bounced = vec3(0.0);

	vec3 origin = pos + normal * 0.02;

#ifdef MODE_REFLECTION
	// A mirror reflection is one direction, not a hemisphere of them.
	count = 1u;
	vec3 view_dir = normalize(pos - params.cam_pos.xyz);
#endif

	float hit_count = 0.0;

	for (uint i = 0u; i < count; i++) {
#ifdef MODE_REFLECTION
		vec3 dir = reflect(view_dir, normal);
#else
		vec3 dir = sample_cosine_hemisphere(normal, rand_float(rng), rand_float(rng));
#endif

		rayQueryEXT rq;
		// Closest hit, not first hit: we need the nearest surface to shade.
		rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFF, origin, 0.005, dir, 40.0);
		while (rayQueryProceedEXT(rq)) {
		}

		if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
			// Missed: the sky is already in Godot's ambient term.
			continue;
		}

		// Vertices come back in the acceleration structure's object space; the
		// object-to-world transform carries the instance placement (and, for
		// compressed meshes, the AABB decode) so this lands in world space.
		vec3 verts[3];
		rayQueryGetIntersectionTriangleVertexPositionsEXT(rq, true, verts);
		mat4x3 obj_to_world = rayQueryGetIntersectionObjectToWorldEXT(rq, true);

		vec3 w0 = obj_to_world * vec4(verts[0], 1.0);
		vec3 w1 = obj_to_world * vec4(verts[1], 1.0);
		vec3 w2 = obj_to_world * vec4(verts[2], 1.0);

		vec3 hit_normal = cross(w1 - w0, w2 - w0);
		float hn_len = length(hit_normal);
		if (hn_len < 1e-8) {
			continue;
		}
		hit_normal /= hn_len;
		// Face the incoming ray, so back faces still light correctly.
		if (dot(hit_normal, dir) > 0.0) {
			hit_normal = -hit_normal;
		}

		hit_count += 1.0;

		float t = rayQueryGetIntersectionTEXT(rq, true);
		vec3 hit_pos = origin + dir * t;

		int id = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
		AsosInstance inst = instance_data.data[id];
		vec3 albedo = inst.albedo.rgb;

		// The primitive index and barycentrics are enough to find the hit
		// triangle's three vertices in the shared buffers and interpolate.
		uint prim = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true));
		vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, true);
		uint base = inst.params.x + prim * 3u;
		uint i0 = shading_indices.data[base + 0u] + inst.params.y;
		uint i1 = shading_indices.data[base + 1u] + inst.params.y;
		uint i2 = shading_indices.data[base + 2u] + inst.params.y;

		vec4 a0 = shading_vertices.data[i0];
		vec4 a1 = shading_vertices.data[i1];
		vec4 a2 = shading_vertices.data[i2];
		// bw0 rather than w0: w0 is already the first triangle world position.
		float bw0 = 1.0 - bary.x - bary.y;

		vec2 hit_uv = a0.xy * bw0 + a1.xy * bary.x + a2.xy * bary.y;

		// Interpolated (smooth) normal, which is what makes a curved surface
		// bounce light like a curve instead of like a bag of flat triangles.
		vec3 n_smooth = oct_to_vec3(a0.zw * 2.0 - 1.0) * bw0 +
				oct_to_vec3(a1.zw * 2.0 - 1.0) * bary.x +
				oct_to_vec3(a2.zw * 2.0 - 1.0) * bary.y;
		n_smooth = mat3(obj_to_world) * n_smooth;
		if (dot(n_smooth, n_smooth) > 1e-12) {
			n_smooth = normalize(n_smooth);
			// Keep it on the same side as the geometric normal, so a mesh with
			// inverted or missing normals cannot flip the shading.
			if (dot(n_smooth, hit_normal) > 0.0) {
				hit_normal = n_smooth;
			}
		}

		float ndotl = max(dot(hit_normal, sun_dir), 0.0);
		if (ndotl > 0.0 && !sun_blocked(hit_pos, hit_normal)) {
			// Cosine weighted sampling already carries the receiver's cosine
			// term, so the estimator is just the mean of incoming radiance.
			bounced += albedo * sun_radiance * ndotl;
		}
	}

	vec3 gi = (bounced / float(count)) * params.sun_color.w;
#ifdef MODE_REFLECTION
	// Alpha carries whether the ray found geometry at all, so a miss can fall
	// back to Godot's existing sky reflection instead of turning black.
	imageStore(dest_gi, coord, vec4(gi, hit_count / float(count)));
#else
	imageStore(dest_gi, coord, vec4(gi, 1.0));
#endif
}
