/* test_share.c - engine end-to-end on fixture transports (no network)
 *
 * SPDX-License-Identifier: MIT
 *
 * Relays are libnostr-publish fixture transports driven by a responder
 * that answers REQ with canned (really signed) events + EOSE, and EVENT
 * with OK. The signer is a vtable signer that signs with a throwaway
 * libnostr key, so every published event verifies.
 */
#include "ns-dav.h"
#include "ns-git.h"
#include "ns-share.h"

#include "np-fake-session-relay.h"

#include <nostr/nip44/nip44.h>
#include <nostr/nip59/nip59.h>
#include <libsoup/soup.h>

#include "nostr-event.h"
#include "nostr-keys.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* ---- signer ---- */

typedef struct {
  gchar *sk;
  gchar *pk;
} Keys;

static Keys K;

static gchar *
sign_json(const gchar *unsigned_json)
{
  NostrEvent *ev = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(ev, unsigned_json, NULL), ==, 1);
  g_assert_cmpint(nostr_event_sign(ev, K.sk), ==, 0);
  char *s = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);
  gchar *out = g_strdup(s);
  free(s);
  return out;
}

static gchar *
mock_sign(gpointer ud, const gchar *unsigned_json, GCancellable *c, GError **e)
{
  (void)ud; (void)c; (void)e;
  return sign_json(unsigned_json);
}

static gchar *
signed_event(gint kind, gint64 created_at, const gchar *tags_json)
{
  g_autofree gchar *u = g_strdup_printf(
    "{\"pubkey\":\"%s\",\"created_at\":%" G_GINT64_FORMAT ",\"kind\":%d,"
    "\"tags\":%s,\"content\":\"\"}", K.pk, created_at, kind, tags_json);
  return sign_json(u);
}

/* An event by someone else (@sk / @pk). */
static gchar *
signed_event_by(const gchar *sk, const gchar *pk, gint kind, const gchar *tags_json)
{
  g_autofree gchar *u = g_strdup_printf(
    "{\"pubkey\":\"%s\",\"created_at\":%" G_GINT64_FORMAT ",\"kind\":%d,"
    "\"tags\":%s,\"content\":\"\"}", pk, g_get_real_time() / G_USEC_PER_SEC - 60,
    kind, tags_json);
  NostrEvent *ev = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(ev, u, NULL), ==, 1);
  g_assert_cmpint(nostr_event_sign(ev, sk), ==, 0);
  char *s = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);
  gchar *out = g_strdup(s);
  free(s);
  return out;
}

static void
hex32(const gchar *hex, guint8 out[32])
{
  for (int i = 0; i < 32; i++)
    out[i] = (guint8)((g_ascii_xdigit_value(hex[2 * i]) << 4) |
                      g_ascii_xdigit_value(hex[2 * i + 1]));
}

/* The signer's NIP44Encrypt, done with the test key (what the daemon does
 * with the user's key). */
static gchar *
mock_nip44(gpointer ud, const gchar *plaintext, const gchar *peer_hex, GCancellable *c,
           GError **e)
{
  (void)ud; (void)c; (void)e;
  guint8 sk[32], pk[32];
  hex32(K.sk, sk);
  hex32(peer_hex, pk);
  char *out = NULL;
  g_assert_cmpint(nostr_nip44_encrypt_v2(sk, pk, (const guint8 *)plaintext, strlen(plaintext),
                                         &out), ==, 0);
  gchar *ret = g_strdup(out);
  free(out);
  return ret;
}

/* ---- fixture relays ---- */

typedef struct {
  GHashTable *canned;      /* "url|kind" → GPtrArray of event JSON */
  GHashTable *reject;      /* url → reason */
  GPtrArray  *published;   /* "url EVENT-json" */
  GPtrArray  *reqs;        /* urls that got a REQ */
} Relays;

static Relays R;

typedef struct {
  NostrPublishTransport *t;
} Responder;

static void
respond(NostrPublishTransport *t)
{
  const gchar *url = nostr_publish_transport_get_url(t);
  gsize n = 0;
  gchar **frames = nostr_publish_transport_fixture_take_sent(t, &n);
  for (gsize i = 0; i < n; i++) {
    g_autoptr(JsonParser) p = json_parser_new();
    g_assert_true(json_parser_load_from_data(p, frames[i], -1, NULL));
    JsonArray *a = json_node_get_array(json_parser_get_root(p));
    const gchar *cmd = json_array_get_string_element(a, 0);
    if (g_str_equal(cmd, "REQ")) {
      const gchar *sub = json_array_get_string_element(a, 1);
      JsonObject *f = json_array_get_object_element(a, 2);
      gint64 kind = json_array_get_int_element(json_object_get_array_member(f, "kinds"), 0);
      g_ptr_array_add(R.reqs, g_strdup(url));
      g_autofree gchar *key = g_strdup_printf("%s|%" G_GINT64_FORMAT, url, kind);
      GPtrArray *evs = g_hash_table_lookup(R.canned, key);
      for (guint j = 0; evs && j < evs->len; j++) {
        g_autofree gchar *env = g_strdup_printf("[\"EVENT\",\"%s\",%s]", sub,
                                                (gchar *)g_ptr_array_index(evs, j));
        nostr_publish_transport_fixture_deliver_frame(t, "EVENT", env);
      }
      g_autofree gchar *eose = g_strdup_printf("[\"EOSE\",\"%s\"]", sub);
      nostr_publish_transport_fixture_deliver_frame(t, "EOSE", eose);
    } else if (g_str_equal(cmd, "EVENT")) {
      JsonObject *ev = json_array_get_object_element(a, 1);
      const gchar *id = json_object_get_string_member(ev, "id");
      g_autofree gchar *evjson = json_to_string(json_array_get_element(a, 1), FALSE);
      g_ptr_array_add(R.published, g_strdup_printf("%s %s", url, evjson));
      const gchar *reason = g_hash_table_lookup(R.reject, url);
      g_autofree gchar *ok = g_strdup_printf("[\"OK\",\"%s\",%s,\"%s\"]", id,
                                             reason ? "false" : "true",
                                             reason ? reason : "");
      nostr_publish_transport_fixture_deliver_frame(t, "OK", ok);
    }
  }
  g_strfreev(frames);
}

static gboolean
responder_tick(gpointer data)
{
  Responder *r = data;
  respond(r->t);
  return G_SOURCE_CONTINUE;
}

static void
responder_free(gpointer data)
{
  Responder *r = data;
  nostr_publish_transport_unref(r->t);
  g_free(r);
}

static NostrPublishTransport *
fixture_factory(const gchar *url, gpointer user_data)
{
  (void)user_data;
  NostrPublishTransport *t = nostr_publish_transport_new_fixture(url);
  Responder *r = g_new0(Responder, 1);
  r->t = nostr_publish_transport_ref(t);
  GSource *s = g_timeout_source_new(2);
  g_source_set_callback(s, responder_tick, r, responder_free);
  GMainContext *ctx = g_main_context_get_thread_default();
  g_source_attach(s, ctx ? ctx : g_main_context_default());
  g_source_unref(s);
  return t;
}

static void
relays_reset(void)
{
  g_clear_pointer(&R.canned, g_hash_table_unref);
  g_clear_pointer(&R.reject, g_hash_table_unref);
  g_clear_pointer(&R.published, g_ptr_array_unref);
  g_clear_pointer(&R.reqs, g_ptr_array_unref);
  R.canned = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                   (GDestroyNotify)g_ptr_array_unref);
  R.reject = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  R.published = g_ptr_array_new_with_free_func(g_free);
  R.reqs = g_ptr_array_new_with_free_func(g_free);
}

static void
can(const gchar *url, gint kind, gchar *event_json)
{
  g_autofree gchar *key = g_strdup_printf("%s|%d", url, kind);
  GPtrArray *a = g_hash_table_lookup(R.canned, key);
  if (a == NULL) {
    a = g_ptr_array_new_with_free_func(g_free);
    g_hash_table_insert(R.canned, g_strdup(key), a);
  }
  g_ptr_array_add(a, event_json);
}

/* ---- share construction ---- */

static NsConfig *
config(const gchar *relays, const gchar *servers)
{
  NsConfig *cfg = ns_config_load(NULL);   /* isolated XDG_CONFIG_HOME: no file */
  g_assert_nonnull(cfg);
  g_strfreev(cfg->home_relays);
  cfg->home_relays = g_strsplit(relays ? relays : "", ";", -1);
  if (relays == NULL) { g_strfreev(cfg->home_relays); cfg->home_relays = g_new0(gchar *, 1); }
  g_strfreev(cfg->blossom_servers);
  cfg->blossom_servers = servers ? g_strsplit(servers, ";", -1) : g_new0(gchar *, 1);
  cfg->ok_wait_sec = 5;
  cfg->query_timeout_ms = 2000;
  return cfg;
}

static NsShare *
share_new(NsConfig *cfg, const gchar *const *texts, const gchar *const *args,
          const gchar *to, gint kind, GError **error)
{
  NsShareOptions o = { .forced_kind = kind, .to = to, .texts = texts, .args = args };
  NsShare *s = ns_share_new(cfg, &o, error);
  if (s == NULL)
    return NULL;
  s->net.factory = fixture_factory;
  NostrPublishSignerVTable vt = { mock_sign, NULL };
  s->signer = nostr_publish_signer_new_from_vtable(&vt, NULL);
  s->pubkey_hex = g_strdup(K.pk);
  return s;
}

static JsonObject *
parse_obj(const gchar *json, JsonParser **keep)
{
  *keep = json_parser_new();
  g_assert_true(json_parser_load_from_data(*keep, json, -1, NULL));
  return json_node_get_object(json_parser_get_root(*keep));
}

static gboolean
has_tag(JsonObject *ev, const gchar *name, const gchar *value)
{
  JsonArray *tags = json_object_get_array_member(ev, "tags");
  for (guint i = 0; i < json_array_get_length(tags); i++) {
    JsonArray *t = json_array_get_array_element(tags, i);
    if (json_array_get_length(t) >= 2 &&
        g_str_equal(json_array_get_string_element(t, 0), name) &&
        (value == NULL || g_str_equal(json_array_get_string_element(t, 1), value)))
      return TRUE;
  }
  return FALSE;
}

