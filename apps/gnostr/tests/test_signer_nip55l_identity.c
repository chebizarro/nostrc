/*
 * test_signer_nip55l_identity.c — GNostr Signer (NIP-55L) session restore
 * and identity selection (nostrc-vuwu).
 *
 * A fake org.nostr.Signer runs on its own thread on a private test bus
 * (tests/common/nostrc-test-bus.h). It holds two identities (A, B), has a
 * switchable DEFAULT identity, records every current_user selector and signs
 * with the key the selector names (docs/dbus-interface.md). Checks that GnostrSignerService:
 *   - restores a NIP-55L session only when the signer runs and its
 *     GetPublicKey() is the saved account (never auto-starting it);
 *   - passes the selected npub as current_user on SignEvent and NIP-44
 *     calls, so a changed daemon default never changes who signs;
 *   - rejects a signed event whose pubkey is not the selected account;
 *   - refuses to call the signer at all when no account is selected;
 *   - nostrc-jppi: maps nip55l 0.4.0 approval errors (no approval window,
 *     user denial, timeout, saved deny rule) to GNOSTR_SIGNER_ERROR with
 *     user-facing text, tracks the NO_APPROVER / REFUSED state, and waits
 *     longer than the daemon's 300 s approval expiry.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ipc/gnostr-signer-service.h"
#include "ipc/signer_ipc.h"
#include "nostrc-test-bus.h"

#include <gio/gio.h>
#include <nostr-gobject-1.0/nostr_nip19.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <string.h>

#define SK_A "7f7ff03d123792d6ac594bfa67bf6d0c0ab55b6b1fdb6249303fe861f1ccba9a"
#define SK_B "3a3e5e1f0b87d1c0e6a4b3f2d8a9c7e6f5d4c3b2a1908f7e6d5c4b3a29181716"

static char *pk_a, *pk_b, *npub_a, *npub_b;

/* ---- fake org.nostr.Signer (own thread + main context) --------------- */

typedef struct {
  GThread *thread;
  GMainContext *ctx;
  GMainLoop *loop;
  GDBusNodeInfo *node;
  GMutex lock;
  GCond cond;
  gboolean ready;
  /* guarded by lock */
  const char *default_sk;       /* the daemon's DEFAULT identity */
  gboolean ignore_selector;     /* misbehave: always sign as default */
  GPtrArray *selectors;         /* "<Method>:<current_user>" */
  /* nostrc-jppi: when set, every gated method fails with
   * org.nostr.Signer.Error.ApprovalDenied and this reason, as nip55l 0.4.0
   * does (e.g. no approval window, a saved deny rule). */
  const char *gate_reason;
} FakeSigner;

static FakeSigner fake;

static const char *
sk_for_selector(const char *sel)
{
  if (!sel || !*sel || fake.ignore_selector) return fake.default_sk;
  if (g_strcmp0(sel, npub_a) == 0) return SK_A;
  if (g_strcmp0(sel, npub_b) == 0) return SK_B;
  return NULL;
}

