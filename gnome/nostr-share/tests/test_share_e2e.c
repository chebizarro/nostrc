/* test_share_e2e.c - nostr-share through the real nostr-session-relayd
 *
 * SPDX-License-Identifier: MIT
 *
 * Integration test for bead nostrc-t24q. Headless and hermetic: a private
 * GTestDBus session bus carrying a fake org.nostr.Signer, the built
 * nostr-session-relayd on a temp HOME / XDG_RUNTIME_DIR, and a fake
 * upstream relay on 127.0.0.1 (apps/relayd/tests/fake_remote_relay.h).
 *
 *   - federation on, upstream_mode session_relay_or_direct / _only: the
 *     share goes to relay.sock only, the daemon forwards it, and nostr-share
 *     reports the upstream verdict ("forwarded by the session relay");
 *   - federation = 0 (FederationState disabled): or_direct publishes to
 *     the write relays itself, _only refuses;
 *   - --private (nostrc-k95e): the gift wrap reaches the recipient's
 *     kind-10050 inbox (a second fake relay) — routed there by the session
 *     relay while it federates, published there directly otherwise — and
 *     opens to the rumor with the recipient's key.
 *
 * Needs $NOSTR_SESSION_RELAYD and dbus-daemon; exits 77 (SKIP) otherwise.
 */
#define _GNU_SOURCE
#include "ns-share.h"
#include "ns-fake-signer.h"

#include "fake_remote_relay.h"
#include "nostr-keys.h"
#include <nostr/nip44/nip44.h>
#include <nostr/nip59/nip59.h>

#include <glib/gstdio.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>

typedef struct {
  const gchar     *bin;
  gchar           *root, *home, *run, *conf, *sock;
  GDBusConnection *bus;
  NsFakeSigner    *signer;
  FakeRelay       *upstream;
  FakeRelay       *inbox;        /* the recipient's kind-10050 relay */
  gchar           *rec_sk, *rec_pk;
  GPid             pid;
} E2e;

static E2e X;

static gboolean
name_has_owner(void)
{
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(
    X.bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
    "NameHasOwner", g_variant_new("(s)", "org.nostr.SessionRelay1"), G_VARIANT_TYPE("(b)"),
    G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
  gboolean b = FALSE;
  if (r != NULL)
    g_variant_get(r, "(b)", &b);
  return b;
}

static void
daemon_start(const gchar *conf_text)
{
  g_assert_true(g_file_set_contents(X.conf, conf_text, -1, NULL));
  gchar **env = g_get_environ();
  g_autofree gchar *data = g_build_filename(X.home, ".local", "share", NULL);
  g_autofree gchar *xcfg = g_build_filename(X.home, ".config", NULL);
  env = g_environ_setenv(env, "HOME", X.home, TRUE);
  env = g_environ_setenv(env, "XDG_RUNTIME_DIR", X.run, TRUE);
  env = g_environ_setenv(env, "XDG_DATA_HOME", data, TRUE);
  env = g_environ_setenv(env, "XDG_CONFIG_HOME", xcfg, TRUE);
  env = g_environ_unsetenv(env, "LISTEN_FDS");
  env = g_environ_unsetenv(env, "LISTEN_PID");
  env = g_environ_unsetenv(env, "NOTIFY_SOCKET");
  env = g_environ_setenv(env, "GIO_USE_VFS", "local", TRUE);
  env = g_environ_setenv(env, "GIO_USE_PROXY_RESOLVER", "dummy", TRUE);
  const gchar *argv[] = { X.bin, NULL };
  GError *err = NULL;
  g_assert_true(g_spawn_async(NULL, (gchar **)argv, env, G_SPAWN_DO_NOT_REAP_CHILD, NULL,
                              NULL, &X.pid, &err));
  g_assert_no_error(err);
  g_strfreev(env);
  gint64 end = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
  struct stat st;
  while (stat(X.sock, &st) != 0 || !name_has_owner()) {
    g_assert_cmpint(g_get_monotonic_time(), <, end);
    g_usleep(20 * 1000);   /* polling a child process coming up; no event to wait on */
  }
}

static void
daemon_stop(void)
{
  kill(X.pid, SIGTERM);
  int status = 0;
  waitpid(X.pid, &status, 0);
  g_spawn_close_pid(X.pid);
  X.pid = 0;
  gint64 end = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  while (name_has_owner() && g_get_monotonic_time() < end)
    g_usleep(20 * 1000);
}

static NsConfig *
config(NsUpstreamMode mode)
{
  NsConfig *cfg = ns_config_load(NULL);   /* NOSTR_SHARE_CONFIG=/nonexistent */
  g_assert_nonnull(cfg);
  g_strfreev(cfg->home_relays);
  const gchar *one[] = { X.upstream->url, NULL };
  cfg->home_relays = g_strdupv((gchar **)one);
  cfg->upstream = mode;
  cfg->ok_wait_sec = 20;
  cfg->query_timeout_ms = 1500;
  return cfg;
}

static gchar *
signed_json_by(const gchar *sk, const gchar *pk, gint kind, const gchar *tags_json,
               const gchar *content)
{
  g_autofree gchar *u = g_strdup_printf(
    "{\"pubkey\":\"%s\",\"created_at\":%" G_GINT64_FORMAT ",\"kind\":%d,\"tags\":%s,"
    "\"content\":\"%s\"}", pk, g_get_real_time() / G_USEC_PER_SEC, kind, tags_json, content);
  NostrEvent *ev = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(ev, u, NULL), ==, 1);
  g_assert_cmpint(nostr_event_sign(ev, sk), ==, 0);
  char *s = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);
  gchar *out = g_strdup(s);
  free(s);
  return out;
}

