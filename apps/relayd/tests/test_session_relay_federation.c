/*
 * test_session_relay_federation — the real nostr-session-relayd forwards
 * what a local app publishes on relay.sock to the account's NIP-65 write
 * relays, and says so on org.nostr.SessionRelay1 (bead nostrc-7d96).
 *
 * Headless: private GTestDBus session bus carrying a fake org.nostr.Signer
 * (GetPublicKey -> npub of a test key; SignEvent signs with it), two fake
 * remote relays on 127.0.0.1 (one accepts, one demands NIP-42 AUTH), temp
 * HOME / XDG dirs, no federation_accounts (the daemon must ask the
 * signer). Checks:
 *   - kind 10002 + kind 1 over relay.sock -> OK true locally -> both
 *     remotes receive the note (the AUTH one after a signer-signed AUTH);
 *   - UpstreamStatusChanged reports event_state "forwarded";
 *   - GetEventUpstream / GetUpstreamRelays / properties / GetStats agree;
 *   - a NIP-70 ["-"] event is stored locally but never leaves;
 *   - `--upstream EVENT_ID` prints the same status;
 *   - the outbox survives a daemon restart.
 * Needs $NOSTR_SESSION_RELAYD; exits 77 (SKIP) when unset.
 */
#define _GNU_SOURCE
#include <gio/gio.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "ws_test_client.h"

#include "fake_remote_relay.h"
#include "nostr-keys.h"

#define NAME "org.nostr.SessionRelay1"
#define PATH "/org/nostr/SessionRelay1"
#define IFACE "org.nostr.SessionRelay1"

/* ── npub (NIP-19 bech32) ─────────────────────────────────────────────── */

static uint32_t polymod(const uint8_t *v, size_t n) {
  static const uint32_t G[5] = {0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3};
  uint32_t c = 1;
  for (size_t i = 0; i < n; i++) {
    uint8_t top = (uint8_t)(c >> 25);
    c = ((c & 0x1ffffff) << 5) ^ v[i];
    for (int j = 0; j < 5; j++)
      if ((top >> j) & 1) c ^= G[j];
  }
  return c;
}

static char *npub_encode(const char *hex) {
  static const char CS[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
  uint8_t raw[32];
  for (int i = 0; i < 32; i++) {
    unsigned b;
    sscanf(hex + 2 * i, "%2x", &b);
    raw[i] = (uint8_t)b;
  }
  uint8_t data[64];
  int n = 0, bits = 0;
  uint32_t acc = 0;
  for (int i = 0; i < 32; i++) {
    acc = (acc << 8) | raw[i];
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      data[n++] = (acc >> bits) & 31;
    }
  }
  if (bits) data[n++] = (acc << (5 - bits)) & 31;
  uint8_t v[128];
  size_t m = 0;
  const char *hrp = "npub";
  for (int i = 0; i < 4; i++) v[m++] = (uint8_t)(hrp[i] >> 5);
  v[m++] = 0;
  for (int i = 0; i < 4; i++) v[m++] = (uint8_t)(hrp[i] & 31);
  for (int i = 0; i < n; i++) v[m++] = data[i];
  for (int i = 0; i < 6; i++) v[m++] = 0;
  uint32_t pm = polymod(v, m) ^ 1;
  GString *s = g_string_new("npub1");
  for (int i = 0; i < n; i++) g_string_append_c(s, CS[data[i]]);
  for (int i = 0; i < 6; i++) g_string_append_c(s, CS[(pm >> (5 * (5 - i))) & 31]);
  return g_string_free(s, FALSE);
}

/* ── fake org.nostr.Signer on its own thread ──────────────────────────── */

typedef struct {
  const char *addr;
  char *sk, *pk, *npub;
  GThread *th;
  GMainContext *ctx;
  GMainLoop *loop;
  GMutex lock;
  GCond cond;
  gboolean ready;
  guint pk_calls, sign_calls;
} FakeSigner;

static const char k_signer_xml[] =
    "<node><interface name='org.nostr.Signer'>"
    "<method name='GetPublicKey'><arg type='s' direction='out'/></method>"
    "<method name='SignEvent'><arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
    "</interface></node>";

