#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../src/neg_message.h"

int main(void) {
  neg_bound_t r = { .ts_delta = 0, .id_prefix_len = 0 };
  unsigned char buf[64];
  size_t n = neg_msg_encode_v1(&r, 1, NULL, 0, buf, sizeof buf);
  const unsigned char *pl=NULL; size_t pln=0; neg_bound_t rr; size_t rn=1; int rc = neg_msg_decode_v1(buf, n, &rr, &rn, &pl, &pln);
  assert(rc==0 && rn==1);
  printf("ok message\n");
  return 0;
}
