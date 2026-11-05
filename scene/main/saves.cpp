/**************************************************************************/
/*  saves.cpp                                                             */
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

#include "saves.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/compression.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/variant/variant_utility.h"
#include "scene/main/save_crypto.h"
#include "scene/main/save_state.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/packed_scene.h"
#include "servers/display/display_server.h"

Saves *Saves::singleton = nullptr;

namespace {

const uint8_t MAGIC[4] = { 'A', 'S', 'A', 'V' };
const uint8_t CONTAINER_VERSION = 1;
const char *RESOURCE_MARKER = "__res";
const char *NODE_MARKER = "__node";
const int MAX_DEPTH = 64;
// What a compressed body may claim to unpack to, so a damaged length is not
// believed.
const uint64_t MAX_BODY_SIZE = 1024ull * 1024 * 1024;

// Iterations of the key derivation. The project key is long and random, so
// stretching it only has to be cheap; a password is what needs the work.
const uint32_t KEY_ITERATIONS = 10000;
const uint32_t PASSWORD_ITERATIONS = 100000;

struct ByteWriter {
	Vector<uint8_t> bytes;

	void u8(uint8_t p_value) { bytes.push_back(p_value); }
	void u32(uint32_t p_value) {
		for (int i = 0; i < 4; i++) {
			bytes.push_back(uint8_t(p_value >> (8 * i)));
		}
	}
	void u64(uint64_t p_value) {
		for (int i = 0; i < 8; i++) {
			bytes.push_back(uint8_t(p_value >> (8 * i)));
		}
	}
	void append(const uint8_t *p_data, size_t p_size) {
		const int64_t at = bytes.size();
		bytes.resize(at + p_size);
		if (p_size > 0) {
			memcpy(bytes.ptrw() + at, p_data, p_size);
		}
	}
};

struct ByteReader {
	const uint8_t *data;
	size_t size;
	size_t position = 0;

	ByteReader(const Vector<uint8_t> &p_bytes) :
			data(p_bytes.ptr()), size(p_bytes.size()) {}

	bool has(size_t p_count) const { return size - position >= p_count; }
	bool u8(uint8_t &r_value) {
		if (!has(1)) {
			return false;
		}
		r_value = data[position++];
		return true;
	}
	bool u32(uint32_t &r_value) {
		if (!has(4)) {
			return false;
		}
		r_value = 0;
		for (int i = 0; i < 4; i++) {
			r_value |= uint32_t(data[position++]) << (8 * i);
		}
		return true;
	}
	bool u64(uint64_t &r_value) {
		if (!has(8)) {
			return false;
		}
		r_value = 0;
		for (int i = 0; i < 8; i++) {
			r_value |= uint64_t(data[position++]) << (8 * i);
		}
		return true;
	}
	const uint8_t *take(size_t p_count) {
		if (!has(p_count)) {
			return nullptr;
		}
		const uint8_t *at = data + position;
		position += p_count;
		return at;
	}
};

} // namespace

// --- Values -------------------------------------------------------------------

Variant Saves::to_storable(const Variant &p_value, int p_depth, const String &p_context) {
	if (p_depth > MAX_DEPTH) {
		WARN_PRINT(vformat("Saves: \"%s\" is nested too deeply to save, so it is left out.", p_context));
		return Variant();
	}

	switch (p_value.get_type()) {
		case Variant::OBJECT: {
			Object *object = p_value;
			if (!object) {
				return Variant();
			}
			if (Resource *resource = Object::cast_to<Resource>(object)) {
				const String path = resource->get_path();
				if (!path.is_empty() && !path.contains("::")) {
					Dictionary marker;
					marker[RESOURCE_MARKER] = path;
					return marker;
				}
				WARN_PRINT(vformat("Saves: \"%s\" holds a %s that is not a file on its own, so it is left out. Save the resource to a file, or keep what matters from it in plain properties.", p_context, resource->get_class()));
				return Variant();
			}
			if (Node *node = Object::cast_to<Node>(object)) {
				if (node->is_inside_tree()) {
					Dictionary marker;
					marker[NODE_MARKER] = node->get_path();
					return marker;
				}
				return Variant();
			}
			WARN_PRINT(vformat("Saves: \"%s\" holds a %s, which can not be saved, so it is left out.", p_context, object->get_class()));
			return Variant();
		}
		case Variant::DICTIONARY: {
			const Dictionary source = p_value;
			Dictionary result;
			for (const Variant &key : source.get_key_list()) {
				if (key.get_type() == Variant::OBJECT) {
					continue;
				}
				result[key] = to_storable(source[key], p_depth + 1, p_context);
			}
			return result;
		}
		case Variant::ARRAY: {
			const Array source = p_value;
			Array result;
			for (int i = 0; i < source.size(); i++) {
				result.push_back(to_storable(source[i], p_depth + 1, p_context));
			}
			return result;
		}
		case Variant::CALLABLE:
		case Variant::SIGNAL:
		case Variant::RID: {
			return Variant();
		}
		default: {
			return p_value;
		}
	}
}