static void signer_call(GDBusConnection *c, const gchar *sender, const gchar *path,
                        const gchar *iface, const gchar *method, GVariant *params,
                        GDBusMethodInvocation *inv, gpointer ud) {
  (void)c; (void)sender; (void)path; (void)iface;
  FakeSigner *s = ud;
  if (g_str_equal(method, "GetPublicKey")) {
    g_mutex_lock(&s->lock);
    s->pk_calls++;
    g_mutex_unlock(&s->lock);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", s->npub));
    return;
  }
  const gchar *ej = NULL, *ident = NULL, *app = NULL;
  g_variant_get(params, "(&s&s&s)", &ej, &ident, &app);
  NostrEvent *ev = nostr_event_new();
  if (nostr_event_deserialize(ev, ej) != 0) {
    nostr_event_free(ev);
    g_dbus_method_invocation_return_dbus_error(inv, "org.nostr.Signer.Error.InvalidInput", "bad");
    return;
  }
  nostr_event_set_pubkey(ev, s->pk);
  nostr_event_sign(ev, s->sk);
  char *out = nostr_event_serialize(ev);
  nostr_event_free(ev);
  g_mutex_lock(&s->lock);
  s->sign_calls++;
  g_mutex_unlock(&s->lock);
  g_printerr("  fake signer: SignEvent for app_id=%s\n", app);
  g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", out));
  free(out);
}

static void signer_name_acquired(GDBusConnection *c, const gchar *n, gpointer ud) {
  (void)c; (void)n;
  FakeSigner *s = ud;
  g_mutex_lock(&s->lock);
  s->ready = TRUE;
  g_cond_broadcast(&s->cond);
  g_mutex_unlock(&s->lock);
}

static gpointer signer_thread(gpointer p) {
  FakeSigner *s = p;
  g_main_context_push_thread_default(s->ctx);
  GDBusConnection *c = g_dbus_connection_new_for_address_sync(
      s->addr, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                   G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
      NULL, NULL, NULL);
  g_assert_nonnull(c);
  GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(k_signer_xml, NULL);
  static const GDBusInterfaceVTable vt = {signer_call, NULL, NULL, {0}};
  guint reg = g_dbus_connection_register_object(c, "/org/nostr/signer", node->interfaces[0],
                                                &vt, s, NULL, NULL);
  guint own = g_bus_own_name_on_connection(c, "org.nostr.Signer", G_BUS_NAME_OWNER_FLAGS_NONE,
                                           signer_name_acquired, NULL, s, NULL);
  g_main_loop_run(s->loop);
  g_bus_unown_name(own);
  g_dbus_connection_unregister_object(c, reg);
  g_dbus_connection_flush_sync(c, NULL, NULL);
  g_object_unref(c);
  g_dbus_node_info_unref(node);
  while (g_main_context_iteration(s->ctx, FALSE)) {
  }
  g_main_context_pop_thread_default(s->ctx);
  return NULL;
}

/* ── helpers ──────────────────────────────────────────────────────────── */

static gint64 s_clock;

static NostrEvent *mk(const char *sk, const char *pk, int kind, const char *tags,
                      const char *content) {
  char *j = g_strdup_printf("{\"kind\":%d,\"created_at\":%" G_GINT64_FORMAT
                            ",\"tags\":%s,\"content\":\"%s\",\"pubkey\":\"%s\"}",
                            kind, ++s_clock, tags, content, pk);
  NostrEvent *ev = nostr_event_new();
  if (nostr_event_deserialize(ev, j) != 0 || nostr_event_sign(ev, sk) != 0)
    g_error("event construction failed");
  g_free(j);
  return ev;
}

/* Publish over relay.sock; expect OK true. Returns the id (free()). */
static char *publish(int fd, NostrEvent *ev, const char *what) {
  char *id = nostr_event_get_id(ev);
  char *json = nostr_event_serialize(ev);
  char *frame = g_strdup_printf("[\"EVENT\",%s]", json);
  char *prefix = g_strdup_printf("[\"OK\",\"%s\",true", id);
  expect_reply(fd, frame, prefix, what);
  g_free(prefix);
  g_free(frame);
  free(json);
  nostr_event_free(ev);
  return id;
}

