#include "neg_fingerprint.h"
#include "neg_varint.h"
#include <string.h>
#if defined(__has_include)
#  if __has_include(<openssl/evp.h>)
#    include <openssl/evp.h>
#    define NIP77_USE_OPENSSL_EVP 1
#  elif __has_include(<openssl/sha.h>)
#    include <openssl/sha.h>
#    define NIP77_USE_OPENSSL_SHA 1
#  endif
#else
#  include <openssl/evp.h>
#  define NIP77_USE_OPENSSL_EVP 1
#endif

/*
 * Compute fingerprint per NIP-77 draft:
 * 1) Sum all 32-byte IDs as little-endian 256-bit integers (mod 2^256).
 * 2) Append varint(count) to the 32-byte sum buffer.
 * 3) SHA-256 the concatenation; take the first 16 bytes as the fingerprint.
 */
int neg_fingerprint_compute(const unsigned char *ids, size_t id_stride, size_t count,
                            unsigned char out16[16]) {
  if (!out16) return -1;
  memset(out16, 0, 16);
  if (!ids || count == 0) return 0;

  if (id_stride == 0) id_stride = 32; /* default compact stride */

  /* Step 1: 256-bit little-endian sum */
  unsigned char acc[32];
  memset(acc, 0, sizeof(acc));
  for (size_t i = 0; i < count; ++i) {
    const unsigned char *id = ids + i * id_stride;
    /* Add id to acc as LE 256-bit */
    unsigned int carry = 0;
    for (size_t b = 0; b < 32; ++b) {
      unsigned int a = acc[b];
      unsigned int v = id[b];
      unsigned int s = a + v + carry;
      acc[b] = (unsigned char)(s & 0xFFu);
      carry = s >> 8;
    }
    /* carry beyond 256 bits is dropped (mod 2^256) */
  }

  /* Step 2: append varint(count) */
  unsigned char trailer[10]; /* varint up to 64-bit */
  size_t tlen = neg_varint_encode((uint64_t)count, trailer, sizeof(trailer));
  if (tlen == 0) return -1;

  /* Step 3: SHA-256(acc || varint(count)) */
  unsigned char hash[32];
#if defined(NIP77_USE_OPENSSL_EVP)
#  if OPENSSL_VERSION_NUMBER >= 0x10100000L
  EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
  if (!mdctx) return -1;
  int ok = EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL) == 1 &&
           EVP_DigestUpdate(mdctx, acc, sizeof(acc)) == 1 &&
           EVP_DigestUpdate(mdctx, trailer, tlen) == 1 &&
           EVP_DigestFinal_ex(mdctx, hash, NULL) == 1;
  EVP_MD_CTX_free(mdctx);
  if (!ok) return -1;
#  else
  EVP_MD_CTX mdctx;
  EVP_MD_CTX_init(&mdctx);
  int ok = EVP_DigestInit_ex(&mdctx, EVP_sha256(), NULL) == 1 &&
           EVP_DigestUpdate(&mdctx, acc, sizeof(acc)) == 1 &&
           EVP_DigestUpdate(&mdctx, trailer, tlen) == 1 &&
           EVP_DigestFinal_ex(&mdctx, hash, NULL) == 1;
  EVP_MD_CTX_cleanup(&mdctx);
  if (!ok) return -1;
#  endif
#elif defined(NIP77_USE_OPENSSL_SHA)
  SHA256_CTX ctx;
  SHA256_Init(&ctx);
  SHA256_Update(&ctx, acc, sizeof(acc));
  SHA256_Update(&ctx, trailer, tlen);
  SHA256_Final(hash, &ctx);
#else
# error "OpenSSL headers not found; please install OpenSSL dev headers"
#endif
  memcpy(out16, hash, 16);
  return 0;
}
