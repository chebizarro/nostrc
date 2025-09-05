#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../libnostr/include/secure_buf.h"

int main(void){
  const char *pattern = "TOPSECRET0123456789";
  size_t n = strlen(pattern);
  nostr_secure_buf sb = secure_alloc(n);
  assert(sb.ptr && sb.len == n);
  memcpy(sb.ptr, pattern, n);
  // Ensure content present
  assert(memcmp(sb.ptr, pattern, n) == 0);
  // Free should wipe
  secure_free(&sb);
  // Can't reliably read freed memory; just ensure fields are reset
  assert(sb.ptr == NULL && sb.len == 0);
  // constant-time compare
  assert(secure_memcmp_ct("abc","abc",3) == 0);
  assert(secure_memcmp_ct("abc","abd",3) != 0);
  printf("ok secure_buf\n");
  return 0;
}
