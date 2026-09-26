/* test_signer_dbus_contract — real-service D-Bus contract test (nostrc-tf3b).
 *
 * Spins up a private session bus with GTestDBus, spawns the actual
 * nostr-signer-daemon on it, and drives the full round-trip through the
 * generated org.nostr.Signer glue: GetPublicKey → SignEvent (with a
 * pre-populated ACL) → parse and cryptographically verify the returned
 * event → NIP44EncryptB64 → NIP44DecryptB64 → NIP44DeriveConversationKey
 * (ACL allow / ACL deny / interactive ApprovalRequested → ApproveRequest
 * approve + deny / malformed peer) → GetRelays → StoreKey /
 * ClearKey (best-effort; libsecret required and skipped clean if absent).
 * Phase 3 (nostrc-bml6, needs gnome-keyring-daemon; skipped clean if absent)
 * runs a real Secret Service on the private bus: legacy-schema items seeded
 * before the daemon starts must be migrated to org.gnostr.Signer/identity
 * and be usable for SignEvent, StoreKey must write the unified schema, and
 * the per-keyring marker must stop a second migration pass.
 * Also asserts the daemon's error path shape: malformed input →
 * Error.InvalidInput; GetRelays on a fresh install → Error.NotFound;
 * mutations without the escape hatch → Error.PermissionDenied.
 *
 * apps/gnostr-signer/tests/test-dbus.c:55-60,117-143 was the private-bus
 * pattern reference, but that test drives a mock: the point of this one is
 * that the real daemon speaks the same contract, using the exact GLib
 * service (signer_service_g.c → signer_dbus.[ch]) an installed system
 * would boot. The daemon binary path is baked in at compile time
 * (NIP55L_DAEMON_PATH) so the test never guesses at install locations.
 *
 * The daemon reads its identity from $NOSTR_SIGNER_SECKEY_HEX when the
 * caller supplies an empty selector, and its relays from
 * $XDG_CONFIG_HOME/nostr/relays.conf — both parked in a per-run tmp dir so
 * the running user's real signer state is never touched.
 */

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <nostr/nip44/nip44.h>

#ifdef NIP55L_TEST_HAVE_LIBSECRET
#include <libsecret/secret.h>
#include <nostr/nip55l/signer_ops.h>
#include "seahorse/secret_store.h"
#endif

#ifndef NIP55L_DAEMON_PATH
#error "NIP55L_DAEMON_PATH must be defined by the build (path to nostr-signer-daemon)"
#endif

#define BUS_NAME     "org.nostr.Signer"
#define OBJ_PATH     "/org/nostr/signer"
#define IFACE        "org.nostr.Signer"

#define ERR_INVALID  "org.nostr.Signer.Error.InvalidInput"
#define ERR_NOT_FND  "org.nostr.Signer.Error.NotFound"
#define ERR_PERM     "org.nostr.Signer.Error.PermissionDenied"
#define ERR_DENIED   "org.nostr.Signer.Error.ApprovalDenied"

