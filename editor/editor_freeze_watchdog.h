/**************************************************************************/
/*  editor_freeze_watchdog.h                                              */
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

#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/string/ustring.h"
#include "core/templates/safe_refcount.h"

// Watches the editor's main loop from another thread. When a frame takes longer than a few
// seconds, it writes a report while the editor is still stuck: what the editor said it was
// doing, the open scene, and, on Windows, the main thread's call stack, sampled again as the
// freeze goes on. The editor names the report once it responds again.
class EditorFreezeWatchdog {
	static EditorFreezeWatchdog *singleton;

	Thread thread;
	SafeFlag exiting;
	SafeNumeric<uint64_t> last_tick_usec;

	Mutex mutex;
	String activity;
	String scene;
	String report_path;
	String finished_report; // Read and cleared by the main thread.

#ifdef WINDOWS_ENABLED
	// The main thread, for its call stack. Only Windows takes one.
	uint64_t main_thread_os_id = 0;
	void *main_thread_handle = nullptr;
	bool symbols_ready = false;
#endif

	static void _thread_func(void *p_self);
	void _watch();
	String _capture_main_stack();
	void _write(const String &p_text, bool p_append);

public:
	static constexpr double FREEZE_SECONDS = 8.0;

	static EditorFreezeWatchdog *get_singleton() { return singleton; }

	// Called every frame by the editor.
	void tick();
	// What the editor is doing, such as an import step, and the scene it has open.
	static void set_activity(const String &p_activity);
	void set_scene(const String &p_scene);
	// A report of a freeze that has ended, once.
	String take_finished_report();

	EditorFreezeWatchdog();
	~EditorFreezeWatchdog();
};
