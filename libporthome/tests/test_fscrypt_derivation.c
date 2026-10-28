#include <porthome/crypto.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Independently generated with Python hmac (RFC 5869 Extract + two Expand
 * blocks), not by the implementation under test. See crypto spec. */
static const char *vectors[] = {
  "de121e9b70369e899436180170aede93ab6de31ef1ddb2a2d47d5536a00ee0a7b"
  "a6777f187b55e610ac7058bd60761e025be4aed79fd712748c51d392cb7d486",
  "4a49340bea08f3ed6bd41cc9a453d3a562610dfcbb1ea4d57420fc6cfd258b2e0"
  "05340fa40da9f755d3cceaf5556b02cddc22c055ee2113c1f3e2825ae7ff2b3",
  "68f9320f38e812d32be7a17307eece1c52f96412a2e930b734a68fb202b74b503"
  "07a5e450b8a3d0971bb98340b6396418df0718a8121d509f905ebc51051562e"
};
int main(void) {
  uint8_t home[32], device[32], master[64], first[64], zero[64] = {0};
  const char *account = "01234567-89ab-cdef-0123-456789abcdef";
  for (unsigned i = 0; i < 32; i++) home[i] = (uint8_t)i;
  for (unsigned v = 0; v < 3; v++) {
    for (unsigned i = 0; i < 32; i++) device[i] = v == 1 ? 0xa5 : (uint8_t)(i + 32);
    assert(porthome_fscrypt_master_derive(home, account, device, v == 2 ? 2 : 1, master) == 0);
    char hex[129];
    porthome_hex64(master, hex); porthome_hex64(master + 32, hex + 64);
    assert(!strcmp(hex, vectors[v]));
    if (v == 0) memcpy(first, master, 64);
    else assert(memcmp(first, master, 64));
  }
  assert(porthome_fscrypt_master_derive(home, "11234567-89ab-cdef-0123-456789abcdef", device, 1, master) == 0);
  assert(memcmp(first, master, 64));
  const char *invalid[] = {"", "01234567-89AB-cdef-0123-456789abcdef", "0123456789abcdef0123456789abcdef", "01234567_89ab-cdef-0123-456789abcdef", "01234567-89ab-cdef-0123-456789abcdeg", "01234567-89ab-cdef-0123-456789abcdef0"};
  for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
    memset(master, 0xff, 64);
    assert(porthome_fscrypt_master_derive(home, invalid[i], device, 1, master) == PORTHOME_ERR_ARG);
    assert(!memcmp(master, zero, 64));
  }
  memset(master, 0xff, 64);
  assert(porthome_fscrypt_master_derive(home, account, device, 0, master) == PORTHOME_ERR_ARG);
  assert(!memcmp(master, zero, 64));
  assert(porthome_fscrypt_master_derive(NULL, account, device, 1, master) == PORTHOME_ERR_ARG);
  assert(porthome_fscrypt_master_derive(home, NULL, device, 1, master) == PORTHOME_ERR_ARG);
  assert(porthome_fscrypt_master_derive(home, account, NULL, 1, master) == PORTHOME_ERR_ARG);
  assert(porthome_fscrypt_master_derive(home, account, device, 1, NULL) == PORTHOME_ERR_ARG);
  puts("fscrypt derivation vectors: PASS");
  return 0;
}
