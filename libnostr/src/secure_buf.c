#define _GNU_SOURCE
#include "../include/secure_buf.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include <sys/mman.h>
#include <unistd.h>

/* Best-effort explicit_bzero fallback */
static void explicit_bzero_portable(void *p, size_t n) {
#if defined(__has_feature)
#  if __has_feature(memory_sanitizer)
  /* Avoid MSan warnings by touching memory in a volatile fashion */
#  endif
#endif
#if defined(__STDC_LIB_EXT1__)
  /* C11 Annex K */
  memset_s(p, n, 0, n);
#else
  volatile unsigned char *vp = (volatile unsigned char *)p;
  while (n--) *vp++ = 0;
#endif
}

void secure_wipe(void *p, size_t n) {
  if (!p || n == 0) return;
#if defined(HAVE_EXPLICIT_BZERO)
  explicit_bzero(p, n);
#else
  explicit_bzero_portable(p, n);
#endif
}

static int try_mlock(void *p, size_t n) {
  if (!p || n == 0) return 0;
  if (mlock(p, n) == 0) return 1;
  return 0; /* best-effort */
}

nostr_secure_buf secure_alloc(size_t len) {
  nostr_secure_buf sb = {0};
  if (len == 0) return sb;

  /* Align to page size for better mlock behavior */
  long pg = sysconf(_SC_PAGESIZE);
  size_t al = (size_t)(pg > 0 ? pg : 4096);
  void *ptr = NULL;
  int r = posix_memalign(&ptr, al, len);
  if (r != 0 || !ptr) {
    sb.ptr = NULL; sb.len = 0; sb.locked = false; return sb;
  }

  /* Zero-init */
  memset(ptr, 0, len);

  sb.ptr = ptr;
  sb.len = len;
  sb.locked = try_mlock(ptr, len) ? true : false;
  return sb;
}

void secure_free(nostr_secure_buf *sb) {
  if (!sb) return;
  if (sb->ptr && sb->len) secure_wipe(sb->ptr, sb->len);
  if (sb->ptr) {
    if (sb->locked) munlock(sb->ptr, sb->len);
    free(sb->ptr);
  }
  sb->ptr = NULL; sb->len = 0; sb->locked = false;
}

int secure_memcmp_ct(const void *a, const void *b, size_t n) {
  const uint8_t *pa = (const uint8_t*)a;
  const uint8_t *pb = (const uint8_t*)b;
  uint8_t diff = 0;
  for (size_t i = 0; i < n; ++i) diff |= (pa[i] ^ pb[i]);
  return diff; /* 0 if equal */
}
