/**************************************************************************/
/*  camera_win.cpp                                                        */
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

#include "camera_win.h"

#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"
#include "servers/camera/camera_feed.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#ifdef _MSC_VER
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#endif

namespace {

template <typename T>
void safe_release(T *&p_object) {
	if (p_object) {
		p_object->Release();
		p_object = nullptr;
	}
}

String guid_to_format_name(const GUID &p_subtype) {
	if (p_subtype == MFVideoFormat_NV12) {
		return "NV12";
	} else if (p_subtype == MFVideoFormat_YUY2) {
		return "YUY2";
	} else if (p_subtype == MFVideoFormat_MJPG) {
		return "MJPG";
	} else if (p_subtype == MFVideoFormat_RGB24) {
		return "RGB24";
	} else if (p_subtype == MFVideoFormat_RGB32) {
		return "RGB32";
	} else if (p_subtype == MFVideoFormat_I420) {
		return "I420";
	} else if (p_subtype == MFVideoFormat_H264) {
		return "H264";
	}
	return vformat("%08X", uint32_t(p_subtype.Data1));
}

} // namespace

//////////////////////////////////////////////////////////////////////////
// CameraFeedWindows: one webcam. Opening it makes a source reader that hands
// back RGB32 whatever the camera sends (Media Foundation converts), read on a
// thread of its own until the feed is turned off.

class CameraFeedWindows : public CameraFeed {
	GDSOFTCLASS(CameraFeedWindows, CameraFeed);

	struct NativeFormat {
		DWORD media_type_index = 0;
		FeedFormat format;
	};

	String symbolic_link;
	Vector<NativeFormat> native_formats;

	IMFMediaSource *source = nullptr;
	IMFSourceReader *reader = nullptr;
	Thread capture_thread;
	SafeFlag exit_flag;
	int output_width = 0;
	int output_height = 0;
	LONG output_stride = 0;

	bool _open_source(IMFMediaSource **r_source) const;
	void _read_formats();
	int _pick_default_format() const;
	static void _capture_thread_func(void *p_feed);
	void _capture_loop();
	void _close();

public:
	const String &get_symbolic_link() const { return symbolic_link; }

	virtual bool activate_feed() override;
	virtual void deactivate_feed() override;
	virtual bool set_format(int p_index, const Dictionary &p_parameters) override;
	virtual Array get_formats() const override;
	virtual FeedFormat get_format() const override;

	CameraFeedWindows(const String &p_name, const String &p_symbolic_link);
	~CameraFeedWindows();
};

CameraFeedWindows::CameraFeedWindows(const String &p_name, const String &p_symbolic_link) :
		CameraFeed(p_name, FEED_FRONT) {
	symbolic_link = p_symbolic_link;
	_read_formats();
	selected_format = _pick_default_format();
}

CameraFeedWindows::~CameraFeedWindows() {
	if (is_active()) {
		deactivate_feed();
	}
	_close();
}

bool CameraFeedWindows::_open_source(IMFMediaSource **r_source) const {
	IMFAttributes *attributes = nullptr;
	HRESULT hr = MFCreateAttributes(&attributes, 2);
	if (SUCCEEDED(hr)) {
		hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
	}
	if (SUCCEEDED(hr)) {
		hr = attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, (LPCWSTR)symbolic_link.utf16().get_data());
	}
	if (SUCCEEDED(hr)) {
		hr = MFCreateDeviceSource(attributes, r_source);
	}
	safe_release(attributes);
	return SUCCEEDED(hr);
}

void CameraFeedWindows::_read_formats() {
	// What the camera itself can send. Opening the device for this is quick and
	// does not start it (the light stays off).
	native_formats.clear();
	formats.clear();
	IMFMediaSource *probe_source = nullptr;
	if (!_open_source(&probe_source)) {
		return;
	}
	IMFSourceReader *probe_reader = nullptr;
	if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(probe_source, nullptr, &probe_reader))) {
		for (DWORD i = 0;; i++) {
			IMFMediaType *type = nullptr;
			if (FAILED(probe_reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &type))) {
				break;
			}
			UINT32 width = 0;
			UINT32 height = 0;
			UINT32 numerator = 0;
			UINT32 denominator = 0;
			GUID subtype = GUID_NULL;
			MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height);
			MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &numerator, &denominator);
			type->GetGUID(MF_MT_SUBTYPE, &subtype);
			safe_release(type);
			if (width == 0 || height == 0 || subtype == MFVideoFormat_H264) {
				continue;
			}
			NativeFormat native;
			native.media_type_index = i;
			native.format.width = int(width);
			native.format.height = int(height);
			native.format.format = guid_to_format_name(subtype);
			// Godot's feeds give the frame time, seconds per frame, as a fraction.
			native.format.frame_numerator = int(denominator);
			native.format.frame_denominator = int(numerator);
			native.format.pixel_format = subtype.Data1;
			native_formats.push_back(native);
			formats.push_back(native.format);
		}
	}
	safe_release(probe_reader);
	if (probe_source) {
		probe_source->Shutdown();
	}
	safe_release(probe_source);
}