Variant Saves::from_storable(const Variant &p_value, int p_depth) {
	if (p_depth > MAX_DEPTH) {
		return Variant();
	}

	switch (p_value.get_type()) {
		case Variant::DICTIONARY: {
			const Dictionary source = p_value;
			if (source.size() == 1 && source.has(RESOURCE_MARKER) && source[RESOURCE_MARKER].get_type() == Variant::STRING) {
				const String path = source[RESOURCE_MARKER];
				// A save only ever names the game's own resources.
				if (!path.begins_with("res://") || !ResourceLoader::exists(path)) {
					WARN_PRINT(vformat("Saves: the saved resource \"%s\" is not in this game, so it is left out.", path));
					return Variant();
				}
				return ResourceLoader::load(path);
			}
			if (source.size() == 1 && source.has(NODE_MARKER) && source[NODE_MARKER].get_type() == Variant::NODE_PATH) {
				SceneTree *tree = SceneTree::get_singleton();
				if (!tree) {
					return Variant();
				}
				return tree->get_root()->get_node_or_null(source[NODE_MARKER]);
			}
			Dictionary result;
			for (const Variant &key : source.get_key_list()) {
				result[key] = from_storable(source[key], p_depth + 1);
			}
			return result;
		}
		case Variant::ARRAY: {
			const Array source = p_value;
			Array result;
			for (int i = 0; i < source.size(); i++) {
				result.push_back(from_storable(source[i], p_depth + 1));
			}
			return result;
		}
		default: {
			return p_value;
		}
	}
}

// --- Files --------------------------------------------------------------------

String Saves::_get_project_key() const {
	const String key = GLOBAL_GET("save/encryption/key");
	if (!key.is_empty()) {
		return key;
	}
	// A project that never made a key still gets its saves protected from being
	// edited by hand; the key only differs from project to project.
	return "asosdot|" + String(GLOBAL_GET("application/config/name"));
}

void Saves::ensure_project_key() {
#ifdef TOOLS_ENABLED
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (!settings || !bool(GLOBAL_GET("save/encryption/enabled"))) {
		return;
	}
	if (!String(GLOBAL_GET("save/encryption/key")).is_empty()) {
		return;
	}

	uint8_t bytes[32];
	if (SaveCrypto::random_bytes(bytes, 32) != OK) {
		return;
	}
	const String key = String::hex_encode_buffer(bytes, 32);
	settings->set_setting("save/encryption/key", key);
	settings->save();
#endif
}

String Saves::_get_directory() const {
	String directory = GLOBAL_GET("save/path");
	return directory.is_empty() ? String("user://saves") : directory;
}

bool Saves::_is_valid_slot(const String &p_slot) const {
	if (p_slot.is_empty() || p_slot.length() > 64 || p_slot.begins_with(".")) {
		return false;
	}
	for (int i = 0; i < p_slot.length(); i++) {
		const char32_t c = p_slot[i];
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ' ';
		if (!ok) {
			return false;
		}
	}
	return true;
}

String Saves::_get_slot_path(const String &p_slot) const {
	return _get_directory().path_join(p_slot + ".save");
}

String Saves::get_slot_path(const String &p_slot) const {
	ERR_FAIL_COND_V_MSG(!_is_valid_slot(p_slot), String(), vformat("\"%s\" is not a slot name: use letters, digits, space, dot, dash and underscore.", p_slot));
	return _get_slot_path(p_slot);
}

Error Saves::_pack(const Dictionary &p_header, const Dictionary &p_body, const String &p_password, Vector<uint8_t> &r_file) const {
	const bool json = int(GLOBAL_GET("save/format")) == 1;
	// A JSON save is there to be read, so it is left as text.
	const bool compressed = bool(GLOBAL_GET("save/compression")) && !json;
	const bool encrypted = bool(GLOBAL_GET("save/encryption/enabled"));
	const bool with_password = encrypted && !p_password.is_empty();

	Vector<uint8_t> body;
	if (json) {
		body = JSON::stringify(JSON::from_native(p_body), "", true, true).to_utf8_buffer();
	} else {
		body = VariantUtilityFunctions::var_to_bytes(p_body);
	}

	Vector<uint8_t> payload;
	if (compressed) {
		const int64_t max_size = Compression::get_max_compressed_buffer_size(body.size(), Compression::MODE_ZSTD);
		Vector<uint8_t> packed;
		packed.resize(max_size);
		const int64_t size = Compression::compress(packed.ptrw(), body.ptr(), body.size(), Compression::MODE_ZSTD);
		ERR_FAIL_COND_V_MSG(size < 0, ERR_FILE_CANT_WRITE, "Could not compress the save.");

		ByteWriter w;
		w.u64(body.size());
		w.append(packed.ptr(), size);
		payload = w.bytes;
	} else {
		payload = body;
	}

	const Vector<uint8_t> header = VariantUtilityFunctions::var_to_bytes(p_header);

	ByteWriter file;
	file.append(MAGIC, 4);
	file.u8(CONTAINER_VERSION);
	file.u8(uint8_t((compressed ? FLAG_COMPRESSED : 0) | (encrypted ? FLAG_ENCRYPTED : 0) | (with_password ? FLAG_PASSWORD : 0) | (json ? FLAG_JSON : 0)));
	file.u8(0);
	file.u8(0);
	file.u32(header.size());
	file.append(header.ptr(), header.size());

	if (encrypted) {
		uint8_t salt[SaveCrypto::SALT_SIZE];
		uint8_t iv[SaveCrypto::IV_SIZE];
		Error err = SaveCrypto::random_bytes(salt, sizeof(salt));
		ERR_FAIL_COND_V(err != OK, err);
		err = SaveCrypto::random_bytes(iv, sizeof(iv));
		ERR_FAIL_COND_V(err != OK, err);

		const uint32_t iterations = with_password ? PASSWORD_ITERATIONS : KEY_ITERATIONS;
		SaveCrypto::Keys keys;
		SaveCrypto::derive_keys(_get_project_key(), p_password, salt, iterations, keys);

		Vector<uint8_t> cipher;
		cipher.resize(payload.size());
		err = SaveCrypto::crypt(keys, iv, payload.ptr(), cipher.ptrw(), payload.size(), true);
		ERR_FAIL_COND_V(err != OK, err);

		file.u32(iterations);
		file.append(salt, sizeof(salt));
		file.append(iv, sizeof(iv));
		file.u64(cipher.size());
		file.append(cipher.ptr(), cipher.size());

		uint8_t mac[SaveCrypto::MAC_SIZE];
		SaveCrypto::hmac_sha256(keys.mac, SaveCrypto::KEY_SIZE, file.bytes.ptr(), file.bytes.size(), mac);
		file.append(mac, sizeof(mac));
	} else {
		file.u64(payload.size());
		file.append(payload.ptr(), payload.size());

		// Not secret, but a damaged file is told from a good one.
		uint8_t hash[32];
		SaveCrypto::sha256(file.bytes.ptr(), file.bytes.size(), hash);
		file.append(hash, sizeof(hash));
	}

	r_file = file.bytes;
	return OK;
}