static gchar *
signed_json(gint kind, const gchar *tags_json, const gchar *content)
{
  return signed_json_by(ns_fake_signer_sk(X.signer), ns_fake_signer_pk(X.signer), kind,
                        tags_json, content);
}

/* Store @json in the session relay (what another local app would do). */
static void
store_locally(const gchar *json)
{
  NsNet net;
  ns_net_init(&net);
  g_assert_cmpstr(net.session_socket, ==, X.sock);
  NsConfig *cfg = config(NS_UPSTREAM_SESSION_RELAY_AND_DIRECT);
  const gchar *one[] = { NS_SESSION_RELAY_URL, NULL };
  NsTargets t = { 0 };
  t.targets = g_strdupv((gchar **)one);
  t.direct = g_new0(gchar *, 1);
  NsPublishReport rep;
  GError *err = NULL;
  g_assert_true(ns_net_publish(&net, cfg, json, &t, &rep, &err));
  g_assert_no_error(err);
  ns_publish_report_clear(&rep);
  ns_targets_clear(&t);
  ns_config_free(cfg);
  ns_net_clear(&net);
}

static NsShare *
share(NsUpstreamMode mode, const gchar *text)
{
  const gchar *texts[] = { text, NULL };
  NsShareOptions o = { .texts = texts };
  GError *err = NULL;
  NsShare *s = ns_share_new(config(mode), &o, &err);
  g_assert_no_error(err);
  g_assert_cmpstr(s->net.session_socket, ==, X.sock);
  g_assert_true(ns_share_connect(s, &err));
  g_assert_no_error(err);
  g_assert_cmpstr(s->pubkey_hex, ==, ns_fake_signer_pk(X.signer));
  return s;
}

/* --private to the recipient; the wrap must arrive at their inbox relay
 * and open to exactly the rumor nostr-share built. */