int CameraFeedWindows::_pick_default_format() const {
	// The biggest picture up to 1920 by 1080 that comes 25 times a second or
	// more. Eye tracking wants the eyes big in the frame; past 1080p costs
	// conversion time for nothing it can use.
	int best = -1;
	int64_t best_score = -1;
	for (int i = 0; i < native_formats.size(); i++) {
		const FeedFormat &f = native_formats[i].format;
		const float fps = f.frame_numerator > 0 ? float(f.frame_denominator) / float(f.frame_numerator) : 0.0f;
		if (f.width > 1920 || f.height > 1080) {
			continue;
		}
		int64_t score = int64_t(f.width) * f.height;
		if (fps >= 24.5f) {
			score += int64_t(1) << 40;
		}
		// Uncompressed formats convert cheaper than MJPG.
		if (f.format != "MJPG") {
			score += 1;
		}
		if (score > best_score) {
			best_score = score;
			best = i;
		}
	}
	if (best < 0 && !native_formats.is_empty()) {
		best = 0;
	}
	return best;
}

bool CameraFeedWindows::set_format(int p_index, const Dictionary &p_parameters) {
	ERR_FAIL_INDEX_V(p_index, native_formats.size(), false);
	parameters = p_parameters.duplicate();
	selected_format = p_index;
	if (is_active()) {
		deactivate_feed();
		return activate_feed();
	}
	return true;
}

Array CameraFeedWindows::get_formats() const {
	Array result;
	for (const NativeFormat &native : native_formats) {
		Dictionary dictionary;
		dictionary["width"] = native.format.width;
		dictionary["height"] = native.format.height;
		dictionary["format"] = native.format.format;
		dictionary["frame_numerator"] = native.format.frame_numerator;
		dictionary["frame_denominator"] = native.format.frame_denominator;
		result.push_back(dictionary);
	}
	return result;
}

CameraFeed::FeedFormat CameraFeedWindows::get_format() const {
	if (selected_format >= 0 && selected_format < native_formats.size()) {
		return native_formats[selected_format].format;
	}
	return FeedFormat();
}

bool CameraFeedWindows::activate_feed() {
	_close();
	if (!_open_source(&source)) {
		WARN_PRINT(vformat("Camera \"%s\" could not be opened.", name));
		return false;
	}

	IMFAttributes *reader_attributes = nullptr;
	HRESULT hr = MFCreateAttributes(&reader_attributes, 1);
	if (SUCCEEDED(hr)) {
		// Lets the reader turn NV12, YUY2 or MJPG into RGB32 for us.
		hr = reader_attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
	}
	if (SUCCEEDED(hr)) {
		hr = MFCreateSourceReaderFromMediaSource(source, reader_attributes, &reader);
	}
	safe_release(reader_attributes);

	if (SUCCEEDED(hr) && selected_format >= 0 && selected_format < native_formats.size()) {
		IMFMediaType *native_type = nullptr;
		if (SUCCEEDED(reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, native_formats[selected_format].media_type_index, &native_type))) {
			reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, native_type);
		}
		safe_release(native_type);
	}

	IMFMediaType *output_type = nullptr;
	if (SUCCEEDED(hr)) {
		hr = MFCreateMediaType(&output_type);
	}
	if (SUCCEEDED(hr)) {
		hr = output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	}
	if (SUCCEEDED(hr)) {
		hr = output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
	}
	if (SUCCEEDED(hr)) {
		hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, output_type);
	}
	safe_release(output_type);

	IMFMediaType *current_type = nullptr;
	if (SUCCEEDED(hr)) {
		hr = reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current_type);
	}
	if (SUCCEEDED(hr)) {
		UINT32 width = 0;
		UINT32 height = 0;
		MFGetAttributeSize(current_type, MF_MT_FRAME_SIZE, &width, &height);
		output_width = int(width);
		output_height = int(height);
		UINT32 stride = 0;
		if (SUCCEEDED(current_type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) {
			output_stride = LONG(int32_t(stride));
		} else {
			// Without a stride attribute uncompressed RGB is bottom-up.
			output_stride = -LONG(width * 4);
		}
	}
	safe_release(current_type);

	if (FAILED(hr) || output_width <= 0 || output_height <= 0) {
		WARN_PRINT(vformat("Camera \"%s\" could not give RGB frames (0x%08X).", name, uint32_t(hr)));
		_close();
		return false;
	}

	exit_flag.clear();
	capture_thread.start(_capture_thread_func, this);
	return true;
}

void CameraFeedWindows::deactivate_feed() {
	exit_flag.set();
	if (capture_thread.is_started()) {
		capture_thread.wait_to_finish();
	}
	_close();
}

void CameraFeedWindows::_close() {
	safe_release(reader);
	if (source) {
		source->Shutdown();
	}
	safe_release(source);
}

void CameraFeedWindows::_capture_thread_func(void *p_feed) {
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	static_cast<CameraFeedWindows *>(p_feed)->_capture_loop();
	CoUninitialize();
}

