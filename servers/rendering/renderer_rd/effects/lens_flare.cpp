/**************************************************************************/
/*  lens_flare.cpp                                                        */
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

#include "lens_flare.h"

#include "core/io/image.h"
#include "servers/rendering/renderer_rd/effects/lens_flare_textures.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"

using namespace RendererRD;

LensFlare::LensFlare() {
	Vector<String> modes;
	modes.push_back("");

	shader.initialize(modes);
	shader_version = shader.version_create();

	RID version_shader = shader.version_get_shader(shader_version, 0);

	// Premultiplied throughout. The alpha factors leave the destination alpha
	// untouched so drawing a flare over a transparent background does not quietly
	// make it opaque.
	RD::PipelineColorBlendState::Attachment attachment;
	attachment.enable_blend = true;
	attachment.src_color_blend_factor = RD::BLEND_FACTOR_ONE;
	attachment.dst_color_blend_factor = RD::BLEND_FACTOR_ONE;
	attachment.src_alpha_blend_factor = RD::BLEND_FACTOR_ZERO;
	attachment.dst_alpha_blend_factor = RD::BLEND_FACTOR_ONE;

	RD::PipelineColorBlendState blend_add;
	blend_add.attachments.push_back(attachment);

	attachment.dst_color_blend_factor = RD::BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	RD::PipelineColorBlendState blend_screen;
	blend_screen.attachments.push_back(attachment);

	pipelines[PIPELINE_ADD].setup(version_shader, RD::RENDER_PRIMITIVE_TRIANGLE_STRIPS, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), blend_add, 0);
	pipelines[PIPELINE_SCREEN].setup(version_shader, RD::RENDER_PRIMITIVE_TRIANGLE_STRIPS, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), blend_screen, 0);

	flare_buffer = RD::get_singleton()->storage_buffer_create(sizeof(FlareData) * MAX_FLARES);
	quad_buffer = RD::get_singleton()->storage_buffer_create(sizeof(QuadData) * MAX_QUADS);
	exception_buffer = RD::get_singleton()->storage_buffer_create(sizeof(float) * 4 * (4 + 2 * MAX_EXCEPTION_BOXES));

	_create_builtin_textures();
}

void LensFlare::_create_builtin_textures() {
	// The bundled flares are 8 bit greyscale where the single channel is
	// coverage. Uploading them as R8 and swizzling to (r, r, r, 1) keeps them at
	// a quarter of the memory an RGBA copy would take and lands on exactly what
	// the shader's texture path expects.
	struct BuiltinTexture {
		const unsigned char *png;
		uint32_t size;
	};

	const BuiltinTexture sources[RSE::LENS_FLARE_ELEMENT_BUILTIN_COUNT] = {
		{ lens_flare_corona_png, uint32_t(sizeof(lens_flare_corona_png)) },
		{ lens_flare_nova_png, uint32_t(sizeof(lens_flare_nova_png)) },
		{ lens_flare_star_png, uint32_t(sizeof(lens_flare_star_png)) },
		{ lens_flare_iris_png, uint32_t(sizeof(lens_flare_iris_png)) },
		{ lens_flare_hexangle_png, uint32_t(sizeof(lens_flare_hexangle_png)) },
		{ lens_flare_hexagon_png, uint32_t(sizeof(lens_flare_hexagon_png)) },
		{ lens_flare_pearl_png, uint32_t(sizeof(lens_flare_pearl_png)) },
		{ lens_flare_ring_png, uint32_t(sizeof(lens_flare_ring_png)) },
		{ lens_flare_flaredouble_png, uint32_t(sizeof(lens_flare_flaredouble_png)) },
	};

	RD::TextureView view;
	view.swizzle_r = RD::TEXTURE_SWIZZLE_R;
	view.swizzle_g = RD::TEXTURE_SWIZZLE_R;
	view.swizzle_b = RD::TEXTURE_SWIZZLE_R;
	view.swizzle_a = RD::TEXTURE_SWIZZLE_ONE;

	for (int i = 0; i < RSE::LENS_FLARE_ELEMENT_BUILTIN_COUNT; i++) {
		Ref<Image> image;
		image.instantiate(sources[i].png, sources[i].size);
		ERR_CONTINUE_MSG(image->is_empty(), "Could not decode a built-in lens flare texture.");

		image->convert(Image::FORMAT_L8);
		// Ghosts are often small on screen, and without mips they crawl.
		image->generate_mipmaps();

		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R8_UNORM;
		tf.width = image->get_width();
		tf.height = image->get_height();
		tf.mipmaps = image->get_mipmap_count() + 1;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT;

		builtin_textures[i] = RD::get_singleton()->texture_create(tf, view, Vector<Vector<uint8_t>>{ image->get_data() });
	}
}

