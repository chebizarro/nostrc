#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr/nip47/nwc_info.h"

static void free_str_array(char **arr, size_t n) {
  if (!arr) return;
  for (size_t i = 0; i < n; i++) free(arr[i]);
  free(arr);
}

int main(void) {
  const char *methods[] = {"pay_invoice", "get_balance", "make_invoice"};
  const char *encs[] = {"nip44_v2", "nip04"};
  char *json = NULL;
  int rc = nostr_nwc_info_build("cafebabecafebabecafebabecafebabecafebabecafebabecafebabecafebabe",
                                0,
                                methods, 3,
                                encs, 2,
                                1,
                                &json);
  assert(rc == 0 && json);
  /* nostrc-iq04: NIP-47 carries the schemes as ONE space-separated tag. */
  assert(strstr(json, "[\"encryption\",\"nip44_v2 nip04\"]") != NULL);
  assert(strstr(json, "nip44-v2") == NULL);
  { const char *first = strstr(json, "\"encryption\"");
    assert(first && strstr(first + 1, "\"encryption\"") == NULL); }

  char **out_methods = NULL; size_t out_methods_n = 0;
  char **out_encs = NULL; size_t out_encs_n = 0;
  int notifications = 0;
  rc = nostr_nwc_info_parse(json, &out_methods, &out_methods_n, &out_encs, &out_encs_n, &notifications);
  assert(rc == 0);
  assert(out_methods_n == 3);
  assert(strcmp(out_methods[0], "pay_invoice") == 0);
  assert(strcmp(out_methods[1], "get_balance") == 0);
  assert(strcmp(out_methods[2], "make_invoice") == 0);
  assert(out_encs_n == 2);
  assert(strcmp(out_encs[0], "nip44_v2") == 0);
  assert(strcmp(out_encs[1], "nip04") == 0);
  assert((strcmp(out_encs[0], "nip04") == 0) || (strcmp(out_encs[1], "nip04") == 0));
  assert(notifications == 1);

  free_str_array(out_methods, out_methods_n);
  free_str_array(out_encs, out_encs_n);
  free(json);

  /* Legacy spelling passed to the builder is normalised on the wire. */
  {
    const char *legacy[] = {"nip44-v2"};
    char *lj = NULL;
    rc = nostr_nwc_info_build(NULL, 0, methods, 3, legacy, 1, 0, &lj);
    assert(rc == 0 && lj);
    assert(strstr(lj, "[\"encryption\",\"nip44_v2\"]") != NULL);
    free(lj);
  }

  /* Legacy nostrc shape (one tag per scheme, "nip44-v2") still parses. */
  {
    const char *old_shape =
      "{\"kind\":13194,\"content\":\"{\\\"methods\\\":[\\\"get_info\\\"]}\","
      "\"tags\":[[\"encryption\",\"nip44-v2\"],[\"encryption\",\"nip04\"],"
      "[\"notifications\",\"false\"]],\"created_at\":1,\"pubkey\":\"\",\"id\":\"\",\"sig\":\"\"}";
    rc = nostr_nwc_info_parse(old_shape, &out_methods, &out_methods_n, &out_encs, &out_encs_n, &notifications);
    assert(rc == 0);
    assert(out_encs_n == 2);
    assert(strcmp(out_encs[0], "nip44-v2") == 0 && strcmp(out_encs[1], "nip04") == 0);
    assert(notifications == 0);
    free_str_array(out_methods, out_methods_n);
    free_str_array(out_encs, out_encs_n);
  }

  /* Negative: require methods in content */
  rc = nostr_nwc_info_parse("{\"content\":\"{}\"}", &out_methods, &out_methods_n, &out_encs, &out_encs_n, &notifications);
  assert(rc != 0);

  printf("test_nwc_info: OK\n");
  return 0;
}
