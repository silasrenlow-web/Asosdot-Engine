/**************************************************************************/
/*  editor_freeze_watchdog.cpp                                            */
/**************************************************************************/
/*  Asosdot: records where the editor is stuck when it stops responding.  */
/**************************************************************************/

#include "editor_freeze_watchdog.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/version.h"

#ifdef WINDOWS_ENABLED
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// Order matters: dbghelp needs windows.h first.
#include <dbghelp.h>
#endif

EditorFreezeWatchdog *EditorFreezeWatchdog::singleton = nullptr;

void EditorFreezeWatchdog::tick() {
	last_tick_usec.set(OS::get_singleton()->get_ticks_usec());
}

void EditorFreezeWatchdog::set_activity(const String &p_activity) {
	if (!singleton) {
		return;
	}
	MutexLock lock(singleton->mutex);
	singleton->activity = p_activity;
}

void EditorFreezeWatchdog::set_scene(const String &p_scene) {
	MutexLock lock(mutex);
	scene = p_scene;
}

String EditorFreezeWatchdog::take_finished_report() {
	MutexLock lock(mutex);
	const String path = finished_report;
	finished_report = String();
	return path;
}

void EditorFreezeWatchdog::_write(const String &p_text, bool p_append) {
	Ref<FileAccess> file = FileAccess::open(report_path, p_append ? FileAccess::READ_WRITE : FileAccess::WRITE);
	if (file.is_null()) {
		return;
	}
	if (p_append) {
		file->seek_end();
	}
	file->store_string(p_text);
}

