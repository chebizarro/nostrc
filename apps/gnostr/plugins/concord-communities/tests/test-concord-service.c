/* Concord service tests.
 *
 * Every case here mints its own wire events in-process: a Concord plane is
 * pure derivation, so the whole read path — wrap, seal, rumor, binding checks
 * — is exercisable with no relay and no signer.
 */

#include "../gn-concord-community-service.h"
#include "../model/gn-concord-channel-item.h"

#include <gnostr-plugin-api.h>
#include <json-glib/json-glib.h>
#include <nip_concord.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-tag.h>
#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <nostr/nip44/nip44.h>

#include <stdlib.h>
#include <string.h>

/* Test hooks owned by the offline host stubs. */
extern const char *gn_concord_test_signer_sk;
extern const char *gn_concord_test_user_pubkey;
extern char *gn_concord_test_published_json;
extern GPtrArray *gn_concord_test_published;
extern GPtrArray *(*gn_concord_test_query_hook)(const char *filter_json);
extern gboolean gn_concord_test_query_fails;
extern void gn_concord_test_reset(void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NostrEvent, nostr_event_free)

#define OWNER_SK "1111111111111111111111111111111111111111111111111111111111111111"
#define AUTHOR_SK "2222222222222222222222222222222222222222222222222222222222222222"
#define COMMUNITY_ROOT "3333333333333333333333333333333333333333333333333333333333333333"
#define CHANNEL_KEY "4444444444444444444444444444444444444444444444444444444444444444"
#define CHANNEL_ID "5555555555555555555555555555555555555555555555555555555555555555"
#define OWNER_SALT "6666666666666666666666666666666666666666666666666666666666666666"
#define TEST_EPOCH 3

typedef struct {
  gchar *community_id;
  gchar *owner_pubkey;
  gchar *author_pubkey;
} Fixture;

static void fixture_clear(Fixture *fixture) {
  g_clear_pointer(&fixture->community_id, g_free);
  g_clear_pointer(&fixture->owner_pubkey, g_free);
  g_clear_pointer(&fixture->author_pubkey, g_free);
}

static void fixture_init(Fixture *fixture) {
  fixture->owner_pubkey = nostr_key_get_public(OWNER_SK);
  fixture->author_pubkey = nostr_key_get_public(AUTHOR_SK);
  g_assert_nonnull(fixture->owner_pubkey);
  g_assert_nonnull(fixture->author_pubkey);

  uint8_t owner[32], salt[32], id[32];
  g_assert_true(nostr_concord_hex_decode_32(fixture->owner_pubkey, owner));
  g_assert_true(nostr_concord_hex_decode_32(OWNER_SALT, salt));
  g_assert_cmpint(nostr_concord_derive_community_id(owner, salt, id), ==,
                  NOSTR_CONCORD_OK);
  char hex[65];
  nostr_concord_hex_encode_32(id, hex);
  fixture->community_id = g_strdup(hex);
}

/* A CORD-05 §1 CommunityInvite granting one private Channel. */
static gchar *build_bundle(const Fixture *fixture, const char *channel_key,
                           gint64 expires_at, guint n_channels) {
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "community_id");
  json_builder_add_string_value(builder, fixture->community_id);
  json_builder_set_member_name(builder, "owner");
  json_builder_add_string_value(builder, fixture->owner_pubkey);
  json_builder_set_member_name(builder, "owner_salt");
  json_builder_add_string_value(builder, OWNER_SALT);
  json_builder_set_member_name(builder, "community_root");
  json_builder_add_string_value(builder, COMMUNITY_ROOT);
  json_builder_set_member_name(builder, "root_epoch");
  json_builder_add_int_value(builder, TEST_EPOCH);
  json_builder_set_member_name(builder, "name");
  json_builder_add_string_value(builder, "Incident Response");
  if (expires_at) {
    json_builder_set_member_name(builder, "expires_at");
    json_builder_add_int_value(builder, expires_at);
  }
  json_builder_set_member_name(builder, "relays");
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "wss://relay.example.org");
  json_builder_end_array(builder);
  json_builder_set_member_name(builder, "channels");
  json_builder_begin_array(builder);
  for (guint i = 0; i < n_channels; i++) {
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, CHANNEL_ID);
    if (channel_key) {
      json_builder_set_member_name(builder, "key");
      json_builder_add_string_value(builder, channel_key);
    }
    json_builder_set_member_name(builder, "epoch");
    json_builder_add_int_value(builder, TEST_EPOCH);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, "general");
    json_builder_end_object(builder);
  }
  json_builder_end_array(builder);
  json_builder_end_object(builder);

  g_autoptr(JsonGenerator) generator = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(generator, root);
  gchar *json = json_generator_to_data(generator, NULL);
  json_node_free(root);
  return json;
}

typedef struct {
  const char *channel_tag;
  const char *epoch_tag;
  const char *ms_tag;
  int seal_kind;
  int rumor_kind;
  gboolean tamper_wrap_author;
} MintOptions;

/* Mints the three-layer envelope a real client would publish: an unsigned
 * rumor inside an author-signed seal inside a stream-key-signed wrap. */
static gchar *mint_wrap(const Fixture *fixture, const char *content,
                        gint64 created_at, const MintOptions *options) {
  uint8_t channel_secret[32], channel_id[32];
  g_assert_true(nostr_concord_hex_decode_32(CHANNEL_KEY, channel_secret));
  g_assert_true(nostr_concord_hex_decode_32(CHANNEL_ID, channel_id));

  nostr_concord_group_key_t key;
  g_assert_cmpint(nostr_concord_channel_key(channel_secret, channel_id,
                                            TEST_EPOCH, &key),
                  ==, NOSTR_CONCORD_OK);

  g_autoptr(NostrEvent) rumor = nostr_event_new();
  nostr_event_set_kind(rumor, options->rumor_kind);
  nostr_event_set_pubkey(rumor, fixture->author_pubkey);
  nostr_event_set_created_at(rumor, created_at);
  nostr_event_set_content(rumor, content);
  nostr_event_set_tags(rumor, nostr_tags_new(
    3,
    nostr_tag_new("channel", options->channel_tag, NULL),
    nostr_tag_new("epoch", options->epoch_tag, NULL),
    nostr_tag_new("ms", options->ms_tag, NULL)));
  g_autofree gchar *rumor_json = nostr_event_serialize_compact(rumor);
  g_assert_nonnull(rumor_json);

  char *seal_content = NULL;
  g_assert_cmpint(
    nostr_concord_stream_seal(key.conv_key, rumor_json, &seal_content), ==,
    NOSTR_CONCORD_OK);

  g_autoptr(NostrEvent) seal = nostr_event_new();
  nostr_event_set_kind(seal, options->seal_kind);
  nostr_event_set_pubkey(seal, fixture->author_pubkey);
  nostr_event_set_created_at(seal, created_at);
  nostr_event_set_content(seal, seal_content);
  nostr_event_set_tags(seal, nostr_tags_new(0));
  free(seal_content);
  g_assert_cmpint(nostr_event_sign(seal, AUTHOR_SK), ==, 0);
  g_autofree gchar *seal_json = nostr_event_serialize_compact(seal);
  g_assert_nonnull(seal_json);

  char *wrap_content = NULL;
  g_assert_cmpint(
    nostr_concord_stream_seal(key.conv_key, seal_json, &wrap_content), ==,
    NOSTR_CONCORD_OK);

  char stream_sk[65], stream_pk[65];
  nostr_concord_hex_encode_32(key.sk, stream_sk);
  nostr_concord_hex_encode_32(key.pk, stream_pk);

  /* A wrap at a *different* address is signed by a different key: the tamper
   * case mints a valid event that simply is not this Channel's stream. */
  const char *sign_with = stream_sk;
  const char *author = stream_pk;
  g_autofree gchar *other_pk = NULL;
  if (options->tamper_wrap_author) {
    other_pk = nostr_key_get_public(AUTHOR_SK);
    sign_with = AUTHOR_SK;
    author = other_pk;
  }

  g_autoptr(NostrEvent) wrap = nostr_event_new();
  nostr_event_set_kind(wrap, CONCORD_STREAM_WRAP);
  nostr_event_set_pubkey(wrap, author);
  nostr_event_set_created_at(wrap, created_at);
  nostr_event_set_content(wrap, wrap_content);
  g_autofree gchar *ephemeral_sk = nostr_key_generate_private();
  g_autofree gchar *ephemeral_pk = nostr_key_get_public(ephemeral_sk);
  nostr_event_set_tags(wrap,
                       nostr_tags_new(1, nostr_tag_new("p", ephemeral_pk,
                                                       NULL)));
  free(wrap_content);
  g_assert_cmpint(nostr_event_sign(wrap, sign_with), ==, 0);
  nostr_concord_group_key_clear(&key);
  return nostr_event_serialize_compact(wrap);
}

static MintOptions default_options(void) {
  MintOptions options = {
    .channel_tag = CHANNEL_ID,
    .epoch_tag = "3",
    .ms_tag = "250",
    .seal_kind = CONCORD_SEAL_ENCRYPTED,
    .rumor_kind = CONCORD_KIND_MESSAGE,
    .tamper_wrap_author = FALSE
  };
  return options;
}

static GnConcordCommunityService *service_with_membership(Fixture *fixture) {
  GnConcordCommunityService *service =
    gn_concord_community_service_new_offline(fixture->author_pubkey);
  g_autofree gchar *bundle = build_bundle(fixture, CHANNEL_KEY, 0, 1);
  g_autoptr(GError) error = NULL;
  g_assert_true(
    gn_concord_community_service_accept_bundle(service, bundle, &error));
  g_assert_no_error(error);
  return service;
}

/* ------------------------------------------------------------------ */

static void test_accept_bundle(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    service_with_membership(&fixture);

  GListModel *model = gn_concord_community_service_get_model(service);
  g_assert_cmpuint(g_list_model_get_n_items(model), ==, 1);

  g_autoptr(GnConcordCommunityItem) item =
    gn_concord_community_service_lookup_community(service,
                                                  fixture.community_id);
  g_assert_nonnull(item);
  g_assert_cmpstr(gn_concord_community_item_get_name(item), ==,
                  "Incident Response");
  g_assert_cmpstr(gn_concord_community_item_get_owner(item), ==,
                  fixture.owner_pubkey);
  g_assert_cmpuint(gn_concord_community_item_get_channel_count(item), ==, 1);
  g_assert_cmpstr(gn_concord_community_item_get_primary_relay(item), ==,
                  "wss://relay.example.org");

  /* The Channel's stream address must be derived at adoption time: it is the
   * `authors` filter, and without it nothing can be subscribed. */
  g_autoptr(GnConcordChannelItem) channel =
    gn_concord_community_item_find_channel(item, CHANNEL_ID);
  g_assert_nonnull(channel);
  g_assert_true(gn_concord_channel_item_get_is_private(channel));
  const char *stream_pk = gn_concord_channel_item_get_stream_pubkey(channel);
  g_assert_nonnull(stream_pk);
  g_assert_true(nostr_concord_is_lower_hex_32(stream_pk));

  /* And it must be exactly what the frozen derivation says. */
  uint8_t secret[32], id[32];
  nostr_concord_hex_decode_32(CHANNEL_KEY, secret);
  nostr_concord_hex_decode_32(CHANNEL_ID, id);
  nostr_concord_group_key_t key;
  g_assert_cmpint(nostr_concord_channel_key(secret, id, TEST_EPOCH, &key), ==,
                  NOSTR_CONCORD_OK);
  char expected[65];
  nostr_concord_hex_encode_32(key.pk, expected);
  g_assert_cmpstr(stream_pk, ==, expected);
  nostr_concord_group_key_clear(&key);

  fixture_clear(&fixture);
}

static void test_reject_forged_owner(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    gn_concord_community_service_new_offline(fixture.author_pubkey);

  /* Swapping the owner for someone else leaves the community_id unchanged,
   * which is precisely what the recomputation catches (CORD-05 §1). */
  g_autofree gchar *bundle = build_bundle(&fixture, CHANNEL_KEY, 0, 1);
  g_autofree gchar *forged =
    g_strdup(bundle);
  gchar *owner_position = strstr(forged, fixture.owner_pubkey);
  g_assert_nonnull(owner_position);
  owner_position[0] = owner_position[0] == 'a' ? 'b' : 'a';

  g_autoptr(GError) error = NULL;
  g_assert_false(
    gn_concord_community_service_accept_bundle(service, forged, &error));
  g_assert_nonnull(error);
  g_assert_cmpuint(
    g_list_model_get_n_items(
      gn_concord_community_service_get_model(service)), ==, 0);

  fixture_clear(&fixture);
}

static void test_reject_expired_invite(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    gn_concord_community_service_new_offline(fixture.author_pubkey);

  /* Past expires_at the preview still renders, joining refuses. */
  g_autofree gchar *bundle = build_bundle(&fixture, CHANNEL_KEY, 1000, 1);
  g_autoptr(GError) error = NULL;
  g_assert_false(
    gn_concord_community_service_accept_bundle(service, bundle, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);

  fixture_clear(&fixture);
}

static void test_message_roundtrip(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    service_with_membership(&fixture);

  MintOptions options = default_options();
  g_autofree gchar *wrap =
    mint_wrap(&fixture, "the relay sees only this ciphertext", 1686840217,
              &options);
  g_assert_true(gn_concord_community_service_ingest_wrap(
    service, fixture.community_id, CHANNEL_ID, wrap));

  GListModel *messages = gn_concord_community_service_get_messages(
    service, fixture.community_id, CHANNEL_ID);
  g_assert_nonnull(messages);
  g_assert_cmpuint(g_list_model_get_n_items(messages), ==, 1);

  g_autoptr(GnConcordMessageItem) item = g_list_model_get_item(messages, 0);
  g_assert_cmpstr(gn_concord_message_item_get_content(item), ==,
                  "the relay sees only this ciphertext");
  /* The author is the seal's npub, never the stream key that signed the
   * wrap — the wrap proves membership, the seal proves authorship. */
  g_assert_cmpstr(gn_concord_message_item_get_author(item), ==,
                  fixture.author_pubkey);
  g_assert_cmpint(gn_concord_message_item_get_ms(item), ==, 250);
  g_assert_cmpint(gn_concord_message_item_get_order_key(item), ==,
                  1686840217LL * 1000 + 250);

  /* The same wrap twice is one message: the fold dedupes on the *rumor* id. */
  g_assert_false(gn_concord_community_service_ingest_wrap(
    service, fixture.community_id, CHANNEL_ID, wrap));
  g_assert_cmpuint(g_list_model_get_n_items(messages), ==, 1);

  fixture_clear(&fixture);
}

static void test_ordering_uses_ms(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    service_with_membership(&fixture);

  /* Two rumors in the same second still sort deterministically, because the
   * comparison basis is created_at * 1000 + ms (CORD-02 §4). */
  MintOptions late = default_options();
  late.ms_tag = "900";
  g_autofree gchar *second =
    mint_wrap(&fixture, "second", 1686840217, &late);
  MintOptions early = default_options();
  early.ms_tag = "100";
  g_autofree gchar *first = mint_wrap(&fixture, "first", 1686840217, &early);

  g_assert_true(gn_concord_community_service_ingest_wrap(
    service, fixture.community_id, CHANNEL_ID, second));
  g_assert_true(gn_concord_community_service_ingest_wrap(
    service, fixture.community_id, CHANNEL_ID, first));

  GListModel *messages = gn_concord_community_service_get_messages(
    service, fixture.community_id, CHANNEL_ID);
  g_assert_cmpuint(g_list_model_get_n_items(messages), ==, 2);
  g_autoptr(GnConcordMessageItem) head = g_list_model_get_item(messages, 0);
  g_autoptr(GnConcordMessageItem) tail = g_list_model_get_item(messages, 1);
  g_assert_cmpstr(gn_concord_message_item_get_content(head), ==, "first");
  g_assert_cmpstr(gn_concord_message_item_get_content(tail), ==, "second");

  fixture_clear(&fixture);
}

static void assert_rejected(GnConcordCommunityService *service,
                            Fixture *fixture, const MintOptions *options,
                            const char *why) {
  g_autofree gchar *wrap = mint_wrap(fixture, why, 1686840300, options);
  g_assert_false(gn_concord_community_service_ingest_wrap(
    service, fixture->community_id, CHANNEL_ID, wrap));
}

static void test_binding_rejections(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    service_with_membership(&fixture);

  /* A rumor naming another Channel cannot be replayed into this one, even
   * though the key that opened the wrap is genuine (NIP-CAS-0008, CORD-03). */
  MintOptions wrong_channel = default_options();
  wrong_channel.channel_tag =
    "9999999999999999999999999999999999999999999999999999999999999999";
  assert_rejected(service, &fixture, &wrong_channel, "cross-channel replay");

  /* Nor into another epoch. */
  MintOptions wrong_epoch = default_options();
  wrong_epoch.epoch_tag = "4";
  assert_rejected(service, &fixture, &wrong_epoch, "cross-epoch replay");

  /* The Chat Plane's seals MUST be encrypted: a plaintext seal would leave
   * the rumor liftable as a standalone public artifact (CORD-02 §5). */
  MintOptions plaintext_seal = default_options();
  plaintext_seal.seal_kind = CONCORD_SEAL_PLAINTEXT;
  assert_rejected(service, &fixture, &plaintext_seal, "plaintext seal");

  /* An ms outside 0..999 is malformed and its entry is dropped, not
   * interpreted — the excess would smuggle a forged future. */
  MintOptions bad_ms = default_options();
  bad_ms.ms_tag = "1000";
  assert_rejected(service, &fixture, &bad_ms, "out-of-range ms");

  /* Leading zeros are not canonical decimal (CORD-01 "Encoding"). */
  MintOptions padded_ms = default_options();
  padded_ms.ms_tag = "007";
  assert_rejected(service, &fixture, &padded_ms, "non-canonical ms");

  /* A control-edition kind has no business on a Chat Plane. */
  MintOptions wrong_kind = default_options();
  wrong_kind.rumor_kind = CONCORD_KIND_CONTROL_EDITION;
  assert_rejected(service, &fixture, &wrong_kind, "control kind in chat");

  /* A perfectly valid event signed by some other key is not this Channel's
   * stream: only a keyholder can produce events at the derived address. */
  MintOptions foreign_wrap = default_options();
  foreign_wrap.tamper_wrap_author = TRUE;
  assert_rejected(service, &fixture, &foreign_wrap, "foreign stream author");

  GListModel *messages = gn_concord_community_service_get_messages(
    service, fixture.community_id, CHANNEL_ID);
  g_assert_cmpuint(g_list_model_get_n_items(messages), ==, 0);

  fixture_clear(&fixture);
}

static void test_wrong_channel_key_cannot_open(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    gn_concord_community_service_new_offline(fixture.author_pubkey);

  /* Same Community, same Channel id — but the invite granted a different
   * Channel key, so the wrap sits at an address this member cannot even
   * identify, let alone decrypt. */
  g_autofree gchar *bundle = build_bundle(
    &fixture,
    "7777777777777777777777777777777777777777777777777777777777777777", 0, 1);
  g_autoptr(GError) error = NULL;
  g_assert_true(
    gn_concord_community_service_accept_bundle(service, bundle, &error));

  MintOptions options = default_options();
  g_autofree gchar *wrap = mint_wrap(&fixture, "not for you", 1686840217,
                                     &options);
  g_assert_false(gn_concord_community_service_ingest_wrap(
    service, fixture.community_id, CHANNEL_ID, wrap));

  fixture_clear(&fixture);
}

static void test_public_channel_uses_community_root(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    gn_concord_community_service_new_offline(fixture.author_pubkey);

  /* A public Channel carries no key of its own: it derives from the
   * community_root and rotates with the base (CORD-03). */
  g_autofree gchar *bundle = build_bundle(&fixture, NULL, 0, 1);
  g_autoptr(GError) error = NULL;
  g_assert_true(
    gn_concord_community_service_accept_bundle(service, bundle, &error));

  g_autoptr(GnConcordCommunityItem) item =
    gn_concord_community_service_lookup_community(service,
                                                  fixture.community_id);
  g_autoptr(GnConcordChannelItem) channel =
    gn_concord_community_item_find_channel(item, CHANNEL_ID);
  g_assert_nonnull(channel);
  g_assert_false(gn_concord_channel_item_get_is_private(channel));

  uint8_t secret[32], id[32];
  nostr_concord_hex_decode_32(COMMUNITY_ROOT, secret);
  nostr_concord_hex_decode_32(CHANNEL_ID, id);
  nostr_concord_group_key_t key;
  g_assert_cmpint(nostr_concord_channel_key(secret, id, TEST_EPOCH, &key), ==,
                  NOSTR_CONCORD_OK);
  char expected[65];
  nostr_concord_hex_encode_32(key.pk, expected);
  g_assert_cmpstr(gn_concord_channel_item_get_stream_pubkey(channel), ==,
                  expected);
  nostr_concord_group_key_clear(&key);

  fixture_clear(&fixture);
}

static void test_reject_oversized_bundle(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    gn_concord_community_service_new_offline(fixture.author_pubkey);

  /* A bundle is attacker-crafted input reached by following a link: bound it
   * before allocating, or a hostile link is an unbounded-allocation vector. */
  g_autofree gchar *bundle = build_bundle(
    &fixture, CHANNEL_KEY, 0, CONCORD_MAX_CHANNELS_IN_INVITE + 1);
  g_autoptr(GError) error = NULL;
  g_assert_false(
    gn_concord_community_service_accept_bundle(service, bundle, &error));
  g_assert_nonnull(error);

  fixture_clear(&fixture);
}

typedef struct {
  GMainLoop *loop;
  gboolean ok;
  gchar *message;
} PublishResult;

static void on_publish_finished(GObject *source, GAsyncResult *result,
                                gpointer user_data) {
  PublishResult *outcome = user_data;
  g_autoptr(GError) error = NULL;
  outcome->ok = gn_concord_community_service_publish_message_finish(
    GN_CONCORD_COMMUNITY_SERVICE(source), result, &error);
  outcome->message = error ? g_strdup(error->message) : NULL;
  g_main_loop_quit(outcome->loop);
}

static void publish_and_expect_failure(GnConcordCommunityService *service,
                                       const char *community_id,
                                       const char *channel_id) {
  PublishResult outcome = { .loop = g_main_loop_new(NULL, FALSE) };
  gn_concord_community_service_publish_message_async(
    service, community_id, channel_id, "hello", NULL, on_publish_finished,
    &outcome);
  g_main_loop_run(outcome.loop);
  g_assert_false(outcome.ok);
  g_assert_nonnull(outcome.message);
  g_free(outcome.message);
  g_main_loop_unref(outcome.loop);
}

static void test_publish_without_signer_fails_cleanly(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    service_with_membership(&fixture);

  /* Offline there is no host context, so publishing must refuse through the
   * task rather than reach into a NULL context. */
  publish_and_expect_failure(service, fixture.community_id, CHANNEL_ID);
  /* A channel this member holds no key for is refused the same way, before
   * any signature is requested. */
  publish_and_expect_failure(service, fixture.community_id, "not-a-channel");

  GListModel *messages = gn_concord_community_service_get_messages(
    service, fixture.community_id, CHANNEL_ID);
  g_assert_cmpuint(g_list_model_get_n_items(messages), ==, 0);

  fixture_clear(&fixture);
}

/* The mint path, end to end: the service builds rumor, seal and wrap, the
 * host signs the seal, and the wrap that lands on the wire is one the
 * service's *own* reader accepts. Writer and reader agreeing is the whole
 * contract — a client that mints what it cannot read is silently broken. */
static void test_publish_roundtrip(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);

  gn_concord_test_reset();
  gn_concord_test_signer_sk = AUTHOR_SK;
  gn_concord_test_user_pubkey = fixture.author_pubkey;

  /* A non-NULL context is all the service needs; every host call it makes
   * lands in the offline stubs. */
  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;
  g_autoptr(GnConcordCommunityService) service =
    gn_concord_community_service_new(context);
  g_autofree gchar *bundle = build_bundle(&fixture, CHANNEL_KEY, 0, 1);
  g_autoptr(GError) error = NULL;
  g_assert_true(
    gn_concord_community_service_accept_bundle(service, bundle, &error));
  g_assert_no_error(error);

  PublishResult outcome = { .loop = g_main_loop_new(NULL, FALSE) };
  gn_concord_community_service_publish_message_async(
    service, fixture.community_id, CHANNEL_ID, "sealed and wrapped", NULL,
    on_publish_finished, &outcome);
  g_main_loop_run(outcome.loop);
  g_assert_true(outcome.ok);
  g_free(outcome.message);
  g_main_loop_unref(outcome.loop);

  g_assert_nonnull(gn_concord_test_published_json);

  /* What went to the relay is a kind-1059 wrap authored by the Channel's
   * derived stream key — never by the member. (The Community List rides the
   * same publish path, so the wrap is found by kind, not by recency.) */
  g_autofree gchar *wrap_json = NULL;
  for (guint i = 0; i < gn_concord_test_published->len; i++) {
    const char *json = g_ptr_array_index(gn_concord_test_published, i);
    g_autoptr(NostrEvent) candidate = nostr_event_new();
    if (nostr_event_deserialize_compact(candidate, json, NULL) &&
        nostr_event_get_kind(candidate) == CONCORD_STREAM_WRAP)
      wrap_json = g_strdup(json);
  }
  g_assert_nonnull(wrap_json);
  g_autoptr(NostrEvent) wrap = nostr_event_new();
  g_assert_true(nostr_event_deserialize_compact(wrap, wrap_json, NULL));
  g_assert_cmpint(nostr_event_get_kind(wrap), ==, CONCORD_STREAM_WRAP);
  g_assert_cmpstr(nostr_event_get_pubkey(wrap), !=, fixture.author_pubkey);
  g_autoptr(GnConcordCommunityItem) item =
    gn_concord_community_service_lookup_community(service,
                                                  fixture.community_id);
  g_autoptr(GnConcordChannelItem) channel =
    gn_concord_community_item_find_channel(item, CHANNEL_ID);
  g_assert_cmpstr(nostr_event_get_pubkey(wrap), ==,
                  gn_concord_channel_item_get_stream_pubkey(channel));

  /* And feeding it back through the reader yields the message, attributed to
   * the sealing member. */
  g_assert_true(gn_concord_community_service_ingest_wrap(
    service, fixture.community_id, CHANNEL_ID, wrap_json));
  GListModel *messages = gn_concord_community_service_get_messages(
    service, fixture.community_id, CHANNEL_ID);
  g_assert_cmpuint(g_list_model_get_n_items(messages), ==, 1);
  g_autoptr(GnConcordMessageItem) message = g_list_model_get_item(messages, 0);
  g_assert_cmpstr(gn_concord_message_item_get_content(message), ==,
                  "sealed and wrapped");
  g_assert_cmpstr(gn_concord_message_item_get_author(message), ==,
                  fixture.author_pubkey);
  g_assert_cmpint(gn_concord_message_item_get_kind(message), ==,
                  CONCORD_KIND_MESSAGE);

  gn_concord_community_service_shutdown(service);
  gn_concord_test_reset();
  fixture_clear(&fixture);
}

