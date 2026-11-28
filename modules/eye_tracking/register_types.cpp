/**************************************************************************/
/*  register_types.cpp                                                    */
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

#include "register_types.h"

#include "eye_tracker.h"
#include "gaze_target_3d.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/class_db.h"

#ifdef TOOLS_ENABLED
#include "editor/gaze_target_3d_gizmo_plugin.h"
#include "editor/plugins/editor_plugin.h"
#endif

static EyeTracker *eye_tracker = nullptr;

void initialize_eye_tracking_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		// Off unless the game asks: a project has to say it may use the webcam,
		// and the game still has to call EyeTracker.start().
		GLOBAL_DEF_BASIC("eye_tracking/webcam/enabled", false);

		// One, made here; scripts reach it as the EyeTracker singleton.
		GDREGISTER_ABSTRACT_CLASS(EyeTracker);
		GDREGISTER_CLASS(GazeTarget3D);
		eye_tracker = memnew(EyeTracker);
		Engine::get_singleton()->add_singleton(Engine::Singleton("EyeTracker", EyeTracker::get_singleton()));
	}
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_INTERNAL_CLASS(GazeTarget3DGizmoPlugin);
		GDREGISTER_INTERNAL_CLASS(EditorPluginEyeTracking);
		EditorPlugins::add_by_type<EditorPluginEyeTracking>();
	}
#endif
}

void uninitialize_eye_tracking_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	if (eye_tracker) {
		Engine::get_singleton()->remove_singleton("EyeTracker");
		memdelete(eye_tracker);
		eye_tracker = nullptr;
	}
}
