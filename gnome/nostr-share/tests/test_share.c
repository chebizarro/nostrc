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

#include "nostr-event.h"
#include "nostr-keys.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

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
  NsConfig *cfg = ns_config_load(NULL);   /* NOSTR_SHARE_CONFIG=/nonexistent */
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

static void
test_session_relay_rules(void)
{
  relays_reset();
  const gchar *texts[] = { "hi", NULL };
  GError *err = NULL;
  g_autoptr(NsShare) s = share_new(config("wss://w1.test", NULL), texts, NULL,
                                   NULL, 0, &err);
  s->net.session_socket = g_strdup("/run/fake/relay.sock");
  g_assert_true(ns_share_resolve(s, &err));
  /* default mode: session relay gets a copy, write relays are authoritative */
  g_assert_true(s->targets.session_included);
  g_assert_cmpuint(g_strv_length(s->targets.targets), ==, 2);
  g_assert_cmpstr(s->targets.targets[0], ==, NS_SESSION_RELAY_URL);
  g_assert_true(ns_share_build(s, NULL));

  g_hash_table_insert(R.reject, g_strdup("wss://w1.test"), g_strdup("blocked: test"));
  g_assert_false(ns_share_publish(s, NULL, NULL, &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_PUBLISH);   /* session ACK alone ≠ success */
  g_clear_error(&err);

  /* session_relay_only: session ACK is the success criterion. */
  relays_reset();
  NsConfig *cfg = config("wss://w1.test", NULL);
  cfg->upstream = NS_UPSTREAM_SESSION_RELAY_ONLY;
  g_autoptr(NsShare) s2 = share_new(cfg, texts, NULL, NULL, 0, &err);
  s2->net.session_socket = g_strdup("/run/fake/relay.sock");
  g_assert_true(ns_share_resolve(s2, &err));
  g_assert_cmpuint(g_strv_length(s2->targets.targets), ==, 1);
  g_assert_true(ns_share_build(s2, NULL));
  g_assert_true(ns_share_publish(s2, NULL, NULL, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(R.published->len, ==, 1);
  g_assert_true(g_str_has_prefix(g_ptr_array_index(R.published, 0), NS_SESSION_RELAY_URL));
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
  g_autofree gchar *mp4 = tmpfile_with("clip.mp4", "\0\0\0\x18" "ftypmp42", 12);
  const gchar *args[] = { mp4, NULL };
  GError *err = NULL;
  g_autoptr(NsShare) s = share_new(config("wss://w1.test", "https://blossom.test"),
                                   NULL, args, NULL, 0, &err);
  g_assert_no_error(err);
  g_autoptr(GString) why = g_string_new(NULL);
  g_assert_true(ns_share_metadata_blocked(s, why));
  g_assert_nonnull(strstr(why->str, "clip.mp4"));
  g_assert_false(ns_share_upload(s, NULL, NULL, NULL, &err));
  g_assert_error(err, NS_ERROR, NS_ERROR_METADATA);
  g_clear_error(&err);
  s->keep_metadata = TRUE;
  g_assert_false(ns_share_metadata_blocked(s, NULL));
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
  K.sk = nostr_key_generate_private();
  K.pk = nostr_key_get_public(K.sk);
  g_test_add_func("/nostr-share/share/url-note-publish", test_url_note_publish);
  g_test_add_func("/nostr-share/share/nip65-verified", test_nip65_verified);
  g_test_add_func("/nostr-share/share/session-relay-rules", test_session_relay_rules);
  g_test_add_func("/nostr-share/share/group-only-group-relay", test_group_only_group_relay);
  g_test_add_func("/nostr-share/share/mention", test_mention);
  g_test_add_func("/nostr-share/share/media-imeta-1063", test_media_imeta_and_1063);
  g_test_add_func("/nostr-share/share/unstrippable-media", test_unstrippable_media_blocked);
  g_test_add_func("/nostr-share/share/markdown-article", test_markdown_article);
  g_test_add_func("/nostr-share/share/calendar-dav", test_calendar_to_dav);
  g_test_add_func("/nostr-share/share/git-public-urls", test_git_public_urls);
  g_test_add_func("/nostr-share/share/git-repo", test_git_repo_announcement);
  g_test_add_func("/nostr-share/share/config-text-defaults", test_config_text_defaults);
  int rc = g_test_run();
  relays_reset();
  free(K.sk);
  free(K.pk);
  return rc;
}
