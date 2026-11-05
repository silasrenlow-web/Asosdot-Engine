#[compute]

#version 450

#VERSION_DEFINES

// Motion blur from a velocity buffer, in four passes, after "A Reconstruction
// Filter for Plausible Motion Blur" (McGuire et al.), the way Unreal does it:
//
//   PREPARE        Turns the velocity buffer into pixels of blur, scaled by how
//                  long the shutter is open and clamped to the longest a streak
//                  may be. Where nothing was drawn, the sky, the velocity comes
//                  from how the camera turned. Packs all a sample of the gather
//                  needs, the colour, the distance from the camera and one over
//                  the length of the streak, into one texel, so a sample is one
//                  read, and the velocity with them. This is also the frame as it
//                  was, which the gather reads while it writes over the frame. Finds
//                  the fastest velocity of each 8 by 8 block of pixels on the way.
//   TILE_MAX       The fastest velocity in each tile of pixels, from the blocks.
//   NEIGHBOR_MAX   The fastest of a tile and the eight around it, so a pixel knows
//                  how far anything that could smear across it is moving.
//   GATHER         Looks along that direction and weighs what it finds: what is in
//                  front smears over what is behind, and a pixel keeps its own
//                  colour when it barely moves.

#define BLOCK_SIZE 8

layout(local_size_x = BLOCK_SIZE, local_size_y = BLOCK_SIZE, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	ivec2 size; // Of the colour image.
	ivec2 tile_count;
	int tile_size;
	int samples; // Odd, so the middle one is the pixel itself.
	float velocity_scale; // Turns the velocity buffer's UV into pixels of blur.
	float max_length; // The longest a streak is, in pixels.
	float z_near;
	float z_far;
	uint orthogonal;
	uint pad;
	mat4 reprojection; // From this frame's clip space to the last one's.
}
params;

// A pixel, in half floats like everything it is made of:
//   x  red, green
//   y  blue, meters from the camera
//   z  one over the length of the streak in pixels, alpha
//   w  the velocity in pixels
// Half precision is plenty for the distance, which the gather compares by a tenth
// of its size.

#if defined(MODE_PREPARE)

layout(set = 0, binding = 0) uniform sampler2D source_depth;
layout(set = 0, binding = 1) uniform sampler2D source_velocity;
layout(set = 0, binding = 2) uniform sampler2D source_color;
layout(rgba32ui, set = 0, binding = 3) uniform restrict writeonly uimage2D out_pixels;
layout(rg16f, set = 0, binding = 4) uniform restrict writeonly image2D out_group_max;

shared vec2 group_velocity[BLOCK_SIZE * BLOCK_SIZE];

// Meters from the camera.
float linear_depth(float p_depth) {
	float depth = p_depth * 2.0 - 1.0;
	float meters;
	if (params.orthogonal != 0u) {
		meters = -(depth * (params.z_far - params.z_near) - (params.z_far + params.z_near)) / 2.0;
	} else {
		meters = 2.0 * params.z_near * params.z_far / (params.z_far + params.z_near + depth * (params.z_far - params.z_near));
	}
	return clamp(meters, 0.0, 60000.0);
}