/* ------------------------------------------------------------------ *
 * the Control Plane fold (CORD-04)
 *
 * The bundle a member joined with is a join-time snapshot; the fold is the
 * authority. Every case here mints its own editions in-process — wrap,
 * plaintext seal, kind-3308 rumor — so the whole fold is exercisable with no
 * relay and no signer.
 * ------------------------------------------------------------------ */

#define CONTROL_ROOT "8888888888888888888888888888888888888888888888888888888888888888"
#define ADMIN_SK "9999999999999999999999999999999999999999999999999999999999999999"
#define ROLE_ID "abababababababababababababababababababababababababababababababab"
#define SECOND_CHANNEL "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"

typedef struct {
  const char *actor_sk;
  guint vsk;
  const char *eid;
  guint64 version;
  const char *prev; /* the superseded edition's hash, NULL on the first */
  const char *content;
  const char *vac_eid;
  guint64 vac_version;
  const char *vac_hash;
  /* The Control Plane's seal MUST be plaintext; this mints the wrong one. */
  gboolean encrypted_seal;
  /* Sign the wrap with the community_root-derived read key rather than the
   * control_root-derived signer: the legacy, pre-split address. */
  gboolean legacy_address;
} EditionOptions;

static void control_keys(const Fixture *fixture, gboolean legacy,
                         nostr_concord_group_key_t *read_key,
                         nostr_concord_group_key_t *signer_key) {
  uint8_t root[32], id[32], control_root[32];
  g_assert_true(nostr_concord_hex_decode_32(COMMUNITY_ROOT, root));
  g_assert_true(nostr_concord_hex_decode_32(fixture->community_id, id));
  g_assert_cmpint(nostr_concord_control_read_key(root, id, TEST_EPOCH, read_key),
                  ==, NOSTR_CONCORD_OK);
  if (legacy) return;
  g_assert_true(nostr_concord_hex_decode_32(CONTROL_ROOT, control_root));
  g_assert_cmpint(
    nostr_concord_control_signer_key(control_root, id, TEST_EPOCH, signer_key),
    ==, NOSTR_CONCORD_OK);
}

/* Mints one Control Plane edition and reports its CORD-04 §1 hash, which is
 * what the next edition's `ep` and any `vac` citation must carry. */
static gchar *mint_edition(const Fixture *fixture,
                           const EditionOptions *options, gchar **out_hash) {
  nostr_concord_group_key_t read_key, signer_key;
  control_keys(fixture, options->legacy_address, &read_key, &signer_key);
  const nostr_concord_group_key_t *stream =
    options->legacy_address ? &read_key : &signer_key;

  g_autofree gchar *actor = nostr_key_get_public(options->actor_sk);
  g_autofree gchar *vsk_text = g_strdup_printf("%u", options->vsk);
  g_autofree gchar *version_text =
    g_strdup_printf("%" G_GUINT64_FORMAT, options->version);

  g_autoptr(NostrEvent) rumor = nostr_event_new();
  nostr_event_set_kind(rumor, CONCORD_KIND_CONTROL_EDITION);
  nostr_event_set_pubkey(rumor, actor);
  nostr_event_set_created_at(rumor, 1686840217);
  nostr_event_set_content(rumor, options->content);

  NostrTags *tags = nostr_tags_new(0);
  tags = nostr_tags_append_unique(tags, nostr_tag_new("vsk", vsk_text, NULL));
  tags = nostr_tags_append_unique(tags, nostr_tag_new("eid", options->eid, NULL));
  tags = nostr_tags_append_unique(tags, nostr_tag_new("ev", version_text, NULL));
  if (options->prev)
    tags = nostr_tags_append_unique(tags,
                                    nostr_tag_new("ep", options->prev, NULL));
  if (options->vac_eid) {
    g_autofree gchar *vac_version =
      g_strdup_printf("%" G_GUINT64_FORMAT, options->vac_version);
    tags = nostr_tags_append_unique(
      tags, nostr_tag_new("vac", options->vac_eid, vac_version,
                          options->vac_hash, NULL));
  }
  nostr_event_set_tags(rumor, tags);

  g_autofree gchar *rumor_json = nostr_event_serialize_compact(rumor);
  g_assert_nonnull(rumor_json);

  /* The plaintext seal carries the rumor's serialized JSON byte-verbatim,
   * which is what lets a compaction re-wrap it with its signature intact. */
  g_autoptr(NostrEvent) seal = nostr_event_new();
  char *encrypted = NULL;
  if (options->encrypted_seal) {
    g_assert_cmpint(
      nostr_concord_stream_seal(read_key.conv_key, rumor_json, &encrypted), ==,
      NOSTR_CONCORD_OK);
    nostr_event_set_kind(seal, CONCORD_SEAL_ENCRYPTED);
    nostr_event_set_content(seal, encrypted);
    free(encrypted);
  } else {
    nostr_event_set_kind(seal, CONCORD_SEAL_PLAINTEXT);
    nostr_event_set_content(seal, rumor_json);
  }
  nostr_event_set_pubkey(seal, actor);
  nostr_event_set_created_at(seal, 1686840217);
  nostr_event_set_tags(seal, nostr_tags_new(0));
  g_assert_cmpint(nostr_event_sign(seal, options->actor_sk), ==, 0);
  g_autofree gchar *seal_json = nostr_event_serialize_compact(seal);

  char *wrap_content = NULL;
  g_assert_cmpint(
    nostr_concord_stream_seal(read_key.conv_key, seal_json, &wrap_content), ==,
    NOSTR_CONCORD_OK);

  char stream_sk[65], stream_pk[65];
  nostr_concord_hex_encode_32(stream->sk, stream_sk);
  nostr_concord_hex_encode_32(stream->pk, stream_pk);

  g_autoptr(NostrEvent) wrap = nostr_event_new();
  nostr_event_set_kind(wrap, CONCORD_STREAM_WRAP);
  nostr_event_set_pubkey(wrap, stream_pk);
  nostr_event_set_created_at(wrap, 1686840217);
  nostr_event_set_content(wrap, wrap_content);
  g_autofree gchar *ephemeral_sk = nostr_key_generate_private();
  g_autofree gchar *ephemeral_pk = nostr_key_get_public(ephemeral_sk);
  nostr_event_set_tags(wrap,
                       nostr_tags_new(1, nostr_tag_new("p", ephemeral_pk,
                                                       NULL)));
  free(wrap_content);
  g_assert_cmpint(nostr_event_sign(wrap, stream_sk), ==, 0);

  if (out_hash) {
    uint8_t eid[32], prev[32], hash[32];
    g_assert_true(nostr_concord_hex_decode_32(options->eid, eid));
    if (options->prev)
      g_assert_true(nostr_concord_hex_decode_32(options->prev, prev));
    g_assert_cmpint(
      nostr_concord_edition_hash(eid, options->version,
                                 options->prev ? prev : NULL,
                                 (const uint8_t *)options->content,
                                 strlen(options->content), hash),
      ==, NOSTR_CONCORD_OK);
    char hex[65];
    nostr_concord_hex_encode_32(hash, hex);
    *out_hash = g_strdup(hex);
  }

  nostr_concord_group_key_clear(&read_key);
  if (!options->legacy_address) nostr_concord_group_key_clear(&signer_key);
  return nostr_event_serialize_compact(wrap);
}

static EditionOptions owner_edition(guint vsk, const char *eid, guint64 version,
                                    const char *content) {
  EditionOptions options = { 0 };
  options.actor_sk = OWNER_SK;
  options.vsk = vsk;
  options.eid = eid;
  options.version = version;
  options.content = content;
  options.legacy_address = TRUE;
  return options;
}

static gboolean ingest_edition(GnConcordCommunityService *service,
                               const Fixture *fixture,
                               const EditionOptions *options,
                               gchar **out_hash) {
  g_autofree gchar *wrap = mint_edition(fixture, options, out_hash);
  return gn_concord_community_service_ingest_control_wrap(
    service, fixture->community_id, wrap);
}

static gchar *grant_coordinate(const Fixture *fixture, const char *member) {
  uint8_t community[32], member_bytes[32], eid[32];
  g_assert_true(nostr_concord_hex_decode_32(fixture->community_id, community));
  g_assert_true(nostr_concord_hex_decode_32(member, member_bytes));
  g_assert_cmpint(nostr_concord_grant_locator(community, member_bytes, eid), ==,
                  NOSTR_CONCORD_OK);
  char hex[65];
  nostr_concord_hex_encode_32(eid, hex);
  return g_strdup(hex);
}

static gchar *banlist_coordinate(const Fixture *fixture) {
  uint8_t community[32], eid[32];
  g_assert_true(nostr_concord_hex_decode_32(fixture->community_id, community));
  g_assert_cmpint(nostr_concord_banlist_locator(community, eid), ==,
                  NOSTR_CONCORD_OK);
  char hex[65];
  nostr_concord_hex_encode_32(eid, hex);
  return g_strdup(hex);
}

/* The name and relays an invite carries are a preview so a parked invite can
 * render; the fold replaces both (CORD-02 §6). */
static void test_control_metadata_is_authority(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    service_with_membership(&fixture);

  g_autoptr(GnConcordCommunityItem) item =
    gn_concord_community_service_lookup_community(service,
                                                  fixture.community_id);
  g_assert_cmpstr(gn_concord_community_item_get_name(item), ==,
                  "Incident Response");

  EditionOptions metadata = owner_edition(
    CONCORD_VSK_METADATA, fixture.community_id, 1,
    "{\"name\":\"Vector\",\"description\":\"No compromises.\","
    "\"relays\":[\"wss://folded.example\"]}");
  g_assert_true(ingest_edition(service, &fixture, &metadata, NULL));

  g_assert_cmpstr(gn_concord_community_item_get_name(item), ==, "Vector");
  g_assert_cmpstr(gn_concord_community_item_get_description(item), ==,
                  "No compromises.");
  g_assert_cmpstr(gn_concord_community_item_get_primary_relay(item), ==,
                  "wss://folded.example");

  /* A relay re-delivering the same edition is one edition: the fold dedupes
   * on the *rumor* id, never the outer wrap's, which differs per re-wrap. */
  g_autofree gchar *rewrapped = mint_edition(&fixture, &metadata, NULL);
  g_assert_false(gn_concord_community_service_ingest_control_wrap(
    service, fixture.community_id, rewrapped));
  g_assert_cmpstr(gn_concord_community_item_get_name(item), ==, "Vector");

  fixture_clear(&fixture);
}

/* A Channel is *defined* in the Control Plane, so a member learns of Channels
 * no invite ever granted — and a rename or a deletion is an authorized,
 * convergent edit rather than a new Channel (CORD-03 §2). */
static void test_control_defines_channels(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    service_with_membership(&fixture);
  g_autoptr(GnConcordCommunityItem) item =
    gn_concord_community_service_lookup_community(service,
                                                  fixture.community_id);
  g_assert_cmpuint(gn_concord_community_item_get_channel_count(item), ==, 1);

  g_autofree gchar *first = NULL;
  EditionOptions define = owner_edition(CONCORD_VSK_CHANNEL, SECOND_CHANNEL, 1,
                                        "{\"name\":\"lounge\","
                                        "\"private\":false}");
  g_assert_true(ingest_edition(service, &fixture, &define, &first));
  g_assert_cmpuint(gn_concord_community_item_get_channel_count(item), ==, 2);

  /* A public Channel needs no delivery: its key derives from the
   * community_root, so it is readable the moment it is defined (CORD-03 §1). */
  g_autoptr(GnConcordChannelItem) channel =
    gn_concord_community_item_find_channel(item, SECOND_CHANNEL);
  g_assert_nonnull(channel);
  g_assert_cmpstr(gn_concord_channel_item_get_name(channel), ==, "lounge");
  g_assert_false(gn_concord_channel_item_get_is_private(channel));

  uint8_t root[32], id[32];
  nostr_concord_hex_decode_32(COMMUNITY_ROOT, root);
  nostr_concord_hex_decode_32(SECOND_CHANNEL, id);
  nostr_concord_group_key_t key;
  g_assert_cmpint(nostr_concord_channel_key(root, id, TEST_EPOCH, &key), ==,
                  NOSTR_CONCORD_OK);
  char expected[65];
  nostr_concord_hex_encode_32(key.pk, expected);
  g_assert_cmpstr(gn_concord_channel_item_get_stream_pubkey(channel), ==,
                  expected);
  nostr_concord_group_key_clear(&key);

  /* A rename folds onto the same channel_id. */
  g_autofree gchar *second = NULL;
  EditionOptions rename = owner_edition(CONCORD_VSK_CHANNEL, SECOND_CHANNEL, 2,
                                        "{\"name\":\"the-lounge\","
                                        "\"private\":false}");
  rename.prev = first;
  g_assert_true(ingest_edition(service, &fixture, &rename, &second));
  g_autoptr(GnConcordChannelItem) renamed =
    gn_concord_community_item_find_channel(item, SECOND_CHANNEL);
  g_assert_cmpstr(gn_concord_channel_item_get_name(renamed), ==, "the-lounge");

  /* Refuse-downgrade: a relay replaying the first edition cannot revert it. */
  g_autofree gchar *replay = mint_edition(&fixture, &define, NULL);
  gn_concord_community_service_ingest_control_wrap(
    service, fixture.community_id, replay);
  g_autoptr(GnConcordChannelItem) still =
    gn_concord_community_item_find_channel(item, SECOND_CHANNEL);
  g_assert_cmpstr(gn_concord_channel_item_get_name(still), ==, "the-lounge");

  /* A version 3 whose `prev` doesn't name the version 2 this client holds is
   * a gap, not a head: the chain-intact rule drops it. */
  EditionOptions forked = owner_edition(
    CONCORD_VSK_CHANNEL, SECOND_CHANNEL, 3,
    "{\"name\":\"forked\",\"private\":false}");
  forked.prev =
    "1111111111111111111111111111111111111111111111111111111111111111";
  g_assert_true(ingest_edition(service, &fixture, &forked, NULL));
  g_autoptr(GnConcordChannelItem) unforked =
    gn_concord_community_item_find_channel(item, SECOND_CHANNEL);
  g_assert_cmpstr(gn_concord_channel_item_get_name(unforked), ==,
                  "the-lounge");

  /* Deletion is terminal: the Channel drops from display. */
  EditionOptions deleted = owner_edition(
    CONCORD_VSK_CHANNEL, SECOND_CHANNEL, 3,
    "{\"name\":\"the-lounge\",\"private\":false,\"deleted\":true}");
  deleted.prev = second;
  g_assert_true(ingest_edition(service, &fixture, &deleted, NULL));
  g_assert_null(
    gn_concord_community_item_find_channel(item, SECOND_CHANNEL));
  g_assert_cmpuint(gn_concord_community_item_get_channel_count(item), ==, 1);

  fixture_clear(&fixture);
}

/* A private Channel the Control Plane defines but no invite delivered a key
 * for is listed and unreadable — never silently derived from the
 * community_root, which would address a plane nobody writes (CORD-03 §1). */
static void test_control_private_channel_without_key(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    service_with_membership(&fixture);
  g_autoptr(GnConcordCommunityItem) item =
    gn_concord_community_service_lookup_community(service,
                                                  fixture.community_id);

  EditionOptions define = owner_edition(CONCORD_VSK_CHANNEL, SECOND_CHANNEL, 1,
                                        "{\"name\":\"staff\","
                                        "\"private\":true}");
  g_assert_true(ingest_edition(service, &fixture, &define, NULL));

  g_autoptr(GnConcordChannelItem) channel =
    gn_concord_community_item_find_channel(item, SECOND_CHANNEL);
  g_assert_nonnull(channel);
  g_assert_true(gn_concord_channel_item_get_is_private(channel));
  g_assert_null(gn_concord_channel_item_get_stream_pubkey(channel));

  fixture_clear(&fixture);
}

/* Authority is rejection, not prevention: any control_root holder can publish,
 * and everyone else drops what doesn't map to a qualifying rank (CORD-04). */
static void test_control_authority_is_rejection(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    service_with_membership(&fixture);
  g_autoptr(GnConcordCommunityItem) item =
    gn_concord_community_service_lookup_community(service,
                                                  fixture.community_id);
  g_autofree gchar *admin = nostr_key_get_public(ADMIN_SK);

  /* A perfectly valid edition from an npub the Roster ranks nowhere. The wrap
   * verifies — they hold the write key — and it changes nothing. */
  EditionOptions rogue = owner_edition(CONCORD_VSK_METADATA,
                                       fixture.community_id, 1,
                                       "{\"name\":\"Seized\"}");
  rogue.actor_sk = ADMIN_SK;
  g_assert_true(ingest_edition(service, &fixture, &rogue, NULL));
  g_assert_cmpstr(gn_concord_community_item_get_name(item), ==,
                  "Incident Response");

  /* The owner mints a Role carrying MANAGE_METADATA and grants it. Neither
   * hands over a key: a Grant confers rank, never a secret. */
  g_autofree gchar *role_content = g_strdup_printf(
    "{\"role_id\":\"%s\",\"name\":\"Editor\",\"position\":5,"
    "\"permissions\":\"%" G_GUINT64_FORMAT "\"}",
    ROLE_ID, CONCORD_PERM_MANAGE_METADATA);
  EditionOptions role =
    owner_edition(CONCORD_VSK_ROLE, ROLE_ID, 1, role_content);
  g_assert_true(ingest_edition(service, &fixture, &role, NULL));

  g_autofree gchar *grant_eid = grant_coordinate(&fixture, admin);
  g_autofree gchar *grant_content = g_strdup_printf(
    "{\"member\":\"%s\",\"role_ids\":[\"%s\"]}", admin, ROLE_ID);
  EditionOptions grant =
    owner_edition(CONCORD_VSK_GRANT, grant_eid, 1, grant_content);
  g_assert_true(ingest_edition(service, &fixture, &grant, NULL));

  /* The rogue edition is already held; the Grant arriving *later* is what
   * makes it fold. An edition is judged at fold time, never at arrival. */
  g_assert_cmpstr(gn_concord_community_item_get_name(item), ==, "Seized");

  /* But rank has a ceiling: no edition may claim a position at or above its
   * own signer, so a position-5 Editor cannot mint a position-1 peer of the
   * owner's deputies. */
  g_autofree gchar *promotion = g_strdup_printf(
    "{\"role_id\":\"%s\",\"name\":\"Overlord\",\"position\":1,"
    "\"permissions\":\"%" G_GUINT64_FORMAT "\"}",
    ROLE_ID, CONCORD_PERM_MANAGE_ROLES | CONCORD_PERM_MANAGE_METADATA);
  EditionOptions self_promotion =
    owner_edition(CONCORD_VSK_ROLE, ROLE_ID, 2, promotion);
  self_promotion.actor_sk = ADMIN_SK;
  g_autofree gchar *role_hash = NULL;
  ingest_edition(service, &fixture, &role, &role_hash);
  self_promotion.prev = role_hash;
  g_assert_true(ingest_edition(service, &fixture, &self_promotion, NULL));
  g_assert_cmpuint(
    gn_concord_community_service_get_position(service, fixture.community_id,
                                              admin),
    ==, 5);

  /* A Grant is honored only at its own derived coordinate: forging one into
   * someone else's slot fails however validly it is signed. */
  g_autofree gchar *squatted = g_strdup_printf(
    "{\"member\":\"%s\",\"role_ids\":[\"%s\"]}", fixture.author_pubkey,
    ROLE_ID);
  EditionOptions misplaced =
    owner_edition(CONCORD_VSK_GRANT, ROLE_ID, 1, squatted);
  ingest_edition(service, &fixture, &misplaced, NULL);
  g_assert_cmpuint(
    gn_concord_community_service_get_position(service, fixture.community_id,
                                              fixture.author_pubkey),
    ==, CONCORD_POSITION_LAST);

  fixture_clear(&fixture);
}

/* A reader will not honor an action until it has synced the Grant it cites,
 * and a citation whose hash doesn't match parks exactly like an unsynced one
 * (CORD-04 §5). It parks only its own author's action. */
static void test_control_vac_blocks_until_synced(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    service_with_membership(&fixture);
  g_autoptr(GnConcordCommunityItem) item =
    gn_concord_community_service_lookup_community(service,
                                                  fixture.community_id);
  g_autofree gchar *admin = nostr_key_get_public(ADMIN_SK);

  g_autofree gchar *role_content = g_strdup_printf(
    "{\"role_id\":\"%s\",\"name\":\"Editor\",\"position\":5,"
    "\"permissions\":\"%" G_GUINT64_FORMAT "\"}",
    ROLE_ID, CONCORD_PERM_MANAGE_METADATA);
  EditionOptions role =
    owner_edition(CONCORD_VSK_ROLE, ROLE_ID, 1, role_content);
  g_assert_true(ingest_edition(service, &fixture, &role, NULL));

  g_autofree gchar *grant_eid = grant_coordinate(&fixture, admin);
  g_autofree gchar *grant_content = g_strdup_printf(
    "{\"member\":\"%s\",\"role_ids\":[\"%s\"]}", admin, ROLE_ID);
  EditionOptions grant =
    owner_edition(CONCORD_VSK_GRANT, grant_eid, 1, grant_content);
  g_autofree gchar *grant_hash = NULL;
  g_autofree gchar *grant_wrap = mint_edition(&fixture, &grant, &grant_hash);

  /* The action arrives before the Grant it cites. */
  EditionOptions cited = owner_edition(CONCORD_VSK_METADATA,
                                       fixture.community_id, 1,
                                       "{\"name\":\"Cited\"}");
  cited.actor_sk = ADMIN_SK;
  cited.vac_eid = grant_eid;
  cited.vac_version = 1;
  cited.vac_hash = grant_hash;
  g_assert_true(ingest_edition(service, &fixture, &cited, NULL));
  g_assert_cmpstr(gn_concord_community_item_get_name(item), ==,
                  "Incident Response");

  /* And once the cited Grant lands, it resolves. */
  g_assert_true(gn_concord_community_service_ingest_control_wrap(
    service, fixture.community_id, grant_wrap));
  g_assert_cmpstr(gn_concord_community_item_get_name(item), ==, "Cited");

  fixture_clear(&fixture);
}

/* Every honest client drops *every* event from a banned npub, so a banned
 * member vanishes entirely (CORD-04 §4). */
static void test_control_banlist_silences(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    service_with_membership(&fixture);

  MintOptions options = default_options();
  g_autofree gchar *before =
    mint_wrap(&fixture, "before the ban", 1686840217, &options);
  g_assert_true(gn_concord_community_service_ingest_wrap(
    service, fixture.community_id, CHANNEL_ID, before));

  g_autofree gchar *banlist_eid = banlist_coordinate(&fixture);
  g_autofree gchar *banlist =
    g_strdup_printf("[\"%s\"]", fixture.author_pubkey);
  EditionOptions ban =
    owner_edition(CONCORD_VSK_BANLIST, banlist_eid, 1, banlist);
  g_assert_true(ingest_edition(service, &fixture, &ban, NULL));

  g_autofree gchar *after =
    mint_wrap(&fixture, "after the ban", 1686840400, &options);
  g_assert_false(gn_concord_community_service_ingest_wrap(
    service, fixture.community_id, CHANNEL_ID, after));

  GListModel *messages = gn_concord_community_service_get_messages(
    service, fixture.community_id, CHANNEL_ID);
  g_assert_cmpuint(g_list_model_get_n_items(messages), ==, 1);

  fixture_clear(&fixture);
}

/* The Control Plane's seal MUST be plaintext, and its wrap MUST sit at the
 * address the member holds. */
