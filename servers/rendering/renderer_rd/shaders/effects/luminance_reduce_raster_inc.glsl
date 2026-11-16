layout(push_constant, std430) uniform PushConstant {
	ivec2 source_size;
	ivec2 dest_size;

	float exposure_adjust;
	float min_luminance;
	float max_luminance;
	float dark_speed;
	float center_weight;
	float center_norm;
	uint pad1;
}
settings;