void main() {
	// Every thread of the group takes part in settling on its fastest velocity, so
	// the ones past the edge of the image work on the pixel at the edge, which can't
	// change the result, and don't write it.
	bool inside = all(lessThan(ivec2(gl_GlobalInvocationID.xy), params.size));
	ivec2 pos = min(ivec2(gl_GlobalInvocationID.xy), params.size - 1);
	uint thread = gl_LocalInvocationIndex;

	vec4 color = texelFetch(source_color, pos, 0);
	vec2 uv = (vec2(pos) + 0.5) / vec2(params.size);
	float depth = texelFetch(source_depth, pos, 0).r;

	vec2 velocity;
	if (depth <= 0.0) {
		// Reverse Z: nothing was drawn here, so there is no velocity either. What is
		// there is as far away as anything gets, and the camera turning is what moves
		// it. Taken through the matrices the scene's own velocity comes from.
		vec2 ndc = uv * 2.0 - 1.0;
		vec4 previous = params.reprojection * vec4(ndc, 0.0, 1.0);
		velocity = (previous.xy / previous.w - ndc) * 0.5;
	} else {
		velocity = texelFetch(source_velocity, pos, 0).rg;
	}

	// Pixels. The buffer holds where the pixel was, relative to where it is.
	vec2 blur = velocity * vec2(params.size) * params.velocity_scale;
	float length_px = length(blur);
	if (length_px > params.max_length) {
		blur *= params.max_length / length_px;
		length_px = params.max_length;
	}
	// Too short to see: left alone, and the gather pass skips it.
	if (length_px < 0.5) {
		blur = vec2(0.0);
		length_px = 0.0;
	}

	uvec4 pixel;
	pixel.x = packHalf2x16(color.rg);
	pixel.y = packHalf2x16(vec2(color.b, linear_depth(depth)));
	pixel.z = packHalf2x16(vec2(1.0 / max(length_px, 0.0001), color.a));
	pixel.w = packHalf2x16(blur);
	if (inside) {
		imageStore(out_pixels, pos, pixel);
	}

	group_velocity[thread] = blur;
	barrier();

	for (uint stride = BLOCK_SIZE * BLOCK_SIZE / 2; stride > 0; stride >>= 1) {
		if (thread < stride) {
			vec2 other = group_velocity[thread + stride];
			if (dot(other, other) > dot(group_velocity[thread], group_velocity[thread])) {
				group_velocity[thread] = other;
			}
		}
		barrier();
	}

	if (thread == 0) {
		imageStore(out_group_max, ivec2(gl_WorkGroupID.xy), vec4(group_velocity[0], 0.0, 0.0));
	}
}

#elif defined(MODE_TILE_MAX)

layout(set = 0, binding = 0) uniform sampler2D group_max_image;
layout(rg16f, set = 0, binding = 1) uniform restrict writeonly image2D out_tile_max;

shared vec2 group_velocity[BLOCK_SIZE * BLOCK_SIZE];

// One group per tile: every thread takes one of the 8 by 8 pixel blocks the tile is
// made of, which the first pass found the fastest velocity of, then the group
// settles on the fastest.
void main() {
	ivec2 tile = ivec2(gl_WorkGroupID.xy);
	uint thread = gl_LocalInvocationIndex;

	int blocks_in_tile = params.tile_size / BLOCK_SIZE;
	ivec2 block = tile * blocks_in_tile + ivec2(gl_LocalInvocationID.xy);
	ivec2 block_count = (params.size + (BLOCK_SIZE - 1)) / BLOCK_SIZE;

	vec2 best = vec2(0.0);
	if (all(lessThan(ivec2(gl_LocalInvocationID.xy), ivec2(blocks_in_tile))) && all(lessThan(block, block_count))) {
		best = texelFetch(group_max_image, block, 0).xy;
	}
	group_velocity[thread] = best;
	barrier();

	for (uint stride = BLOCK_SIZE * BLOCK_SIZE / 2; stride > 0; stride >>= 1) {
		if (thread < stride) {
			vec2 other = group_velocity[thread + stride];
			if (dot(other, other) > dot(group_velocity[thread], group_velocity[thread])) {
				group_velocity[thread] = other;
			}
		}
		barrier();
	}

	if (thread == 0 && all(lessThan(tile, params.tile_count))) {
		imageStore(out_tile_max, tile, vec4(group_velocity[0], 0.0, 0.0));
	}
}

#elif defined(MODE_NEIGHBOR_MAX)

layout(set = 0, binding = 0) uniform sampler2D tile_image;
layout(rg16f, set = 0, binding = 1) uniform restrict writeonly image2D out_neighbor_max;

void main() {
	ivec2 tile = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(tile, params.tile_count))) {
		return;
	}

	vec2 best = vec2(0.0);
	float best_length = 0.0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 t = clamp(tile + ivec2(x, y), ivec2(0), params.tile_count - 1);
			vec2 v = texelFetch(tile_image, t, 0).xy;
			float l = dot(v, v);
			if (l > best_length) {
				best_length = l;
				best = v;
			}
		}
	}
	imageStore(out_neighbor_max, tile, vec4(best, 0.0, 0.0));
}