static void test_control_rejections(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    service_with_membership(&fixture);

  /* An encrypted seal cannot survive a compaction's re-wrap, so the Control
   * Plane refuses one outright (CORD-02 §5). */
  EditionOptions encrypted = owner_edition(
    CONCORD_VSK_METADATA, fixture.community_id, 1, "{\"name\":\"Sealed\"}");
  encrypted.encrypted_seal = TRUE;
  g_assert_false(ingest_edition(service, &fixture, &encrypted, NULL));

  /* This membership carries no control_pk, so its plane is the legacy address
   * — a wrap signed by the split-scheme signer is not on it. The two schemes
   * never collide: different labels, different addresses. */
  EditionOptions split = owner_edition(
    CONCORD_VSK_METADATA, fixture.community_id, 1, "{\"name\":\"Split\"}");
  split.legacy_address = FALSE;
  g_assert_false(ingest_edition(service, &fixture, &split, NULL));

  /* A version 1 carrying a `prev`, and a version 2 carrying none: `prev` is
   * absent on the first edition alone. */
  EditionOptions chained = owner_edition(
    CONCORD_VSK_METADATA, fixture.community_id, 1, "{\"name\":\"Chained\"}");
  chained.prev =
    "2222222222222222222222222222222222222222222222222222222222222222";
  g_assert_false(ingest_edition(service, &fixture, &chained, NULL));

  EditionOptions orphan = owner_edition(
    CONCORD_VSK_METADATA, fixture.community_id, 2, "{\"name\":\"Orphan\"}");
  g_assert_false(ingest_edition(service, &fixture, &orphan, NULL));

  g_autoptr(GnConcordCommunityItem) item =
    gn_concord_community_service_lookup_community(service,
                                                  fixture.community_id);
  g_assert_cmpstr(gn_concord_community_item_get_name(item), ==,
                  "Incident Response");

  fixture_clear(&fixture);
}

/* On a split-key Community the address is the control_root-derived signer's,
 * which a member holds from their invite and never derives (CORD-02 §5). */
static void test_control_split_address(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    gn_concord_community_service_new_offline(fixture.author_pubkey);

  uint8_t control_root[32], id[32];
  g_assert_true(nostr_concord_hex_decode_32(CONTROL_ROOT, control_root));
  g_assert_true(nostr_concord_hex_decode_32(fixture.community_id, id));
  nostr_concord_group_key_t signer;
  g_assert_cmpint(
    nostr_concord_control_signer_key(control_root, id, TEST_EPOCH, &signer), ==,
    NOSTR_CONCORD_OK);
  char control_pk[65];
  nostr_concord_hex_encode_32(signer.pk, control_pk);
  nostr_concord_group_key_clear(&signer);

  g_autofree gchar *bundle = build_bundle(&fixture, CHANNEL_KEY, 0, 1);
  g_autofree gchar *with_control = g_strdup_printf(
    "%.*s,\"control_pk\":\"%s\"}", (int)(strlen(bundle) - 1), bundle,
    control_pk);
  g_autoptr(GError) error = NULL;
  g_assert_true(gn_concord_community_service_accept_bundle(
    service, with_control, &error));
  g_assert_no_error(error);

  EditionOptions metadata = owner_edition(
    CONCORD_VSK_METADATA, fixture.community_id, 1, "{\"name\":\"Split\"}");
  metadata.legacy_address = FALSE;
  g_assert_true(ingest_edition(service, &fixture, &metadata, NULL));

  g_autoptr(GnConcordCommunityItem) item =
    gn_concord_community_service_lookup_community(service,
                                                  fixture.community_id);
  g_assert_cmpstr(gn_concord_community_item_get_name(item), ==, "Split");

  /* And the legacy address is not this Community's plane: only staff can mint
   * a wrap that verifies at the signer's address. */
  EditionOptions legacy = owner_edition(
    CONCORD_VSK_METADATA, fixture.community_id, 2, "{\"name\":\"Legacy\"}");
  legacy.legacy_address = TRUE;
  legacy.prev =
    "3333333333333333333333333333333333333333333333333333333333333333";
  g_assert_false(ingest_edition(service, &fixture, &legacy, NULL));
  g_assert_cmpstr(gn_concord_community_item_get_name(item), ==, "Split");

  fixture_clear(&fixture);
}

#define LINK_SIGNER_A "1010101010101010101010101010101010101010101010101010101010101010"
#define LINK_SIGNER_B "2020202020202020202020202020202020202020202020202020202020202020"
#define LINK_SIGNER_C "3030303030303030303030303030303030303030303030303030303030303030"

static gchar *registry_coordinate(const Fixture *fixture,
                                  const char *creator) {
  uint8_t community[32], creator_bytes[32], eid[32];
  g_assert_true(nostr_concord_hex_decode_32(fixture->community_id, community));
  g_assert_true(nostr_concord_hex_decode_32(creator, creator_bytes));
  g_assert_cmpint(
    nostr_concord_invite_registry_locator(community, creator_bytes, eid), ==,
    NOSTR_CONCORD_OK);
  char hex[65];
  nostr_concord_hex_encode_32(eid, hex);
  return g_strdup(hex);
}

static gboolean links_hold(GPtrArray *links, const char *signer) {
  for (guint i = 0; links && i < links->len; i++)
    if (g_strcmp0(g_ptr_array_index(links, i), signer) == 0) return TRUE;
  return FALSE;
}

/* CORD-05 §5: the Registry is the Invite List's member-facing shadow, and the
 * aggregate active-set it folds to is the Public/Private source of truth. */
static void test_control_invite_registry(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    service_with_membership(&fixture);
  g_autofree gchar *admin = nostr_key_get_public(ADMIN_SK);

  /* A Community with no live link is Private. */
  g_assert_false(
    gn_concord_community_service_is_public(service, fixture.community_id));

  g_autofree gchar *owner_registry =
    registry_coordinate(&fixture, fixture.owner_pubkey);
  g_autofree gchar *owner_content =
    g_strdup_printf("[\"%s\"]", LINK_SIGNER_A);
  EditionOptions mint =
    owner_edition(CONCORD_VSK_INVITE_REGISTRY, owner_registry, 1,
                  owner_content);
  g_autofree gchar *mint_hash = NULL;
  g_assert_true(ingest_edition(service, &fixture, &mint, &mint_hash));
  g_assert_true(
    gn_concord_community_service_is_public(service, fixture.community_id));
  GPtrArray *links =
    gn_concord_community_service_get_invite_links(service,
                                                  fixture.community_id);
  g_assert_cmpuint(links->len, ==, 1);
  g_assert_true(links_hold(links, LINK_SIGNER_A));

  /* A Registry is honored only while its author holds CREATE_INVITE: an npub
   * the Roster ranks nowhere publishes into the void. */
  g_autofree gchar *admin_registry = registry_coordinate(&fixture, admin);
  g_autofree gchar *admin_content =
    g_strdup_printf("[\"%s\"]", LINK_SIGNER_B);
  EditionOptions rogue = owner_edition(CONCORD_VSK_INVITE_REGISTRY,
                                       admin_registry, 1, admin_content);
  rogue.actor_sk = ADMIN_SK;
  g_assert_true(ingest_edition(service, &fixture, &rogue, NULL));
  links = gn_concord_community_service_get_invite_links(service,
                                                        fixture.community_id);
  g_assert_cmpuint(links->len, ==, 1);
  g_assert_false(links_hold(links, LINK_SIGNER_B));

  /* The Grant arriving later is what makes it fold — authority is judged at
   * fold time here exactly as it is for every other entity. */
  g_autofree gchar *role_content = g_strdup_printf(
    "{\"role_id\":\"%s\",\"name\":\"Host\",\"position\":5,"
    "\"permissions\":\"%" G_GUINT64_FORMAT "\"}",
    ROLE_ID, CONCORD_PERM_CREATE_INVITE);
  EditionOptions role =
    owner_edition(CONCORD_VSK_ROLE, ROLE_ID, 1, role_content);
  g_assert_true(ingest_edition(service, &fixture, &role, NULL));
  g_autofree gchar *grant_eid = grant_coordinate(&fixture, admin);
  g_autofree gchar *grant_content = g_strdup_printf(
    "{\"member\":\"%s\",\"role_ids\":[\"%s\"]}", admin, ROLE_ID);
  EditionOptions grant =
    owner_edition(CONCORD_VSK_GRANT, grant_eid, 1, grant_content);
  g_assert_true(ingest_edition(service, &fixture, &grant, NULL));

  links = gn_concord_community_service_get_invite_links(service,
                                                        fixture.community_id);
  g_assert_cmpuint(links->len, ==, 2);
  g_assert_true(links_hold(links, LINK_SIGNER_A));
  g_assert_true(links_hold(links, LINK_SIGNER_B));

  /* Retiring empties the set, and an empty Registry must replace the prior
   * one rather than reading as nothing said. */
  EditionOptions retire = owner_edition(CONCORD_VSK_INVITE_REGISTRY,
                                        owner_registry, 2, "[]");
  retire.prev = mint_hash;
  g_autofree gchar *retire_hash = NULL;
  g_assert_true(ingest_edition(service, &fixture, &retire, &retire_hash));
  links = gn_concord_community_service_get_invite_links(service,
                                                        fixture.community_id);
  g_assert_cmpuint(links->len, ==, 1);
  g_assert_true(links_hold(links, LINK_SIGNER_B));

  /* Each creator owns exactly their own list: a Registry forged into someone
   * else's coordinate is not that creator's, however validly it is signed and
   * however cleanly it chains onto what sits there. */
  g_autofree gchar *squatted = g_strdup_printf("[\"%s\"]", LINK_SIGNER_C);
  EditionOptions forgery = owner_edition(CONCORD_VSK_INVITE_REGISTRY,
                                         owner_registry, 3, squatted);
  forgery.actor_sk = ADMIN_SK;
  forgery.prev = retire_hash;
  g_assert_true(ingest_edition(service, &fixture, &forgery, NULL));
  links = gn_concord_community_service_get_invite_links(service,
                                                        fixture.community_id);
  g_assert_cmpuint(links->len, ==, 1);
  g_assert_false(links_hold(links, LINK_SIGNER_C));

  /* The last retire flips the Community back to Private, which is CORD-06's
   * Refounding trigger. */

  EditionOptions admin_retire = owner_edition(CONCORD_VSK_INVITE_REGISTRY,
                                              admin_registry, 2, "[]");
  admin_retire.actor_sk = ADMIN_SK;
  g_autofree gchar *admin_hash = NULL;
  ingest_edition(service, &fixture, &rogue, &admin_hash);
  admin_retire.prev = admin_hash;
  g_assert_true(ingest_edition(service, &fixture, &admin_retire, NULL));
  g_assert_false(
    gn_concord_community_service_is_public(service, fixture.community_id));

  fixture_clear(&fixture);
}

/* ------------------------------------------------------------------ *
 * shared harness for the tests that need a host
 * ------------------------------------------------------------------ */

/* Every host completion here is a GTask returning on the caller's own stack,
 * so it lands on the next main-context iteration rather than inline. */
static void pump(void) {
  for (int i = 0; i < 64 && g_main_context_iteration(NULL, FALSE); i++)
    ;
}

/* The stored document the fake relay serves, and the filter it saw. */
static gchar *g_stored_list_json = NULL;
static gchar *g_last_filter = NULL;

static GPtrArray *serve_stored_list(const char *filter_json) {
  g_free(g_last_filter);
  g_last_filter = g_strdup(filter_json);
  if (!g_stored_list_json || !strstr(filter_json, "13302")) return NULL;
  GPtrArray *events = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(events, g_strdup(g_stored_list_json));
  return events;
}

/* The last kind-13302 event the service published, or NULL. */
static gchar *published_community_list(void) {
  if (!gn_concord_test_published) return NULL;
  gchar *found = NULL;
  for (guint i = 0; i < gn_concord_test_published->len; i++) {
    const char *json = g_ptr_array_index(gn_concord_test_published, i);
    g_autoptr(NostrEvent) event = nostr_event_new();
    if (!nostr_event_deserialize_compact(event, json, NULL)) continue;
    if (nostr_event_get_kind(event) == CONCORD_COMMUNITY_LIST)
      found = (gchar *)json;
  }
  return found;
}

/* Opens a published List the way another of that npub's devices would: NIP-44
 * under the conversation key they share with themselves. */
static JsonNode *decrypt_published_document(const char *event_json,
                                            const char *author_sk) {
  g_autoptr(NostrEvent) event = nostr_event_new();
  g_assert_true(nostr_event_deserialize_compact(event, event_json, NULL));
  g_assert_cmpint(nostr_event_validate(event, NULL), ==,
                  NOSTR_EVENT_VALIDATION_OK);

  g_autofree gchar *pubkey = nostr_key_get_public(author_sk);
  uint8_t sk[32], pk[32], convkey[32];
  g_assert_true(nostr_hex2bin(sk, author_sk, sizeof(sk)));
  g_assert_true(nostr_hex2bin(pk, pubkey, sizeof(pk)));
  g_assert_cmpint(nostr_nip44_convkey(sk, pk, convkey), ==, 0);

  uint8_t *plaintext = NULL;
  size_t len = 0;
  g_assert_cmpint(
    nostr_nip44_decrypt_v2_with_convkey(convkey, nostr_event_get_content(event),
                                        &plaintext, &len),
    ==, 0);
  g_autofree gchar *document = g_strndup((const char *)plaintext, len);
  free(plaintext);

  JsonParser *parser = json_parser_new();
  g_assert_true(json_parser_load_from_data(parser, document, -1, NULL));
  JsonNode *root = json_node_copy(json_parser_get_root(parser));
  g_object_unref(parser);
  return root;
}

static JsonNode *decrypt_published_list(const char *event_json) {
  return decrypt_published_document(event_json, AUTHOR_SK);
}

static void list_test_begin(Fixture *fixture) {
  fixture_init(fixture);
  gn_concord_test_reset();
  gn_concord_test_signer_sk = AUTHOR_SK;
  gn_concord_test_user_pubkey = fixture->author_pubkey;
  gn_concord_test_query_hook = serve_stored_list;
  g_clear_pointer(&g_stored_list_json, g_free);
  g_clear_pointer(&g_last_filter, g_free);
}

static void list_test_end(Fixture *fixture) {
  gn_concord_test_reset();
  g_clear_pointer(&g_stored_list_json, g_free);
  g_clear_pointer(&g_last_filter, g_free);
  fixture_clear(fixture);
}

/* ------------------------------------------------------------------ *
 * the Guestbook Plane (CORD-02 §5) and Direct Invites (CORD-05 §6)
 * ------------------------------------------------------------------ */

static void guestbook_key(const Fixture *fixture,
                          nostr_concord_group_key_t *out) {
  uint8_t root[32], id[32];
  g_assert_true(nostr_concord_hex_decode_32(COMMUNITY_ROOT, root));
  g_assert_true(nostr_concord_hex_decode_32(fixture->community_id, id));
  g_assert_cmpint(nostr_concord_guestbook_key(root, id, TEST_EPOCH, out), ==,
                  NOSTR_CONCORD_OK);
}

/* One Guestbook rumor, wrapped the way any member publishes: the plane is
 * member-writable by design, so minting one takes nothing but the
 * community_root — which is exactly why the fold trusts the seal's npub for
 * authorship and nothing else. */
static gchar *seal_and_wrap_guestbook_rumor(const Fixture *fixture,
                                            const char *actor_sk,
                                            NostrEvent *rumor,
                                            gint64 created_at) {
  nostr_concord_group_key_t key;
  guestbook_key(fixture, &key);
  g_autofree gchar *actor = nostr_key_get_public(actor_sk);
  g_autofree gchar *rumor_json = nostr_event_serialize_compact(rumor);

  char *seal_content = NULL;
  g_assert_cmpint(
    nostr_concord_stream_seal(key.conv_key, rumor_json, &seal_content), ==,
    NOSTR_CONCORD_OK);
  g_autoptr(NostrEvent) seal = nostr_event_new();
  nostr_event_set_kind(seal, CONCORD_SEAL_ENCRYPTED);
  nostr_event_set_pubkey(seal, actor);
  nostr_event_set_created_at(seal, created_at);
  nostr_event_set_content(seal, seal_content);
  nostr_event_set_tags(seal, nostr_tags_new(0));
  free(seal_content);
  g_assert_cmpint(nostr_event_sign(seal, actor_sk), ==, 0);
  g_autofree gchar *seal_json = nostr_event_serialize_compact(seal);

  char *wrap_content = NULL;
  g_assert_cmpint(
    nostr_concord_stream_seal(key.conv_key, seal_json, &wrap_content), ==,
    NOSTR_CONCORD_OK);
  char stream_sk[65], stream_pk[65];
  nostr_concord_hex_encode_32(key.sk, stream_sk);
  nostr_concord_hex_encode_32(key.pk, stream_pk);

  g_autoptr(NostrEvent) wrap = nostr_event_new();
  nostr_event_set_kind(wrap, CONCORD_STREAM_WRAP);
  nostr_event_set_pubkey(wrap, stream_pk);
  nostr_event_set_created_at(wrap, created_at);
  nostr_event_set_content(wrap, wrap_content);
  g_autofree gchar *ephemeral_sk = nostr_key_generate_private();
  g_autofree gchar *ephemeral_pk = nostr_key_get_public(ephemeral_sk);
  nostr_event_set_tags(wrap,
                       nostr_tags_new(1, nostr_tag_new("p", ephemeral_pk,
                                                       NULL)));
  free(wrap_content);
  g_assert_cmpint(nostr_event_sign(wrap, stream_sk), ==, 0);
  nostr_concord_group_key_clear(&key);
  return nostr_event_serialize_compact(wrap);
}

static gchar *mint_guestbook_wrap(const Fixture *fixture, const char *actor_sk,
                                  int kind, const char *content,
                                  gint64 created_at, const char *ms_tag,
                                  const char *target) {
  g_autofree gchar *actor = nostr_key_get_public(actor_sk);
  g_autoptr(NostrEvent) rumor = nostr_event_new();
  nostr_event_set_kind(rumor, kind);
  nostr_event_set_pubkey(rumor, actor);
  nostr_event_set_created_at(rumor, created_at);
  nostr_event_set_content(rumor, content);
  NostrTags *tags = nostr_tags_new(1, nostr_tag_new("ms", ms_tag, NULL));
  if (target)
    tags = nostr_tags_append_unique(tags, nostr_tag_new("p", target, NULL));
  nostr_event_set_tags(rumor, tags);
  return seal_and_wrap_guestbook_rumor(fixture, actor_sk, rumor, created_at);
}

/* One snapshot chunk (CORD-02 §5): present members only, all chunks of a set
 * sharing one snapshot id and one created_at. */
static gchar *mint_snapshot_wrap(const Fixture *fixture, const char *actor_sk,
                                 const char *content, gint64 created_at,
                                 const char *ms_tag, const char *snapshot_id,
                                 const char *index, const char *count) {
  g_autofree gchar *actor = nostr_key_get_public(actor_sk);
  g_autoptr(NostrEvent) rumor = nostr_event_new();
  nostr_event_set_kind(rumor, CONCORD_KIND_SNAPSHOT);
  nostr_event_set_pubkey(rumor, actor);
  nostr_event_set_created_at(rumor, created_at);
  nostr_event_set_content(rumor, content);
  NostrTags *tags = nostr_tags_new(1, nostr_tag_new("ms", ms_tag, NULL));
  tags = nostr_tags_append_unique(
    tags, index ? nostr_tag_new("snap", snapshot_id, index, count, NULL)
                : nostr_tag_new("snap", snapshot_id, NULL));
  nostr_event_set_tags(rumor, tags);
  return seal_and_wrap_guestbook_rumor(fixture, actor_sk, rumor, created_at);
}

static gboolean is_member(GnConcordCommunityService *service,
                          const Fixture *fixture, const char *pubkey) {
  g_autoptr(GPtrArray) members =
    gn_concord_community_service_get_members(service, fixture->community_id);
  for (guint i = 0; i < members->len; i++)
    if (g_strcmp0(g_ptr_array_index(members, i), pubkey) == 0) return TRUE;
  return FALSE;
}

/* A Join is a member's own word, and the coalesce keeps one final state per
 * npub: their latest Join, Leave or Kick wins. */
static void test_guestbook_coalesces(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    service_with_membership(&fixture);

  g_autofree gchar *join = mint_guestbook_wrap(
    &fixture, AUTHOR_SK, CONCORD_KIND_JOIN_LEAVE, "join", 1686840217, "100",
    NULL);
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    service, fixture.community_id, join));
  g_assert_true(is_member(service, &fixture, fixture.author_pubkey));

  /* An older entry never supersedes a newer one, whichever order it arrives
   * in — the comparison basis is created_at * 1000 + ms. */
  g_autofree gchar *stale_leave = mint_guestbook_wrap(
    &fixture, AUTHOR_SK, CONCORD_KIND_JOIN_LEAVE, "leave", 1686840217, "50",
    NULL);
  gn_concord_community_service_ingest_guestbook_wrap(
    service, fixture.community_id, stale_leave);
  g_assert_true(is_member(service, &fixture, fixture.author_pubkey));

  g_autofree gchar *leave = mint_guestbook_wrap(
    &fixture, AUTHOR_SK, CONCORD_KIND_JOIN_LEAVE, "leave", 1686840217, "900",
    NULL);
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    service, fixture.community_id, leave));
  g_assert_false(is_member(service, &fixture, fixture.author_pubkey));

  fixture_clear(&fixture);
}

/* An entry dated more than an hour ahead of the receiver's clock is dropped
 * outright — ample for skew, and a deterrent against squatting "latest" with
 * a forged future date. An out-of-range ms is dropped for the same reason:
 * the excess would smuggle arbitrary "future" past the clock check. */
static void test_guestbook_drops_forged_future(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    service_with_membership(&fixture);

  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *join = mint_guestbook_wrap(
    &fixture, AUTHOR_SK, CONCORD_KIND_JOIN_LEAVE, "join", now, "0", NULL);
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    service, fixture.community_id, join));

  g_autofree gchar *forged = mint_guestbook_wrap(
    &fixture, AUTHOR_SK, CONCORD_KIND_JOIN_LEAVE, "leave", now + 7200, "0",
    NULL);
  g_assert_false(gn_concord_community_service_ingest_guestbook_wrap(
    service, fixture.community_id, forged));
  g_assert_true(is_member(service, &fixture, fixture.author_pubkey));

  /* An hour of skew is tolerated, so this one lands. */
  g_autofree gchar *skewed = mint_guestbook_wrap(
    &fixture, AUTHOR_SK, CONCORD_KIND_JOIN_LEAVE, "leave", now + 60, "0",
    NULL);
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    service, fixture.community_id, skewed));
  g_assert_false(is_member(service, &fixture, fixture.author_pubkey));

  g_autofree gchar *malformed = mint_guestbook_wrap(
    &fixture, AUTHOR_SK, CONCORD_KIND_JOIN_LEAVE, "join", now + 120, "1000",
    NULL);
  g_assert_false(gn_concord_community_service_ingest_guestbook_wrap(
    service, fixture.community_id, malformed));
  g_assert_false(is_member(service, &fixture, fixture.author_pubkey));

  fixture_clear(&fixture);
}

/* A snapshot is the refounder's *secondhand* attestation: it seeds an npub's
 * state at its own timestamp, anything newer supersedes it, and it is honored
 * only from the npub whose Refounding minted the epoch (CORD-02 §5). */
static void test_guestbook_snapshot_seeds(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    service_with_membership(&fixture);
  g_autofree gchar *admin = nostr_key_get_public(ADMIN_SK);
  g_autofree gchar *listed = g_strdup_printf(
    "[\"%s\",\"%s\"]", fixture.author_pubkey, admin);

  /* With no Rotator known, an epoch nobody has proven they minted honors no
   * snapshot at all — which is where this client sits until CORD-06 lands. */
  g_autofree gchar *unattributed = mint_snapshot_wrap(
    &fixture, OWNER_SK, listed, 1686840000, "0", "snap-1", "1", "1");
  g_assert_false(gn_concord_community_service_ingest_guestbook_wrap(
    service, fixture.community_id, unattributed));
  g_assert_false(is_member(service, &fixture, admin));

  gn_concord_community_service_set_refounder(service, fixture.community_id,
                                             fixture.owner_pubkey);

  /* From anyone but that npub it stays secondhand hearsay. */
  g_autofree gchar *impostor = mint_snapshot_wrap(
    &fixture, ADMIN_SK, listed, 1686840000, "0", "snap-2", "1", "1");
  g_assert_false(gn_concord_community_service_ingest_guestbook_wrap(
    service, fixture.community_id, impostor));
  g_assert_false(is_member(service, &fixture, admin));

  /* From the refounder it seeds every npub it lists, none of whom published
   * anything in this epoch. */
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    service, fixture.community_id, unattributed));
  g_assert_true(is_member(service, &fixture, fixture.author_pubkey));
  g_assert_true(is_member(service, &fixture, admin));

  /* A member's own word, newer, supersedes the attestation about them. */
  g_autofree gchar *leave = mint_guestbook_wrap(
    &fixture, AUTHOR_SK, CONCORD_KIND_JOIN_LEAVE, "leave", 1686840100, "0",
    NULL);
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    service, fixture.community_id, leave));
  g_assert_false(is_member(service, &fixture, fixture.author_pubkey));

  /* And a snapshot newer than that Leave seeds them present again: the
   * refounder is attesting to the state at their own, later timestamp. */
  g_autofree gchar *later = mint_snapshot_wrap(
    &fixture, OWNER_SK, listed, 1686840200, "0", "snap-3", "1", "1");
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    service, fixture.community_id, later));
  g_assert_true(is_member(service, &fixture, fixture.author_pubkey));

  fixture_clear(&fixture);
}

/* Chunks share one snapshot id and one timestamp but are independently
 * useful: a partially received snapshot seeds whoever arrived, and absence
 * from one means "no seed", never a negative state. */