#define CHECK(cond) do { if (!(cond)) { \
    g_printerr("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); \
  } } while (0)

/* ---------------------------------------------------------------------------
 * Utilities
 * ------------------------------------------------------------------------- */

typedef struct { GMainLoop *loop; gboolean appeared; } WaitCtx;

static void on_appeared(GDBusConnection *c, const gchar *n, const gchar *o, gpointer ud) {
  (void)c; (void)n; (void)o;
  WaitCtx *w = ud; w->appeared = TRUE; g_main_loop_quit(w->loop);
}
static gboolean on_timeout(gpointer ud) {
  WaitCtx *w = ud; g_main_loop_quit(w->loop); return G_SOURCE_REMOVE;
}

static void wait_for_named(GDBusConnection *bus, const char *name, guint timeout_s) {
  WaitCtx w = { g_main_loop_new(NULL, FALSE), FALSE };
  guint watch = g_bus_watch_name_on_connection(bus, name,
                                               G_BUS_NAME_WATCHER_FLAGS_NONE,
                                               on_appeared, NULL, &w, NULL);
  guint to = g_timeout_add_seconds(timeout_s, on_timeout, &w);
  g_main_loop_run(w.loop);
  if (w.appeared) g_source_remove(to);
  g_bus_unwatch_name(watch);
  g_main_loop_unref(w.loop);
  CHECK(w.appeared);
}

static void wait_for_name(GDBusConnection *bus, guint timeout_s) {
  wait_for_named(bus, BUS_NAME, timeout_s);
}

static GVariant *call(GDBusConnection *bus, const char *method, GVariant *args,
                      const char *reply_sig, GError **err) {
  return g_dbus_connection_call_sync(bus, BUS_NAME, OBJ_PATH, IFACE, method,
                                     args, G_VARIANT_TYPE(reply_sig),
                                     G_DBUS_CALL_FLAGS_NONE, 10000, NULL, err);
}

static void write_file(const char *path, const char *body) {
  GError *err = NULL;
  CHECK(g_file_set_contents(path, body, -1, &err));
  CHECK(g_chmod(path, 0600) == 0);
}

/* Convert an npub-shaped identity to the raw 64-hex pubkey the tests need
 * to check who signed a returned event. */
static char *npub_to_hex(const char *npub) {
  uint8_t pk[32];
  CHECK(nostr_nip19_decode_npub(npub, pk) == 0);
  char *hex = g_malloc(65);
  for (int i = 0; i < 32; i++) g_snprintf(hex + 2 * i, 3, "%02x", pk[i]);
  hex[64] = '\0';
  return hex;
}

/* ---------------------------------------------------------------------------
 * Fixture: tmp dir tree + daemon subprocess on a private GTestDBus bus.
 * ------------------------------------------------------------------------- */

typedef struct {
  GTestDBus  *tbus;
  GSubprocess *keyring;  /* phase 3 only: gnome-keyring-daemon on tbus */
  GSubprocess *daemon;
  GDBusConnection *bus;
  char *tmpdir;
  char *sk_hex;   /* the daemon's identity */
  char *pk_hex;   /* x-only pubkey hex */
  char *npub;     /* npub of the pk */
} Ctx;

/* pre_daemon runs on the private bus before the daemon is spawned (phase 3
 * starts a keyring and seeds legacy items there). extra_acl is appended to
 * the [SignEvent] section. */
typedef void (*PreDaemonFn)(Ctx *ctx, gpointer data);

static void ctx_setup_full(Ctx *ctx, gboolean allow_mutations, gboolean write_relays,
                           const char *extra_acl, PreDaemonFn pre_daemon, gpointer data) {
  memset(ctx, 0, sizeof *ctx);

  /* Isolated $XDG_CONFIG_HOME / $HOME so the real user's configs are safe. */
  const char *tmp_base = g_get_tmp_dir();
  ctx->tmpdir = g_build_filename(tmp_base, "nip55l_dbus_contractXXXXXX", NULL);
  CHECK(mkdtemp(ctx->tmpdir) != NULL);

  char *xdg_cfg = g_build_filename(ctx->tmpdir, "config", NULL);
  char *xdg_data = g_build_filename(ctx->tmpdir, "data", NULL);
  char *xdg_runtime = g_build_filename(ctx->tmpdir, "runtime", NULL);
  g_mkdir_with_parents(xdg_cfg, 0700);
  g_mkdir_with_parents(xdg_data, 0700);
  g_mkdir_with_parents(xdg_runtime, 0700);
  g_setenv("XDG_CONFIG_HOME", xdg_cfg, TRUE);
  g_setenv("XDG_DATA_HOME", xdg_data, TRUE);
  g_setenv("XDG_RUNTIME_DIR", xdg_runtime, TRUE);
  g_setenv("HOME", ctx->tmpdir, TRUE);
  /* Ensure the daemon never blocks on the unrelated user bus. */
  g_unsetenv("DBUS_SESSION_BUS_ADDRESS");

  /* Test identity: a fresh Schnorr key, exposed to the daemon via the env
   * lane so no libsecret / Keychain is involved and the run is hermetic. */
  ctx->sk_hex = nostr_key_generate_private();
  CHECK(ctx->sk_hex != NULL);
  ctx->pk_hex = nostr_key_get_public(ctx->sk_hex);
  CHECK(ctx->pk_hex != NULL);
  uint8_t pk[32];
  CHECK(nostr_hex2bin(pk, ctx->pk_hex, 32));
  CHECK(nostr_nip19_encode_npub(pk, &ctx->npub) == 0 && ctx->npub);
  g_setenv("NOSTR_SIGNER_SECKEY_HEX", ctx->sk_hex, TRUE);
  if (allow_mutations)
    g_setenv("NOSTR_SIGNER_ALLOW_KEY_MUTATIONS", "1", TRUE);
  else
    g_unsetenv("NOSTR_SIGNER_ALLOW_KEY_MUTATIONS");

  /* Pre-populate the ACL so SignEvent takes the auto-approve branch and
   * never needs an ApproveRequest reply. The daemon's key is "<app>:<id>",
   * and identity is what the caller passes over the wire — we pass the
   * empty string in the tests so both parts are stable. */
  {
    char *gnostr_dir = g_build_filename(xdg_cfg, "gnostr", NULL);
    g_mkdir_with_parents(gnostr_dir, 0700);
    char *acl = g_build_filename(gnostr_dir, "signer-acl.ini", NULL);
    /* NIP44DeriveConversationKey (nip55l 0.3.0): "contract-test" is
     * pre-allowed, "contract-deny" pre-denied; any other app_id goes
     * through ApprovalRequested. */
    gchar *body = g_strdup_printf("[NIP44DeriveConversationKey]\n"
                                  "contract-test:=allow\n"
                                  "contract-deny:=deny\n"
                                  "[SignEvent]\ncontract-test:=allow\n%s",
                                  extra_acl ? extra_acl : "");
    write_file(acl, body);
    g_free(body);
    g_free(acl);
    g_free(gnostr_dir);
  }

  /* Relays: leave the file absent for the first GetRelays call (NotFound
   * expected), then write it later when the test wants the happy path. */
  if (write_relays) {
    char *nostr_dir = g_build_filename(xdg_cfg, "nostr", NULL);
    g_mkdir_with_parents(nostr_dir, 0700);
    char *relays = g_build_filename(nostr_dir, "relays.conf", NULL);
    write_file(relays,
               "[\"wss://relay.example\", \"wss://nos.lol\"]\n");
    g_free(relays);
    g_free(nostr_dir);
  }

  /* Private session bus. g_test_dbus_up sets DBUS_SESSION_BUS_ADDRESS in
   * this process's env, which g_subprocess_new inherits. */
  ctx->tbus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(ctx->tbus);

  if (pre_daemon) pre_daemon(ctx, data);

  GError *err = NULL;
  ctx->daemon = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                 G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                 &err, NIP55L_DAEMON_PATH, NULL);
  if (!ctx->daemon) {
    g_printerr("spawn daemon (%s): %s\n", NIP55L_DAEMON_PATH,
               err ? err->message : "?");
    exit(1);
  }
  g_clear_error(&err);

  ctx->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  CHECK(ctx->bus != NULL);

  wait_for_name(ctx->bus, 20);

  g_free(xdg_cfg); g_free(xdg_data); g_free(xdg_runtime);
}

static void ctx_setup(Ctx *ctx, gboolean allow_mutations, gboolean write_relays) {
  ctx_setup_full(ctx, allow_mutations, write_relays, NULL, NULL, NULL);
}

static void ctx_teardown(Ctx *ctx) {
  if (ctx->daemon) {
    g_subprocess_force_exit(ctx->daemon);
    (void)g_subprocess_wait(ctx->daemon, NULL, NULL);
    g_object_unref(ctx->daemon);
  }
  if (ctx->keyring) {
    g_subprocess_force_exit(ctx->keyring);
    (void)g_subprocess_wait(ctx->keyring, NULL, NULL);
  }
  if (ctx->bus) g_object_unref(ctx->bus);
  if (ctx->tbus) {
    if (ctx->keyring) {
      /* libsecret's sync API parks proxies (and their session-bus refs) on
       * private main contexts that are never iterated again, so the bus
       * singleton is never finalized and g_test_dbus_down — also run by
       * GTestDBus's dispose — stalls on its 30 s weak-notify timeout. The
       * keyring fixture runs last: stop the bus without that check and let
       * process exit reclaim the GTestDBus object. */
#ifdef NIP55L_TEST_HAVE_LIBSECRET
      secret_service_disconnect();
#endif
      GDBusConnection *singleton = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
      if (singleton) {
        g_dbus_connection_set_exit_on_close(singleton, FALSE);
        g_object_unref(singleton);
      }
      g_test_dbus_stop(ctx->tbus);
    } else {
      g_test_dbus_down(ctx->tbus);
      g_object_unref(ctx->tbus);
    }
  }
  g_clear_object(&ctx->keyring);
  if (ctx->tmpdir) {
    /* Best-effort recursive cleanup; a leftover tmpdir is a hygiene issue,
     * not a test failure. */
    char *cmd = g_strdup_printf("rm -rf '%s'", ctx->tmpdir);
    int rc = system(cmd);
    (void)rc;
    g_free(cmd);
    g_free(ctx->tmpdir);
  }
  free(ctx->sk_hex);
  free(ctx->pk_hex);
  free(ctx->npub);
}

