/**************************************************************************/
/*  asos_rtao.h                                                           */
/**************************************************************************/
/*  Asosdot ray traced ambient occlusion and sun shadows.                 */
/**************************************************************************/

#pragma once

#include "core/math/projection.h"
#include "servers/rendering/renderer_rd/shaders/effects/asos_rt_denoise.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/asos_rt_gi.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/asos_rt_temporal.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/asos_rt_shadow.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/asos_rtao.glsl.gen.h"

// Render buffer slot for the traced sun visibility. AO reuses the existing
// SSAO slot, but shadows need their own since nothing else writes one.
#define RB_SCOPE_ASOS SNAME("rb_asos")
#define RB_RT_SHADOW SNAME("rt_shadow")
#define RB_RT_SHADOW_BLUR SNAME("rt_shadow_blur")
#define RB_RT_AO_RAW SNAME("rt_ao_raw")
#define RB_RT_GI SNAME("rt_gi")
#define RB_RT_GI_RAW SNAME("rt_gi_raw")
#define RB_RT_REFLECTION SNAME("rt_reflection")
#define RB_RT_REFLECTION_RAW SNAME("rt_reflection_raw")
// Temporal accumulation needs somewhere to accumulate into and a copy of
// last frame to blend against.
#define RB_RT_GI_ACC SNAME("rt_gi_acc")
#define RB_RT_GI_HIST SNAME("rt_gi_hist")
#define RB_RT_SHADOW_ACC SNAME("rt_shadow_acc")
#define RB_RT_SHADOW_HIST SNAME("rt_shadow_hist")
#define RB_RT_REFL_ACC SNAME("rt_refl_acc")
#define RB_RT_REFL_HIST SNAME("rt_refl_hist")
#define RB_RT_AO_ACC SNAME("rt_ao_acc")
#define RB_RT_AO_HIST SNAME("rt_ao_hist")

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
	RID denoise_color_pipeline;

	AsosRtGiShaderRD gi_shader;
	RID gi_shader_version;
	RID gi_pipeline;
	RID reflection_pipeline;

	AsosRtTemporalShaderRD temporal_shader;
	RID temporal_shader_version;
	RID temporal_pipeline;
	RID temporal_color_pipeline;

	struct TemporalPushConstant {
		float reproject[16];
		float screen_size[2];
		float blend;
		float pad;
	};

	struct GiPushConstant {
		float inv_view_proj[16];
		float cam_pos[4];
		float sun_dir[4]; // w = sun energy
		float sun_color[4]; // w = gi intensity
		float screen_size[2];
		uint32_t ray_count;
		uint32_t frame;
	};

	static_assert(sizeof(GiPushConstant) <= 128, "Push constant must fit the guaranteed 128 byte minimum.");

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

	// Blends this frame's estimate with the reprojected previous result.
	// p_reproject maps current clip space to the previous frame's.
	void accumulate(RID p_source, RID p_history, RID p_depth_texture, RID p_dest,
			const Size2i &p_size, const Projection &p_reproject, float p_blend, bool p_color = false);

	// Depth aware blur of a traced visibility buffer. Source and destination
	// must differ; a compute pass cannot safely read and write one image.
	void denoise(RID p_source, RID p_depth_texture, RID p_dest, const Size2i &p_size,
			int p_radius, float p_depth_tolerance, bool p_color = false);

	// One bounce diffuse GI. p_instance_buffer holds a flat albedo per TLAS
	// instance so hits can be shaded without bindless material access.
	void render_gi(RID p_tlas, RID p_depth_texture, RID p_instance_buffer, RID p_dest_gi,
			const Size2i &p_size, const Projection &p_inv_view_proj, const Vector3 &p_cam_pos,
			const Vector3 &p_sun_dir, float p_sun_energy, const Color &p_sun_color,
			float p_gi_intensity, uint32_t p_ray_count, uint32_t p_frame,
			bool p_reflection = false);
};

} // namespace RendererRD
