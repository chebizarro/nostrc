#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../src/neg_fingerprint.h"

int main(void) {
  unsigned char ids[32] = {0};
  unsigned char fp[16];
  int r = neg_fingerprint_compute(ids, 32, 1, fp);
  assert(r==0);
  printf("ok fingerprint\n");
  return 0;
}