static void test_guestbook_snapshot_chunks_stand_alone(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    service_with_membership(&fixture);
  g_autofree gchar *admin = nostr_key_get_public(ADMIN_SK);
  gn_concord_community_service_set_refounder(service, fixture.community_id,
                                             fixture.owner_pubkey);

  /* Chunk 1 of 2 arrives; chunk 2 never does. */
  g_autofree gchar *first = g_strdup_printf("[\"%s\"]", admin);
  g_autofree gchar *chunk = mint_snapshot_wrap(
    &fixture, OWNER_SK, first, 1686840000, "0", "snap-1", "1", "2");
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    service, fixture.community_id, chunk));
  g_assert_true(is_member(service, &fixture, admin));
  /* Absent from the chunk that arrived is not a Leave. */
  g_assert_false(is_member(service, &fixture, fixture.author_pubkey));

  /* A member's own Join stands regardless of what any chunk says. */
  g_autofree gchar *join = mint_guestbook_wrap(
    &fixture, AUTHOR_SK, CONCORD_KIND_JOIN_LEAVE, "join", 1686839000, "0",
    NULL);
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    service, fixture.community_id, join));
  g_assert_true(is_member(service, &fixture, fixture.author_pubkey));

  /* A chunk claiming a place outside its own set is malformed, and a
   * snapshot with no id cannot be correlated at all. */
  g_autofree gchar *misnumbered = mint_snapshot_wrap(
    &fixture, OWNER_SK, first, 1686840300, "0", "snap-4", "3", "2");
  g_assert_false(gn_concord_community_service_ingest_guestbook_wrap(
    service, fixture.community_id, misnumbered));
  g_autofree gchar *unidentified = mint_snapshot_wrap(
    &fixture, OWNER_SK, first, 1686840400, "0", "", NULL, NULL);
  g_assert_false(gn_concord_community_service_ingest_guestbook_wrap(
    service, fixture.community_id, unidentified));

  fixture_clear(&fixture);
}

/* An author seen publishing is observably present, auto-included even if
 * their Join never arrived — but observation counts *forward* only, so a
 * departed member's old history can never resurrect them. */
static void test_guestbook_observation_counts_forward(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    service_with_membership(&fixture);

  MintOptions options = default_options();
  g_autofree gchar *old_message =
    mint_wrap(&fixture, "before leaving", 1686840100, &options);
  g_assert_true(gn_concord_community_service_ingest_wrap(
    service, fixture.community_id, CHANNEL_ID, old_message));
  /* No Join ever arrived, and they are a member all the same. */
  g_assert_true(is_member(service, &fixture, fixture.author_pubkey));

  g_autofree gchar *leave = mint_guestbook_wrap(
    &fixture, AUTHOR_SK, CONCORD_KIND_JOIN_LEAVE, "leave", 1686840217, "0",
    NULL);
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    service, fixture.community_id, leave));
  g_assert_false(is_member(service, &fixture, fixture.author_pubkey));

  /* Backfilling history older than the Leave must not resurrect them. */
  g_autofree gchar *older =
    mint_wrap(&fixture, "older backfill", 1686840000, &options);
  g_assert_true(gn_concord_community_service_ingest_wrap(
    service, fixture.community_id, CHANNEL_ID, older));
  g_assert_false(is_member(service, &fixture, fixture.author_pubkey));

  /* Activity newer than the Leave does. */
  g_autofree gchar *newer =
    mint_wrap(&fixture, "back again", 1686840400, &options);
  g_assert_true(gn_concord_community_service_ingest_wrap(
    service, fixture.community_id, CHANNEL_ID, newer));
  g_assert_true(is_member(service, &fixture, fixture.author_pubkey));

  fixture_clear(&fixture);
}

/* A Kick is honored only if its signer holds KICK and strictly outranks its
 * target. The Guestbook holds no authority of its own; it asks the Roster. */
static void test_guestbook_kick_needs_authority(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    service_with_membership(&fixture);
  g_autofree gchar *admin = nostr_key_get_public(ADMIN_SK);

  g_autofree gchar *join = mint_guestbook_wrap(
    &fixture, AUTHOR_SK, CONCORD_KIND_JOIN_LEAVE, "join", 1686840217, "0",
    NULL);
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    service, fixture.community_id, join));

  /* From an npub the Roster ranks nowhere, a Kick is just noise. */
  g_autofree gchar *rogue = mint_guestbook_wrap(
    &fixture, ADMIN_SK, CONCORD_KIND_KICK, "", 1686840300, "0",
    fixture.author_pubkey);
  g_assert_false(gn_concord_community_service_ingest_guestbook_wrap(
    service, fixture.community_id, rogue));
  g_assert_true(is_member(service, &fixture, fixture.author_pubkey));

  /* KICK writes to the Guestbook, so it is deliberately *not* one of the six
   * staff bits — but it still takes rank, granted on the Control Plane. */
  g_autofree gchar *role_content = g_strdup_printf(
    "{\"role_id\":\"%s\",\"name\":\"Moderator\",\"position\":5,"
    "\"permissions\":\"%" G_GUINT64_FORMAT "\"}",
    ROLE_ID, CONCORD_PERM_KICK);
  EditionOptions role =
    owner_edition(CONCORD_VSK_ROLE, ROLE_ID, 1, role_content);
  g_assert_true(ingest_edition(service, &fixture, &role, NULL));

  g_autofree gchar *grant_eid = grant_coordinate(&fixture, admin);
  g_autofree gchar *grant_content = g_strdup_printf(
    "{\"member\":\"%s\",\"role_ids\":[\"%s\"]}", admin, ROLE_ID);
  EditionOptions grant =
    owner_edition(CONCORD_VSK_GRANT, grant_eid, 1, grant_content);
  g_assert_true(ingest_edition(service, &fixture, &grant, NULL));

  g_autofree gchar *authorized = mint_guestbook_wrap(
    &fixture, ADMIN_SK, CONCORD_KIND_KICK, "", 1686840400, "0",
    fixture.author_pubkey);
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    service, fixture.community_id, authorized));
  g_assert_false(is_member(service, &fixture, fixture.author_pubkey));

  fixture_clear(&fixture);
}

/* Accepting an invite publishes a self-signed Join, echoing the link's
 * creator and label so per-link usage counters are possible (CORD-05 §1). */
static void test_guestbook_join_announced_on_accept(void) {
  Fixture fixture = { 0 };
  list_test_begin(&fixture);

  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;
  g_autoptr(GnConcordCommunityService) service =
    gn_concord_community_service_new(context);

  g_autofree gchar *bundle = build_bundle(&fixture, CHANNEL_KEY, 0, 1);
  g_autofree gchar *attributed = g_strdup_printf(
    "%.*s,\"creator_npub\":\"%s\",\"label\":\"Conf 2026\"}",
    (int)(strlen(bundle) - 1), bundle, fixture.owner_pubkey);
  g_autoptr(GError) error = NULL;
  g_assert_true(
    gn_concord_community_service_accept_bundle(service, attributed, &error));
  g_assert_no_error(error);
  pump();

  nostr_concord_group_key_t key;
  guestbook_key(&fixture, &key);
  char address[65];
  nostr_concord_hex_encode_32(key.pk, address);
  nostr_concord_group_key_clear(&key);

  gchar *join_wrap = NULL;
  for (guint i = 0; i < gn_concord_test_published->len; i++) {
    gchar *json = g_ptr_array_index(gn_concord_test_published, i);
    g_autoptr(NostrEvent) event = nostr_event_new();
    if (nostr_event_deserialize_compact(event, json, NULL) &&
        g_strcmp0(nostr_event_get_pubkey(event), address) == 0)
      join_wrap = json;
  }
  g_assert_nonnull(join_wrap);

  /* And the client's own reader accepts what it minted: writer and reader
   * agreeing is the whole contract. */
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    service, fixture.community_id, join_wrap));
  g_assert_true(is_member(service, &fixture, fixture.author_pubkey));

  /* Restoring the same membership is not a join and announces nothing. */
  guint published = gn_concord_test_published->len;
  g_assert_true(
    gn_concord_community_service_accept_bundle(service, attributed, &error));
  pump();
  for (guint i = published; i < gn_concord_test_published->len; i++) {
    g_autoptr(NostrEvent) event = nostr_event_new();
    g_assert_true(nostr_event_deserialize_compact(
      event, g_ptr_array_index(gn_concord_test_published, i), NULL));
    g_assert_cmpstr(nostr_event_get_pubkey(event), !=, address);
  }

  gn_concord_community_service_shutdown(service);
  list_test_end(&fixture);
}

/* A Direct Invite is a *standard* NIP-59 giftwrap — ephemeral wrap author,
 * recipient in the `p` tag, kind-13 seal — not CORD-01's reversed stream
 * wrap, and the seal's verified npub proves who invited them. */
static gchar *encrypt_to(const char *sender_sk, const char *recipient_pubkey,
                         const char *plaintext) {
  uint8_t sk[32], pk[32];
  g_assert_true(nostr_hex2bin(sk, sender_sk, sizeof(sk)));
  g_assert_true(nostr_hex2bin(pk, recipient_pubkey, sizeof(pk)));
  char *payload = NULL;
  g_assert_cmpint(nostr_nip44_encrypt_v2(sk, pk, (const uint8_t *)plaintext,
                                         strlen(plaintext), &payload),
                  ==, 0);
  gchar *result = g_strdup(payload);
  free(payload);
  return result;
}

static gchar *mint_direct_invite(const Fixture *fixture, const char *bundle,
                                 const char *recipient, gboolean impersonate) {
  g_autofree gchar *inviter = nostr_key_get_public(OWNER_SK);

  g_autoptr(NostrEvent) rumor = nostr_event_new();
  nostr_event_set_kind(rumor, CONCORD_DIRECT_INVITE);
  nostr_event_set_pubkey(rumor,
                         impersonate ? fixture->author_pubkey : inviter);
  nostr_event_set_created_at(rumor, 1686840217);
  nostr_event_set_content(rumor, bundle);
  nostr_event_set_tags(rumor, nostr_tags_new(0));
  g_autofree gchar *rumor_json = nostr_event_serialize_compact(rumor);

  g_autofree gchar *seal_content =
    encrypt_to(OWNER_SK, recipient, rumor_json);
  g_autoptr(NostrEvent) seal = nostr_event_new();
  nostr_event_set_kind(seal, 13);
  nostr_event_set_pubkey(seal, inviter);
  nostr_event_set_created_at(seal, 1686840217);
  nostr_event_set_content(seal, seal_content);
  nostr_event_set_tags(seal, nostr_tags_new(0));
  g_assert_cmpint(nostr_event_sign(seal, OWNER_SK), ==, 0);
  g_autofree gchar *seal_json = nostr_event_serialize_compact(seal);

  g_autofree gchar *ephemeral_sk = nostr_key_generate_private();
  g_autofree gchar *ephemeral_pk = nostr_key_get_public(ephemeral_sk);
  g_autofree gchar *wrap_content =
    encrypt_to(ephemeral_sk, recipient, seal_json);

  g_autoptr(NostrEvent) wrap = nostr_event_new();
  nostr_event_set_kind(wrap, CONCORD_STREAM_WRAP);
  nostr_event_set_pubkey(wrap, ephemeral_pk);
  nostr_event_set_created_at(wrap, 1686840217);
  nostr_event_set_content(wrap, wrap_content);
  /* The one identifying outer tag Concord permits, and only here: it makes
   * invites indexable without decrypting a whole giftwrap inbox. */
  nostr_event_set_tags(wrap, nostr_tags_new(
    2, nostr_tag_new("p", recipient, NULL),
    nostr_tag_new("k", "3313", NULL)));
  g_assert_cmpint(nostr_event_sign(wrap, ephemeral_sk), ==, 0);
  return nostr_event_serialize_compact(wrap);
}

typedef struct {
  GMainLoop *loop;
  gchar *bundle;
  gchar *message;
} OpenResult;

static void on_direct_opened(GObject *source, GAsyncResult *result,
                             gpointer user_data) {
  OpenResult *outcome = user_data;
  g_autoptr(GError) error = NULL;
  outcome->bundle = gn_concord_community_service_open_direct_invite_finish(
    GN_CONCORD_COMMUNITY_SERVICE(source), result, &error);
  outcome->message = error ? g_strdup(error->message) : NULL;
  g_main_loop_quit(outcome->loop);
}

static gchar *open_direct_invite(GnConcordCommunityService *service,
                                 const char *wrap) {
  OpenResult outcome = { .loop = g_main_loop_new(NULL, FALSE) };
  gn_concord_community_service_open_direct_invite_async(
    service, wrap, NULL, on_direct_opened, &outcome);
  g_main_loop_run(outcome.loop);
  g_main_loop_unref(outcome.loop);
  g_free(outcome.message);
  return outcome.bundle;
}

static void test_direct_invite_roundtrip(void) {
  Fixture fixture = { 0 };
  list_test_begin(&fixture);

  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;
  g_autoptr(GnConcordCommunityService) service =
    gn_concord_community_service_new(context);
  g_autofree gchar *bundle = build_bundle(&fixture, CHANNEL_KEY, 0, 1);

  g_autofree gchar *wrap =
    mint_direct_invite(&fixture, bundle, fixture.author_pubkey, FALSE);
  g_autofree gchar *offered = open_direct_invite(service, wrap);
  g_assert_nonnull(offered);

  /* No coordinate, no token, nothing to fetch — and the bundle validates
   * exactly as a fetched one does. */
  g_autoptr(GError) error = NULL;
  g_assert_true(
    gn_concord_community_service_accept_bundle(service, offered, &error));
  g_assert_no_error(error);
  g_assert_nonnull(gn_concord_community_service_lookup_community(
    service, fixture.community_id));

  /* A rumor claiming an author other than the seal that carried it is a
   * forgery: NIP-59's impersonation check, and renderers display rumor
   * fields. */
  g_autofree gchar *forged =
    mint_direct_invite(&fixture, bundle, fixture.author_pubkey, TRUE);
  g_autofree gchar *refused = open_direct_invite(service, forged);
  g_assert_null(refused);

  /* And a giftwrap addressed to someone else is not this npub's to open. */
  g_autofree gchar *stranger =
    mint_direct_invite(&fixture, bundle, fixture.owner_pubkey, FALSE);
  g_autofree gchar *not_mine = open_direct_invite(service, stranger);
  g_assert_null(not_mine);

  gn_concord_community_service_shutdown(service);
  list_test_end(&fixture);
}

/* ------------------------------------------------------------------ *
 * the Community List (CORD-02 §8)
 * ------------------------------------------------------------------ */


/* Accepting an invite publishes the membership as a self-encrypted kind-13302
 * document, and a *second* device holding nothing but that document and the
 * member's key reconstructs the Community from it. The two halves are the
 * whole point of the List: without the read, memberships are stranded on the
 * device that joined. */
static void test_community_list_roundtrip(void) {
  Fixture fixture = { 0 };
  list_test_begin(&fixture);

  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;
  {
    g_autoptr(GnConcordCommunityService) service =
      gn_concord_community_service_new(context);
    g_autofree gchar *bundle = build_bundle(&fixture, CHANNEL_KEY, 0, 1);
    g_autoptr(GError) error = NULL;
    g_assert_true(
      gn_concord_community_service_accept_bundle(service, bundle, &error));
    g_assert_no_error(error);
    pump();
    gn_concord_community_service_shutdown(service);
  }

  const char *event_json = published_community_list();
  g_assert_nonnull(event_json);
  g_stored_list_json = g_strdup(event_json);

  /* The relay sees a replaceable signed by the member's real key, and nothing
   * else: the join material is inside the NIP-44 payload. */
  g_autoptr(NostrEvent) event = nostr_event_new();
  g_assert_true(nostr_event_deserialize_compact(event, event_json, NULL));
  g_assert_cmpstr(nostr_event_get_pubkey(event), ==, fixture.author_pubkey);
  g_assert_null(strstr(nostr_event_get_content(event), COMMUNITY_ROOT));

  JsonNode *root = decrypt_published_list(event_json);
  JsonObject *document = json_node_get_object(root);
  JsonArray *entries = json_object_get_array_member(document, "entries");
  g_assert_cmpuint(json_array_get_length(entries), ==, 1);
  JsonObject *entry = json_array_get_object_element(entries, 0);
  g_assert_cmpstr(json_object_get_string_member(entry, "community_id"), ==,
                  fixture.community_id);
  g_assert_cmpint(json_object_get_int_member(entry, "added_at"), >, 0);

  /* Join material is the bundle's membership subset: the keys a device needs,
   * and on a first join the seed and the current snapshot are the same one. */
  JsonObject *current = json_object_get_object_member(entry, "current");
  g_assert_cmpstr(json_object_get_string_member(current, "community_root"), ==,
                  COMMUNITY_ROOT);
  g_assert_cmpint(json_object_get_int_member(current, "root_epoch"), ==,
                  TEST_EPOCH);
  JsonObject *seed = json_object_get_object_member(entry, "seed");
  g_assert_cmpint(json_object_get_int_member(seed, "root_epoch"), ==,
                  TEST_EPOCH);
  json_node_free(root);

  /* The second device: same npub, no local state, only the wire document. */
  g_autoptr(GnConcordCommunityService) other =
    gn_concord_community_service_new(context);
  pump();
  g_assert_cmpuint(
    g_list_model_get_n_items(gn_concord_community_service_get_model(other)),
    ==, 1);
  g_autoptr(GnConcordCommunityItem) item =
    gn_concord_community_service_lookup_community(other,
                                                  fixture.community_id);
  g_assert_nonnull(item);
  g_assert_cmpstr(gn_concord_community_item_get_name(item), ==,
                  "Incident Response");
  /* And the reconstruction is complete enough to *read*: the Channel's derived
   * stream address is back, which is the filter everything else hangs off. */
  g_autoptr(GnConcordChannelItem) channel =
    gn_concord_community_item_find_channel(item, CHANNEL_ID);
  g_assert_nonnull(channel);
  g_assert_true(nostr_concord_is_lower_hex_32(
    gn_concord_channel_item_get_stream_pubkey(channel)));
  gn_concord_community_service_shutdown(other);

  list_test_end(&fixture);
}

/* Two clients share this one document, so a republish must carry forward what
 * this client is not the authority on: the tombstones, the fields it doesn't
 * understand, and the memberships it could not adopt. */
static void test_community_list_round_trips_foreign_fields(void) {
  Fixture fixture = { 0 };
  list_test_begin(&fixture);

  /* A document as another client left it: one tombstoned Community, one entry
   * this client cannot use, and unknown fields at both levels. */
  const char *other_id =
    "aaaa111111111111111111111111111111111111111111111111111111111111";
  g_autofree gchar *known = build_bundle(&fixture, CHANNEL_KEY, 0, 1);
  g_autofree gchar *document = g_strdup_printf(
    "{\"entries\":["
    "{\"community_id\":\"%s\",\"added_at\":1719800000000,"
    "\"seed\":%s,\"current\":%s,\"vector/pinned\":true},"
    "{\"community_id\":\"%s\",\"added_at\":1719800000000,"
    "\"current\":{\"community_id\":\"%s\"}}"
    "],\"tombstones\":[{\"community_id\":\"%s\","
    "\"removed_at\":1722400000000}],\"soapbox/schema\":2}",
    fixture.community_id, known, known, other_id, other_id, other_id);

  uint8_t sk[32], pk[32], convkey[32];
  g_assert_true(nostr_hex2bin(sk, AUTHOR_SK, sizeof(sk)));
  g_assert_true(nostr_hex2bin(pk, fixture.author_pubkey, sizeof(pk)));
  g_assert_cmpint(nostr_nip44_convkey(sk, pk, convkey), ==, 0);
  char *payload = NULL;
  g_assert_cmpint(nostr_nip44_encrypt_v2_with_convkey(
                    convkey, (const uint8_t *)document, strlen(document),
                    &payload), ==, 0);

  g_autoptr(NostrEvent) stored = nostr_event_new();
  nostr_event_set_kind(stored, CONCORD_COMMUNITY_LIST);
  nostr_event_set_pubkey(stored, fixture.author_pubkey);
  nostr_event_set_created_at(stored, 1719800000);
  nostr_event_set_content(stored, payload);
  nostr_event_set_tags(stored, nostr_tags_new(0));
  free(payload);
  g_assert_cmpint(nostr_event_sign(stored, AUTHOR_SK), ==, 0);
  g_stored_list_json = nostr_event_serialize_compact(stored);

  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;
  g_autoptr(GnConcordCommunityService) service =
    gn_concord_community_service_new(context);
  pump();

  /* The tombstoned Community is not resurrected by a backfill — its removal
   * is newer than its `added_at` — while the live one is adopted. */
  g_assert_cmpuint(
    g_list_model_get_n_items(gn_concord_community_service_get_model(service)),
    ==, 1);
  g_assert_nonnull(gn_concord_community_service_lookup_community(
    service, fixture.community_id));

  /* Re-accepting republishes, and the republish preserves everything. */
  g_autoptr(GError) error = NULL;
  g_assert_true(
    gn_concord_community_service_accept_bundle(service, known, &error));
  pump();

  const char *event_json = published_community_list();
  g_assert_nonnull(event_json);
  JsonNode *root = decrypt_published_list(event_json);
  JsonObject *republished = json_node_get_object(root);
  g_assert_cmpint(json_object_get_int_member(republished, "soapbox/schema"),
                  ==, 2);
  JsonArray *stones = json_object_get_array_member(republished, "tombstones");
  g_assert_cmpuint(json_array_get_length(stones), ==, 1);
  g_assert_cmpstr(json_object_get_string_member(
                    json_array_get_object_element(stones, 0), "community_id"),
                  ==, other_id);

  JsonArray *entries = json_object_get_array_member(republished, "entries");
  g_assert_cmpuint(json_array_get_length(entries), ==, 2);
  gboolean saw_known = FALSE, saw_orphan = FALSE;
  for (guint i = 0; i < json_array_get_length(entries); i++) {
    JsonObject *entry = json_array_get_object_element(entries, i);
    const char *id = json_object_get_string_member(entry, "community_id");
    if (g_strcmp0(id, fixture.community_id) == 0) {
      saw_known = TRUE;
      /* An unknown per-entry field survives an edit by this client. */
      g_assert_true(json_object_get_boolean_member(entry, "vector/pinned"));
      /* `added_at` is when the membership began, not when it was refreshed. */
      g_assert_cmpint(json_object_get_int_member(entry, "added_at"), ==,
                      1719800000000);
    } else if (g_strcmp0(id, other_id) == 0) {
      saw_orphan = TRUE;
    }
  }
  g_assert_true(saw_known);
  g_assert_true(saw_orphan);
  json_node_free(root);

  gn_concord_community_service_shutdown(service);
  list_test_end(&fixture);
}

/* An unreachable relay and an empty one are indistinguishable from here, and
 * one of them costs the user every membership on every other device. So a
 * client that never read the document must never write one. */
static void swallow_message(const char *domain, GLogLevelFlags level,
                            const char *message, gpointer user_data) {
  (void)domain;
  (void)level;
  (void)message;
  (void)user_data;
}

static void test_community_list_never_published_before_read(void) {
  Fixture fixture = { 0 };
  list_test_begin(&fixture);
  gn_concord_test_query_fails = TRUE;
  /* An unreachable relay is *reported*, not swallowed — the service warns its
   * way through this test and that reporting is the point. Take the warnings
   * off the test logger rather than making them non-fatal, so a real one
   * elsewhere still fails the suite. */
  GLogLevelFlags fatal = g_log_set_always_fatal(G_LOG_LEVEL_ERROR);
  guint handler = g_log_set_handler(
    NULL, G_LOG_LEVEL_WARNING | G_LOG_FLAG_RECURSION, swallow_message, NULL);

  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;
  g_autoptr(GnConcordCommunityService) service =
    gn_concord_community_service_new(context);
  g_autofree gchar *bundle = build_bundle(&fixture, CHANNEL_KEY, 0, 1);
  g_autoptr(GError) error = NULL;
  g_assert_true(
    gn_concord_community_service_accept_bundle(service, bundle, &error));
  pump();

  /* The membership is live locally — the keys are in hand — but nothing was
   * written over the List this session could not read. */
  g_assert_cmpuint(
    g_list_model_get_n_items(gn_concord_community_service_get_model(service)),
    ==, 1);
  g_assert_null(published_community_list());

  gn_concord_community_service_shutdown(service);
  g_log_remove_handler(NULL, handler);
  g_log_set_always_fatal(fatal);
  list_test_end(&fixture);
}

/* ------------------------------------------------------------------ *
 * minting an invite link (CORD-05 §2, §4, §5)
 * ------------------------------------------------------------------ */

/* The nth published event of @kind, oldest first, or NULL. */
static const char *published_nth_of_kind(int kind, guint nth) {
  guint seen = 0;
  for (guint i = 0; gn_concord_test_published &&
                    i < gn_concord_test_published->len; i++) {
    const char *json = g_ptr_array_index(gn_concord_test_published, i);
    g_autoptr(NostrEvent) event = nostr_event_new();
    if (!nostr_event_deserialize_compact(event, json, NULL)) continue;
    if (nostr_event_get_kind(event) != kind) continue;
    if (seen++ == nth) return json;
  }
  return NULL;
}

/* Where that event sits in the published order. The mint's whole correctness
 * argument is an ordering one, so the test asserts on positions. */
/* The newest of a replaceable kind: the Community List is republished every
 * time the membership changes, so the last one is the one that matters. */
static const char *published_last_of_kind(int kind) {
  const char *last = NULL;
  for (guint nth = 0;; nth++) {
    const char *json = published_nth_of_kind(kind, nth);
    if (!json) return last;
    last = json;
  }
}

static gint published_index_of_kind(int kind, guint nth) {
  guint seen = 0;
  for (guint i = 0; gn_concord_test_published &&
                    i < gn_concord_test_published->len; i++) {
    const char *json = g_ptr_array_index(gn_concord_test_published, i);
    g_autoptr(NostrEvent) event = nostr_event_new();
    if (!nostr_event_deserialize_compact(event, json, NULL)) continue;
    if (nostr_event_get_kind(event) != kind) continue;
    if (seen++ == nth) return (gint)i;
  }
  return -1;
}

