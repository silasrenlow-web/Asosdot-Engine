/**************************************************************************/
/*  save_crypto.cpp                                                       */
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

#include "save_crypto.h"

#include "core/crypto/crypto_core.h"

Error SaveCrypto::random_bytes(uint8_t *r_buffer, size_t p_size) {
	CryptoCore::RandomGenerator generator;
	Error err = generator.init();
	ERR_FAIL_COND_V(err != OK, err);
	return generator.get_random_bytes(r_buffer, p_size);
}

void SaveCrypto::sha256(const uint8_t *p_data, size_t p_size, uint8_t r_hash[32]) {
	CryptoCore::sha256(p_data, p_size, r_hash);
}

// HMAC (RFC 2104) over SHA-256, whose block is 64 bytes.
class HMACSHA256 {
	CryptoCore::SHA256Context inner;
	CryptoCore::SHA256Context outer;

public:
	HMACSHA256(const uint8_t *p_key, size_t p_key_size) {
		uint8_t block[64] = {};
		if (p_key_size > 64) {
			CryptoCore::sha256(p_key, p_key_size, block);
		} else if (p_key_size > 0) {
			memcpy(block, p_key, p_key_size);
		}

		uint8_t ipad[64];
		uint8_t opad[64];
		for (int i = 0; i < 64; i++) {
			ipad[i] = block[i] ^ 0x36;
			opad[i] = block[i] ^ 0x5c;
		}
		inner.start();
		inner.update(ipad, 64);
		outer.start();
		outer.update(opad, 64);
	}

	void update(const uint8_t *p_data, size_t p_size) {
		inner.update(p_data, p_size);
	}

	void finish(uint8_t r_mac[32]) {
		uint8_t digest[32];
		inner.finish(digest);
		outer.update(digest, 32);
		outer.finish(r_mac);
	}
};

void SaveCrypto::hmac_sha256(const uint8_t *p_key, size_t p_key_size, const uint8_t *p_data, size_t p_size, uint8_t r_mac[MAC_SIZE]) {
	HMACSHA256 hmac(p_key, p_key_size);
	hmac.update(p_data, p_size);
	hmac.finish(r_mac);
}

void SaveCrypto::pbkdf2_sha256(const uint8_t *p_secret, size_t p_secret_size, const uint8_t *p_salt, size_t p_salt_size, uint32_t p_iterations, uint8_t *r_out, size_t p_out_size) {
	p_iterations = MAX(p_iterations, 1u);
	uint32_t block_index = 1;
	size_t written = 0;

	while (written < p_out_size) {
		// U1 = HMAC(secret, salt || INT(block_index)), Un = HMAC(secret, Un-1).
		uint8_t counter[4] = {
			uint8_t(block_index >> 24), uint8_t(block_index >> 16), uint8_t(block_index >> 8), uint8_t(block_index)
		};
		uint8_t u[32];
		{
			HMACSHA256 hmac(p_secret, p_secret_size);
			hmac.update(p_salt, p_salt_size);
			hmac.update(counter, 4);
			hmac.finish(u);
		}
		uint8_t t[32];
		memcpy(t, u, 32);

		for (uint32_t i = 1; i < p_iterations; i++) {
			uint8_t next[32];
			HMACSHA256 hmac(p_secret, p_secret_size);
			hmac.update(u, 32);
			hmac.finish(next);
			memcpy(u, next, 32);
			for (int k = 0; k < 32; k++) {
				t[k] ^= u[k];
			}
		}

		const size_t take = MIN((size_t)32, p_out_size - written);
		memcpy(r_out + written, t, take);
		written += take;
		block_index++;
	}
}

void SaveCrypto::derive_keys(const String &p_project_key, const String &p_password, const uint8_t p_salt[SALT_SIZE], uint32_t p_iterations, Keys &r_keys) {
	const CharString key = p_project_key.utf8();
	const CharString password = p_password.utf8();

	Vector<uint8_t> secret;
	secret.resize(key.length() + 1 + password.length());
	uint8_t *w = secret.ptrw();
	memcpy(w, key.get_data(), key.length());
	w[key.length()] = 0;
	if (password.length() > 0) {
		memcpy(w + key.length() + 1, password.get_data(), password.length());
	}

	uint8_t out[KEY_SIZE * 2];
	pbkdf2_sha256(secret.ptr(), secret.size(), p_salt, SALT_SIZE, p_iterations, out, sizeof(out));
	memcpy(r_keys.encryption, out, KEY_SIZE);
	memcpy(r_keys.mac, out + KEY_SIZE, KEY_SIZE);
}

Error SaveCrypto::crypt(const Keys &p_keys, const uint8_t p_iv[IV_SIZE], const uint8_t *p_src, uint8_t *r_dst, size_t p_size, bool p_encrypt) {
	CryptoCore::AESContext aes;
	Error err = aes.set_encode_key(p_keys.encryption, 256);
	ERR_FAIL_COND_V(err != OK, err);

	// CFB changes the IV as it goes, so it works on a copy.
	uint8_t iv[IV_SIZE];
	memcpy(iv, p_iv, IV_SIZE);
	return p_encrypt ? aes.encrypt_cfb(p_size, iv, p_src, r_dst) : aes.decrypt_cfb(p_size, iv, p_src, r_dst);
}

bool SaveCrypto::equal(const uint8_t *p_a, const uint8_t *p_b, size_t p_size) {
	uint8_t difference = 0;
	for (size_t i = 0; i < p_size; i++) {
		difference |= p_a[i] ^ p_b[i];
	}
	return difference == 0;
}