static GHashTable *s_signal_state; /* id -> event_state (main thread) */

static void on_upstream_signal(GDBusConnection *c, const gchar *sender, const gchar *path,
                               const gchar *iface, const gchar *sig, GVariant *params,
                               gpointer ud) {
  (void)c; (void)sender; (void)path; (void)iface; (void)sig; (void)ud;
  const gchar *id, *relay, *rstate, *reason, *estate;
  g_variant_get(params, "(&s&s&s&s&s)", &id, &relay, &rstate, &reason, &estate);
  g_printerr("  signal: %.12s.. %s %s %s%s%s\n", id, *relay ? relay : "-", rstate, estate,
             *reason ? " — " : "", reason);
  if (*estate) g_hash_table_replace(s_signal_state, g_strdup(id), g_strdup(estate));
}

static gboolean wait_signal_state(const char *id, const char *want, int timeout_s) {
  gint64 end = g_get_monotonic_time() + (gint64)timeout_s * G_USEC_PER_SEC;
  while (g_strcmp0(g_hash_table_lookup(s_signal_state, id), want) != 0) {
    if (g_get_monotonic_time() > end) return FALSE;
    g_main_context_iteration(NULL, FALSE);
    g_usleep(10 * 1000);
  }
  return TRUE;
}

static GVariant *call(GDBusConnection *c, const char *method, GVariant *args, const char *type) {
  GError *err = NULL;
  GVariant *r = g_dbus_connection_call_sync(c, NAME, PATH, IFACE, method, args,
                                            G_VARIANT_TYPE(type), G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                            5000, NULL, &err);
  if (!r) g_error("%s: %s", method, err->message);
  return r;
}

static GVariant *prop(GDBusConnection *c, const char *name) {
  GError *err = NULL;
  GVariant *r = g_dbus_connection_call_sync(
      c, NAME, PATH, "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", IFACE, name),
      G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, NULL, &err);
  if (!r) g_error("Get %s: %s", name, err->message);
  GVariant *v = NULL;
  g_variant_get(r, "(v)", &v);
  g_variant_unref(r);
  return v;
}

static char *event_state(GDBusConnection *c, const char *id, GVariant **relays_out) {
  GVariant *r = call(c, "GetEventUpstream", g_variant_new("(s)", id), "(ssa(sssuxx))");
  const gchar *st = NULL, *detail = NULL;
  GVariant *relays = NULL;
  g_variant_get(r, "(&s&s@a(sssuxx))", &st, &detail, &relays);
  char *out = g_strdup(st);
  if (relays_out) *relays_out = relays;
  else g_variant_unref(relays);
  g_variant_unref(r);
  return out;
}

