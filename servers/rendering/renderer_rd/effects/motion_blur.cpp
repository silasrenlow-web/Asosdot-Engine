/**************************************************************************/
/*  motion_blur.cpp                                                       */
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

#include "motion_blur.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

using namespace RendererRD;

namespace {

#define MB_SCOPE SNAME("motion_blur")
#define MB_TEX_PIXELS SNAME("pixels")
#define MB_TEX_GROUP_MAX SNAME("group_max")
#define MB_TEX_TILE_MAX SNAME("tile_max")
#define MB_TEX_NEIGHBOR_MAX SNAME("neighbor_max")

// A tile is made of the 8 by 8 blocks the first pass finds the fastest velocity of.
// Smaller tiles than a block would be work for nothing, and the textures that hold
// them are made for this size, so a change of the longest streak never needs new
// ones.
const int BLOCK_SIZE = 8;
const int MIN_TILE_SIZE = 8;
const int MAX_TILE_SIZE = 64;

} // namespace

MotionBlur::MotionBlur() {
	// The packed pixel is four 32 bit integers and the rest are 16 bit floats, all
	// written by one pass and read by the next.
	const BitField<RD::TextureUsageBits> usage = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	supported = RD::get_singleton()->texture_is_format_supported_for_usage(RD::DATA_FORMAT_R32G32B32A32_UINT, usage) && RD::get_singleton()->texture_is_format_supported_for_usage(RD::DATA_FORMAT_R16G16_SFLOAT, usage);
	if (!supported) {
		WARN_PRINT("Motion blur is not available: this graphics device cannot use the texture formats it needs as storage images.");
		return;
	}

	Vector<String> modes;
	modes.push_back("\n#define MODE_PREPARE\n");
	modes.push_back("\n#define MODE_TILE_MAX\n");
	modes.push_back("\n#define MODE_NEIGHBOR_MAX\n");
	modes.push_back("\n#define MODE_GATHER\n");

	shader.initialize(modes);
	shader_version = shader.version_create();

	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}
}

MotionBlur::~MotionBlur() {
	if (shader_version.is_valid()) {
		shader.version_free(shader_version);
	}
}