Error Saves::_unpack(const Vector<uint8_t> &p_file, const String &p_password, bool p_header_only, Dictionary &r_header, Dictionary &r_body) const {
	ByteReader r(p_file);

	const uint8_t *magic = r.take(4);
	if (!magic || memcmp(magic, MAGIC, 4) != 0) {
		return ERR_FILE_UNRECOGNIZED;
	}
	uint8_t version = 0;
	uint8_t flags = 0;
	uint8_t reserved = 0;
	if (!r.u8(version) || !r.u8(flags) || !r.u8(reserved) || !r.u8(reserved)) {
		return ERR_FILE_CORRUPT;
	}
	if (version != CONTAINER_VERSION) {
		return ERR_FILE_UNRECOGNIZED;
	}

	uint32_t header_size = 0;
	if (!r.u32(header_size) || !r.has(header_size)) {
		return ERR_FILE_CORRUPT;
	}
	const uint8_t *header_bytes = r.take(header_size);
	Vector<uint8_t> header_copy;
	header_copy.resize(header_size);
	if (header_size > 0) {
		memcpy(header_copy.ptrw(), header_bytes, header_size);
	}
	const Variant header = VariantUtilityFunctions::bytes_to_var(header_copy);
	if (header.get_type() != Variant::DICTIONARY) {
		return ERR_FILE_CORRUPT;
	}
	r_header = header;
	r_header["encrypted"] = bool(flags & FLAG_ENCRYPTED);
	r_header["password_required"] = bool(flags & FLAG_PASSWORD);
	if (p_header_only) {
		return OK;
	}

	const bool encrypted = flags & FLAG_ENCRYPTED;
	// Where encryption is asked for, a file without it is not taken: it is the
	// way a hand edited save would be slipped in.
	if (bool(GLOBAL_GET("save/encryption/enabled")) && !encrypted) {
		return ERR_FILE_CORRUPT;
	}

	Vector<uint8_t> payload;
	if (encrypted) {
		uint32_t iterations = 0;
		if (!r.u32(iterations) || iterations == 0 || iterations > 10000000) {
			return ERR_FILE_CORRUPT;
		}
		const uint8_t *salt = r.take(SaveCrypto::SALT_SIZE);
		const uint8_t *iv = r.take(SaveCrypto::IV_SIZE);
		uint64_t payload_size = 0;
		if (!salt || !iv || !r.u64(payload_size) || payload_size > MAX_BODY_SIZE || !r.has(payload_size + SaveCrypto::MAC_SIZE)) {
			return ERR_FILE_CORRUPT;
		}
		const uint8_t *cipher = r.take(payload_size);
		const uint8_t *mac = r.take(SaveCrypto::MAC_SIZE);

		if ((flags & FLAG_PASSWORD) && p_password.is_empty()) {
			return ERR_UNAUTHORIZED;
		}

		SaveCrypto::Keys keys;
		SaveCrypto::derive_keys(_get_project_key(), (flags & FLAG_PASSWORD) ? p_password : String(), salt, iterations, keys);

		uint8_t expected[SaveCrypto::MAC_SIZE];
		SaveCrypto::hmac_sha256(keys.mac, SaveCrypto::KEY_SIZE, p_file.ptr(), r.position - SaveCrypto::MAC_SIZE, expected);
		if (!SaveCrypto::equal(expected, mac, SaveCrypto::MAC_SIZE)) {
			// A wrong password and a changed file are the same to look at.
			return ERR_FILE_CORRUPT;
		}

		payload.resize(payload_size);
		Error err = SaveCrypto::crypt(keys, iv, cipher, payload.ptrw(), payload_size, false);
		if (err != OK) {
			return ERR_FILE_CORRUPT;
		}
	} else {
		uint64_t payload_size = 0;
		if (!r.u64(payload_size) || payload_size > MAX_BODY_SIZE || !r.has(payload_size + 32)) {
			return ERR_FILE_CORRUPT;
		}
		const uint8_t *plain = r.take(payload_size);
		const uint8_t *hash = r.take(32);

		uint8_t expected[32];
		SaveCrypto::sha256(p_file.ptr(), r.position - 32, expected);
		if (!SaveCrypto::equal(expected, hash, 32)) {
			return ERR_FILE_CORRUPT;
		}
		payload.resize(payload_size);
		if (payload_size > 0) {
			memcpy(payload.ptrw(), plain, payload_size);
		}
	}

	Vector<uint8_t> body;
	if (flags & FLAG_COMPRESSED) {
		ByteReader pr(payload);
		uint64_t body_size = 0;
		if (!pr.u64(body_size) || body_size > MAX_BODY_SIZE) {
			return ERR_FILE_CORRUPT;
		}
		body.resize(body_size);
		const int64_t got = Compression::decompress(body.ptrw(), body_size, payload.ptr() + pr.position, payload.size() - pr.position, Compression::MODE_ZSTD);
		if (got != int64_t(body_size)) {
			return ERR_FILE_CORRUPT;
		}
	} else {
		body = payload;
	}

	Variant content;
	if (flags & FLAG_JSON) {
		JSON json;
		if (json.parse(String::utf8((const char *)body.ptr(), body.size())) != OK) {
			return ERR_FILE_CORRUPT;
		}
		content = JSON::to_native(json.get_data());
	} else {
		content = VariantUtilityFunctions::bytes_to_var(body);
	}
	if (content.get_type() != Variant::DICTIONARY) {
		return ERR_FILE_CORRUPT;
	}
	r_body = content;
	return OK;
}

