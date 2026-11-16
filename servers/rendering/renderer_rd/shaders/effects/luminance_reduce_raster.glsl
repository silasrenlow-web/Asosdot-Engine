/* clang-format off */
#[vertex]

#version 450

#VERSION_DEFINES

#include "luminance_reduce_raster_inc.glsl"

layout(location = 0) out vec2 uv_interp;
/* clang-format on */

void main() {
	vec2 base_arr[3] = vec2[](vec2(-1.0, -1.0), vec2(-1.0, 3.0), vec2(3.0, -1.0));
	gl_Position = vec4(base_arr[gl_VertexIndex], 0.0, 1.0);
	uv_interp = clamp(gl_Position.xy, vec2(0.0, 0.0), vec2(1.0, 1.0)) * 2.0; // saturate(x) * 2.0
}

/* clang-format off */
#[fragment]

#version 450

#VERSION_DEFINES

#include "luminance_reduce_raster_inc.glsl"

layout(location = 0) in vec2 uv_interp;
/* clang-format on */

#define CENTER_WEIGHT_RADIUS 0.3 // Matches Luminance::CENTER_WEIGHT_RADIUS.

layout(set = 0, binding = 0) uniform sampler2D source_exposure;

#ifdef FINAL_PASS
layout(set = 1, binding = 0) uniform sampler2D prev_luminance;
#endif

layout(location = 0) out highp float luminance;

void main() {
	ivec2 dest_pos = ivec2(uv_interp * settings.dest_size);
	ivec2 src_pos = ivec2(uv_interp * settings.source_size);

	ivec2 next_pos = (dest_pos + ivec2(1)) * settings.source_size / settings.dest_size;
	next_pos = max(next_pos, src_pos + ivec2(1)); //so it at least reads one pixel

	highp vec3 source_color = vec3(0.0);
	for (int i = src_pos.x; i < next_pos.x; i++) {
		for (int j = src_pos.y; j < next_pos.y; j++) {
			source_color += texelFetch(source_exposure, ivec2(i, j), 0).rgb;
		}
	}

	source_color /= float((next_pos.x - src_pos.x) * (next_pos.y - src_pos.y));

#ifdef FIRST_PASS
	luminance = max(source_color.r, max(source_color.g, source_color.b));

	// Weight the middle of the screen more; the weights average to 1.
	vec2 center_offset = vec2(src_pos + next_pos) * 0.5 / vec2(settings.source_size) - 0.5;
	float center_falloff = exp(-dot(center_offset, center_offset) / (CENTER_WEIGHT_RADIUS * CENTER_WEIGHT_RADIUS));
	luminance *= (1.0 + settings.center_weight * center_falloff) * settings.center_norm;

	// This formula should be more "accurate" but gave an overexposed result when testing.
	// Leaving it here so we can revisit it if we want.
	// luminance = source_color.r * 0.21 + source_color.g * 0.71 + source_color.b * 0.07;
#else
	luminance = source_color.r;
#endif

#ifdef FINAL_PASS
	// Obtain our target luminance
	luminance = clamp(luminance, settings.min_luminance, settings.max_luminance);

	// Now smooth to our transition
	highp float prev_lum = texelFetch(prev_luminance, ivec2(0, 0), 0).r; //1 pixel previous luminance
	// The eye gets used to the dark more slowly than to the light.
	highp float adjust = settings.exposure_adjust * (luminance < prev_lum ? settings.dark_speed : 1.0);
	luminance = prev_lum + (luminance - prev_lum) * clamp(adjust, 0.0, 1.0);
#endif
}
