/* Concord service tests.
 *
 * Every case here mints its own wire events in-process: a Concord plane is
 * pure derivation, so the whole read path — wrap, seal, rumor, binding checks
 * — is exercisable with no relay and no signer.
 */

#include "../gn-concord-community-service.h"
#include "../model/gn-concord-channel-item.h"

#include <json-glib/json-glib.h>
#include <nip_concord.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-tag.h>
#include <nostr-utils.h>
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

/* ------------------------------------------------------------------ *
 * the Community List (CORD-02 §8)
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

/* Opens a published List the way another of the member's devices would: NIP-44
 * under the conversation key they share with themselves. */
static JsonNode *decrypt_published_list(const char *event_json) {
  g_autoptr(NostrEvent) event = nostr_event_new();
  g_assert_true(nostr_event_deserialize_compact(event, event_json, NULL));
  g_assert_cmpint(nostr_event_validate(event, NULL), ==,
                  NOSTR_EVENT_VALIDATION_OK);

  g_autofree gchar *pubkey = nostr_key_get_public(AUTHOR_SK);
  uint8_t sk[32], pk[32], convkey[32];
  g_assert_true(nostr_hex2bin(sk, AUTHOR_SK, sizeof(sk)));
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

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
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
  g_test_add_func("/concord/control/split-address",
                  test_control_split_address);
  return g_test_run();
}
