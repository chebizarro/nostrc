#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "json.h"
#include "nostr_jansson.h"

static void expect_eq(const char *got, const char *expect) {
  if (!got || !expect) {
    assert(got == expect);
    return;
  }
  if (strcmp(got, expect) != 0) {
    fprintf(stderr, "mismatch: got='%s' expect='%s'\n", got, expect);
    assert(0);
  }
}

int main(void) {
  /* Ensure JSON backend is installed to mirror other tests */
  nostr_set_json_interface(jansson_impl);
  nostr_json_init();
  const char *json = "{\n"
                     "  \"s\": \"abc\",\n"
                     "  \"n\": 123,\n"
                     "  \"r\": 3.14,\n"
                     "  \"b1\": true,\n"
                     "  \"b0\": false,\n"
                     "  \"z\": null,\n"
                     "  \"o\": {\"k\":1,\"t\":\"x\"},\n"
                     "  \"a\": [1,2,3]\n"
                     "}";

  char *out = NULL;

  /* Strings: expect quoted JSON text */
  int rc = nostr_json_get_raw(json, "s", &out);
  fprintf(stderr, "rc(s)=%d out=%s\n", rc, out ? out : "(null)");
  assert(rc == 0);
  expect_eq(out, "\"abc\"");
  free(out); out = NULL;

  /* Integers */
  rc = nostr_json_get_raw(json, "n", &out);
  fprintf(stderr, "rc(n)=%d out=%s\n", rc, out ? out : "(null)");
  assert(rc == 0);
  expect_eq(out, "123");
  free(out); out = NULL;

  /* Reals */
  rc = nostr_json_get_raw(json, "r", &out);
  fprintf(stderr, "rc(r)=%d out=%s\n", rc, out ? out : "(null)");
  assert(rc == 0);
  /* Jansson may print 3.14 without trailing zeros */
  expect_eq(out, "3.14");
  free(out); out = NULL;

  /* Booleans */
  rc = nostr_json_get_raw(json, "b1", &out);
  fprintf(stderr, "rc(b1)=%d out=%s\n", rc, out ? out : "(null)");
  assert(rc == 0);
  expect_eq(out, "true");
  free(out); out = NULL;
  rc = nostr_json_get_raw(json, "b0", &out);
  fprintf(stderr, "rc(b0)=%d out=%s\n", rc, out ? out : "(null)");
  assert(rc == 0);
  expect_eq(out, "false");
  free(out); out = NULL;

  /* Null */
  rc = nostr_json_get_raw(json, "z", &out);
  fprintf(stderr, "rc(z)=%d out=%s\n", rc, out ? out : "(null)");
  assert(rc == 0);
  expect_eq(out, "null");
  free(out); out = NULL;

  /* Object: compact form with no spaces */
  rc = nostr_json_get_raw(json, "o", &out);
  fprintf(stderr, "rc(o)=%d out=%s\n", rc, out ? out : "(null)");
  assert(rc == 0);
  expect_eq(out, "{\"k\":1,\"t\":\"x\"}");
  free(out); out = NULL;

  /* Array */
  rc = nostr_json_get_raw(json, "a", &out);
  fprintf(stderr, "rc(a)=%d out=%s\n", rc, out ? out : "(null)");
  assert(rc == 0);
  expect_eq(out, "[1,2,3]");
  free(out); out = NULL;

  /* Missing key */
  assert(nostr_json_get_raw(json, "missing", &out) != 0);
  assert(out == NULL);

  /* Null args */
  assert(nostr_json_get_raw(NULL, "s", &out) != 0);
  assert(nostr_json_get_raw(json, NULL, &out) != 0);
  assert(nostr_json_get_raw(json, "s", NULL) != 0);

  nostr_json_cleanup();
  printf("test_json_raw OK\n");
  return 0;
}
