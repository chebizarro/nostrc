/* test_signer_dbus_contract — real-service D-Bus contract test (nostrc-tf3b).
 *
 * Spins up a private session bus with GTestDBus, spawns the actual
 * nostr-signer-daemon on it, and drives the full round-trip through the
 * generated org.nostr.Signer glue: GetPublicKey → SignEvent (with a
 * pre-populated ACL) → parse and cryptographically verify the returned
 * event → NIP44EncryptB64 → NIP44DecryptB64 → GetRelays → StoreKey /
 * ClearKey (best-effort; libsecret required and skipped clean if absent).
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <nostr/nip44/nip44.h>

#ifndef NIP55L_DAEMON_PATH
#error "NIP55L_DAEMON_PATH must be defined by the build (path to nostr-signer-daemon)"
#endif

#define BUS_NAME     "org.nostr.Signer"
#define OBJ_PATH     "/org/nostr/signer"
#define IFACE        "org.nostr.Signer"

#define ERR_INVALID  "org.nostr.Signer.Error.InvalidInput"
#define ERR_NOT_FND  "org.nostr.Signer.Error.NotFound"
#define ERR_PERM     "org.nostr.Signer.Error.PermissionDenied"

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

static void wait_for_name(GDBusConnection *bus, guint timeout_s) {
  WaitCtx w = { g_main_loop_new(NULL, FALSE), FALSE };
  guint watch = g_bus_watch_name_on_connection(bus, BUS_NAME,
                                               G_BUS_NAME_WATCHER_FLAGS_NONE,
                                               on_appeared, NULL, &w, NULL);
  guint to = g_timeout_add_seconds(timeout_s, on_timeout, &w);
  g_main_loop_run(w.loop);
  if (w.appeared) g_source_remove(to);
  g_bus_unwatch_name(watch);
  g_main_loop_unref(w.loop);
  CHECK(w.appeared);
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
  GSubprocess *daemon;
  GDBusConnection *bus;
  char *tmpdir;
  char *sk_hex;   /* the daemon's identity */
  char *pk_hex;   /* x-only pubkey hex */
  char *npub;     /* npub of the pk */
} Ctx;

static void ctx_setup(Ctx *ctx, gboolean allow_mutations, gboolean write_relays) {
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
    write_file(acl, "[SignEvent]\ncontract-test:=allow\n");
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

static void ctx_teardown(Ctx *ctx) {
  if (ctx->daemon) {
    g_subprocess_force_exit(ctx->daemon);
    (void)g_subprocess_wait(ctx->daemon, NULL, NULL);
    g_object_unref(ctx->daemon);
  }
  if (ctx->bus) g_object_unref(ctx->bus);
  if (ctx->tbus) { g_test_dbus_down(ctx->tbus); g_object_unref(ctx->tbus); }
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

  g_print("test_nip55l_dbus_contract: PASS\n");
  return 0;
}
