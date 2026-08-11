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

#include <stdlib.h>
#include <string.h>

/* Test hooks owned by the offline host stubs. */
extern const char *gn_concord_test_signer_sk;
extern const char *gn_concord_test_user_pubkey;
extern char *gn_concord_test_published_json;

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

  gn_concord_test_signer_sk = AUTHOR_SK;
  gn_concord_test_user_pubkey = fixture.author_pubkey;
  g_clear_pointer(&gn_concord_test_published_json, free);

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
   * derived stream key — never by the member. */
  g_autoptr(NostrEvent) wrap = nostr_event_new();
  g_assert_true(nostr_event_deserialize_compact(
    wrap, gn_concord_test_published_json, NULL));
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
    service, fixture.community_id, CHANNEL_ID,
    gn_concord_test_published_json));
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
  gn_concord_test_signer_sk = NULL;
  gn_concord_test_user_pubkey = NULL;
  g_clear_pointer(&gn_concord_test_published_json, free);
  fixture_clear(&fixture);
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
  return g_test_run();
}
