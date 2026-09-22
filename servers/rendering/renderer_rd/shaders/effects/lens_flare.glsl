/* clang-format off */
#[vertex]

#version 450

#VERSION_DEFINES

/* clang-format on */

struct Flare {
	vec4 source; // xyz: world position, or the direction the source lies in. w: 0 for a directional source, 1 otherwise.
	vec4 direction; // xyz: the source's -Z axis in world space. w: cos(angle_limit), or -2 when the limit is off.
	vec4 params; // x: occlusion radius, 0 when occlusion is off. yz: distance fade begin and end. w: screen edge fade.
	vec4 params2; // x: angular fade width. y: overall scale. zw: unused.
};

struct Quad {
	vec4 color; // rgb: the element tint, already folded in with its own intensity, the source's intensity and, when asked for, the source's colour. a: unused.
	vec4 layout_data; // x: position along the axis. yz: size. w: rotation in radians.
	uvec4 flags; // x: flare index. y: shape. z: polygon sides / starburst spikes. w: auto rotate.
	vec4 shape; // x: falloff. y: chromatic aberration. zw: unused.
};

layout(set = 0, binding = 0, std430) restrict readonly buffer Flares {
	Flare data[];
}
flares;

layout(set = 0, binding = 1, std430) restrict readonly buffer Quads {
	Quad data[];
}
quads;

layout(set = 0, binding = 2) uniform sampler2D source_depth;