LensFlare::~LensFlare() {
	if (flare_buffer.is_valid()) {
		RD::get_singleton()->free_rid(flare_buffer);
	}
	if (quad_buffer.is_valid()) {
		RD::get_singleton()->free_rid(quad_buffer);
	}
	if (exception_buffer.is_valid()) {
		RD::get_singleton()->free_rid(exception_buffer);
	}
	for (int i = 0; i < RSE::LENS_FLARE_ELEMENT_BUILTIN_COUNT; i++) {
		if (builtin_textures[i].is_valid()) {
			RD::get_singleton()->free_rid(builtin_textures[i]);
		}
	}
	shader.version_free(shader_version);
}

bool LensFlare::_build_draw_data(const LocalVector<RendererSceneRender::LensFlareInstance> &p_flares) {
	RendererLensFlare *flare_storage = RSG::lens_flare;
	TextureStorage *texture_storage = TextureStorage::get_singleton();

	flare_data.clear();
	quad_data.clear();
	batches.clear();
	// The first four vec4 are the inverse view projection, which draw() fills
	// in; the boxes follow it.
	exception_data.clear();
	exception_data.resize(16);

	RID white_texture = texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_WHITE);

	for (const RendererSceneRender::LensFlareInstance &instance : p_flares) {
		if (flare_data.size() >= (uint32_t)MAX_FLARES || quad_data.size() >= (uint32_t)MAX_QUADS) {
			break;
		}

		const LocalVector<RendererLensFlare::Element> &elements = flare_storage->lens_flare_get_elements(instance.flare);
		if (elements.is_empty()) {
			continue;
		}

		const uint32_t flare_index = flare_data.size();
		const Color source_color = flare_storage->lens_flare_get_color(instance.flare);
		const float source_intensity = flare_storage->lens_flare_get_intensity(instance.flare);
		const bool directional = flare_storage->lens_flare_is_directional(instance.flare);

		// A DirectionalLight3D shines along its own -Z, so the source of a
		// directional flare lies in the +Z direction and carries w = 0: the
		// projection then lands on the vanishing point, which is exactly where a
		// source at infinity belongs.
		const Vector3 forward = instance.transform.basis.get_column(Vector3::AXIS_Z).normalized();
		const Vector3 source = directional ? forward : instance.transform.origin;

		FlareData flare;
		flare.source[0] = source.x;
		flare.source[1] = source.y;
		flare.source[2] = source.z;
		flare.source[3] = directional ? 0.0f : 1.0f;

		flare.direction[0] = -forward.x;
		flare.direction[1] = -forward.y;
		flare.direction[2] = -forward.z;

		const float angle_limit = flare_storage->lens_flare_get_angle_limit(instance.flare);
		flare.direction[3] = angle_limit > 0.0f ? Math::cos(angle_limit) : -2.0f;

		flare.params[0] = flare_storage->lens_flare_get_occlusion_enabled(instance.flare) ? flare_storage->lens_flare_get_occlusion_radius(instance.flare) : 0.0f;
		flare.params[1] = flare_storage->lens_flare_get_fade_distance_begin(instance.flare);
		flare.params[2] = flare_storage->lens_flare_get_fade_distance_end(instance.flare);
		flare.params[3] = flare_storage->lens_flare_get_fade_screen_edge(instance.flare);

		flare.params2[0] = flare_storage->lens_flare_get_angle_fade(instance.flare);
		flare.params2[1] = flare_storage->lens_flare_get_scale(instance.flare);
		// How much light the source puts into the frame, for sharing the ghosts
		// out between flares in proportion to it.
		flare.params2[2] = source_intensity * source_color.get_luminance();

		// Where this flare's excluded bounds sit in the buffer, packed as
		// first box * 16 + count into a float, which holds it exactly.
		const uint32_t first_box = (exception_data.size() - 16) / 8;
		uint32_t box_count = 0;
		for (const AABB &bounds : instance.occlusion_exceptions) {
			if (box_count >= (uint32_t)MAX_EXCEPTIONS_PER_FLARE || first_box + box_count >= (uint32_t)MAX_EXCEPTION_BOXES) {
				break;
			}
			// The surface the depth buffer holds sits on the bounds, so they
			// grow a little to catch it despite the rounding of a reconstructed
			// position.
			const AABB padded = bounds.grow(0.03f + 0.02f * bounds.size[bounds.get_longest_axis_index()]);
			const Vector3 min = padded.position;
			const Vector3 max = padded.position + padded.size;
			const float box[8] = { float(min.x), float(min.y), float(min.z), 0.0f, float(max.x), float(max.y), float(max.z), 0.0f };
			for (int k = 0; k < 8; k++) {
				exception_data.push_back(box[k]);
			}
			box_count++;
		}
		flare.params2[3] = float(first_box * 16 + box_count);

		flare_data.push_back(flare);

		for (const RendererLensFlare::Element &element : elements) {
			// A built-in draws down the same path as a user texture; only where
			// the image comes from differs, and the shader never has to know.
			const bool is_builtin = element.type >= RSE::LENS_FLARE_ELEMENT_BUILTIN_CORONA;
			uint32_t shape = element.type;
			RID texture = white_texture;

			if (is_builtin) {
				shape = RSE::LENS_FLARE_ELEMENT_TEXTURE;
				RID builtin = builtin_textures[element.type - RSE::LENS_FLARE_ELEMENT_BUILTIN_CORONA];
				if (builtin.is_valid()) {
					texture = builtin;
				}
			} else if (element.type == RSE::LENS_FLARE_ELEMENT_TEXTURE && element.texture.is_valid()) {
				RID rd_texture = texture_storage->texture_get_rd_texture(element.texture);
				if (rd_texture.is_valid()) {
					texture = rd_texture;
				}
			}

			Color tint = element.color;
			tint *= element.intensity * source_intensity;
			if (element.tint_by_source) {
				tint *= source_color;
			}

			for (int repeat = 0; repeat < element.count; repeat++) {
				if (quad_data.size() >= (uint32_t)MAX_QUADS) {
					break;
				}

				QuadData quad;
				quad.color[0] = tint.r;
				quad.color[1] = tint.g;
				quad.color[2] = tint.b;
				quad.color[3] = 0.0f;

				quad.layout_data[0] = element.position + element.spacing * repeat;
				quad.layout_data[1] = element.size.x;
				quad.layout_data[2] = element.size.y;
				quad.layout_data[3] = element.rotation;

				quad.flags[0] = flare_index;
				quad.flags[1] = shape;
				quad.flags[2] = (uint32_t)element.sides;
				quad.flags[3] = element.auto_rotate ? 1u : 0u;

				quad.shape[0] = element.falloff;
				quad.shape[1] = element.chromatic_aberration;
				quad.shape[2] = 0.0f;
				quad.shape[3] = 0.0f;

				// Authored order is kept rather than sorted by state: additive
				// quads commute, but screen blended ones do not, and a run only
				// breaks when the pipeline or texture actually changes. Procedural
				// shapes all land on the default white texture, so the usual flare
				// still goes out in one draw.
				const uint32_t blend_mode = (uint32_t)element.blend_mode;
				if (batches.is_empty() || batches[batches.size() - 1].texture != texture || batches[batches.size() - 1].blend_mode != blend_mode) {
					Batch batch;
					batch.texture = texture;
					batch.blend_mode = blend_mode;
					batch.offset = quad_data.size();
					batch.count = 0;
					batches.push_back(batch);
				}

				batches[batches.size() - 1].count++;
				quad_data.push_back(quad);
			}
		}
	}

	return !quad_data.is_empty();
}

