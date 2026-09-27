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
 * the per-keyring marker must stop a second migration pass. The keyring
 * starts with the v1 marker already present (a bml6 pass completed) plus
 * gnostr-client org.gnostr.NostrKey items (nostrc-e5nz): the v2 pass must
 * still import them, keep a label the signer already had for the same key,
 * and leave another application's item under that schema name alone.
 * Also asserts the daemon's error path shape: malformed input →
 * Error.InvalidInput; GetRelays on a fresh install → Error.NotFound;
 * mutations without the escape hatch → Error.PermissionDenied.
 *
 * Access control (nip55l 0.4.0; nostrc-y02q/phk4/1e31/eie5/f7hk): grants are
 * keyed on the principal the daemon derives from this process's bus
 * connection (computed here with the same signer_caller helpers and
 * cross-checked against ApprovalRequested), never on app_id. The gating
 * phase proves: decrypt/pubkey calls without a grant are parked and raise
 * ApprovalRequested; a spoofed web-origin app_id from a non-bridge process
 * does not inherit that origin's grant; remember → no second prompt, for the
 * empty selector and the npub selector alike; remembered decisions are per
 * kind; queued identical calls share one prompt; an untrusted process cannot
 * answer approvals; with no approval agent on the bus a prompt fails fast.
 * The bridge phase runs this process as the trusted browser bridge
 * (test-build NOSTR_SIGNER_TEST_ORIGIN_BRIDGES) so origin grants apply.
 * The NIP-5F phase (nostrc-q23h) starts the daemon's opt-in socket and
 * proves every socket method goes through the same gate: the principal is
 * this process's (kernel peer credentials; on Linux identical to its D-Bus
 * principal, so grants are shared), an ungranted call raises
 * ApprovalRequested on the bus, remember/deny/grant-file entries apply,
 * the legacy signer-acl.ini and a claimed app_id do not, and a client that
 * hangs up while waiting drops its request.
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
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
#ifdef __APPLE__
#include <libproc.h>
#endif

#include "signer_caller.h"

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
#define APPROVER_NAME "org.gnostr.Signer"

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

/* This process's executable as the daemon will see it (/proc/<pid>/exe or
 * proc_pidpath), and the principal the daemon derives for this connection. */
static char *self_exe(void) {
#if defined(__linux__)
  char *exe = g_file_read_link("/proc/self/exe", NULL);
  CHECK(exe != NULL);
  return exe;
#elif defined(__APPLE__)
  char buf[PROC_PIDPATHINFO_MAXSIZE];
  CHECK(proc_pidpath(getpid(), buf, sizeof buf) > 0);
  return g_strdup(buf);
#else
  CHECK(!"unsupported platform");
  return NULL;
#endif
}

static char *self_principal(void) {
  char *exe = self_exe();
  SignerCallerKind kind = SIGNER_CALLER_EXE;
  char *app = NULL;
#ifdef __linux__
  char *cg = NULL;
  if (g_file_get_contents("/proc/self/cgroup", &cg, NULL, NULL)) {
    SignerCallerKind k = SIGNER_CALLER_UNKNOWN;
    app = signer_caller_parse_cgroup(cg, &k);
    if (app) kind = k;
    g_free(cg);
  }
#endif
  char *p = signer_caller_build_principal(kind, app, exe);
  CHECK(p != NULL);
  g_free(app); g_free(exe);
  return p;
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
  char *principal; /* how the daemon identifies this process (app_id "") */
  char *grants_path;
  gboolean attested; /* the bus reports our PID (Linux); FALSE on macOS */
} Ctx;

/* Principal the daemon assigns to a call from this process naming @app. */
static char *pr_buf[16];
static guint pr_next;
static const char *pr(const Ctx *ctx, const char *app) {
  if (ctx->attested) return ctx->principal;
  g_free(pr_buf[pr_next]);
  pr_buf[pr_next] = g_strconcat("claimed:", app, NULL);
  const char *r = pr_buf[pr_next];
  pr_next = (pr_next + 1) % G_N_ELEMENTS(pr_buf);
  return r;
}

/* Trust knobs for one fixture (test-build daemon env overrides). */
typedef struct {
  gboolean approver;     /* this process may answer ApproveRequest */
  gboolean bridge;       /* this process may assert web origins */
  gboolean own_ui_name;  /* own org.gnostr.Signer (an approval agent is present) */
} Trust;
static const Trust TRUST_UI = { TRUE, FALSE, TRUE };

/* pre_daemon runs on the private bus before the daemon is spawned (phase 3
 * starts a keyring and seeds legacy items there). extra_acl is appended to
 * the [SignEvent] section. */
typedef void (*PreDaemonFn)(Ctx *ctx, gpointer data);

/* Grants body: "@N" is replaced by the fixture identity's npub and "@P" by
 * this process's principal; without PID attestation (macOS) a line with @P
 * is emitted once per claimed app_id the phases use. */
static char *expand_line(const Ctx *ctx, const char *line, const char *principal) {
  GString *o = g_string_new(NULL);
  for (const char *p = line; *p; p++) {
    if (p[0] == '@' && p[1] == 'P') { g_string_append(o, principal); p++; }
    else if (p[0] == '@' && p[1] == 'N') { g_string_append(o, ctx->npub); p++; }
    else g_string_append_c(o, *p);
  }
  return g_string_free(o, FALSE);
}

static char *expand_grants(const Ctx *ctx, const char *tmpl) {
  static const char *claims[] = { "claimed:", "claimed:contract-test" };
  GString *o = g_string_new(NULL);
  gchar **lines = g_strsplit(tmpl, "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    if (!*lines[i]) continue;
    guint n = (strstr(lines[i], "@P") && !ctx->attested) ? G_N_ELEMENTS(claims) : 1;
    for (guint j = 0; j < n; j++) {
      char *l = expand_line(ctx, lines[i], ctx->attested ? ctx->principal : claims[j]);
      g_string_append_printf(o, "%s\n", l);
      g_free(l);
    }
  }
  g_strfreev(lines);
  return g_string_free(o, FALSE);
}