static void
private_share_reaches_inbox(NsUpstreamMode mode, const gchar *text, gboolean via_session)
{
  const gchar *texts[] = { text, NULL };
  NsShareOptions o = { .texts = texts, .to = X.rec_pk, .private_share = TRUE };
  GError *err = NULL;
  g_autoptr(NsShare) s = ns_share_new(config(mode), &o, &err);
  g_assert_no_error(err);
  g_assert_true(ns_share_connect(s, &err));
  g_assert_true(ns_share_resolve(s, &err));
  g_assert_no_error(err);
  g_assert_cmpint(s->targets.session_upstream, ==, via_session);
  g_assert_cmpstr(s->inbox_to.relays[0], ==, X.inbox->url);
  g_assert_true(ns_share_build(s, &err));
  g_assert_true(ns_share_publish(s, NULL, NULL, &err));
  g_assert_no_error(err);
  NsPost *p = g_ptr_array_index(s->posts, 0);
  g_test_message("%s", p->result);
  g_autofree gchar *wrap_id = ns_event_id_from_signed_json(p->signed_json);
  g_assert_true(fake_wait_id(X.inbox, wrap_id, 1, 10));
  g_assert_cmpuint(fake_count_id(X.upstream, wrap_id), ==, 0);   /* never the write relay */

  /* The recipient opens it. */
  NostrEvent *wrap = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(wrap, p->signed_json, NULL), ==,
                  NOSTR_EVENT_VALIDATION_OK);
  NostrEvent *seal = nostr_nip59_unwrap(wrap, X.rec_sk);
  g_assert_nonnull(seal);
  g_assert_cmpstr(nostr_event_get_pubkey(seal), ==, ns_fake_signer_pk(X.signer));
  guint8 rsk[32], spk[32];
  for (int i = 0; i < 32; i++) {
    rsk[i] = (guint8)((g_ascii_xdigit_value(X.rec_sk[2 * i]) << 4) |
                      g_ascii_xdigit_value(X.rec_sk[2 * i + 1]));
    const gchar *pk = ns_fake_signer_pk(X.signer);
    spk[i] = (guint8)((g_ascii_xdigit_value(pk[2 * i]) << 4) | g_ascii_xdigit_value(pk[2 * i + 1]));
  }
  guint8 *plain = NULL;
  size_t n = 0;
  g_assert_cmpint(nostr_nip44_decrypt_v2(rsk, spk, nostr_event_get_content(seal), &plain, &n),
                  ==, 0);
  g_autofree gchar *rumor = g_strndup((const gchar *)plain, n);
  free(plain);
  g_assert_cmpstr(rumor, ==, p->unsigned_json);
  nostr_event_free(seal);
  nostr_event_free(wrap);
}

static void
test_federating(void)
{
  daemon_start("federation_backoff_initial_seconds = 1\n"
               "federation_backoff_max_seconds = 2\n"
               "federation_ok_timeout_seconds = 5\n");
  /* The relay routes the author's events by the kind 10002 it holds. */
  g_autofree gchar *tags = g_strdup_printf("[[\"r\",\"%s\"]]", X.upstream->url);
  g_autofree gchar *rl = signed_json(10002, tags, "");
  store_locally(rl);

  static const NsUpstreamMode modes[] = { NS_UPSTREAM_SESSION_RELAY_OR_DIRECT,
                                          NS_UPSTREAM_SESSION_RELAY_ONLY };
  for (gsize i = 0; i < G_N_ELEMENTS(modes); i++) {
    GError *err = NULL;
    g_autoptr(NsShare) s = share(modes[i], i == 0 ? "e2e via the session relay"
                                                  : "e2e session relay only");
    g_assert_true(ns_share_resolve(s, &err));
    g_assert_no_error(err);
    g_assert_true(s->targets.session_upstream);
    g_assert_cmpuint(g_strv_length(s->targets.targets), ==, 1);
    g_assert_true(ns_share_build(s, &err));
    g_assert_true(ns_share_publish(s, NULL, NULL, &err));
    g_assert_no_error(err);
    NsPost *p = g_ptr_array_index(s->posts, 0);
    g_test_message("%s", p->result);
    g_assert_nonnull(strstr(p->result, "forwarded by the session relay: 1/1 relays accepted"));
    g_autofree gchar *id = ns_event_id_from_signed_json(p->signed_json);
    g_assert_true(fake_wait_id(X.upstream, id, 1, 10));   /* it really left */
  }

  /* The recipient's kind 10050, as a local client would have cached it. */
  g_autofree gchar *inbox_tags = g_strdup_printf("[[\"relay\",\"%s\"]]", X.inbox->url);
  g_autofree gchar *inbox = signed_json_by(X.rec_sk, X.rec_pk, 10050, inbox_tags, "");
  store_locally(inbox);
  private_share_reaches_inbox(NS_UPSTREAM_SESSION_RELAY_OR_DIRECT, "e2e private, federated",
                              TRUE);
  daemon_stop();
}

