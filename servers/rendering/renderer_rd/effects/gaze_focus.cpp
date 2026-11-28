/**************************************************************************/
/*  gaze_focus.cpp                                                        */
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

#include "gaze_focus.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

using namespace RendererRD;

namespace {

#define GF_SCOPE SNAME("gaze_focus")
#define GF_TEX_BLUR_A SNAME("blur_a")
#define GF_TEX_BLUR_B SNAME("blur_b")
#define GF_TEX_FOCUS SNAME("focus")

} // namespace

GazeFocus::GazeFocus() {
	const BitField<RD::TextureUsageBits> usage = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	supported = RD::get_singleton()->texture_is_format_supported_for_usage(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage) && RD::get_singleton()->texture_is_format_supported_for_usage(RD::DATA_FORMAT_R32_SFLOAT, usage);
	if (!supported) {
		WARN_PRINT("Gaze focus is not available: this graphics device cannot use the texture formats it needs as storage images.");
		return;
	}

	Vector<String> modes;
	modes.push_back("\n#define MODE_FOCUS\n");
	modes.push_back("\n#define MODE_DOWNSAMPLE\n");
	modes.push_back("\n#define MODE_BLUR\n");
	modes.push_back("\n#define MODE_COMPOSITE\n");

	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}
}

GazeFocus::~GazeFocus() {
	if (shader_version.is_valid()) {
		shader.version_free(shader_version);
	}
}

void GazeFocus::process(const Parameters &p_parameters) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);
	ERR_FAIL_COND(p_parameters.render_buffers.is_null());

	Ref<RenderSceneBuffersRD> rb = p_parameters.render_buffers;
	const Size2i size = p_parameters.size;
	ERR_FAIL_COND(size.x < 4 || size.y < 4);
	const Size2i small = Size2i(MAX(size.x / 4, 1), MAX(size.y / 4, 1));
	const RendererCameraAttributes::GazeFocusSettings &settings = p_parameters.settings;

	// The focus distance is kept from frame to frame; when its texture is new
	// there is nothing to ease from.
	const bool reset = !rb->has_texture(GF_SCOPE, GF_TEX_FOCUS);
	const uint32_t usage = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	rb->create_texture(GF_SCOPE, GF_TEX_BLUR_A, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, small);
	rb->create_texture(GF_SCOPE, GF_TEX_BLUR_B, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, small);
	rb->create_texture(GF_SCOPE, GF_TEX_FOCUS, RD::DATA_FORMAT_R32_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, Size2i(1, 1));
	const RID blur_a = rb->get_texture_slice(GF_SCOPE, GF_TEX_BLUR_A, p_parameters.view, 0);
	const RID blur_b = rb->get_texture_slice(GF_SCOPE, GF_TEX_BLUR_B, p_parameters.view, 0);
	const RID focus = rb->get_texture_slice(GF_SCOPE, GF_TEX_FOCUS, p_parameters.view, 0);
	ERR_FAIL_COND(blur_a.is_null() || blur_b.is_null() || focus.is_null());

	PushConstant push_constant;
	memset(&push_constant, 0, sizeof(PushConstant));
	push_constant.gaze_uv[0] = CLAMP(p_parameters.gaze_uv.x, -1.0f, 2.0f);
	push_constant.gaze_uv[1] = CLAMP(p_parameters.gaze_uv.y, -1.0f, 2.0f);
	push_constant.radius = settings.radius;
	push_constant.softness = MAX(settings.softness, 0.001f);
	push_constant.amount = settings.amount;
	push_constant.depth_range = settings.depth_range;
	push_constant.aspect = float(size.x) / float(size.y);
	push_constant.z_near = p_parameters.z_near;
	push_constant.z_far = p_parameters.z_far;
	push_constant.orthogonal = p_parameters.orthogonal ? 1 : 0;
	// Eased by the time the frame took, so refocusing takes as long at any frame rate.
	push_constant.focus_blend = settings.adapt_speed > 0.0f ? 1.0f - Math::exp(-settings.adapt_speed * p_parameters.time_step) : 1.0f;
	push_constant.reset = reset ? 1 : 0;

	RID nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID linear = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RD *rd = RD::get_singleton();

	if (settings.depth_range > 0.0f) {
		RENDER_TIMESTAMP("GF focus");
		RD::ComputeListID compute_list = rd->compute_list_begin();
		RID shader_rid = shader.version_get_shader(shader_version, MODE_FOCUS);
		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, p_parameters.depth }));
		RD::Uniform u_focus(RD::UNIFORM_TYPE_IMAGE, 1, focus);
		rd->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_FOCUS]);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_depth, u_focus), 0);
		push_constant.size[0] = 1;
		push_constant.size[1] = 1;
		rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
		rd->compute_list_dispatch(compute_list, 1, 1, 1);
		rd->compute_list_end();
	}

	push_constant.size[0] = small.x;
	push_constant.size[1] = small.y;
	{
		RENDER_TIMESTAMP("GF downsample");
		RD::ComputeListID compute_list = rd->compute_list_begin();
		RID shader_rid = shader.version_get_shader(shader_version, MODE_DOWNSAMPLE);
		RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ linear, p_parameters.color }));
		RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 1, blur_a);
		rd->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_DOWNSAMPLE]);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_source, u_dest), 0);
		rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
		rd->compute_list_dispatch_threads(compute_list, small.x, small.y, 1);
		rd->compute_list_end();
	}

	{
		RENDER_TIMESTAMP("GF blur");
		RID shader_rid = shader.version_get_shader(shader_version, MODE_BLUR);
		// Across, then down.
		for (int pass = 0; pass < 2; pass++) {
			RD::ComputeListID compute_list = rd->compute_list_begin();
			const RID from = pass == 0 ? blur_a : blur_b;
			const RID to = pass == 0 ? blur_b : blur_a;
			push_constant.blur_step[0] = pass == 0 ? 1.0f / small.x : 0.0f;
			push_constant.blur_step[1] = pass == 0 ? 0.0f : 1.0f / small.y;
			RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ linear, from }));
			RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 1, to);
			rd->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_BLUR]);
			rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_source, u_dest), 0);
			rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
			rd->compute_list_dispatch_threads(compute_list, small.x, small.y, 1);
			rd->compute_list_end();
		}
	}

	push_constant.size[0] = size.x;
	push_constant.size[1] = size.y;
	{
		RENDER_TIMESTAMP("GF composite");
		RD::ComputeListID compute_list = rd->compute_list_begin();
		RID shader_rid = shader.version_get_shader(shader_version, MODE_COMPOSITE);
		RD::Uniform u_blurred(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ linear, blur_a }));
		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest, p_parameters.depth }));
		RD::Uniform u_focus(RD::UNIFORM_TYPE_IMAGE, 2, focus);
		RD::Uniform u_color(RD::UNIFORM_TYPE_IMAGE, 3, p_parameters.color);
		rd->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_COMPOSITE]);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_blurred, u_depth, u_focus, u_color), 0);
		rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
		rd->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
		rd->compute_list_end();
	}
}