static gchar *
tmpfile_with(const gchar *name, const void *data, gsize len)
{
  g_autofree gchar *dir = g_dir_make_tmp("ns-test-XXXXXX", NULL);
  gchar *path = g_build_filename(dir, name, NULL);
  g_assert_true(g_file_set_contents(path, data, (gssize)len, NULL));
  return path;
}

/* ---- tests ---- */

/* nostrc-eqdz: the process runs with a private XDG_RUNTIME_DIR (see
 * main()), so a session relay the host or lab happens to run is never
 * picked up; the socket is found only where this test puts one. */
static gchar *s_private;   /* private XDG_RUNTIME_DIR / XDG_CONFIG_HOME root */

static void
test_session_socket_detection(void)
{
  const gchar *run = g_get_user_runtime_dir();
  g_assert_true(g_str_has_prefix(run, s_private));
  g_assert_null(g_getenv("NOSTR_SHARE_SESSION_RELAY_SOCKET"));
  g_assert_null(ns_session_relay_socket());

  g_autofree gchar *dir = g_build_filename(run, "nostr", NULL);
  g_autofree gchar *path = g_build_filename(dir, "relay.sock", NULL);
  g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
  g_assert_true(g_file_set_contents(path, "", 0, NULL));
  g_assert_null(ns_session_relay_socket());          /* not a socket */
  g_assert_cmpint(g_unlink(path), ==, 0);

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un sa = { 0 };
  sa.sun_family = AF_UNIX;
  g_assert_cmpuint(strlen(path), <, sizeof(sa.sun_path));
  g_strlcpy(sa.sun_path, path, sizeof(sa.sun_path));
  g_assert_cmpint(bind(fd, (struct sockaddr *)&sa, sizeof(sa)), ==, 0);
  g_autofree gchar *found = ns_session_relay_socket();
  g_assert_cmpstr(found, ==, path);
  close(fd);
  g_unlink(path);
  g_rmdir(dir);
}

static void
rm_rf(const gchar *path)
{
  GDir *d = g_dir_open(path, 0, NULL);
  if (d != NULL) {
    const gchar *name;
    while ((name = g_dir_read_name(d)) != NULL) {
      g_autofree gchar *child = g_build_filename(path, name, NULL);
      rm_rf(child);
    }
    g_dir_close(d);
    g_rmdir(path);
  } else {
    g_unlink(path);
  }
}

