/* test_nm_router — request parsing, origin policy, event validation and
 * error envelopes through the router, with no signer reachable
 * (nostrc-jjyp). The real-daemon path is covered by test_nm_e2e. */
#include "nm_router.h"

#include <string.h>

typedef struct {
  GPtrArray *replies; /* JsonNode* */
} Sink;

static void sink_reply(const gchar *json, gsize len, gpointer data) {
  Sink *s = data;
  g_autoptr(JsonParser) p = json_parser_new();
  g_assert_true(json_parser_load_from_data(p, json, (gssize)len, NULL));
  g_ptr_array_add(s->replies, json_node_ref(json_parser_get_root(p)));
}

static NmRouter *new_router(Sink *s) {
  s->replies = g_ptr_array_new_with_free_func((GDestroyNotify)json_node_unref);
  NmRouterConfig cfg = { .identity = "", .call_timeout_ms = 2000, .approval_timeout_ms = 2000 };
  return nm_router_new(NULL, &cfg, sink_reply, s);
}

/* Send one request and return its (single) reply object. */
static JsonObject *roundtrip(NmRouter *r, Sink *s, const gchar *json) {
  guint before = s->replies->len;
  nm_router_handle(r, json, strlen(json));
  while (s->replies->len == before) g_main_context_iteration(NULL, TRUE);
  g_assert_cmpuint(s->replies->len, ==, before + 1);
  return json_node_get_object(g_ptr_array_index(s->replies, before));
}

static void assert_error(JsonObject *o, const gchar *id, const gchar *code) {
  if (id) g_assert_cmpstr(json_object_get_string_member(o, "id"), ==, id);
  else g_assert_true(json_object_get_null_member(o, "id"));
  g_assert_true(json_object_has_member(o, "error"));
  g_assert_false(json_object_has_member(o, "result"));
  JsonObject *e = json_object_get_object_member(o, "error");
  g_assert_cmpstr(json_object_get_string_member(e, "code"), ==, code);
  g_assert_nonnull(json_object_get_string_member(e, "message"));
}

static void test_hello(void) {
  Sink s; NmRouter *r = new_router(&s);
  JsonObject *o = roundtrip(r, &s, "{\"id\":\"h\",\"method\":\"host.hello\"}");
  g_assert_cmpstr(json_object_get_string_member(o, "id"), ==, "h");
  JsonObject *res = json_object_get_object_member(o, "result");
  g_assert_cmpstr(json_object_get_string_member(res, "host"), ==, NM_HOST_NAME);
  g_assert_cmpint(json_object_get_int_member(res, "protocol"), ==, NM_PROTOCOL_VERSION);
  JsonObject *prov = json_object_get_object_member(res, "providers");
  g_assert_cmpuint(json_array_get_length(json_object_get_array_member(prov, "nip07")), ==, 7);
  g_assert_true(json_object_has_member(prov, "webln"));
  nm_router_free(r); g_ptr_array_unref(s.replies);
}

static void test_malformed(void) {
  Sink s; NmRouter *r = new_router(&s);
  assert_error(roundtrip(r, &s, "{not json"), NULL, "invalid_request");
  assert_error(roundtrip(r, &s, "[1,2]"), NULL, "invalid_request");
  assert_error(roundtrip(r, &s, "{\"method\":\"getPublicKey\"}"), NULL, "invalid_request");
  assert_error(roundtrip(r, &s, "{\"id\":\"\",\"method\":\"getPublicKey\"}"), NULL, "invalid_request");
  assert_error(roundtrip(r, &s, "{\"id\":{},\"method\":\"getPublicKey\"}"), NULL, "invalid_request");
  assert_error(roundtrip(r, &s,
    "{\"id\":\"0123456789012345678901234567890123456789012345678901234567890123456789\",\"method\":\"x\"}"),
    NULL, "invalid_request");
  assert_error(roundtrip(r, &s, "{\"id\":\"1\"}"), "1", "invalid_request");
  assert_error(roundtrip(r, &s, "{\"id\":\"2\",\"method\":7}"), "2", "invalid_request");
  assert_error(roundtrip(r, &s, "{\"id\":\"3\",\"method\":\"nostr.exfiltrate\",\"origin\":\"https://a.example\"}"),
               "3", "unknown_method");
  assert_error(roundtrip(r, &s,
    "{\"id\":\"4\",\"method\":\"getPublicKey\",\"origin\":\"https://a.example\",\"params\":[1]}"),
    "4", "invalid_request");
  /* integer ids are accepted and echoed as strings */
  assert_error(roundtrip(r, &s, "{\"id\":17,\"method\":\"nope\"}"), "17", "unknown_method");
  nm_router_reply_frame_error(r, NM_ERR_TOO_LARGE);
  assert_error(json_node_get_object(g_ptr_array_index(s.replies, s.replies->len - 1)), NULL, "too_large");
  g_assert_cmpuint(nm_router_in_flight(r), ==, 0);
  nm_router_free(r); g_ptr_array_unref(s.replies);
}