layout(push_constant, std430) uniform Params {
	mat4 view_projection;
	vec4 camera_right; // xyz: the camera's right vector in world space. w: luminance multiplier.
	vec4 camera_position; // xyz: the camera's world position. w: aspect ratio.
	uint quad_offset;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

layout(location = 0) out vec2 uv_interp;
layout(location = 1) out vec3 color_interp;
layout(location = 2) flat out uvec2 shape_interp; // x: shape, y: sides.
layout(location = 3) out vec2 shape_params_interp; // x: falloff, y: chromatic aberration.

// Sampling the depth buffer on a spiral rather than a grid keeps the tap count
// low without the banding a grid gives on a partly occluded source.
const vec2 occlusion_taps[12] = vec2[](
		vec2(0.0000, 0.0000),
		vec2(0.2887, 0.0000),
		vec2(-0.1433, 0.3564),
		vec2(-0.3536, -0.3536),
		vec2(0.2027, -0.5155),
		vec2(0.5164, 0.3536),
		vec2(-0.4568, 0.5378),
		vec2(-0.6124, -0.4114),
		vec2(0.2399, -0.7391),
		vec2(0.7746, 0.1620),
		vec2(-0.5321, 0.7325),
		vec2(-0.4456, -0.8293));

// Godot's render device uses reverse Z, so a larger stored depth is nearer. A tap
// occludes the source when whatever the scene wrote there sits in front of it.
float sample_occlusion(vec2 p_uv, float p_depth, float p_radius) {
	float visible = 0.0;

	for (int i = 0; i < 12; i++) {
		// Clamped into the frame rather than skipped. A tap that falls outside
		// reads the nearest edge texel, which is the best evidence available for
		// what sits between the lens and a source just off screen. Counting those
		// taps as unoccluded instead, which is what this did, switched occlusion
		// off entirely the moment a source left the frame and let a flare shine
		// straight through a wall.
		vec2 tap_uv = clamp(p_uv + occlusion_taps[i] * p_radius * vec2(1.0, params.camera_position.w), vec2(0.0), vec2(1.0));
		visible += textureLod(source_depth, tap_uv, 0.0).r <= p_depth ? 1.0 : 0.0;
	}

	return visible / 12.0;
}

void main() {
	Quad quad = quads.data[params.quad_offset + uint(gl_InstanceIndex)];
	Flare flare = flares.data[quad.flags.x];

	vec4 clip = params.view_projection * flare.source;

	float fade = clip.w > 0.0 ? 1.0 : 0.0;
	vec2 source_uv = vec2(0.0);

	if (fade > 0.0) {
		vec3 ndc = clip.xyz / clip.w;
		source_uv = ndc.xy * 0.5 + 0.5;

		// Reverse Z puts the near plane at 1 and the far plane at 0, so a source
		// outside that range is behind the camera or past the far plane. The
		// clip.w test above already catches the first case under a perspective
		// projection, but not under an orthogonal one: its w is 1 everywhere, so
		// a source behind an orthogonal camera projects onto the screen as
		// though it were in front. That is what puts flares in the editor's Top
		// and Front views.
		if (flare.source.w > 0.5) {
			fade *= (ndc.z >= 0.0 && ndc.z <= 1.0) ? 1.0 : 0.0;
		}

		// A directional source sits at infinity, where the projected depth is the
		// far plane and a depth compare is all floating point noise. Anything the
		// scene drew at all is in front of it.
		float source_depth = flare.source.w > 0.5 ? ndc.z : 0.0;

		if (fade > 0.0 && flare.params.x > 0.0) {
			// Offsetting along the camera's right vector turns a radius given in
			// world units into one in UV, and the same expression covers a
			// directional source, where the offset is the tangent of an angle
			// because the source itself is a direction of unit length.
			vec4 probe = params.view_projection * vec4(flare.source.xyz + params.camera_right.xyz * flare.params.x, flare.source.w);
			float radius = probe.w > 0.0 ? distance(probe.xy / probe.w * 0.5 + 0.5, source_uv) : 0.0;
			fade *= sample_occlusion(source_uv, source_depth, radius);
		}

		// Screen edge fade, measured on the axis the source is furthest out along
		// so that it tracks the rectangle rather than a circle inscribed in it.
		// The floor keeps the two smoothstep edges apart: equal edges are
		// undefined in GLSL, and a zero width fade is a legitimate setting.
		float edge = max(flare.params.w, 0.0001);
		fade *= 1.0 - smoothstep(1.0 - edge, 1.0 + edge, max(abs(ndc.x), abs(ndc.y)));

		if (flare.source.w > 0.5) {
			// Distance fade, on the view depth the projection already worked out.
			if (flare.params.z > flare.params.y + 0.0001) {
				fade *= 1.0 - smoothstep(flare.params.y, flare.params.z, clip.w);
			}

			// Angular fade, for a source that only throws a flare within a cone
			// around its own -Z, the way a SpotLight3D only lights within one.
			if (flare.direction.w > -1.0) {
				float outer = flare.direction.w;
				float inner = max(mix(outer, 1.0, flare.params2.x), outer + 0.0001);
				float facing = dot(normalize(params.camera_position.xyz - flare.source.xyz), flare.direction.xyz);
				fade *= smoothstep(outer, inner, facing);
			}
		}
	}

	if (fade <= 0.0) {
		// Collapsing the quad outside clip space is cheaper than a second pass
		// over the list on the CPU to work out which quads survive their fades.
		gl_Position = vec4(-2.0, -2.0, 0.5, 1.0);
		uv_interp = vec2(0.0);
		color_interp = vec3(0.0);
		shape_interp = uvec2(0u);
		shape_params_interp = vec2(0.0);
		return;
	}

	// The axis every element is strung along: from the source through the centre
	// of the screen and out the far side.
	vec2 to_centre = vec2(0.5) - source_uv;
	vec2 centre = source_uv + to_centre * quad.layout_data.x;

	float rotation = quad.layout_data.w;
	if (quad.flags.w != 0u) {
		// Aspect corrected, or the axis an element lines up with is not the axis
		// the eye sees it lined up with.
		rotation += atan(to_centre.y, to_centre.x * params.camera_position.w);
	}

	// Sized before it is turned. Scaling a rotated quad by two different factors
	// shears it, which turns a rotated streak into a parallelogram.
	vec2 corner = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1)) - 0.5;
	vec2 scaled = corner * quad.layout_data.yz * flare.params2.y;

	float sin_r = sin(rotation);
	float cos_r = cos(rotation);
	vec2 rotated = vec2(scaled.x * cos_r - scaled.y * sin_r, scaled.x * sin_r + scaled.y * cos_r);

	// Sizes are in fractions of the screen height on both axes, so a round
	// element stays round. Dividing x by the aspect is the change of basis from
	// that square space into UV.
	vec2 uv = centre + vec2(rotated.x / params.camera_position.w, rotated.y);

	gl_Position = vec4(uv * 2.0 - 1.0, 0.5, 1.0);

	uv_interp = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
	color_interp = quad.color.rgb * fade * params.camera_right.w;
	shape_interp = uvec2(quad.flags.y, quad.flags.z);
	shape_params_interp = quad.shape.xy;
}

/* clang-format off */
#[fragment]

#version 450

#VERSION_DEFINES

#define M_PI 3.14159265359

