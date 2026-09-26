/* test_mock_signer_contract.c - the nostr-homed signer mock speaks the
 * nip55l 0.2.0 SignEvent contract.
 *
 * The integration runners stand mock_signer in for the real signer, so the
 * mock is only useful if a consumer that verifies the real signer's reply
 * (nostrfs, the Blossom NIP-98 header) accepts the mock's reply too, and a
 * consumer still coded for the old bare-signature reply fails against it.
 * This runs the mock on a private bus and checks its SignEvent reply the
 * way those consumers do: strict signed-event parse, canonical id, Schnorr
 * signature, and the signer's own pubkey.
 *
 * Usage: test_mock_signer_contract <path-to-mock_signer>
 */
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr/nip19/nip19.h"

#define CHECK(cond) do { if (!(cond)) { \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static const char *BUS_NAME = "org.nostr.Signer";
static const char *OBJ_PATH = "/org/nostr/signer";

typedef struct { GMainLoop *loop; gboolean appeared; } WaitCtx;

static void on_appeared(GDBusConnection *c, const gchar *n, const gchar *o, gpointer ud){
  (void)c; (void)n; (void)o;
  WaitCtx *w = ud; w->appeared = TRUE; g_main_loop_quit(w->loop);
}
static gboolean on_timeout(gpointer ud){ WaitCtx *w = ud; g_main_loop_quit(w->loop); return G_SOURCE_REMOVE; }

static GVariant *call(GDBusConnection *bus, const char *method, GVariant *args, GError **err){
  return g_dbus_connection_call_sync(bus, BUS_NAME, OBJ_PATH, "org.nostr.Signer", method,
                                     args, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE,
                                     10000, NULL, err);
}

/* Returns the verified signed event; asserts on any contract breach. */
static NostrEvent *sign_and_verify(GDBusConnection *bus, const char *tmpl, const char *want_pk_hex){
  GError *err = NULL;
  GVariant *ret = call(bus, "SignEvent", g_variant_new("(sss)", tmpl, "", "contract-test"), &err);
  if (!ret) { fprintf(stderr, "SignEvent: %s\n", err ? err->message : "?"); exit(1); }
  const char *reply = NULL;
  g_variant_get(ret, "(&s)", &reply);
  /* Not the pre-0.2.0 shape: a bare 128-hex signature. */
  CHECK(reply && reply[0] == '{');
  NostrEvent *ev = nostr_event_new();
  CHECK(nostr_event_deserialize_signed(ev, reply, NULL) == NOSTR_EVENT_VALIDATION_OK);
  CHECK(nostr_event_validate(ev, NULL) == NOSTR_EVENT_VALIDATION_OK);
  CHECK(g_strcmp0(nostr_event_get_pubkey(ev), want_pk_hex) == 0);
  CHECK(nostr_event_get_created_at(ev) != 0);
  g_variant_unref(ret);
  return ev;
}

int main(int argc, char **argv){
  if (argc < 2) { fprintf(stderr, "usage: %s <mock_signer>\n", argv[0]); return 2; }

  GTestDBus *tbus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(tbus);

  GError *err = NULL;
  GSubprocess *mock = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &err, argv[1], NULL);
  CHECK(mock != NULL);

  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  CHECK(bus != NULL);

  WaitCtx w = { g_main_loop_new(NULL, FALSE), FALSE };
  guint watch = g_bus_watch_name_on_connection(bus, BUS_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                               on_appeared, NULL, &w, NULL);
  guint to = g_timeout_add_seconds(15, on_timeout, &w);
  g_main_loop_run(w.loop);
  if (w.appeared) g_source_remove(to);
  g_bus_unwatch_name(watch);
  CHECK(w.appeared);

  /* The signer's identity, as consumers learn it. */
  GVariant *ret = call(bus, "GetPublicKey", NULL, &err);
  CHECK(ret != NULL);
  const char *npub = NULL;
  g_variant_get(ret, "(&s)", &npub);
  uint8_t pk[32];
  CHECK(nostr_nip19_decode_npub(npub, pk) == 0);
  char pk_hex[65];
  for (int i = 0; i < 32; i++) snprintf(pk_hex + 2 * i, 3, "%02x", pk[i]);
  g_variant_unref(ret);

  /* A template with no pubkey and created_at 0 (pam-style). */
  NostrEvent *a = sign_and_verify(bus,
    "{\"kind\":22242,\"created_at\":0,\"tags\":[[\"challenge\",\"abc\"]],\"content\":\"hi \\\"there\\\"\"}",
    pk_hex);
  CHECK(nostr_event_get_kind(a) == 22242);
  CHECK(g_strcmp0(nostr_event_get_content(a), "hi \"there\"") == 0);
  nostr_event_free(a);

  /* A template carrying the author pubkey and a timestamp (nostrfs-style). */
  char *tmpl = g_strdup_printf(
    "{\"kind\":30081,\"created_at\":1700000000,\"pubkey\":\"%s\",\"tags\":[[\"d\",\"home\"]],\"content\":\"{}\"}",
    pk_hex);
  NostrEvent *b = sign_and_verify(bus, tmpl, pk_hex);
  CHECK(nostr_event_get_created_at(b) == 1700000000);
  CHECK(nostr_event_get_kind(b) == 30081);
  nostr_event_free(b);
  g_free(tmpl);

  /* Not an event: a typed error, never a "signature". */
  ret = call(bus, "SignEvent", g_variant_new("(sss)", "{not json", "", "contract-test"), &err);
  CHECK(ret == NULL && err != NULL);
  gchar *remote = g_dbus_error_get_remote_error(err);
  CHECK(g_strcmp0(remote, "org.nostr.Signer.Error.InvalidInput") == 0);
  g_free(remote);
  g_clear_error(&err);

  g_subprocess_force_exit(mock);
  (void)g_subprocess_wait(mock, NULL, NULL);
  g_object_unref(mock);
  g_object_unref(bus);
  g_main_loop_unref(w.loop);
  g_test_dbus_down(tbus);
  g_object_unref(tbus);
  printf("mock_signer: SignEvent contract ok (signed event JSON, verified)\n");
  return 0;
}