/* ---------------------------------------------------------------------------
 * Contract assertions
 * ------------------------------------------------------------------------- */

static void assert_signed_event(const char *reply_json, const char *want_pk_hex,
                                int want_kind, int64_t not_before) {
  CHECK(reply_json && reply_json[0] == '{');
  NostrEvent *ev = nostr_event_new();
  CHECK(ev != NULL);
  CHECK(nostr_event_deserialize_signed(ev, reply_json, NULL)
        == NOSTR_EVENT_VALIDATION_OK);
  CHECK(nostr_event_validate(ev, NULL) == NOSTR_EVENT_VALIDATION_OK);
  CHECK(g_strcmp0(nostr_event_get_pubkey(ev), want_pk_hex) == 0);
  CHECK(nostr_event_get_kind(ev) == want_kind);
  CHECK(nostr_event_get_created_at(ev) >= not_before);
  nostr_event_free(ev);
}

static void test_get_public_key(Ctx *ctx) {
  GError *err = NULL;
  GVariant *ret = call(ctx->bus, "GetPublicKey", NULL, "(s)", &err);
  CHECK(ret != NULL);
  const char *npub = NULL;
  g_variant_get(ret, "(&s)", &npub);
  CHECK(g_strcmp0(npub, ctx->npub) == 0);
  /* Sanity: decoding it back produces the pubkey the env var derived from. */
  char *hex = npub_to_hex(npub);
  CHECK(g_strcmp0(hex, ctx->pk_hex) == 0);
  g_free(hex);
  g_variant_unref(ret);
}

static void test_sign_event_ok(Ctx *ctx) {
  /* Auto-approve branch: identity="" matches the pre-populated ACL entry.
   * Template with a real timestamp; the signer must return the full signed
   * event JSON, its own pubkey embedded, verified by libnostr. */
  int64_t now = (int64_t)time(NULL);
  gchar *tmpl = g_strdup_printf(
      "{\"kind\":1,\"created_at\":%lld,\"tags\":[[\"e\",\"aa\"]],"
      "\"content\":\"hi there\"}",
      (long long)now);
  GError *err = NULL;
  GVariant *ret = call(ctx->bus, "SignEvent",
                       g_variant_new("(sss)", tmpl, "", "contract-test"),
                       "(s)", &err);
  if (!ret) { g_printerr("SignEvent: %s\n", err ? err->message : "?"); exit(1); }
  const char *signed_json = NULL;
  g_variant_get(ret, "(&s)", &signed_json);
  assert_signed_event(signed_json, ctx->pk_hex, 1, now);
  g_variant_unref(ret);
  g_free(tmpl);

  /* Zero created_at is filled with the current time. */
  int64_t before = (int64_t)time(NULL);
  ret = call(ctx->bus, "SignEvent",
             g_variant_new("(sss)",
                           "{\"kind\":22242,\"created_at\":0,"
                           "\"tags\":[[\"challenge\",\"abc\"]],\"content\":\"\"}",
                           "", "contract-test"),
             "(s)", &err);
  CHECK(ret != NULL);
  g_variant_get(ret, "(&s)", &signed_json);
  assert_signed_event(signed_json, ctx->pk_hex, 22242, before);
  g_variant_unref(ret);
}

static void test_sign_event_bad_json(Ctx *ctx) {
  /* Malformed input is refused with the typed error name every consumer
   * checks; not a "signature", not a truncated reply. */
  GError *err = NULL;
  GVariant *ret = call(ctx->bus, "SignEvent",
                       g_variant_new("(sss)", "{not json", "", "contract-test"),
                       "(s)", &err);
  CHECK(ret == NULL && err != NULL);
  gchar *remote = g_dbus_error_get_remote_error(err);
  CHECK(g_strcmp0(remote, ERR_INVALID) == 0);
  g_free(remote);
  g_clear_error(&err);
}

