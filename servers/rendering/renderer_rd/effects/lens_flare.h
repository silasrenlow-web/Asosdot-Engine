/**************************************************************************/
/*  lens_flare.h                                                          */
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
#include "servers/rendering/renderer_rd/shaders/effects/lens_flare.glsl.gen.h"
#include "servers/rendering/renderer_scene_render.h"

namespace RendererRD {

// Draws the screen space flares for the sources a camera can see, straight into
// the HDR colour buffer before glow and tonemapping. That is where a real lens
// puts them: they are stray light hitting the sensor, so they should bloom and
// be exposed along with everything else rather than be painted over the top of a
// finished image.
class LensFlare {
public:
	enum {
		// A scene only ever has a handful of flare sources worth drawing, and the
		// buffers are allocated up front, so these are caps rather than budgets.
		MAX_FLARES = 32,
		MAX_QUADS = 256,
	};

	LensFlare();
	~LensFlare();

	void draw(RID p_dest_fb, RID p_depth_texture, const LocalVector<RendererSceneRender::LensFlareInstance> &p_flares, const Projection &p_view_projection, const Transform3D &p_camera_transform, float p_aspect, float p_luminance_multiplier);

private:
	// Mirrors the Flare struct in lens_flare.glsl.
	struct FlareData {
		float source[4];
		float direction[4];
		float params[4];
		float params2[4];
	};

	// Mirrors the Quad struct in lens_flare.glsl.
	struct QuadData {
		float color[4];
		float layout_data[4];
		uint32_t flags[4];
		float shape[4];
	};

	struct PushConstant {
		float view_projection[16];
		float camera_right[4];
		float camera_position[4];
		uint32_t quad_offset;
		uint32_t pad[3];
	};

	// A run of quads that share a pipeline and a texture, so they go out in one
	// draw. Elements with a procedural shape all resolve to the default white
	// texture, which is what keeps the common case down to a single draw.
	struct Batch {
		RID texture;
		uint32_t blend_mode = 0;
		uint32_t offset = 0;
		uint32_t count = 0;
	};

	enum Pipeline {
		PIPELINE_ADD,
		PIPELINE_SCREEN,
		PIPELINE_MAX,
	};

	LensFlareShaderRD shader;
	RID shader_version;
	PipelineCacheRD pipelines[PIPELINE_MAX];

	RID flare_buffer;
	RID quad_buffer;

	// The bundled element textures, in RSE::LensFlareElementType order starting
	// at LENS_FLARE_ELEMENT_BUILTIN_CORONA.
	RID builtin_textures[RSE::LENS_FLARE_ELEMENT_BUILTIN_COUNT];

	// Held across frames so a steady scene does no allocation.
	LocalVector<FlareData> flare_data;
	LocalVector<QuadData> quad_data;
	LocalVector<Batch> batches;

	void _create_builtin_textures();
	bool _build_draw_data(const LocalVector<RendererSceneRender::LensFlareInstance> &p_flares);
};

} // namespace RendererRD
