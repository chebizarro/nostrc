/* Wire-shape conformance, not a live cross-client/media qualification.
 * Amethyst quartz ce558d90e70d432157cc0672762b2d4119d7f904:
 * nipACWebRtcCalls/events and the independent nip59 vector below.
 * Nospeak 518b7711d397d8d62bcbe9774c34300b4e33d9d4:
 * src/lib/core/voiceCall/{nipAcGiftWrap,VoiceCallService,wireParity.test}.ts
 * tests/fixtures/nip-ac-wire/inner-events.json (vendored unchanged).
 * Upstream vectors are unsigned shapes, signed here with disposable keys.
 */
#include "gh-call-service.h"
#include "fake-call-media.h"
#include "gh-test-signer.h"
#include <nostr/nip59/nip59.h>
#include <nostr-kinds.h>
#include <nostr-tag.h>
#include <json-glib/json-glib.h>
#if GROUNDHOG_HAVE_MESSAGE_POLICY
#include "gh-store-conversations.h"
#include "gh-store.h"
#include <sqlite3.h>
#endif
#include "../../../../nips/nip59/tests/nipac_wire_vector.h"

static const gchar *CALL_ID = "123e4567-e89b-12d3-a456-426614174000";
static const gchar *SDP = "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n";
static gint64 now;
static gchar *me, *peer;
static JsonParser *vectors;
static JsonObject *root;
static const gchar *vector_content(const gchar *name)
{
  JsonArray *cases = json_object_get_array_member(root, "cases");
  for (guint i = 0; i < json_array_get_length(cases); i++) {
    JsonObject *c = json_array_get_object_element(cases, i);
    if (!g_strcmp0(json_object_get_string_member(c, "name"), name))
      return json_object_get_string_member(c, "content");
  }
  g_error("Missing upstream fixture %s", name); return NULL;
}
static gchar *event(guint key, const gchar *recipient, const gchar *id, int kind,
                    const gchar *content, gint64 time)
{
  g_autofree gchar *author = gh_test_pub(key);
  NostrEvent *e = gh_call_event_new(author, recipient, id, kind, GH_CALL_VOICE, content, time);
  g_assert_cmpint(nostr_event_sign(e, gh_test_secret[key]), ==, 0);
  gchar *json = nostr_event_serialize_compact(e); nostr_event_free(e); return json;
}
static void drain(void) { while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE); }
typedef struct {
  GSettings *settings;
  GhNetSession *network;
  GhConversationStore *conversations;
  GhCallService *service;
  GhFakeCallMedia *media;
  guint ring, missed;
  GArray *sent;
#if GROUNDHOG_HAVE_MESSAGE_POLICY
  GhStore *store;
  GhStoreConversations *stored;
#endif
} Fixture;
static void ring(GhCallService *s, GhCall *c, gpointer data)
{ (void)s; (void)c; ((Fixture *)data)->ring++; }
static void missed(GhCallService *s, GhCall *c, gpointer data)
{ (void)s; g_assert_cmpint(gh_call_get_state(c), ==, GH_CALL_ENDED); ((Fixture *)data)->missed++; }
static void sent(GhCallService *s, const gchar *p, const gchar *id, int kind, int type, const gchar *text, gpointer data)
{ (void)s; (void)p; (void)id; (void)type; (void)text; g_array_append_val(((Fixture *)data)->sent, kind); }
static void setup(Fixture *f, gconstpointer data)
{
  (void)data;
  f->settings = g_settings_new("org.nostr.Groundhog");
  g_settings_reset(f->settings, "allow-calls-from-unaccepted");
  f->network = gh_net_session_new(NULL); f->conversations = gh_conversation_store_new();
  gh_conversation_store_set_account(f->conversations, me, NULL, NULL, NULL);
#if GROUNDHOG_HAVE_MESSAGE_POLICY
  f->store = gh_store_open_ephemeral(me, NULL, NULL); g_assert_nonnull(f->store);
  f->stored = gh_store_conversations_new(f->store);
  g_assert_true(gh_store_conversations_attach(f->stored, f->conversations, 0, NULL));
#endif
  f->service = gh_call_service_new(NULL, f->conversations, f->settings, f->network);
  gh_call_service_bind(f->service, me);
  gh_call_service_set_media_factory(f->service, gh_fake_call_media_factory, &f->media, NULL);
  f->sent = g_array_new(FALSE, FALSE, sizeof(int));
  g_signal_connect(f->service, "incoming-ring", G_CALLBACK(ring), f);
  g_signal_connect(f->service, "missed-call", G_CALLBACK(missed), f);
  g_signal_connect(f->service, "send", G_CALLBACK(sent), f);
}
static void teardown(Fixture *f, gconstpointer data)
{
  (void)data; gh_call_service_stop(f->service); drain();
  g_clear_object(&f->service); g_clear_object(&f->media);
#if GROUNDHOG_HAVE_MESSAGE_POLICY
  sqlite3_stmt *stmt = NULL;
  g_assert_cmpint(sqlite3_prepare_v2(gh_store_get_db(f->store),
    "SELECT (SELECT count(*) FROM messages)+(SELECT count(*) FROM seen)+(SELECT count(*) FROM outbox)+(SELECT count(*) FROM outbox_events)", -1, &stmt, NULL), ==, SQLITE_OK);
  g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW); g_assert_cmpint(sqlite3_column_int(stmt, 0), ==, 0); sqlite3_finalize(stmt);
  gh_conversation_store_set_account(f->conversations, NULL, NULL, NULL, NULL);
  gh_store_conversations_close(f->stored); g_object_unref(f->stored); gh_store_close(f->store);
