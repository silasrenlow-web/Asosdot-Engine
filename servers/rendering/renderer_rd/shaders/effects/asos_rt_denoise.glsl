#[compute]

#version 450

#VERSION_DEFINES

// Asosdot denoiser for traced visibility buffers.
//
// A depth aware (bilateral) blur. Tracing a handful of rays per pixel is cheap
// but noisy; neighbouring pixels on the same surface are estimating the same
// quantity, so averaging them recovers most of the quality of a far larger ray
// budget. Weighting by depth keeps the blur from bleeding across silhouettes,
// which is what separates this from a plain box blur.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1) uniform sampler2D source_depth;
#ifdef MODE_COLOR
layout(rgba16f, set = 0, binding = 2) uniform restrict writeonly image2D dest;
#else
layout(r8, set = 0, binding = 2) uniform restrict writeonly image2D dest;
#endif

layout(push_constant, std430) uniform Params {
	vec2 screen_size;
	int radius;
	float depth_tolerance; // relative depth difference treated as "same surface"
	// A-trous: taps spread by this many pixels. Running the same small filter
	// twice with an increasing step covers far more area than one wide pass,
	// because cost grows with tap count, not with reach.
	int step_size;
	int pad;
}
params;

void main() {
	ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = ivec2(params.screen_size);
	if (coord.x >= size.x || coord.y >= size.y) {
		return;
	}

	vec2 texel = 1.0 / params.screen_size;
	vec2 uv = (vec2(coord) + 0.5) * texel;

	float center_depth = texture(source_depth, uv).r;
	if (center_depth <= 0.0) {
#ifdef MODE_COLOR
		// Sky contributes no bounced light.
		imageStore(dest, coord, vec4(0.0));
#else
		// Sky is unoccluded.
		imageStore(dest, coord, vec4(1.0));
#endif
		return;
	}

	// Accumulates rgb so one shader serves both the single channel visibility
	// buffers and the colour GI buffer.
	vec4 sum = vec4(0.0);
	float weight_sum = 0.0;

	// Gaussian falloff over the kernel, so distant taps contribute less.
	float sigma = max(float(params.radius) * 0.5, 0.5);
	float inv_two_sigma_sq = 1.0 / (2.0 * sigma * sigma);

	for (int y = -params.radius; y <= params.radius; y++) {
		for (int x = -params.radius; x <= params.radius; x++) {
			vec2 offset = vec2(float(x), float(y)) * float(max(params.step_size, 1)) * texel;
			vec2 tap_uv = uv + offset;
			if (tap_uv.x < 0.0 || tap_uv.y < 0.0 || tap_uv.x > 1.0 || tap_uv.y > 1.0) {
				continue;
			}

			float tap_depth = texture(source_depth, tap_uv).r;
			if (tap_depth <= 0.0) {
				continue;
			}

			// Reverse-Z depth is non-linear, so compare relative difference
			// rather than an absolute epsilon that would only hold at one range.
			float rel = abs(tap_depth - center_depth) / max(center_depth, 1e-6);
			if (rel > params.depth_tolerance) {
				// Different surface; including it would bleed across the edge.
				continue;
			}

			float dist_sq = float(x * x + y * y) * float(max(params.step_size, 1));
			float w = exp(-dist_sq * inv_two_sigma_sq) * (1.0 - rel / params.depth_tolerance);

			sum += texture(source, tap_uv) * w;
			weight_sum += w;
		}
	}

	vec4 result = weight_sum > 0.0 ? (sum / weight_sum) : texture(source, uv);
#ifdef MODE_COLOR
	// Alpha is meaningful for reflections (did the ray hit anything), so it
	// is filtered alongside the colour rather than forced to one.
	imageStore(dest, coord, vec4(max(result.rgb, vec3(0.0)), clamp(result.a, 0.0, 1.0)));
#else
	imageStore(dest, coord, vec4(clamp(result.r, 0.0, 1.0)));
#endif
}
