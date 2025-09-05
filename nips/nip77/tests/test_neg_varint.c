#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../src/neg_varint.h"

int main(void) {
  unsigned char buf[10];
  size_t n = neg_varint_encode(0, buf, sizeof buf);
  assert(n >= 1);
  uint64_t v=1; size_t c=0; int r = neg_varint_decode(buf, n, &v, &c);
  assert(r==0 && v==0);
  printf("ok varint\n");
  return 0;
}