static void
fake_method(GDBusConnection *c, const char *sender, const char *path, const char *iface,
            const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer ud)
{
  (void)c; (void)sender; (void)path; (void)iface; (void)ud;
  g_mutex_lock(&fake.lock);
  if (fake.gate_reason) {
    g_ptr_array_add(fake.selectors, g_strdup_printf("%s:gated", method));
    g_dbus_method_invocation_return_dbus_error(inv, "org.nostr.Signer.Error.ApprovalDenied",
                                               fake.gate_reason);
  } else if (strcmp(method, "GetPublicKey") == 0) {
    g_autofree char *pk = nostr_key_get_public(fake.default_sk);
    g_autoptr(GNostrNip19) n = gnostr_nip19_encode_npub(pk, NULL);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", gnostr_nip19_get_bech32(n)));
  } else if (strcmp(method, "SignEvent") == 0) {
    const char *json, *sel, *app;
    g_variant_get(params, "(&s&s&s)", &json, &sel, &app);
    g_ptr_array_add(fake.selectors, g_strdup_printf("SignEvent:%s", sel));
    const char *sk = sk_for_selector(sel);
    NostrEvent *ev = nostr_event_new();
    if (!sk || nostr_event_deserialize_compact(ev, json, NULL) != 1 || nostr_event_sign(ev, sk) != 0) {
      g_dbus_method_invocation_return_dbus_error(inv, "org.nostr.Signer.Error.NoKeyConfigured",
                                                 "no key for selector");
    } else {
      g_autofree char *out = nostr_event_serialize_compact(ev);
      g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", out));
    }
    nostr_event_free(ev);
  } else if (g_str_has_prefix(method, "NIP44")) {
    const char *data, *peer, *sel;
    g_variant_get(params, "(&s&s&s)", &data, &peer, &sel);
    g_ptr_array_add(fake.selectors, g_strdup_printf("%s:%s", method, sel));
    g_dbus_method_invocation_return_value(inv, g_variant_new("(s)",
        g_str_has_suffix(method, "B64") ? "aGVsbG8=" : "result"));
  } else {
    g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "%s", method);
  }
  g_mutex_unlock(&fake.lock);
}

static const GDBusInterfaceVTable fake_vtable = { fake_method, NULL, NULL, { 0 } };

static void
on_fake_name(GDBusConnection *c, const char *name, gpointer ud)
{
  (void)c; (void)name; (void)ud;
  g_mutex_lock(&fake.lock);
  fake.ready = TRUE;
  g_cond_signal(&fake.cond);
  g_mutex_unlock(&fake.lock);
}

static gpointer
fake_thread(gpointer bus)
{
  g_main_context_push_thread_default(fake.ctx);
  g_autoptr(GError) error = NULL;
  /* Owned by the bus, which releases it only after the daemon is gone. */
  GDBusConnection *conn = nostrc_test_bus_connect(bus);
  guint reg = g_dbus_connection_register_object(conn, "/org/nostr/signer",
      g_dbus_node_info_lookup_interface(fake.node, "org.nostr.Signer"), &fake_vtable,
      NULL, NULL, &error);
  g_assert_no_error(error);
  guint own = g_bus_own_name_on_connection(conn, "org.nostr.Signer", G_BUS_NAME_OWNER_FLAGS_NONE,
                                           on_fake_name, NULL, NULL, NULL);
  g_main_loop_run(fake.loop);
  g_bus_unown_name(own);
  g_dbus_connection_unregister_object(conn, reg);
  g_dbus_connection_flush_sync(conn, NULL, NULL);
  g_main_context_pop_thread_default(fake.ctx);
  return NULL;
}

static void
fake_start(NostrcTestBus *bus)
{
  g_autofree char *xml = NULL;
  g_assert_true(g_file_get_contents(SIGNER_DBUS_XML, &xml, NULL, NULL));
  fake.node = g_dbus_node_info_new_for_xml(xml, NULL);
  g_assert_nonnull(fake.node);
  fake.ctx = g_main_context_new();
  fake.loop = g_main_loop_new(fake.ctx, FALSE);
  fake.selectors = g_ptr_array_new_with_free_func(g_free);
  fake.thread = g_thread_new("fake-signer", fake_thread, bus);
  g_mutex_lock(&fake.lock);
  while (!fake.ready)
    g_cond_wait(&fake.cond, &fake.lock);
  g_mutex_unlock(&fake.lock);
}

static void
fake_stop(void)
{
  g_main_loop_quit(fake.loop);
  g_thread_join(fake.thread);
  g_main_loop_unref(fake.loop);
  g_main_context_unref(fake.ctx);
  g_dbus_node_info_unref(fake.node);
  g_ptr_array_unref(fake.selectors);
}

