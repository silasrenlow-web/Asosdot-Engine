/**************************************************************************/
/*  editor_freeze_watchdog.h                                              */
/**************************************************************************/
/*  Asosdot: records where the editor is stuck when it stops responding.  */
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

	// The main thread, for its call stack.
	uint64_t main_thread_os_id = 0;
	void *main_thread_handle = nullptr;
	bool symbols_ready = false;

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