static void test_origin_policy(void) {
  Sink s; NmRouter *r = new_router(&s);
  const gchar *bad[] = {
    "{\"id\":\"o1\",\"method\":\"getPublicKey\"}",
    "{\"id\":\"o2\",\"method\":\"getPublicKey\",\"origin\":\"http://snort.social\"}",
    "{\"id\":\"o3\",\"method\":\"signEvent\",\"origin\":\"null\",\"params\":{\"event\":{}}}",
    "{\"id\":\"o4\",\"method\":\"nip44.decrypt\",\"origin\":\"file:///tmp/x.html\"}",
    "{\"id\":\"o5\",\"method\":\"getPublicKey\",\"origin\":42}",
    "{\"id\":\"o6\",\"method\":\"webln.enable\",\"origin\":\"http://evil.example\"}",
    "{\"id\":\"o7\",\"method\":\"getPublicKey\",\"origin\":\"https://A.example\"}",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
    g_autofree gchar *id = g_strdup_printf("o%" G_GSIZE_FORMAT, i + 1);
    assert_error(roundtrip(r, &s, bad[i]), id, "origin_denied");
  }
  nm_router_free(r); g_ptr_array_unref(s.replies);
}

/* Malformed events never reach the signer: invalid_request, not
 * signer_unavailable, even though no bus is reachable here. */
static void test_event_validation_precedes_signer(void) {
  Sink s; NmRouter *r = new_router(&s);
  const gchar *bad[] = {
    "{\"id\":\"e1\",\"method\":\"signEvent\",\"origin\":\"https://a.example\"}",
    "{\"id\":\"e2\",\"method\":\"signEvent\",\"origin\":\"https://a.example\",\"params\":{\"event\":{\"kind\":\"1\",\"tags\":[],\"content\":\"\"}}}",
    "{\"id\":\"e3\",\"method\":\"signEvent\",\"origin\":\"https://a.example\",\"params\":{\"event\":{\"kind\":1,\"tags\":[[1]],\"content\":\"\"}}}",
    "{\"id\":\"e4\",\"method\":\"signEvent\",\"origin\":\"https://a.example\",\"params\":{\"event\":{\"kind\":1,\"tags\":[],\"content\":\"a\\u0000b\"}}}",
    "{\"id\":\"e5\",\"method\":\"nip44.encrypt\",\"origin\":\"https://a.example\",\"params\":{\"pubkey\":\"xyz\",\"plaintext\":\"hi\"}}",
    ("{\"id\":\"e6\",\"method\":\"nip04.decrypt\",\"origin\":\"https://a.example\",\"params\":{\"pubkey\":\""
     "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9\"}}"),
  };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
    g_autofree gchar *id = g_strdup_printf("e%" G_GSIZE_FORMAT, i + 1);
    assert_error(roundtrip(r, &s, bad[i]), id, "invalid_request");
  }
  nm_router_free(r); g_ptr_array_unref(s.replies);
}

static void test_signer_unavailable_and_webln_stub(void) {
  Sink s; NmRouter *r = new_router(&s);
  assert_error(roundtrip(r, &s, "{\"id\":\"u1\",\"method\":\"getPublicKey\",\"origin\":\"https://a.example\"}"),
               "u1", "signer_unavailable");
  assert_error(roundtrip(r, &s,
    "{\"id\":\"u2\",\"method\":\"signEvent\",\"origin\":\"http://localhost:5173\","
    "\"params\":{\"event\":{\"kind\":1,\"tags\":[],\"content\":\"x\",\"created_at\":1}}}"),
    "u2", "signer_unavailable");
  JsonObject *o = roundtrip(r, &s, "{\"id\":\"w1\",\"method\":\"webln.sendPayment\",\"origin\":\"https://a.example\"}");
  assert_error(o, "w1", "unsupported");
  g_assert_nonnull(strstr(json_object_get_string_member(json_object_get_object_member(o, "error"), "message"),
                          "nostrc-yka8"));
  g_assert_cmpuint(nm_router_in_flight(r), ==, 0);
  nm_router_free(r); g_ptr_array_unref(s.replies);
}

int main(int argc, char **argv) {
  /* No signer: point the session bus at nothing so calls fail fast. */
  g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/nmh-test-bus", TRUE);
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nmh/router/hello", test_hello);
  g_test_add_func("/nmh/router/malformed", test_malformed);
  g_test_add_func("/nmh/router/origin-policy", test_origin_policy);
  g_test_add_func("/nmh/router/event-validation-first", test_event_validation_precedes_signer);
  g_test_add_func("/nmh/router/signer-unavailable-webln-stub", test_signer_unavailable_and_webln_stub);
  return g_test_run();
}
