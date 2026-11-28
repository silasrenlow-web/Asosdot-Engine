#[compute]

#version 450

#VERSION_DEFINES

// Gaze focus: what the player looks at stays sharp, the rest of the view goes
// soft, the way it does for an eye. In four kinds of pass:
//
//   FOCUS       One group of threads reads the depth around the gaze point and
//               moves the focus distance towards it, a little every frame, so
//               the eye takes a moment to refocus on something nearer or further.
//   DOWNSAMPLE  The frame at a quarter of its size, each pixel the average of
//               the 4 by 4 it covers.
//   BLUR        A 9 tap Gaussian at that size, across then down.
//   COMPOSITE   Mixes the blurred frame into the sharp one by how far a pixel is
//               from the gaze on the screen and, with a depth range, by how far
//               it is in front of or behind the focus distance.

#define GROUP_SIZE 8

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	ivec2 size; // Of the image the pass writes.
	vec2 gaze_uv;
	float radius; // Sharp area, in screen heights.
	float softness; // How far beyond it the blur fades in.
	float amount;
	float depth_range; // Metres; 0 leaves depth out.
	float aspect; // Width over height.
	float z_near;
	float z_far;
	uint orthogonal;
	float focus_blend; // How much of the new focus distance to take this frame.
	uint reset;
	vec2 blur_step; // One texel across or down, in UV.
}
params;

// Meters from the camera, as motion blur works it out.
float linear_depth(float p_depth) {
	float depth = p_depth * 2.0 - 1.0;
	float meters;
	if (params.orthogonal != 0u) {
		meters = -(depth * (params.z_far - params.z_near) - (params.z_far + params.z_near)) / 2.0;
	} else {
		meters = 2.0 * params.z_near * params.z_far / (params.z_far + params.z_near + depth * (params.z_far - params.z_near));
	}
	return clamp(meters, 0.0, params.z_far);
}

#if defined(MODE_FOCUS)

layout(set = 0, binding = 0) uniform sampler2D source_depth;
layout(r32f, set = 0, binding = 1) uniform restrict image2D focus_image;

shared float group_depth[GROUP_SIZE * GROUP_SIZE];

void main() {
	// An 8 by 8 grid of depth samples over a small patch around the gaze, about
	// what the middle of the eye takes in.
	ivec2 depth_size = textureSize(source_depth, 0);
	vec2 offset = (vec2(gl_LocalInvocationID.xy) - 3.5) / 3.5 * vec2(0.02 / params.aspect, 0.02);
	vec2 uv = clamp(params.gaze_uv + offset, vec2(0.0), vec2(1.0));
	ivec2 pos = clamp(ivec2(uv * vec2(depth_size)), ivec2(0), depth_size - 1);
	float depth = texelFetch(source_depth, pos, 0).r;
	// Reverse Z: 0 is nothing drawn, the sky, as far as anything gets.
	group_depth[gl_LocalInvocationIndex] = depth <= 0.0 ? params.z_far : linear_depth(depth);
	barrier();

	for (uint stride = GROUP_SIZE * GROUP_SIZE / 2; stride > 0; stride >>= 1) {
		if (gl_LocalInvocationIndex < stride) {
			group_depth[gl_LocalInvocationIndex] += group_depth[gl_LocalInvocationIndex + stride];
		}
		barrier();
	}

	if (gl_LocalInvocationIndex == 0) {
		float target = group_depth[0] / float(GROUP_SIZE * GROUP_SIZE);
		float focus = imageLoad(focus_image, ivec2(0)).r;
		if (params.reset != 0u || !(focus > 0.0) || isinf(focus)) {
			focus = target;
		} else {
			focus = mix(focus, target, params.focus_blend);
		}
		imageStore(focus_image, ivec2(0), vec4(focus, 0.0, 0.0, 0.0));
	}
}

#elif defined(MODE_DOWNSAMPLE)

layout(set = 0, binding = 0) uniform sampler2D source_color;
layout(rgba16f, set = 0, binding = 1) uniform restrict writeonly image2D dest_image;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.size))) {
		return;
	}
	// Four bilinear reads at the middles of the 2 by 2 blocks of a 4 by 4.
	vec2 texel = 1.0 / vec2(textureSize(source_color, 0));
	vec2 centre = (vec2(pos) + 0.5) / vec2(params.size);
	vec4 color = textureLod(source_color, centre + vec2(-1.0, -1.0) * texel, 0.0);
	color += textureLod(source_color, centre + vec2(1.0, -1.0) * texel, 0.0);
	color += textureLod(source_color, centre + vec2(-1.0, 1.0) * texel, 0.0);
	color += textureLod(source_color, centre + vec2(1.0, 1.0) * texel, 0.0);
	imageStore(dest_image, pos, color * 0.25);
}

#elif defined(MODE_BLUR)

layout(set = 0, binding = 0) uniform sampler2D source_color;
layout(rgba16f, set = 0, binding = 1) uniform restrict writeonly image2D dest_image;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.size))) {
		return;
	}
	const float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
	vec2 uv = (vec2(pos) + 0.5) / vec2(params.size);
	// Wider apart for more blur; still smooth at a quarter of the size.
	vec2 step = params.blur_step * mix(1.0, 2.0, params.amount);
	vec4 color = textureLod(source_color, uv, 0.0) * weights[0];
	for (int i = 1; i < 5; i++) {
		color += textureLod(source_color, uv + step * float(i), 0.0) * weights[i];
		color += textureLod(source_color, uv - step * float(i), 0.0) * weights[i];
	}
	imageStore(dest_image, pos, color);
}

#elif defined(MODE_COMPOSITE)

layout(set = 0, binding = 0) uniform sampler2D blurred_color;
layout(set = 0, binding = 1) uniform sampler2D source_depth;
layout(r32f, set = 0, binding = 2) uniform restrict readonly image2D focus_image;
layout(rgba16f, set = 0, binding = 3) uniform restrict image2D color_image;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.size))) {
		return;
	}
	vec2 uv = (vec2(pos) + 0.5) / vec2(params.size);

	// On the screen: sharp inside the radius, fading to blurred past it.
	float distance_from_gaze = length((uv - params.gaze_uv) * vec2(params.aspect, 1.0));
	float blur = smoothstep(params.radius, params.radius + params.softness, distance_from_gaze);

	if (params.depth_range > 0.0) {
		// In depth: out of focus by how far in front of or behind the looked-at
		// distance a pixel is.
		float focus = imageLoad(focus_image, ivec2(0)).r;
		float depth = texelFetch(source_depth, pos, 0).r;
		float meters = depth <= 0.0 ? params.z_far : linear_depth(depth);
		float depth_blur = clamp(abs(meters - focus) / params.depth_range, 0.0, 1.0);
		blur = 1.0 - (1.0 - blur) * (1.0 - depth_blur);
	}

	blur *= params.amount;
	if (blur <= 0.001) {
		return;
	}
	vec4 color = imageLoad(color_image, pos);
	vec4 soft = textureLod(blurred_color, uv, 0.0);
	imageStore(color_image, pos, vec4(mix(color.rgb, soft.rgb, blur), color.a));
}

#endif