typedef struct {
  gchar *url;
  gboolean done;
  gboolean ok;
  GError *error;
} InviteResult;

static void on_invite_minted(GObject *source, GAsyncResult *result,
                             gpointer user_data) {
  InviteResult *out = user_data;
  out->url = gn_concord_community_service_create_invite_finish(
    GN_CONCORD_COMMUNITY_SERVICE(source), result, &out->error);
  out->ok = out->url != NULL;
  out->done = TRUE;
}

static void on_invite_revoked(GObject *source, GAsyncResult *result,
                              gpointer user_data) {
  InviteResult *out = user_data;
  out->ok = gn_concord_community_service_revoke_invite_finish(
    GN_CONCORD_COMMUNITY_SERVICE(source), result, &out->error);
  out->done = TRUE;
}

static gchar *control_signer_address(const Fixture *fixture) {
  uint8_t control_root[32], id[32];
  g_assert_true(nostr_concord_hex_decode_32(CONTROL_ROOT, control_root));
  g_assert_true(nostr_concord_hex_decode_32(fixture->community_id, id));
  nostr_concord_group_key_t signer;
  g_assert_cmpint(
    nostr_concord_control_signer_key(control_root, id, TEST_EPOCH, &signer), ==,
    NOSTR_CONCORD_OK);
  char control_pk[65];
  nostr_concord_hex_encode_32(signer.pk, control_pk);
  nostr_concord_group_key_clear(&signer);
  return g_strdup(control_pk);
}

/* Every plane is a stream of kind-1059 wraps, so a wrap is told apart by the
 * address that signed it — the Control Plane's, here, never the Guestbook's. */
static const char *published_nth_wrap_at(const char *address, guint nth,
                                         gint *out_index) {
  guint seen = 0;
  if (out_index) *out_index = -1;
  for (guint i = 0; gn_concord_test_published &&
                    i < gn_concord_test_published->len; i++) {
    const char *json = g_ptr_array_index(gn_concord_test_published, i);
    g_autoptr(NostrEvent) event = nostr_event_new();
    if (!nostr_event_deserialize_compact(event, json, NULL)) continue;
    if (nostr_event_get_kind(event) != CONCORD_STREAM_WRAP) continue;
    if (g_strcmp0(nostr_event_get_pubkey(event), address) != 0) continue;
    if (seen++ != nth) continue;
    if (out_index) *out_index = (gint)i;
    return json;
  }
  return NULL;
}

/* A staff membership: `control_pk` addresses the Control Plane and
 * `control_root` writes it, which is what a staffer's own Community List
 * entry carries and no invite bundle ever does (CORD-02 §2, §8). */
static gchar *build_staff_bundle(const Fixture *fixture) {
  g_autofree gchar *control_pk = control_signer_address(fixture);
  g_autofree gchar *bundle = build_bundle(fixture, CHANNEL_KEY, 0, 1);
  return g_strdup_printf(
    "%.*s,\"control_pk\":\"%s\",\"control_root\":\"%s\"}",
    (int)(strlen(bundle) - 1), bundle, control_pk, CONTROL_ROOT);
}

static void invite_test_begin(Fixture *fixture) {
  fixture_init(fixture);
  gn_concord_test_reset();
  /* The owner mints: their rank comes from the community_id itself, so
   * CREATE_INVITE resolves with no Roster synced at all. */
  gn_concord_test_signer_sk = OWNER_SK;
  gn_concord_test_user_pubkey = fixture->owner_pubkey;
}

/* Minting publishes three things in one order, and the order is the point: a
 * Registry naming a coordinate with no bundle behind it reads to every member
 * as a live link that cannot be followed. */
static void test_invite_mint_roundtrip(void) {
  Fixture fixture = { 0 };
  invite_test_begin(&fixture);
  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;
  g_autoptr(GnConcordCommunityService) service =
    gn_concord_community_service_new(context);

  g_autofree gchar *bundle = build_staff_bundle(&fixture);
  g_autoptr(GError) accept_error = NULL;
  g_assert_true(
    gn_concord_community_service_accept_bundle(service, bundle,
                                               &accept_error));
  g_assert_no_error(accept_error);
  pump();

  InviteResult minted = { 0 };
  gn_concord_community_service_create_invite_async(
    service, fixture.community_id, "Conf 2026", 0, NULL, on_invite_minted,
    &minted);
  pump();
  pump();
  g_assert_no_error(minted.error);
  g_assert_true(minted.done);
  g_assert_nonnull(minted.url);

  /* Bundle, then Registry, then the creator's private bookkeeping. */
  g_autofree gchar *control_pk = control_signer_address(&fixture);
  gint registry_at = -1;
  const char *registry_json =
    published_nth_wrap_at(control_pk, 0, &registry_at);
  gint bundle_at = published_index_of_kind(CONCORD_INVITE_BUNDLE, 0);
  gint list_at = published_index_of_kind(CONCORD_INVITE_LIST, 0);
  g_assert_nonnull(registry_json);
  g_assert_cmpint(bundle_at, >=, 0);
  g_assert_cmpint(registry_at, >, bundle_at);
  g_assert_cmpint(list_at, >, registry_at);

  /* The link is `$BASE/invite/<naddr>#<fragment>`, and only the naddr and the
   * fragment are protocol. */
  const char *hash = strchr(minted.url, '#');
  g_assert_nonnull(hash);
  const char *slash = strrchr(minted.url, '/');
  g_assert_nonnull(slash);
  g_autofree gchar *naddr = g_strndup(slash + 1, (gsize)(hash - slash - 1));
  NostrEntityPointer *pointer = NULL;
  g_assert_cmpint(nostr_nip19_decode_naddr(naddr, &pointer), ==, 0);
  g_assert_cmpint(pointer->kind, ==, CONCORD_INVITE_BUNDLE);
  /* The per-link pubkey alone makes the coordinate unique, so the identifier
   * carries no bytes (CORD-05 §2). */
  g_assert_cmpstr(pointer->identifier, ==, "");
  g_autofree gchar *signer = g_strdup(pointer->public_key);
  nostr_entity_pointer_free(pointer);

  const char *bundle_json = published_nth_of_kind(CONCORD_INVITE_BUNDLE, 0);
  g_assert_nonnull(bundle_json);
  g_autoptr(NostrEvent) bundle_event = nostr_event_new();
  g_assert_true(
    nostr_event_deserialize_compact(bundle_event, bundle_json, NULL));
  g_assert_cmpint(nostr_event_validate(bundle_event, NULL), ==,
                  NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpstr(nostr_event_get_pubkey(bundle_event), ==, signer);

  /* The fragment is never sent to any server, so it is the only thing that
   * opens the bundle the relay can see. */
  nostr_concord_invite_fragment_t fragment;
  g_assert_cmpint(nostr_concord_invite_fragment_parse(hash + 1, &fragment), ==,
                  NOSTR_CONCORD_OK);
  char *opened = NULL;
  g_assert_cmpint(
    nostr_concord_invite_bundle_decrypt(nostr_event_get_content(bundle_event),
                                        fragment.token, &opened),
    ==, NOSTR_CONCORD_OK);
  nostr_concord_invite_fragment_clear(&fragment);
  g_assert_nonnull(strstr(opened, COMMUNITY_ROOT));
  g_assert_nonnull(strstr(opened, "Conf 2026"));
  g_assert_nonnull(strstr(opened, fixture.owner_pubkey));
  /* A link hands out membership, never the staff write key. */
  g_assert_null(strstr(opened, CONTROL_ROOT));
  free(opened);

  /* The Registry edition folds back as a live link, which is what makes the
   * Community Public. */
  g_assert_true(gn_concord_community_service_ingest_control_wrap(
    service, fixture.community_id, registry_json));
  GPtrArray *links = gn_concord_community_service_get_invite_links(
    service, fixture.community_id);
  g_assert_cmpuint(links->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(links, 0), ==, signer);
  g_assert_true(
    gn_concord_community_service_is_public(service, fixture.community_id));

  /* And the creator's own Invite List holds what only they may hold: the
   * unlock token and the link signer's secret. */
  JsonNode *root =
    decrypt_published_document(published_nth_of_kind(CONCORD_INVITE_LIST, 0),
                               OWNER_SK);
  JsonArray *entries =
    json_object_get_array_member(json_node_get_object(root), "entries");
  g_assert_cmpuint(json_array_get_length(entries), ==, 1);
  JsonObject *entry = json_array_get_object_element(entries, 0);
  g_assert_cmpstr(json_object_get_string_member(entry, "community_id"), ==,
                  fixture.community_id);
  g_assert_cmpstr(json_object_get_string_member(entry, "url"), ==, minted.url);
  g_assert_cmpstr(json_object_get_string_member(entry, "label"), ==,
                  "Conf 2026");
  g_autofree gchar *entry_signer =
    nostr_key_get_public(json_object_get_string_member(entry, "signer_sk"));
  g_assert_cmpstr(entry_signer, ==, signer);
  g_autofree gchar *token =
    g_strdup(json_object_get_string_member(entry, "token"));
  g_assert_cmpuint(strlen(token), ==, CONCORD_INVITE_TOKEN_BYTES * 2);
  json_node_free(root);

  /* The service's own view of its links is the same one, minus the secret. */
  g_autoptr(GPtrArray) live =
    gn_concord_community_service_get_invites(service, fixture.community_id);
  g_assert_cmpuint(live->len, ==, 1);
  const GnConcordInviteLink *link = g_ptr_array_index(live, 0);
  g_assert_cmpstr(link->url, ==, minted.url);
  g_assert_cmpstr(link->label, ==, "Conf 2026");

  /* Retiring re-posts the coordinate as a tombstone, drops it from the
   * Registry, and tombstones it in the List — tombstone first, so live keys
   * never outlive the listing. */
  InviteResult revoked = { 0 };
  gn_concord_community_service_revoke_invite_async(
    service, fixture.community_id, token, NULL, on_invite_revoked, &revoked);
  pump();
  pump();
  g_assert_no_error(revoked.error);
  g_assert_true(revoked.ok);

  gint tombstone_at = published_index_of_kind(CONCORD_INVITE_BUNDLE, 1);
  gint retire_at = -1;
  const char *retire_json = published_nth_wrap_at(control_pk, 1, &retire_at);
  g_assert_nonnull(retire_json);
  g_assert_cmpint(tombstone_at, >, list_at);
  g_assert_cmpint(retire_at, >, tombstone_at);

  g_autoptr(NostrEvent) tombstone = nostr_event_new();
  g_assert_true(nostr_event_deserialize_compact(
    tombstone, published_nth_of_kind(CONCORD_INVITE_BUNDLE, 1), NULL));
  g_assert_cmpstr(nostr_event_get_pubkey(tombstone), ==, signer);
  NostrTags *tags = nostr_event_get_tags(tombstone);
  gboolean is_tombstone = FALSE;
  for (gsize i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (nostr_tag_size(tag) >= 2 &&
        g_strcmp0(nostr_tag_get(tag, 0), "vsk") == 0 &&
        g_strcmp0(nostr_tag_get(tag, 1), "9") == 0)
      is_tombstone = TRUE;
  }
  g_assert_true(is_tombstone);

  /* The retiring edition empties this creator's Registry, and with no live
   * link left the Community is Private again — CORD-06's Refounding trigger. */
  g_assert_true(gn_concord_community_service_ingest_control_wrap(
    service, fixture.community_id, retire_json));
  g_assert_false(
    gn_concord_community_service_is_public(service, fixture.community_id));

  /* A tombstone always beats an entry, so the link is gone from the creator's
   * own view even though the entry stays in the document. */
  g_autoptr(GPtrArray) after =
    gn_concord_community_service_get_invites(service, fixture.community_id);
  g_assert_cmpuint(after->len, ==, 0);

  /* Private again — but everyone who ever opened the link just retired still
   * holds the community_root its bundle handed out, so "Private" describes
   * who can join and not yet who can read. The Community owes a Refounding
   * and says so until one rolls the root (CORD-06 §3). */
  g_assert_true(gn_concord_community_service_refounding_due(
    service, fixture.community_id));

  /* The debt rides the Community List, so the staffer's other devices see it
   * too — as the epoch that would pay it, which is what lets a Refounding
   * clear it by happening rather than by remembering to. */
  pump();
  pump();
  JsonNode *listed =
    decrypt_published_document(published_last_of_kind(CONCORD_COMMUNITY_LIST),
                               OWNER_SK);
  g_assert_nonnull(listed);
  JsonArray *listed_entries =
    json_object_get_array_member(json_node_get_object(listed), "entries");
  JsonObject *membership = NULL;
  for (guint i = 0; i < json_array_get_length(listed_entries); i++) {
    JsonObject *candidate = json_array_get_object_element(listed_entries, i);
    if (g_strcmp0(json_object_get_string_member(candidate, "community_id"),
                  fixture.community_id) == 0)
      membership = candidate;
  }
  g_assert_nonnull(membership);
  g_assert_cmpint(
    json_object_get_int_member(membership, "refounding_due_epoch"), ==,
    TEST_EPOCH + 1);
  json_node_free(listed);

  g_free(minted.url);
  gn_concord_community_service_shutdown(service);
  gn_concord_test_reset();
  fixture_clear(&fixture);
}

/* The Public/Private truth is the *aggregate* active-set (CORD-05 §5), so a
 * creator emptying their own Registry while another creator still lists a
 * live link has retired a link, not the last one — the Community stays
 * Public and owes nothing. */
static void test_invite_retire_leaves_other_creators_links(void) {
  Fixture fixture = { 0 };
  invite_test_begin(&fixture);
  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;
  g_autoptr(GnConcordCommunityService) service =
    gn_concord_community_service_new(context);

  g_autofree gchar *bundle = build_staff_bundle(&fixture);
  g_autoptr(GError) accept_error = NULL;
  g_assert_true(
    gn_concord_community_service_accept_bundle(service, bundle,
                                               &accept_error));
  g_assert_no_error(accept_error);
  pump();

  /* A second creator, holding CREATE_INVITE through a Role and a Grant, with
   * one live link of their own. */
  g_autofree gchar *admin = nostr_key_get_public(ADMIN_SK);
  g_autofree gchar *role_content = g_strdup_printf(
    "{\"role_id\":\"%s\",\"name\":\"Host\",\"position\":5,"
    "\"permissions\":\"%" G_GUINT64_FORMAT "\"}",
    ROLE_ID, CONCORD_PERM_CREATE_INVITE);
  EditionOptions role =
    owner_edition(CONCORD_VSK_ROLE, ROLE_ID, 1, role_content);
  /* This membership is a staff bundle, so its Control Plane sits at the split
   * address the control_root signs — not the legacy derivation. */
  role.legacy_address = FALSE;
  g_assert_true(ingest_edition(service, &fixture, &role, NULL));
  g_autofree gchar *grant_eid = grant_coordinate(&fixture, admin);
  g_autofree gchar *grant_content = g_strdup_printf(
    "{\"member\":\"%s\",\"role_ids\":[\"%s\"]}", admin, ROLE_ID);
  EditionOptions grant =
    owner_edition(CONCORD_VSK_GRANT, grant_eid, 1, grant_content);
  grant.legacy_address = FALSE;
  g_assert_true(ingest_edition(service, &fixture, &grant, NULL));
  g_autofree gchar *admin_registry = registry_coordinate(&fixture, admin);
  g_autofree gchar *admin_content =
    g_strdup_printf("[\"%s\"]", LINK_SIGNER_B);
  EditionOptions admin_mint = owner_edition(CONCORD_VSK_INVITE_REGISTRY,
                                            admin_registry, 1, admin_content);
  admin_mint.actor_sk = ADMIN_SK;
  admin_mint.legacy_address = FALSE;
  g_assert_true(ingest_edition(service, &fixture, &admin_mint, NULL));
  g_assert_true(
    gn_concord_community_service_is_public(service, fixture.community_id));

  /* The owner mints and retires their own link. */
  InviteResult minted = { 0 };
  gn_concord_community_service_create_invite_async(
    service, fixture.community_id, NULL, 0, NULL, on_invite_minted, &minted);
  pump();
  pump();
  g_assert_no_error(minted.error);
  g_assert_nonnull(minted.url);

  g_autoptr(GPtrArray) live =
    gn_concord_community_service_get_invites(service, fixture.community_id);
  g_assert_cmpuint(live->len, ==, 1);
  g_autofree gchar *token =
    g_strdup(((const GnConcordInviteLink *)g_ptr_array_index(live, 0))->token);

  InviteResult revoked = { 0 };
  gn_concord_community_service_revoke_invite_async(
    service, fixture.community_id, token, NULL, on_invite_revoked, &revoked);
  pump();
  pump();
  g_assert_no_error(revoked.error);
  g_assert_true(revoked.ok);

  /* Somebody else's link is still live, so nothing turned Private and no
   * Refounding is owed. */
  g_assert_true(
    gn_concord_community_service_is_public(service, fixture.community_id));
  g_assert_false(gn_concord_community_service_refounding_due(
    service, fixture.community_id));

  g_free(minted.url);
  gn_concord_community_service_shutdown(service);
  gn_concord_test_reset();
  fixture_clear(&fixture);
}

/* The Invite List holds every link's signing secret, so replacing one this
 * session never read would cost the creator the ability to refresh or retire
 * the links their other devices minted. An unreachable relay and an empty one
 * are indistinguishable from here: fail closed. */
static void test_invite_never_minted_before_list_read(void) {
  Fixture fixture = { 0 };
  invite_test_begin(&fixture);
  gn_concord_test_query_fails = TRUE;
  GLogLevelFlags fatal = g_log_set_always_fatal(G_LOG_LEVEL_ERROR);
  guint handler = g_log_set_handler(
    NULL, G_LOG_LEVEL_WARNING | G_LOG_FLAG_RECURSION, swallow_message, NULL);

  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;
  g_autoptr(GnConcordCommunityService) service =
    gn_concord_community_service_new(context);
  g_autofree gchar *bundle = build_staff_bundle(&fixture);
  g_autoptr(GError) accept_error = NULL;
  g_assert_true(
    gn_concord_community_service_accept_bundle(service, bundle,
                                               &accept_error));
  pump();

  InviteResult minted = { 0 };
  gn_concord_community_service_create_invite_async(
    service, fixture.community_id, NULL, 0, NULL, on_invite_minted, &minted);
  pump();
  g_assert_true(minted.done);
  g_assert_null(minted.url);
  g_assert_error(minted.error, G_IO_ERROR, G_IO_ERROR_PENDING);
  /* And nothing was published: not the bundle, not the Registry edit. */
  g_autofree gchar *control_pk = control_signer_address(&fixture);
  g_assert_null(published_nth_of_kind(CONCORD_INVITE_BUNDLE, 0));
  g_assert_null(published_nth_wrap_at(control_pk, 0, NULL));
  g_clear_error(&minted.error);

  gn_concord_community_service_shutdown(service);
  g_log_remove_handler(NULL, handler);
  g_log_set_always_fatal(fatal);
  gn_concord_test_reset();
  fixture_clear(&fixture);
}

/* Minting is gated by CREATE_INVITE, and a member the Roster ranks nowhere
 * holds none of it (CORD-05 §5). */
static void test_invite_requires_permission(void) {
  Fixture fixture = { 0 };
  invite_test_begin(&fixture);
  /* Not the owner this time: an ordinary member with the keys to read. */
  gn_concord_test_signer_sk = AUTHOR_SK;
  gn_concord_test_user_pubkey = fixture.author_pubkey;

  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;
  g_autoptr(GnConcordCommunityService) service =
    gn_concord_community_service_new(context);
  g_autofree gchar *bundle = build_bundle(&fixture, CHANNEL_KEY, 0, 1);
  g_autoptr(GError) accept_error = NULL;
  g_assert_true(
    gn_concord_community_service_accept_bundle(service, bundle,
                                               &accept_error));
  pump();

  InviteResult minted = { 0 };
  gn_concord_community_service_create_invite_async(
    service, fixture.community_id, NULL, 0, NULL, on_invite_minted, &minted);
  pump();
  g_assert_true(minted.done);
  g_assert_null(minted.url);
  g_assert_error(minted.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_null(published_nth_of_kind(CONCORD_INVITE_BUNDLE, 0));
  g_clear_error(&minted.error);

  gn_concord_community_service_shutdown(service);
  gn_concord_test_reset();
  fixture_clear(&fixture);
}

/* ------------------------------------------------------------------ *
 * the binary pairwise lane a rekey blob rides (nostrc-3m86)
 * ------------------------------------------------------------------ */

typedef struct {
  gboolean done;
  GBytes *bytes;
  char *text;
  GError *error;
} CryptoResult;

static void on_bytes_encrypted(GObject *source, GAsyncResult *result,
                               gpointer user_data) {
  (void)source;
  CryptoResult *out = user_data;
  out->text = gnostr_plugin_context_nip44_encrypt_bytes_finish(NULL, result,
                                                               &out->error);
  out->done = TRUE;
}

static void on_bytes_decrypted(GObject *source, GAsyncResult *result,
                               gpointer user_data) {
  (void)source;
  CryptoResult *out = user_data;
  out->bytes = gnostr_plugin_context_nip44_decrypt_bytes_finish(NULL, result,
                                                                &out->error);
  out->done = TRUE;
}

static void on_text_decrypted(GObject *source, GAsyncResult *result,
                              gpointer user_data) {
  (void)source;
  CryptoResult *out = user_data;
  out->text =
    gnostr_plugin_context_nip44_decrypt_finish(NULL, result, &out->error);
  out->done = TRUE;
}

/* A blob is key material: high bytes, embedded NULs, sequences that are not
 * valid UTF-8 anywhere. */
static GBytes *blob_of_width(gsize width) {
  guint8 *bytes = g_malloc(width);
  for (gsize i = 0; i < width; i++) bytes[i] = (guint8)((i * 7u + 0x80u) & 0xff);
  bytes[0] = 0x00;
  bytes[1] = 0xff;
  bytes[width / 2] = 0x00;
  bytes[width - 1] = 0xc0;
  return g_bytes_new_take(bytes, width);
}

/* Every CORD-06 rekey blob width survives the pairwise lane byte for byte.
 * The width is the format signal — a 104-byte blob is a member's base
 * rotation and a 136-byte one is a staff recipient's — so a substituted byte
 * is not a recoverable error but a rotation that silently never lands. */
static void test_nip44_bytes_roundtrip(void) {
  gn_concord_test_reset();
  gn_concord_test_signer_sk = OWNER_SK;
  g_autofree char *peer = nostr_key_get_public(AUTHOR_SK);
  g_assert_nonnull(peer);

  const gsize widths[] = {72, 104, 136};
  for (guint i = 0; i < G_N_ELEMENTS(widths); i++) {
    g_autoptr(GBytes) blob = blob_of_width(widths[i]);

    CryptoResult wrapped = {0};
    gnostr_plugin_context_nip44_encrypt_bytes_async(
      (GnostrPluginContext *)1, peer, blob, NULL, on_bytes_encrypted, &wrapped);
    pump();
    g_assert_true(wrapped.done);
    g_assert_no_error(wrapped.error);
    g_assert_nonnull(wrapped.text);

    CryptoResult opened = {0};
    gnostr_plugin_context_nip44_decrypt_bytes_async(
      (GnostrPluginContext *)1, peer, wrapped.text, NULL, on_bytes_decrypted,
      &opened);
    pump();
    g_assert_true(opened.done);
    g_assert_no_error(opened.error);
    g_assert_nonnull(opened.bytes);
    g_assert_cmpuint(g_bytes_get_size(opened.bytes), ==, widths[i]);
    g_assert_true(g_bytes_equal(opened.bytes, blob));
    g_bytes_unref(opened.bytes);

    /* The same payload through the string lane comes back truncated at the
     * first NUL: the exact loss the bytes lane exists to prevent, and the
     * reason this is a separate method pair rather than a flag. */
    CryptoResult as_text = {0};
    gnostr_plugin_context_nip44_decrypt_async((GnostrPluginContext *)1, peer,
                                              wrapped.text, NULL,
                                              on_text_decrypted, &as_text);
    pump();
    g_assert_true(as_text.done);
    g_assert_no_error(as_text.error);
    g_assert_nonnull(as_text.text);
    g_assert_cmpuint(strlen(as_text.text), <, widths[i]);
    g_free(as_text.text);

    g_free(wrapped.text);
  }

  /* A payload that was never a valid wrap fails; it never yields empty
   * bytes that a caller could mistake for a zero-width blob. */
  CryptoResult junk = {0};
  gnostr_plugin_context_nip44_decrypt_bytes_async((GnostrPluginContext *)1, peer,
                                                  "not-a-payload", NULL,
                                                  on_bytes_decrypted, &junk);
  pump();
  g_assert_true(junk.done);
  g_assert_null(junk.bytes);
  g_assert_nonnull(junk.error);
  g_clear_error(&junk.error);

  gn_concord_test_reset();
}

/* ------------------------------------------------------------------ *
 * the Refounding (CORD-06)
 * ------------------------------------------------------------------ */

typedef struct {
  gboolean done;
  gboolean ok;
  GError *error;
} RefoundResult;

static void on_channel_rekeyed(GObject *source, GAsyncResult *result,
                               gpointer user_data) {
  RefoundResult *out = user_data;
  out->ok = gn_concord_community_service_rekey_channel_finish(
    GN_CONCORD_COMMUNITY_SERVICE(source), result, &out->error);
  out->done = TRUE;
}

static void on_refounded(GObject *source, GAsyncResult *result,
                         gpointer user_data) {
  RefoundResult *out = user_data;
  out->ok = gn_concord_community_service_refound_finish(
    GN_CONCORD_COMMUNITY_SERVICE(source), result, &out->error);
  out->done = TRUE;
}

/* The address the *prior* root derives for the next epoch — where a rotation
 * has to land for a current keyholder to find it, and nowhere a lapsed one
 * can look. */
static void base_rekey_key(const Fixture *fixture,
                           nostr_concord_group_key_t *out) {
  uint8_t root[32], id[32];
  g_assert_true(nostr_concord_hex_decode_32(COMMUNITY_ROOT, root));
  g_assert_true(nostr_concord_hex_decode_32(fixture->community_id, id));
  g_assert_cmpint(nostr_concord_base_rekey_key(root, id, TEST_EPOCH + 1, out),
                  ==, NOSTR_CONCORD_OK);
}

/* Carries an already-signed seal in a wrap at @key's address, the way a
 * Rotator's own client would. */
static gchar *wrap_at_key(const nostr_concord_group_key_t *key,
                          const char *seal_json, gint64 created_at) {
  char *wrap_content = NULL;
  g_assert_cmpint(
    nostr_concord_stream_seal(key->conv_key, seal_json, &wrap_content), ==,
    NOSTR_CONCORD_OK);
  char stream_sk[65], stream_pk[65];
  nostr_concord_hex_encode_32(key->sk, stream_sk);
  nostr_concord_hex_encode_32(key->pk, stream_pk);

  g_autoptr(NostrEvent) wrap = nostr_event_new();
  nostr_event_set_kind(wrap, CONCORD_STREAM_WRAP);
  nostr_event_set_pubkey(wrap, stream_pk);
  nostr_event_set_created_at(wrap, created_at);
  nostr_event_set_content(wrap, wrap_content);
  g_autofree gchar *ephemeral_sk = nostr_key_generate_private();
  g_autofree gchar *ephemeral_pk = nostr_key_get_public(ephemeral_sk);
  nostr_event_set_tags(wrap,
                       nostr_tags_new(1, nostr_tag_new("p", ephemeral_pk,
                                                       NULL)));
  free(wrap_content);
  g_assert_cmpint(nostr_event_sign(wrap, stream_sk), ==, 0);
  return nostr_event_serialize_compact(wrap);
}

static GPtrArray *published_rekey_wraps(const Fixture *fixture) {
  nostr_concord_group_key_t key;
  base_rekey_key(fixture, &key);
  char address[65];
  nostr_concord_hex_encode_32(key.pk, address);
  nostr_concord_group_key_clear(&key);

  GPtrArray *wraps = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; gn_concord_test_published &&
                    i < gn_concord_test_published->len; i++) {
    const char *json = g_ptr_array_index(gn_concord_test_published, i);
    g_autoptr(NostrEvent) event = nostr_event_new();
    if (!nostr_event_deserialize_compact(event, json, NULL)) continue;
    if (nostr_event_get_kind(event) != CONCORD_STREAM_WRAP) continue;
    if (g_strcmp0(nostr_event_get_pubkey(event), address) != 0) continue;
    g_ptr_array_add(wraps, g_strdup(json));
  }
  return wraps;
}

static GnConcordCommunityService *joined_service(const Fixture *fixture,
                                                 GnostrPluginContext *context) {
  GnConcordCommunityService *service =
    gn_concord_community_service_new(context);
  g_autofree gchar *bundle = build_bundle(fixture, CHANNEL_KEY, 0, 1);
  g_autoptr(GError) error = NULL;
  g_assert_true(
    gn_concord_community_service_accept_bundle(service, bundle, &error));
  g_assert_no_error(error);
  /* A public Channel on every side: its key comes from the community_root, so
   * it is the plane that proves who landed on which root (CORD-03 §1). */
  EditionOptions define =
    owner_edition(CONCORD_VSK_CHANNEL, SECOND_CHANNEL, 1,
                  "{\"name\":\"lounge\",\"private\":false}");
  g_assert_true(ingest_edition(service, fixture, &define, NULL));
  return service;
}

/* The whole rotation, both halves, against the only question that matters:
 * afterwards, do the Rotator and the member hold the *same* new root, and
 * does a device that never received the rotation hold nothing?
 *
 * The public Channel answers all three at once. Its key derives from the
 * community_root, so an event published there after the roll opens for
 * exactly those who converged on the new one. */
static void test_refounding_roundtrip(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  gn_concord_test_reset();
  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;

  gn_concord_test_signer_sk = OWNER_SK;
  gn_concord_test_user_pubkey = fixture.owner_pubkey;
  g_autoptr(GnConcordCommunityService) rotator =
    joined_service(&fixture, context);
  g_autoptr(GnConcordCommunityService) member =
    joined_service(&fixture, context);
  /* A third device that never sees the rotation: the removed member's view,
   * and the control for every claim below. */
  g_autoptr(GnConcordCommunityService) stale =
    joined_service(&fixture, context);

  /* The Complete Memberlist decides who receives a blob, so it is acquired
   * before the first publish (CORD-06 "Failure and races"). */
  g_autofree gchar *join = mint_guestbook_wrap(
    &fixture, AUTHOR_SK, CONCORD_KIND_JOIN_LEAVE, "join", 1686840217, "100",
    NULL);
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    rotator, fixture.community_id, join));
  g_assert_true(is_member(rotator, &fixture, fixture.author_pubkey));

  RefoundResult refounded = { 0 };
  gn_concord_community_service_refound_async(rotator, fixture.community_id,
                                             NULL, on_refounded, &refounded);
  pump();
  g_assert_true(refounded.done);
  g_assert_no_error(refounded.error);
  g_assert_true(refounded.ok);

  /* Two recipients — the member and the Rotator itself, which would otherwise
   * lock itself out of the epoch it minted — fit one chunk of 120. */
  g_autoptr(GPtrArray) wraps = published_rekey_wraps(&fixture);
  g_assert_cmpuint(wraps->len, ==, 1);

  /* The member opens its own blob by locator and adopts. */
  gn_concord_test_signer_sk = AUTHOR_SK;
  gn_concord_test_user_pubkey = fixture.author_pubkey;
  g_assert_true(gn_concord_community_service_ingest_rekey_wrap(
    member, fixture.community_id, g_ptr_array_index(wraps, 0)));
  pump();

  /* A message into the public Channel now rides a key derived from the new
   * root, which only a device that adopted the rotation can compute. */
  gn_concord_test_signer_sk = OWNER_SK;
  gn_concord_test_user_pubkey = fixture.owner_pubkey;
  guint published_before =
    gn_concord_test_published ? gn_concord_test_published->len : 0;
  PublishResult outcome = { .loop = g_main_loop_new(NULL, FALSE) };
  gn_concord_community_service_publish_message_async(
    rotator, fixture.community_id, SECOND_CHANNEL, "after the refounding",
    NULL, on_publish_finished, &outcome);
  g_main_loop_run(outcome.loop);
  g_assert_true(outcome.ok);
  g_free(outcome.message);
  g_main_loop_unref(outcome.loop);
  g_assert_cmpuint(gn_concord_test_published->len, >, published_before);
  const char *chat = g_ptr_array_index(gn_concord_test_published,
                                       gn_concord_test_published->len - 1);

  g_assert_true(gn_concord_community_service_ingest_wrap(
    member, fixture.community_id, SECOND_CHANNEL, chat));
  GListModel *folded = gn_concord_community_service_get_messages(
    member, fixture.community_id, SECOND_CHANNEL);
  g_assert_cmpuint(g_list_model_get_n_items(folded), ==, 1);

  /* And the device that never received the rotation cannot even find the
   * address: retiring a link stops joins, but this is what actually severs a
   * reader (CORD-06 §3). */
  g_assert_false(gn_concord_community_service_ingest_wrap(
    stale, fixture.community_id, SECOND_CHANNEL, chat));

  /* Replaying the rotation is harmless: it re-delivers the same keys rather
   * than rolling again, which is what makes a crashed Rotator resumable
   * instead of forking the Community on every retry. */
  g_autoptr(GnConcordCommunityItem) settled =
    gn_concord_community_service_lookup_community(member,
                                                  fixture.community_id);
  g_autoptr(GnConcordChannelItem) before =
    gn_concord_community_item_find_channel(settled, SECOND_CHANNEL);
  g_autofree gchar *address_before =
    g_strdup(gn_concord_channel_item_get_stream_pubkey(before));
  gn_concord_test_signer_sk = AUTHOR_SK;
  gn_concord_test_user_pubkey = fixture.author_pubkey;
  gn_concord_community_service_ingest_rekey_wrap(
    member, fixture.community_id, g_ptr_array_index(wraps, 0));
  pump();
  g_autoptr(GnConcordChannelItem) after =
    gn_concord_community_item_find_channel(settled, SECOND_CHANNEL);
  g_assert_cmpstr(gn_concord_channel_item_get_stream_pubkey(after), ==,
                  address_before);

  gn_concord_community_service_shutdown(rotator);
  gn_concord_community_service_shutdown(member);
  gn_concord_community_service_shutdown(stale);
  gn_concord_test_reset();
  fixture_clear(&fixture);
}

