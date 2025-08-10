#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static FILE *try_open(void) {
  const char *candidates[] = {
    "nips/nip49/SPEC_SOURCE",
    "../nips/nip49/SPEC_SOURCE",
    "../../nips/nip49/SPEC_SOURCE",
    "../../../nips/nip49/SPEC_SOURCE",
    "SPEC_SOURCE",
    NULL
  };
  for (int i=0; candidates[i]; ++i) {
    FILE *f = fopen(candidates[i], "r");
    if (f) return f;
  }
  return NULL;
}

int main(void) {
  const char *expected = "SPEC_MD=../../docs/nips/49.md";
  FILE *f = try_open();
  if (!f) { perror("open SPEC_SOURCE"); return 1; }
  char buf[256] = {0};
  size_t n = fread(buf, 1, sizeof(buf)-1, f);
  fclose(f);
  if (n == 0) { fprintf(stderr, "SPEC_SOURCE empty\n"); return 1; }
  if (strstr(buf, expected) == NULL) {
    fprintf(stderr, "SPEC_SOURCE missing '%s'\n", expected);
    return 2;
  }
  printf("ok\n");
  return 0;
}