Error Saves::_write_file(const String &p_slot, const Vector<uint8_t> &p_file) const {
	const String directory = _get_directory();
	Error err = DirAccess::make_dir_recursive_absolute(directory);
	ERR_FAIL_COND_V_MSG(err != OK && err != ERR_ALREADY_EXISTS, err, vformat("Could not make the save folder \"%s\".", directory));

	const String path = _get_slot_path(p_slot);
	const String temporary = path + ".tmp";

	// Written beside the save and moved over it, so a crash half way through leaves
	// the old save whole.
	{
		Ref<FileAccess> file = FileAccess::open(temporary, FileAccess::WRITE, &err);
		ERR_FAIL_COND_V_MSG(file.is_null(), err, vformat("Could not write \"%s\".", temporary));
		file->store_buffer(p_file.ptr(), p_file.size());
		file->flush();
		err = file->get_error();
		file.unref();
		if (err != OK) {
			return err;
		}
	}

	Ref<DirAccess> dir = DirAccess::create_for_path(directory);
	ERR_FAIL_COND_V(dir.is_null(), ERR_CANT_CREATE);

	const int backups = CLAMP(int(GLOBAL_GET("save/backups")), 0, 9);
	if (dir->file_exists(path)) {
		if (backups > 0) {
			for (int i = backups; i >= 1; i--) {
				const String from = i == 1 ? path : path + ".bak" + itos(i - 1);
				const String to = path + ".bak" + itos(i);
				if (!dir->file_exists(from)) {
					continue;
				}
				if (dir->file_exists(to)) {
					dir->remove(to);
				}
				dir->rename(from, to);
			}
		} else {
			dir->remove(path);
		}
	}
	err = dir->rename(temporary, path);
	if (err != OK) {
		dir->remove(temporary);
		ERR_FAIL_V_MSG(err, vformat("Could not move the save into place at \"%s\".", path));
	}
	return OK;
}

Error Saves::_read_slot(const String &p_slot, const String &p_password, bool p_header_only, Dictionary &r_content) const {
	if (!_is_valid_slot(p_slot)) {
		return ERR_INVALID_PARAMETER;
	}

	const String path = _get_slot_path(p_slot);
	const int backups = CLAMP(int(GLOBAL_GET("save/backups")), 0, 9);

	Error first_error = ERR_FILE_NOT_FOUND;
	for (int attempt = 0; attempt <= backups; attempt++) {
		const String file_path = attempt == 0 ? path : path + ".bak" + itos(attempt);
		if (!FileAccess::exists(file_path)) {
			if (attempt == 0) {
				return ERR_FILE_NOT_FOUND;
			}
			continue;
		}

		Error err = OK;
		const Vector<uint8_t> file = FileAccess::get_file_as_bytes(file_path, &err);
		Dictionary header;
		Dictionary body;
		if (err == OK) {
			err = _unpack(file, p_password.is_empty() ? password : p_password, p_header_only, header, body);
		}

		if (err == OK) {
			r_content = header;
			r_content["size"] = (int64_t)file.size();
			if (!p_header_only) {
				r_content["data"] = body.get("data", Dictionary());
				r_content["states"] = body.get("states", Dictionary());
				r_content["visited"] = body.get("visited", PackedStringArray());
			}
			if (attempt > 0) {
				// What the save was before the last one broke it.
				r_content["recovered"] = true;
				WARN_PRINT(vformat("Saves: \"%s\" was damaged, so the backup before it was read instead.", p_slot));
			}
			return OK;
		}

		if (attempt == 0) {
			first_error = err;
		}
		// Only a damaged file has a backup worth trying: a missing password or an
		// unrecognized format would be the same in every copy.
		if (err != ERR_FILE_CORRUPT) {
			return err;
		}
	}
	return first_error;
}