static void
test_url_note_publish(void)
{
  relays_reset();
  const gchar *texts[] = { "look at this", NULL };
  const gchar *args[] = { "https://example.com/post?id=1", "nostr:nevent1qqsabc", NULL };
  GError *err = NULL;
  g_autoptr(NsShare) s = share_new(config("wss://r1.test;wss://r2.test", NULL),
                                   texts, args, NULL, 0, &err);
  g_assert_no_error(err);
  g_assert_cmpuint(s->posts->len, ==, 1);
  NsPost *p = g_ptr_array_index(s->posts, 0);
  g_assert_cmpint(p->action, ==, NS_ACTION_NOTE);

  g_assert_true(ns_share_resolve(s, &err));
  g_assert_no_error(err);
  /* No kind 10002 on the relays → config home_relays are the write set. */
  g_assert_cmpuint(g_strv_length(s->targets.targets), ==, 2);
  g_assert_false(s->targets.session_included);

  g_assert_true(ns_share_build(s, &err));
  g_autoptr(JsonParser) jp = NULL;
  JsonObject *ev = parse_obj(p->unsigned_json, &jp);
  g_assert_cmpint(json_object_get_int_member(ev, "kind"), ==, 1);
  g_assert_cmpstr(json_object_get_string_member(ev, "pubkey"), ==, K.pk);
  const gchar *content = json_object_get_string_member(ev, "content");
  g_assert_nonnull(strstr(content, "look at this"));
  g_assert_nonnull(strstr(content, "nostr:nevent1qqsabc"));   /* NIP-27 untouched */
  g_assert_nonnull(strstr(content, "https://example.com/post?id=1"));
  g_assert_true(has_tag(ev, "r", "https://example.com/post?id=1"));
  g_assert_cmpuint(json_array_get_length(json_object_get_array_member(ev, "tags")), ==, 1);

  g_assert_true(ns_share_publish(s, NULL, NULL, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(R.published->len, ==, 2);
  /* What went out is exactly what was reviewed, plus id/pubkey/sig. */
  NostrEvent *sev = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(sev, p->signed_json, NULL), ==,
                  NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpint(nostr_event_validate(sev, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpstr(nostr_event_get_content(sev), ==, content);
  nostr_event_free(sev);
  g_assert_nonnull(strstr(p->result, "2/2 relays accepted"));
}

static void
test_nip65_verified(void)
{
  relays_reset();
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  can("wss://home.test", 10002,
      signed_event(10002, now - 100, "[[\"r\",\"wss://w1.test\",\"write\"],"
                                     "[\"r\",\"wss://read.test\",\"read\"]]"));
  /* Newer but forged: signed, then tampered. Must be ignored. */
  gchar *forged = signed_event(10002, now, "[[\"r\",\"wss://evil.test\"]]");
  gchar *swap = strstr(forged, "evil");
  memcpy(swap, "evix", 4);
  can("wss://home.test", 10002, forged);

  const gchar *texts[] = { "hello", NULL };
  GError *err = NULL;
  g_autoptr(NsShare) s = share_new(config("wss://home.test", NULL), texts, NULL,
                                   NULL, 0, &err);
  g_assert_no_error(err);
  g_assert_true(ns_share_resolve(s, &err));
  g_assert_cmpuint(g_strv_length(s->targets.targets), ==, 1);
  g_assert_cmpstr(s->targets.targets[0], ==, "wss://w1.test");
  g_assert_nonnull(strstr(s->targets.write_source, "10002"));
}

/* ---- session relay routing (nostrc-t24q) ---- */

static GTestDBus *s_bus;   /* private bus for the fake SessionRelay1 */

static NsShare *
session_share(NsUpstreamMode mode, guint ok_wait_sec)
{
  relays_reset();
  NsConfig *cfg = config("wss://w1.test", NULL);
  cfg->upstream = mode;
  cfg->ok_wait_sec = ok_wait_sec;
  const gchar *texts[] = { "hi", NULL };
  GError *err = NULL;
  NsShare *s = share_new(cfg, texts, NULL, NULL, 0, &err);
  g_assert_no_error(err);
  g_free(s->net.session_socket);
  s->net.session_socket = g_strdup("/run/fake/relay.sock");   /* not a real socket */
  return s;
}

static guint
published_to(const gchar *url_prefix)
{
  guint n = 0;
  for (guint i = 0; i < R.published->len; i++)
    if (g_str_has_prefix(g_ptr_array_index(R.published, i), url_prefix))
      n++;
  return n;
}

/* Without a forwarding session relay (none on the bus here), the modes
 * degrade exactly as documented; no D-Bus daemon needed. */
static void
test_session_relay_rules(void)
{
  GError *err = NULL;

  /* session_relay_and_direct: a local copy, the write relays decide. */
  g_autoptr(NsShare) s = session_share(NS_UPSTREAM_SESSION_RELAY_AND_DIRECT, 5);
  g_assert_true(ns_share_resolve(s, &err));
  g_assert_true(s->targets.session_included);
  g_assert_false(s->targets.session_upstream);
  g_assert_cmpuint(g_strv_length(s->targets.targets), ==, 2);
  g_assert_cmpstr(s->targets.targets[0], ==, NS_SESSION_RELAY_URL);
  g_assert_true(ns_share_build(s, NULL));
  g_hash_table_insert(R.reject, g_strdup("wss://w1.test"), g_strdup("blocked: test"));
  g_assert_false(ns_share_publish(s, NULL, NULL, &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_PUBLISH);   /* session ACK alone ≠ success */
  g_clear_error(&err);

  /* session_relay_or_direct (the default) with a relay that is not on
   * the bus: straight to the write relays, the relay is left alone. */
  g_autoptr(NsShare) d = session_share(NS_UPSTREAM_SESSION_RELAY_OR_DIRECT, 5);
  g_assert_cmpint(d->cfg->upstream, ==, NS_UPSTREAM_SESSION_RELAY_OR_DIRECT);
  g_assert_true(ns_share_resolve(d, &err));
  g_assert_no_error(err);
  g_assert_false(d->targets.session_included);
  g_assert_cmpuint(g_strv_length(d->targets.targets), ==, 1);
  g_assert_cmpstr(d->targets.targets[0], ==, "wss://w1.test");
  g_autofree gchar *desc = ns_share_describe_targets(d);
  g_assert_nonnull(strstr(desc, "session relay not used"));
  g_assert_true(ns_share_build(d, NULL));
  g_assert_true(ns_share_publish(d, NULL, NULL, &err));
  g_assert_cmpuint(published_to(NS_SESSION_RELAY_URL), ==, 0);
  g_assert_cmpuint(published_to("wss://w1.test"), ==, 1);

  /* session_relay_only fails closed: nothing is published anywhere. */
  g_autoptr(NsShare) o = session_share(NS_UPSTREAM_SESSION_RELAY_ONLY, 5);
  g_assert_false(ns_share_resolve(o, &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_NO_RELAYS);
  g_assert_nonnull(strstr(err->message, "FederationState"));
  g_clear_error(&err);
  g_assert_cmpuint(R.published->len, ==, 0);
  g_assert_cmpuint(R.reqs->len, ==, 0);   /* no relay contacted at all */
}

/* Relays that exist on the bus but do not forward: disabled, and a daemon
 * from before FederationState existed. */
static void
test_session_relay_not_forwarding(void)
{
  if (s_bus == NULL) {
    g_test_skip("dbus-daemon not installed: no private bus for the fake session relay");
    return;
  }
  const gchar *states[] = { "disabled", "unavailable", NULL /* old daemon */ };
  for (gsize i = 0; i < G_N_ELEMENTS(states); i++) {
    NpFakeSessionRelay *fake = np_fake_session_relay_start(states[i]);
    GError *err = NULL;
    g_autoptr(NsShare) d = session_share(NS_UPSTREAM_SESSION_RELAY_OR_DIRECT, 5);
    g_assert_true(ns_share_resolve(d, &err));
    g_assert_false(d->targets.session_included);
    g_assert_cmpstr(d->targets.targets[0], ==, "wss://w1.test");
    g_autoptr(NsShare) o = session_share(NS_UPSTREAM_SESSION_RELAY_ONLY, 5);
    g_assert_false(ns_share_resolve(o, &err));
    g_assert_error(err, NS_ERROR, NS_ERROR_NO_RELAYS);
    g_assert_nonnull(strstr(err->message,
                            states[i] ? states[i] : "unsupported"));
    g_clear_error(&err);
    np_fake_session_relay_stop(fake);
  }
}

/* A forwarding relay: the only target, and its upstream report — not its
 * OK — is the verdict. */
static void
test_session_relay_upstream_verdicts(void)
{
  if (s_bus == NULL) {
    g_test_skip("dbus-daemon not installed: no private bus for the fake session relay");
    return;
  }
  GError *err = NULL;
  NpFakeSessionRelay *fake = np_fake_session_relay_start("active");

  /* forwarded straight away */
  const gchar *both[] = { "wss://a.test acked", "wss://b.test acked", NULL };
  np_fake_session_relay_set_reply(fake, "forwarded", "", both);
  g_autoptr(NsShare) s = session_share(NS_UPSTREAM_SESSION_RELAY_OR_DIRECT, 5);
  g_assert_true(ns_share_resolve(s, &err));
  g_assert_true(s->targets.session_upstream);
  g_assert_cmpuint(g_strv_length(s->targets.targets), ==, 1);
  g_assert_cmpstr(s->targets.targets[0], ==, NS_SESSION_RELAY_URL);
  g_assert_cmpuint(R.reqs->len, ==, 0);   /* no 10002 lookup needed */
  g_autofree gchar *desc = ns_share_describe_targets(s);
  g_assert_nonnull(strstr(desc, "forwards to your relays (active)"));
  g_assert_true(ns_share_build(s, NULL));
  g_assert_true(ns_share_publish(s, NULL, NULL, &err));
  g_assert_no_error(err);
  NsPost *p = g_ptr_array_index(s->posts, 0);
  g_assert_nonnull(strstr(p->result, "forwarded by the session relay: 2/2 relays accepted"));
  g_assert_nonnull(strstr(p->result, "wss://b.test: accepted via the session relay"));
  g_autofree gchar *id = ns_event_id_from_signed_json(p->signed_json);
  g_autofree gchar *asked = np_fake_session_relay_last_query(fake);
  g_assert_cmpstr(asked, ==, id);
  g_assert_cmpuint(published_to(NS_SESSION_RELAY_URL), ==, 1);
  g_assert_cmpuint(R.published->len, ==, 1);   /* never the write relays */

  /* pending, then a signal settles it as partial: still published */
  const gchar *pend[] = { "wss://a.test pending", "wss://b.test pending", NULL };
  np_fake_session_relay_set_reply(fake, "pending", "", pend);
  np_fake_session_relay_set_followup(fake, "wss://b.test", "failed", "blocked: nope",
                                     "partial");
  g_autoptr(NsShare) s2 = session_share(NS_UPSTREAM_SESSION_RELAY_ONLY, 5);
  g_assert_true(ns_share_resolve(s2, &err));
  g_assert_true(ns_share_build(s2, NULL));
  g_assert_true(ns_share_publish(s2, NULL, NULL, &err));
  g_assert_no_error(err);
  p = g_ptr_array_index(s2->posts, 0);
  g_assert_nonnull(strstr(p->result, "partial by the session relay: 0/2"));
  g_assert_nonnull(strstr(p->result, "wss://b.test: rejected via the session relay (blocked: nope)"));

  /* final without delivery: skipped (not a local account) */
  np_fake_session_relay_set_followup(fake, NULL, NULL, NULL, NULL);
  np_fake_session_relay_set_reply(fake, "skipped", "author is not a local account", NULL);
  g_autoptr(NsShare) s3 = session_share(NS_UPSTREAM_SESSION_RELAY_OR_DIRECT, 5);
  g_assert_true(ns_share_resolve(s3, &err));
  g_assert_true(ns_share_build(s3, NULL));
  g_assert_false(ns_share_publish(s3, NULL, NULL, &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_PUBLISH);
  g_assert_nonnull(strstr(err->message, "skipped: author is not a local account"));
  g_clear_error(&err);

  /* still unroutable when ok_wait_sec runs out: queued, not failed */
  np_fake_session_relay_set_reply(fake, "unroutable", "no relay list for the author", NULL);
  g_autoptr(NsShare) s4 = session_share(NS_UPSTREAM_SESSION_RELAY_OR_DIRECT, 1);
  g_assert_true(ns_share_resolve(s4, &err));
  g_assert_true(ns_share_build(s4, NULL));
  g_assert_false(ns_share_publish(s4, NULL, NULL, &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_QUEUED);
  g_assert_nonnull(strstr(err->message, "unroutable: no relay list for the author"));
  g_assert_nonnull(strstr(err->message, "sharing again would post it twice"));
  g_clear_error(&err);
  g_assert_cmpuint(R.published->len, ==, 1);   /* handed to the session relay once */

  np_fake_session_relay_stop(fake);
}

static void
test_group_only_group_relay(void)
{
  relays_reset();
  const gchar *texts[] = { "for the group", NULL };
  GError *err = NULL;
  g_autoptr(NsShare) s = share_new(config("wss://w1.test", NULL), texts, NULL,
                                   "groups.test'dev", 0, &err);
  s->net.session_socket = g_strdup("/run/fake/relay.sock");
  g_assert_true(ns_share_resolve(s, &err));
  g_assert_cmpuint(g_strv_length(s->targets.targets), ==, 1);
  g_assert_cmpstr(s->targets.targets[0], ==, "wss://groups.test");
  g_assert_cmpuint(R.reqs->len, ==, 0);   /* no relay-list lookup leaked */
  g_assert_true(ns_share_build(s, NULL));
  NsPost *p = g_ptr_array_index(s->posts, 0);
  g_autoptr(JsonParser) jp = NULL;
  JsonObject *ev = parse_obj(p->unsigned_json, &jp);
  g_assert_true(has_tag(ev, "h", "dev"));
  g_assert_cmpint(json_object_get_int_member(ev, "kind"), ==, 9);   /* NIP-29 chat */
}

static void
test_mention(void)
{
  relays_reset();
  const gchar *texts[] = { "cc", NULL };
  const gchar *hex = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
  GError *err = NULL;
  g_autoptr(NsShare) s = share_new(config("wss://w1.test", NULL), texts, NULL, hex, 0, &err);
  g_assert_no_error(err);
  g_assert_true(ns_share_build(s, NULL));
  NsPost *p = g_ptr_array_index(s->posts, 0);
  g_autoptr(JsonParser) jp = NULL;
  JsonObject *ev = parse_obj(p->unsigned_json, &jp);
  g_assert_true(has_tag(ev, "p", hex));
  g_assert_nonnull(strstr(json_object_get_string_member(ev, "content"), "nostr:npub1"));
}

static const guint8 TINY_JPEG[] = {
  0xFF, 0xD8,
  0xFF, 0xE1, 0x00, 0x0C, 'E', 'x', 'i', 'f', 0, 0, 'G', 'P', 'S', '!',
  0xFF, 0xC0, 0x00, 0x0B, 8, 0x00, 0x10, 0x00, 0x20, 1, 1, 0x11, 0,
  0xFF, 0xDA, 0x00, 0x08, 1, 1, 0, 0, 0x3F, 0,
  0x12, 0x34,
  0xFF, 0xD9,
};

static void
test_media_imeta_and_1063(void)
{
  relays_reset();
  g_autofree gchar *jpg = tmpfile_with("photo.jpg", TINY_JPEG, sizeof(TINY_JPEG));
  const gchar *texts[] = { "sunset", NULL };
  const gchar *args[] = { jpg, NULL };
  GError *err = NULL;
  g_autoptr(NsShare) s = share_new(config("wss://w1.test", "https://blossom.test/"),
                                   texts, args, NULL, 0, &err);
  g_assert_no_error(err);
  NsFile *f = g_ptr_array_index(s->files, 0);
  g_assert_cmpstr(f->blob.mime, ==, "image/jpeg");
  g_assert_true(f->stripped);
  g_assert_cmpuint(f->n_meta_removed, ==, 1);
  g_assert_cmpuint(f->blob.width, ==, 32);
  g_assert_cmpuint(f->blob.height, ==, 16);
  g_assert_cmpuint(f->blob.size, ==, sizeof(TINY_JPEG) - 14);
  g_assert_false(ns_share_metadata_blocked(s, NULL));

  g_assert_true(ns_share_resolve(s, &err));
  g_assert_cmpstr(s->servers[0], ==, "https://blossom.test");
  g_assert_true(ns_share_build(s, &err));
  NsPost *p = g_ptr_array_index(s->posts, 0);
  g_assert_cmpint(p->action, ==, NS_ACTION_MEDIA_NOTE);
  g_autofree gchar *url = g_strdup_printf("https://blossom.test/%s.jpg", f->blob.sha256);
  g_autoptr(JsonParser) jp = NULL;
  JsonObject *ev = parse_obj(p->unsigned_json, &jp);
  g_assert_cmpint(json_object_get_int_member(ev, "kind"), ==, 1);
  g_assert_nonnull(strstr(json_object_get_string_member(ev, "content"), url));
  JsonArray *imeta = json_array_get_array_element(json_object_get_array_member(ev, "tags"), 0);
  g_assert_cmpstr(json_array_get_string_element(imeta, 0), ==, "imeta");
  g_autofree gchar *u = g_strdup_printf("url %s", url);
  g_assert_cmpstr(json_array_get_string_element(imeta, 1), ==, u);
  g_assert_cmpstr(json_array_get_string_element(imeta, 2), ==, "m image/jpeg");
  g_assert_false(has_tag(ev, "r", NULL));   /* no r tag for our own blob URL */

  /* The dialog's picker: kind 1063 */
  g_assert_true(ns_share_set_kind(s, 1063, &err));
  g_assert_true(ns_share_build(s, &err));
  p = g_ptr_array_index(s->posts, 0);
  g_autoptr(JsonParser) jp2 = NULL;
  JsonObject *fm = parse_obj(p->unsigned_json, &jp2);
  g_assert_cmpint(json_object_get_int_member(fm, "kind"), ==, 1063);
  g_assert_true(has_tag(fm, "url", url));
  g_assert_true(has_tag(fm, "x", f->blob.sha256));
  g_assert_true(has_tag(fm, "dim", "32x16"));
  g_assert_cmpstr(json_object_get_string_member(fm, "content"), ==, "sunset");

  g_assert_false(ns_share_set_kind(s, 30023, &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_BAD_KIND);
  g_clear_error(&err);
}

static void
test_unstrippable_media_blocked(void)
{
  relays_reset();
  /* MP3 (ID3 tags) is not handled by the stripper: blocked without
   * --keep-metadata. */
  static const guint8 MP3[] = { 'I', 'D', '3', 3, 0, 0, 0, 0, 0, 10,
                                'T', 'I', 'T', '2', 0, 0, 0, 1, 0, 0,
                                0xFF, 0xFB, 0x90, 0x00 };
  g_autofree gchar *mp3 = tmpfile_with("song.mp3", MP3, sizeof(MP3));
  const gchar *args[] = { mp3, NULL };
  GError *err = NULL;
  g_autoptr(NsShare) s = share_new(config("wss://w1.test", "https://blossom.test"),
                                   NULL, args, NULL, 0, &err);
  g_assert_no_error(err);
  g_autoptr(GString) why = g_string_new(NULL);
  g_assert_true(ns_share_metadata_blocked(s, why));
  g_assert_nonnull(strstr(why->str, "song.mp3"));
  g_assert_false(ns_share_upload(s, NULL, NULL, NULL, &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_METADATA);
  g_clear_error(&err);
  s->keep_metadata = TRUE;
  g_assert_false(ns_share_metadata_blocked(s, NULL));

  /* An MP4 is stripped now (nostrc-wu3s), so one that cannot be walked is
   * refused outright, never uploaded half-checked. */
  g_autofree gchar *mp4 = tmpfile_with("clip.mp4", "\0\0\0\x18" "ftypmp42", 12);
  const gchar *args2[] = { mp4, NULL };
  g_assert_null(share_new(config("wss://w1.test", "https://blossom.test"), NULL, args2,
                          NULL, 0, &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_METADATA);
  g_assert_nonnull(strstr(err->message, "malformed"));
  g_clear_error(&err);
}

static void
test_markdown_article(void)
{
  relays_reset();
  const gchar *doc = "# Why Nostr\n\nBecause https://nostr.com rocks.\n";
  g_autofree gchar *path = tmpfile_with("why.md", doc, strlen(doc));
  const gchar *args[] = { path, NULL };
  GError *err = NULL;
  g_autoptr(NsShare) s = share_new(config("wss://w1.test", NULL), NULL, args, NULL, 0, &err);
  g_assert_no_error(err);
  g_assert_true(ns_share_build(s, &err));
  NsPost *p = g_ptr_array_index(s->posts, 0);
  g_assert_cmpint(p->action, ==, NS_ACTION_ARTICLE);
  g_autoptr(JsonParser) jp = NULL;
  JsonObject *ev = parse_obj(p->unsigned_json, &jp);
  g_assert_cmpint(json_object_get_int_member(ev, "kind"), ==, 30023);
  g_assert_true(has_tag(ev, "d", "why-nostr"));
  g_assert_true(has_tag(ev, "title", "Why Nostr"));
  g_assert_true(has_tag(ev, "published_at", NULL));
  g_assert_cmpstr(json_object_get_string_member(ev, "content"), ==, doc);

  /* published: the source file carries the event id (nostrc-tepd) */
  g_autofree gchar *probe = tmpfile_with("probe", "x", 1);
  gboolean xattrs = FALSE;
  if (ns_share_mark_published(probe, "00")) {
    g_autoptr(GFile) pf = g_file_new_for_path(probe);
    g_autoptr(GFileInfo) pi = g_file_query_info(pf, NS_XATTR_EVENT, G_FILE_QUERY_INFO_NONE, NULL, NULL);
    xattrs = pi && g_strcmp0(g_file_info_get_attribute_string(pi, NS_XATTR_EVENT), "00") == 0;
  }
  g_assert_true(ns_share_resolve(s, &err));
  g_assert_true(ns_share_publish(s, NULL, NULL, &err));
  g_assert_no_error(err);
  g_autofree gchar *id = ns_event_id_from_signed_json(p->signed_json);
  g_assert_nonnull(id);
  g_assert_cmpuint(strlen(id), ==, 64);
  if (xattrs) {
    g_autoptr(GFile) gf = g_file_new_for_path(path);
    g_autoptr(GFileInfo) info = g_file_query_info(gf, NS_XATTR_EVENT, G_FILE_QUERY_INFO_NONE, NULL, &err);
    g_assert_no_error(err);
    g_assert_cmpstr(g_file_info_get_attribute_string(info, NS_XATTR_EVENT), ==, id);
  } else {
    g_test_message("filesystem without user xattrs: nothing to check (best effort)");
  }
  g_assert_false(ns_share_mark_published("/nonexistent/file", id)); /* silent failure */
  /* an edited post is not the file: no tag */
  const gchar *doc2 = "# Draft\n\nfirst words\n";
  g_autofree gchar *path2 = tmpfile_with("draft.md", doc2, strlen(doc2));
  const gchar *args2[] = { path2, NULL };
  g_autoptr(NsShare) s3 = share_new(config("wss://w1.test", NULL), NULL, args2, NULL, 0, &err);
  g_assert_true(ns_share_set_text(s3, "# Draft\n\nrewritten in the dialog\n", &err));
  g_assert_true(ns_share_build(s3, &err));
  g_assert_true(ns_share_resolve(s3, &err));
  g_assert_true(ns_share_publish(s3, NULL, NULL, &err));
  g_autoptr(GFile) gf2 = g_file_new_for_path(path2);
  g_autoptr(GFileInfo) info2 = g_file_query_info(gf2, NS_XATTR_EVENT, G_FILE_QUERY_INFO_NONE, NULL, NULL);
  g_assert_null(info2 ? g_file_info_get_attribute_string(info2, NS_XATTR_EVENT) : NULL);
  g_assert_null(ns_event_id_from_signed_json("{\"id\":\"xyz\"}"));
  g_assert_null(ns_event_id_from_signed_json("not json"));

  /* long plain text stays a kind-1 note (no replaceable semantics) unless
   * the user asks for an article */
  GString *lng = g_string_new(NULL);
  while (lng->len <= 20000)
    g_string_append(lng, "lorem ipsum ");
  const gchar *texts[] = { lng->str, NULL };
  g_autoptr(NsShare) s2 = share_new(config("wss://w1.test", NULL), texts, NULL, NULL, 0, &err);
  g_assert_cmpint(((NsPost *)g_ptr_array_index(s2->posts, 0))->action, ==, NS_ACTION_NOTE);
  g_assert_true(ns_share_set_kind(s2, 30023, &err));
  g_assert_cmpint(((NsPost *)g_ptr_array_index(s2->posts, 0))->action, ==, NS_ACTION_ARTICLE);
  g_string_free(lng, TRUE);
}

static void
test_calendar_to_dav(void)
{
  relays_reset();
  const gchar *ics = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:abc-123@host\r\n"
                     "SUMMARY:Meetup\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
  g_autofree gchar *path = tmpfile_with("meetup.ics", ics, strlen(ics));
  const gchar *args[] = { path, NULL };
  GError *err = NULL;
  g_autoptr(NsShare) s = share_new(config(NULL, NULL), NULL, args, NULL, 0, &err);
  g_assert_no_error(err);
  NsPost *p = g_ptr_array_index(s->posts, 0);
  g_assert_cmpint(p->action, ==, NS_ACTION_DAV_CALENDAR);
  g_assert_false(ns_share_needs_relays(s));
  g_assert_true(ns_share_build(s, &err));
  g_assert_null(p->unsigned_json);

  g_autofree gchar *url = NULL;
  g_assert_true(ns_dav_stage(NULL, NS_CLASS_CALENDAR, ((NsFile *)g_ptr_array_index(p->files, 0))->bytes,
                             TRUE, &url, &err));
  g_assert_cmpstr(url, ==, "http://127.0.0.1:7680/calendars/nostr/abc-123@host.ics");

  g_autofree gchar *uid = ns_dav_extract_uid("BEGIN:VCARD\nUID;VALUE=text:../../etc/x\nEND:VCARD");
  g_assert_cmpstr(uid, ==, ".._.._etc_x");   /* no "/" survives */

  const gchar *two = "BEGIN:VCALENDAR\nBEGIN:VEVENT\nUID:a\nEND:VEVENT\n"
                    "BEGIN:VEVENT\nUID:b\nEND:VEVENT\n"
                    "BEGIN:VEVENT\nUID:a\nRECURRENCE-ID:20260101T000000Z\nEND:VEVENT\n"
                    "END:VCALENDAR\n";
  g_assert_false(ns_dav_check_single(NS_CLASS_CALENDAR, two, &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_DAV);
  g_assert_nonnull(strstr(err->message, "2 separate events"));
  g_clear_error(&err);
  g_assert_true(ns_dav_check_single(NS_CLASS_CALENDAR, ics, NULL));
  g_assert_false(ns_dav_check_single(NS_CLASS_CONTACT,
                                     "BEGIN:VCARD\nEND:VCARD\nBEGIN:VCARD\nEND:VCARD\n", NULL));

  g_assert_null(ns_share_new(config(NULL, NULL),
                             &(NsShareOptions){ .forced_kind = 1,
                                                .args = (const gchar *const[]){ path, NULL } },
                             &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_BAD_KIND);
  g_clear_error(&err);
}

static void
test_git_public_urls(void)
{
  const gchar *remotes[] = {
    "https://user:ghp_secret@github.com/a/b.git",
    "git@github.com:a/b.git",
    "ssh://git@host.example/x",
    "/home/me/src/b",
    "file:///srv/b.git",
    "../sibling",
    "https://github.com/a/b.git",
    NULL
  };
  g_autoptr(GPtrArray) clone = g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GPtrArray) web = g_ptr_array_new_with_free_func(g_free);
  ns_git_public_urls(remotes, clone, web);
  g_assert_cmpuint(clone->len, ==, 3);
  g_assert_cmpstr(g_ptr_array_index(clone, 0), ==, "https://github.com/a/b.git");
  g_assert_cmpstr(g_ptr_array_index(clone, 1), ==, "git@github.com:a/b.git");
  g_assert_cmpstr(g_ptr_array_index(clone, 2), ==, "ssh://git@host.example/x");
  g_assert_cmpuint(web->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(web, 0), ==, "https://github.com/a/b");
  for (guint i = 0; i < clone->len; i++)
    g_assert_null(strstr(g_ptr_array_index(clone, i), "secret"));
}

static void
test_git_repo_announcement(void)
{
  g_autofree gchar *git = g_find_program_in_path("git");
  if (git == NULL) {
    g_test_skip("git not installed");
    return;
  }
  relays_reset();
  g_autofree gchar *dir = g_dir_make_tmp("ns-repo-XXXXXX", NULL);
  g_autofree gchar *repo = g_build_filename(dir, "My Repo", NULL);
  g_mkdir(repo, 0700);
  const gchar *cmds[][12] = {
    { "git", "-C", repo, "init", "-q", NULL },
    { "git", "-C", repo, "remote", "add", "origin",
      "https://tok@codeberg.org/me/my-repo.git", NULL },
    { "git", "-C", repo, "-c", "user.name=t", "-c", "user.email=t@t", "commit",
      "-q", "--allow-empty", "-m", NULL },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(cmds); i++) {
    const gchar *argv[14];
    gsize n = 0;
    for (; cmds[i][n]; n++) argv[n] = cmds[i][n];
    if (i == 2) argv[n++] = "init";
    argv[n] = NULL;
    gint status = 0;
    g_assert_true(g_spawn_sync(NULL, (gchar **)argv, NULL,
                               G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                               G_SPAWN_STDERR_TO_DEV_NULL,
                               NULL, NULL, NULL, NULL, &status, NULL));
    g_assert_true(g_spawn_check_wait_status(status, NULL));
  }

  const gchar *args[] = { repo, NULL };
  GError *err = NULL;
  g_autoptr(NsShare) s = share_new(config("wss://w1.test", NULL), NULL, args, NULL, 0, &err);
  g_assert_no_error(err);
  g_assert_true(ns_share_resolve(s, &err));
  g_assert_true(ns_share_build(s, &err));
  g_assert_no_error(err);
  NsPost *p = g_ptr_array_index(s->posts, 0);
  g_assert_cmpint(p->action, ==, NS_ACTION_GIT_REPO);
  g_autoptr(JsonParser) jp = NULL;
  JsonObject *ev = parse_obj(p->unsigned_json, &jp);
  g_assert_cmpint(json_object_get_int_member(ev, "kind"), ==, 30617);
  g_assert_true(has_tag(ev, "d", "my-repo"));
  g_assert_true(has_tag(ev, "name", "My Repo"));
  g_assert_true(has_tag(ev, "clone", "https://codeberg.org/me/my-repo.git"));
  g_assert_true(has_tag(ev, "web", "https://codeberg.org/me/my-repo"));
  g_assert_true(has_tag(ev, "relays", "wss://w1.test"));
  g_assert_true(has_tag(ev, "r", NULL));   /* euc */
  g_assert_null(strstr(p->unsigned_json, "tok@"));
}

/* ---- private shares: NIP-17 over NIP-59 (nostrc-k95e) ---- */

typedef struct { gchar *sk, *pk; } Who;
static Who REC;   /* the recipient */

static NsShare *
private_share(const gchar *const *texts, const gchar *const *args, const gchar *servers,
              GError **error)
{
  NsShareOptions o = { .to = REC.pk, .texts = texts, .args = args, .private_share = TRUE };
  NsShare *s = ns_share_new(config("wss://home.test", servers), &o, error);
  if (s == NULL)
    return NULL;
  s->net.factory = fixture_factory;
  NostrPublishSignerVTable vt = { mock_sign, NULL };
  s->signer = nostr_publish_signer_new_from_vtable(&vt, NULL);
  nostr_publish_signer_set_nip44_encrypt(s->signer, mock_nip44);
  s->pubkey_hex = g_strdup(K.pk);
  return s;
}

/* The recipient's kind 10050 (one entry is not a relay), and ours. */
static void
can_inboxes(gboolean mine)
{
  can("wss://home.test", 10050,
      signed_event_by(REC.sk, REC.pk, 10050,
                      "[[\"relay\",\"wss://inbox1.test\"],[\"relay\",\"wss://inbox2.test\"],"
                      "[\"relay\",\"https://not-a-relay.test\"],[\"relay\",\"wss://inbox1.test\"]]"));
  if (mine)
    can("wss://home.test", 10050,
        signed_event(10050, g_get_real_time() / G_USEC_PER_SEC - 60,
                     "[[\"relay\",\"wss://mine.test\"]]"));
}

static guint
published_count(const gchar *url)
{
  g_autofree gchar *prefix = g_strdup_printf("%s ", url);
  return published_to(prefix);
}

static gchar *
published_json(const gchar *url)
{
  g_autofree gchar *prefix = g_strdup_printf("%s ", url);
  for (guint i = 0; i < R.published->len; i++) {
    const gchar *e = g_ptr_array_index(R.published, i);
    if (g_str_has_prefix(e, prefix))
      return g_strdup(e + strlen(prefix));
  }
  return NULL;
}

static JsonArray *
tags_of(JsonParser **keep, const gchar *json)
{
  return json_object_get_array_member(parse_obj(json, keep), "tags");
}

/* Strictly in the past, within two days: never the send time (which is
 * what nips/nip59 produced before nostrc-rd8j). */
static void
assert_randomised(gint64 created_at)
{
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_assert_cmpint(created_at, <, now);
  g_assert_cmpint(created_at, >=, now - 2 * 24 * 3600 - 5);
}

/* Open @wrap_json as @reader and return the rumor; checks every NIP-59 /
 * NIP-17 property of the layers on the way. @out_wrap_pk: the throwaway
 * key it came from. */
static gchar *
open_wrap(const gchar *wrap_json, const gchar *reader_sk, const gchar *reader_pk,
          gchar **out_wrap_pk)
{
  NostrEvent *wrap = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(wrap, wrap_json, NULL), ==,
                  NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpint(nostr_event_validate(wrap, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpint(nostr_event_get_kind(wrap), ==, 1059);
  g_assert_cmpstr(nostr_event_get_pubkey(wrap), !=, K.pk);     /* not the sender */
  g_assert_cmpstr(nostr_event_get_pubkey(wrap), !=, reader_pk);
  assert_randomised(nostr_event_get_created_at(wrap));
  g_autoptr(JsonParser) wp = NULL;
  JsonArray *wt = tags_of(&wp, wrap_json);
  g_assert_cmpuint(json_array_get_length(wt), ==, 1);           /* only ["p", reader] */
  JsonArray *pt = json_array_get_array_element(wt, 0);
  g_assert_cmpstr(json_array_get_string_element(pt, 0), ==, "p");
  g_assert_cmpstr(json_array_get_string_element(pt, 1), ==, reader_pk);
  if (out_wrap_pk)
    *out_wrap_pk = g_strdup(nostr_event_get_pubkey(wrap));

  NostrEvent *seal = nostr_nip59_unwrap(wrap, reader_sk);
  g_assert_nonnull(seal);
  g_assert_cmpint(nostr_event_validate(seal, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpint(nostr_event_get_kind(seal), ==, 13);
  g_assert_cmpstr(nostr_event_get_pubkey(seal), ==, K.pk);      /* signed by the sender */
  assert_randomised(nostr_event_get_created_at(seal));
  char *seal_json = nostr_event_serialize_compact(seal);
  g_autoptr(JsonParser) sp = NULL;
  g_assert_cmpuint(json_array_get_length(tags_of(&sp, seal_json)), ==, 0);  /* no tags */
  free(seal_json);

  guint8 rsk[32], spk[32];
  hex32(reader_sk, rsk);
  hex32(K.pk, spk);
  guint8 *plain = NULL;
  size_t plain_len = 0;
  g_assert_cmpint(nostr_nip44_decrypt_v2(rsk, spk, nostr_event_get_content(seal), &plain,
                                         &plain_len), ==, 0);
  gchar *rumor = g_strndup((const gchar *)plain, plain_len);
  free(plain);
  nostr_event_free(seal);
  nostr_event_free(wrap);

  NostrEvent *r = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_unsigned(r, rumor, NULL), ==,
                  NOSTR_EVENT_VALIDATION_OK);                     /* unsigned: no sig */
  gchar id[65];
  g_assert_cmpint(nostr_event_validate_id(r, id), ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpstr(nostr_event_get_pubkey(r), ==, K.pk);         /* seal pubkey == rumor pubkey */
  nostr_event_free(r);
  return rumor;
}

static void
test_private_text(void)
{
  relays_reset();
  can_inboxes(TRUE);
  const gchar *texts[] = { "just for you https://example.com/x", NULL };
  GError *err = NULL;
  g_autoptr(NsShare) s = private_share(texts, NULL, NULL, &err);
  g_assert_no_error(err);
  g_assert_cmpuint(s->posts->len, ==, 1);
  NsPost *p = g_ptr_array_index(s->posts, 0);
  g_assert_cmpint(p->action, ==, NS_ACTION_PRIVATE_MESSAGE);
  g_assert_false(ns_share_needs_upload(s));

  g_assert_true(ns_share_resolve(s, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(g_strv_length(s->targets.targets), ==, 2);   /* inbox only, deduplicated */
  g_assert_cmpstr(s->targets.targets[0], ==, "wss://inbox1.test");
  g_assert_cmpstr(s->targets.targets[1], ==, "wss://inbox2.test");
  g_assert_cmpstr(s->inbox_self.relays[0], ==, "wss://mine.test");
  g_autofree gchar *desc = ns_share_describe_targets(s);
  g_assert_nonnull(strstr(desc, "private to npub1"));
  g_assert_nonnull(strstr(desc, "wss://inbox2.test"));

  g_assert_true(ns_share_build(s, &err));
  g_autoptr(JsonParser) rp = NULL;
  JsonObject *rumor = parse_obj(p->unsigned_json, &rp);
  g_assert_cmpint(json_object_get_int_member(rumor, "kind"), ==, 14);
  g_assert_cmpstr(json_object_get_string_member(rumor, "pubkey"), ==, K.pk);
  g_assert_false(json_object_has_member(rumor, "sig"));
  g_assert_cmpstr(json_object_get_string_member(rumor, "content"), ==, texts[0]);
  g_assert_cmpuint(json_array_get_length(json_object_get_array_member(rumor, "tags")), ==, 1);
  g_assert_true(has_tag(rumor, "p", REC.pk));          /* no public mention, no r tag */
  g_autofree gchar *preview = ns_share_preview_json(s, FALSE);
  g_assert_nonnull(strstr(preview, "// private (NIP-17)"));

  gboolean published = ns_share_publish(s, NULL, NULL, &err);
  g_assert_no_error(err);
  g_assert_true(published);
  /* The recipient's wrap to their inbox relays, our copy to ours; not a
   * byte to the write/home relay. */
  g_assert_cmpuint(published_count("wss://inbox1.test"), ==, 1);
  g_assert_cmpuint(published_count("wss://inbox2.test"), ==, 1);
  g_assert_cmpuint(published_count("wss://mine.test"), ==, 1);
  g_assert_cmpuint(published_count("wss://home.test"), ==, 0);
  g_assert_cmpuint(R.published->len, ==, 3);
  g_assert_nonnull(strstr(p->result, "2/2 inbox relays accepted"));
  g_assert_nonnull(strstr(p->result, "copy to your own inbox: sent"));

  g_autofree gchar *to_wrap = published_json("wss://inbox1.test");
  g_autofree gchar *to_wrap2 = published_json("wss://inbox2.test");
  g_autofree gchar *wrap_pk = NULL, *self_wrap_pk = NULL;
  g_autofree gchar *got = open_wrap(to_wrap, REC.sk, REC.pk, &wrap_pk);
  g_autoptr(JsonParser) gp = NULL;
  JsonObject *g = parse_obj(got, &gp);
  g_assert_cmpstr(json_object_get_string_member(g, "id"), ==,
                  json_object_get_string_member(rumor, "id"));
  g_assert_cmpstr(json_object_get_string_member(g, "content"), ==, texts[0]);
  g_autofree gchar *wrap_id = ns_event_id_from_signed_json(to_wrap);
  g_autofree gchar *wrap_id2 = ns_event_id_from_signed_json(to_wrap2);
  g_autofree gchar *out_id = ns_event_id_from_signed_json(p->signed_json);
  g_assert_cmpstr(wrap_id, ==, wrap_id2);          /* one wrap, two relays */
  g_assert_cmpstr(out_id, ==, wrap_id);             /* stdout shows what went out */

  g_autofree gchar *mine = published_json("wss://mine.test");
  g_autofree gchar *mine_rumor = open_wrap(mine, K.sk, K.pk, &self_wrap_pk);
  g_autoptr(JsonParser) mp = NULL;
  g_assert_cmpstr(json_object_get_string_member(parse_obj(mine_rumor, &mp), "id"), ==,
                  json_object_get_string_member(rumor, "id"));
  g_assert_cmpstr(wrap_pk, !=, self_wrap_pk);       /* a fresh throwaway key per wrap */
}

static void
test_private_needs_inbox(void)
{
  relays_reset();   /* the recipient has no kind 10050 anywhere */
  const gchar *texts[] = { "hello?", NULL };
  GError *err = NULL;
  g_autoptr(NsShare) s = private_share(texts, NULL, NULL, &err);
  g_assert_false(ns_share_resolve(s, &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_NO_RELAYS);
  g_assert_nonnull(strstr(err->message, "kind-10050"));
  g_assert_nonnull(strstr(err->message, "Nothing was uploaded or sent"));
  g_clear_error(&err);
  g_assert_cmpuint(R.published->len, ==, 0);
  /* Nor does publishing fall back to anything. */
  g_assert_false(ns_share_publish(s, NULL, NULL, &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_NO_RELAYS);
  g_clear_error(&err);
  g_assert_cmpuint(R.published->len, ==, 0);
}

static void
test_private_refusals(void)
{
  const gchar *texts[] = { "x", NULL };
  GError *err = NULL;
  NsShareOptions none = { .texts = texts, .private_share = TRUE };
  g_assert_null(ns_share_new(config("wss://w1.test", NULL), &none, &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_BAD_INPUT);
  g_assert_nonnull(strstr(err->message, "--private needs --to"));
  g_clear_error(&err);
  NsShareOptions group = { .texts = texts, .to = "groups.test'dev", .private_share = TRUE };
  g_assert_null(ns_share_new(config("wss://w1.test", NULL), &group, &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_BAD_INPUT);
  g_clear_error(&err);
  NsShareOptions kind = { .texts = texts, .to = REC.pk, .forced_kind = 1, .private_share = TRUE };
  g_assert_null(ns_share_new(config("wss://w1.test", NULL), &kind, &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_BAD_KIND);
  g_clear_error(&err);

  const gchar *ics = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:p@h\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
  g_autofree gchar *path = tmpfile_with("secret.ics", ics, strlen(ics));
  const gchar *args[] = { path, NULL };
  NsShareOptions cal = { .args = args, .to = REC.pk, .private_share = TRUE };
  g_assert_null(ns_share_new(config("wss://w1.test", NULL), &cal, &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_BAD_INPUT);
  g_assert_nonnull(strstr(err->message, "cannot be shared privately"));
  g_clear_error(&err);

  /* The dialog's switch refuses the same, leaving the share public. */
  g_autoptr(NsShare) pub = share_new(config("wss://w1.test", NULL), NULL, args, REC.pk, 0, &err);
  g_assert_no_error(err);
  g_assert_false(ns_share_set_private(pub, TRUE, &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_BAD_INPUT);
  g_clear_error(&err);
  g_assert_false(pub->private_share);
  g_assert_cmpint(((NsPost *)g_ptr_array_index(pub->posts, 0))->action, ==,
                  NS_ACTION_DAV_CALENDAR);
}

static void
test_private_file(void)
{
  relays_reset();
  can_inboxes(FALSE);
  g_autofree gchar *jpg = tmpfile_with("photo.jpg", TINY_JPEG, sizeof(TINY_JPEG));
  const gchar *texts[] = { "look", NULL };
  const gchar *args[] = { jpg, NULL };
  GError *err = NULL;
  g_autoptr(NsShare) s = private_share(texts, args, "https://blossom.test", &err);
  g_assert_no_error(err);
  g_assert_cmpuint(s->posts->len, ==, 2);              /* caption message + file message */
  g_assert_cmpint(((NsPost *)g_ptr_array_index(s->posts, 0))->action, ==,
                  NS_ACTION_PRIVATE_MESSAGE);
  NsPost *fp = g_ptr_array_index(s->posts, 1);
  g_assert_cmpint(fp->action, ==, NS_ACTION_PRIVATE_FILE);
  g_assert_true(ns_share_needs_upload(s));

  /* Stripped first, then AES-256-GCM: what would be uploaded opens to the
   * cleaned bytes with the rumor's key, and only with it. */
  NsFile *f = g_ptr_array_index(s->files, 0);
  g_assert_true(f->stripped);
  g_assert_nonnull(f->sealed);
  g_assert_cmpuint(g_bytes_get_size(f->sealed), ==, g_bytes_get_size(f->bytes) + 16);
  g_autoptr(GBytes) back = ns_private_decrypt_file(f->sealed, &f->file_key, &err);
  g_assert_no_error(err);
  g_assert_true(g_bytes_equal(back, f->bytes));
  gsize n = g_bytes_get_size(f->sealed);
  guint8 *tampered = g_memdup2(g_bytes_get_data(f->sealed, NULL), n);
  tampered[3] ^= 1;
  g_autoptr(GBytes) bad = g_bytes_new_take(tampered, n);
  g_assert_null(ns_private_decrypt_file(bad, &f->file_key, &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_BAD_INPUT);
  g_clear_error(&err);

  g_assert_true(ns_share_resolve(s, &err));
  g_assert_no_error(err);
  g_assert_true(ns_share_build(s, &err));
  g_assert_no_error(err);
  g_autoptr(JsonParser) jp = NULL;
  JsonObject *r = parse_obj(fp->unsigned_json, &jp);
  g_assert_cmpint(json_object_get_int_member(r, "kind"), ==, 15);
  g_autofree gchar *url = g_strdup_printf("https://blossom.test/%s", f->sealed_blob.sha256);
  g_assert_cmpstr(json_object_get_string_member(r, "content"), ==, url);  /* no .jpg */
  g_assert_true(has_tag(r, "p", REC.pk));
  g_assert_true(has_tag(r, "file-type", "image/jpeg"));
  g_assert_true(has_tag(r, "encryption-algorithm", "aes-gcm"));
  g_assert_true(has_tag(r, "x", f->sealed_blob.sha256));
  g_assert_true(has_tag(r, "ox", f->blob.sha256));
  g_assert_cmpstr(f->sealed_blob.sha256, !=, f->blob.sha256);
  g_assert_true(has_tag(r, "dim", "32x16"));
  GString *k = g_string_new(NULL);
  for (int i = 0; i < 32; i++)
    g_string_append_printf(k, "%02x", f->file_key.key[i]);
  g_assert_true(has_tag(r, "decryption-key", k->str));
  g_string_free(k, TRUE);
  JsonArray *tags = json_object_get_array_member(r, "tags");
  for (guint i = 0; i < json_array_get_length(tags); i++) {
    JsonArray *t = json_array_get_array_element(tags, i);
    if (g_str_equal(json_array_get_string_element(t, 0), "decryption-nonce"))
      g_assert_cmpuint(strlen(json_array_get_string_element(t, 1)), ==, 24);
  }
}

/* ---- a fake Blossom server (BUD-02 PUT /upload) on 127.0.0.1 ---- */

typedef struct {
  SoupServer   *server;
  GThread      *thread;
  GMainContext *ctx;
  GMainLoop    *loop;
  GMutex        lock;
  GCond         cond;
  gboolean      ready;
  gchar        *url;          /* http://127.0.0.1:<port> */
  gchar        *require_pk;   /* NULL: any valid auth */
  GPtrArray    *auth_pks;     /* pubkeys that authorised an attempt */
  GBytes       *body;         /* last accepted upload */
  gchar        *content_type;
} FakeBlossom;

static void
blossom_upload(SoupServer *srv, SoupServerMessage *msg, const char *path, GHashTable *q,
               gpointer ud)
{
  (void)srv; (void)path; (void)q;
  FakeBlossom *b = ud;
  const char *auth = soup_message_headers_get_one(soup_server_message_get_request_headers(msg),
                                                  "Authorization");
  gchar *pk = NULL;
  if (auth != NULL && g_str_has_prefix(auth, "Nostr ")) {
    gsize n = 0;
    g_autofree guchar *json = g_base64_decode(auth + 6, &n);
    g_autofree gchar *s = g_strndup((const gchar *)json, n);
    NostrEvent *ev = nostr_event_new();
    if (nostr_event_deserialize_compact(ev, s, NULL) == 1 &&
        nostr_event_validate(ev, NULL) == NOSTR_EVENT_VALIDATION_OK &&
        nostr_event_get_kind(ev) == 24242)
      pk = g_strdup(nostr_event_get_pubkey(ev));
    nostr_event_free(ev);
  }
  g_mutex_lock(&b->lock);
  if (pk != NULL)
    g_ptr_array_add(b->auth_pks, g_strdup(pk));
  gboolean allowed = pk != NULL && (b->require_pk == NULL || g_str_equal(pk, b->require_pk));
  if (allowed) {
    g_autoptr(GBytes) body = soup_message_body_flatten(soup_server_message_get_request_body(msg));
    g_clear_pointer(&b->body, g_bytes_unref);
    b->body = g_bytes_ref(body);
    g_free(b->content_type);
    b->content_type = g_strdup(soup_message_headers_get_content_type(
      soup_server_message_get_request_headers(msg), NULL));
    gsize n = 0;
    const guint8 *d = g_bytes_get_data(body, &n);
    g_autofree gchar *sha = ns_sha256_hex(d, n);
    g_autofree gchar *reply = g_strdup_printf(
      "{\"url\":\"%s/%s\",\"sha256\":\"%s\",\"size\":%zu,\"type\":\"application/octet-stream\"}",
      b->url, sha, sha, n);
    soup_server_message_set_status(msg, 200, NULL);
    soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY, reply,
                                     strlen(reply));
  } else {
    soup_server_message_set_status(msg, 401, NULL);
  }
  g_mutex_unlock(&b->lock);
  g_free(pk);
}

static gpointer
blossom_thread(gpointer p)
{
  FakeBlossom *b = p;
  g_main_context_push_thread_default(b->ctx);
  b->server = soup_server_new(NULL, NULL);
  soup_server_add_handler(b->server, "/upload", blossom_upload, b, NULL);
  g_assert_true(soup_server_listen_local(b->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, NULL));
  GSList *uris = soup_server_get_uris(b->server);
  g_mutex_lock(&b->lock);
  b->url = g_strdup_printf("http://127.0.0.1:%d", g_uri_get_port(uris->data));
  b->ready = TRUE;
  g_cond_signal(&b->cond);
  g_mutex_unlock(&b->lock);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  g_main_loop_run(b->loop);
  soup_server_disconnect(b->server);
  g_clear_object(&b->server);
  g_main_context_pop_thread_default(b->ctx);
  return NULL;
}

static FakeBlossom *
blossom_start(const gchar *require_pk)
{
  FakeBlossom *b = g_new0(FakeBlossom, 1);
  g_mutex_init(&b->lock);
  g_cond_init(&b->cond);
  b->require_pk = g_strdup(require_pk);
  b->auth_pks = g_ptr_array_new_with_free_func(g_free);
  b->ctx = g_main_context_new();
  b->loop = g_main_loop_new(b->ctx, FALSE);
  b->thread = g_thread_new("fake-blossom", blossom_thread, b);
  g_mutex_lock(&b->lock);
  while (!b->ready)
    g_cond_wait(&b->cond, &b->lock);
  g_mutex_unlock(&b->lock);
  return b;
}

static void
blossom_stop(FakeBlossom *b)
{
  g_main_loop_quit(b->loop);
  g_thread_join(b->thread);
  g_main_loop_unref(b->loop);
  g_main_context_unref(b->ctx);
  g_ptr_array_unref(b->auth_pks);
  g_clear_pointer(&b->body, g_bytes_unref);
  g_free(b->content_type);
  g_free(b->require_pk);
  g_free(b->url);
  g_mutex_clear(&b->lock);
  g_cond_clear(&b->cond);
  g_free(b);
}

/* Private uploads are authorised by a throwaway key; only a server that
 * refuses it gets the account key — unless private_blob_auth forbids. */
static void
test_private_upload_unlinked(void)
{
  static const struct { gboolean strict, throwaway_only; } cases[] = {
    { FALSE, FALSE }, { TRUE, FALSE }, { TRUE, TRUE },
  };
  for (gsize c = 0; c < G_N_ELEMENTS(cases); c++) {
    relays_reset();
    can_inboxes(FALSE);
    FakeBlossom *b = blossom_start(cases[c].strict ? K.pk : NULL);
    g_autofree gchar *jpg = tmpfile_with("photo.jpg", TINY_JPEG, sizeof(TINY_JPEG));
    const gchar *args[] = { jpg, NULL };
    GError *err = NULL;
    g_autoptr(NsShare) s = private_share(NULL, args, b->url, &err);
    g_assert_no_error(err);
    s->cfg->allow_loopback_relays = TRUE;          /* the fake server is http://127.0.0.1 */
    s->cfg->private_blob_throwaway_only = cases[c].throwaway_only;
    NsFile *f = g_ptr_array_index(s->files, 0);
    gboolean changed = FALSE;
    gboolean ok = ns_share_upload(s, &changed, NULL, NULL, &err);
    g_mutex_lock(&b->lock);
    g_assert_cmpuint(b->auth_pks->len, >=, 1);
    const gchar *first = g_ptr_array_index(b->auth_pks, 0);
    g_assert_cmpstr(first, !=, K.pk);               /* tried unlinked first */
    if (cases[c].throwaway_only) {
      g_assert_false(ok);
      g_assert_error(err, NS_ERROR, NS_ERROR_UPLOAD);
      g_assert_nonnull(strstr(err->message, "throwaway_only"));
      g_clear_error(&err);
      g_assert_cmpuint(b->auth_pks->len, ==, 1);    /* never as the user */
      g_assert_null(b->body);
    } else {
      g_assert_no_error(err);
      g_assert_true(ok);
      g_assert_true(f->sealed_uploaded);
      g_assert_true(g_bytes_equal(b->body, f->sealed));   /* only ciphertext left */
      g_assert_cmpstr(b->content_type, ==, "application/octet-stream");
      g_assert_cmpint(f->sealed_upload_linked, ==, cases[c].strict);
      if (cases[c].strict) {
        g_assert_cmpuint(b->auth_pks->len, ==, 2);
        g_assert_cmpstr(g_ptr_array_index(b->auth_pks, 1), ==, K.pk);
      } else {
        g_assert_cmpuint(b->auth_pks->len, ==, 1);
      }
      g_autofree gchar *want = g_strdup_printf("%s/%s", b->url, f->sealed_blob.sha256);
      g_assert_cmpstr(f->sealed_blob.url, ==, want);
    }
    g_mutex_unlock(&b->lock);
    blossom_stop(b);
  }
}

/* Other people's relay lists are untrusted input (Oracle review). */
static void
test_private_relay_policy(void)
{
  static const struct { const gchar *url; gboolean ok, ok_loopback; } T[] = {
    { "wss://relay.example.com", TRUE, TRUE },
    { "wss://relay.example.com:4443/inbox", TRUE, TRUE },
    { "ws://relay.example.com", FALSE, FALSE },           /* plaintext: path observers */
    { "ws://localhost/", FALSE, FALSE },                  /* = relay.sock */
    { "wss://localhost", FALSE, FALSE },
    { "wss://foo.localhost", FALSE, FALSE },
    { "ws://127.0.0.1:7777", FALSE, TRUE },
    { "wss://127.0.0.1", FALSE, TRUE },
    { "wss://[::1]:8080", FALSE, TRUE },
    { "wss://10.0.0.5", FALSE, FALSE },
    { "wss://192.168.1.2", FALSE, FALSE },
    { "wss://169.254.1.1", FALSE, FALSE },
    { "wss://[fd00::1]", FALSE, FALSE },
    { "wss://0.0.0.0", FALSE, FALSE },
    { "wss://printer.local", FALSE, FALSE },
    { "wss://user:pw@relay.example.com", FALSE, FALSE },
    { "https://relay.example.com", FALSE, FALSE },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(T); i++) {
    g_test_message("%s", T[i].url);
    g_assert_cmpint(ns_private_remote_relay_ok(T[i].url, FALSE), ==, T[i].ok);
    g_assert_cmpint(ns_private_remote_relay_ok(T[i].url, TRUE), ==, T[i].ok_loopback);
  }

  /* A hostile kind 10050: only the public wss:// entries survive, capped. */
  GString *tags = g_string_new("[[\"relay\",\"ws://localhost/\"],[\"relay\",\"ws://127.0.0.1:80\"],"
                               "[\"relay\",\"ws://plain.example.com\"]");
  for (int i = 0; i < 12; i++)
    g_string_append_printf(tags, ",[\"relay\",\"wss://r%d.example.com\"]", i);
  g_string_append(tags, "]");
  g_autofree gchar *ev = signed_event_by(REC.sk, REC.pk, 10050, tags->str);
  g_string_free(tags, TRUE);
  guint dropped = 0;
  g_auto(GStrv) relays = ns_private_inbox_relays(ev, FALSE, &dropped);
  g_assert_cmpuint(g_strv_length(relays), ==, NS_PRIVATE_MAX_INBOX_RELAYS);
  g_assert_cmpstr(relays[0], ==, "wss://r0.example.com");
  g_assert_cmpuint(dropped, ==, 3 + 12 - NS_PRIVATE_MAX_INBOX_RELAYS);

  /* Nothing usable: refused, and the message says why. */
  relays_reset();
  can("wss://home.test", 10050, signed_event_by(REC.sk, REC.pk, 10050,
                                                "[[\"relay\",\"ws://localhost/\"]]"));
  const gchar *texts[] = { "x", NULL };
  GError *err = NULL;
  g_autoptr(NsShare) s = private_share(texts, NULL, NULL, &err);
  g_assert_false(ns_share_resolve(s, &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_NO_RELAYS);
  g_assert_nonnull(strstr(err->message, "wss:// to a public host only"));
  g_clear_error(&err);
  g_assert_cmpuint(R.published->len, ==, 0);
}

/* A file a failed public attempt uploaded in the clear cannot turn private. */
static void
test_private_after_public_upload(void)
{
  relays_reset();
  g_autofree gchar *jpg = tmpfile_with("photo.jpg", TINY_JPEG, sizeof(TINY_JPEG));
  const gchar *args[] = { jpg, NULL };
  GError *err = NULL;
  g_autoptr(NsShare) s = share_new(config("wss://w1.test", "https://blossom.test"), NULL,
                                   args, REC.pk, 0, &err);
  NsFile *f = g_ptr_array_index(s->files, 0);
  f->uploaded = TRUE;             /* as ns_share_upload() leaves it */
  f->blob.url = g_strdup("https://blossom.test/abc.jpg");
  g_assert_false(ns_share_set_private(s, TRUE, &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_BAD_INPUT);
  g_assert_nonnull(strstr(err->message, "already uploaded unencrypted"));
  g_clear_error(&err);
  g_assert_false(s->private_share);
}

/* A forwarding session relay carries the wrap; it learns the recipient's
 * inbox from their kind 10050, handed to it first. */
static void
test_private_via_session_relay(void)
{
  if (s_bus == NULL) {
    g_test_skip("dbus-daemon not installed: no private bus for the fake session relay");
    return;
  }
  NpFakeSessionRelay *fake = np_fake_session_relay_start("active");
  const gchar *acked[] = { "wss://inbox1.test acked", NULL };
  np_fake_session_relay_set_reply(fake, "forwarded", "", acked);
  relays_reset();
  can_inboxes(FALSE);
  const gchar *texts[] = { "via the session relay", NULL };
  GError *err = NULL;
  g_autoptr(NsShare) s = private_share(texts, NULL, NULL, &err);
  s->net.session_socket = g_strdup("/run/fake/relay.sock");
  g_assert_true(ns_share_resolve(s, &err));
  g_assert_no_error(err);
  g_assert_true(s->targets.session_upstream);
  g_assert_true(ns_share_build(s, &err));
  g_assert_true(ns_share_publish(s, NULL, NULL, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(R.published->len, ==, 2);
  g_autoptr(JsonParser) p0 = NULL, p1 = NULL;
  const gchar *e0 = g_ptr_array_index(R.published, 0);
  const gchar *e1 = g_ptr_array_index(R.published, 1);
  g_assert_true(g_str_has_prefix(e0, NS_SESSION_RELAY_URL " "));
  g_assert_true(g_str_has_prefix(e1, NS_SESSION_RELAY_URL " "));
  JsonObject *inbox = parse_obj(e0 + strlen(NS_SESSION_RELAY_URL " "), &p0);
  g_assert_cmpint(json_object_get_int_member(inbox, "kind"), ==, 10050);
  g_assert_cmpstr(json_object_get_string_member(inbox, "pubkey"), ==, REC.pk);
  JsonObject *wrap = parse_obj(e1 + strlen(NS_SESSION_RELAY_URL " "), &p1);
  g_assert_cmpint(json_object_get_int_member(wrap, "kind"), ==, 1059);
  NsPost *p = g_ptr_array_index(s->posts, 0);
  g_assert_nonnull(strstr(p->result, "forwarded by the session relay: 1/1 inbox relays"));
  np_fake_session_relay_stop(fake);
}

/* default_text_kind / keep_metadata (written by org.nostr.Settings). */
static void
test_config_text_defaults(void)
{
  g_autofree gchar *conf = tmpfile_with("nostr-share.conf",
    "[nostr-share]\ndefault_text_kind=30023\nkeep_metadata=true\n", 57);
  const gchar *saved = g_getenv("NOSTR_SHARE_CONFIG");
  g_autofree gchar *restore = g_strdup(saved);
  g_setenv("NOSTR_SHARE_CONFIG", conf, TRUE);
  GError *err = NULL;
  NsConfig *cfg = ns_config_load(&err);
  g_assert_no_error(err);
  g_assert_cmpint(cfg->text_kind, ==, NS_KIND_ARTICLE);
  g_assert_true(cfg->keep_metadata);
  g_strfreev(cfg->home_relays);
  cfg->home_relays = g_strsplit("wss://w1.test", ";", -1);

  /* Plain text → article by default; --kind 1 still wins. */
  const gchar *texts[] = { "hello world", NULL };
  g_autoptr(NsShare) s = share_new(cfg, texts, NULL, NULL, 0, &err);
  g_assert_no_error(err);
  g_assert_cmpint(((NsPost *)g_ptr_array_index(s->posts, 0))->action, ==, NS_ACTION_ARTICLE);
  g_assert_true(ns_share_set_kind(s, NS_KIND_NOTE, &err));
  g_assert_cmpint(((NsPost *)g_ptr_array_index(s->posts, 0))->action, ==, NS_ACTION_NOTE);

  /* Markdown and URLs are unaffected by the text default. */
  g_autofree gchar *conf2 = tmpfile_with("nostr-share.conf",
    "[nostr-share]\ndefault_text_kind=30023\n", 38);
  g_setenv("NOSTR_SHARE_CONFIG", conf2, TRUE);
  NsConfig *cfg2 = ns_config_load(&err);
  g_assert_no_error(err);
  g_assert_false(cfg2->keep_metadata);
  const gchar *urls[] = { "https://example.com/a", NULL };
  g_autoptr(NsShare) s2 = share_new(cfg2, NULL, urls, NULL, 0, &err);
  g_assert_no_error(err);
  g_assert_cmpint(((NsPost *)g_ptr_array_index(s2->posts, 0))->action, ==, NS_ACTION_NOTE);

  /* Bad values are errors naming the file. */
  g_autofree gchar *bad = tmpfile_with("nostr-share.conf",
    "[nostr-share]\ndefault_text_kind=1063\n", 37);
  g_setenv("NOSTR_SHARE_CONFIG", bad, TRUE);
  g_assert_null(ns_config_load(&err));
  g_assert_error(err, NS_ERROR, NS_ERROR_BAD_INPUT);
  g_assert_nonnull(strstr(err->message, "default_text_kind"));
  g_clear_error(&err);
  g_autofree gchar *bad2 = tmpfile_with("nostr-share.conf",
    "[nostr-share]\nkeep_metadata=maybe\n", 34);
  g_setenv("NOSTR_SHARE_CONFIG", bad2, TRUE);
  g_assert_null(ns_config_load(&err));
  g_clear_error(&err);

  if (restore != NULL)
    g_setenv("NOSTR_SHARE_CONFIG", restore, TRUE);
  else
    g_unsetenv("NOSTR_SHARE_CONFIG");
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  /* Never the caller's session bus: a private one for the fake
   * org.nostr.SessionRelay1, or none at all. */
  GDBusConnection *session = NULL;
  if (np_fake_session_relay_bus_available()) {
    s_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(s_bus);   /* note: unsets XDG_RUNTIME_DIR */
    /* Held for the whole run, like a real process holds its bus: GLib's
     * shared connection is not torn down and rebuilt per share. */
    session = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    g_assert_nonnull(session);
  } else {
    g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/ns-test-bus", TRUE);
  }
  /* Hermetic (nostrc-eqdz): a private XDG_RUNTIME_DIR and XDG_CONFIG_HOME,
   * set before GLib caches either, so neither a running session relay's
   * socket ($XDG_RUNTIME_DIR/nostr/relay.sock) nor the user's
   * nostr-share.conf can leak in, under ctest or run by hand. (Not
   * G_TEST_OPTION_ISOLATE_DIRS: it also hides XDG_DATA_DIRS, i.e. the MIME
   * database, and its paths are too long for an AF_UNIX socket.) */
  s_private = g_dir_make_tmp("ns-share-XXXXXX", NULL);
  g_assert_nonnull(s_private);
  g_autofree gchar *priv_run = g_build_filename(s_private, "run", NULL);
  g_autofree gchar *priv_cfg = g_build_filename(s_private, "config", NULL);
  g_mkdir_with_parents(priv_run, 0700);
  g_mkdir_with_parents(priv_cfg, 0700);
  g_setenv("XDG_RUNTIME_DIR", priv_run, TRUE);
  g_setenv("XDG_CONFIG_HOME", priv_cfg, TRUE);
  g_unsetenv("NOSTR_SHARE_SESSION_RELAY_SOCKET");
  g_unsetenv("NOSTR_SHARE_CONFIG");
  K.sk = nostr_key_generate_private();
  K.pk = nostr_key_get_public(K.sk);
  REC.sk = nostr_key_generate_private();
  REC.pk = nostr_key_get_public(REC.sk);
  g_test_add_func("/nostr-share/share/session-socket-detection",
                  test_session_socket_detection);
  g_test_add_func("/nostr-share/share/url-note-publish", test_url_note_publish);
  g_test_add_func("/nostr-share/share/nip65-verified", test_nip65_verified);
  g_test_add_func("/nostr-share/share/session-relay-rules", test_session_relay_rules);
  g_test_add_func("/nostr-share/share/session-relay-not-forwarding",
                  test_session_relay_not_forwarding);
  g_test_add_func("/nostr-share/share/session-relay-upstream-verdicts",
                  test_session_relay_upstream_verdicts);
  g_test_add_func("/nostr-share/share/group-only-group-relay", test_group_only_group_relay);
  g_test_add_func("/nostr-share/share/mention", test_mention);
  g_test_add_func("/nostr-share/share/media-imeta-1063", test_media_imeta_and_1063);
  g_test_add_func("/nostr-share/share/unstrippable-media", test_unstrippable_media_blocked);
  g_test_add_func("/nostr-share/share/markdown-article", test_markdown_article);
  g_test_add_func("/nostr-share/share/calendar-dav", test_calendar_to_dav);
  g_test_add_func("/nostr-share/share/git-public-urls", test_git_public_urls);
  g_test_add_func("/nostr-share/share/git-repo", test_git_repo_announcement);
  g_test_add_func("/nostr-share/share/config-text-defaults", test_config_text_defaults);
  g_test_add_func("/nostr-share/private/text", test_private_text);
  g_test_add_func("/nostr-share/private/needs-inbox", test_private_needs_inbox);
  g_test_add_func("/nostr-share/private/refusals", test_private_refusals);
  g_test_add_func("/nostr-share/private/file", test_private_file);
  g_test_add_func("/nostr-share/private/via-session-relay", test_private_via_session_relay);
  g_test_add_func("/nostr-share/private/upload-unlinked", test_private_upload_unlinked);
  g_test_add_func("/nostr-share/private/relay-policy", test_private_relay_policy);
  g_test_add_func("/nostr-share/private/after-public-upload", test_private_after_public_upload);
  int rc = g_test_run();
  relays_reset();
  free(K.sk);
  free(K.pk);
  free(REC.sk);
  free(REC.pk);
  g_clear_object(&session);
  if (s_bus != NULL) {
    g_test_dbus_down(s_bus);
    g_object_unref(s_bus);
  }
  rm_rf(s_private);
  g_free(s_private);
  return rc;
}
