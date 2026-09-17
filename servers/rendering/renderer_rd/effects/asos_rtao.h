/**************************************************************************/
/*  asos_rtao.h                                                           */
/**************************************************************************/
/*  Asosdot ray traced ambient occlusion and sun shadows.                 */
/**************************************************************************/

#pragma once

#include "core/math/projection.h"
#include "servers/rendering/renderer_rd/shaders/effects/asos_rt_denoise.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/asos_rt_shadow.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/asos_rtao.glsl.gen.h"

// Render buffer slot for the traced sun visibility. AO reuses the existing
// SSAO slot, but shadows need their own since nothing else writes one.
#define RB_SCOPE_ASOS SNAME("rb_asos")
#define RB_RT_SHADOW SNAME("rt_shadow")
#define RB_RT_SHADOW_BLUR SNAME("rt_shadow_blur")
#define RB_RT_AO_RAW SNAME("rt_ao_raw")

namespace RendererRD {

// Traces ambient occlusion and sun visibility against the scene acceleration
// structure. AO is written into the buffer screen space AO would have used, so
// the lighting consumes it unchanged; sun visibility gets its own buffer and is
// multiplied into the directional shadow term.
class AsosRTAO {
private:
	struct PushConstant {
		float inv_view_proj[16];
		float cam_pos[4];
		float screen_size[2];
		float radius;
		float intensity;
		uint32_t ray_count;
		uint32_t frame;
		float pad0;
		float pad1;
	};

	struct ShadowPushConstant {
		float inv_view_proj[16];
		float cam_pos[4];
		float sun[4]; // xyz = direction toward the sun, w = angular radius (radians)
		float screen_size[2];
		uint32_t ray_count;
		uint32_t frame;
	};

	static_assert(sizeof(PushConstant) <= 128, "Push constant must fit the guaranteed 128 byte minimum.");
	static_assert(sizeof(ShadowPushConstant) <= 128, "Push constant must fit the guaranteed 128 byte minimum.");

	AsosRtaoShaderRD shader;
	RID shader_version;
	RID pipeline;

	AsosRtShadowShaderRD shadow_shader;
	RID shadow_shader_version;
	RID shadow_pipeline;

	AsosRtDenoiseShaderRD denoise_shader;
	RID denoise_shader_version;
	RID denoise_pipeline;

	struct DenoisePushConstant {
		float screen_size[2];
		int32_t radius;
		float depth_tolerance;
	};

	void _dispatch(RID p_pipeline, RID p_shader, RID p_tlas, RID p_depth_texture, RID p_dest,
			const Size2i &p_size, const void *p_push_constant, uint32_t p_push_constant_size);

public:
	AsosRTAO();
	~AsosRTAO();

	// p_inv_view_proj maps NDC back to world space; p_depth_texture is the
	// scene depth buffer and p_dest_ao the R8 target the lighting samples.
	void render(RID p_tlas, RID p_depth_texture, RID p_dest_ao, const Size2i &p_size,
			const Projection &p_inv_view_proj, const Vector3 &p_cam_pos,
			float p_radius, float p_intensity, uint32_t p_ray_count, uint32_t p_frame);

	// p_sun_dir points toward the sun. p_sun_angular_radius in radians drives
	// the penumbra; zero gives hard shadows.
	void render_shadow(RID p_tlas, RID p_depth_texture, RID p_dest_shadow, const Size2i &p_size,
			const Projection &p_inv_view_proj, const Vector3 &p_cam_pos,
			const Vector3 &p_sun_dir, float p_sun_angular_radius,
			uint32_t p_ray_count, uint32_t p_frame);

	// Depth aware blur of a traced visibility buffer. Source and destination
	// must differ; a compute pass cannot safely read and write one image.
	void denoise(RID p_source, RID p_depth_texture, RID p_dest, const Size2i &p_size,
			int p_radius, float p_depth_tolerance);
};

} // namespace RendererRD