static guint
selectors_len(void)
{
  g_mutex_lock(&fake.lock);
  guint n = fake.selectors->len;
  g_mutex_unlock(&fake.lock);
  return n;
}

static char *
selector_at(guint i)
{
  g_mutex_lock(&fake.lock);
  char *s = g_strdup(g_ptr_array_index(fake.selectors, i));
  g_mutex_unlock(&fake.lock);
  return s;
}

/* ---- helpers ----------------------------------------------------------- */

typedef struct {
  gboolean done;
  gboolean ok;
  GError *error;
  char *result;
} Wait;

static void
on_restore(GObject *src, GAsyncResult *res, gpointer ud)
{
  Wait *w = ud;
  w->ok = gnostr_signer_service_restore_nip55l_finish(GNOSTR_SIGNER_SERVICE(src), res, &w->error);
  w->done = TRUE;
}

static void
on_signed(GnostrSignerService *s, const char *json, GError *error, gpointer ud)
{
  (void)s;
  Wait *w = ud;
  w->ok = json != NULL;
  w->result = g_strdup(json);
  w->error = error ? g_error_copy(error) : NULL;
  w->done = TRUE;
}

static void
on_bytes(GnostrSignerService *s, GBytes *bytes, GError *error, gpointer ud)
{
  (void)s;
  Wait *w = ud;
  w->ok = bytes != NULL;
  w->error = error ? g_error_copy(error) : NULL;
  w->done = TRUE;
}

static void
count_signal(guint *counter)
{
  (*counter)++;
}

static void
wait_for(Wait *w)
{
  while (!w->done)
    g_main_context_iteration(NULL, TRUE);
}

static void
wait_clear(Wait *w)
{
  g_clear_error(&w->error);
  g_clear_pointer(&w->result, g_free);
  memset(w, 0, sizeof *w);
}

static char *
signed_pubkey(const char *json)
{
  NostrEvent *ev = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(ev, json, NULL), ==, 1);
  char *pk = g_strdup(nostr_event_get_pubkey(ev));
  nostr_event_free(ev);
  return pk;
}

/* ---- the test --------------------------------------------------------- */