static gboolean name_has_owner(GDBusConnection *c) {
  GVariant *r = g_dbus_connection_call_sync(
      c, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
      g_variant_new("(s)", NAME), G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
  gboolean b = FALSE;
  if (r) {
    g_variant_get(r, "(b)", &b);
    g_variant_unref(r);
  }
  return b;
}

static GPid spawn(const char *bin, gchar **envp) {
  const gchar *argv[] = {bin, NULL};
  GPid pid = 0;
  GError *err = NULL;
  if (!g_spawn_async(NULL, (gchar **)argv, envp, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, &err))
    g_error("spawn: %s", err->message);
  return pid;
}

static int stop(GPid pid) {
  kill(pid, SIGTERM);
  int status = 0;
  gint64 end = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
  while (waitpid(pid, &status, WNOHANG) == 0) {
    if (g_get_monotonic_time() > end) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      return -1;
    }
    g_usleep(20 * 1000);
  }
  g_spawn_close_pid(pid);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

static void wait_up(GDBusConnection *c, const char *sock) {
  gint64 end = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
  struct stat st;
  while (stat(sock, &st) != 0 || !name_has_owner(c)) {
    if (g_get_monotonic_time() > end) g_error("daemon did not come up");
    g_usleep(20 * 1000);
  }
}

int main(void) {
  const char *bin = getenv("NOSTR_SESSION_RELAYD");
  if (!bin || !*bin) {
    fprintf(stderr, "NOSTR_SESSION_RELAYD unset; SKIP\n");
    return 77;
  }
  signal(SIGPIPE, SIG_IGN);
  nostr_json_init();
  s_clock = (gint64)time(NULL) - 100;
  s_signal_state = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

  FakeRelay *A = fake_relay_start(FAKE_ACCEPT, 0);
  FakeRelay *C = fake_relay_start(FAKE_AUTH, 0);
  if (!A || !C) g_error("fake relays");

  GTestDBus *bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);
  const char *addr = g_test_dbus_get_bus_address(bus);
  GDBusConnection *conn = g_dbus_connection_new_for_address_sync(
      addr, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
      NULL, NULL, NULL);
  g_dbus_connection_signal_subscribe(conn, NULL, IFACE, "UpstreamStatusChanged", PATH, NULL,
                                     G_DBUS_SIGNAL_FLAGS_NONE, on_upstream_signal, NULL, NULL);

  FakeSigner sg = {0};
  sg.addr = addr;
  sg.sk = nostr_key_generate_private();
  sg.pk = nostr_key_get_public(sg.sk);
  sg.npub = npub_encode(sg.pk);
  g_mutex_init(&sg.lock);
  g_cond_init(&sg.cond);
  sg.ctx = g_main_context_new();
  sg.loop = g_main_loop_new(sg.ctx, FALSE);
  sg.th = g_thread_new("fake-signer", signer_thread, &sg);
  g_mutex_lock(&sg.lock);
  while (!sg.ready) g_cond_wait(&sg.cond, &sg.lock);
  g_mutex_unlock(&sg.lock);

  gchar *root = g_dir_make_tmp("nsr-fed-e2e-XXXXXX", NULL);
  gchar *home = g_build_filename(root, "home", NULL);
  gchar *run = g_build_filename(root, "run", NULL);
  gchar *cfgdir = g_build_filename(home, ".config", "nostr", NULL);
  gchar *data = g_build_filename(home, ".local", "share", NULL);
  gchar *xcfg = g_build_filename(home, ".config", NULL);
  g_mkdir_with_parents(cfgdir, 0700);
  g_mkdir_with_parents(run, 0700);
  gchar *conf = g_build_filename(cfgdir, "session-relay.conf", NULL);
  g_file_set_contents(conf,
                      "federation_backoff_initial_seconds = 1\n"
                      "federation_backoff_max_seconds = 2\n"
                      "federation_ok_timeout_seconds = 5\n", -1, NULL);
  gchar **env = g_get_environ();
  env = g_environ_setenv(env, "HOME", home, TRUE);
  env = g_environ_setenv(env, "XDG_RUNTIME_DIR", run, TRUE);
  env = g_environ_setenv(env, "XDG_DATA_HOME", data, TRUE);
  env = g_environ_setenv(env, "XDG_CONFIG_HOME", xcfg, TRUE);
  env = g_environ_setenv(env, "DBUS_SESSION_BUS_ADDRESS", addr, TRUE);
  env = g_environ_unsetenv(env, "LISTEN_FDS");
  env = g_environ_unsetenv(env, "LISTEN_PID");
  env = g_environ_unsetenv(env, "NOTIFY_SOCKET");
  /* Keep the host's GIO modules (gvfs, libproxy) out of the daemon: their
   * process-lifetime singletons read as leaks under LSan. The packaged
   * daemon keeps honouring the desktop proxy settings. */
  env = g_environ_setenv(env, "GIO_USE_VFS", "local", TRUE);
  env = g_environ_setenv(env, "GIO_USE_PROXY_RESOLVER", "dummy", TRUE);
  gchar *sock = g_build_filename(run, "nostr", "relay.sock", NULL);

  GPid pid = spawn(bin, env);
  wait_up(conn, sock);

  GVariant *v = prop(conn, "FederationState");
  CHECK(g_str_equal(g_variant_get_string(v, NULL), "waiting-for-account"),
        "fresh daemon: FederationState %s", g_variant_get_string(v, NULL));
  g_variant_unref(v);

  long long up = 0;
  int fd = ws_open(sock, &up);
  if (fd < 0) g_error("ws upgrade over relay.sock failed");
  char *tags = g_strdup_printf("[[\"r\",\"%s\"],[\"r\",\"%s\",\"write\"]]", A->url, C->url);
  char *rl = publish(fd, mk(sg.sk, sg.pk, 10002, tags, ""), "kind 10002 stored");
  g_free(tags);
  char *note = publish(fd, mk(sg.sk, sg.pk, 1, "[]", "hello from relay.sock"), "kind 1 stored");
  char *prot = publish(fd, mk(sg.sk, sg.pk, 1, "[[\"-\"]]", "protected"), "NIP-70 stored");

  CHECK(fake_wait_id(A, note, 1, 30), "accepting remote never received the note");
  CHECK(fake_wait_id(C, note, 1, 30), "AUTH-demanding remote never received the note");
  CHECK(wait_signal_state(note, "forwarded", 30), "no UpstreamStatusChanged(forwarded)");
  CHECK(wait_signal_state(rl, "forwarded", 30), "relay list not forwarded");
  /* Acked by the AUTH relay only after a valid AUTH as the account. */
  CHECK(fake_auth_count(C, sg.pk) >= 1, "no valid NIP-42 AUTH as the account");

  GVariant *relays = NULL;
  char *st = event_state(conn, note, &relays);
  CHECK(g_str_equal(st, "forwarded"), "GetEventUpstream: %s", st);
  CHECK(g_variant_n_children(relays) == 2, "targets: %zu", g_variant_n_children(relays));
  for (gsize i = 0; i < g_variant_n_children(relays); i++) {
    const gchar *url, *rs, *reason;
    guint32 attempts;
    gint64 upd, acked;
    g_variant_get_child(relays, i, "(&s&s&suxx)", &url, &rs, &reason, &attempts, &upd, &acked);
    fprintf(stderr, "  %s: %s acked_at=%" G_GINT64_FORMAT "\n", url, rs, acked);
    CHECK(g_str_equal(rs, "acked") && acked > 0, "%s not acked", url);
  }
  g_variant_unref(relays);
  g_free(st);
  st = event_state(conn, prot, NULL);
  CHECK(g_str_equal(st, "unknown"), "protected event queued upstream: %s", st);
  g_free(st);
  CHECK(fake_count_id(A, prot) == 0 && fake_count_id(C, prot) == 0, "NIP-70 event left the machine");

  v = prop(conn, "FederationState");
  CHECK(g_str_equal(g_variant_get_string(v, NULL), "active"), "FederationState %s",
        g_variant_get_string(v, NULL));
  g_variant_unref(v);
  v = prop(conn, "ForwardedCount");
  CHECK(g_variant_get_uint64(v) >= 2, "ForwardedCount %" G_GUINT64_FORMAT, g_variant_get_uint64(v));
  g_variant_unref(v);
  v = prop(conn, "PendingUpstream");
  CHECK(g_variant_get_uint32(v) == 0, "PendingUpstream %u", g_variant_get_uint32(v));
  g_variant_unref(v);
  v = prop(conn, "FailedUpstream");
  CHECK(g_variant_get_uint64(v) == 0, "FailedUpstream %" G_GUINT64_FORMAT, g_variant_get_uint64(v));
  g_variant_unref(v);

  GVariant *stats = call(conn, "GetStats", NULL, "(a{sv})");
  GVariant *dict = g_variant_get_child_value(stats, 0);
  const struct { const char *k, *t; } keys[] = {
      {"federation_state", "s"}, {"federation_detail", "s"}, {"pending_upstream", "u"},
      {"unroutable_upstream", "u"}, {"pending_upstream_deliveries", "u"},
      {"forwarded_count", "t"}, {"partially_forwarded_count", "t"}, {"failed_upstream", "t"},
      {"skipped_upstream", "t"}, {"last_upstream_error", "s"},
      {"upstream_relays_connected", "u"},
  };
  for (gsize i = 0; i < G_N_ELEMENTS(keys); i++) {
    GVariant *x = g_variant_lookup_value(dict, keys[i].k, G_VARIANT_TYPE(keys[i].t));
    CHECK(x != NULL, "GetStats lacks %s:%s", keys[i].k, keys[i].t);
    if (x) g_variant_unref(x);
  }
  g_variant_unref(dict);
  g_variant_unref(stats);

  GVariant *ur = call(conn, "GetUpstreamRelays", NULL, "(a(sa{sv}))");
  GVariantIter *it = NULL;
  g_variant_get(ur, "(a(sa{sv}))", &it);
  const gchar *url;
  GVariant *props;
  gboolean saw_auth = FALSE, saw_a = FALSE;
  while (g_variant_iter_next(it, "(&s@a{sv})", &url, &props)) {
    gboolean authed = FALSE;
    guint64 acked = 0;
    g_variant_lookup(props, "authenticated", "b", &authed);
    g_variant_lookup(props, "acked", "t", &acked);
    if (g_str_equal(url, C->url)) saw_auth = authed && acked >= 2;
    if (g_str_equal(url, A->url)) saw_a = acked >= 2;
    g_variant_unref(props);
  }
  g_variant_iter_free(it);
  g_variant_unref(ur);
  CHECK(saw_a, "GetUpstreamRelays: accepting relay not acked");
  CHECK(saw_auth, "GetUpstreamRelays: AUTH relay not authenticated/acked");
  g_mutex_lock(&sg.lock);
  CHECK(sg.pk_calls >= 1, "daemon never asked GetPublicKey");
  CHECK(sg.sign_calls >= 1, "daemon never asked the signer to sign AUTH");
  g_mutex_unlock(&sg.lock);

  /* --upstream CLI */
  const gchar *argv[] = {bin, "--upstream", note, NULL};
  gchar *out = NULL;
  gint wstatus = 0;
  g_spawn_sync(NULL, (gchar **)argv, env, G_SPAWN_DEFAULT, NULL, NULL, &out, NULL, &wstatus, NULL);
  fprintf(stderr, "  --upstream:\n%s", out ? out : "");
  CHECK(out && strstr(out, "state: forwarded") && strstr(out, C->url), "--upstream output");
  g_free(out);

  close(fd);
  CHECK(stop(pid) == 0, "daemon did not exit cleanly");

  /* The outbox survives a restart. */
  pid = spawn(bin, env);
  wait_up(conn, sock);
  st = event_state(conn, note, NULL);
  CHECK(g_str_equal(st, "forwarded"), "after restart: %s", st);
  g_free(st);
  CHECK(stop(pid) == 0, "daemon did not exit cleanly (restart)");

  g_main_loop_quit(sg.loop);
  g_thread_join(sg.th);
  g_main_loop_unref(sg.loop);
  g_main_context_unref(sg.ctx);
  g_object_unref(conn);
  g_test_dbus_down(bus);
  g_object_unref(bus);
  fake_relay_stop(A);
  fake_relay_stop(C);
  free(rl);
  free(note);
  free(prot);
  free(sg.sk);
  free(sg.pk);
  g_free(sg.npub);
  g_mutex_clear(&sg.lock);
  g_cond_clear(&sg.cond);
  g_hash_table_unref(s_signal_state);
  g_strfreev(env);
  gchar *cmd = g_strdup_printf("rm -rf '%s'", root);
  if (g_failures == 0 && system(cmd) != 0) fprintf(stderr, "cleanup failed\n");
  g_free(cmd);
  g_free(sock);
  g_free(conf);
  g_free(xcfg);
  g_free(data);
  g_free(cfgdir);
  g_free(run);
  g_free(home);
  if (g_failures) {
    fprintf(stderr, "test_session_relay_federation: %d failure(s) (state in %s)\n", g_failures,
            root);
    g_free(root);
    return 1;
  }
  g_free(root);
  fprintf(stderr, "test_session_relay_federation: ok\n");
  return 0;
}