/* Holding a key is never authority: a member who kept the prior root can
 * construct a perfectly shaped rotation, and every honest client drops it
 * against the folded Roster (CORD-06 "Authority"). */
static void test_refounding_requires_ban(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  gn_concord_test_reset();
  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;

  gn_concord_test_signer_sk = AUTHOR_SK;
  gn_concord_test_user_pubkey = fixture.author_pubkey;
  g_autoptr(GnConcordCommunityService) roleless =
    joined_service(&fixture, context);

  RefoundResult refounded = { 0 };
  gn_concord_community_service_refound_async(roleless, fixture.community_id,
                                             NULL, on_refounded, &refounded);
  pump();
  g_assert_true(refounded.done);
  g_assert_false(refounded.ok);
  g_assert_error(refounded.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error(&refounded.error);
  g_assert_cmpuint(published_rekey_wraps(&fixture)->len, ==, 0);

  /* The same rotation, minted by that same roleless npub with the root it
   * legitimately holds, is refused on the way in too. */
  gn_concord_test_signer_sk = OWNER_SK;
  gn_concord_test_user_pubkey = fixture.owner_pubkey;
  g_autoptr(GnConcordCommunityService) owner_side =
    joined_service(&fixture, context);
  g_autofree gchar *join = mint_guestbook_wrap(
    &fixture, AUTHOR_SK, CONCORD_KIND_JOIN_LEAVE, "join", 1686840217, "100",
    NULL);
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    owner_side, fixture.community_id, join));
  RefoundResult by_owner = { 0 };
  gn_concord_community_service_refound_async(owner_side, fixture.community_id,
                                             NULL, on_refounded, &by_owner);
  pump();
  g_assert_true(by_owner.ok);

  g_autoptr(GPtrArray) wraps = published_rekey_wraps(&fixture);
  g_assert_cmpuint(wraps->len, ==, 1);

  gn_concord_test_signer_sk = AUTHOR_SK;
  gn_concord_test_user_pubkey = fixture.author_pubkey;
  g_autoptr(GnConcordCommunityService) receiver =
    joined_service(&fixture, context);
  g_assert_true(gn_concord_community_service_ingest_rekey_wrap(
    receiver, fixture.community_id, g_ptr_array_index(wraps, 0)));
  pump();

  /* The same rotation, byte for byte, resealed by the roleless npub that
   * legitimately holds the prior root: it lands at the right address, the
   * wrap opens, and it is dropped on rank alone. Holding a key is never
   * authority — which is the only reason a Refounding severs anyone. */
  g_autoptr(GnConcordCommunityService) judge =
    joined_service(&fixture, context);
  g_autoptr(NostrEvent) honest = nostr_event_new();
  g_assert_true(nostr_event_deserialize_compact(
    honest, g_ptr_array_index(wraps, 0), NULL));
  nostr_concord_group_key_t rekey;
  base_rekey_key(&fixture, &rekey);
  char *honest_seal = NULL;
  g_assert_cmpint(nostr_concord_stream_open(rekey.conv_key,
                                            nostr_event_get_content(honest),
                                            &honest_seal),
                  ==, NOSTR_CONCORD_OK);
  g_autoptr(NostrEvent) seal = nostr_event_new();
  g_assert_true(nostr_event_deserialize_compact(seal, honest_seal, NULL));
  free(honest_seal);

  g_autoptr(NostrEvent) forged = nostr_event_new();
  nostr_event_set_kind(forged, CONCORD_SEAL_ENCRYPTED);
  nostr_event_set_pubkey(forged, fixture.author_pubkey);
  nostr_event_set_created_at(forged, nostr_event_get_created_at(seal));
  nostr_event_set_content(forged, nostr_event_get_content(seal));
  nostr_event_set_tags(forged, nostr_tags_new(0));
  g_assert_cmpint(nostr_event_sign(forged, AUTHOR_SK), ==, 0);
  g_autofree gchar *forged_json = nostr_event_serialize_compact(forged);
  g_autofree gchar *forged_wrap =
    wrap_at_key(&rekey, forged_json, nostr_event_get_created_at(honest));
  nostr_concord_group_key_clear(&rekey);

  g_assert_false(gn_concord_community_service_ingest_rekey_wrap(
    judge, fixture.community_id, forged_wrap));

  pump();
  gn_concord_community_service_shutdown(roleless);
  gn_concord_community_service_shutdown(owner_side);
  gn_concord_community_service_shutdown(receiver);
  gn_concord_community_service_shutdown(judge);
  gn_concord_test_reset();
  fixture_clear(&fixture);
}

/* The compaction is what a fresh joiner anchors on: the Control Plane
 * republished at the new epoch, trimmed to one edition per entity, its
 * original signatures intact because a Control seal is plaintext and a
 * re-wrap never touches it (CORD-02 §5, CORD-06 §3).
 *
 * The test's joiner has folded nothing at all — it holds the new root and the
 * new control_pk from its own rekey blob and not one prior edition — so the
 * authority it ends up with can only have come from the compaction. */
static void test_refounding_re_anchors_control(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  gn_concord_test_reset();
  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;
  g_autofree gchar *admin = nostr_key_get_public(ADMIN_SK);

  gn_concord_test_signer_sk = OWNER_SK;
  gn_concord_test_user_pubkey = fixture.owner_pubkey;
  g_autoptr(GnConcordCommunityService) rotator =
    joined_service(&fixture, context);

  /* Authority worth carrying across the boundary: a rename, and a Role with
   * the Grant that hands it out. */
  g_autofree gchar *first_hash = NULL;
  EditionOptions renamed = owner_edition(CONCORD_VSK_METADATA,
                                         fixture.community_id, 1,
                                         "{\"name\":\"Refounded\"}");
  g_assert_true(ingest_edition(rotator, &fixture, &renamed, &first_hash));
  g_autofree gchar *role_content = g_strdup_printf(
    "{\"role_id\":\"%s\",\"name\":\"Editor\",\"position\":5,"
    "\"permissions\":\"%" G_GUINT64_FORMAT "\"}",
    ROLE_ID, CONCORD_PERM_MANAGE_METADATA);
  EditionOptions role =
    owner_edition(CONCORD_VSK_ROLE, ROLE_ID, 1, role_content);
  g_assert_true(ingest_edition(rotator, &fixture, &role, NULL));
  g_autofree gchar *grant_eid = grant_coordinate(&fixture, admin);
  g_autofree gchar *grant_content = g_strdup_printf(
    "{\"member\":\"%s\",\"role_ids\":[\"%s\"]}", admin, ROLE_ID);
  EditionOptions grant =
    owner_edition(CONCORD_VSK_GRANT, grant_eid, 1, grant_content);
  g_assert_true(ingest_edition(rotator, &fixture, &grant, NULL));

  /* A second version of the metadata entity, so the compaction has something
   * to trim: a joiner must land on the head, not on the history. */
  EditionOptions final = owner_edition(CONCORD_VSK_METADATA,
                                       fixture.community_id, 2,
                                       "{\"name\":\"Refounded Twice\"}");
  final.prev = first_hash;
  g_assert_true(ingest_edition(rotator, &fixture, &final, NULL));

  g_autofree gchar *join = mint_guestbook_wrap(
    &fixture, ADMIN_SK, CONCORD_KIND_JOIN_LEAVE, "join", 1686840217, "100",
    NULL);
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    rotator, fixture.community_id, join));
  g_assert_true(is_member(rotator, &fixture, admin));

  RefoundResult refounded = { 0 };
  gn_concord_community_service_refound_async(rotator, fixture.community_id,
                                             NULL, on_refounded, &refounded);
  pump();
  g_assert_true(refounded.ok);

  g_autoptr(GPtrArray) wraps = published_rekey_wraps(&fixture);
  g_assert_cmpuint(wraps->len, ==, 1);

  /* The joiner: the membership bundle and nothing else. Its Control Plane has
   * never seen an edition. */
  gn_concord_test_signer_sk = ADMIN_SK;
  gn_concord_test_user_pubkey = admin;
  g_autoptr(GnConcordCommunityService) joiner =
    gn_concord_community_service_new(context);
  g_autofree gchar *bundle = build_bundle(&fixture, CHANNEL_KEY, 0, 1);
  g_assert_true(
    gn_concord_community_service_accept_bundle(joiner, bundle, NULL));
  g_autoptr(GnConcordCommunityItem) item =
    gn_concord_community_service_lookup_community(joiner,
                                                  fixture.community_id);
  g_assert_cmpstr(gn_concord_community_item_get_name(item), ==,
                  "Incident Response");

  g_assert_true(gn_concord_community_service_ingest_rekey_wrap(
    joiner, fixture.community_id, g_ptr_array_index(wraps, 0)));
  pump();

  /* Everything the Rotator published, offered as a relay would: only the
   * wraps at this joiner's Control address open, and they are the compaction.
   * The fold that comes out of them is the Community's authority. */
  guint anchored = 0;
  for (guint i = 0; i < gn_concord_test_published->len; i++)
    if (gn_concord_community_service_ingest_control_wrap(
          joiner, fixture.community_id,
          g_ptr_array_index(gn_concord_test_published, i)))
      anchored++;
  g_assert_cmpuint(anchored, >, 0);

  /* The head, not the history: one metadata edition, at version 2. */
  g_assert_cmpstr(gn_concord_community_item_get_name(item), ==,
                  "Refounded Twice");
  /* And the Roster, folded from a Grant this device never saw published — the
   * original owner's signature still proves who wrote it. */
  g_assert_cmpuint(
    gn_concord_community_service_get_permissions(joiner, fixture.community_id,
                                                 admin) &
      CONCORD_PERM_MANAGE_METADATA,
    ==, CONCORD_PERM_MANAGE_METADATA);
  /* Compacted: five editions went in — a Channel, the two metadata versions,
   * the Role and the Grant — and four heads came out. The superseded
   * metadata version is exactly what a joiner never has to replay. */
  g_assert_cmpuint(anchored, ==, 4);

  gn_concord_community_service_shutdown(rotator);
  gn_concord_community_service_shutdown(joiner);
  gn_concord_test_reset();
  fixture_clear(&fixture);
}

/* CORD-06 §3: a Refounding whose Control events cannot all be folded must be
 * aborted. A parked edition is exactly that — one whose Grant this device has
 * not synced — and compacting past it would publish a plane that silently
 * dropped an action nobody could yet judge. */
static void test_refounding_aborts_on_unsettled_control(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  gn_concord_test_reset();
  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;
  g_autofree gchar *admin = nostr_key_get_public(ADMIN_SK);

  gn_concord_test_signer_sk = OWNER_SK;
  gn_concord_test_user_pubkey = fixture.owner_pubkey;
  g_autoptr(GnConcordCommunityService) rotator =
    joined_service(&fixture, context);
  g_autofree gchar *join = mint_guestbook_wrap(
    &fixture, AUTHOR_SK, CONCORD_KIND_JOIN_LEAVE, "join", 1686840217, "100",
    NULL);
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    rotator, fixture.community_id, join));

  /* An action citing a Grant that never arrived: held, unjudged, parked. */
  g_autofree gchar *grant_eid = grant_coordinate(&fixture, admin);
  g_autofree gchar *grant_content = g_strdup_printf(
    "{\"member\":\"%s\",\"role_ids\":[\"%s\"]}", admin, ROLE_ID);
  EditionOptions grant =
    owner_edition(CONCORD_VSK_GRANT, grant_eid, 1, grant_content);
  g_autofree gchar *grant_hash = NULL;
  g_autofree gchar *grant_wrap = mint_edition(&fixture, &grant, &grant_hash);
  EditionOptions cited = owner_edition(CONCORD_VSK_METADATA,
                                       fixture.community_id, 1,
                                       "{\"name\":\"Cited\"}");
  cited.actor_sk = ADMIN_SK;
  cited.vac_eid = grant_eid;
  cited.vac_version = 1;
  cited.vac_hash = grant_hash;
  g_assert_true(ingest_edition(rotator, &fixture, &cited, NULL));

  RefoundResult blocked = { 0 };
  gn_concord_community_service_refound_async(rotator, fixture.community_id,
                                             NULL, on_refounded, &blocked);
  pump();
  g_assert_true(blocked.done);
  g_assert_false(blocked.ok);
  g_assert_nonnull(blocked.error);
  g_clear_error(&blocked.error);
  /* Aborted means nothing was published: not one blob, not a partial roll. */
  g_assert_cmpuint(published_rekey_wraps(&fixture)->len, ==, 0);

  /* Once the cited Grant lands the fold settles, and the rotation proceeds. */
  g_assert_true(gn_concord_community_service_ingest_control_wrap(
    rotator, fixture.community_id, grant_wrap));
  RefoundResult settled = { 0 };
  gn_concord_community_service_refound_async(rotator, fixture.community_id,
                                             NULL, on_refounded, &settled);
  pump();
  g_assert_true(settled.ok);
  g_assert_cmpuint(published_rekey_wraps(&fixture)->len, ==, 1);

  gn_concord_community_service_shutdown(rotator);
  gn_concord_test_reset();
  fixture_clear(&fixture);
}

/* The last step of a Refounding, and the one that decides whether the new
 * epoch looks like a Community or an empty room. The Guestbook's address moved
 * with the root, so nobody has said "I am here" at the new one yet; the
 * Refounder attests to who was present, and a device that folds the
 * attestation sees the same Complete Memberlist.
 *
 * It is best-effort and an attestation only — honored from the Refounder's
 * npub alone, superseded by any member's own later word — so this asserts what
 * a fresh device ends up believing, not that the snapshot is authority. */
static void test_refounding_seeds_guestbook(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  gn_concord_test_reset();
  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;
  g_autofree gchar *admin = nostr_key_get_public(ADMIN_SK);

  gn_concord_test_signer_sk = OWNER_SK;
  gn_concord_test_user_pubkey = fixture.owner_pubkey;
  g_autoptr(GnConcordCommunityService) rotator =
    joined_service(&fixture, context);

  /* Two members besides the Rotator, both present in the prior epoch. */
  g_autofree gchar *author_join = mint_guestbook_wrap(
    &fixture, AUTHOR_SK, CONCORD_KIND_JOIN_LEAVE, "join", 1686840217, "100",
    NULL);
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    rotator, fixture.community_id, author_join));
  g_autofree gchar *admin_join = mint_guestbook_wrap(
    &fixture, ADMIN_SK, CONCORD_KIND_JOIN_LEAVE, "join", 1686840218, "100",
    NULL);
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    rotator, fixture.community_id, admin_join));

  RefoundResult refounded = { 0 };
  gn_concord_community_service_refound_async(rotator, fixture.community_id,
                                             NULL, on_refounded, &refounded);
  pump();
  g_assert_true(refounded.ok);
  g_autoptr(GPtrArray) wraps = published_rekey_wraps(&fixture);
  g_assert_cmpuint(wraps->len, ==, 1);

  /* A device that adopts the rotation and has folded no Guestbook of its own:
   * at the new epoch it has heard from nobody. */
  gn_concord_test_signer_sk = ADMIN_SK;
  gn_concord_test_user_pubkey = admin;
  g_autoptr(GnConcordCommunityService) arriving =
    gn_concord_community_service_new(context);
  g_autofree gchar *bundle = build_bundle(&fixture, CHANNEL_KEY, 0, 1);
  g_assert_true(
    gn_concord_community_service_accept_bundle(arriving, bundle, NULL));
  g_assert_false(is_member(arriving, &fixture, fixture.author_pubkey));

  g_assert_true(gn_concord_community_service_ingest_rekey_wrap(
    arriving, fixture.community_id, g_ptr_array_index(wraps, 0)));
  pump();

  /* Everything the Rotator published, offered as a relay would: only the
   * wraps at this device's new Guestbook address open. */
  guint seeded = 0;
  for (guint i = 0; i < gn_concord_test_published->len; i++)
    if (gn_concord_community_service_ingest_guestbook_wrap(
          arriving, fixture.community_id,
          g_ptr_array_index(gn_concord_test_published, i)))
      seeded++;
  g_assert_cmpuint(seeded, ==, 1);

  /* The prior epoch's memberlist, folded from the attestation alone. */
  g_assert_true(is_member(arriving, &fixture, fixture.author_pubkey));
  g_assert_true(is_member(arriving, &fixture, admin));
  g_assert_true(is_member(arriving, &fixture, fixture.owner_pubkey));

  /* That it folded at all is the other half of the claim: a snapshot is
   * honored only from the npub whose Refounding minted the epoch, and this
   * device learned that npub from the rotation it just adopted — nothing
   * told it who to trust. */

  gn_concord_community_service_shutdown(rotator);
  gn_concord_community_service_shutdown(arriving);
  gn_concord_test_reset();
  fixture_clear(&fixture);
}

/* The debt a Refounding pays is incurred by the retire that empties the
 * aggregate active-set — whoever published it. A staffer retiring the last
 * link on their laptop must not leave every other staffer's device believing
 * the Community is settled: everyone who ever opened that link still holds the
 * community_root, and only rolling it severs them (CORD-06 §3). */
