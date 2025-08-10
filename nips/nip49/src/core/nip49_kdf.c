#include <sodium.h>
#include <stdint.h>
#include <string.h>

// Derive 32-byte key using scrypt: N = 1<<log_n, r=8, p=1
int nip49_kdf_scrypt(const char *password_nfkc,
                     const uint8_t salt[16],
                     uint8_t log_n,
                     uint8_t out_key32[32]) {
  if (!password_nfkc || !salt || !out_key32) return -1;
  const uint64_t N = (uint64_t)1u << log_n;
  if (crypto_pwhash_scryptsalsa208sha256_ll((const uint8_t*)password_nfkc,
                                            strlen(password_nfkc),
                                            salt, 16,
                                            (uint64_t)N,
                                            8, 1,
                                            out_key32, 32) != 0) {
    return -1;
  }
  return 0;
}
