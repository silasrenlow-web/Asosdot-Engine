/**************************************************************************/
/*  save_crypto.h                                                         */
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

#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/variant.h"

// What the save files are encrypted and authenticated with, built from the
// primitives the engine already carries (SHA-256 and AES). Encrypt-then-MAC: the
// file is AES-256-CFB under a random IV, and an HMAC-SHA256 over everything
// before it, header included, is checked before a byte is decrypted, so a
// modified file or a wrong password is refused rather than decoded to garbage.
class SaveCrypto {
public:
	enum {
		SALT_SIZE = 16,
		IV_SIZE = 16,
		MAC_SIZE = 32,
		KEY_SIZE = 32,
	};

	// The two keys a save is protected with.
	struct Keys {
		uint8_t encryption[KEY_SIZE];
		uint8_t mac[KEY_SIZE];
	};

	static Error random_bytes(uint8_t *r_buffer, size_t p_size);

	static void sha256(const uint8_t *p_data, size_t p_size, uint8_t r_hash[32]);
	static void hmac_sha256(const uint8_t *p_key, size_t p_key_size, const uint8_t *p_data, size_t p_size, uint8_t r_mac[MAC_SIZE]);

	// PBKDF2 with HMAC-SHA256, for turning the project key and a password into
	// keys. Fills r_out with p_out_size bytes.
	static void pbkdf2_sha256(const uint8_t *p_secret, size_t p_secret_size, const uint8_t *p_salt, size_t p_salt_size, uint32_t p_iterations, uint8_t *r_out, size_t p_out_size);

	// The project key, then a zero byte, then the password.
	static void derive_keys(const String &p_project_key, const String &p_password, const uint8_t p_salt[SALT_SIZE], uint32_t p_iterations, Keys &r_keys);

	// In place; the IV is only read.
	static Error crypt(const Keys &p_keys, const uint8_t p_iv[IV_SIZE], const uint8_t *p_src, uint8_t *r_dst, size_t p_size, bool p_encrypt);

	// Constant time, so how much of a MAC matched is not something a modified file
	// can be used to find out.
	static bool equal(const uint8_t *p_a, const uint8_t *p_b, size_t p_size);
};
