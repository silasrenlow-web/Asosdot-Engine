/**************************************************************************/
/*  luminance.h                                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "servers/rendering/renderer_rd/shaders/effects/luminance_reduce.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/luminance_reduce_raster.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

#define RB_LUMINANCE_BUFFERS SNAME("luminance_buffers")

namespace RendererRD {

class Luminance {
private:
	bool prefer_raster_effects;

	enum LuminanceReduceMode {
		LUMINANCE_REDUCE_READ,
		LUMINANCE_REDUCE,
		LUMINANCE_REDUCE_WRITE,
		LUMINANCE_REDUCE_MAX
	};

	struct LuminanceReducePushConstant {
		int32_t source_size[2];
		float max_luminance;
		float min_luminance;
		float exposure_adjust;
		float dark_speed;
		float center_weight;
		float center_norm;
	};

	struct LuminanceReduce {
		LuminanceReduceShaderRD shader;
		RID shader_version;
		RID pipelines[LUMINANCE_REDUCE_MAX];
	} luminance_reduce;

	enum LuminanceReduceRasterMode {
		LUMINANCE_REDUCE_FRAGMENT_FIRST,
		LUMINANCE_REDUCE_FRAGMENT,
		LUMINANCE_REDUCE_FRAGMENT_FINAL,
		LUMINANCE_REDUCE_FRAGMENT_MAX
	};

	struct LuminanceReduceRasterPushConstant {
		int32_t source_size[2];
		int32_t dest_size[2];
		float exposure_adjust;
		float min_luminance;
		float max_luminance;
		float dark_speed;
		float center_weight;
		float center_norm;
		uint32_t pad1;
	};

	struct LuminanceReduceFragment {
		LuminanceReduceRasterShaderRD shader;
		RID shader_version;
		PipelineCacheRD pipelines[LUMINANCE_REDUCE_FRAGMENT_MAX];
	} luminance_reduce_raster;

	// Asosdot: how the middle of the screen is weighted in the exposure measurement. A pixel at
	// distance r from the center counts (1 + weight * exp(-r^2 / radius^2)) / (1 + weight * mean),
	// so the weights average to 1 over the screen and the measurement stays an average.
	static constexpr float CENTER_WEIGHT_RADIUS = 0.3f;
	static float get_center_weight_norm(float p_center_weight);

public:
	class LuminanceBuffers : public RenderBufferCustomDataRD {
		GDCLASS(LuminanceBuffers, RenderBufferCustomDataRD);

	private:
		bool prefer_raster_effects;

	public:
		Vector<RID> reduce;
		RID current;

		virtual void configure(RenderSceneBuffersRD *p_render_buffers) override;
		virtual void free_data() override;

		void set_prefer_raster_effects(bool p_prefer_raster_effects);
	};

	Ref<LuminanceBuffers> get_luminance_buffers(Ref<RenderSceneBuffersRD> p_render_buffers);
	RID get_current_luminance_buffer(Ref<RenderSceneBuffersRD> p_render_buffers);
	void luminance_reduction(RID p_source_texture, const Size2i p_source_size, Ref<LuminanceBuffers> p_luminance_buffers, float p_min_luminance, float p_max_luminance, float p_adjust, bool p_set = false, float p_dark_speed = 1.0, float p_center_weight = 0.0);

	Luminance(bool p_prefer_raster_effects);
	~Luminance();
};

} // namespace RendererRD