static void test_refounding_debt_from_another_staffer(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  g_autoptr(GnConcordCommunityService) service =
    service_with_membership(&fixture);

  /* Arriving to a Private Community owes nothing: an empty set that was never
   * seen becoming empty is not a debt, which is what keeps a fresh device
   * from re-raising one an earlier Refounding already paid. */
  g_assert_false(gn_concord_community_service_refounding_due(
    service, fixture.community_id));

  g_autofree gchar *registry =
    registry_coordinate(&fixture, fixture.owner_pubkey);
  g_autofree gchar *listed = g_strdup_printf("[\"%s\"]", LINK_SIGNER_A);
  EditionOptions mint =
    owner_edition(CONCORD_VSK_INVITE_REGISTRY, registry, 1, listed);
  g_autofree gchar *mint_hash = NULL;
  g_assert_true(ingest_edition(service, &fixture, &mint, &mint_hash));
  g_assert_true(
    gn_concord_community_service_is_public(service, fixture.community_id));
  g_assert_false(gn_concord_community_service_refounding_due(
    service, fixture.community_id));

  /* The owner retires it elsewhere; this device only ever sees the edition. */
  EditionOptions retire =
    owner_edition(CONCORD_VSK_INVITE_REGISTRY, registry, 2, "[]");
  retire.prev = mint_hash;
  g_assert_true(ingest_edition(service, &fixture, &retire, NULL));
  g_assert_false(
    gn_concord_community_service_is_public(service, fixture.community_id));
  g_assert_true(gn_concord_community_service_refounding_due(
    service, fixture.community_id));

  fixture_clear(&fixture);
}

/* And the debt is paid by the rotation, not by anything remembering to clear
 * a flag: it is recorded as the epoch that would pay it, so rolling the root
 * that far *is* the payment. */
static void test_refounding_pays_the_debt(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  gn_concord_test_reset();
  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;

  gn_concord_test_signer_sk = OWNER_SK;
  gn_concord_test_user_pubkey = fixture.owner_pubkey;
  g_autoptr(GnConcordCommunityService) rotator =
    joined_service(&fixture, context);

  g_autofree gchar *registry =
    registry_coordinate(&fixture, fixture.owner_pubkey);
  g_autofree gchar *listed = g_strdup_printf("[\"%s\"]", LINK_SIGNER_A);
  EditionOptions mint =
    owner_edition(CONCORD_VSK_INVITE_REGISTRY, registry, 1, listed);
  g_autofree gchar *mint_hash = NULL;
  g_assert_true(ingest_edition(rotator, &fixture, &mint, &mint_hash));
  EditionOptions retire =
    owner_edition(CONCORD_VSK_INVITE_REGISTRY, registry, 2, "[]");
  retire.prev = mint_hash;
  g_assert_true(ingest_edition(rotator, &fixture, &retire, NULL));
  g_assert_true(gn_concord_community_service_refounding_due(
    rotator, fixture.community_id));

  g_autofree gchar *join = mint_guestbook_wrap(
    &fixture, AUTHOR_SK, CONCORD_KIND_JOIN_LEAVE, "join", 1686840217, "100",
    NULL);
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    rotator, fixture.community_id, join));

  RefoundResult refounded = { 0 };
  gn_concord_community_service_refound_async(rotator, fixture.community_id,
                                             NULL, on_refounded, &refounded);
  pump();
  g_assert_true(refounded.ok);
  g_assert_false(gn_concord_community_service_refounding_due(
    rotator, fixture.community_id));

  gn_concord_community_service_shutdown(rotator);
  gn_concord_test_reset();
  fixture_clear(&fixture);
}

/* A Refounding rotates the Private Channels too, and addresses them under the
 * root it is about to retire.
 *
 * Rolling the base severs a banned member from every plane keyed by it, but a
 * Private Channel is independently keyed (CORD-03): its key survives the roll,
 * so without this the removed member reads that Channel forever. The address
 * is the load-bearing part. Two Refoundings racing the same epoch mint two
 * different roots, the base converges on one and the loser drops its own, so a
 * Channel rekey sealed under a *new* root would be unreadable to everyone on
 * the other branch — while one sealed under the prior root, which both
 * branches held, opens on either (CORD-06 §3).
 *
 * So: two Refoundings, two branches, and one Channel rotation that both sides
 * open — proven where it counts, in a message neither could read without it. */
static void test_refounding_rotates_private_channels(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  gn_concord_test_reset();
  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;

  gn_concord_test_signer_sk = OWNER_SK;
  gn_concord_test_user_pubkey = fixture.owner_pubkey;
  g_autoptr(GnConcordCommunityService) rotator =
    joined_service(&fixture, context);
  /* The Refounding that loses the base race: same epoch, same prior root, a
   * different new one. */
  g_autoptr(GnConcordCommunityService) rival =
    joined_service(&fixture, context);
  g_autoptr(GnConcordCommunityService) winners_branch =
    joined_service(&fixture, context);
  g_autoptr(GnConcordCommunityService) losers_branch =
    joined_service(&fixture, context);
  /* A device that receives neither rotation: the removed member's view. */
  g_autoptr(GnConcordCommunityService) stale =
    joined_service(&fixture, context);

  g_autofree gchar *join = mint_guestbook_wrap(
    &fixture, AUTHOR_SK, CONCORD_KIND_JOIN_LEAVE, "join", 1686840217, "100",
    NULL);
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    rotator, fixture.community_id, join));
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    rival, fixture.community_id, join));

  /* The one address a Channel rotation may land at: the *prior* root, this
   * Channel's id, and the epoch it is climbing to. Every current keyholder
   * computes it, on either branch of the base race, and nobody else can. */
  uint8_t root[32], id[32];
  g_assert_true(nostr_concord_hex_decode_32(COMMUNITY_ROOT, root));
  g_assert_true(nostr_concord_hex_decode_32(CHANNEL_ID, id));
  nostr_concord_group_key_t rekey;
  g_assert_cmpint(
    nostr_concord_channel_rekey_key(root, id, TEST_EPOCH + 1, &rekey), ==,
    NOSTR_CONCORD_OK);
  char channel_address[65];
  nostr_concord_hex_encode_32(rekey.pk, channel_address);
  nostr_concord_group_key_clear(&rekey);

  guint before = gn_concord_test_published ? gn_concord_test_published->len : 0;
  RefoundResult refounded = { 0 };
  gn_concord_community_service_refound_async(rotator, fixture.community_id,
                                             NULL, on_refounded, &refounded);
  pump();
  g_assert_true(refounded.done);
  g_assert_no_error(refounded.error);
  g_assert_true(refounded.ok);
  guint after_winner = gn_concord_test_published->len;

  /* The Refounding published a rotation for the held Private Channel, at the
   * prior root's address — not at one derived from the root it just minted,
   * which no other branch could compute. */
  g_autofree gchar *channel_rotation = NULL;
  for (guint i = before; i < after_winner; i++) {
    const char *json = g_ptr_array_index(gn_concord_test_published, i);
    g_autoptr(NostrEvent) event = nostr_event_new();
    if (!nostr_event_deserialize_compact(event, json, NULL)) continue;
    if (g_strcmp0(nostr_event_get_pubkey(event), channel_address) != 0)
      continue;
    g_free(channel_rotation);
    channel_rotation = g_strdup(json);
  }
  g_assert_nonnull(channel_rotation);

  /* And the Refounder itself moved: the Channel climbed one epoch onto a key
   * the banned member never receives. */
  g_autoptr(GnConcordCommunityItem) rotated =
    gn_concord_community_service_lookup_community(rotator,
                                                  fixture.community_id);
  g_autoptr(GnConcordChannelItem) rotators_channel =
    gn_concord_community_item_find_channel(rotated, CHANNEL_ID);
  g_assert_cmpuint(gn_concord_channel_item_get_epoch(rotators_channel), ==,
                   TEST_EPOCH + 1);
  g_assert_cmpstr(gn_concord_channel_item_get_key(rotators_channel), !=,
                  CHANNEL_KEY);

  /* The racing Refounding, minted from the same prior root at the same
   * epoch. Its blobs carry a different new root: this is the fork. */
  RefoundResult raced = { 0 };
  gn_concord_community_service_refound_async(rival, fixture.community_id, NULL,
                                             on_refounded, &raced);
  pump();
  g_assert_true(raced.ok);

  g_autoptr(GPtrArray) base_wraps = published_rekey_wraps(&fixture);
  g_assert_cmpuint(base_wraps->len, ==, 2);

  /* Both members open the Channel rotation off the shared prior root, then
   * land on opposite branches of the base race. */
  gn_concord_test_signer_sk = AUTHOR_SK;
  gn_concord_test_user_pubkey = fixture.author_pubkey;
  g_assert_true(gn_concord_community_service_ingest_channel_rekey_wrap(
    winners_branch, fixture.community_id, CHANNEL_ID, channel_rotation));
  pump();
  g_assert_true(gn_concord_community_service_ingest_channel_rekey_wrap(
    losers_branch, fixture.community_id, CHANNEL_ID, channel_rotation));
  pump();
  g_assert_true(gn_concord_community_service_ingest_rekey_wrap(
    winners_branch, fixture.community_id, g_ptr_array_index(base_wraps, 0)));
  pump();
  g_assert_true(gn_concord_community_service_ingest_rekey_wrap(
    losers_branch, fixture.community_id, g_ptr_array_index(base_wraps, 1)));
  pump();

  /* The branches really did diverge: the public Channel's key comes from the
   * community_root, so a message published on one branch is unreadable on the
   * other. */
  gn_concord_test_signer_sk = OWNER_SK;
  gn_concord_test_user_pubkey = fixture.owner_pubkey;
  PublishResult on_root = { .loop = g_main_loop_new(NULL, FALSE) };
  gn_concord_community_service_publish_message_async(
    rotator, fixture.community_id, SECOND_CHANNEL, "on the winning root", NULL,
    on_publish_finished, &on_root);
  g_main_loop_run(on_root.loop);
  g_assert_true(on_root.ok);
  g_free(on_root.message);
  g_main_loop_unref(on_root.loop);
  const char *public_chat = g_ptr_array_index(gn_concord_test_published,
                                              gn_concord_test_published->len - 1);
  g_assert_true(gn_concord_community_service_ingest_wrap(
    winners_branch, fixture.community_id, SECOND_CHANNEL, public_chat));
  g_assert_false(gn_concord_community_service_ingest_wrap(
    losers_branch, fixture.community_id, SECOND_CHANNEL, public_chat));

  /* And the Private Channel opens on both, because its rotation was sealed
   * under the root the two branches shared. The base-fork loser lost its
   * root, not its Channels. */
  PublishResult in_channel = { .loop = g_main_loop_new(NULL, FALSE) };
  gn_concord_community_service_publish_message_async(
    rotator, fixture.community_id, CHANNEL_ID, "after the refounding", NULL,
    on_publish_finished, &in_channel);
  g_main_loop_run(in_channel.loop);
  g_assert_true(in_channel.ok);
  g_free(in_channel.message);
  g_main_loop_unref(in_channel.loop);
  const char *private_chat = g_ptr_array_index(
    gn_concord_test_published, gn_concord_test_published->len - 1);

  g_assert_true(gn_concord_community_service_ingest_wrap(
    winners_branch, fixture.community_id, CHANNEL_ID, private_chat));
  g_assert_true(gn_concord_community_service_ingest_wrap(
    losers_branch, fixture.community_id, CHANNEL_ID, private_chat));
  /* A device still holding the prior Channel key cannot even find the
   * address — which is the severing the Refounding was for. */
  g_assert_false(gn_concord_community_service_ingest_wrap(
    stale, fixture.community_id, CHANNEL_ID, private_chat));

  gn_concord_community_service_shutdown(rotator);
  gn_concord_community_service_shutdown(rival);
  gn_concord_community_service_shutdown(winners_branch);
  gn_concord_community_service_shutdown(losers_branch);
  gn_concord_community_service_shutdown(stale);
  gn_concord_test_reset();
  fixture_clear(&fixture);
}

/* ------------------------------------------------------------------ *
 * the same-epoch race (CORD-06 "Failure and races")
 * ------------------------------------------------------------------ */

/* Two new roots the test can order by eye. A minted root is random, and a race
 * is only testable when the test names which of the two is the lower one — so
 * these rotations are hand-minted rather than driven through refound_async. */
#define RACE_LOW_ROOT \
  "0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a"
#define RACE_LOW_CONTROL_ROOT \
  "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"
#define RACE_HIGH_ROOT \
  "f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0"
#define RACE_HIGH_CONTROL_ROOT \
  "f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1"

/* Grants @member a Role carrying BAN — what a Refounding takes (CORD-06
 * "Authority"), and therefore what a second Rotator needs to exist at all. Two
 * rotations from *one* npub at one continuity point are the same resumable
 * rotation, so a race needs two npubs. */
static void grant_ban(GnConcordCommunityService *service,
                      const Fixture *fixture, const char *member) {
  g_autofree gchar *role_content = g_strdup_printf(
    "{\"role_id\":\"%s\",\"name\":\"Marshal\",\"position\":5,"
    "\"permissions\":\"%" G_GUINT64_FORMAT "\"}",
    ROLE_ID, CONCORD_PERM_BAN);
  EditionOptions role =
    owner_edition(CONCORD_VSK_ROLE, ROLE_ID, 1, role_content);
  g_assert_true(ingest_edition(service, fixture, &role, NULL));

  g_autofree gchar *grant_eid = grant_coordinate(fixture, member);
  g_autofree gchar *grant_content = g_strdup_printf(
    "{\"member\":\"%s\",\"role_ids\":[\"%s\"]}", member, ROLE_ID);
  EditionOptions grant =
    owner_edition(CONCORD_VSK_GRANT, grant_eid, 1, grant_content);
  g_assert_true(ingest_edition(service, fixture, &grant, NULL));
}

/* One complete base rotation, minted by hand: @rotator_sk rolls the root to
 * @new_root at TEST_EPOCH + 1, from the continuity point every device in these
 * tests still holds, with one blob per recipient. Addressed under the prior
 * root, which is where a rotation has to land for a current keyholder to find
 * it (CORD-06 §2). */
static gchar *mint_base_rotation(const Fixture *fixture,
                                 const char *rotator_sk, const char *new_root,
                                 const char *new_control_root,
                                 const char *const *recipients, guint n) {
  g_autofree gchar *rotator = nostr_key_get_public(rotator_sk);
  uint8_t community[32], control_root[32], scope[32], rotator_x[32];
  uint8_t rotator_secret[32];
  g_assert_true(nostr_concord_hex_decode_32(fixture->community_id, community));
  g_assert_true(nostr_concord_hex_decode_32(new_control_root, control_root));
  g_assert_true(nostr_concord_hex_decode_32(rotator, rotator_x));
  g_assert_true(nostr_concord_hex_decode_32(rotator_sk, rotator_secret));
  nostr_concord_base_scope_id(scope);

  nostr_concord_group_key_t signer;
  g_assert_cmpint(nostr_concord_control_signer_key(control_root, community,
                                                   TEST_EPOCH + 1, &signer),
                  ==, NOSTR_CONCORD_OK);

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_array(builder);
  for (guint i = 0; i < n; i++) {
    nostr_concord_rekey_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.form = NOSTR_CONCORD_REKEY_BASE_MEMBER;
    memcpy(blob.scope_id, scope, sizeof(scope));
    blob.epoch = TEST_EPOCH + 1;
    g_assert_true(nostr_concord_hex_decode_32(new_root, blob.new_key));
    memcpy(blob.new_control_pk, signer.pk, sizeof(blob.new_control_pk));
    blob.has_control_pk = true;

    uint8_t plaintext[CONCORD_REKEY_BLOB_STAFF_BYTES];
    size_t len = 0;
    g_assert_cmpint(nostr_concord_rekey_blob_pack(&blob, plaintext,
                                                  sizeof(plaintext), &len),
                    ==, NOSTR_CONCORD_OK);
    nostr_concord_rekey_blob_clear(&blob);

    uint8_t recipient_x[32], convkey[32], locator[32];
    g_assert_true(nostr_concord_hex_decode_32(recipients[i], recipient_x));
    g_assert_cmpint(nostr_nip44_convkey(rotator_secret, recipient_x, convkey),
                    ==, 0);
    char *wrapped = NULL;
    g_assert_cmpint(
      nostr_nip44_encrypt_v2_with_convkey(convkey, plaintext, len, &wrapped),
      ==, 0);
    memset(convkey, 0, sizeof(convkey));
    memset(plaintext, 0, sizeof(plaintext));
    g_assert_cmpint(nostr_concord_rekey_locator(rotator_x, recipient_x, scope,
                                                TEST_EPOCH + 1, locator),
                    ==, NOSTR_CONCORD_OK);
    char locator_hex[65];
    nostr_concord_hex_encode_32(locator, locator_hex);

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "locator");
    json_builder_add_string_value(builder, locator_hex);
    json_builder_set_member_name(builder, "wrapped");
    json_builder_add_string_value(builder, wrapped);
    json_builder_end_object(builder);
    free(wrapped);
  }
  json_builder_end_array(builder);
  nostr_concord_group_key_clear(&signer);
  memset(rotator_secret, 0, sizeof(rotator_secret));

  g_autoptr(JsonGenerator) generator = json_generator_new();
  JsonNode *blobs = json_builder_get_root(builder);
  json_generator_set_root(generator, blobs);
  g_autofree gchar *content = json_generator_to_data(generator, NULL);
  json_node_free(blobs);

  /* The commitment over the root both Rotators still held: what makes these
   * two rotations siblings at one continuity point rather than two unrelated
   * forks (CORD-02 A.5). */
  uint8_t prior[32], commitment[32];
  g_assert_true(nostr_concord_hex_decode_32(COMMUNITY_ROOT, prior));
  g_assert_cmpint(nostr_concord_epoch_commitment(TEST_EPOCH, prior, commitment),
                  ==, NOSTR_CONCORD_OK);
  char prevcommit[65], scope_hex[65];
  nostr_concord_hex_encode_32(commitment, prevcommit);
  nostr_concord_hex_encode_32(scope, scope_hex);
  g_autofree gchar *newepoch =
    g_strdup_printf("%d", (int)(TEST_EPOCH + 1));
  g_autofree gchar *prevepoch = g_strdup_printf("%d", (int)TEST_EPOCH);

  g_autoptr(NostrEvent) rumor = nostr_event_new();
  nostr_event_set_kind(rumor, CONCORD_KIND_REKEY);
  nostr_event_set_pubkey(rumor, rotator);
  nostr_event_set_created_at(rumor, 1686840218);
  nostr_event_set_content(rumor, content);
  NostrTags *tags = nostr_tags_new(0);
  tags = nostr_tags_append_unique(tags,
                                  nostr_tag_new("scope", scope_hex, NULL));
  tags =
    nostr_tags_append_unique(tags, nostr_tag_new("newepoch", newepoch, NULL));
  tags =
    nostr_tags_append_unique(tags, nostr_tag_new("prevepoch", prevepoch, NULL));
  tags = nostr_tags_append_unique(
    tags, nostr_tag_new("prevcommit", prevcommit, NULL));
  tags =
    nostr_tags_append_unique(tags, nostr_tag_new("chunk", "0", "1", NULL));
  nostr_event_set_tags(rumor, tags);
  g_autofree gchar *rumor_json = nostr_event_serialize_compact(rumor);

  nostr_concord_group_key_t rekey;
  base_rekey_key(fixture, &rekey);
  char *seal_content = NULL;
  g_assert_cmpint(
    nostr_concord_stream_seal(rekey.conv_key, rumor_json, &seal_content), ==,
    NOSTR_CONCORD_OK);
  g_autoptr(NostrEvent) seal = nostr_event_new();
  nostr_event_set_kind(seal, CONCORD_SEAL_ENCRYPTED);
  nostr_event_set_pubkey(seal, rotator);
  nostr_event_set_created_at(seal, 1686840218);
  nostr_event_set_content(seal, seal_content);
  nostr_event_set_tags(seal, nostr_tags_new(0));
  free(seal_content);
  g_assert_cmpint(nostr_event_sign(seal, rotator_sk), ==, 0);
  g_autofree gchar *seal_json = nostr_event_serialize_compact(seal);

  gchar *wrap = wrap_at_key(&rekey, seal_json, 1686840218);
  nostr_concord_group_key_clear(&rekey);
  return wrap;
}

/* Where a public Channel's plane sits under @root. Its key comes from the
 * community_root, so this address *is* which fork a device landed on. */
static gchar *public_channel_address(const char *root, const char *channel_id,
                                     guint64 epoch) {
  uint8_t secret[32], id[32];
  g_assert_true(nostr_concord_hex_decode_32(root, secret));
  g_assert_true(nostr_concord_hex_decode_32(channel_id, id));
  nostr_concord_group_key_t key;
  g_assert_cmpint(nostr_concord_channel_key(secret, id, epoch, &key), ==,
                  NOSTR_CONCORD_OK);
  char hex[65];
  nostr_concord_hex_encode_32(key.pk, hex);
  nostr_concord_group_key_clear(&key);
  return g_strdup(hex);
}

/* A message written into one fork's public Channel, at the address @root
 * derives — readable only by a device that holds that root, now or retired. */
static gchar *mint_fork_chat(const Fixture *fixture, const char *root,
                             const char *channel_id, guint64 epoch,
                             const char *content) {
  uint8_t secret[32], id[32];
  g_assert_true(nostr_concord_hex_decode_32(root, secret));
  g_assert_true(nostr_concord_hex_decode_32(channel_id, id));
  nostr_concord_group_key_t key;
  g_assert_cmpint(nostr_concord_channel_key(secret, id, epoch, &key), ==,
                  NOSTR_CONCORD_OK);

  g_autofree gchar *epoch_tag = g_strdup_printf("%" G_GUINT64_FORMAT, epoch);
  g_autoptr(NostrEvent) rumor = nostr_event_new();
  nostr_event_set_kind(rumor, CONCORD_KIND_MESSAGE);
  nostr_event_set_pubkey(rumor, fixture->author_pubkey);
  nostr_event_set_created_at(rumor, 1686840300);
  nostr_event_set_content(rumor, content);
  nostr_event_set_tags(
    rumor, nostr_tags_new(3, nostr_tag_new("channel", channel_id, NULL),
                          nostr_tag_new("epoch", epoch_tag, NULL),
                          nostr_tag_new("ms", "250", NULL)));
  g_autofree gchar *rumor_json = nostr_event_serialize_compact(rumor);

  char *seal_content = NULL;
  g_assert_cmpint(
    nostr_concord_stream_seal(key.conv_key, rumor_json, &seal_content), ==,
    NOSTR_CONCORD_OK);
  g_autoptr(NostrEvent) seal = nostr_event_new();
  nostr_event_set_kind(seal, CONCORD_SEAL_ENCRYPTED);
  nostr_event_set_pubkey(seal, fixture->author_pubkey);
  nostr_event_set_created_at(seal, 1686840300);
  nostr_event_set_content(seal, seal_content);
  nostr_event_set_tags(seal, nostr_tags_new(0));
  free(seal_content);
  g_assert_cmpint(nostr_event_sign(seal, AUTHOR_SK), ==, 0);
  g_autofree gchar *seal_json = nostr_event_serialize_compact(seal);

  gchar *wrap = wrap_at_key(&key, seal_json, 1686840300);
  nostr_concord_group_key_clear(&key);
  return wrap;
}

static gchar *channel_address_of(GnConcordCommunityService *service,
                                 const Fixture *fixture, const char *channel_id,
                                 guint64 *out_epoch) {
  g_autoptr(GnConcordCommunityItem) item =
    gn_concord_community_service_lookup_community(service,
                                                  fixture->community_id);
  g_assert_nonnull(item);
  g_autoptr(GnConcordChannelItem) channel =
    gn_concord_community_item_find_channel(item, channel_id);
  g_assert_nonnull(channel);
  if (out_epoch) *out_epoch = gn_concord_channel_item_get_epoch(channel);
  return g_strdup(gn_concord_channel_item_get_stream_pubkey(channel));
}

/* Two Refoundings reach one epoch, and every client settles on the same one.
 *
 * The rule is the whole of CORD-06 "Failure and races": among authorized
 * candidates at one continuity point, the lexicographically lowest new base
 * key wins. It has to hold whichever order the two rotations arrive in, so
 * both orders run here — and the answer is checked against an address the test
 * derives itself from the lower root, not merely against the other device
 * agreeing.
 *
 * The public Channel is the instrument: its key comes from the community_root,
 * so its address is a direct readout of which fork a device is on. */
