#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

int nip44_base64_encode(const uint8_t *buf, size_t len, char **out_b64) {
  if (!buf || !out_b64) return -1;
  BIO *b64 = BIO_new(BIO_f_base64());
  BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
  BIO *mem = BIO_new(BIO_s_mem());
  BIO *chain = BIO_push(b64, mem);
  if (BIO_write(chain, buf, (int)len) != (int)len) { BIO_free_all(chain); return -1; }
  if (BIO_flush(chain) != 1) { BIO_free_all(chain); return -1; }
  BUF_MEM *bptr = NULL;
  BIO_get_mem_ptr(chain, &bptr);
  if (!bptr || !bptr->data) { BIO_free_all(chain); return -1; }
  char *out = (char*)malloc(bptr->length + 1);
  if (!out) { BIO_free_all(chain); return -1; }
  memcpy(out, bptr->data, bptr->length);
  out[bptr->length] = '\0';
  *out_b64 = out;
  BIO_free_all(chain);
  return 0;
}

int nip44_base64_decode(const char *b64, uint8_t **out_buf, size_t *out_len) {
  if (!b64 || !out_buf || !out_len) return -1;
  BIO *bmem = BIO_new_mem_buf((void*)b64, -1);
  BIO *b64f = BIO_new(BIO_f_base64());
  BIO_set_flags(b64f, BIO_FLAGS_BASE64_NO_NL);
  BIO *chain = BIO_push(b64f, bmem);
  size_t in_len = strlen(b64);
  size_t cap = (in_len * 3) / 4 + 3;
  uint8_t *out = (uint8_t*)malloc(cap);
  if (!out) { BIO_free_all(chain); return -1; }
  int n = BIO_read(chain, out, (int)cap);
  if (n <= 0) { free(out); BIO_free_all(chain); return -1; }
  *out_buf = out;
  *out_len = (size_t)n;
  BIO_free_all(chain);
  return 0;
}