#define SHAPE_GLOW 0u
#define SHAPE_DISC 1u
#define SHAPE_RING 2u
#define SHAPE_POLYGON 3u
#define SHAPE_STREAK 4u
#define SHAPE_STARBURST 5u
#define SHAPE_TEXTURE 6u

/* clang-format on */

layout(set = 0, binding = 3) uniform sampler2D flare_texture;

layout(location = 0) in vec2 uv_interp;
layout(location = 1) in vec3 color_interp;
layout(location = 2) flat in uvec2 shape_interp;
layout(location = 3) in vec2 shape_params_interp;

layout(location = 0) out vec4 frag_color;

// Distance from the centre to a point, in units where the polygon's
// circumscribed circle has radius 1, so the shape fits inside its quad rather
// than having its corners clipped off.
float polygon_distance(vec2 p_point, uint p_sides) {
	float segment = 2.0 * M_PI / float(p_sides);
	float angle = atan(p_point.y, p_point.x);
	float inradius = cos(M_PI / float(p_sides));
	return cos(floor(0.5 + angle / segment) * segment - angle) * length(p_point) / inradius;
}

// rgb is the element's own colour, a its coverage. Only a texture carries a
// colour of its own; the analytic shapes are white and take their colour from
// the element and the source.
vec4 shape_sample(vec2 p_uv, uint p_shape, uint p_sides, float p_falloff) {
	if (p_shape == SHAPE_TEXTURE) {
		return texture(flare_texture, p_uv);
	}

	// Evaluated on [-1, 1], which makes every analytic shape resolution
	// independent and costs no texture memory.
	vec2 point = p_uv * 2.0 - 1.0;
	float radius = length(point);
	float alpha = 0.0;

	switch (p_shape) {
		case SHAPE_DISC: {
			float edge = clamp(p_falloff, 0.002, 1.0);
			alpha = 1.0 - smoothstep(1.0 - edge, 1.0, radius);
		} break;
		case SHAPE_RING: {
			float width = clamp(p_falloff, 0.01, 0.5);
			alpha = smoothstep(1.0 - 2.0 * width, 1.0 - width, radius) * (1.0 - smoothstep(1.0 - width, 1.0, radius));
		} break;
		case SHAPE_POLYGON: {
			float edge = clamp(p_falloff, 0.002, 1.0);
			alpha = 1.0 - smoothstep(1.0 - edge, 1.0, polygon_distance(point, p_sides));
		} break;
		case SHAPE_STREAK: {
			float along = max(0.0, 1.0 - abs(point.x));
			float across = exp(-point.y * point.y * (16.0 * p_falloff + 4.0));
			alpha = along * along * across;
		} break;
		case SHAPE_STARBURST: {
			float angle = atan(point.y, point.x);
			float spikes = abs(cos(angle * float(p_sides) * 0.5));
			float radial = max(0.0, 1.0 - radius);
			// The cubed term is the soft core the spikes radiate out of.
			alpha = radial * pow(spikes, 4.0 + 28.0 * p_falloff) + radial * radial * radial * 0.25;
		} break;
		default: {
			// SHAPE_GLOW.
			alpha = pow(max(0.0, 1.0 - radius), 1.0 + p_falloff * 6.0);
		} break;
	}

	return vec4(1.0, 1.0, 1.0, alpha);
}

void main() {
	uint shape = shape_interp.x;
	uint sides = max(shape_interp.y, 3u);
	float falloff = shape_params_interp.x;
	float aberration = shape_params_interp.y;

	vec3 shaded;
	if (aberration > 0.0) {
		// Spreading the channels radially about the element's own centre is what
		// gives a ghost its coloured fringe.
		vec2 centred = uv_interp - 0.5;
		vec4 red = shape_sample(centred / (1.0 + aberration) + 0.5, shape, sides, falloff);
		vec4 green = shape_sample(uv_interp, shape, sides, falloff);
		vec4 blue = shape_sample(centred / max(1.0 - aberration, 0.05) + 0.5, shape, sides, falloff);
		shaded = vec3(red.r * red.a, green.g * green.a, blue.b * blue.a);
	} else {
		vec4 sample_value = shape_sample(uv_interp, shape, sides, falloff);
		shaded = sample_value.rgb * sample_value.a;
	}

	// Premultiplied: the blend states add or screen the colour and leave the
	// destination alpha alone, so a transparent background stays transparent.
	frag_color = vec4(color_interp * shaded, 1.0);
}