Dictionary Saves::_describe(const String &p_slot, const Dictionary &p_header, int64_t p_size) const {
	Dictionary info;
	info["slot"] = p_slot;
	info["version"] = p_header.get("version", 0);
	info["timestamp"] = p_header.get("timestamp", 0);
	info["play_time"] = p_header.get("play_time", 0.0);
	info["scene"] = p_header.get("scene", String());
	info["info"] = from_storable(p_header.get("info", Dictionary()), 0);
	info["encrypted"] = p_header.get("encrypted", false);
	info["password_required"] = p_header.get("password_required", false);
	info["size"] = p_size;
	info["path"] = _get_slot_path(p_slot);

	Ref<Image> thumbnail;
	const Vector<uint8_t> png = p_header.get("thumbnail", PackedByteArray());
	if (!png.is_empty()) {
		thumbnail.instantiate();
		if (thumbnail->load_png_from_buffer(png) != OK) {
			thumbnail.unref();
		}
	}
	info["thumbnail"] = thumbnail;
	return info;
}

Vector<uint8_t> Saves::_capture_thumbnail() const {
	Vector<uint8_t> png;
	if (!bool(GLOBAL_GET("save/thumbnail/enabled")) || !DisplayServer::get_singleton() || DisplayServer::get_singleton()->get_name() == "headless") {
		return png;
	}
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		return png;
	}
	Ref<Image> image = tree->get_root()->get_texture()->get_image();
	if (image.is_null() || image->is_empty()) {
		return png;
	}

	const Vector2i size = GLOBAL_GET("save/thumbnail/size");
	if (size.x > 0 && size.y > 0) {
		// Fitted inside the size, keeping the shape of the screen.
		const real_t scale = MIN(real_t(size.x) / image->get_width(), real_t(size.y) / image->get_height());
		if (scale < 1.0) {
			image->resize(MAX(1, int(image->get_width() * scale)), MAX(1, int(image->get_height() * scale)), Image::INTERPOLATE_BILINEAR);
		}
	}
	image->convert(Image::FORMAT_RGB8);
	return image->save_png_to_buffer();
}

// --- Capture and apply --------------------------------------------------------

String Saves::_get_level() const {
	SceneTree *tree = SceneTree::get_singleton();
	Node *scene = tree ? tree->get_current_scene() : nullptr;
	return scene ? scene->get_scene_file_path() : String();
}

Dictionary Saves::_capture_live() {
	const String level = _get_level();

	LocalVector<SaveState *> states(SaveState::get_states_in_tree());
	states.sort_custom<Node::Comparator>();

	HashSet<String> seen;
	for (SaveState *state : states) {
		if (!state->get_target_node()) {
			continue;
		}
		const String id = state->get_state_id();
		if (seen.has(id)) {
			ERR_PRINT(vformat("Saves: two SaveStates are saved as \"%s\", so \"%s\" is left out. Give one of them its own Save Id.", id, state->get_path()));
			continue;
		}
		seen.insert(id);

		Dictionary captured = state->capture();
		SceneTree *tree = SceneTree::get_singleton();
		Node *scene = tree ? tree->get_current_scene() : nullptr;
		const bool in_scene = scene && (scene == state->get_target_node() || scene->is_ancestor_of(state->get_target_node()));
		captured["level"] = in_scene ? level : String();
		world[id] = captured;
	}

	if (!level.is_empty() && !visited.has(level)) {
		visited.push_back(level);
	}
	return world;
}

void Saves::_spawn(const String &p_id, const Dictionary &p_state) {
	const String scene_path = p_state.get("scene", String());
	if (!scene_path.begins_with("res://") || !ResourceLoader::exists(scene_path)) {
		WARN_PRINT(vformat("Saves: \"%s\" was saved as a spawnable node of \"%s\", which is not in this game, so it is not brought back.", p_id, scene_path));
		return;
	}
	Ref<PackedScene> scene = ResourceLoader::load(scene_path);
	ERR_FAIL_COND(scene.is_null());
	Node *instance = scene->instantiate();
	ERR_FAIL_NULL(instance);

	// The SaveState that keeps the root of the instance is the one to hand the id.
	SaveState *keeper = nullptr;
	TypedArray<Node> candidates = instance->find_children("*", "SaveState", true, false);
	for (int i = 0; i < candidates.size(); i++) {
		SaveState *candidate = Object::cast_to<SaveState>(candidates[i]);
		if (candidate && candidate->get_target_node() == instance) {
			keeper = candidate;
			break;
		}
	}
	if (!keeper) {
		WARN_PRINT(vformat("Saves: \"%s\" has no SaveState on its root, so \"%s\" can not be brought back.", scene_path, p_id));
		memdelete(instance);
		return;
	}
	keeper->set_save_id(p_id);
	instance->set_name(String(p_state.get("name", String(instance->get_name()))));

	SceneTree *tree = SceneTree::get_singleton();
	Node *parent = nullptr;
	if (p_state.has("parent")) {
		parent = tree->get_root()->get_node_or_null(p_state["parent"]);
	}
	if (!parent) {
		parent = tree->get_current_scene();
	}
	if (!parent) {
		memdelete(instance);
		return;
	}
	// Ready on the way in, which is where it is given its state.
	parent->add_child(instance);
}

