#[compute]

#version 450

#VERSION_DEFINES

// Asosdot temporal accumulation for traced buffers.
//
// A spatial blur can smooth noise but cannot create information, so at a
// handful of rays per pixel it turns fine grain into low frequency blotches.
// The fix is more samples, gathered over time rather than all in one frame:
// each pixel is reprojected into the previous frame's result and blended with
// it, so a still camera converges towards a clean image within a second.
//
// The danger is ghosting -- trailing smears when something the history knew
// about is no longer there. That is handled by clamping the history to the
// range of values actually present around the current pixel, so it can only
// ever refine the current estimate, never contradict it.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1) uniform sampler2D history;
layout(set = 0, binding = 2) uniform sampler2D source_depth;
#ifdef MODE_COLOR
layout(rgba16f, set = 0, binding = 3) uniform restrict writeonly image2D dest;
#else
layout(r8, set = 0, binding = 3) uniform restrict writeonly image2D dest;
#endif

layout(push_constant, std430) uniform Params {
	// Maps this frame's clip space directly to the previous frame's, so the
	// reprojection is a single matrix rather than an unproject and reproject.
	mat4 reproject;
	vec2 screen_size;
	float blend; // weight given to history; higher converges further but reacts slower
	float pad;
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

	vec4 current = texture(source, uv);

	float depth = texture(source_depth, uv).r;
	if (depth <= 0.0) {
		// Sky has no surface to track between frames.
		imageStore(dest, coord, current);
		return;
	}

	vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
	vec4 prev_clip = params.reproject * clip;
	if (prev_clip.w <= 0.0) {
		imageStore(dest, coord, current);
		return;
	}

	vec2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;
	if (prev_uv.x < 0.0 || prev_uv.y < 0.0 || prev_uv.x > 1.0 || prev_uv.y > 1.0) {
		// Was off screen last frame; nothing to accumulate from.
		imageStore(dest, coord, current);
		return;
	}

	// Range of values actually present around this pixel. Clamping history into
	// it is what keeps stale data from smearing across the image.
	vec4 lo = current;
	vec4 hi = current;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			vec4 s = texture(source, uv + vec2(float(x), float(y)) * texel);
			lo = min(lo, s);
			hi = max(hi, s);
		}
	}

	vec4 hist = clamp(texture(history, prev_uv), lo, hi);

	imageStore(dest, coord, mix(current, hist, params.blend));
}
