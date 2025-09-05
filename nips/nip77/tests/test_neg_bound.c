#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../src/neg_bound.h"

int main(void) {
  neg_bound_t b = { .ts_delta = 0, .id_prefix_len = 0 };
  unsigned char buf[64];
  size_t n = neg_bound_encode(&b, buf, sizeof buf);
  neg_bound_t out = {0}; size_t c=0; int r = neg_bound_decode(buf, n, &out, &c);
  assert(r==0 && out.ts_delta==0 && out.id_prefix_len==0);
  printf("ok bound\n");
  return 0;
}