static void test_nip44_b64_roundtrip(Ctx *ctx) {
  /* A recipient of the ciphertext: another random Schnorr key. The signer
   * encrypts under (my_sk, peer_pk); the test decrypts back on the same
   * signer identity, feeding the peer's pub as the identity's own peer. */
  char *peer_sk = nostr_key_generate_private();
  char *peer_pk = nostr_key_get_public(peer_sk);
  CHECK(peer_sk && peer_pk);

  /* A 136-byte "rekey blob" width: raw bytes, non-UTF-8, with a NUL. The
   * whole point of the b64 lane is that this survives the D-Bus string
   * transport untouched. */
  uint8_t blob[136];
  for (size_t i = 0; i < sizeof blob; i++) blob[i] = (uint8_t)((i * 7u + 0x80u) & 0xffu);
  blob[0] = 0x00;
  blob[1] = 0xff;
  blob[sizeof blob - 1] = 0xc0;

  gchar *b64_in = g_base64_encode(blob, sizeof blob);

  GError *err = NULL;
  GVariant *ret = call(ctx->bus, "NIP44EncryptB64",
                       g_variant_new("(sss)", b64_in, peer_pk, ""),
                       "(s)", &err);
  CHECK(ret != NULL);
  const char *payload = NULL;
  g_variant_get(ret, "(&s)", &payload);
  gchar *payload_dup = g_strdup(payload);
  g_variant_unref(ret);

  /* Because both peers are the same identity in the daemon (the daemon
   * only has one key), we exercise DecryptB64 with (payload, peer_pk_as_id,
   * my_pk_as_peer)? Actually the daemon's key is fixed to the env var, so
   * "identity" is ignored (empty selector → env). We need decrypt to be
   * done against the same shared secret, i.e. by asking the daemon to
   * decrypt with peer_pk as the peer. That is exactly what the encrypt call
   * used, so the daemon (as recipient of its own payload) cannot open it;
   * ECDH is symmetric between (sk_A, pk_B) and (sk_B, pk_A). What we can
   * check byte-for-byte on the wire: the ciphertext decrypts back through
   * the reverse call — the daemon speaking as A, we speaking as B — but
   * that needs the daemon to hold B's key. It doesn't, so we perform the
   * B-side decrypt in-process against the returned ciphertext with libnostr.
   */
  {
    uint8_t peer_sk_bin[32], my_pk_bin[32];
    CHECK(nostr_hex2bin(peer_sk_bin, peer_sk, sizeof peer_sk_bin));
    CHECK(nostr_hex2bin(my_pk_bin, ctx->pk_hex, sizeof my_pk_bin));
    uint8_t *plain = NULL; size_t plain_len = 0;
    CHECK(nostr_nip44_decrypt_v2(peer_sk_bin, my_pk_bin, payload_dup,
                                 &plain, &plain_len) == 0);
    CHECK(plain_len == sizeof blob);
    CHECK(memcmp(plain, blob, sizeof blob) == 0);
    free(plain);
  }

  /* Now the reverse direction over D-Bus: ask the daemon to open a payload
   * addressed *to* it. Encrypt in-process as the peer, then hand the
   * ciphertext to NIP44DecryptB64 with the peer's pubkey as the sender. */
  {
    uint8_t peer_sk_bin[32], my_pk_bin[32];
    CHECK(nostr_hex2bin(peer_sk_bin, peer_sk, sizeof peer_sk_bin));
    CHECK(nostr_hex2bin(my_pk_bin, ctx->pk_hex, sizeof my_pk_bin));
    char *incoming = NULL;
    CHECK(nostr_nip44_encrypt_v2(peer_sk_bin, my_pk_bin, blob, sizeof blob,
                                 &incoming) == 0 && incoming);
    ret = call(ctx->bus, "NIP44DecryptB64",
               g_variant_new("(sss)", incoming, peer_pk, ""),
               "(s)", &err);
    free(incoming);
    if (!ret) { g_printerr("NIP44DecryptB64: %s\n", err ? err->message : "?"); exit(1); }
    const char *plain_b64 = NULL;
    g_variant_get(ret, "(&s)", &plain_b64);
    gsize back_len = 0;
    guchar *back = g_base64_decode(plain_b64, &back_len);
    CHECK(back_len == sizeof blob);
    CHECK(memcmp(back, blob, sizeof blob) == 0);
    g_free(back);
    g_variant_unref(ret);
  }

  g_free(payload_dup);
  g_free(b64_in);
  free(peer_sk);
  free(peer_pk);
}

/* ---- NIP44DeriveConversationKey (nostrc-da9c) --------------------------- */

static char *expected_convkey_hex(const char *peer_sk_hex, const char *my_pk_hex) {
  /* ECDH is symmetric: convkey(my_sk, peer_pk) == convkey(peer_sk, my_pk). */
  uint8_t sk[32], pk[32], ck[32];
  CHECK(nostr_hex2bin(sk, peer_sk_hex, sizeof sk));
  CHECK(nostr_hex2bin(pk, my_pk_hex, sizeof pk));
  CHECK(nostr_nip44_convkey(sk, pk, ck) == 0);
  char *hex = g_malloc(65);
  for (int i = 0; i < 32; i++) g_snprintf(hex + 2 * i, 3, "%02x", ck[i]);
  hex[64] = '\0';
  return hex;
}

static void expect_remote_error(GError *err, const char *name) {
  CHECK(err != NULL);
  gchar *remote = g_dbus_error_get_remote_error(err);
  if (g_strcmp0(remote, name) != 0)
    g_printerr("expected %s, got %s (%s)\n", name, remote ? remote : "(none)", err->message);
  CHECK(g_strcmp0(remote, name) == 0);
  g_free(remote);
}

typedef struct {
  GMainLoop *loop;
  char *req_id, *kind, *preview, *app_id;
  GVariant *reply;
  GError *err;
  gboolean done;
} ApprCtx;

static void on_approval_requested(GDBusConnection *c, const gchar *snd, const gchar *path,
                                  const gchar *iface, const gchar *sig, GVariant *params,
                                  gpointer ud) {
  (void)c; (void)snd; (void)path; (void)iface; (void)sig;
  ApprCtx *a = ud;
  const char *app = NULL, *id = NULL, *kind = NULL, *prev = NULL, *rid = NULL;
  g_variant_get(params, "(&s&s&s&s&s)", &app, &id, &kind, &prev, &rid);
  g_free(a->req_id); a->req_id = g_strdup(rid);
  g_free(a->kind); a->kind = g_strdup(kind);
  g_free(a->preview); a->preview = g_strdup(prev);
  g_free(a->app_id); a->app_id = g_strdup(app);
  g_main_loop_quit(a->loop);
}

static void on_convkey_reply(GObject *src, GAsyncResult *res, gpointer ud) {
  ApprCtx *a = ud;
  a->reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &a->err);
  a->done = TRUE;
  g_main_loop_quit(a->loop);
}

static gboolean appr_timeout(gpointer ud) {
  ApprCtx *a = ud;
  g_printerr("FAIL: approval round-trip timed out\n");
  exit(1);
  g_main_loop_quit(a->loop);
  return G_SOURCE_REMOVE;
}

/* Interactive lane: no ACL entry for app_id, so the daemon parks the call,
 * emits ApprovalRequested, and completes it when ApproveRequest decides. */