void Saves::_apply_world() {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree) {
		return;
	}
	const String level = _get_level();

	HashSet<String> live;
	for (const SaveState *state : SaveState::get_states_in_tree()) {
		if (state->get_target_node()) {
			live.insert(state->get_state_id());
		}
	}

	// What the save has of this scene and the scene does not have is brought back.
	struct Spawn {
		String id;
		Dictionary state;
	};
	LocalVector<Spawn> to_spawn;
	for (const KeyValue<Variant, Variant> &kv : world) {
		const String id = kv.key;
		const Dictionary state = kv.value;
		if (!live.has(id) && bool(state.get("spawnable", false)) && !level.is_empty() && String(state.get("level", String())) == level) {
			to_spawn.push_back({ id, state });
		}
	}
	for (const Spawn &entry : to_spawn) {
		_spawn(entry.id, entry.state);
	}

	// The save has seen this scene, so a spawnable node it has no state for was
	// gone by then: killed, or picked up.
	const bool seen_level = !level.is_empty() && visited.has(level);
	LocalVector<SaveState *> current(SaveState::get_states_in_tree());
	for (SaveState *state : current) {
		Node *target = state->get_target_node();
		if (!target) {
			continue;
		}
		const String id = state->get_state_id();
		if (world.has(id)) {
			if (state->applied_generation != generation) {
				state->applied_generation = generation;
				state->apply(world[id]);
			}
		} else if (seen_level && state->is_spawnable() && !target->is_queued_for_deletion()) {
			target->queue_free();
		}
	}
}

void Saves::_state_entered(SaveState *p_state) {
	if (world.is_empty() || p_state->applied_generation == generation) {
		return;
	}
	Node *target = p_state->get_target_node();
	if (!target) {
		return;
	}

	const String id = p_state->get_state_id();
	if (world.has(id)) {
		p_state->applied_generation = generation;
		p_state->apply(world[id]);
	} else if (p_state->is_spawnable() && !id.begins_with("spawn-") && !_get_level().is_empty() && visited.has(_get_level())) {
		// The save has seen this scene and has nothing of this node: it was gone by
		// then. A node a script made has a spawn id and is not this one's business;
		// it was made after the save.
		target->queue_free();
	}
}

void Saves::_on_scene_changed() {
	_apply_world();
	if (!pending_loaded_slot.is_empty()) {
		const String slot = pending_loaded_slot;
		pending_loaded_slot = String();
		emit_signal(SNAME("loaded"), slot);
	}
}

void Saves::_tick(double p_delta, bool p_paused) {
	if (!p_paused) {
		play_time += p_delta;
		if (autosave_interval > 0.0) {
			autosave_timer += p_delta;
			if (autosave_timer >= autosave_interval) {
				autosave_timer = 0.0;
				save(autosave_slot);
			}
		}
	}

	SceneTree *tree = SceneTree::get_singleton();
	Node *scene = tree ? tree->get_current_scene() : nullptr;
	const ObjectID id = scene ? scene->get_instance_id() : ObjectID();
	if (!scene_tracked) {
		scene_tracked = true;
		last_scene = id;
	} else if (id != last_scene) {
		last_scene = id;
		if (scene) {
			_on_scene_changed();
		}
	}
}

// --- The API ------------------------------------------------------------------

Error Saves::save(const String &p_slot, const Dictionary &p_info, const String &p_password) {
	ERR_FAIL_COND_V_MSG(!_is_valid_slot(p_slot), ERR_INVALID_PARAMETER, vformat("\"%s\" is not a slot name: use letters, digits, space, dot, dash and underscore.", p_slot));

	// A script gets to bring Saves.data up to date.
	emit_signal(SNAME("saving"), p_slot);

	_capture_live();

	Dictionary header;
	header["slot"] = p_slot;
	header["version"] = save_version;
	header["timestamp"] = (int64_t)Time::get_singleton()->get_unix_time_from_system();
	header["play_time"] = play_time;
	header["scene"] = _get_level();
	header["info"] = to_storable(p_info, 0, "info");
	header["thumbnail"] = _capture_thumbnail();

	Dictionary body;
	body["data"] = to_storable(data, 0, "Saves.data");
	body["states"] = world;
	body["visited"] = visited;

	Vector<uint8_t> file;
	Error err = _pack(header, body, p_password.is_empty() ? password : p_password, file);
	if (err == OK) {
		err = _write_file(p_slot, file);
	}
	emit_signal(SNAME("saved"), p_slot, (int)err);
	return err;
}