static void test_rotation_race_converges_on_lowest(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  gn_concord_test_reset();
  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;

  gn_concord_test_signer_sk = AUTHOR_SK;
  gn_concord_test_user_pubkey = fixture.author_pubkey;
  g_autoptr(GnConcordCommunityService) high_first =
    joined_service(&fixture, context);
  g_autoptr(GnConcordCommunityService) low_first =
    joined_service(&fixture, context);
  /* A device that receives neither rotation, to prove the fork messages below
   * are unreadable without the root that addressed them. */
  g_autoptr(GnConcordCommunityService) stale =
    joined_service(&fixture, context);

  g_autofree gchar *admin = nostr_key_get_public(ADMIN_SK);
  grant_ban(high_first, &fixture, admin);
  grant_ban(low_first, &fixture, admin);

  const char *recipients[] = { fixture.owner_pubkey, fixture.author_pubkey,
                               admin };
  g_autofree gchar *low = mint_base_rotation(
    &fixture, OWNER_SK, RACE_LOW_ROOT, RACE_LOW_CONTROL_ROOT, recipients, 3);
  g_autofree gchar *high = mint_base_rotation(
    &fixture, ADMIN_SK, RACE_HIGH_ROOT, RACE_HIGH_CONTROL_ROOT, recipients, 3);
  g_assert_cmpstr(RACE_LOW_ROOT, <, RACE_HIGH_ROOT);

  /* One device adopts the higher fork first and has to heal down to the
   * lower one; the other adopts the lower one and must refuse the higher. */
  g_assert_true(gn_concord_community_service_ingest_rekey_wrap(
    high_first, fixture.community_id, high));
  pump();
  g_assert_true(gn_concord_community_service_ingest_rekey_wrap(
    low_first, fixture.community_id, low));
  pump();

  guint64 epoch = 0;
  g_autofree gchar *settled =
    channel_address_of(low_first, &fixture, SECOND_CHANNEL, &epoch);
  g_autofree gchar *expected =
    public_channel_address(RACE_LOW_ROOT, SECOND_CHANNEL, epoch);
  g_assert_cmpstr(settled, ==, expected);

  g_assert_true(gn_concord_community_service_ingest_rekey_wrap(
    high_first, fixture.community_id, low));
  pump();
  g_assert_true(gn_concord_community_service_ingest_rekey_wrap(
    low_first, fixture.community_id, high));
  pump();

  /* Converged, and on the value the rule names — not merely on each other. */
  g_autofree gchar *healed =
    channel_address_of(high_first, &fixture, SECOND_CHANNEL, NULL);
  g_assert_cmpstr(healed, ==, expected);

  /* And the settled epoch did not re-fork upward when the higher sibling
   * arrived late, which is exactly what a flaky fetch looks like. */
  g_autofree gchar *unmoved =
    channel_address_of(low_first, &fixture, SECOND_CHANNEL, NULL);
  g_assert_cmpstr(unmoved, ==, expected);

  /* Re-delivering the winner is a no-op rather than a second adopt. */
  g_assert_true(gn_concord_community_service_ingest_rekey_wrap(
    low_first, fixture.community_id, low));
  pump();
  g_autofree gchar *replayed =
    channel_address_of(low_first, &fixture, SECOND_CHANNEL, NULL);
  g_assert_cmpstr(replayed, ==, expected);

  /* Messages sent into the losing fork stay readable on both devices: one
   * held that root and retired it, the other opened the blob only to compare
   * it and kept the fork's key anyway (CORD-06 "Failure and races"). */
  g_autofree gchar *fork_chat = mint_fork_chat(
    &fixture, RACE_HIGH_ROOT, SECOND_CHANNEL, epoch, "sent into the loser");
  g_assert_true(gn_concord_community_service_ingest_wrap(
    high_first, fixture.community_id, SECOND_CHANNEL, fork_chat));
  g_assert_true(gn_concord_community_service_ingest_wrap(
    low_first, fixture.community_id, SECOND_CHANNEL, fork_chat));
  /* Retention is retention, not a hole: a device that never held that root
   * cannot read it. */
  g_assert_false(gn_concord_community_service_ingest_wrap(
    stale, fixture.community_id, SECOND_CHANNEL, fork_chat));

  /* And the epoch just left is still readable too — the same retention, one
   * rotation earlier. */
  g_autofree gchar *before_chat = mint_fork_chat(
    &fixture, COMMUNITY_ROOT, SECOND_CHANNEL, epoch, "before the rotation");
  g_assert_true(gn_concord_community_service_ingest_wrap(
    low_first, fixture.community_id, SECOND_CHANNEL, before_chat));

  gn_concord_community_service_shutdown(high_first);
  gn_concord_community_service_shutdown(low_first);
  gn_concord_community_service_shutdown(stale);
  gn_concord_test_reset();
  fixture_clear(&fixture);
}

/* The retained roots are membership material: they ride the Community List to
 * this npub's own devices (CORD-02 §8), because a fork's messages must stay
 * readable on the phone as well as the laptop that judged the race. */
static void test_retired_roots_ride_the_community_list(void) {
  Fixture fixture = { 0 };
  list_test_begin(&fixture);
  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;

  gn_concord_test_signer_sk = AUTHOR_SK;
  gn_concord_test_user_pubkey = fixture.author_pubkey;
  g_autoptr(GnConcordCommunityService) device =
    joined_service(&fixture, context);
  g_autofree gchar *admin = nostr_key_get_public(ADMIN_SK);
  grant_ban(device, &fixture, admin);

  const char *recipients[] = { fixture.owner_pubkey, fixture.author_pubkey,
                               admin };
  g_autofree gchar *low = mint_base_rotation(
    &fixture, OWNER_SK, RACE_LOW_ROOT, RACE_LOW_CONTROL_ROOT, recipients, 3);
  g_autofree gchar *high = mint_base_rotation(
    &fixture, ADMIN_SK, RACE_HIGH_ROOT, RACE_HIGH_CONTROL_ROOT, recipients, 3);

  g_assert_true(gn_concord_community_service_ingest_rekey_wrap(
    device, fixture.community_id, low));
  pump();
  g_assert_true(gn_concord_community_service_ingest_rekey_wrap(
    device, fixture.community_id, high));
  pump();
  pump();

  JsonNode *listed = decrypt_published_document(
    published_last_of_kind(CONCORD_COMMUNITY_LIST), AUTHOR_SK);
  g_assert_nonnull(listed);
  JsonArray *entries =
    json_object_get_array_member(json_node_get_object(listed), "entries");
  JsonObject *membership = NULL;
  for (guint i = 0; i < json_array_get_length(entries); i++) {
    JsonObject *candidate = json_array_get_object_element(entries, i);
    if (g_strcmp0(json_object_get_string_member(candidate, "community_id"),
                  fixture.community_id) == 0)
      membership = candidate;
  }
  g_assert_nonnull(membership);
  JsonObject *current = json_object_get_object_member(membership, "current");
  g_assert_nonnull(current);
  g_assert_cmpstr(json_object_get_string_member(current, "community_root"), ==,
                  RACE_LOW_ROOT);

  /* Both forks: the epoch left behind, and the sibling that lost. */
  JsonArray *retired = json_object_get_array_member(current, "retired");
  g_assert_nonnull(retired);
  gboolean holds_prior = FALSE, holds_loser = FALSE;
  for (guint i = 0; i < json_array_get_length(retired); i++) {
    JsonObject *entry = json_array_get_object_element(retired, i);
    const char *root = json_object_get_string_member(entry, "root");
    if (g_strcmp0(root, COMMUNITY_ROOT) == 0) {
      holds_prior = TRUE;
      g_assert_cmpint(json_object_get_int_member(entry, "epoch"), ==,
                      TEST_EPOCH);
    }
    if (g_strcmp0(root, RACE_HIGH_ROOT) == 0) {
      holds_loser = TRUE;
      g_assert_cmpint(json_object_get_int_member(entry, "epoch"), ==,
                      TEST_EPOCH + 1);
    }
  }
  g_assert_true(holds_prior);
  g_assert_true(holds_loser);

  /* A second device reconstructs from that entry and reads the losing fork,
   * which is the whole point of carrying them. */
  g_autoptr(JsonGenerator) generator = json_generator_new();
  json_generator_set_root(generator,
                          json_object_get_member(membership, "current"));
  g_autofree gchar *material = json_generator_to_data(generator, NULL);
  g_autoptr(GnConcordCommunityService) second =
    gn_concord_community_service_new(context);
  g_autoptr(GError) error = NULL;
  g_assert_true(
    gn_concord_community_service_accept_bundle(second, material, &error));
  g_assert_no_error(error);

  guint64 epoch = 0;
  g_autofree gchar *address =
    channel_address_of(second, &fixture, SECOND_CHANNEL, &epoch);
  g_autofree gchar *expected =
    public_channel_address(RACE_LOW_ROOT, SECOND_CHANNEL, epoch);
  g_assert_cmpstr(address, ==, expected);

  g_autofree gchar *fork_chat = mint_fork_chat(
    &fixture, RACE_HIGH_ROOT, SECOND_CHANNEL, epoch, "sent into the loser");
  g_assert_true(gn_concord_community_service_ingest_wrap(
    second, fixture.community_id, SECOND_CHANNEL, fork_chat));

  /* An invite bundle carries no `retired` — it is join material for a
   * stranger, and the epochs a Refounding severed are not theirs to read. A
   * re-accepted link therefore arrives without them, and must not take the
   * set away from the membership it refreshes. */
  g_autoptr(JsonNode) refreshed =
    json_node_copy(json_object_get_member(membership, "current"));
  json_object_remove_member(json_node_get_object(refreshed), "retired");
  g_autoptr(JsonGenerator) refresher = json_generator_new();
  json_generator_set_root(refresher, refreshed);
  g_autofree gchar *plain = json_generator_to_data(refresher, NULL);
  g_assert_true(
    gn_concord_community_service_accept_bundle(second, plain, &error));
  g_assert_no_error(error);
  g_autofree gchar *after_refresh = mint_fork_chat(
    &fixture, RACE_HIGH_ROOT, SECOND_CHANNEL, epoch, "still readable");
  g_assert_true(gn_concord_community_service_ingest_wrap(
    second, fixture.community_id, SECOND_CHANNEL, after_refresh));

  json_node_free(listed);
  gn_concord_community_service_shutdown(device);
  gn_concord_community_service_shutdown(second);
  list_test_end(&fixture);
}

/* A recipient online through a whole Refounding ends up holding the same
 * Private Channel keys as the Refounder, whichever order the two halves land
 * in (nostrc-8h0l).
 *
 * The live order is the dangerous one: the base chunks publish first and the
 * Channel rotations second, both addressed under the prior root. A device that
 * folds the base set and adopts has, at that instant, re-derived every Channel
 * rekey plane under the *new* root — so the rotation still sitting at the prior
 * root's address arrives at a plane nobody is watching, and that device keeps
 * the old Channel key while the removed member keeps reading it. Retaining the
 * prior root is what keeps that address derivable. */
static void test_refounding_channel_rotation_after_base(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  gn_concord_test_reset();
  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;

  gn_concord_test_signer_sk = OWNER_SK;
  gn_concord_test_user_pubkey = fixture.owner_pubkey;
  g_autoptr(GnConcordCommunityService) rotator =
    joined_service(&fixture, context);
  g_autoptr(GnConcordCommunityService) recipient =
    joined_service(&fixture, context);

  g_autofree gchar *join = mint_guestbook_wrap(
    &fixture, AUTHOR_SK, CONCORD_KIND_JOIN_LEAVE, "join", 1686840217, "100",
    NULL);
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    rotator, fixture.community_id, join));

  uint8_t root[32], id[32];
  g_assert_true(nostr_concord_hex_decode_32(COMMUNITY_ROOT, root));
  g_assert_true(nostr_concord_hex_decode_32(CHANNEL_ID, id));
  nostr_concord_group_key_t rekey;
  g_assert_cmpint(
    nostr_concord_channel_rekey_key(root, id, TEST_EPOCH + 1, &rekey), ==,
    NOSTR_CONCORD_OK);
  char channel_address[65];
  nostr_concord_hex_encode_32(rekey.pk, channel_address);
  nostr_concord_group_key_clear(&rekey);

  guint before = gn_concord_test_published ? gn_concord_test_published->len : 0;
  RefoundResult refounded = { 0 };
  gn_concord_community_service_refound_async(rotator, fixture.community_id,
                                             NULL, on_refounded, &refounded);
  pump();
  g_assert_true(refounded.ok);

  g_autofree gchar *channel_rotation = NULL;
  for (guint i = before; i < gn_concord_test_published->len; i++) {
    const char *json = g_ptr_array_index(gn_concord_test_published, i);
    g_autoptr(NostrEvent) event = nostr_event_new();
    if (!nostr_event_deserialize_compact(event, json, NULL)) continue;
    if (g_strcmp0(nostr_event_get_pubkey(event), channel_address) != 0)
      continue;
    g_free(channel_rotation);
    channel_rotation = g_strdup(json);
  }
  g_assert_nonnull(channel_rotation);

  g_autoptr(GPtrArray) base_wraps = published_rekey_wraps(&fixture);
  g_assert_cmpuint(base_wraps->len, ==, 1);

  /* The base first, adopted in full — the recipient is now on the new root and
   * has re-addressed every plane it holds. */
  gn_concord_test_signer_sk = AUTHOR_SK;
  gn_concord_test_user_pubkey = fixture.author_pubkey;
  g_assert_true(gn_concord_community_service_ingest_rekey_wrap(
    recipient, fixture.community_id, g_ptr_array_index(base_wraps, 0)));
  pump();

  /* And only then the Channel rotation, at the retired root's address. */
  g_assert_true(gn_concord_community_service_ingest_channel_rekey_wrap(
    recipient, fixture.community_id, CHANNEL_ID, channel_rotation));
  pump();

  g_autoptr(GnConcordCommunityItem) held =
    gn_concord_community_service_lookup_community(recipient,
                                                  fixture.community_id);
  g_autoptr(GnConcordChannelItem) channel =
    gn_concord_community_item_find_channel(held, CHANNEL_ID);
  g_assert_cmpuint(gn_concord_channel_item_get_epoch(channel), ==,
                   TEST_EPOCH + 1);
  g_assert_cmpstr(gn_concord_channel_item_get_key(channel), !=, CHANNEL_KEY);

  /* Same key as the Refounder's, proven where it counts: a message the
   * Refounder publishes into the rotated Channel opens here. */
  gn_concord_test_signer_sk = OWNER_SK;
  gn_concord_test_user_pubkey = fixture.owner_pubkey;
  PublishResult sent = { .loop = g_main_loop_new(NULL, FALSE) };
  gn_concord_community_service_publish_message_async(
    rotator, fixture.community_id, CHANNEL_ID, "after the refounding", NULL,
    on_publish_finished, &sent);
  g_main_loop_run(sent.loop);
  g_assert_true(sent.ok);
  g_free(sent.message);
  g_main_loop_unref(sent.loop);
  const char *chat = g_ptr_array_index(gn_concord_test_published,
                                       gn_concord_test_published->len - 1);
  g_assert_true(gn_concord_community_service_ingest_wrap(
    recipient, fixture.community_id, CHANNEL_ID, chat));

  gn_concord_community_service_shutdown(rotator);
  gn_concord_community_service_shutdown(recipient);
  gn_concord_test_reset();
  fixture_clear(&fixture);
}

/* A Private Channel rotates on its own. It is independently keyed and
 * cryptographically unrelated to the community_root (CORD-03), so severing
 * someone from one Channel costs exactly that Channel — no Refounding, no
 * other plane moved.
 *
 * The published address is the tell: it derives from the community_root, not
 * from the Channel key being replaced, so a member losing the Channel is still
 * reachable at it (CORD-06 §2). */
static void test_channel_rekey_roundtrip(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  gn_concord_test_reset();
  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;

  gn_concord_test_signer_sk = OWNER_SK;
  gn_concord_test_user_pubkey = fixture.owner_pubkey;
  g_autoptr(GnConcordCommunityService) rotator =
    joined_service(&fixture, context);
  g_autoptr(GnConcordCommunityService) member =
    joined_service(&fixture, context);

  g_autofree gchar *join = mint_guestbook_wrap(
    &fixture, AUTHOR_SK, CONCORD_KIND_JOIN_LEAVE, "join", 1686840217, "100",
    NULL);
  g_assert_true(gn_concord_community_service_ingest_guestbook_wrap(
    rotator, fixture.community_id, join));

  /* The address every recipient watches: the prior community_root, this
   * Channel's id, and the epoch it is climbing to. */
  uint8_t root[32], id[32];
  g_assert_true(nostr_concord_hex_decode_32(COMMUNITY_ROOT, root));
  g_assert_true(nostr_concord_hex_decode_32(CHANNEL_ID, id));
  nostr_concord_group_key_t rekey;
  g_assert_cmpint(
    nostr_concord_channel_rekey_key(root, id, TEST_EPOCH + 1, &rekey), ==,
    NOSTR_CONCORD_OK);
  char address[65];
  nostr_concord_hex_encode_32(rekey.pk, address);
  nostr_concord_group_key_clear(&rekey);

  guint before = gn_concord_test_published ? gn_concord_test_published->len : 0;
  RefoundResult rekeyed = { 0 };
  gn_concord_community_service_rekey_channel_async(
    rotator, fixture.community_id, CHANNEL_ID, NULL, 0, NULL, on_channel_rekeyed,
    &rekeyed);
  pump();
  g_assert_true(rekeyed.done);
  g_assert_no_error(rekeyed.error);
  g_assert_true(rekeyed.ok);

  const char *rotation = NULL;
  for (guint i = before; i < gn_concord_test_published->len; i++) {
    const char *json = g_ptr_array_index(gn_concord_test_published, i);
    g_autoptr(NostrEvent) event = nostr_event_new();
    if (!nostr_event_deserialize_compact(event, json, NULL)) continue;
    if (g_strcmp0(nostr_event_get_pubkey(event), address) == 0) rotation = json;
  }
  g_assert_nonnull(rotation);

  /* The member adopts it, and both land on the same Channel key: a message
   * published into the rotated Channel opens for the member and for nobody
   * still holding the old one. */
  gn_concord_test_signer_sk = AUTHOR_SK;
  gn_concord_test_user_pubkey = fixture.author_pubkey;
  g_assert_true(gn_concord_community_service_ingest_channel_rekey_wrap(
    member, fixture.community_id, CHANNEL_ID, rotation));
  pump();

  gn_concord_test_signer_sk = OWNER_SK;
  gn_concord_test_user_pubkey = fixture.owner_pubkey;
  PublishResult outcome = { .loop = g_main_loop_new(NULL, FALSE) };
  gn_concord_community_service_publish_message_async(
    rotator, fixture.community_id, CHANNEL_ID, "after the rekey", NULL,
    on_publish_finished, &outcome);
  g_main_loop_run(outcome.loop);
  g_assert_true(outcome.ok);
  g_free(outcome.message);
  g_main_loop_unref(outcome.loop);
  const char *chat = g_ptr_array_index(gn_concord_test_published,
                                       gn_concord_test_published->len - 1);

  g_assert_true(gn_concord_community_service_ingest_wrap(
    member, fixture.community_id, CHANNEL_ID, chat));
  /* A device still on the old Channel key cannot even find the address. */
  g_autoptr(GnConcordCommunityService) stale =
    joined_service(&fixture, context);
  g_assert_false(gn_concord_community_service_ingest_wrap(
    stale, fixture.community_id, CHANNEL_ID, chat));

  /* And nothing else moved: the base epoch is where it was, so the Community
   * owes no Refounding and every other plane is untouched. */
  g_assert_false(gn_concord_community_service_refounding_due(
    rotator, fixture.community_id));

  gn_concord_community_service_shutdown(rotator);
  gn_concord_community_service_shutdown(member);
  gn_concord_community_service_shutdown(stale);
  gn_concord_test_reset();
  fixture_clear(&fixture);
}

/* A public Channel has no independent rotation: its key comes from the
 * community_root, so it moves when the base does and rekeying it alone would
 * address a plane nobody else computes (CORD-03, CORD-06 §1). And a Rotator
 * without MANAGE_CHANNELS cannot rekey at all — holding the Channel key is
 * not authority over it. */
static void test_channel_rekey_refusals(void) {
  Fixture fixture = { 0 };
  fixture_init(&fixture);
  gn_concord_test_reset();
  GnostrPluginContext *context = (GnostrPluginContext *)&fixture;

  gn_concord_test_signer_sk = OWNER_SK;
  gn_concord_test_user_pubkey = fixture.owner_pubkey;
  g_autoptr(GnConcordCommunityService) owner_side =
    joined_service(&fixture, context);

  RefoundResult public_channel = { 0 };
  gn_concord_community_service_rekey_channel_async(
    owner_side, fixture.community_id, SECOND_CHANNEL, NULL, 0, NULL,
    on_channel_rekeyed, &public_channel);
  pump();
  g_assert_false(public_channel.ok);
  g_assert_error(public_channel.error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error(&public_channel.error);

  gn_concord_test_signer_sk = AUTHOR_SK;
  gn_concord_test_user_pubkey = fixture.author_pubkey;
  g_autoptr(GnConcordCommunityService) roleless =
    joined_service(&fixture, context);
  RefoundResult unauthorized = { 0 };
  gn_concord_community_service_rekey_channel_async(
    roleless, fixture.community_id, CHANNEL_ID, NULL, 0, NULL,
    on_channel_rekeyed, &unauthorized);
  pump();
  g_assert_false(unauthorized.ok);
  g_assert_error(unauthorized.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error(&unauthorized.error);

  gn_concord_community_service_shutdown(owner_side);
  gn_concord_community_service_shutdown(roleless);
  gn_concord_test_reset();
  fixture_clear(&fixture);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/concord/crypto/nip44-bytes-roundtrip",
                  test_nip44_bytes_roundtrip);
  g_test_add_func("/concord/refound/roundtrip", test_refounding_roundtrip);
  g_test_add_func("/concord/refound/re-anchors-control",
                  test_refounding_re_anchors_control);
  g_test_add_func("/concord/refound/rotates-private-channels",
                  test_refounding_rotates_private_channels);
  g_test_add_func("/concord/refound/seeds-guestbook",
                  test_refounding_seeds_guestbook);
  g_test_add_func("/concord/refound/aborts-on-unsettled-control",
                  test_refounding_aborts_on_unsettled_control);
  g_test_add_func("/concord/refound/debt-from-another-staffer",
                  test_refounding_debt_from_another_staffer);
  g_test_add_func("/concord/refound/pays-the-debt",
                  test_refounding_pays_the_debt);
  g_test_add_func("/concord/rekey/channel-roundtrip",
                  test_channel_rekey_roundtrip);
  g_test_add_func("/concord/rekey/channel-refusals",
                  test_channel_rekey_refusals);
  g_test_add_func("/concord/race/converges-on-lowest",
                  test_rotation_race_converges_on_lowest);
  g_test_add_func("/concord/race/retired-roots-ride-the-list",
                  test_retired_roots_ride_the_community_list);
  g_test_add_func("/concord/refound/channel-rotation-after-base",
                  test_refounding_channel_rotation_after_base);
  g_test_add_func("/concord/refound/requires-ban",
                  test_refounding_requires_ban);
  g_test_add_func("/concord/service/accept-bundle", test_accept_bundle);
  g_test_add_func("/concord/service/reject-forged-owner",
                  test_reject_forged_owner);
  g_test_add_func("/concord/service/reject-expired-invite",
                  test_reject_expired_invite);
  g_test_add_func("/concord/service/reject-oversized-bundle",
                  test_reject_oversized_bundle);
  g_test_add_func("/concord/service/message-roundtrip", test_message_roundtrip);
  g_test_add_func("/concord/service/ordering-uses-ms", test_ordering_uses_ms);
  g_test_add_func("/concord/service/binding-rejections",
                  test_binding_rejections);
  g_test_add_func("/concord/service/wrong-channel-key",
                  test_wrong_channel_key_cannot_open);
  g_test_add_func("/concord/service/public-channel-root",
                  test_public_channel_uses_community_root);
  g_test_add_func("/concord/service/publish-offline",
                  test_publish_without_signer_fails_cleanly);
  g_test_add_func("/concord/service/publish-roundtrip", test_publish_roundtrip);
  g_test_add_func("/concord/service/community-list-roundtrip",
                  test_community_list_roundtrip);
  g_test_add_func("/concord/service/community-list-round-trips-foreign",
                  test_community_list_round_trips_foreign_fields);
  g_test_add_func("/concord/service/community-list-fails-closed",
                  test_community_list_never_published_before_read);
  g_test_add_func("/concord/control/metadata-is-authority",
                  test_control_metadata_is_authority);
  g_test_add_func("/concord/control/defines-channels",
                  test_control_defines_channels);
  g_test_add_func("/concord/control/private-channel-without-key",
                  test_control_private_channel_without_key);
  g_test_add_func("/concord/control/authority-is-rejection",
                  test_control_authority_is_rejection);
  g_test_add_func("/concord/control/vac-blocks-until-synced",
                  test_control_vac_blocks_until_synced);
  g_test_add_func("/concord/control/banlist-silences",
                  test_control_banlist_silences);
  g_test_add_func("/concord/control/rejections", test_control_rejections);
  g_test_add_func("/concord/control/invite-registry",
                  test_control_invite_registry);
  g_test_add_func("/concord/control/split-address",
                  test_control_split_address);
  g_test_add_func("/concord/guestbook/coalesces", test_guestbook_coalesces);
  g_test_add_func("/concord/guestbook/drops-forged-future",
                  test_guestbook_drops_forged_future);
  g_test_add_func("/concord/guestbook/snapshot-seeds",
                  test_guestbook_snapshot_seeds);
  g_test_add_func("/concord/guestbook/snapshot-chunks-stand-alone",
                  test_guestbook_snapshot_chunks_stand_alone);
  g_test_add_func("/concord/guestbook/observation-counts-forward",
                  test_guestbook_observation_counts_forward);
  g_test_add_func("/concord/guestbook/kick-needs-authority",
                  test_guestbook_kick_needs_authority);
  g_test_add_func("/concord/guestbook/join-announced-on-accept",
                  test_guestbook_join_announced_on_accept);
  g_test_add_func("/concord/invite/mint-roundtrip", test_invite_mint_roundtrip);
  g_test_add_func("/concord/invite/retire-leaves-other-creators-links",
                  test_invite_retire_leaves_other_creators_links);
  g_test_add_func("/concord/invite/mint-fails-closed",
                  test_invite_never_minted_before_list_read);
  g_test_add_func("/concord/invite/mint-requires-permission",
                  test_invite_requires_permission);
  g_test_add_func("/concord/invite/direct-roundtrip",
                  test_direct_invite_roundtrip);
  return g_test_run();
}