static void convkey_interactive(Ctx *ctx, const char *peer_pk, const char *app_id,
                                gboolean decision, const char *want_hex) {
  ApprCtx a = { .loop = g_main_loop_new(NULL, FALSE) };
  guint sub = g_dbus_connection_signal_subscribe(ctx->bus, BUS_NAME, IFACE,
                                                 "ApprovalRequested", OBJ_PATH, NULL,
                                                 G_DBUS_SIGNAL_FLAGS_NONE,
                                                 on_approval_requested, &a, NULL);
  guint to = g_timeout_add_seconds(20, appr_timeout, &a);
  g_dbus_connection_call(ctx->bus, BUS_NAME, OBJ_PATH, IFACE, "NIP44DeriveConversationKey",
                         g_variant_new("(sss)", peer_pk, "", app_id),
                         G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 30000, NULL,
                         on_convkey_reply, &a);
  while (!a.req_id && !a.done) g_main_loop_run(a.loop);
  CHECK(!a.done);           /* must be parked, not answered */
  CHECK(a.req_id != NULL);
  CHECK(g_strcmp0(a.kind, "nip44_conversation_key") == 0);
  CHECK(g_strcmp0(a.app_id, app_id) == 0);
  CHECK(a.preview && strstr(a.preview, peer_pk) != NULL);

  GError *err = NULL;
  GVariant *ok = call(ctx->bus, "ApproveRequest",
                      g_variant_new("(sbbt)", a.req_id, decision, FALSE, (guint64)0),
                      "(b)", &err);
  if (!ok) { g_printerr("ApproveRequest: %s\n", err ? err->message : "?"); exit(1); }
  gboolean handled = FALSE;
  g_variant_get(ok, "(b)", &handled);
  CHECK(handled);
  g_variant_unref(ok);

  while (!a.done) g_main_loop_run(a.loop);
  g_source_remove(to);
  g_dbus_connection_signal_unsubscribe(ctx->bus, sub);
  if (decision) {
    if (!a.reply) { g_printerr("convkey after approve: %s\n", a.err ? a.err->message : "?"); exit(1); }
    const char *hex = NULL;
    g_variant_get(a.reply, "(&s)", &hex);
    CHECK(g_strcmp0(hex, want_hex) == 0);
    g_variant_unref(a.reply);
  } else {
    CHECK(a.reply == NULL);
    expect_remote_error(a.err, ERR_DENIED);
    g_clear_error(&a.err);
  }
  g_free(a.req_id); g_free(a.kind); g_free(a.preview); g_free(a.app_id);
  g_main_loop_unref(a.loop);
}

static void test_nip44_derive_conversation_key(Ctx *ctx) {
  char *peer_sk = nostr_key_generate_private();
  char *peer_pk = nostr_key_get_public(peer_sk);
  CHECK(peer_sk && peer_pk);
  char *want = expected_convkey_hex(peer_sk, ctx->pk_hex);

  /* ACL allow: answered immediately, equal to the NIP-44 derivation on the
   * peer's side, and usable with the stock NIP-44 v2 cipher. */
  GError *err = NULL;
  GVariant *ret = call(ctx->bus, "NIP44DeriveConversationKey",
                       g_variant_new("(sss)", peer_pk, "", "contract-test"), "(s)", &err);
  if (!ret) { g_printerr("NIP44DeriveConversationKey: %s\n", err ? err->message : "?"); exit(1); }
  const char *hex = NULL;
  g_variant_get(ret, "(&s)", &hex);
  CHECK(strlen(hex) == 64);
  CHECK(g_strcmp0(hex, want) == 0);
  {
    uint8_t ck[32], peer_sk_bin[32], my_pk_bin[32];
    CHECK(nostr_hex2bin(ck, hex, sizeof ck));
    CHECK(nostr_hex2bin(peer_sk_bin, peer_sk, sizeof peer_sk_bin));
    CHECK(nostr_hex2bin(my_pk_bin, ctx->pk_hex, sizeof my_pk_bin));
    char *payload = NULL;
    CHECK(nostr_nip44_encrypt_v2(peer_sk_bin, my_pk_bin, (const uint8_t *)"sealed", 6, &payload) == 0);
    uint8_t *pt = NULL; size_t pt_len = 0;
    CHECK(nostr_nip44_decrypt_v2_with_convkey(ck, payload, &pt, &pt_len) == 0);
    CHECK(pt_len == 6 && memcmp(pt, "sealed", 6) == 0);
    free(pt); free(payload);
  }
  g_variant_unref(ret);

  /* Uppercase hex peers are the same key. */
  gchar *upper = g_ascii_strup(peer_pk, -1);
  ret = call(ctx->bus, "NIP44DeriveConversationKey",
             g_variant_new("(sss)", upper, "", "contract-test"), "(s)", &err);
  CHECK(ret != NULL);
  g_variant_get(ret, "(&s)", &hex);
  CHECK(g_strcmp0(hex, want) == 0);
  g_variant_unref(ret);
  g_free(upper);

  /* ACL deny: refused without prompting. */
  ret = call(ctx->bus, "NIP44DeriveConversationKey",
             g_variant_new("(sss)", peer_pk, "", "contract-deny"), "(s)", &err);
  CHECK(ret == NULL);
  expect_remote_error(err, ERR_DENIED);
  g_clear_error(&err);

  /* Malformed and off-curve peers: InvalidInput, never an approval prompt.
   * x = 5 has no point on secp256k1 (5^3 + 7 = 132 is a non-residue). */
  const char *bad[] = {
    "zz", "not-hex-not-hex-not-hex-not-hex-not-hex-not-hex-not-hex-not-hex",
    "0000000000000000000000000000000000000000000000000000000000000005",
  };
  for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
    ret = call(ctx->bus, "NIP44DeriveConversationKey",
               g_variant_new("(sss)", bad[i], "", "contract-test"), "(s)", &err);
    CHECK(ret == NULL);
    expect_remote_error(err, ERR_INVALID);
    g_clear_error(&err);
  }

  /* Interactive approve, then interactive deny (distinct app_ids with no
   * ACL entry; the 100 ms per-sender rate limit is respected). */
  g_usleep(150 * 1000);
  convkey_interactive(ctx, peer_pk, "contract-ask", TRUE, want);
  g_usleep(150 * 1000);
  convkey_interactive(ctx, peer_pk, "contract-ask-2", FALSE, NULL);

  g_free(want);
  free(peer_sk);
  free(peer_pk);
}

