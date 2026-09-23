/**************************************************************************/
/*  terrain_heightmap_io.cpp                                              */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#include "terrain_heightmap_io.h"

#include "core/io/file_access.h"

#include <png.h>

namespace TerrainHeightmapIO {

static Vector2 find_range(const Ref<Image> &p_image) {
	const float *h = (const float *)p_image->ptr();
	const int count = p_image->get_width() * p_image->get_height();
	float lo = Math::INF;
	float hi = -Math::INF;
	for (int i = 0; i < count; i++) {
		if (!Math::is_nan(h[i])) {
			lo = MIN(lo, h[i]);
			hi = MAX(hi, h[i]);
		}
	}
	return lo > hi ? Vector2() : Vector2(lo, hi);
}

static Ref<Image> from_u16(const uint16_t *p_data, int p_width, int p_height) {
	Vector<uint8_t> bytes;
	bytes.resize(p_width * p_height * sizeof(float));
	float *dst = (float *)bytes.ptrw();
	for (int i = 0; i < p_width * p_height; i++) {
		dst[i] = p_data[i] / 65535.0f;
	}
	return Image::create_from_data(p_width, p_height, false, Image::FORMAT_RF, bytes);
}

// The engine's PNG loader narrows 16-bit files to 8 bits, which would turn a
// smooth hillside into 256 terraces, so 16-bit files are read here instead.
static Error load_png16(const Vector<uint8_t> &p_file, Loaded &r_loaded, bool &r_handled) {
	r_handled = false;
	png_image png;
	memset(&png, 0, sizeof(png));
	png.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_memory(&png, p_file.ptr(), p_file.size())) {
		return ERR_FILE_CORRUPT;
	}
	if (!(png.format & PNG_FORMAT_FLAG_LINEAR)) {
		png_image_free(&png);
		return OK; // 8-bit: the ordinary loader handles it exactly.
	}
	r_handled = true;
	// Without an sRGB flag, libpng treats 16-bit samples as linear and hands
	// them over untouched, which is what a heightmap is.
	png.format = PNG_FORMAT_LINEAR_Y;
	Vector<uint16_t> samples;
	samples.resize(png.width * png.height);
	if (!png_image_finish_read(&png, nullptr, samples.ptrw(), 0, nullptr)) {
		png_image_free(&png);
		return ERR_FILE_CORRUPT;
	}
	r_loaded.image = from_u16(samples.ptr(), png.width, png.height);
	r_loaded.bits = 16;
	r_loaded.normalized = true;
	return OK;
}

Error load(const String &p_path, Loaded &r_loaded, String &r_error) {
	const String ext = p_path.get_extension().to_lower();
	r_loaded = Loaded();

	if (ext == "r16" || ext == "raw") {
		const Vector<uint8_t> file = FileAccess::get_file_as_bytes(p_path);
		const int samples = file.size() / 2;
		const int side = (int)Math::round(Math::sqrt((double)samples));
		if (file.is_empty() || side * side != samples) {
			r_error = RTR("A raw heightmap must be square, 16 bits per sample, little endian.");
			return ERR_FILE_UNRECOGNIZED;
		}
		Vector<uint16_t> values;
		values.resize(samples);
		for (int i = 0; i < samples; i++) {
			values.write[i] = (uint16_t)file[i * 2] | ((uint16_t)file[i * 2 + 1] << 8);
		}
		r_loaded.image = from_u16(values.ptr(), side, side);
		r_loaded.bits = 16;
	} else {
		if (ext == "png") {
			const Vector<uint8_t> file = FileAccess::get_file_as_bytes(p_path);
			bool handled = false;
			const Error err = load_png16(file, r_loaded, handled);
			if (err != OK) {
				r_error = RTR("The PNG file could not be read.");
				return err;
			}
			if (handled) {
				r_loaded.range = find_range(r_loaded.image);
				return OK;
			}
		}
		Ref<Image> image;
		image.instantiate();
		const Error err = image->load(p_path);
		if (err != OK) {
			r_error = vformat(RTR("Could not load \"%s\" as an image."), p_path.get_file());
			return err;
		}
		if (image->is_compressed()) {
			image->decompress();
		}
		image->clear_mipmaps();
		const bool is_float = image->get_format() >= Image::FORMAT_RF && image->get_format() <= Image::FORMAT_RGBE9995;
		image->convert(Image::FORMAT_RF);
		r_loaded.image = image;
		r_loaded.bits = is_float ? 32 : 8;
		if (is_float) {
			// A float map in 0..1 is a normalised map; anything else is metres.
			const Vector2 range = find_range(image);
			r_loaded.normalized = range.x >= 0.0f && range.y <= 1.0f;
		}
	}
	r_loaded.range = find_range(r_loaded.image);
	return OK;
}

Error save(const Ref<Image> &p_heights, const String &p_path, Vector2 &r_range, String &r_error) {
	ERR_FAIL_COND_V(p_heights.is_null() || p_heights->get_format() != Image::FORMAT_RF, ERR_INVALID_PARAMETER);
	const String ext = p_path.get_extension().to_lower();
	r_range = find_range(p_heights);

	if (ext == "exr") {
		const Error err = p_heights->save_exr(p_path, true);
		if (err != OK) {
			r_error = RTR("Could not write the EXR file.");
		}
		return err;
	}

	const int w = p_heights->get_width();
	const int h = p_heights->get_height();
	const float *src = (const float *)p_heights->ptr();
	const float span = MAX(r_range.y - r_range.x, 1e-6f);
	Vector<uint16_t> values;
	values.resize(w * h);
	for (int i = 0; i < w * h; i++) {
		const float v = Math::is_nan(src[i]) ? 0.0f : (src[i] - r_range.x) / span;
		values.write[i] = (uint16_t)CLAMP((int)Math::round(v * 65535.0f), 0, 65535);
	}

	if (ext == "r16" || ext == "raw") {
		if (w != h) {
			r_error = RTR("Raw heightmaps must be square. Export as EXR or PNG instead.");
			return ERR_INVALID_PARAMETER;
		}
		Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
		if (f.is_null()) {
			r_error = RTR("Could not open the file for writing.");
			return ERR_FILE_CANT_WRITE;
		}
		for (int i = 0; i < w * h; i++) {
			f->store_16(values[i]);
		}
		return OK;
	}

	if (ext == "png") {
		png_image png;
		memset(&png, 0, sizeof(png));
		png.version = PNG_IMAGE_VERSION;
		png.width = w;
		png.height = h;
		png.format = PNG_FORMAT_LINEAR_Y;
		png_alloc_size_t size = 0;
		if (!png_image_write_to_memory(&png, nullptr, &size, 0, values.ptr(), 0, nullptr)) {
			r_error = RTR("Could not encode the PNG file.");
			return ERR_CANT_CREATE;
		}
		Vector<uint8_t> bytes;
		bytes.resize(size);
		if (!png_image_write_to_memory(&png, bytes.ptrw(), &size, 0, values.ptr(), 0, nullptr)) {
			r_error = RTR("Could not encode the PNG file.");
			return ERR_CANT_CREATE;
		}
		Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
		if (f.is_null()) {
			r_error = RTR("Could not open the file for writing.");
			return ERR_FILE_CANT_WRITE;
		}
		f->store_buffer(bytes.ptr(), size);
		return OK;
	}

	r_error = RTR("Heightmaps can be exported as .exr, .png or .r16.");
	return ERR_FILE_UNRECOGNIZED;
}

} // namespace TerrainHeightmapIO