void CameraFeedWindows::_capture_loop() {
	const int width = output_width;
	const int height = output_height;
	Vector<uint8_t> rgb;

	while (!exit_flag.is_set()) {
		DWORD stream_index = 0;
		DWORD flags = 0;
		LONGLONG timestamp = 0;
		IMFSample *sample = nullptr;
		// Waits for the next frame, so this thread sleeps at the camera's rate.
		HRESULT hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream_index, &flags, &timestamp, &sample);
		if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
			safe_release(sample);
			if (!exit_flag.is_set()) {
				WARN_PRINT(vformat("Camera \"%s\" stopped sending frames (0x%08X).", name, uint32_t(hr)));
			}
			break;
		}
		if (!sample) {
			continue;
		}

		IMFMediaBuffer *buffer = nullptr;
		if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer))) {
			BYTE *scanline0 = nullptr;
			LONG pitch = 0;
			IMF2DBuffer *buffer_2d = nullptr;
			BYTE *data = nullptr;
			DWORD length = 0;
			bool locked_2d = false;
			if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer_2d))) && SUCCEEDED(buffer_2d->Lock2D(&scanline0, &pitch))) {
				locked_2d = true;
			} else if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
				pitch = output_stride;
				scanline0 = pitch < 0 ? data + size_t(-pitch) * size_t(height - 1) : data;
			}

			if (scanline0) {
				// BGRX to RGB, top row first.
				rgb.resize(width * height * 3);
				uint8_t *dst = rgb.ptrw();
				for (int y = 0; y < height; y++) {
					const uint8_t *src = scanline0 + intptr_t(y) * intptr_t(pitch);
					uint8_t *row = dst + size_t(y) * width * 3;
					for (int x = 0; x < width; x++) {
						row[x * 3 + 0] = src[x * 4 + 2];
						row[x * 3 + 1] = src[x * 4 + 1];
						row[x * 3 + 2] = src[x * 4 + 0];
					}
				}
				if (locked_2d) {
					buffer_2d->Unlock2D();
				} else {
					buffer->Unlock();
				}
				Ref<Image> image = Image::create_from_data(width, height, false, Image::FORMAT_RGB8, rgb);
				set_rgb_image(image);
			} else if (locked_2d) {
				buffer_2d->Unlock2D();
			}
			safe_release(buffer_2d);
		}
		safe_release(buffer);
		safe_release(sample);
	}
}

//////////////////////////////////////////////////////////////////////////
// CameraWindows

CameraWindows::CameraWindows() {
	mf_started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
	if (!mf_started) {
		WARN_PRINT("Media Foundation is not available; no webcams can be used.");
	}
}

CameraWindows::~CameraWindows() {
	for (int i = feeds.size() - 1; i >= 0; i--) {
		Ref<CameraFeed> feed = feeds[i];
		if (feed.is_valid() && feed->is_active()) {
			feed->set_active(false);
		}
	}
	feeds.clear();
	if (mf_started) {
		MFShutdown();
	}
}

void CameraWindows::set_monitoring_feeds(bool p_monitoring_feeds) {
	if (p_monitoring_feeds == monitoring_feeds) {
		return;
	}
	CameraServer::set_monitoring_feeds(p_monitoring_feeds);
	if (p_monitoring_feeds) {
		update_feeds();
	}
}

void CameraWindows::update_feeds() {
	if (!mf_started) {
		return;
	}

	IMFAttributes *attributes = nullptr;
	IMFActivate **devices = nullptr;
	UINT32 count = 0;
	HRESULT hr = MFCreateAttributes(&attributes, 1);
	if (SUCCEEDED(hr)) {
		hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
	}
	if (SUCCEEDED(hr)) {
		hr = MFEnumDeviceSources(attributes, &devices, &count);
	}
	safe_release(attributes);

	Vector<String> found_links;
	for (UINT32 i = 0; SUCCEEDED(hr) && i < count; i++) {
		WCHAR *friendly_name = nullptr;
		WCHAR *link = nullptr;
		UINT32 length = 0;
		devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendly_name, &length);
		devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &length);
		if (link) {
			const String link_string = String::utf16((const char16_t *)link);
			const String name_string = friendly_name ? String::utf16((const char16_t *)friendly_name) : String("Camera");
			found_links.push_back(link_string);

			bool known = false;
			for (const Ref<CameraFeed> &feed : feeds) {
				Ref<CameraFeedWindows> windows_feed = feed;
				if (windows_feed.is_valid() && windows_feed->get_symbolic_link() == link_string) {
					known = true;
					break;
				}
			}
			if (!known) {
				Ref<CameraFeedWindows> feed = memnew(CameraFeedWindows(name_string, link_string));
				add_feed(feed);
			}
		}
		CoTaskMemFree(friendly_name);
		CoTaskMemFree(link);
		devices[i]->Release();
	}
	CoTaskMemFree(devices);

	// Cameras that were unplugged.
	for (int i = feeds.size() - 1; i >= 0; i--) {
		Ref<CameraFeedWindows> windows_feed = feeds[i];
		if (windows_feed.is_valid() && !found_links.has(windows_feed->get_symbolic_link())) {
			if (windows_feed->is_active()) {
				windows_feed->set_active(false);
			}
			remove_feed(windows_feed);
		}
	}

	emit_signal(SNAME(feeds_updated_signal_name));
}
