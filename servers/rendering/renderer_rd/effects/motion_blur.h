/**************************************************************************/
/*  motion_blur.h                                                         */
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

#include "core/math/projection.h"
#include "servers/rendering/renderer_rd/shaders/effects/motion_blur.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/storage/camera_attributes_storage.h"

namespace RendererRD {

// Motion blur from the velocity buffer: camera and object motion both, streaks
// that stay in front of or behind what they belong to, and the sky blurred by the
// camera turning. Works on the HDR frame before glow and tonemapping, the way a
// camera's shutter does, so bright things leave bright streaks.
class MotionBlur {
public:
	struct Parameters {
		Ref<RenderSceneBuffersRD> render_buffers;
		uint32_t view = 0;

		// The frame is blurred in place. Every pixel needs its neighbours as they were,
		// so the first pass keeps a copy of it, packed with what else is needed.
		RID color_dest;
		RID depth;
		RID velocity;
		Size2i size;

		// From this frame's clip space to the last one's.
		Projection reprojection;
		float z_near = 0.05;
		float z_far = 4000.0;
		bool orthogonal = false;

		RendererCameraAttributes::MotionBlurSettings settings;
		// How long the last frame took, which is what a velocity is a distance over.
		float time_step = 1.0 / 60.0;
	};

	void process(const Parameters &p_parameters);

	// Whether the graphics device can do what the passes need of it: storage images
	// in the formats they hand each other. Some drivers and older GPUs can't, and
	// there the effect is left out rather than failing every frame.
	bool is_supported() const { return supported; }

	MotionBlur();
	~MotionBlur();

private:
	enum Mode {
		MODE_PREPARE,
		MODE_TILE_MAX,
		MODE_NEIGHBOR_MAX,
		MODE_GATHER,
		MODE_MAX,
	};

	struct PushConstant {
		int32_t size[2];
		int32_t tile_count[2];
		int32_t tile_size;
		int32_t samples;
		float velocity_scale;
		float max_length;
		float z_near;
		float z_far;
		uint32_t orthogonal;
		uint32_t pad;
		float reprojection[16];
	};

	bool supported = false;

	MotionBlurShaderRD shader;
	RID shader_version;
	RID pipelines[MODE_MAX];
};

} // namespace RendererRD
