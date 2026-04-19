/* checksum/shim.c — one-shot MD5 and SHA-1 of a byte buffer, hex-encoded.
 *
 * We go through OpenSSL's EVP layer so the same code picks up future digests
 * (SHA-256, etc.) by swapping the algorithm pointer. The returned hex string
 * is malloc'd; callers free it with svnae_checksum_free_hex (plain free).
 *
 * Scope is deliberately small for Phase 0.4: digest a full buffer and get hex.
 * Streaming (init/update/final like svn_checksum_ctx_t) comes later when we
 * actually need to hash data as it flows through a stream — libsvn_delta
 * and libsvn_fs_fs both want that, so it'll grow then.
 */

#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>

static char *
hex_encode(const unsigned char *bytes, unsigned int len)
{
    static const char hex[] = "0123456789abcdef";
    char *out = malloc(len * 2 + 1);
    if (!out) return NULL;
    for (unsigned int i = 0; i < len; i++) {
        out[i * 2]     = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    out[len * 2] = '\0';
    return out;
}

static char *
digest_hex(const EVP_MD *md, const char *data, int data_len)
{
    if (!md) return NULL;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return NULL;

    unsigned char buf[EVP_MAX_MD_SIZE];
    unsigned int out_len = 0;
    char *hex = NULL;

    if (EVP_DigestInit_ex(ctx, md, NULL) == 1
        && EVP_DigestUpdate(ctx, data, (size_t)data_len) == 1
        && EVP_DigestFinal_ex(ctx, buf, &out_len) == 1) {
        hex = hex_encode(buf, out_len);
    }

    EVP_MD_CTX_free(ctx);
    return hex;
}

/* Aether exposes `string` as C's `const char*` (NUL-terminated). We accept an
 * explicit length because svn checksums have to work on binary data too —
 * the future streaming API will use the same convention. For now the test
 * passes NUL-terminated strings and computes the length on the Aether side. */

char *svnae_md5_hex(const char *data, int data_len)  { return digest_hex(EVP_md5(),  data, data_len); }
char *svnae_sha1_hex(const char *data, int data_len) { return digest_hex(EVP_sha1(), data, data_len); }

void svnae_checksum_free_hex(char *s) { free(s); }