#elif defined(MODE_GATHER)

layout(set = 0, binding = 0) uniform usampler2D source_pixels;
layout(set = 0, binding = 1) uniform sampler2D neighbor_image;
layout(rgba16f, set = 0, binding = 2) uniform restrict writeonly image2D out_color;

// 1 when p_a is no farther away than p_b, falling to 0 when it is farther by
// more than the extent, so the line between what is in front and what is behind
// is soft rather than a step. Takes one over the extent.
float soft_compare(float p_a, float p_b, float p_inverse_extent) {
	return clamp(1.0 - (p_a - p_b) * p_inverse_extent, 0.0, 1.0);
}

// How much a pixel p_distance from a streak's start is covered by a streak: all the
// way in at its start, none at its end. Takes one over the streak's length.
float cone(float p_distance, float p_inverse_length) {
	return clamp(1.0 - p_distance * p_inverse_length, 0.0, 1.0);
}

float cylinder(float p_distance, float p_inverse_length) {
	return 1.0 - smoothstep(0.95, 1.05, p_distance * p_inverse_length);
}

// Noise that is even over the screen, so the samples' jitter doesn't clump.
float gradient_noise(ivec2 p) {
	return fract(52.9829189 * fract(dot(vec2(p), vec2(0.06711056, 0.00583715))));
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.size))) {
		return;
	}

	ivec2 tile = min(pos / params.tile_size, params.tile_count - 1);
	vec2 neighbor = texelFetch(neighbor_image, tile, 0).xy;
	float neighbor_length = length(neighbor);
	if (neighbor_length < 0.5) {
		// Nothing near enough to move over this pixel, and the frame is already in
		// the image being written.
		return;
	}

	uvec4 center_pixel = texelFetch(source_pixels, pos, 0);
	vec2 center_blue_depth = unpackHalf2x16(center_pixel.y);
	vec2 center_inverse_length_alpha = unpackHalf2x16(center_pixel.z);
	vec3 center_color = vec3(unpackHalf2x16(center_pixel.x), center_blue_depth.x);
	float center_depth = center_blue_depth.y;
	float center_inverse_length = center_inverse_length_alpha.x;
	float jitter = gradient_noise(pos) - 0.5;

	// One sample for every couple of pixels the streak is long is all that can be told
	// apart; more is work for nothing. The pixel itself weighs the same as it would
	// with all the samples there are.
	int count = min(params.samples, max(5, (int(neighbor_length * 0.5) + 1) | 1));

	float total = (float(count) / float(params.samples)) * min(1.0, center_inverse_length);
	vec3 sum = center_color * total;

	for (int i = 0; i < count; i++) {
		if (i == count / 2) {
			continue;
		}

		float t = mix(-1.0, 1.0, (float(i) + jitter + 1.0) / float(count + 1));
		// How far along the streak, which is how far from the pixel.
		float distance_px = abs(t) * 0.5 * neighbor_length;
		ivec2 other = clamp(ivec2(round(vec2(pos) + neighbor * t * 0.5)), ivec2(0), params.size - 1);

		uvec4 other_pixel = texelFetch(source_pixels, other, 0);
		vec2 other_blue_depth = unpackHalf2x16(other_pixel.y);
		float other_depth = other_blue_depth.y;
		float other_inverse_length = unpackHalf2x16(other_pixel.z).x;

		float inverse_extent = 1.0 / (max(center_depth, other_depth) * 0.1 + 0.02);
		float other_in_front = soft_compare(other_depth, center_depth, inverse_extent);
		float other_behind = soft_compare(center_depth, other_depth, inverse_extent);

		float weight = other_in_front * cone(distance_px, other_inverse_length) + other_behind * cone(distance_px, center_inverse_length) + cylinder(distance_px, other_inverse_length) * cylinder(distance_px, center_inverse_length) * 2.0;

		total += weight;
		sum += vec3(unpackHalf2x16(other_pixel.x), other_blue_depth.x) * weight;
	}

	imageStore(out_color, pos, vec4(sum / total, center_inverse_length_alpha.y));
}

#endif