static void ctx_setup_full(Ctx *ctx, gboolean allow_mutations, gboolean write_relays,
                           const char *grants, const Trust *trust,
                           PreDaemonFn pre_daemon, gpointer data) {
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

  char *gnostr_dir = g_build_filename(xdg_cfg, "gnostr", NULL);
  g_mkdir_with_parents(gnostr_dir, 0700);

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

  /* Does the bus attest our PID? (Linux dbus-daemon: yes; macOS: no.) */
  ctx->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  CHECK(ctx->bus != NULL);
  {
    GVariant *r = g_dbus_connection_call_sync(ctx->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetConnectionUnixProcessID",
        g_variant_new("(s)", g_dbus_connection_get_unique_name(ctx->bus)),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
    ctx->attested = r != NULL;
    if (r) g_variant_unref(r);
  }
#ifdef __linux__
  CHECK(ctx->attested);
#endif
  ctx->principal = ctx->attested ? self_principal() : g_strdup("claimed:");

  /* Grants ($XDG_CONFIG_HOME/gnostr/signer-grants.ini): section = request
   * kind, key "<principal>|<npub or *>". */
  {
    ctx->grants_path = g_build_filename(gnostr_dir, "signer-grants.ini", NULL);
    if (grants) {
      char *body = expand_grants(ctx, grants);
      write_file(ctx->grants_path, body);
      g_free(body);
    }
    /* The pre-0.4.0 ACL, keyed on claimed app_ids: must be ignored. */
    char *legacy = g_build_filename(gnostr_dir, "signer-acl.ini", NULL);
    write_file(legacy, "[SignEvent]\ncontract-legacy:=allow\n"
                       "[NIP04Decrypt]\ncontract-legacy:=allow\n");
    g_free(legacy);
    g_free(gnostr_dir);
  }

#ifdef __APPLE__
  /* The daemon's startup key migration would search (and could write) the
   * user's login keychain; keep it off in tests (test-build switch). */
  g_setenv("NOSTR_SIGNER_TEST_NO_MIGRATION", "1", TRUE);
#endif
  /* Test-build trust overrides read by the daemon at call time. */
  char *exe = self_exe();
  g_setenv("NOSTR_SIGNER_TEST_APPROVERS", trust && trust->approver ? exe : "/nonexistent", TRUE);
  g_setenv("NOSTR_SIGNER_TEST_ORIGIN_BRIDGES", trust && trust->bridge ? exe : "/nonexistent", TRUE);
  g_free(exe);

  if (pre_daemon) pre_daemon(ctx, data);

  GError *err = NULL;
  /* NIP55L_TEST_DAEMON_LOG=1 keeps the daemon's stderr (debugging). */
  ctx->daemon = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                 (g_getenv("NIP55L_TEST_DAEMON_LOG") ? 0 : G_SUBPROCESS_FLAGS_STDERR_SILENCE),
                                 &err, NIP55L_DAEMON_PATH, NULL);
  if (!ctx->daemon) {
    g_printerr("spawn daemon (%s): %s\n", NIP55L_DAEMON_PATH,
               err ? err->message : "?");
    exit(1);
  }
  g_clear_error(&err);

  wait_for_name(ctx->bus, 20);
  if (trust && trust->own_ui_name) {
    GVariant *r = g_dbus_connection_call_sync(ctx->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", APPROVER_NAME, 4u),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
    CHECK(r != NULL);
    g_variant_unref(r);
  }

  g_free(xdg_cfg); g_free(xdg_data); g_free(xdg_runtime);
}

/* Phases 1-2 run with full grants for this process: the lanes themselves
 * are under test there, the gate is exercised in the gating phase. */
#define ALL_KINDS_GRANT \
  "[event]\n@P|*=allow\n[nip44_conversation_key]\n@P|@N=allow\n" \
  "[get_public_key]\n@P|*=allow\n[get_relays]\n@P|*=allow\n" \
  "[nip44_encrypt]\n@P|@N=allow\n[nip44_decrypt]\n@P|@N=allow\n"

static void ctx_setup(Ctx *ctx, gboolean allow_mutations, gboolean write_relays) {
  ctx_setup_full(ctx, allow_mutations, write_relays, ALL_KINDS_GRANT, &TRUST_UI, NULL, NULL);
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
  g_free(ctx->principal);
  g_free(ctx->grants_path);
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
  if (!ret) { g_printerr("GetPublicKey (principal %s): %s\n", ctx->principal, err ? err->message : "?"); exit(1); }
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

/* ---- approval round-trips ------------------------------------------------ */

typedef struct {
  GMainLoop *loop;
  guint n_requests;            /* ApprovalRequested seen */
  char *req_id, *kind, *preview, *app_id, *identity;
  guint pending_replies;       /* async calls not yet answered */
  GPtrArray *replies;          /* GVariant* or NULL, in completion order */
  GPtrArray *errors;           /* GError* or NULL */
} ApprCtx;

static void on_approval_requested(GDBusConnection *c, const gchar *snd, const gchar *path,
                                  const gchar *iface, const gchar *sig, GVariant *params,
                                  gpointer ud) {
  (void)c; (void)snd; (void)path; (void)iface; (void)sig;
  ApprCtx *a = ud;
  const char *app = NULL, *id = NULL, *kind = NULL, *prev = NULL, *rid = NULL;
  g_variant_get(params, "(&s&s&s&s&s)", &app, &id, &kind, &prev, &rid);
  a->n_requests++;
  g_free(a->req_id); a->req_id = g_strdup(rid);
  g_free(a->kind); a->kind = g_strdup(kind);
  g_free(a->preview); a->preview = g_strdup(prev);
  g_free(a->app_id); a->app_id = g_strdup(app);
  g_free(a->identity); a->identity = g_strdup(id);
  g_main_loop_quit(a->loop);
}

static void on_async_reply(GObject *src, GAsyncResult *res, gpointer ud) {
  ApprCtx *a = ud;
  GError *err = NULL;
  GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
  g_ptr_array_add(a->replies, r);
  g_ptr_array_add(a->errors, err);
  a->pending_replies--;
  g_main_loop_quit(a->loop);
}

static gboolean appr_timeout(gpointer ud) {
  (void)ud;
  g_printerr("FAIL: approval round-trip timed out\n");
  exit(1);
  return G_SOURCE_REMOVE;
}

typedef struct { ApprCtx a; guint sub; guint to; } Watch;

static void watch_start(Ctx *ctx, Watch *w) {
  memset(w, 0, sizeof *w);
  w->a.loop = g_main_loop_new(NULL, FALSE);
  w->a.replies = g_ptr_array_new();
  w->a.errors = g_ptr_array_new();
  w->sub = g_dbus_connection_signal_subscribe(ctx->bus, BUS_NAME, IFACE, "ApprovalRequested",
                                              OBJ_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                              on_approval_requested, &w->a, NULL);
  w->to = g_timeout_add_seconds(20, appr_timeout, &w->a);
}

static void watch_drain(void) {
  /* Deliver signals already received (they precede any reply on the wire). */
  while (g_main_context_iteration(NULL, FALSE)) {}
}

static void watch_stop(Ctx *ctx, Watch *w) {
  watch_drain();
  g_source_remove(w->to);
  g_dbus_connection_signal_unsubscribe(ctx->bus, w->sub);
  for (guint i = 0; i < w->a.replies->len; i++)
    if (w->a.replies->pdata[i]) g_variant_unref(w->a.replies->pdata[i]);
  for (guint i = 0; i < w->a.errors->len; i++)
    if (w->a.errors->pdata[i]) g_error_free(w->a.errors->pdata[i]);
  g_ptr_array_unref(w->a.replies);
  g_ptr_array_unref(w->a.errors);
  g_free(w->a.req_id); g_free(w->a.kind); g_free(w->a.preview);
  g_free(w->a.app_id); g_free(w->a.identity);
  g_main_loop_unref(w->a.loop);
}

static void watch_call(Ctx *ctx, Watch *w, const char *method, GVariant *args) {
  w->a.pending_replies++;
  g_dbus_connection_call(ctx->bus, BUS_NAME, OBJ_PATH, IFACE, method, args,
                         G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 30000, NULL,
                         on_async_reply, &w->a);
}

/* Wait until the n-th ApprovalRequested (or an unexpected reply). */
static void watch_wait_request(Watch *w, guint n) {
  guint replies = w->a.replies->len;
  while (w->a.n_requests < n && w->a.replies->len == replies) g_main_loop_run(w->a.loop);
  if (w->a.n_requests < n) {
    GError *e = w->a.errors->pdata[w->a.errors->len - 1];
    g_printerr("FAIL: call answered without a prompt (%s)\n", e ? e->message : "success");
    exit(1);
  }
}

static void watch_wait_replies(Watch *w) {
  while (w->a.pending_replies > 0) g_main_loop_run(w->a.loop);
}

static GVariant *approve_call(Ctx *ctx, const char *req_id, gboolean decision, gboolean remember,
                              GError **err) {
  return call(ctx->bus, "ApproveRequest",
              g_variant_new("(sbbt)", req_id, decision, remember, (guint64)0), "(b)", err);
}

static void approve(Ctx *ctx, const char *req_id, gboolean decision, gboolean remember) {
  GError *err = NULL;
  GVariant *ok = approve_call(ctx, req_id, decision, remember, &err);
  if (!ok) { g_printerr("ApproveRequest: %s\n", err ? err->message : "?"); exit(1); }
  g_variant_unref(ok);
}

/* One gated call that must be parked: checks the ApprovalRequested shape,
 * answers it, and returns the reply (caller unrefs) or sets *err_out. */
static GVariant *interactive(Ctx *ctx, const char *method, GVariant *args, const char *want_kind,
                             const char *want_app_id, gboolean decision, gboolean remember,
                             char **preview_out, GError **err_out) {
  g_usleep(120 * 1000); /* the daemon allows one new prompt per 100 ms per sender */
  Watch w;
  watch_start(ctx, &w);
  watch_call(ctx, &w, method, args);
  watch_wait_request(&w, 1);
  if (g_strcmp0(w.a.kind, want_kind) != 0)
    g_printerr("kind: want %s got %s\n", want_kind, w.a.kind);
  CHECK(g_strcmp0(w.a.kind, want_kind) == 0);
  if (g_strcmp0(w.a.app_id, want_app_id) != 0)
    g_printerr("app_id: want %s got %s\n", want_app_id, w.a.app_id);
  CHECK(g_strcmp0(w.a.app_id, want_app_id) == 0);
  if (g_strcmp0(want_kind, "get_relays") != 0) CHECK(g_strcmp0(w.a.identity, ctx->npub) == 0);
  if (preview_out) *preview_out = g_strdup(w.a.preview);
  approve(ctx, w.a.req_id, decision, remember);
  watch_wait_replies(&w);
  GVariant *r = w.a.replies->pdata[0];
  if (r) g_variant_ref(r);
  if (err_out && w.a.errors->pdata[0]) *err_out = g_error_copy(w.a.errors->pdata[0]);
  watch_stop(ctx, &w);
  return r;
}

/* A gated call that must be answered from the grants alone. */
static GVariant *no_prompt(Ctx *ctx, const char *method, GVariant *args, GError **err) {
  Watch w;
  watch_start(ctx, &w);
  GVariant *r = call(ctx->bus, method, args, "(s)", err);
  watch_drain();
  if (w.a.n_requests) g_printerr("unexpected ApprovalRequested (%s) for %s\n", w.a.kind, method);
  CHECK(w.a.n_requests == 0);
  watch_stop(ctx, &w);
  return r;
}

static void test_nip44_derive_conversation_key(Ctx *ctx) {
  char *peer_sk = nostr_key_generate_private();
  char *peer_pk = nostr_key_get_public(peer_sk);
  CHECK(peer_sk && peer_pk);
  char *want = expected_convkey_hex(peer_sk, ctx->pk_hex);

  /* Granted: answered immediately, equal to the NIP-44 derivation on the
   * peer's side, and usable with the stock NIP-44 v2 cipher. */
  GError *err = NULL;
  GVariant *ret = no_prompt(ctx, "NIP44DeriveConversationKey",
                            g_variant_new("(sss)", peer_pk, "", "contract-test"), &err);
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

  g_free(want);
  free(peer_sk);
  free(peer_pk);
}

/* Incoming NIP-44 / NIP-04 payloads addressed to the daemon's identity. */
static char *incoming_nip44(Ctx *ctx, const char *peer_sk, const char *msg) {
  uint8_t sk[32], pk[32];
  CHECK(nostr_hex2bin(sk, peer_sk, 32));
  CHECK(nostr_hex2bin(pk, ctx->pk_hex, 32));
  char *out = NULL;
  CHECK(nostr_nip44_encrypt_v2(sk, pk, (const uint8_t *)msg, strlen(msg), &out) == 0 && out);
  return out;
}

static int grants_has(Ctx *ctx, const char *section, const char *key, const char *want_value) {
  GKeyFile *kf = g_key_file_new();
  int found = 0;
  if (g_key_file_load_from_file(kf, ctx->grants_path, G_KEY_FILE_NONE, NULL)) {
    char *v = g_key_file_get_string(kf, section, key, NULL);
    found = v && (!want_value || g_str_has_prefix(v, want_value));
    g_free(v);
  }
  g_key_file_unref(kf);
  return found;
}

/* nostrc-y02q / phk4 / 1e31 / eie5 / f7hk. Fixture: no grants except a
 * web-origin one this process must NOT inherit, approvals answered here. */
static void test_gating(Ctx *ctx) {
  char *peer_sk = nostr_key_generate_private();
  char *peer_pk = nostr_key_get_public(peer_sk);
  CHECK(peer_sk && peer_pk);
  GError *err = NULL;
  char *preview = NULL;

  /* y02q: decrypt is parked until approved; the prompt names the verified
   * principal (not the claimed app_id) and the resolved npub. */
  char *ct = incoming_nip44(ctx, peer_sk, "dm one");
  GVariant *r = interactive(ctx, "NIP44Decrypt", g_variant_new("(sss)", ct, peer_pk, ""),
                            "nip44_decrypt", ctx->principal, TRUE, FALSE, &preview, &err);
  if (!r) { g_printerr("NIP44Decrypt after approve: %s\n", err ? err->message : "?"); exit(1); }
  const char *pt = NULL;
  g_variant_get(r, "(&s)", &pt);
  CHECK(g_strcmp0(pt, "dm one") == 0);
  CHECK(preview && strstr(preview, peer_pk) != NULL);
  g_variant_unref(r); g_free(preview); preview = NULL;
  g_free(ct);
  g_usleep(150 * 1000);

  /* Allow-once is not remembered: the next decrypt prompts again; deny. */
  ct = incoming_nip44(ctx, peer_sk, "dm two");
  r = interactive(ctx, "NIP44Decrypt", g_variant_new("(sss)", ct, peer_pk, ""),
                  "nip44_decrypt", ctx->principal, FALSE, FALSE, NULL, &err);
  CHECK(r == NULL);
  expect_remote_error(err, ERR_DENIED);
  g_clear_error(&err);
  g_usleep(150 * 1000);

  /* NIP04Decrypt, NIP44DecryptB64, GetPublicKey and GetRelays are gated too. */
  r = interactive(ctx, "NIP04Decrypt", g_variant_new("(sss)", "bm90LWEtcmVhbC1jaXBoZXI=?iv=AAAAAAAAAAAAAAAAAAAAAA==", peer_pk, ""),
                  "nip04_decrypt", ctx->principal, FALSE, FALSE, NULL, &err);
  CHECK(r == NULL); expect_remote_error(err, ERR_DENIED); g_clear_error(&err);
  g_usleep(150 * 1000);
  r = interactive(ctx, "NIP44DecryptB64", g_variant_new("(sss)", ct, peer_pk, ""),
                  "nip44_decrypt", ctx->principal, FALSE, FALSE, NULL, &err);
  CHECK(r == NULL); expect_remote_error(err, ERR_DENIED); g_clear_error(&err);
  g_usleep(150 * 1000);
  r = interactive(ctx, "GetPublicKey", NULL, "get_public_key", ctx->principal, TRUE, FALSE, NULL, &err);
  CHECK(r != NULL);
  const char *np = NULL; g_variant_get(r, "(&s)", &np);
  CHECK(g_strcmp0(np, ctx->npub) == 0);
  g_variant_unref(r);
  g_usleep(150 * 1000);
  r = interactive(ctx, "GetRelays", NULL, "get_relays", ctx->principal, FALSE, FALSE, NULL, &err);
  CHECK(r == NULL); expect_remote_error(err, ERR_DENIED); g_clear_error(&err);
  g_usleep(150 * 1000);
  g_free(ct);

  /* The pre-0.4.0 ACL entries ("contract-legacy:") are not honoured. */
  r = interactive(ctx, "SignEvent",
                  g_variant_new("(sss)", "{\"kind\":1,\"created_at\":0,\"tags\":[],\"content\":\"legacy\"}",
                                "", "contract-legacy"),
                  "event", pr(ctx, "contract-legacy"), FALSE, FALSE, NULL, &err);
  CHECK(r == NULL); expect_remote_error(err, ERR_DENIED); g_clear_error(&err);
  g_usleep(150 * 1000);

  /* phk4: claiming a web origin does not inherit its grant, and the
   * approver sees both the verified principal and the claimed id. Needs a
   * bus that attests PIDs. */
  if (!ctx->attested) {
    g_print("SKIP spoofed-app_id check: the bus does not attest caller PIDs here\n");
  } else {
    g_usleep(120 * 1000);
    Watch w;
    watch_start(ctx, &w);
    watch_call(ctx, &w, "SignEvent",
               g_variant_new("(sss)", "{\"kind\":1,\"created_at\":0,\"tags\":[],\"content\":\"spoof\"}",
                             "", "https://allowed.example"));
    watch_wait_request(&w, 1);
    CHECK(g_strcmp0(w.a.app_id, ctx->principal) == 0);
    GVariant *info = call(ctx->bus, "GetApprovalInfo", g_variant_new("(s)", w.a.req_id), "(a{sv})", &err);
    if (!info) { g_printerr("GetApprovalInfo: %s\n", err ? err->message : "?"); exit(1); }
    GVariant *d = g_variant_get_child_value(info, 0);
    const char *v = NULL;
    CHECK(g_variant_lookup(d, "claimed_app_id", "&s", &v) && g_strcmp0(v, "https://allowed.example") == 0);
    CHECK(g_variant_lookup(d, "principal", "&s", &v) && g_strcmp0(v, ctx->principal) == 0);
    CHECK(g_variant_lookup(d, "principal_kind", "&s", &v) &&
          (g_strcmp0(v, "executable") == 0 || g_strcmp0(v, "systemd-scope") == 0));
    CHECK(g_variant_lookup(d, "kind", "&s", &v) && g_strcmp0(v, "event") == 0);
    gboolean rememberable = FALSE;
    CHECK(g_variant_lookup(d, "rememberable", "b", &rememberable) && rememberable);
    g_variant_unref(d); g_variant_unref(info);
    approve(ctx, w.a.req_id, FALSE, FALSE);
    watch_wait_replies(&w);
    CHECK(w.a.replies->pdata[0] == NULL);
    expect_remote_error(w.a.errors->pdata[0], ERR_DENIED);
    watch_stop(ctx, &w);
  }
  g_usleep(150 * 1000);

  /* eie5: remember with the empty selector (what gnostr and the browser
   * bridge send) → the grant is stored under the resolved npub → the next
   * SignEvent, with "" or with the npub as selector, is not prompted. */
  int64_t now = (int64_t)time(NULL);
  r = interactive(ctx, "SignEvent",
                  g_variant_new("(sss)", "{\"kind\":1,\"created_at\":0,\"tags\":[],\"content\":\"remember me\"}",
                                "", "contract-test"),
                  "event", pr(ctx, "contract-test"), TRUE, TRUE, NULL, &err);
  CHECK(r != NULL);
  const char *signed_json = NULL;
  g_variant_get(r, "(&s)", &signed_json);
  assert_signed_event(signed_json, ctx->pk_hex, 1, now);
  g_variant_unref(r);
  {
    char *key = g_strdup_printf("%s|%s", pr(ctx, "contract-test"), ctx->npub);
    CHECK(grants_has(ctx, "event", key, "allow"));
    g_free(key);
  }
  /* Three selector shapes for one key: empty (active), its npub and its
   * hex public key (nostrc-a4w5 maps both onto the active identity). */
  const char *selectors[] = { "", ctx->npub, ctx->pk_hex };
  for (size_t i = 0; i < G_N_ELEMENTS(selectors); i++) {
    r = no_prompt(ctx, "SignEvent",
                  g_variant_new("(sss)", "{\"kind\":1,\"created_at\":0,\"tags\":[],\"content\":\"again\"}",
                                selectors[i], ctx->attested ? "whatever-app" : "contract-test"), &err);
    if (!r) { g_printerr("remembered SignEvent (selector #%zu): %s\n", i, err ? err->message : "?"); exit(1); }
    g_variant_get(r, "(&s)", &signed_json);
    assert_signed_event(signed_json, ctx->pk_hex, 1, now);
    g_variant_unref(r);
  }

  /* f7hk: the remembered "event" allow does not cover decryption; a
   * remembered deny for nip44_decrypt then refuses without prompting while
   * SignEvent stays allowed. */
  ct = incoming_nip44(ctx, peer_sk, "dm three");
  r = interactive(ctx, "NIP44Decrypt", g_variant_new("(sss)", ct, peer_pk, ""),
                  "nip44_decrypt", ctx->principal, FALSE, TRUE, NULL, &err);
  CHECK(r == NULL); expect_remote_error(err, ERR_DENIED); g_clear_error(&err);
  r = no_prompt(ctx, "NIP44Decrypt", g_variant_new("(sss)", ct, peer_pk, ""), &err);
  CHECK(r == NULL); expect_remote_error(err, ERR_DENIED); g_clear_error(&err);
  r = no_prompt(ctx, "SignEvent",
                g_variant_new("(sss)", "{\"kind\":1,\"created_at\":0,\"tags\":[],\"content\":\"still ok\"}", "",
                              "contract-test"),
                &err);
  CHECK(r != NULL); g_variant_unref(r);
  g_free(ct);
  g_usleep(150 * 1000);

  /* Convkey: remembered deny → denied by policy without a prompt. */
  r = interactive(ctx, "NIP44DeriveConversationKey", g_variant_new("(sss)", peer_pk, "", "contract-ask"),
                  "nip44_conversation_key", pr(ctx, "contract-ask"), FALSE, TRUE, &preview, &err);
  CHECK(r == NULL); expect_remote_error(err, ERR_DENIED); g_clear_error(&err);
  CHECK(preview && strstr(preview, peer_pk) != NULL);
  g_free(preview); preview = NULL;
  r = no_prompt(ctx, "NIP44DeriveConversationKey", g_variant_new("(sss)", peer_pk, "", "contract-ask"), &err);
  CHECK(r == NULL); expect_remote_error(err, ERR_DENIED); g_clear_error(&err);
  g_usleep(150 * 1000);

  /* Queued identical calls share one prompt; one approval answers all. */
  {
    g_usleep(120 * 1000);
    Watch w;
    watch_start(ctx, &w);
    char *cts[3];
    for (int i = 0; i < 3; i++) {
      char msg[16]; g_snprintf(msg, sizeof msg, "burst %d", i);
      cts[i] = incoming_nip44(ctx, peer_sk, msg);
    }
    /* A fresh kind (encrypt) so the remembered decrypt deny does not apply. */
    for (int i = 0; i < 3; i++)
      watch_call(ctx, &w, "NIP44Encrypt", g_variant_new("(sss)", cts[i], peer_pk, ""));
    watch_wait_request(&w, 1);
    CHECK(g_strcmp0(w.a.kind, "nip44_encrypt") == 0);
    /* Let the other two reach the daemon, then answer once. */
    for (int i = 0; i < 20; i++) { watch_drain(); g_usleep(10 * 1000); }
    GVariant *info = call(ctx->bus, "GetApprovalInfo", g_variant_new("(s)", w.a.req_id), "(a{sv})", &err);
    CHECK(info != NULL);
    GVariant *d = g_variant_get_child_value(info, 0);
    guint32 ncalls = 0;
    CHECK(g_variant_lookup(d, "calls", "u", &ncalls) && ncalls == 3);
    g_variant_unref(d); g_variant_unref(info);
    approve(ctx, w.a.req_id, TRUE, FALSE);
    watch_wait_replies(&w);
    CHECK(w.a.n_requests == 1);
    CHECK(w.a.replies->len == 3);
    for (guint i = 0; i < 3; i++) CHECK(w.a.replies->pdata[i] != NULL);
    watch_stop(ctx, &w);
    for (int i = 0; i < 3; i++) free(cts[i]);
  }
  g_usleep(150 * 1000);

  /* One application cannot park more than 8 requests (the table is shared
   * with every other app); the 9th is refused, the 8 still answerable.
   * Forget the grants remembered above first (the daemon reloads the file
   * when it changes). */
  {
    write_file(ctx->grants_path, "");
    Watch w;
    watch_start(ctx, &w);
    for (guint i = 0; i < 9; i++) {
      g_usleep(120 * 1000);
      gchar *ev = g_strdup_printf("{\"kind\":1,\"created_at\":0,\"tags\":[],\"content\":\"cap %u\"}", i);
      watch_call(ctx, &w, "SignEvent", g_variant_new("(sss)", ev, "", "contract-cap"));
      g_free(ev);
    }
    while (w.a.n_requests < 8 || w.a.replies->len < 1) g_main_loop_run(w.a.loop);
    CHECK(w.a.n_requests == 8);
    CHECK(w.a.replies->len == 1 && w.a.replies->pdata[0] == NULL);
    expect_remote_error(w.a.errors->pdata[0], "org.nostr.Signer.Error.RateLimited");
    g_free(w.a.req_id); w.a.req_id = NULL;
    /* Deny the parked eight by id: ids are req-N, consecutive. */
    GVariant *info = NULL;
    guint denied = 0;
    for (guint n = 1; n < 200 && denied < 8; n++) {
      gchar *id = g_strdup_printf("req-%u", n);
      info = call(ctx->bus, "GetApprovalInfo", g_variant_new("(s)", id), "(a{sv})", NULL);
      if (info) {
        GVariant *d = g_variant_get_child_value(info, 0);
        const char *claimed = NULL;
        if (g_variant_lookup(d, "claimed_app_id", "&s", &claimed) && g_strcmp0(claimed, "contract-cap") == 0) {
          approve(ctx, id, FALSE, FALSE);
          denied++;
        }
        g_variant_unref(d);
        g_variant_unref(info);
      }
      g_free(id);
    }
    CHECK(denied == 8);
    watch_wait_replies(&w);
    watch_stop(ctx, &w);
  }

  /* Unknown request ids are not "handled". */
  {
    GVariant *ok = approve_call(ctx, "req-does-not-exist", TRUE, FALSE, &err);
    CHECK(ok != NULL);
    gboolean handled = TRUE; g_variant_get(ok, "(b)", &handled);
    CHECK(!handled);
    g_variant_unref(ok);
  }

  /* No approval agent on the bus: a call that needs a prompt fails fast. */
  {
    GVariant *rel = g_dbus_connection_call_sync(ctx->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "ReleaseName", g_variant_new("(s)", APPROVER_NAME),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
    CHECK(rel != NULL); g_variant_unref(rel);
    r = no_prompt(ctx, "NIP04Encrypt", g_variant_new("(sss)", "hi", peer_pk, ""), &err);
    CHECK(r == NULL); expect_remote_error(err, ERR_DENIED); g_clear_error(&err);
  }

  free(peer_sk);
  free(peer_pk);
}

/* An untrusted process cannot answer (or inspect) approval requests. */
static void test_untrusted_approver(Ctx *ctx) {
  if (!ctx->attested) {
    g_print("SKIP untrusted-approver check: the bus does not attest caller PIDs here\n");
    return;
  }
  char *peer_sk = nostr_key_generate_private();
  char *peer_pk = nostr_key_get_public(peer_sk);
  Watch w;
  watch_start(ctx, &w);
  watch_call(ctx, &w, "NIP04Encrypt", g_variant_new("(sss)", "hi", peer_pk, ""));
  watch_wait_request(&w, 1);
  GError *err = NULL;
  GVariant *ok = approve_call(ctx, w.a.req_id, TRUE, TRUE, &err);
  CHECK(ok == NULL); expect_remote_error(err, ERR_PERM); g_clear_error(&err);
  GVariant *info = call(ctx->bus, "GetApprovalInfo", g_variant_new("(s)", w.a.req_id), "(a{sv})", &err);
  CHECK(info == NULL); expect_remote_error(err, ERR_PERM); g_clear_error(&err);
  CHECK(w.a.pending_replies == 1); /* still parked, not approved */
  CHECK(!g_file_test(ctx->grants_path, G_FILE_TEST_EXISTS));
  /* Leave it parked; the daemon is torn down with the fixture. */
  g_source_remove(w.to);
  g_dbus_connection_signal_unsubscribe(ctx->bus, w.sub);
  free(peer_sk); free(peer_pk);
}

/* This process as the trusted browser bridge: the app_id is the principal
 * when it is a web origin, so origin grants apply; anything else is not. */
static void test_bridge_origins(Ctx *ctx) {
  if (!ctx->attested) {
    g_print("SKIP bridge check: the bus does not attest caller PIDs here\n");
    return;
  }
  const char *tmpl = "{\"kind\":1,\"created_at\":0,\"tags\":[],\"content\":\"from a site\"}";
  GError *err = NULL;
  GVariant *r = no_prompt(ctx, "SignEvent", g_variant_new("(sss)", tmpl, "", "https://allowed.example"), &err);
  if (!r) { g_printerr("bridge allowed origin: %s\n", err ? err->message : "?"); exit(1); }
  g_variant_unref(r);
  r = no_prompt(ctx, "SignEvent", g_variant_new("(sss)", tmpl, "", "https://denied.example"), &err);
  CHECK(r == NULL); expect_remote_error(err, ERR_DENIED); g_clear_error(&err);
  r = no_prompt(ctx, "GetPublicKeyForApp", g_variant_new("(s)", "https://allowed.example"), &err);
  CHECK(r != NULL);
  const char *np = NULL; g_variant_get(r, "(&s)", &np);
  CHECK(g_strcmp0(np, ctx->npub) == 0);
  g_variant_unref(r);

  /* An origin without a grant prompts as itself; the bridge is "via". */
  {
    g_usleep(120 * 1000);
    Watch w;
    watch_start(ctx, &w);
    watch_call(ctx, &w, "SignEvent", g_variant_new("(sss)", tmpl, "", "https://prompt.example"));
    watch_wait_request(&w, 1);
    CHECK(g_strcmp0(w.a.app_id, "https://prompt.example") == 0);
    GVariant *info = call(ctx->bus, "GetApprovalInfo", g_variant_new("(s)", w.a.req_id), "(a{sv})", &err);
    CHECK(info != NULL);
    GVariant *d = g_variant_get_child_value(info, 0);
    const char *v = NULL;
    CHECK(g_variant_lookup(d, "principal_kind", "&s", &v) && g_strcmp0(v, "website") == 0);
    CHECK(g_variant_lookup(d, "via", "&s", &v) && g_strcmp0(v, ctx->principal) == 0);
    g_variant_unref(d); g_variant_unref(info);
    approve(ctx, w.a.req_id, TRUE, TRUE);
    watch_wait_replies(&w);
    CHECK(w.a.replies->pdata[0] != NULL);
    watch_stop(ctx, &w);
  }
  char *key = g_strdup_printf("https://prompt.example|%s", ctx->npub);
  CHECK(grants_has(ctx, "event", key, "allow"));
  g_free(key);
  r = no_prompt(ctx, "SignEvent", g_variant_new("(sss)", tmpl, "", "https://prompt.example"), &err);
  CHECK(r != NULL); g_variant_unref(r);
  g_usleep(150 * 1000);

  /* Not an origin (or not normalized): the bridge's own principal. */
  const char *not_origins[] = { "contract-test", "https://Allowed.example", "https://allowed.example/path",
                                "http://allowed.example" };
  for (size_t i = 0; i < G_N_ELEMENTS(not_origins); i++) {
    r = interactive(ctx, "SignEvent", g_variant_new("(sss)", tmpl, "", not_origins[i]),
                    "event", ctx->principal, FALSE, FALSE, NULL, &err);
    CHECK(r == NULL); expect_remote_error(err, ERR_DENIED); g_clear_error(&err);
    g_usleep(150 * 1000);
  }
}

/* nostrc-a4w5: a caller's identity selector is never key material. A bare
 * 64-hex is a public key - it must name a known identity - and an nsec is
 * refused; before, any 64-hex was used as the private key, so a pubkey
 * passed by mistake signed with a key derived from the pubkey bytes. */
/* ---------------------------------------------------------------------------
 * NIP-5F socket (nostrc-q23h): same gate as D-Bus
 * ------------------------------------------------------------------------- */

typedef struct { char *dir; char *path; } Nip5fSock;

static void nip5f_pre_daemon(Ctx *ctx, gpointer data) {
  (void)ctx;
  Nip5fSock *s = data;
  /* Short path: sun_path is 104 bytes on macOS, and $TMPDIR is long there. */
  s->dir = g_strdup("/tmp/n5fXXXXXX");
  CHECK(mkdtemp(s->dir) != NULL);
  s->path = g_build_filename(s->dir, "s.sock", NULL);
  char *ep = g_strconcat("unix:", s->path, NULL);
  g_setenv("NOSTR_SIGNER_ENDPOINT", ep, TRUE);
  g_free(ep);
}

static int n5f_write(int fd, const char *json) {
  guint32 n = (guint32)strlen(json);
  unsigned char hdr[4] = { (unsigned char)(n >> 24), (unsigned char)(n >> 16),
                           (unsigned char)(n >> 8), (unsigned char)n };
  if (send(fd, hdr, 4, 0) != 4) return -1;
  return send(fd, json, n, 0) == (ssize_t)n ? 0 : -1;
}

static int read_all(int fd, void *buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = recv(fd, (char *)buf + got, n - got, 0);
    if (r <= 0) return -1;
    got += (size_t)r;
  }
  return 0;
}

static char *n5f_read(int fd) {
  unsigned char hdr[4];
  if (read_all(fd, hdr, 4) != 0) return NULL;
  guint32 n = ((guint32)hdr[0] << 24) | ((guint32)hdr[1] << 16) | ((guint32)hdr[2] << 8) | hdr[3];
  CHECK(n > 0 && n < (1u << 20));
  char *buf = g_malloc0(n + 1);
  if (read_all(fd, buf, n) != 0) { g_free(buf); return NULL; }
  return buf;
}

static int n5f_connect(const Nip5fSock *s) {
  int fd = -1;
  for (int i = 0; i < 100 && fd < 0; i++) { /* the daemon listens once exported */
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    g_strlcpy(a.sun_path, s->path, sizeof a.sun_path);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
      close(fd); fd = -1;
      g_usleep(50 * 1000);
    }
  }
  CHECK(fd >= 0);
  char *banner = n5f_read(fd);
  CHECK(banner && strstr(banner, "nostr-signer"));
  g_free(banner);
  CHECK(n5f_write(fd, "{\"client\":\"contract-test\"}") == 0);
  return fd;
}

/* One request on a worker thread (it may wait for the user). */
typedef struct { int fd; char *req; char *resp; } N5fCall;

static gpointer n5f_call_thread(gpointer data) {
  N5fCall *c = data;
  if (n5f_write(c->fd, c->req) == 0) c->resp = n5f_read(c->fd);
  return NULL;
}

static GThread *n5f_call_start(N5fCall *c, int fd, const char *req) {
  c->fd = fd; c->req = g_strdup(req); c->resp = NULL;
  return g_thread_new("n5f-call", n5f_call_thread, c);
}

static char *n5f_call_finish(N5fCall *c, GThread *t) {
  g_thread_join(t);
  g_free(c->req);
  CHECK(c->resp != NULL);
  return c->resp;
}

/* A socket request that must raise ApprovalRequested for @principal; answered
 * with @decision/@remember. Returns the response frame. */
static char *n5f_interactive(Ctx *ctx, int fd, const char *req, const char *want_kind,
                             const char *principal, gboolean decision, gboolean remember) {
  g_usleep(120 * 1000); /* one new prompt per 100 ms per connection */
  Watch w;
  watch_start(ctx, &w);
  N5fCall c;
  GThread *t = n5f_call_start(&c, fd, req);
  watch_wait_request(&w, 1);
  if (g_strcmp0(w.a.kind, want_kind) != 0) g_printerr("kind: want %s got %s\n", want_kind, w.a.kind);
  CHECK(g_strcmp0(w.a.kind, want_kind) == 0);
  if (g_strcmp0(w.a.app_id, principal) != 0) g_printerr("principal: want %s got %s\n", principal, w.a.app_id);
  CHECK(g_strcmp0(w.a.app_id, principal) == 0);
  CHECK(g_strcmp0(w.a.identity, ctx->npub) == 0);
  approve(ctx, w.a.req_id, decision, remember);
  char *resp = n5f_call_finish(&c, t);
  watch_stop(ctx, &w);
  return resp;
}

/* A socket request that must be answered without a prompt. */
static char *n5f_no_prompt(Ctx *ctx, int fd, const char *req) {
  Watch w;
  watch_start(ctx, &w);
  N5fCall c;
  GThread *t = n5f_call_start(&c, fd, req);
  char *resp = n5f_call_finish(&c, t);
  watch_drain();
  if (w.a.n_requests) g_printerr("unexpected ApprovalRequested (%s) for %s\n", w.a.kind, req);
  CHECK(w.a.n_requests == 0);
  watch_stop(ctx, &w);
  return resp;
}

/* The result string of a success frame (hex, base64 or plain words: no
 * escapes expected). */
static char *n5f_result_string(const char *resp) {
  if (!strstr(resp, "\"error\":null")) g_printerr("unexpected error frame: %s\n", resp);
  CHECK(strstr(resp, "\"error\":null") != NULL);
  const char *p = strstr(resp, "\"result\":\"");
  CHECK(p != NULL);
  p += strlen("\"result\":\"");
  const char *e = strchr(p, '"');
  CHECK(e != NULL);
  char *r = g_strndup(p, (gsize)(e - p));
  CHECK(strchr(r, '\\') == NULL);
  return r;
}

static void n5f_expect_code(const char *resp, int code) {
  char want[32];
  g_snprintf(want, sizeof want, "\"code\":%d", code);
  if (!strstr(resp, want)) g_printerr("want error %d, got %s\n", code, resp);
  CHECK(strstr(resp, want) != NULL);
}

static void test_nip5f_gating(Ctx *ctx, const Nip5fSock *sock) {
  /* The socket principal: always this process's executable/scope (the
   * kernel reports the peer PID on Linux and macOS alike). */
  char *principal = self_principal();
  if (ctx->attested) CHECK(g_strcmp0(principal, ctx->principal) == 0);
  char *peer_sk = nostr_key_generate_private();
  char *peer_pk = nostr_key_get_public(peer_sk);
  CHECK(peer_sk && peer_pk);
  int fd = n5f_connect(sock);

  /* get_public_key: prompt; remembered; then answered from the grant. */
  char *resp = n5f_interactive(ctx, fd, "{\"id\":\"1\",\"method\":\"get_public_key\",\"params\":null}",
                               "get_public_key", principal, TRUE, TRUE);
  char *pk = n5f_result_string(resp);
  CHECK(g_strcmp0(pk, ctx->pk_hex) == 0);
  g_free(pk); g_free(resp);
  {
    char *key = g_strdup_printf("%s|%s", principal, ctx->npub);
    CHECK(grants_has(ctx, "get_public_key", key, "allow"));
    g_free(key);
  }
  resp = n5f_no_prompt(ctx, fd, "{\"id\":\"2\",\"method\":\"list_public_keys\",\"params\":null}");
  CHECK(strstr(resp, ctx->pk_hex) != NULL);
  g_free(resp);
  if (ctx->attested) {
    /* Same principal on the bus: the socket's grant answers D-Bus too. */
    GError *err = NULL;
    GVariant *r = no_prompt(ctx, "GetPublicKey", NULL, &err);
    CHECK(r != NULL);
    const char *npub = NULL;
    g_variant_get(r, "(&s)", &npub);
    CHECK(g_strcmp0(npub, ctx->npub) == 0);
    g_variant_unref(r);
  }

  /* nip44_decrypt: prompt; denied (not remembered) -> code 5, no grant. */
  {
    char *ct = incoming_nip44(ctx, peer_sk, "socket secret");
    char *req = g_strdup_printf("{\"id\":\"3\",\"method\":\"nip44_decrypt\",\"params\":"
                                "{\"peer_pub\":\"%s\",\"cipher_b64\":\"%s\"}}", peer_pk, ct);
    resp = n5f_interactive(ctx, fd, req, "nip44_decrypt", principal, FALSE, FALSE);
    n5f_expect_code(resp, 5);
    g_free(resp);
    char *key = g_strdup_printf("%s|%s", principal, ctx->npub);
    CHECK(!grants_has(ctx, "nip44_decrypt", key, NULL));
    g_free(key);
    /* ... and approved once: the plaintext comes back. */
    resp = n5f_interactive(ctx, fd, req, "nip44_decrypt", principal, TRUE, FALSE);
    char *pt = n5f_result_string(resp);
    CHECK(g_strcmp0(pt, "socket secret") == 0);
    g_free(pt); g_free(resp); g_free(req); g_free(ct);
  }

  /* A grant written to signer-grants.ini for this principal applies. */
  {
    char *body = g_strdup_printf("[nip44_encrypt]\n%s|%s=allow\n[event]\n%s|*=deny\n",
                                 principal, ctx->npub, principal);
    /* Keep the remembered get_public_key grant: append. */
    char *old = NULL;
    CHECK(g_file_get_contents(ctx->grants_path, &old, NULL, NULL));
    char *all = g_strconcat(old, body, NULL);
    write_file(ctx->grants_path, all);
    g_free(old); g_free(all); g_free(body);
    g_usleep(1100 * 1000); /* the daemon reloads on mtime/size change */
    char *req = g_strdup_printf("{\"id\":\"4\",\"method\":\"nip44_encrypt\",\"params\":"
                                "{\"peer_pub\":\"%s\",\"plaintext\":\"a \\\"quoted\\\" line\\n\"}}", peer_pk);
    resp = n5f_no_prompt(ctx, fd, req);
    char *payload = n5f_result_string(resp);
    uint8_t psk[32], mpk[32];
    CHECK(nostr_hex2bin(psk, peer_sk, 32));
    CHECK(nostr_hex2bin(mpk, ctx->pk_hex, 32));
    uint8_t *pt = NULL;
    size_t ptlen = 0;
    CHECK(nostr_nip44_decrypt_v2(psk, mpk, payload, &pt, &ptlen) == 0 && pt);
    CHECK(ptlen == strlen("a \"quoted\" line\n") && memcmp(pt, "a \"quoted\" line\n", ptlen) == 0);
    free(pt); g_free(payload); g_free(resp); g_free(req);

    /* sign_event: denied by the grants file, even though the request claims
     * an app_id the legacy signer-acl.ini allows. */
    resp = n5f_no_prompt(ctx, fd, "{\"id\":\"5\",\"method\":\"sign_event\",\"params\":{\"app_id\":"
                                  "\"contract-legacy\",\"event\":{\"kind\":1,\"content\":\"x\","
                                  "\"tags\":[],\"created_at\":0}}}");
    n5f_expect_code(resp, 5);
    g_free(resp);
  }
  close(fd);

  /* A client that hangs up while its request waits: the request is dropped. */
  {
    fd = n5f_connect(sock);
    g_usleep(120 * 1000);
    Watch w;
    watch_start(ctx, &w);
    CHECK(n5f_write(fd, "{\"id\":\"6\",\"method\":\"nip44_decrypt\",\"params\":{\"peer_pub\":"
                        "\"0000000000000000000000000000000000000000000000000000000000000001\","
                        "\"cipher_b64\":\"AA==\"}}") == 0);
    watch_wait_request(&w, 1);
    char *rid = g_strdup(w.a.req_id);
    close(fd);
    gboolean gone = FALSE;
    for (int i = 0; i < 40 && !gone; i++) {
      g_usleep(100 * 1000);
      GError *err = NULL;
      GVariant *info = call(ctx->bus, "GetApprovalInfo", g_variant_new("(s)", rid), "(a{sv})", &err);
      if (info) g_variant_unref(info);
      else { expect_remote_error(err, ERR_NOT_FND); gone = TRUE; }
      g_clear_error(&err);
    }
    CHECK(gone);
    g_free(rid);
    watch_stop(ctx, &w);
  }

  free(peer_sk); free(peer_pk);
  g_free(principal);
}

static void test_selector_not_key_material(Ctx *ctx) {
  const char *tmpl = "{\"kind\":1,\"created_at\":0,\"tags\":[],\"content\":\"selector\"}";
  GError *err = NULL;
  /* The daemon key's hex pubkey and npub select the daemon key. */
  const char *mine[] = { ctx->pk_hex, ctx->npub };
  for (size_t i = 0; i < G_N_ELEMENTS(mine); i++) {
    GVariant *r = call(ctx->bus, "SignEvent", g_variant_new("(sss)", tmpl, mine[i], "contract-test"), "(s)", &err);
    if (!r) { g_printerr("SignEvent(own pubkey selector #%zu): %s\n", i, err ? err->message : "?"); exit(1); }
    const char *js = NULL;
    g_variant_get(r, "(&s)", &js);
    assert_signed_event(js, ctx->pk_hex, 1, 0);
    g_variant_unref(r);
  }
  /* Another key's pubkey, another key's secret, and even the daemon's own
   * secret as hex: none names a known identity, none is used as a key. */
  char *osk = nostr_key_generate_private();
  char *opk = nostr_key_get_public(osk);
  CHECK(osk && opk);
  const char *not_known[] = { opk, osk, ctx->sk_hex };
  for (size_t i = 0; i < G_N_ELEMENTS(not_known); i++) {
    GVariant *r = call(ctx->bus, "SignEvent", g_variant_new("(sss)", tmpl, not_known[i], "contract-test"), "(s)", &err);
    if (r) {
      const char *js = NULL; g_variant_get(r, "(&s)", &js);
      g_printerr("SignEvent with unknown hex selector #%zu signed: %s\n", i, js);
      exit(1);
    }
    expect_remote_error(err, "org.nostr.Signer.Error.NoKeyConfigured");
    g_clear_error(&err);
  }
  /* An unknown npub: not found (the lookup's fallbacks must not sign with
   * some other key). An nsec: refused as input. */
  uint8_t b[32];
  char *onpub = NULL, *onsec = NULL;
  CHECK(nostr_hex2bin(b, opk, 32) && nostr_nip19_encode_npub(b, &onpub) == 0);
  CHECK(nostr_hex2bin(b, osk, 32) && nostr_nip19_encode_nsec(b, &onsec) == 0);
  GVariant *r = call(ctx->bus, "SignEvent", g_variant_new("(sss)", tmpl, onpub, "contract-test"), "(s)", &err);
  CHECK(r == NULL); expect_remote_error(err, "org.nostr.Signer.Error.NoKeyConfigured"); g_clear_error(&err);
  r = call(ctx->bus, "SignEvent", g_variant_new("(sss)", tmpl, onsec, "contract-test"), "(s)", &err);
  CHECK(r == NULL); expect_remote_error(err, ERR_INVALID); g_clear_error(&err);
  /* Same rule for the other identity-taking methods. */
  r = call(ctx->bus, "NIP44DeriveConversationKey", g_variant_new("(sss)", opk, osk, "contract-test"), "(s)", &err);
  CHECK(r == NULL); expect_remote_error(err, "org.nostr.Signer.Error.NoKeyConfigured"); g_clear_error(&err);
  r = call(ctx->bus, "NIP44Encrypt", g_variant_new("(sss)", "hi", opk, ctx->pk_hex), "(s)", &err);
  if (!r) { g_printerr("NIP44Encrypt(own hex pubkey selector): %s\n", err ? err->message : "?"); exit(1); }
  g_variant_unref(r);
  free(onpub); free(onsec); free(osk); free(opk);
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
#ifdef __APPLE__
  /* The Keychain backend writes the user's real login keychain; a test
   * must never do that. */
  (void)ctx;
  g_printerr("SKIP: StoreKey/ClearKey round-trip: the macOS Keychain is the login keychain\n");
  return FALSE;
#endif
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

  /* Clean up the stored item so the run leaves no residue in libsecret
   * (after the 500 ms per-sender mutation interval). */
  g_usleep(600 * 1000);
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
  TestKey client;          /* org.gnostr.NostrKey (gnostr client), nsec secret */
  TestKey client_dup;      /* org.gnostr.NostrKey whose key the signer already holds */
  TestKey client_foreign;  /* org.gnostr.NostrKey written by another application */
  TestKey client_hw;       /* org.gnostr.NostrKey whose npub has a hardware enrollment */
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

  /* A keyring whose bml6 (v1) pass already completed. */
  seed(&gnostr_secret_migration_schema, "Nostr signer migration marker (not a key)",
       "legacy-keys-v1", "name", "legacy-keys-v1", NULL);
  /* What apps/gnostr/src/util/keystore_libsecret.c wrote before e5nz. */
  gchar *client_label = g_strdup_printf("GNostr: %s", p->client.npub);
  seed(&gnostr_secret_legacy_client_schema, client_label, p->client.nsec,
       "npub", p->client.npub, "application", "org.gnostr.Client", NULL);
  g_free(client_label);
  seed(&gnostr_secret_legacy_client_schema, "GNostr: dup", p->client_dup.nsec,
       "npub", p->client_dup.npub, "application", "org.gnostr.Client", NULL);
  seed(&gnostr_secret_legacy_client_schema, "Other: foreign", p->client_foreign.nsec,
       "npub", p->client_foreign.npub, "application", "org.example.Other", NULL);
  /* The same key as client_dup, already in the signer under the user's own
   * label with key_id = npub (the {key_id, npub} a naive re-store would
   * collide with and replace). */
  const GnostrSecretIdentity dup = {
    .npub = p->client_dup.npub, .label = "Alice", .owner_uid = uid_buf,
  };
  GError *err2 = NULL;
  if (!gnostr_secret_store_save(&dup, p->client_dup.sk_hex, &err2)) {
    g_printerr("seed dup: %s\n", err2 ? err2->message : "?");
    exit(1);
  }
  /* A hardware enrollment of client_hw's key (key_id = npub, a token
   * reference as secret) plus the client's software copy: importing the
   * software copy must not prune the hardware item. */
  const GnostrSecretIdentity hw = {
    .npub = p->client_hw.npub, .label = "token", .origin = "hardware",
    .hardware_slot = "9c", .owner_uid = uid_buf,
  };
  if (!gnostr_secret_store_save(&hw, "pkcs11:token=yubikey;id=9c", &err2)) {
    g_printerr("seed hw: %s\n", err2 ? err2->message : "?");
    exit(1);
  }
  seed(&gnostr_secret_legacy_client_schema, "GNostr: hw", p->client_hw.nsec,
       "npub", p->client_hw.npub, "application", "org.gnostr.Client", NULL);
}

static const char *attr(GHashTable *a, const char *k) {
  const char *v = g_hash_table_lookup(a, k);
  return v ? v : "";
}

/* The single unified item for npub, checked against the expectations. */
static void check_unified(const TestKey *k, const char *want_label_attr,
                          const char *want_key_id) {
  GList *items = search_items(&gnostr_secret_schema, "npub", k->npub);
  CHECK(g_list_length(items) == 1);
  SecretItem *it = items->data;
  GHashTable *a = secret_item_get_attributes(it);
  gchar uid_buf[32];
  g_snprintf(uid_buf, sizeof uid_buf, "%u", (unsigned)getuid());
  CHECK(g_strcmp0(attr(a, "key_id"), want_key_id) == 0);
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
  test_key_new(&p.client);
  test_key_new(&p.client_dup);
  test_key_new(&p.client_foreign);
  test_key_new(&p.client_hw);
  /* Selectors resolve to different keys here, so grant any identity. */
  Ctx ctx;
  ctx_setup_full(&ctx, /*allow_mutations=*/TRUE, /*write_relays=*/FALSE,
                 "[event]\n@P|*=allow\n[get_public_key]\n@P|*=allow\n", &TRUST_UI,
                 phase3_pre_daemon, &p);

  /* The daemon migrates off the main loop; the marker is its last write.
   * The pre-seeded v1 marker must not have short-circuited this pass. */
  gboolean done = FALSE;
  for (int i = 0; i < 150 && !done; i++) {
    done = count_items(&gnostr_secret_migration_schema, "name",
                       GNOSTR_SECRET_MIGRATION_MARKER) == 1;
    if (!done) g_usleep(100 * 1000);
  }
  CHECK(done);

  /* Legacy software keys: moved, originals gone. */
  CHECK(count_items(&gnostr_secret_legacy_signer_key_schema, NULL, NULL) == 0);
  CHECK(count_items(&gnostr_secret_legacy_helper_schema, "npub", p.legacy_helper.npub) == 0);
  check_unified(&p.legacy_signer, "Legacy Main", "Legacy Main");
  check_unified(&p.legacy_helper, "bob", "bob");   /* nsec normalized to hex */
  /* Hardware reference: left in place, not copied. */
  CHECK(count_items(&gnostr_secret_legacy_helper_schema, "npub", p.legacy_hardware.npub) == 1);
  CHECK(count_items(&gnostr_secret_schema, "npub", p.legacy_hardware.npub) == 0);

  /* gnostr client keystore (nostrc-e5nz): imported under the fixed label
   * with key_id = npub, nsec normalized to hex, client copy deleted. */
  CHECK(count_items(&gnostr_secret_legacy_client_schema, "npub", p.client.npub) == 0);
  check_unified(&p.client, "gnostr import", p.client.npub);
  /* Already in the signer: client copy deleted, the signer's item (and the
   * user's label) untouched rather than replaced by the import label. */
  CHECK(count_items(&gnostr_secret_legacy_client_schema, "npub", p.client_dup.npub) == 0);
  check_unified(&p.client_dup, "Alice", p.client_dup.npub);
  /* Another application's item under the schema name: not ours, left. */
  CHECK(count_items(&gnostr_secret_legacy_client_schema, "npub", p.client_foreign.npub) == 1);
  CHECK(count_items(&gnostr_secret_schema, "npub", p.client_foreign.npub) == 0);
  /* Hardware enrollment present: the software copy is imported next to
   * it and the hardware item survives the save's duplicate pruning. */
  CHECK(count_items(&gnostr_secret_legacy_client_schema, "npub", p.client_hw.npub) == 0);
  CHECK(count_items(&gnostr_secret_schema, "npub", p.client_hw.npub) == 2);
  {
    GList *items = search_items(&gnostr_secret_schema, "npub", p.client_hw.npub);
    gboolean saw_hw = FALSE, saw_sw = FALSE;
    for (GList *l = items; l; l = l->next) {
      GHashTable *a = secret_item_get_attributes(l->data);
      SecretValue *sv = secret_item_get_secret(l->data);
      if (g_strcmp0(attr(a, "origin"), "hardware") == 0)
        saw_hw = g_strcmp0(secret_value_get_text(sv), "pkcs11:token=yubikey;id=9c") == 0;
      else
        saw_sw = g_strcmp0(attr(a, "label"), "gnostr import") == 0 &&
                 g_strcmp0(secret_value_get_text(sv), p.client_hw.sk_hex) == 0;
      secret_value_unref(sv);
      g_hash_table_unref(a);
    }
    g_list_free_full(items, g_object_unref);
    CHECK(saw_hw && saw_sw);
  }

  /* The daemon signs with a migrated key selected by npub (key_id is the
   * legacy label, so this exercises the npub fallback lookup), by the
   * legacy label as selector, and with the imported client key by npub. */
  for (int sel = 0; sel < 3; sel++) {
    const char *selector = sel == 0 ? p.legacy_signer.npub
                         : sel == 1 ? "Legacy Main" : p.client.npub;
    const char *want_pk = sel == 2 ? p.client.pk_hex : p.legacy_signer.pk_hex;
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
    assert_signed_event(signed_json, want_pk, 1, now);
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
    check_unified(&p.pre_bml6, "", p.pre_bml6.npub);
  }

  /* StoreKey → GetPublicKey → ClearKey against the real keyring. Key
   * mutations are rate-limited to one per 500 ms per sender. */
  g_usleep(600 * 1000);
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
  test_key_free(&p.legacy_signer);
  test_key_free(&p.legacy_helper);
  test_key_free(&p.legacy_hardware);
  test_key_free(&p.pre_bml6);
  test_key_free(&p.client);
  test_key_free(&p.client_dup);
  test_key_free(&p.client_foreign);
  test_key_free(&p.client_hw);
  g_print("PASS phase 3 (real keyring: legacy migration incl. gnostr client keystore "
          "past a v1 marker, unified StoreKey, marker)\n");
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
    test_selector_not_key_material(&ctx);
    test_get_relays_paths(&ctx, /*expect_ok=*/FALSE);
    test_store_key_denied_without_flag(&ctx);
    ctx_teardown(&ctx);
    g_print("PASS phase 1 (no mutations, no relays.conf)\n");
  }

  /* Gating (nip55l 0.4.0): no grants for this process except a web-origin
   * one it must not inherit; this process answers approvals. */
  {
    Ctx ctx;
    ctx_setup_full(&ctx, FALSE, TRUE, "[event]\nhttps://allowed.example|*=allow\n",
                   &TRUST_UI, NULL, NULL);
    test_gating(&ctx);
    gboolean attested = ctx.attested;
    ctx_teardown(&ctx);
    g_print("PASS gating (decrypt/pubkey/relays gated,%s remember round-trip, kind-keyed "
            "remember, coalescing, no-agent fail-fast)\n",
            attested ? " spoofed app_id denied," : " [unattested bus: claimed-app_id principals],");
  }

  /* An approval agent is present but this process is not a trusted one. */
  {
    Ctx ctx;
    const Trust untrusted = { FALSE, FALSE, TRUE };
    ctx_setup_full(&ctx, FALSE, FALSE, NULL, &untrusted, NULL, NULL);
    test_untrusted_approver(&ctx);
    gboolean ran = ctx.attested;
    ctx_teardown(&ctx);
    if (ran) g_print("PASS untrusted approver refused\n");
  }

  /* NIP-5F socket (nostrc-q23h): gated exactly like D-Bus. */
  {
    Ctx ctx;
    Nip5fSock sock = { NULL, NULL };
    ctx_setup_full(&ctx, FALSE, FALSE, NULL, &TRUST_UI, nip5f_pre_daemon, &sock);
    test_nip5f_gating(&ctx, &sock);
    gboolean attested = ctx.attested;
    ctx_teardown(&ctx);
    g_unsetenv("NOSTR_SIGNER_ENDPOINT");
    g_unlink(sock.path);
    g_rmdir(sock.dir);
    g_free(sock.path); g_free(sock.dir);
    g_print("PASS nip5f gating (peer-credential principal%s, prompt + remember, deny, grants "
            "file, legacy ACL/app_id ignored, hang-up drops request)\n",
            attested ? " = D-Bus principal, shared grant" : "");
  }

  /* This process as the trusted browser bridge. */
  {
    Ctx ctx;
    const Trust bridge = { TRUE, TRUE, TRUE };
    ctx_setup_full(&ctx, FALSE, FALSE,
                   "[event]\nhttps://allowed.example|*=allow\nhttps://denied.example|*=deny\n"
                   "[get_public_key]\nhttps://allowed.example|*=allow\n",
                   &bridge, NULL, NULL);
    test_bridge_origins(&ctx);
    gboolean ran = ctx.attested;
    ctx_teardown(&ctx);
    if (ran) g_print("PASS bridge (origin grants, origin prompt + remember, non-origin app_id)\n");
  }

  /* Mutations allowed, relays written. Exercises GetRelays
   * and (best-effort) StoreKey/ClearKey. */
  {
    Ctx ctx;
    ctx_setup(&ctx, /*allow_mutations=*/TRUE, /*write_relays=*/TRUE);
    test_get_relays_paths(&ctx, /*expect_ok=*/TRUE);
    g_usleep(600 * 1000); /* phase 1's StoreKey probe used this sender's mutation slot */
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