static void
test_not_federating(void)
{
  daemon_start("federation = 0\n");
  GError *err = NULL;

  g_autoptr(NsShare) d = share(NS_UPSTREAM_SESSION_RELAY_OR_DIRECT, "e2e direct fallback");
  g_assert_true(ns_share_resolve(d, &err));
  g_assert_no_error(err);
  g_assert_false(d->targets.session_upstream);
  g_assert_false(d->targets.session_included);
  g_autofree gchar *desc = ns_share_describe_targets(d);
  g_assert_nonnull(strstr(desc, "FederationState: disabled"));
  g_assert_true(ns_share_build(d, &err));
  g_assert_true(ns_share_publish(d, NULL, NULL, &err));
  g_assert_no_error(err);
  NsPost *p = g_ptr_array_index(d->posts, 0);
  g_autofree gchar *id = ns_event_id_from_signed_json(p->signed_json);
  g_assert_cmpuint(fake_count_id(X.upstream, id), ==, 1);

  g_autoptr(NsShare) o = share(NS_UPSTREAM_SESSION_RELAY_ONLY, "e2e refused");
  g_assert_false(ns_share_resolve(o, &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_NO_RELAYS);
  g_assert_nonnull(strstr(err->message, "FederationState: disabled"));
  g_clear_error(&err);

  /* The session relay still answers REQs from its store (the kind 10050
   * cached above), but the wrap goes to the inbox directly. */
  private_share_reaches_inbox(NS_UPSTREAM_SESSION_RELAY_OR_DIRECT, "e2e private, direct",
                              FALSE);
  daemon_stop();
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  X.bin = g_getenv("NOSTR_SESSION_RELAYD");
  g_autofree gchar *dbus_daemon = g_find_program_in_path("dbus-daemon");
  if (X.bin == NULL || *X.bin == '\0' || dbus_daemon == NULL) {
    g_printerr("NOSTR_SESSION_RELAYD unset or no dbus-daemon; SKIP\n");
    return 77;
  }
  signal(SIGPIPE, SIG_IGN);
  nostr_json_init();

  GTestDBus *bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);
  X.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  g_assert_nonnull(X.bus);
  X.signer = ns_fake_signer_start();
  X.upstream = fake_relay_start(FAKE_ACCEPT, 0);
  g_assert_nonnull(X.upstream);
  X.inbox = fake_relay_start(FAKE_ACCEPT, 0);
  g_assert_nonnull(X.inbox);
  X.rec_sk = nostr_key_generate_private();
  X.rec_pk = nostr_key_get_public(X.rec_sk);

  X.root = g_dir_make_tmp("ns-e2e-XXXXXX", NULL);
  X.home = g_build_filename(X.root, "home", NULL);
  X.run = g_build_filename(X.root, "run", NULL);
  g_autofree gchar *cfgdir = g_build_filename(X.home, ".config", "nostr", NULL);
  g_mkdir_with_parents(cfgdir, 0700);
  g_mkdir_with_parents(X.run, 0700);
  X.conf = g_build_filename(cfgdir, "session-relay.conf", NULL);
  X.sock = g_build_filename(X.run, "nostr", "relay.sock", NULL);
  /* nostr-share finds the relay where a user session would have it. */
  g_setenv("XDG_RUNTIME_DIR", X.run, TRUE);
  g_unsetenv("NOSTR_SHARE_SESSION_RELAY_SOCKET");
  g_setenv("NOSTR_SHARE_CONFIG", "/nonexistent", TRUE);

  g_test_add_func("/nostr-share/e2e/federating", test_federating);
  g_test_add_func("/nostr-share/e2e/not-federating", test_not_federating);
  int rc = g_test_run();

  if (X.pid != 0)
    daemon_stop();
  fake_relay_stop(X.upstream);
  fake_relay_stop(X.inbox);
  free(X.rec_sk);
  free(X.rec_pk);
  ns_fake_signer_stop(X.signer);
  g_clear_object(&X.bus);
  g_test_dbus_down(bus);
  g_object_unref(bus);
  return rc;
}
