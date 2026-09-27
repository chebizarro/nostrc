#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr/nip47/nwc_info.h"

typedef struct {
  char **methods; size_t methods_n;
  char **encs; size_t encs_n;
  char **notifs; size_t notifs_n;
} Parsed;

static void free_str_array(char **arr, size_t n) {
  if (!arr) return;
  for (size_t i = 0; i < n; i++) free(arr[i]);
  free(arr);
}

static void parsed_free(Parsed *p) {
  free_str_array(p->methods, p->methods_n);
  free_str_array(p->encs, p->encs_n);
  free_str_array(p->notifs, p->notifs_n);
  memset(p, 0, sizeof(*p));
}

static int parse(const char *json, Parsed *p) {
  memset(p, 0, sizeof(*p));
  return nostr_nwc_info_parse(json, &p->methods, &p->methods_n, &p->encs, &p->encs_n,
                              &p->notifs, &p->notifs_n);
}

static void expect_list(char **got, size_t n, const char **want, size_t want_n) {
  assert(n == want_n);
  for (size_t i = 0; i < n; i++) assert(strcmp(got[i], want[i]) == 0);
}

#define N(a) (sizeof(a) / sizeof((a)[0]))

int main(void) {
  const char *methods[] = {"pay_invoice", "get_balance", "make_invoice"};
  const char *encs[] = {"nip44_v2", "nip04"};
  const char *notifs[] = {"payment_received", "payment_sent"};
  Parsed p;
  char *json = NULL;

  /* Spec shape (nostrc-krqc): plaintext capability content, one
   * space-separated encryption tag (nostrc-iq04), notification types. */
  int rc = nostr_nwc_info_build("cafebabecafebabecafebabecafebabecafebabecafebabecafebabecafebabe",
                                0, methods, 3, encs, 2, notifs, 2, &json);
  assert(rc == 0 && json);
  assert(strstr(json, "\"content\":\"pay_invoice get_balance make_invoice notifications\"") != NULL);
  assert(strstr(json, "[\"encryption\",\"nip44_v2 nip04\"]") != NULL);
  assert(strstr(json, "[\"notifications\",\"payment_received payment_sent\"]") != NULL);
  assert(strstr(json, "\"methods\"") == NULL && strstr(json, "\"true\"") == NULL);
  { const char *first = strstr(json, "\"encryption\"");
    assert(first && strstr(first + 1, "\"encryption\"") == NULL); }

  assert(parse(json, &p) == 0);
  { const char *want[] = {"pay_invoice", "get_balance", "make_invoice", "notifications"};
    expect_list(p.methods, p.methods_n, want, N(want)); }
  expect_list(p.encs, p.encs_n, encs, N(encs));
  expect_list(p.notifs, p.notifs_n, notifs, N(notifs));
  parsed_free(&p);
  free(json);

  /* No notification types: no tag, no implied capability. An explicit
   * "notifications" capability is not duplicated. */
  rc = nostr_nwc_info_build(NULL, 0, methods, 3, encs, 2, NULL, 0, &json);
  assert(rc == 0 && json);
  assert(strstr(json, "notifications") == NULL);
  free(json);
  { const char *with_cap[] = {"get_info", "notifications"};
    const char *one[] = {"payment_received"};
    rc = nostr_nwc_info_build(NULL, 0, with_cap, 2, NULL, 0, one, 1, &json);
    assert(rc == 0 && json);
    assert(strstr(json, "\"content\":\"get_info notifications\"") != NULL);
    assert(strstr(json, "encryption") == NULL);
    free(json); }

  /* Tokens that would not survive the space-separated encoding. */
  { const char *bad[] = {"pay invoice"};
    assert(nostr_nwc_info_build(NULL, 0, bad, 1, NULL, 0, NULL, 0, &json) != 0 && json == NULL);
    const char *empty[] = {""};
    assert(nostr_nwc_info_build(NULL, 0, empty, 1, NULL, 0, NULL, 0, &json) != 0);
    assert(nostr_nwc_info_build(NULL, 0, methods, 3, NULL, 0, empty, 1, &json) != 0);
    assert(nostr_nwc_info_build(NULL, 0, methods, 3, NULL, 0, NULL, 2, &json) != 0);
    assert(nostr_nwc_info_build(NULL, 0, methods, 0, NULL, 0, NULL, 0, &json) != 0); }

  /* Legacy spelling passed to the builder is normalised on the wire. */
  {
    const char *legacy[] = {"nip44-v2"};
    rc = nostr_nwc_info_build(NULL, 0, methods, 3, legacy, 1, NULL, 0, &json);
    assert(rc == 0 && json);
    assert(strstr(json, "[\"encryption\",\"nip44_v2\"]") != NULL);
    free(json);
  }

  /* The NIP-47 appendix example, verbatim. */
  {
    const char *spec =
      "{\"id\":\"df467db0a9f9ec77ffe6f561811714ccaa2e26051c20f58f33c3d66d6c2b4d1c\","
      "\"pubkey\":\"c04ccd5c82fc1ea3499b9c6a5c0a7ab627fbe00a0116110d4c750faeaecba1e2\","
      "\"created_at\":1713883677,\"kind\":13194,"
      "\"tags\":[[\"encryption\",\"nip44_v2 nip04\"],[\"notifications\",\"payment_received payment_sent\"]],"
      "\"content\":\"pay_invoice pay_keysend get_balance get_info make_invoice lookup_invoice "
      "list_transactions multi_pay_invoice multi_pay_keysend sign_message notifications\","
      "\"sig\":\"31f57b369459b5306a5353aa9e03be7fbde169bc881c3233625605dd12f53548179def16b9fe1137e6465d7e4d5bb27ce81fd6e75908c46b06269f4233c845d8\"}";
    assert(parse(spec, &p) == 0);
    assert(p.methods_n == 11);
    assert(strcmp(p.methods[0], "pay_invoice") == 0 && strcmp(p.methods[10], "notifications") == 0);
    expect_list(p.encs, p.encs_n, encs, N(encs));
    expect_list(p.notifs, p.notifs_n, notifs, N(notifs));
    parsed_free(&p);
  }

  /* Legacy nostrc shape: JSON content, one tag per scheme ("nip44-v2"),
   * boolean notifications (names no types). */
  {
    const char *old_shape =
      "{\"kind\":13194,\"content\":\"{\\\"methods\\\":[\\\"get_info\\\",\\\"get_balance\\\"]}\","
      "\"tags\":[[\"encryption\",\"nip44-v2\"],[\"encryption\",\"nip04\"],"
      "[\"notifications\",\"true\"]],\"created_at\":1,\"pubkey\":\"\",\"id\":\"\",\"sig\":\"\"}";
    assert(parse(old_shape, &p) == 0);
    { const char *want[] = {"get_info", "get_balance"};
      expect_list(p.methods, p.methods_n, want, N(want)); }
    { const char *want[] = {"nip44-v2", "nip04"};
      expect_list(p.encs, p.encs_n, want, N(want)); }
    assert(p.notifs_n == 0 && p.notifs == NULL);
    parsed_free(&p);
  }

  /* Whitespace runs and duplicates collapse; out-params are optional. */
  {
    const char *messy =
      "{\"kind\":13194,\"content\":\"  get_info\\tget_info  pay_invoice \","
      "\"tags\":[[\"encryption\",\"nip04  nip04\"]],\"created_at\":1,\"pubkey\":\"\",\"id\":\"\",\"sig\":\"\"}";
    assert(parse(messy, &p) == 0);
    { const char *want[] = {"get_info", "pay_invoice"};
      expect_list(p.methods, p.methods_n, want, N(want)); }
    assert(p.encs_n == 1 && p.notifs_n == 0);
    parsed_free(&p);
    assert(nostr_nwc_info_parse(messy, NULL, NULL, NULL, NULL, NULL, NULL) == 0);
  }

  /* Negative: no capabilities. */
  assert(parse("{\"kind\":13194,\"content\":\"\",\"tags\":[],\"created_at\":1,\"pubkey\":\"\",\"id\":\"\",\"sig\":\"\"}", &p) != 0);
  assert(parse("{\"kind\":13194,\"content\":\"   \",\"tags\":[],\"created_at\":1,\"pubkey\":\"\",\"id\":\"\",\"sig\":\"\"}", &p) != 0);
  assert(parse("{\"kind\":13194,\"content\":\"{}\",\"tags\":[],\"created_at\":1,\"pubkey\":\"\",\"id\":\"\",\"sig\":\"\"}", &p) != 0);
  assert(parse("{\"content\":\"{}\"}", &p) != 0);
  assert(parse("not json", &p) != 0);
  assert(nostr_nwc_info_parse(NULL, NULL, NULL, NULL, NULL, NULL, NULL) != 0);

  printf("test_nwc_info: OK\n");
  return 0;
}