#endif
  g_clear_object(&f->conversations);
  g_clear_object(&f->network); g_clear_object(&f->settings); g_array_unref(f->sent);
}
static void deliver(Fixture *f, int kind, const gchar *text)
{
  g_autofree gchar *json = event(1, me, CALL_ID, kind, text, now);
  gh_call_service_receive(f->service, json, now);
}
static void allow(Fixture *f) { g_settings_set_boolean(f->settings, "allow-calls-from-unaccepted", TRUE); }
static void requests(Fixture *f, gconstpointer data)
{
  (void)data; g_assert_false(g_settings_get_boolean(f->settings, "allow-calls-from-unaccepted"));
  deliver(f, NOSTR_KIND_CALL_OFFER, SDP);
  g_assert_cmpuint(f->ring, ==, 0); g_assert_cmpuint(f->missed, ==, 1);
  g_assert_cmpuint(f->sent->len, ==, 0);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f->conversations)), ==, 0);
  deliver(f, NOSTR_KIND_CALL_OFFER, SDP); g_assert_cmpuint(f->missed, ==, 1);
}
static void accepted_offer(Fixture *f, gconstpointer data)
{
  (void)data;
  const gchar *peers[] = {peer, NULL};
  g_assert_nonnull(gh_conversation_store_open_room(f->conversations, peers, NULL));
  deliver(f, NOSTR_KIND_CALL_OFFER, SDP); g_assert_cmpuint(f->ring, ==, 1);
  gh_call_service_decline(f->service);
  g_assert_cmpint(gh_call_get_end_reason(gh_call_service_get_current(f->service)), ==, GH_CALL_END_DECLINED);
}
static void hidden_offer(Fixture *f, gconstpointer data)
{
  (void)data;
#if GROUNDHOG_HAVE_MESSAGE_POLICY
  allow(f);
  g_autofree gchar *sql = g_strdup_printf("INSERT INTO social_mutes(entry_key,tag_name,value,position,is_private) VALUES('p','p','%s',0,1)", peer);
  g_assert_true(gh_store_exec(f->store, sql, NULL));
  GhMessagePolicy *policy = gh_conversation_store_get_policy(f->conversations);
  g_assert_true(gh_message_policy_refresh(policy, NULL)); drain();
  g_assert_true(gh_message_policy_hides_author(policy, peer));
  deliver(f, NOSTR_KIND_CALL_OFFER, SDP);
  g_assert_cmpuint(f->ring, ==, 0); g_assert_cmpuint(f->missed, ==, 0); g_assert_cmpuint(f->sent->len, ==, 0);
#else
  g_test_skip("Build lacks encrypted message policy");
#endif
}
static void incoming(Fixture *f, gconstpointer data)
{
  (void)data; allow(f); deliver(f, NOSTR_KIND_CALL_OFFER, SDP);
  GhCall *c = gh_call_service_get_current(f->service);
  g_assert_cmpuint(f->ring, ==, 1); g_assert_cmpint(gh_call_get_state(c), ==, GH_CALL_RINGING);
  g_autofree gchar *ice = gh_call_ice_json("candidate:1 1 UDP 1 192.0.2.1 9 typ relay", "0", 0);
  deliver(f, NOSTR_KIND_CALL_ICE_CANDIDATE, ice);
  g_assert_true(gh_call_service_answer(f->service, NULL));
  g_assert_cmpuint(f->media->ice, ==, 0); drain();
  g_assert_cmpuint(f->media->ice, ==, 1); g_assert_cmpuint(f->media->answers, ==, 1);
  g_assert_cmpuint(f->sent->len, ==, 2);
  g_assert_cmpint(g_array_index(f->sent, int, 0), ==, NOSTR_KIND_CALL_ANSWER);
  g_assert_cmpint(g_array_index(f->sent, int, 1), ==, NOSTR_KIND_CALL_ICE_CANDIDATE);
  deliver(f, NOSTR_KIND_CALL_ICE_CANDIDATE, ice); g_assert_cmpuint(f->media->ice, ==, 1);
  g_signal_emit_by_name(f->media, "connection-state", GH_CALL_MEDIA_CONNECTED);
  g_assert_cmpint(gh_call_get_state(c), ==, GH_CALL_ACTIVE);
  deliver(f, NOSTR_KIND_CALL_RENEGOTIATE, vector_content("renegotiate (voice -> video upgrade)")); drain();
  g_assert_cmpuint(f->media->answers, ==, 2); g_assert_cmpint(gh_call_get_call_type(c), ==, GH_CALL_VIDEO);
  deliver(f, NOSTR_KIND_CALL_HANGUP, ""); g_assert_cmpint(gh_call_get_state(c), ==, GH_CALL_ENDED);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f->conversations)), ==, 0);
}
static void outgoing(Fixture *f, gconstpointer data)
{
  (void)data;
  GhCall *c = gh_call_service_place(f->service, peer, GH_CALL_VOICE, NULL); g_assert_nonnull(c); drain();
  const gchar *id = gh_call_get_id(c);
  g_autofree gchar *answer = event(1, me, id, NOSTR_KIND_CALL_ANSWER, SDP, now);
  gh_call_service_receive(f->service, answer, now); drain();
  g_assert_cmpuint(f->media->remote_answers, ==, 1);
  g_signal_emit_by_name(f->media, "connection-state", GH_CALL_MEDIA_CONNECTED);
  g_assert_true(gh_call_service_upgrade_video(f->service, NULL)); drain();
  g_autofree gchar *answer2 = event(1, me, id, NOSTR_KIND_CALL_ANSWER, "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\n", now+1);
  gh_call_service_receive(f->service, answer2, now+1); drain();
  g_assert_cmpuint(f->media->remote_answers, ==, 2);
  gh_call_service_hang_up(f->service);
  g_assert_cmpint(gh_call_get_end_reason(c), ==, GH_CALL_END_HANGUP);
}
static void busy_timeout(Fixture *f, gconstpointer data)
{
  (void)data; allow(f); deliver(f, NOSTR_KIND_CALL_OFFER, SDP);
  g_autofree gchar *other = event(3, me, "223e4567-e89b-12d3-a456-426614174000", NOSTR_KIND_CALL_OFFER, SDP, now);
  gh_call_service_receive(f->service, other, now); g_assert_cmpuint(f->ring, ==, 1);
  g_assert_cmpint(g_array_index(f->sent, int, 0), ==, NOSTR_KIND_CALL_REJECT);
  gh_call_service_expire(f->service, now+59); g_assert_cmpuint(f->missed, ==, 0);
  gh_call_service_expire(f->service, now+60); g_assert_cmpuint(f->missed, ==, 1);
  deliver(f, NOSTR_KIND_CALL_OFFER, SDP); g_assert_cmpuint(f->ring, ==, 1);
}
static void stale_unknown_elsewhere(Fixture *f, gconstpointer data)
{
  (void)data; allow(f);
  g_autofree gchar *stale = event(1, me, CALL_ID, NOSTR_KIND_CALL_OFFER, SDP, now-61);
  gh_call_service_receive(f->service, stale, now); g_assert_cmpuint(f->ring, ==, 0);
  deliver(f, NOSTR_KIND_CALL_ANSWER, SDP); g_assert_null(gh_call_service_get_current(f->service));
  deliver(f, NOSTR_KIND_CALL_OFFER, SDP);
  g_autofree gchar *self = event(2, peer, CALL_ID, NOSTR_KIND_CALL_ANSWER, SDP, now);
  gh_call_service_receive(f->service, self, now);
  g_assert_cmpint(gh_call_get_end_reason(gh_call_service_get_current(f->service)), ==, GH_CALL_END_ELSEWHERE);
}
static void remote_reject(Fixture *f, gconstpointer data)
{
  (void)data;
  for (guint busy = 0; busy <= 1; busy++) {
    GhCall *c = gh_call_service_place(f->service, peer, GH_CALL_VOICE, NULL); g_assert_nonnull(c); drain();
    g_autofree gchar *json = event(1, me, gh_call_get_id(c), NOSTR_KIND_CALL_REJECT,
      vector_content(busy ? "reject (busy)" : "reject (no reason)"), now);
    gh_call_service_receive(f->service, json, now);
    g_assert_cmpint(gh_call_get_end_reason(c), ==, busy ? GH_CALL_END_BUSY : GH_CALL_END_DECLINED);
  }
}
static void switch_cancel(Fixture *f, gconstpointer data)
{
  (void)data;
  GhCall *c = gh_call_service_place(f->service, peer, GH_CALL_VOICE, NULL); g_assert_nonnull(c); g_object_ref(c);
  gh_call_service_bind(f->service, peer); drain();
  g_assert_cmpint(gh_call_get_end_reason(c), ==, GH_CALL_END_ACCOUNT_CHANGED);
  g_assert_cmpuint(f->sent->len, ==, 0); g_assert_null(gh_call_service_get_current(f->service)); g_object_unref(c);
}
static void privacy(Fixture *f, gconstpointer data)
{
  (void)data; allow(f);
  GhCall *c = gh_call_service_place(f->service, peer, GH_CALL_VOICE, NULL); g_assert_nonnull(c); drain();
  gh_net_session_set_mode(f->network, GH_NET_MODE_TOR, "127.0.0.1:9050");
  g_assert_cmpint(gh_call_get_end_reason(c), ==, GH_CALL_END_PRIVACY);
  g_assert_null(gh_call_service_place(f->service, peer, GH_CALL_VOICE, NULL));
}
static void codec(void)
{
  NostrEvent *e = gh_call_event_parse(NIPAC_INNER_JSON, me, 1700000000, NULL); g_assert_nonnull(e);
  nostr_event_free(e);
  g_assert_null(gh_call_event_parse(NIPAC_INNER_JSON, me, 1700000061, NULL));
  g_assert_null(gh_call_event_parse(NIPAC_INNER_JSON, peer, 1700000000, NULL));
  NostrEvent *outer = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(outer, NIPAC_OUTER_JSON, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_true(nostr_nip59_validate_gift_wrap_kind(outer, NOSTR_KIND_EPHEMERAL_GIFT_WRAP));
  e = nostr_nip59_unwrap_kind(outer, NIPAC_RECIPIENT_SK, NOSTR_KIND_EPHEMERAL_GIFT_WRAP); g_assert_nonnull(e);
  g_assert_cmpstr(gh_call_event_tag(e, "call-id"), ==, CALL_ID);
  nostr_event_free(e); nostr_event_free(outer);
  g_assert_false(gh_call_ice_parse("{\"candidate\":3}", NULL, NULL, NULL));
  JsonArray *cases = json_object_get_array_member(root, "cases");
  for (guint i = 0; i < json_array_get_length(cases); i++) {
    JsonObject *c = json_array_get_object_element(cases, i);
    NostrEvent *v = nostr_event_new();
    nostr_event_set_pubkey(v, json_object_get_string_member(root, "sender"));
    nostr_event_set_created_at(v, json_object_get_int_member(root, "createdAt"));
    nostr_event_set_kind(v, json_object_get_int_member(c, "kind"));
    nostr_event_set_content(v, json_object_get_string_member(c, "content"));
    NostrTags *tags = nostr_tags_new(0);
    nostr_tags_append(tags, nostr_tag_new("p", json_object_get_string_member(root, "recipient"), NULL));
    nostr_tags_append(tags, nostr_tag_new("call-id", json_object_get_string_member(root, "callId"), NULL));
    nostr_tags_append(tags, nostr_tag_new("alt", json_object_get_string_member(c, "altText"), NULL));
    JsonArray *extra = json_object_get_array_member(c, "extraTags");
    for (guint j = 0; j < json_array_get_length(extra); j++) {
      JsonArray *a = json_array_get_array_element(extra, j);
      NostrTag *t = nostr_tag_new(json_array_get_string_element(a, 0), NULL);
      for (guint k = 1; k < json_array_get_length(a); k++) nostr_tag_append(t, json_array_get_string_element(a, k));
      nostr_tags_append(tags, t);
    }
    nostr_event_set_tags(v, tags); gchar hash[65];
    g_assert_cmpint(nostr_event_compute_id(v, hash), ==, NOSTR_EVENT_VALIDATION_OK);
    g_assert_cmpstr(hash, ==, json_object_get_string_member(c, "expectedId"));
    /* Rebind only identities and sign locally: preserve upstream content,
     * call id and every tag shape, including nullable ICE and group negatives. */
    nostr_event_set_pubkey(v, peer);
    nostr_tag_set(nostr_tags_get(tags, 0), 1, me);
    g_assert_cmpint(nostr_event_sign(v, gh_test_secret[1]), ==, 0);
    g_autofree gchar *wire = nostr_event_serialize_compact(v);
    NostrEvent *parsed = gh_call_event_parse(wire, me, 1700000000, NULL);
    if (g_str_has_prefix(json_object_get_string_member(c, "name"), "group")) g_assert_null(parsed);
    else { g_assert_nonnull(parsed); nostr_event_free(parsed); }
    nostr_event_free(v);
  }
}
void gh_test_register_call_transport(void);
int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL); now = g_get_real_time()/G_USEC_PER_SEC;
  me = gh_test_pub(2); peer = gh_test_pub(1);
  vectors = json_parser_new(); g_assert_true(json_parser_load_from_file(vectors, GH_CALL_FIXTURE_DIR "/nipac-nospeak.json", NULL));
  root = json_node_get_object(json_parser_get_root(vectors));
  SDP = vector_content("offer");
  g_test_add_func("/calls/wire-vectors", codec);
#define TEST(name, fn) g_test_add(name, Fixture, NULL, setup, fn, teardown)
  TEST("/calls/accepted-offer", accepted_offer);
  TEST("/calls/hidden-offer-silent", hidden_offer);
  TEST("/calls/message-requests", requests); TEST("/calls/incoming-ice-renegotiate", incoming);
  TEST("/calls/remote-reject-busy", remote_reject);
  TEST("/calls/outgoing-upgrade", outgoing); TEST("/calls/busy-timeout", busy_timeout);
  TEST("/calls/stale-unknown-self-answer", stale_unknown_elsewhere);
  TEST("/calls/account-switch-pending-sdp", switch_cancel); TEST("/calls/tor-policy", privacy);
  gh_test_register_call_transport();
  int rc = g_test_run(); g_free(me); g_free(peer); g_object_unref(vectors); return rc;
}