static void test_get_relays_paths(Ctx *ctx, gboolean expect_ok) {
  GError *err = NULL;
  GVariant *ret = call(ctx->bus, "GetRelays", NULL, "(s)", &err);
  if (expect_ok) {
    CHECK(ret != NULL);
    const char *json = NULL;
    g_variant_get(ret, "(&s)", &json);
    /* Order-preserving read of the file we wrote. */
    CHECK(g_strcmp0(json, "[\"wss://relay.example\",\"wss://nos.lol\"]") == 0);
    g_variant_unref(ret);
  } else {
    CHECK(ret == NULL && err != NULL);
    gchar *remote = g_dbus_error_get_remote_error(err);
    CHECK(g_strcmp0(remote, ERR_NOT_FND) == 0);
    g_free(remote);
    g_clear_error(&err);
  }
}

static void test_store_key_denied_without_flag(Ctx *ctx) {
  /* This runs on a fixture built without allow_mutations: the daemon must
   * refuse StoreKey and ClearKey with PermissionDenied, regardless of the
   * key material shape. */
  GError *err = NULL;
  GVariant *ret = call(ctx->bus, "StoreKey",
                       g_variant_new("(ss)", ctx->sk_hex, ""),
                       "(bs)", &err);
  CHECK(ret == NULL && err != NULL);
  gchar *remote = g_dbus_error_get_remote_error(err);
  CHECK(g_strcmp0(remote, ERR_PERM) == 0);
  g_free(remote);
  g_clear_error(&err);
}

/* StoreKey → GetPublicKey → ClearKey when libsecret is functional. Returns
 * TRUE if the whole chain ran; FALSE if libsecret was unavailable so the
 * outer test could log-and-skip rather than fail. */
static gboolean try_store_and_clear_key(Ctx *ctx) {
  /* A separate key so we can tell whether the round-trip changed the
   * daemon's derived npub. */
  char *fresh_sk = nostr_key_generate_private();
  CHECK(fresh_sk != NULL);
  char *fresh_pk = nostr_key_get_public(fresh_sk);
  CHECK(fresh_pk != NULL);
  uint8_t pk[32]; CHECK(nostr_hex2bin(pk, fresh_pk, 32));
  char *fresh_npub = NULL;
  CHECK(nostr_nip19_encode_npub(pk, &fresh_npub) == 0 && fresh_npub);

  GError *err = NULL;
  GVariant *ret = call(ctx->bus, "StoreKey",
                       g_variant_new("(ss)", fresh_sk, ""),
                       "(bs)", &err);
  if (!ret) {
    /* SecretServiceUnavailable / any BACKEND flavour = libsecret missing
     * on this host. Not a test failure: we exercised the permission
     * boundary in test_store_key_denied_without_flag already. */
    gchar *remote = err ? g_dbus_error_get_remote_error(err) : NULL;
    g_printerr("SKIP: StoreKey backend unavailable: %s\n",
               remote ? remote : (err ? err->message : "?"));
    g_free(remote); g_clear_error(&err);
    free(fresh_sk); free(fresh_pk); free(fresh_npub);
    return FALSE;
  }
  gboolean ok = FALSE; const char *out_npub = NULL;
  g_variant_get(ret, "(bs)", &ok, &out_npub);
  CHECK(ok);
  /* The daemon derives npub from the stored key, not from the env-var key. */
  CHECK(g_strcmp0(out_npub, fresh_npub) == 0);
  g_variant_unref(ret);

  /* nostrc-7g9d regression: GetPublicKey immediately after StoreKey must
   * return the just-stored key's npub, not fall back to the env-var
   * identity or fail with NoKeyConfigured. Papa's smoke against the lab
   * caught the pre-fix bug where a libsecret search race returned
   * NOT_FOUND for a key libsecret had already accepted. The in-process
   * cache added in nostr_nip55l_store_key() closes that race. */
  {
    GVariant *gpk_ret = call(ctx->bus, "GetPublicKey", NULL, "(s)", &err);
    if (!gpk_ret) {
      g_printerr("GetPublicKey after StoreKey: %s\n", err ? err->message : "?");
      exit(1);
    }
    const char *live_npub = NULL;
    g_variant_get(gpk_ret, "(&s)", &live_npub);
    CHECK(g_strcmp0(live_npub, fresh_npub) == 0);
    g_variant_unref(gpk_ret);
  }

  /* Clean up the stored item so the run leaves no residue in libsecret. */
  ret = call(ctx->bus, "ClearKey", g_variant_new("(s)", fresh_npub),
             "(b)", &err);
  if (ret) {
    g_variant_get(ret, "(b)", &ok);
    g_variant_unref(ret);
  } else {
    /* ClearKey returned an error; log it. Cleanup best-effort. */
    g_printerr("WARN: ClearKey failed: %s\n", err ? err->message : "?");
    g_clear_error(&err);
  }
  free(fresh_sk); free(fresh_pk); free(fresh_npub);
  return TRUE;
}

/* ---------------------------------------------------------------------------
 * Phase 3: real Secret Service (nostrc-bml6)
 * ------------------------------------------------------------------------- */

#ifdef NIP55L_TEST_HAVE_LIBSECRET

typedef struct { char *sk_hex, *pk_hex, *npub, *nsec; } TestKey;

static void test_key_new(TestKey *k) {
  k->sk_hex = nostr_key_generate_private();
  CHECK(k->sk_hex != NULL);
  k->pk_hex = nostr_key_get_public(k->sk_hex);
  CHECK(k->pk_hex != NULL);
  uint8_t b[32];
  CHECK(nostr_hex2bin(b, k->pk_hex, 32));
  CHECK(nostr_nip19_encode_npub(b, &k->npub) == 0 && k->npub);
  CHECK(nostr_hex2bin(b, k->sk_hex, 32));
  CHECK(nostr_nip19_encode_nsec(b, &k->nsec) == 0 && k->nsec);
}

static void test_key_free(TestKey *k) {
  free(k->sk_hex); free(k->pk_hex); free(k->npub); free(k->nsec);
}

typedef struct {
  TestKey legacy_signer;   /* org.gnostr.Signer/key, hex secret */
  TestKey legacy_helper;   /* org.gnostr.Key, nsec secret */
  TestKey legacy_hardware; /* org.gnostr.Key origin=hardware: must stay */
  TestKey pre_bml6;        /* old daemon attribute set under the same schema */
} Phase3;