void LensFlare::draw(RID p_dest_fb, RID p_depth_texture, const LocalVector<RendererSceneRender::LensFlareInstance> &p_flares, const Projection &p_view_projection, const Transform3D &p_camera_transform, float p_aspect, float p_luminance_multiplier, float p_ghost_limit) {
	if (p_flares.is_empty()) {
		return;
	}

	if (!_build_draw_data(p_flares)) {
		return;
	}

	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);

	RD::get_singleton()->buffer_update(flare_buffer, 0, uint32_t(sizeof(FlareData)) * flare_data.size(), flare_data.ptr());
	RD::get_singleton()->buffer_update(quad_buffer, 0, uint32_t(sizeof(QuadData)) * quad_data.size(), quad_data.ptr());

	const Projection view_projection = p_view_projection * Projection(p_camera_transform.affine_inverse());
	{
		// Lets the shader turn a depth sample back into a world position.
		const Projection inverse = view_projection.inverse();
		for (int column = 0; column < 4; column++) {
			for (int row = 0; row < 4; row++) {
				exception_data[column * 4 + row] = inverse.columns[column][row];
			}
		}
	}
	RD::get_singleton()->buffer_update(exception_buffer, 0, uint32_t(sizeof(float)) * exception_data.size(), exception_data.ptr());

	PushConstant push_constant;
	MaterialStorage::store_camera(view_projection, push_constant.view_projection);

	const Vector3 camera_right = p_camera_transform.basis.get_column(Vector3::AXIS_X).normalized();
	push_constant.camera_right[0] = camera_right.x;
	push_constant.camera_right[1] = camera_right.y;
	push_constant.camera_right[2] = camera_right.z;
	push_constant.camera_right[3] = p_luminance_multiplier;

	push_constant.camera_position[0] = p_camera_transform.origin.x;
	push_constant.camera_position[1] = p_camera_transform.origin.y;
	push_constant.camera_position[2] = p_camera_transform.origin.z;
	push_constant.camera_position[3] = p_aspect;

	push_constant.flare_count = flare_data.size();
	push_constant.ghost_limit = CLAMP(p_ghost_limit, 0.0f, 1.0f);
	push_constant.pad = 0;

	RID depth_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID flare_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	RD::Uniform u_flares(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, flare_buffer);
	RD::Uniform u_quads(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, quad_buffer);
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ depth_sampler, p_depth_texture }));
	RD::Uniform u_exceptions(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, exception_buffer);

	RID version_shader = shader.version_get_shader(shader_version, 0);
	RD::FramebufferFormatID fb_format = RD::get_singleton()->framebuffer_get_format(p_dest_fb);

	RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_dest_fb);

	for (const Batch &batch : batches) {
		RD::Uniform u_texture(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ flare_sampler, batch.texture }));

		const Pipeline pipeline = batch.blend_mode == RSE::LENS_FLARE_BLEND_SCREEN ? PIPELINE_SCREEN : PIPELINE_ADD;
		RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, pipelines[pipeline].get_render_pipeline(RD::INVALID_ID, fb_format, false, RD::get_singleton()->draw_list_get_current_pass()));
		RD::get_singleton()->draw_list_bind_uniform_set(draw_list, uniform_set_cache->get_cache(version_shader, 0, u_flares, u_quads, u_depth, u_texture, u_exceptions), 0);

		push_constant.quad_offset = batch.offset;
		RD::get_singleton()->draw_list_set_push_constant(draw_list, &push_constant, sizeof(PushConstant));
		RD::get_singleton()->draw_list_draw(draw_list, false, batch.count, 4u);
	}

	RD::get_singleton()->draw_list_end();
}
