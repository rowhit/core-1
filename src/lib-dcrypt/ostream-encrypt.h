#ifndef OSTREAM_ENCRYPT_H
#define OSTREAM_ENCRYPT_H

struct dcrypt_public_key;
struct dcrypt_context_symmetric;

/**
 * algorithm is in form AES-256-CBC-SHA1, recommended
 * AES-256-GCM-SHA256
 *
 * Algorithms (both crypto and digest) *MUST* have OID to use it.
 *
 */

enum io_stream_encrypt_flags {
	IO_STREAM_ENC_INTEGRITY_HMAC = 0x1,
	IO_STREAM_ENC_INTEGRITY_AEAD = 0x2,
	IO_STREAM_ENC_INTEGRITY_NONE = 0x4,
	IO_STREAM_ENC_VERSION_1      = 0x8,
};

struct ostream *
o_stream_create_encrypt(struct ostream *output,
	const char *algorithm,
	struct dcrypt_public_key *box_pub,
	enum io_stream_encrypt_flags flags);

/* create context for performing encryption with
   preset crypto context. do not call ctx_sym_init.

   no header or mac is written, just plain crypto
   data.
 */
struct ostream *
o_stream_create_sym_encrypt(struct ostream *output,
	struct dcrypt_context_symmetric *ctx);

#endif