Error Saves::load_slot(const String &p_slot, const String &p_password, bool p_restore_scene) {
	emit_signal(SNAME("loading"), p_slot);

	Dictionary content;
	Error err = _read_slot(p_slot, p_password, false, content);
	if (err == OK && int(content.get("version", 0)) > save_version) {
		// Made by a newer version of the game; reading it would lose what it holds.
		err = ERR_FILE_UNRECOGNIZED;
	}
	if (err != OK) {
		emit_signal(SNAME("load_failed"), p_slot, (int)err);
		return err;
	}

	const int from_version = content.get("version", 0);
	if (from_version < save_version) {
		// The game gets to bring an old save up to date before it is used.
		emit_signal(SNAME("migrate"), content, from_version);
	}

	data = from_storable(content.get("data", Dictionary()), 0);
	world = content.get("states", Dictionary());
	visited = content.get("visited", PackedStringArray());
	play_time = content.get("play_time", 0.0);
	generation++;

	const String scene_path = content.get("scene", String());
	SceneTree *tree = SceneTree::get_singleton();
	if (tree && p_restore_scene && !scene_path.is_empty() && _get_level() != scene_path) {
		// Applied, and the load reported, once the scene is in.
		pending_loaded_slot = p_slot;
		err = tree->change_scene_to_file(scene_path);
		if (err != OK) {
			pending_loaded_slot = String();
			emit_signal(SNAME("load_failed"), p_slot, (int)err);
			return err;
		}
		return OK;
	}

	_apply_world();
	emit_signal(SNAME("loaded"), p_slot);
	return OK;
}

Dictionary Saves::read(const String &p_slot, const String &p_password) const {
	Dictionary content;
	const Error err = _read_slot(p_slot, p_password, false, content);
	if (err != OK) {
		Dictionary failure;
		failure["error"] = (int)err;
		return failure;
	}

	Dictionary result = _describe(p_slot, content, content.get("size", 0));
	result["data"] = from_storable(content.get("data", Dictionary()), 0);
	result["states"] = content.get("states", Dictionary());
	result["error"] = (int)OK;
	return result;
}

Dictionary Saves::get_info(const String &p_slot) const {
	Dictionary content;
	if (_read_slot(p_slot, String(), true, content) != OK) {
		return Dictionary();
	}
	return _describe(p_slot, content, content.get("size", 0));
}

PackedStringArray Saves::list_slots() const {
	struct Entry {
		String slot;
		int64_t timestamp;
	};
	struct NewestFirst {
		bool operator()(const Entry &p_a, const Entry &p_b) const {
			return p_a.timestamp != p_b.timestamp ? p_a.timestamp > p_b.timestamp : p_a.slot < p_b.slot;
		}
	};
	Vector<Entry> entries;

	Ref<DirAccess> dir = DirAccess::open(_get_directory());
	if (dir.is_null()) {
		return PackedStringArray();
	}
	dir->list_dir_begin();
	for (String file = dir->get_next(); !file.is_empty(); file = dir->get_next()) {
		if (dir->current_is_dir() || file.get_extension() != "save") {
			continue;
		}
		const String slot = file.get_basename();
		if (!_is_valid_slot(slot)) {
			continue;
		}
		Entry entry;
		entry.slot = slot;
		entry.timestamp = 0;
		Dictionary content;
		if (_read_slot(slot, String(), true, content) == OK) {
			entry.timestamp = content.get("timestamp", 0);
		}
		entries.push_back(entry);
	}
	dir->list_dir_end();

	// Newest first.
	entries.sort_custom<NewestFirst>();
	PackedStringArray result;
	for (const Entry &entry : entries) {
		result.push_back(entry.slot);
	}
	return result;
}

bool Saves::has_slot(const String &p_slot) const {
	return _is_valid_slot(p_slot) && FileAccess::exists(_get_slot_path(p_slot));
}

Error Saves::delete_slot(const String &p_slot) {
	ERR_FAIL_COND_V(!_is_valid_slot(p_slot), ERR_INVALID_PARAMETER);
	const String path = _get_slot_path(p_slot);
	if (!FileAccess::exists(path)) {
		return ERR_FILE_NOT_FOUND;
	}

	Ref<DirAccess> dir = DirAccess::create_for_path(_get_directory());
	ERR_FAIL_COND_V(dir.is_null(), ERR_CANT_OPEN);
	Error err = dir->remove(path);
	for (int i = 1; i <= 9; i++) {
		const String backup = path + ".bak" + itos(i);
		if (dir->file_exists(backup)) {
			dir->remove(backup);
		}
	}
	return err;
}

Error Saves::copy_slot(const String &p_from, const String &p_to) {
	ERR_FAIL_COND_V(!_is_valid_slot(p_from) || !_is_valid_slot(p_to), ERR_INVALID_PARAMETER);
	Error err = OK;
	const Vector<uint8_t> file = FileAccess::get_file_as_bytes(_get_slot_path(p_from), &err);
	if (err != OK) {
		return err;
	}
	return _write_file(p_to, file);
}

void Saves::stash() {
	_capture_live();
}

Dictionary Saves::snapshot() {
	_capture_live();
	Dictionary result;
	result["states"] = world.duplicate(true);
	result["visited"] = visited;
	return result;
}

void Saves::restore(const Dictionary &p_snapshot) {
	world = Dictionary(p_snapshot.get("states", Dictionary())).duplicate(true);
	visited = p_snapshot.get("visited", PackedStringArray());
	generation++;
	_apply_world();
}

void Saves::reset() {
	data.clear();
	world.clear();
	visited.clear();
	play_time = 0.0;
	autosave_timer = 0.0;
	generation++;
}

void Saves::set_data(const Dictionary &p_data) {
	data = p_data;
}

Dictionary Saves::get_data() const {
	return data;
}

void Saves::set_save_version(int p_version) {
	save_version = MAX(p_version, 1);
}

