/**************************************************************************/
/*  gaze_focus.h                                                          */
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

#include "servers/rendering/renderer_rd/shaders/effects/gaze_focus.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/storage/camera_attributes_storage.h"

namespace RendererRD {

// Keeps where the player looks sharp and softens the rest, and with a depth
// range also what is nearer or further than the looked-at thing, refocusing
// over a moment like an eye. The gaze point comes from the viewport
// (RenderingServer.viewport_set_gaze_point, which EyeTracker sets); without one
// it is the middle of the screen. Works on the HDR frame, after depth of field.
class GazeFocus {
public:
	struct Parameters {
		Ref<RenderSceneBuffersRD> render_buffers;
		uint32_t view = 0;
		RID color; // Blurred in place.
		RID depth;
		Size2i size;
		Vector2 gaze_uv = Vector2(0.5, 0.5);
		float z_near = 0.05;
		float z_far = 4000.0;
		bool orthogonal = false;
		float time_step = 1.0 / 60.0;
		RendererCameraAttributes::GazeFocusSettings settings;
	};

	void process(const Parameters &p_parameters);
	bool is_supported() const { return supported; }

	GazeFocus();
	~GazeFocus();

private:
	enum Mode {
		MODE_FOCUS,
		MODE_DOWNSAMPLE,
		MODE_BLUR,
		MODE_COMPOSITE,
		MODE_MAX,
	};

	struct PushConstant {
		int32_t size[2];
		float gaze_uv[2];
		float radius;
		float softness;
		float amount;
		float depth_range;
		float aspect;
		float z_near;
		float z_far;
		uint32_t orthogonal;
		float focus_blend;
		uint32_t reset;
		float blur_step[2];
	};

	bool supported = false;
	GazeFocusShaderRD shader;
	RID shader_version;
	RID pipelines[MODE_MAX];
};

} // namespace RendererRD