String EditorFreezeWatchdog::_capture_main_stack() {
#ifdef WINDOWS_ENABLED
	HANDLE process = GetCurrentProcess();
	HANDLE main_thread = (HANDLE)main_thread_handle;
	if (!main_thread) {
		return "  (the main thread can't be read)\n";
	}
	if (!symbols_ready) {
		// Before the main thread is paused: loading symbols takes memory and time.
		SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
		symbols_ready = SymInitialize(process, nullptr, TRUE);
	}

	// The main thread is paused only to copy its return addresses into a fixed array, so it
	// can't be left holding a lock this thread needs.
	static constexpr int MAX_FRAMES = 96;
	DWORD64 addresses[MAX_FRAMES];
	int count = 0;
	if (SuspendThread(main_thread) == (DWORD)-1) {
		return "  (the main thread can't be paused)\n";
	}
	CONTEXT context;
	memset(&context, 0, sizeof(context));
	context.ContextFlags = CONTEXT_FULL;
	if (GetThreadContext(main_thread, &context)) {
		STACKFRAME64 frame;
		memset(&frame, 0, sizeof(frame));
		frame.AddrPC.Mode = AddrModeFlat;
		frame.AddrStack.Mode = AddrModeFlat;
		frame.AddrFrame.Mode = AddrModeFlat;
#if defined(_M_X64)
		const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
		frame.AddrPC.Offset = context.Rip;
		frame.AddrStack.Offset = context.Rsp;
		frame.AddrFrame.Offset = context.Rbp;
#elif defined(_M_ARM64) || defined(_M_ARM64EC)
		const DWORD machine = IMAGE_FILE_MACHINE_ARM64;
		frame.AddrPC.Offset = context.Pc;
		frame.AddrStack.Offset = context.Sp;
		frame.AddrFrame.Offset = context.Fp;
#else
		const DWORD machine = IMAGE_FILE_MACHINE_I386;
		frame.AddrPC.Offset = context.Eip;
		frame.AddrStack.Offset = context.Esp;
		frame.AddrFrame.Offset = context.Ebp;
#endif
		while (count < MAX_FRAMES && StackWalk64(machine, process, main_thread, &frame, &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
			if (frame.AddrPC.Offset == 0) {
				break;
			}
			addresses[count++] = frame.AddrPC.Offset;
		}
	}
	ResumeThread(main_thread);

	String text;
	alignas(SYMBOL_INFO) char symbol_buffer[sizeof(SYMBOL_INFO) + 1024];
	for (int i = 0; i < count; i++) {
		String module = "?";
		DWORD64 module_base = 0;
		IMAGEHLP_MODULE64 module_info;
		memset(&module_info, 0, sizeof(module_info));
		module_info.SizeOfStruct = sizeof(module_info);
		if (SymGetModuleInfo64(process, addresses[i], &module_info)) {
			module = String(module_info.ModuleName);
			module_base = module_info.BaseOfImage;
		}
		String function = "???";
		SYMBOL_INFO *symbol = (SYMBOL_INFO *)symbol_buffer;
		memset(symbol_buffer, 0, sizeof(symbol_buffer));
		symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
		symbol->MaxNameLen = 1023;
		DWORD64 displacement = 0;
		if (SymFromAddr(process, addresses[i], &displacement, symbol)) {
			function = String(symbol->Name);
		}
		String where;
		IMAGEHLP_LINE64 line;
		memset(&line, 0, sizeof(line));
		line.SizeOfStruct = sizeof(line);
		DWORD line_displacement = 0;
		if (SymGetLineFromAddr64(process, addresses[i], &line_displacement, &line)) {
			where = vformat(" (%s:%d)", String(line.FileName).get_file(), (int)line.LineNumber);
		}
		text += vformat("  [%d] %s+0x%x %s%s\n", i, module, (uint64_t)(addresses[i] - module_base), function, where);
	}
	return text.is_empty() ? "  (no frames)\n" : text;
#else
	return "  (call stacks are recorded on Windows)\n";
#endif
}

void EditorFreezeWatchdog::_watch() {
	bool frozen = false;
	double next_sample = FREEZE_SECONDS;
	while (!exiting.is_set()) {
		OS::get_singleton()->delay_usec(250000);
		const uint64_t last = last_tick_usec.get();
		if (last == 0) {
			continue;
		}
		const double stalled = (OS::get_singleton()->get_ticks_usec() - last) / 1000000.0;
		if (stalled < FREEZE_SECONDS) {
			if (frozen) {
				// It responds again.
				_write(vformat("\nThe editor responded again after %.1f s.\n", stalled), true);
				MutexLock lock(mutex);
				finished_report = report_path;
				frozen = false;
			}
			continue;
		}
		if (!frozen) {
			frozen = true;
			next_sample = FREEZE_SECONDS;
			String doing;
			String open_scene;
			{
				MutexLock lock(mutex);
				doing = activity;
				open_scene = scene;
			}
			const String logs = OS::get_singleton()->get_user_data_dir().path_join("logs").path_join("freezes");
			DirAccess::make_dir_recursive_absolute(logs);
			report_path = logs.path_join(vformat("freeze-%s.txt", Time::get_singleton()->get_datetime_string_from_system().replace_char(':', '-')));
			_write(vformat("Asosdot freeze report\n%s\n\nThe editor stopped responding at %s.\nLast thing it reported doing: %s\nOpen scene: %s\n\nWhere the editor's main thread was, sampled as the freeze went on:\n", GODOT_VERSION_FULL_NAME, Time::get_singleton()->get_datetime_string_from_system(), doing.is_empty() ? String("nothing reported") : doing, open_scene.is_empty() ? String("none") : open_scene), false);
		}
		if (stalled >= next_sample && next_sample <= 64.0) {
			_write(vformat("\nAfter %.0f s:\n%s", stalled, _capture_main_stack()), true);
			next_sample *= 2.0;
		}
	}
}

void EditorFreezeWatchdog::_thread_func(void *p_self) {
	static_cast<EditorFreezeWatchdog *>(p_self)->_watch();
}

EditorFreezeWatchdog::EditorFreezeWatchdog() {
	singleton = this;
#ifdef WINDOWS_ENABLED
	main_thread_os_id = GetCurrentThreadId();
	main_thread_handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, (DWORD)main_thread_os_id);
#endif
	thread.start(_thread_func, this);
}

EditorFreezeWatchdog::~EditorFreezeWatchdog() {
	exiting.set();
	if (thread.is_started()) {
		thread.wait_to_finish();
	}
#ifdef WINDOWS_ENABLED
	if (main_thread_handle) {
		CloseHandle((HANDLE)main_thread_handle);
	}
	if (symbols_ready) {
		SymCleanup(GetCurrentProcess());
	}
#endif
	singleton = nullptr;
}