/* Items of schema, optionally filtered by one attribute, secrets loaded. */
static GList *search_items(const SecretSchema *schema, const char *attr, const char *value) {
  GError *err = NULL;
  SecretService *svc = secret_service_get_sync(SECRET_SERVICE_OPEN_SESSION, NULL, &err);
  if (!svc) { g_printerr("secret service: %s\n", err ? err->message : "?"); exit(1); }
  GHashTable *a = g_hash_table_new(g_str_hash, g_str_equal);
  if (attr) g_hash_table_insert(a, (gpointer)attr, (gpointer)value);
  GList *items = secret_service_search_sync(svc, schema, a,
      SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK | SECRET_SEARCH_LOAD_SECRETS, NULL, &err);
  g_hash_table_unref(a);
  if (err) { g_printerr("search: %s\n", err->message); exit(1); }
  g_object_unref(svc);
  return items;
}

static guint count_items(const SecretSchema *schema, const char *attr, const char *value) {
  GList *items = search_items(schema, attr, value);
  guint n = g_list_length(items);
  g_list_free_full(items, g_object_unref);
  return n;
}

static void seed(const SecretSchema *schema, const char *label, const char *secret, ...) {
  va_list ap;
  va_start(ap, secret);
  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  const char *k;
  while ((k = va_arg(ap, const char *)) != NULL)
    g_hash_table_insert(attrs, (gpointer)k, (gpointer)va_arg(ap, const char *));
  va_end(ap);
  GError *err = NULL;
  if (!secret_password_storev_sync(schema, attrs, SECRET_COLLECTION_DEFAULT, label,
                                   secret, NULL, &err)) {
    g_printerr("seed %s: %s\n", schema->name, err ? err->message : "?");
    exit(1);
  }
  g_hash_table_unref(attrs);
}

/* Start gnome-keyring-daemon on the private bus (login keyring unlocked
 * from stdin, so no prompter is ever needed) and seed legacy items. */
static void phase3_pre_daemon(Ctx *ctx, gpointer data) {
  Phase3 *p = data;
  gchar *gk = g_find_program_in_path("gnome-keyring-daemon");
  CHECK(gk != NULL);
  GError *err = NULL;
  ctx->keyring = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                  G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                  G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                  &err, gk, "--foreground", "--unlock",
                                  "--components=secrets", NULL);
  g_free(gk);
  if (!ctx->keyring) { g_printerr("spawn keyring: %s\n", err ? err->message : "?"); exit(1); }
  GOutputStream *in = g_subprocess_get_stdin_pipe(ctx->keyring);
  CHECK(g_output_stream_write_all(in, "contract-test", 13, NULL, NULL, &err));
  CHECK(g_output_stream_close(in, NULL, &err));
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  CHECK(bus != NULL);
  wait_for_named(bus, "org.freedesktop.secrets", 20);
  g_object_unref(bus);

  seed(&gnostr_secret_legacy_signer_key_schema, "Nostr Key: Legacy Main",
       p->legacy_signer.sk_hex,
       "application", "gnostr-signer", "label", "Legacy Main",
       "npub", p->legacy_signer.npub, "key_type", "nostr",
       "created_at", "2025-01-01T00:00:00Z", NULL);
  seed(&gnostr_secret_legacy_helper_schema, "Nostr key", p->legacy_helper.nsec,
       "type", "nostr-key", "npub", p->legacy_helper.npub, "uid", "bob",
       "curve", "secp256k1", "origin", "software", NULL);
  seed(&gnostr_secret_legacy_helper_schema, "Nostr key", "pkcs11:token=yubikey;id=9c",
       "type", "nostr-key", "npub", p->legacy_hardware.npub, "uid", "token",
       "curve", "secp256k1", "origin", "hardware", "hardware_slot", "9c", NULL);
  /* What StoreKey wrote before the schema gained curve/origin. */
  gchar uid_buf[32];
  g_snprintf(uid_buf, sizeof uid_buf, "%u", (unsigned)getuid());
  seed(&gnostr_secret_schema, "Gnostr Identity Key", p->pre_bml6.sk_hex,
       "key_id", p->pre_bml6.npub, "npub", p->pre_bml6.npub,
       "owner_uid", uid_buf, "hardware", "false", NULL);
}

static const char *attr(GHashTable *a, const char *k) {
  const char *v = g_hash_table_lookup(a, k);
  return v ? v : "";
}

/* The single unified item for npub, checked against the expectations. A
 * non-empty label is also the key_id selector (StoreKey's convention). */
static void check_unified(const TestKey *k, const char *want_label_attr) {
  GList *items = search_items(&gnostr_secret_schema, "npub", k->npub);
  CHECK(g_list_length(items) == 1);
  SecretItem *it = items->data;
  GHashTable *a = secret_item_get_attributes(it);
  gchar uid_buf[32];
  g_snprintf(uid_buf, sizeof uid_buf, "%u", (unsigned)getuid());
  CHECK(g_strcmp0(attr(a, "key_id"),
                  *want_label_attr ? want_label_attr : k->npub) == 0);
  CHECK(g_strcmp0(attr(a, "curve"), "secp256k1") == 0);
  CHECK(g_strcmp0(attr(a, "origin"), "software") == 0);
  CHECK(g_strcmp0(attr(a, "hardware"), "false") == 0);
  CHECK(g_strcmp0(attr(a, "owner_uid"), uid_buf) == 0);
  CHECK(g_strcmp0(attr(a, "label"), want_label_attr) == 0);
  g_hash_table_unref(a);
  gchar *label = secret_item_get_label(it);
  gchar *want = gnostr_secret_store_build_label(want_label_attr, k->npub);
  CHECK(g_strcmp0(label, want) == 0);
  g_free(label); g_free(want);
  SecretValue *sv = secret_item_get_secret(it);
  CHECK(sv != NULL && g_strcmp0(secret_value_get_text(sv), k->sk_hex) == 0);
  secret_value_unref(sv);
  g_list_free_full(items, g_object_unref);
}