void MotionBlur::process(const Parameters &p_parameters) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);
	ERR_FAIL_COND(p_parameters.render_buffers.is_null());

	Ref<RenderSceneBuffersRD> rb = p_parameters.render_buffers;
	const Size2i size = p_parameters.size;
	ERR_FAIL_COND(size.x < 1 || size.y < 1);

	// How long a streak is: the distance a pixel moved in a frame, scaled by how
	// much of the frame a shutter is open. With a target frame rate that is
	// worked out for that rate, so the blur looks the same at 60 and at 144 frames
	// a second, not shorter the faster the game runs.
	const RendererCameraAttributes::MotionBlurSettings &settings = p_parameters.settings;
	float velocity_scale = settings.intensity;
	if (settings.target_fps > 0.0f) {
		velocity_scale /= MAX(settings.target_fps * p_parameters.time_step, 0.0001f);
	}

	float max_length = settings.max_length * 0.01f * float(size.x);
	// A streak reaches half its length each way from a pixel, and the tile has to
	// hold that, so the tile is as big as the half.
	const int tile_size = CLAMP((int(Math::ceil(max_length * 0.5f)) + BLOCK_SIZE - 1) / BLOCK_SIZE * BLOCK_SIZE, MIN_TILE_SIZE, MAX_TILE_SIZE);
	max_length = MIN(max_length, float(tile_size * 2));
	if (max_length < 1.0f) {
		return;
	}
	const Size2i tile_count = Size2i((size.x + tile_size - 1) / tile_size, (size.y + tile_size - 1) / tile_size);

	static const int sample_counts[3] = { 9, 15, 23 };
	const int samples = sample_counts[CLAMP(settings.quality, 0, 2)];

	// Textures for what the passes hand each other, one layer for each eye.
	const uint32_t usage = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	const Size2i max_tiles = Size2i((size.x + MIN_TILE_SIZE - 1) / MIN_TILE_SIZE, (size.y + MIN_TILE_SIZE - 1) / MIN_TILE_SIZE);
	rb->create_texture(MB_SCOPE, MB_TEX_PIXELS, RD::DATA_FORMAT_R32G32B32A32_UINT, usage, RD::TEXTURE_SAMPLES_1, size);
	const Size2i block_count = Size2i((size.x + BLOCK_SIZE - 1) / BLOCK_SIZE, (size.y + BLOCK_SIZE - 1) / BLOCK_SIZE);
	rb->create_texture(MB_SCOPE, MB_TEX_GROUP_MAX, RD::DATA_FORMAT_R16G16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, block_count);
	rb->create_texture(MB_SCOPE, MB_TEX_TILE_MAX, RD::DATA_FORMAT_R16G16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, max_tiles);
	rb->create_texture(MB_SCOPE, MB_TEX_NEIGHBOR_MAX, RD::DATA_FORMAT_R16G16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, max_tiles);
	const RID pixels_image = rb->get_texture_slice(MB_SCOPE, MB_TEX_PIXELS, p_parameters.view, 0);
	const RID group_max_image = rb->get_texture_slice(MB_SCOPE, MB_TEX_GROUP_MAX, p_parameters.view, 0);
	const RID tile_image = rb->get_texture_slice(MB_SCOPE, MB_TEX_TILE_MAX, p_parameters.view, 0);
	const RID neighbor_image = rb->get_texture_slice(MB_SCOPE, MB_TEX_NEIGHBOR_MAX, p_parameters.view, 0);
	ERR_FAIL_COND(pixels_image.is_null() || group_max_image.is_null() || tile_image.is_null() || neighbor_image.is_null());

	PushConstant push_constant;
	memset(&push_constant, 0, sizeof(PushConstant));
	push_constant.size[0] = size.x;
	push_constant.size[1] = size.y;
	push_constant.tile_count[0] = tile_count.x;
	push_constant.tile_count[1] = tile_count.y;
	push_constant.tile_size = tile_size;
	push_constant.samples = samples;
	push_constant.velocity_scale = velocity_scale;
	push_constant.max_length = max_length;
	push_constant.z_near = p_parameters.z_near;
	push_constant.z_far = p_parameters.z_far;
	push_constant.orthogonal = p_parameters.orthogonal ? 1 : 0;
	MaterialStorage::store_camera(p_parameters.reprojection, push_constant.reprojection);

	RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	{
		RENDER_TIMESTAMP("MB prepare");
		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		// Velocity in pixels, from what the scene drew and where the camera turned, with
		// the frame as it was and what the gather needs of each pixel.
		RID shader_rid = shader.version_get_shader(shader_version, MODE_PREPARE);
		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, p_parameters.depth }));
		RD::Uniform u_velocity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, p_parameters.velocity }));
		RD::Uniform u_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, p_parameters.color_dest }));
		RD::Uniform u_pixels(RD::UNIFORM_TYPE_IMAGE, 3, pixels_image);
		RD::Uniform u_group_max(RD::UNIFORM_TYPE_IMAGE, 4, group_max_image);

		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_PREPARE]);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_depth, u_velocity, u_color, u_pixels, u_group_max), 0);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
		RD::get_singleton()->compute_list_end();
	}

	{
		RENDER_TIMESTAMP("MB tile max");
		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		// The fastest velocity in each tile.
		RID shader_rid = shader.version_get_shader(shader_version, MODE_TILE_MAX);
		RD::Uniform u_in(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, group_max_image }));
		RD::Uniform u_out(RD::UNIFORM_TYPE_IMAGE, 1, tile_image);

		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_TILE_MAX]);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_in, u_out), 0);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
		RD::get_singleton()->compute_list_dispatch(compute_list, tile_count.x, tile_count.y, 1);
		RD::get_singleton()->compute_list_end();
	}

	{
		RENDER_TIMESTAMP("MB neighbor max");
		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		// And the fastest around each tile.
		RID shader_rid = shader.version_get_shader(shader_version, MODE_NEIGHBOR_MAX);
		RD::Uniform u_in(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, tile_image }));
		RD::Uniform u_out(RD::UNIFORM_TYPE_IMAGE, 1, neighbor_image);

		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_NEIGHBOR_MAX]);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_in, u_out), 0);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, tile_count.x, tile_count.y, 1);
		RD::get_singleton()->compute_list_end();
	}

	{
		RENDER_TIMESTAMP("MB gather");
		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		RID shader_rid = shader.version_get_shader(shader_version, MODE_GATHER);
		RD::Uniform u_pixels(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, pixels_image }));
		RD::Uniform u_neighbor(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, neighbor_image }));
		RD::Uniform u_out(RD::UNIFORM_TYPE_IMAGE, 2, p_parameters.color_dest);

		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_GATHER]);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_pixels, u_neighbor, u_out), 0);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
		RD::get_singleton()->compute_list_end();
	}
}