static void
test_nip55l_identity(void)
{
  NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_up(bus);
  Wait w = { 0 };
  const char *tmpl = "{\"kind\":1,\"created_at\":1700000000,\"tags\":[],\"content\":\"hi\"}";

  /* 1. Signer not running: stay signed out, never auto-start it. */
  GnostrSignerService *svc = gnostr_signer_service_new();
  gnostr_signer_service_restore_nip55l_async(svc, npub_a, NULL, on_restore, &w);
  wait_for(&w);
  g_assert_false(w.ok);
  g_assert_error(w.error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_assert_cmpint(gnostr_signer_service_get_method(svc), ==, GNOSTR_SIGNER_METHOD_NONE);
  wait_clear(&w);

  fake.default_sk = SK_B;
  fake_start(bus);
  gnostr_signer_proxy_reset();

  /* 2. The signer's account differs from the saved one: do not restore. */
  gnostr_signer_service_restore_nip55l_async(svc, npub_a, NULL, on_restore, &w);
  wait_for(&w);
  g_assert_error(w.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_cmpint(gnostr_signer_service_get_method(svc), ==, GNOSTR_SIGNER_METHOD_NONE);
  g_assert_false(gnostr_signer_service_is_ready(svc));
  wait_clear(&w);

  /* 3a. A sign-out while the restore is in flight wins: no resurrection. */
  g_mutex_lock(&fake.lock);
  fake.default_sk = SK_A;
  g_mutex_unlock(&fake.lock);
  gnostr_signer_service_restore_nip55l_async(svc, npub_a, NULL, on_restore, &w);
  gnostr_signer_service_logout(svc);
  wait_for(&w);
  g_assert_error(w.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpint(gnostr_signer_service_get_method(svc), ==, GNOSTR_SIGNER_METHOD_NONE);
  wait_clear(&w);

  /* 3. Same account: restored as NIP-55L for that account. */
  gnostr_signer_service_restore_nip55l_async(svc, npub_a, NULL, on_restore, &w);
  wait_for(&w);
  g_assert_no_error(w.error);
  g_assert_true(w.ok);
  g_assert_cmpint(gnostr_signer_service_get_method(svc), ==, GNOSTR_SIGNER_METHOD_NIP55L);
  g_assert_true(gnostr_signer_service_is_ready(svc));
  g_assert_cmpstr(gnostr_signer_service_get_pubkey(svc), ==, pk_a);
  wait_clear(&w);

  /* 4. The daemon's default switches to B: GNostr still signs as A. */
  g_mutex_lock(&fake.lock);
  fake.default_sk = SK_B;
  g_mutex_unlock(&fake.lock);
  guint before = selectors_len();
  gnostr_signer_service_sign_event_async(svc, tmpl, NULL, on_signed, &w);
  wait_for(&w);
  g_assert_no_error(w.error);
  g_assert_true(w.ok);
  g_autofree char *pk = signed_pubkey(w.result);
  g_assert_cmpstr(pk, ==, pk_a);
  g_autofree char *sel = selector_at(before);
  g_autofree char *want = g_strdup_printf("SignEvent:%s", npub_a);
  g_assert_cmpstr(sel, ==, want);
  wait_clear(&w);

  /* 5. A daemon that signs as its default anyway is caught. */
  g_mutex_lock(&fake.lock);
  fake.ignore_selector = TRUE;
  g_mutex_unlock(&fake.lock);
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*NIP-55L signed as*");
  gnostr_signer_service_sign_event_async(svc, tmpl, NULL, on_signed, &w);
  wait_for(&w);
  g_test_assert_expected_messages();
  g_assert_false(w.ok);
  g_assert_error(w.error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  wait_clear(&w);
  g_mutex_lock(&fake.lock);
  fake.ignore_selector = FALSE;
  g_mutex_unlock(&fake.lock);

  /* 6. NIP-44 calls carry the selected npub too. */
  before = selectors_len();
  gnostr_signer_service_nip44_encrypt_async(svc, pk_b, "secret", NULL, on_signed, &w);
  wait_for(&w);
  g_assert_true(w.ok);
  wait_clear(&w);
  gnostr_signer_service_nip44_decrypt_async(svc, pk_b, "cipher", NULL, on_signed, &w);
  wait_for(&w);
  g_assert_true(w.ok);
  wait_clear(&w);
  g_autoptr(GBytes) plain = g_bytes_new_static("abc", 3);
  gnostr_signer_service_nip44_encrypt_bytes_async(svc, pk_b, plain, NULL, on_signed, &w);
  wait_for(&w);
  g_assert_true(w.ok);
  wait_clear(&w);
  gnostr_signer_service_nip44_decrypt_bytes_async(svc, pk_b, "cipher", NULL, on_bytes, &w);
  wait_for(&w);
  g_assert_true(w.ok);
  wait_clear(&w);
  g_assert_cmpuint(selectors_len(), ==, before + 4);
  for (guint i = before; i < before + 4; i++) {
    g_autofree char *s = selector_at(i);
    g_assert_true(g_str_has_suffix(s, npub_a));
  }
  g_object_unref(svc);

  /* 7. NIP-55L without a selected account: refuse, never ask the daemon
   * for its default identity. */
  svc = gnostr_signer_service_new();
  gnostr_signer_service_set_nip46_session(svc, NULL);  /* the sign-in fallback */
  g_assert_cmpint(gnostr_signer_service_get_method(svc), ==, GNOSTR_SIGNER_METHOD_NIP55L);
  before = selectors_len();
  gnostr_signer_service_sign_event_async(svc, tmpl, NULL, on_signed, &w);
  wait_for(&w);
  g_assert_error(w.error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
  wait_clear(&w);
  gnostr_signer_service_nip44_encrypt_async(svc, pk_b, "secret", NULL, on_signed, &w);
  wait_for(&w);
  g_assert_error(w.error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
  wait_clear(&w);
  g_assert_cmpuint(selectors_len(), ==, before);
  g_object_unref(svc);

  /* 8. nostrc-jppi: nip55l 0.4.0 gating. Calls wait out an approval prompt
   * (the proxy's timeout exceeds the daemon's 300 s expiry) ... */
  NostrSignerProxy *proxy = gnostr_signer_proxy_get(NULL);
  g_assert_nonnull(proxy);
  g_assert_cmpint(g_dbus_proxy_get_default_timeout(G_DBUS_PROXY(proxy)), ==,
                  GNOSTR_SIGNER_CALL_TIMEOUT_MS);
  g_assert_cmpint(GNOSTR_SIGNER_CALL_TIMEOUT_MS, >, 300 * 1000);

  /* ... and a fail-fast "no approval window" becomes a clear error plus the
   * service's NO_APPROVER state, announced once. */
  svc = gnostr_signer_service_new();
  guint changes = 0;
  g_signal_connect_swapped(svc, "approval-changed", G_CALLBACK(count_signal), &changes);
  g_mutex_lock(&fake.lock);
  fake.default_sk = SK_A;
  fake.gate_reason = "approval required but no approval agent is running (start GNostr Signer)";
  g_mutex_unlock(&fake.lock);
  /* Restore: the key store is not readable here (no app bridge), so the
   * service asks GetPublicKey, which is gated too. */
  gnostr_signer_service_restore_nip55l_async(svc, npub_a, NULL, on_restore, &w);
  wait_for(&w);
  g_assert_error(w.error, GNOSTR_SIGNER_ERROR, GNOSTR_SIGNER_ERROR_NO_APPROVER);
  g_assert_null(strstr(w.error->message, "GDBus.Error"));
  g_assert_cmpint(gnostr_signer_service_get_method(svc), ==, GNOSTR_SIGNER_METHOD_NONE);
  g_assert_cmpint(gnostr_signer_service_get_approval(svc), ==,
                  GNOSTR_SIGNER_APPROVAL_NO_APPROVER);
  g_assert_cmpuint(changes, ==, 1);
  wait_clear(&w);

  /* Signed in: signing and decrypting fail the same way, no new signal. */
  g_mutex_lock(&fake.lock);
  fake.gate_reason = NULL;
  g_mutex_unlock(&fake.lock);
  gnostr_signer_service_restore_nip55l_async(svc, npub_a, NULL, on_restore, &w);
  wait_for(&w);
  g_assert_true(w.ok);
  wait_clear(&w);
  g_mutex_lock(&fake.lock);
  fake.gate_reason = "approval required but no approval agent is running (start GNostr Signer)";
  g_mutex_unlock(&fake.lock);
  gnostr_signer_service_sign_event_async(svc, tmpl, NULL, on_signed, &w);
  wait_for(&w);
  g_assert_error(w.error, GNOSTR_SIGNER_ERROR, GNOSTR_SIGNER_ERROR_NO_APPROVER);
  wait_clear(&w);
  gnostr_signer_service_nip44_decrypt_async(svc, pk_b, "cipher", NULL, on_signed, &w);
  wait_for(&w);
  g_assert_error(w.error, GNOSTR_SIGNER_ERROR, GNOSTR_SIGNER_ERROR_NO_APPROVER);
  wait_clear(&w);
  g_assert_cmpuint(changes, ==, 1);

  /* The window opens (the main window resets): OK again. */
  gnostr_signer_service_reset_approval(svc);
  g_assert_cmpint(gnostr_signer_service_get_approval(svc), ==, GNOSTR_SIGNER_APPROVAL_OK);
  g_assert_cmpuint(changes, ==, 2);

  /* A saved deny rule: REFUSED. A one-off denial or timeout: only the
   * error, no lasting state. */
  g_mutex_lock(&fake.lock);
  fake.gate_reason = "user denied";
  g_mutex_unlock(&fake.lock);
  gnostr_signer_service_nip44_encrypt_async(svc, pk_b, "secret", NULL, on_signed, &w);
  wait_for(&w);
  g_assert_error(w.error, GNOSTR_SIGNER_ERROR, GNOSTR_SIGNER_ERROR_DENIED);
  wait_clear(&w);
  g_mutex_lock(&fake.lock);
  fake.gate_reason = "approval timed out";
  g_mutex_unlock(&fake.lock);
  gnostr_signer_service_nip44_decrypt_bytes_async(svc, pk_b, "cipher", NULL, on_bytes, &w);
  wait_for(&w);
  g_assert_error(w.error, GNOSTR_SIGNER_ERROR, GNOSTR_SIGNER_ERROR_TIMED_OUT);
  wait_clear(&w);
  g_assert_cmpint(gnostr_signer_service_get_approval(svc), ==, GNOSTR_SIGNER_APPROVAL_OK);
  g_mutex_lock(&fake.lock);
  fake.gate_reason = "denied by policy";
  g_mutex_unlock(&fake.lock);
  gnostr_signer_service_nip44_encrypt_bytes_async(svc, pk_b, plain, NULL, on_signed, &w);
  wait_for(&w);
  g_assert_error(w.error, GNOSTR_SIGNER_ERROR, GNOSTR_SIGNER_ERROR_DENIED_BY_RULE);
  wait_clear(&w);
  g_assert_cmpint(gnostr_signer_service_get_approval(svc), ==, GNOSTR_SIGNER_APPROVAL_REFUSED);
  g_assert_cmpuint(changes, ==, 3);

  /* Signing out forgets it. */
  gnostr_signer_service_logout(svc);
  g_assert_cmpint(gnostr_signer_service_get_approval(svc), ==, GNOSTR_SIGNER_APPROVAL_OK);
  g_mutex_lock(&fake.lock);
  fake.gate_reason = NULL;
  g_mutex_unlock(&fake.lock);
  g_object_unref(svc);

  gnostr_signer_proxy_shutdown();
  fake_stop();
  nostrc_test_bus_down(bus);
}

/* nostrc-jppi: nip55l 0.4.0 D-Bus errors -> user-facing GNOSTR_SIGNER_ERROR. */
static void
test_error_map(void)
{
  static const struct { const char *name, *msg; int code; } cases[] = {
    { "org.nostr.Signer.Error.ApprovalDenied",
      "approval required but no approval agent is running (start GNostr Signer)",
      GNOSTR_SIGNER_ERROR_NO_APPROVER },
    { "org.nostr.Signer.Error.ApprovalDenied", "approval timed out", GNOSTR_SIGNER_ERROR_TIMED_OUT },
    { "org.nostr.Signer.Error.ApprovalDenied", "denied by policy", GNOSTR_SIGNER_ERROR_DENIED_BY_RULE },
    { "org.nostr.Signer.Error.ApprovalDenied", "user denied", GNOSTR_SIGNER_ERROR_DENIED },
    { "org.nostr.Signer.Error.ApprovalDenied", "caller disconnected", GNOSTR_SIGNER_ERROR_DENIED },
    { "org.nostr.Signer.Error.RateLimited", "rate limited", GNOSTR_SIGNER_ERROR_RATE_LIMITED },
    { "org.nostr.Signer.Error.NoKeyConfigured", "no key", GNOSTR_SIGNER_ERROR_NO_KEY },
    { "org.nostr.Signer.Error.InvalidInput", "bad pubkey", GNOSTR_SIGNER_ERROR_FAILED },
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_autoptr(GError) raw = g_dbus_error_new_for_dbus_error(cases[i].name, cases[i].msg);
    g_autoptr(GError) e = gnostr_signer_error_from_dbus(raw);
    g_assert_error(e, GNOSTR_SIGNER_ERROR, cases[i].code);
    g_assert_null(strstr(e->message, "GDBus.Error"));
    g_assert_null(strstr(e->message, "org.nostr.Signer.Error"));
  }
  /* Unknown reasons keep the daemon's text. */
  g_autoptr(GError) raw = g_dbus_error_new_for_dbus_error("org.nostr.Signer.Error.InvalidInput",
                                                          "bad pubkey");
  g_autoptr(GError) e = gnostr_signer_error_from_dbus(raw);
  g_assert_nonnull(strstr(e->message, "bad pubkey"));

  g_autoptr(GError) unknown = g_error_new_literal(G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN, "x");
  g_autoptr(GError) e2 = gnostr_signer_error_from_dbus(unknown);
  g_assert_error(e2, GNOSTR_SIGNER_ERROR, GNOSTR_SIGNER_ERROR_NOT_RUNNING);
  g_autoptr(GError) slow = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "x");
  g_autoptr(GError) e3 = gnostr_signer_error_from_dbus(slow);
  g_assert_error(e3, GNOSTR_SIGNER_ERROR, GNOSTR_SIGNER_ERROR_TIMED_OUT);
  g_autoptr(GError) cancelled = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "x");
  g_autoptr(GError) e4 = gnostr_signer_error_from_dbus(cancelled);
  g_assert_error(e4, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_null(gnostr_signer_error_from_dbus(NULL));
}

/* The mapping keys on the daemon's reason text: keep it honest against the
 * daemon source it was written for (nostrc-jppi). */
static void
test_error_map_contract(void)
{
  g_autofree char *src = NULL;
  if (!g_file_get_contents(NIP55L_SIGNER_SERVICE_SRC, &src, NULL, NULL)) {
    g_test_skip("nip55l daemon source not found");
    return;
  }
  static const struct { const char *phrase; int code; } reasons[] = {
    { "approval required but no approval agent is running (start GNostr Signer)",
      GNOSTR_SIGNER_ERROR_NO_APPROVER },
    { "approval timed out", GNOSTR_SIGNER_ERROR_TIMED_OUT },
    { "denied by policy", GNOSTR_SIGNER_ERROR_DENIED_BY_RULE },
    { "user denied", GNOSTR_SIGNER_ERROR_DENIED },
  };
  for (guint i = 0; i < G_N_ELEMENTS(reasons); i++) {
    g_autofree char *quoted = g_strdup_printf("\"%s\"", reasons[i].phrase);
    if (!strstr(src, quoted))
      g_error("nip55l no longer says %s; update gnostr_signer_error_from_dbus()", quoted);
    g_autoptr(GError) raw = g_dbus_error_new_for_dbus_error(
        "org.nostr.Signer.Error.ApprovalDenied", reasons[i].phrase);
    g_autoptr(GError) e = gnostr_signer_error_from_dbus(raw);
    g_assert_error(e, GNOSTR_SIGNER_ERROR, reasons[i].code);
  }
  g_assert_nonnull(strstr(src, "ORG_NOSTR_SIGNER_ERR_APPROVAL"));
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  pk_a = nostr_key_get_public(SK_A);
  pk_b = nostr_key_get_public(SK_B);
  g_autoptr(GNostrNip19) na = gnostr_nip19_encode_npub(pk_a, NULL);
  g_autoptr(GNostrNip19) nb = gnostr_nip19_encode_npub(pk_b, NULL);
  npub_a = g_strdup(gnostr_nip19_get_bech32(na));
  npub_b = g_strdup(gnostr_nip19_get_bech32(nb));
  g_test_add_func("/signer/nip55l/identity", test_nip55l_identity);
  g_test_add_func("/signer/nip55l/error-map", test_error_map);
  g_test_add_func("/signer/nip55l/error-map-contract", test_error_map_contract);
  return g_test_run();
}