static void run_phase3(void) {
  gchar *gk = g_find_program_in_path("gnome-keyring-daemon");
  if (!gk) {
    g_print("SKIP phase 3: gnome-keyring-daemon not installed\n");
    return;
  }
  g_free(gk);

  Phase3 p;
  test_key_new(&p.legacy_signer);
  test_key_new(&p.legacy_helper);
  test_key_new(&p.legacy_hardware);
  test_key_new(&p.pre_bml6);
  gchar *acl = g_strdup_printf("contract-test:%s=allow\ncontract-test:Legacy Main=allow\n",
                               p.legacy_signer.npub);

  Ctx ctx;
  ctx_setup_full(&ctx, /*allow_mutations=*/TRUE, /*write_relays=*/FALSE, acl,
                 phase3_pre_daemon, &p);

  /* The daemon migrates off the main loop; the marker is its last write. */
  gboolean done = FALSE;
  for (int i = 0; i < 150 && !done; i++) {
    done = count_items(&gnostr_secret_migration_schema, NULL, NULL) == 1;
    if (!done) g_usleep(100 * 1000);
  }
  CHECK(done);

  /* Legacy software keys: moved, originals gone. */
  CHECK(count_items(&gnostr_secret_legacy_signer_key_schema, NULL, NULL) == 0);
  CHECK(count_items(&gnostr_secret_legacy_helper_schema, "npub", p.legacy_helper.npub) == 0);
  check_unified(&p.legacy_signer, "Legacy Main");
  check_unified(&p.legacy_helper, "bob");   /* nsec normalized to hex */
  /* Hardware reference: left in place, not copied. */
  CHECK(count_items(&gnostr_secret_legacy_helper_schema, "npub", p.legacy_hardware.npub) == 1);
  CHECK(count_items(&gnostr_secret_schema, "npub", p.legacy_hardware.npub) == 0);

  /* The daemon signs with a migrated key selected by npub (key_id is the
   * legacy label, so this exercises the npub fallback lookup), and by the
   * legacy label as selector. */
  for (int sel = 0; sel < 2; sel++) {
    const char *selector = sel == 0 ? p.legacy_signer.npub : "Legacy Main";
    int64_t now = (int64_t)time(NULL);
    gchar *tmpl = g_strdup_printf(
        "{\"kind\":1,\"created_at\":%lld,\"tags\":[],\"content\":\"migrated\"}",
        (long long)now);
    GError *err = NULL;
    GVariant *ret = call(ctx.bus, "SignEvent",
                         g_variant_new("(sss)", tmpl, selector, "contract-test"),
                         "(s)", &err);
    if (!ret) { g_printerr("SignEvent(migrated): %s\n", err ? err->message : "?"); exit(1); }
    const char *signed_json = NULL;
    g_variant_get(ret, "(&s)", &signed_json);
    assert_signed_event(signed_json, p.legacy_signer.pk_hex, 1, now);
    g_variant_unref(ret);
    g_free(tmpl);
  }

  /* StoreKey writes the unified attribute set and replaces the pre-bml6
   * item for the same identity instead of duplicating it. */
  {
    GError *err = NULL;
    GVariant *ret = call(ctx.bus, "StoreKey",
                         g_variant_new("(ss)", p.pre_bml6.sk_hex, ""), "(bs)", &err);
    if (!ret) { g_printerr("StoreKey(pre-bml6): %s\n", err ? err->message : "?"); exit(1); }
    g_variant_unref(ret);
    check_unified(&p.pre_bml6, "");
  }

  /* StoreKey → GetPublicKey → ClearKey against the real keyring. */
  CHECK(try_store_and_clear_key(&ctx));

  /* Marker: a later pass is a no-op, even with a new legacy item present. */
  {
    TestKey late;
    test_key_new(&late);
    seed(&gnostr_secret_legacy_signer_key_schema, "Nostr Key: Late", late.sk_hex,
         "application", "gnostr-signer", "label", "Late", "npub", late.npub,
         "key_type", "nostr", NULL);
    nostr_nip55l_keyring_migration r;
    CHECK(nostr_nip55l_migrate_legacy_keys(&r) == 0);
    CHECK(r.already_done == 1 && r.found == 0 && r.migrated == 0);
    CHECK(count_items(&gnostr_secret_legacy_signer_key_schema, "npub", late.npub) == 1);
    test_key_free(&late);
  }

  ctx_teardown(&ctx);
  g_free(acl);
  test_key_free(&p.legacy_signer);
  test_key_free(&p.legacy_helper);
  test_key_free(&p.legacy_hardware);
  test_key_free(&p.pre_bml6);
  g_print("PASS phase 3 (real keyring: legacy migration, unified StoreKey, marker)\n");
}
#endif /* NIP55L_TEST_HAVE_LIBSECRET */

/* ---------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(void) {
  /* First fixture: mutations disabled, relays absent. Exercises the read
   * lanes plus GetRelays=NotFound plus StoreKey=PermissionDenied. */
  {
    Ctx ctx;
    ctx_setup(&ctx, /*allow_mutations=*/FALSE, /*write_relays=*/FALSE);
    test_get_public_key(&ctx);
    test_sign_event_ok(&ctx);
    test_sign_event_bad_json(&ctx);
    test_nip44_b64_roundtrip(&ctx);
    test_nip44_derive_conversation_key(&ctx);
    test_get_relays_paths(&ctx, /*expect_ok=*/FALSE);
    test_store_key_denied_without_flag(&ctx);
    ctx_teardown(&ctx);
    g_print("PASS phase 1 (no mutations, no relays.conf)\n");
  }

  /* Second fixture: mutations allowed, relays written. Exercises GetRelays
   * and (best-effort) StoreKey/ClearKey. */
  {
    Ctx ctx;
    ctx_setup(&ctx, /*allow_mutations=*/TRUE, /*write_relays=*/TRUE);
    test_get_relays_paths(&ctx, /*expect_ok=*/TRUE);
    gboolean stored = try_store_and_clear_key(&ctx);
    if (!stored) {
      g_print("PARTIAL phase 2: libsecret unavailable, "
              "StoreKey/ClearKey round-trip skipped\n");
    }
    ctx_teardown(&ctx);
    g_print("PASS phase 2 (mutations, relays.conf%s)\n",
            stored ? ", StoreKey/ClearKey verified" : "");
  }

#ifdef NIP55L_TEST_HAVE_LIBSECRET
  run_phase3();
#else
  g_print("SKIP phase 3: built without libsecret\n");
#endif

  g_print("test_nip55l_dbus_contract: PASS\n");
  return 0;
}