int Saves::get_save_version() const {
	return save_version;
}

void Saves::set_play_time(double p_seconds) {
	play_time = MAX(p_seconds, 0.0);
}

double Saves::get_play_time() const {
	return play_time;
}

void Saves::set_password(const String &p_password) {
	password = p_password;
}

String Saves::get_password() const {
	return password;
}

void Saves::set_autosave_interval(double p_seconds) {
	autosave_interval = MAX(p_seconds, 0.0);
	autosave_timer = 0.0;
}

double Saves::get_autosave_interval() const {
	return autosave_interval;
}

void Saves::set_autosave_slot(const String &p_slot) {
	autosave_slot = p_slot;
}

String Saves::get_autosave_slot() const {
	return autosave_slot;
}

void Saves::_bind_methods() {
	ClassDB::bind_method(D_METHOD("save", "slot", "info", "password"), &Saves::save, DEFVAL(Dictionary()), DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("load", "slot", "password", "restore_scene"), &Saves::load_slot, DEFVAL(String()), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("read", "slot", "password"), &Saves::read, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("get_info", "slot"), &Saves::get_info);
	ClassDB::bind_method(D_METHOD("list_slots"), &Saves::list_slots);
	ClassDB::bind_method(D_METHOD("has_slot", "slot"), &Saves::has_slot);
	ClassDB::bind_method(D_METHOD("delete_slot", "slot"), &Saves::delete_slot);
	ClassDB::bind_method(D_METHOD("copy_slot", "from", "to"), &Saves::copy_slot);
	ClassDB::bind_method(D_METHOD("get_slot_path", "slot"), &Saves::get_slot_path);
	ClassDB::bind_method(D_METHOD("stash"), &Saves::stash);
	ClassDB::bind_method(D_METHOD("snapshot"), &Saves::snapshot);
	ClassDB::bind_method(D_METHOD("restore", "snapshot"), &Saves::restore);
	ClassDB::bind_method(D_METHOD("reset"), &Saves::reset);

	ClassDB::bind_method(D_METHOD("set_data", "data"), &Saves::set_data);
	ClassDB::bind_method(D_METHOD("get_data"), &Saves::get_data);
	ClassDB::bind_method(D_METHOD("set_save_version", "version"), &Saves::set_save_version);
	ClassDB::bind_method(D_METHOD("get_save_version"), &Saves::get_save_version);
	ClassDB::bind_method(D_METHOD("set_play_time", "seconds"), &Saves::set_play_time);
	ClassDB::bind_method(D_METHOD("get_play_time"), &Saves::get_play_time);
	ClassDB::bind_method(D_METHOD("set_password", "password"), &Saves::set_password);
	ClassDB::bind_method(D_METHOD("get_password"), &Saves::get_password);
	ClassDB::bind_method(D_METHOD("set_autosave_interval", "seconds"), &Saves::set_autosave_interval);
	ClassDB::bind_method(D_METHOD("get_autosave_interval"), &Saves::get_autosave_interval);
	ClassDB::bind_method(D_METHOD("set_autosave_slot", "slot"), &Saves::set_autosave_slot);
	ClassDB::bind_method(D_METHOD("get_autosave_slot"), &Saves::get_autosave_slot);

	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "data"), "set_data", "get_data");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "save_version"), "set_save_version", "get_save_version");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "play_time"), "set_play_time", "get_play_time");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "password"), "set_password", "get_password");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "autosave_interval"), "set_autosave_interval", "get_autosave_interval");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "autosave_slot"), "set_autosave_slot", "get_autosave_slot");

	ADD_SIGNAL(MethodInfo("saving", PropertyInfo(Variant::STRING, "slot")));
	ADD_SIGNAL(MethodInfo("saved", PropertyInfo(Variant::STRING, "slot"), PropertyInfo(Variant::INT, "error")));
	ADD_SIGNAL(MethodInfo("loading", PropertyInfo(Variant::STRING, "slot")));
	ADD_SIGNAL(MethodInfo("loaded", PropertyInfo(Variant::STRING, "slot")));
	ADD_SIGNAL(MethodInfo("load_failed", PropertyInfo(Variant::STRING, "slot"), PropertyInfo(Variant::INT, "error")));
	ADD_SIGNAL(MethodInfo("migrate", PropertyInfo(Variant::DICTIONARY, "content"), PropertyInfo(Variant::INT, "from_version")));
}

Saves::Saves() {
	singleton = this;

	GLOBAL_DEF("save/path", "user://saves");
	GLOBAL_DEF("save/version", 1);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "save/format", PROPERTY_HINT_ENUM, "Binary,JSON"), 0);
	GLOBAL_DEF("save/compression", true);
	GLOBAL_DEF("save/encryption/enabled", true);
	GLOBAL_DEF(PropertyInfo(Variant::STRING, "save/encryption/key", PROPERTY_HINT_PASSWORD), "");
	GLOBAL_DEF(PropertyInfo(Variant::INT, "save/backups", PROPERTY_HINT_RANGE, "0,9,1"), 1);
	GLOBAL_DEF("save/thumbnail/enabled", true);
	GLOBAL_DEF("save/thumbnail/size", Vector2i(320, 180));

	save_version = MAX(int(GLOBAL_GET("save/version")), 1);
}

Saves::~Saves() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
