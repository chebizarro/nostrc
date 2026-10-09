#include "gh-public-note-post.h"
#include "gh-relay-publish.h"
#include <nostr-event.h>
#include <nostr-gobject-1.0/nostr_relay.h>
#include <nostr-keys.h>
#include <nostr-tag.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

static const gchar author_secret[] =
  "0000000000000000000000000000000000000000000000000000000000000001";
static const gchar account_secret[] =
  "0000000000000000000000000000000000000000000000000000000000000002";

typedef struct { gchar *url; gchar *event_json; } RecordingPublish;

static gpointer
record_open(GhRelayPublish *publish, const gchar *url, const gchar *event_json,
            gpointer data, GError **error)
{
  (void)publish;
  (void)error;
  RecordingPublish *record = data;
  record->url = g_strdup(url);
  record->event_json = g_strdup(event_json);
  return record;
}

static void
record_close(gpointer handle, gpointer data)
{
  g_assert_true(handle == data);
}

static const GhRelayPublishTransport recording_transport = { record_open, record_close };

static gchar *
original_json(gint kind)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, kind);
  nostr_event_set_created_at(event, 1700000000);
  nostr_event_set_content(event, "A verified public note");
  NostrTags *tags = nostr_tags_new(0);
  if (kind == 30000) nostr_tags_append(tags, nostr_tag_new("d", "demo", NULL));
  nostr_event_set_tags(event, tags);
  g_assert_cmpint(nostr_event_sign(event, author_secret), ==, 0);
  char *raw = nostr_event_serialize_compact(event);
  gchar *json = g_strdup(raw);
  free(raw);
  nostr_event_free(event);
  return json;
}

static void
test_repost_quote_snapshot(void)
{
  /* Groundhog and the portable template parser must use one GNostrRelay GType. */
  GType relay_type = gnostr_relay_get_type();
  g_assert_cmpuint(g_type_from_name("GNostrRelay"), ==, relay_type);
  g_autofree gchar *json = original_json(1);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhPublicNote) note = gh_public_note_from_signed_json(json, &error);
  g_assert_no_error(error);
  g_assert_nonnull(note);
  GnNostrReference ref = { .type = GN_NOSTR_REFERENCE_EVENT,
    .id = note->id, .author = note->pubkey, .kind = 1 };
  g_autofree gchar *account = nostr_key_get_public(account_secret);
  const gchar *write[] = { "wss://nos.lol", "wss://relay.nostr.band", NULL };
  g_autoptr(GhPublicPostSnapshot) repost = gh_public_post_snapshot_new(
    note, &ref, FALSE, NULL, account, 1700000001, write, &error);
  g_assert_no_error(error);
  g_assert_nonnull(repost);
  g_assert_cmpuint(g_strv_length(repost->write_relays), ==, 2);
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_unsigned(event, repost->unsigned_json, NULL),
                  ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpint(nostr_event_get_kind(event), ==, 6);
  g_assert_cmpstr(nostr_event_get_content(event), ==, json);
  g_assert_cmpstr(nostr_event_get_pubkey(event), ==, account);
  g_assert_cmpint(nostr_event_get_created_at(event), ==, 1700000001);
  const NostrTags *tags = nostr_event_get_tags(event);
  g_assert_cmpstr(nostr_tag_get(nostr_tags_get(tags, 0), 0), ==, "e");
  g_assert_cmpstr(nostr_tag_get(nostr_tags_get(tags, 0), 1), ==, note->id);
  g_assert_cmpstr(nostr_tag_get(nostr_tags_get(tags, 1), 0), ==, "p");
  g_assert_cmpint(nostr_event_sign(event, account_secret), ==, 0);
  g_assert_cmpint(nostr_event_validate(event, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  char *signed_raw = nostr_event_serialize_compact(event);
  g_assert_true(gh_public_post_snapshot_matches_signed(repost, signed_raw, &error));
  g_assert_no_error(error);
  RecordingPublish record = {0};
  GhRelayPublish *publish = gh_relay_publish_new_with_transport(7, signed_raw,
    &recording_transport, &record, NULL, NULL, NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(publish);
  g_assert_true(gh_relay_publish_add_url(publish, repost->write_relays[0], &error));
  g_assert_no_error(error);
  g_assert_true(gh_relay_publish_start(publish, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(record.url, ==, repost->write_relays[0]);
  g_assert_cmpstr(record.event_json, ==, signed_raw);
  g_assert_true(gh_public_post_snapshot_matches_signed(repost, record.event_json, &error));
  g_assert_no_error(error);
  gh_relay_publish_cancel(publish);
  gh_relay_publish_unref(publish);
  g_free(record.url);
  g_free(record.event_json);
  free(signed_raw);
  nostr_event_free(event);

  g_autoptr(GhPublicPostSnapshot) quote = gh_public_post_snapshot_new(
    note, &ref, TRUE, "Worth reading", account, 1700000002, write, &error);
  g_assert_no_error(error);
  g_assert_nonnull(quote);
  event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_unsigned(event, quote->unsigned_json, NULL),
                  ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpint(nostr_event_get_kind(event), ==, 1);
  g_assert_true(g_str_has_prefix(nostr_event_get_content(event), "Worth reading\n\nnostr:nevent1"));
  tags = nostr_event_get_tags(event);
  g_assert_cmpstr(nostr_tag_get(nostr_tags_get(tags, 0), 0), ==, "q");
  g_assert_cmpstr(nostr_tag_get(nostr_tags_get(tags, 0), 1), ==, note->id);
  nostr_event_free(event);
  g_assert_cmpuint(gnostr_relay_get_type(), ==, relay_type);
  g_assert_cmpuint(g_type_from_name("GNostrRelay"), ==, relay_type);
}

static void
test_address_repost_and_signer_mismatch(void)
{
  g_autofree gchar *json = original_json(30000);
  g_autoptr(GhPublicNote) note = gh_public_note_from_signed_json(json, NULL);
  g_assert_cmpstr(note->dtag, ==, "demo");
  GnNostrReference ref = { .type = GN_NOSTR_REFERENCE_ADDRESS,
    .id = "demo", .author = note->pubkey, .kind = 30000 };
  g_autofree gchar *account = nostr_key_get_public(account_secret);
  const gchar *write[] = { "wss://nos.lol", NULL };
  g_autoptr(GError) error = NULL;
  g_autoptr(GhPublicPostSnapshot) repost = gh_public_post_snapshot_new(
    note, &ref, FALSE, NULL, account, 1700000001, write, &error);
  g_assert_no_error(error);
  g_assert_nonnull(repost);
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_unsigned(event, repost->unsigned_json, NULL),
                  ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpint(nostr_event_get_kind(event), ==, 16);
  const NostrTags *tags = nostr_event_get_tags(event);
  g_assert_cmpstr(nostr_tag_get(nostr_tags_get(tags, 0), 0), ==, "a");
  g_assert_cmpstr(nostr_tag_get(nostr_tags_get(tags, 0), 1), ==,
    "30000:79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798:demo");
  g_autoptr(GhPublicPostSnapshot) quote = gh_public_post_snapshot_new(
    note, &ref, TRUE, "Addressable event", account, 1700000002, write, &error);
  g_assert_no_error(error);
  NostrEvent *quoted = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_unsigned(quoted, quote->unsigned_json, NULL),
                  ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpstr(nostr_tag_get(nostr_tags_get(nostr_event_get_tags(quoted), 0), 0), ==, "q");
  g_assert_cmpstr(nostr_tag_get(nostr_tags_get(nostr_event_get_tags(quoted), 0), 1), ==, note->id);
  g_assert_nonnull(strstr(nostr_event_get_content(quoted), "nostr:nevent1"));
  nostr_event_free(quoted);
  nostr_event_set_content(event, "Signer changed the reviewed content");
  g_assert_cmpint(nostr_event_sign(event, account_secret), ==, 0);
  char *wrong = nostr_event_serialize_compact(event);
  g_assert_false(gh_public_post_snapshot_matches_signed(repost, wrong, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  free(wrong);
  nostr_event_free(event);
  ref.id = "other";
  g_assert_null(gh_public_post_snapshot_new(note, &ref, FALSE, NULL, account,
                                             1700000001, write, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_private_seal_rejected(void)
{
  g_autofree gchar *json = original_json(13);
  g_autoptr(GError) error = NULL;
  g_assert_null(gh_public_note_from_signed_json(json, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
}

static void
test_local_store_resolution(void)
{
  g_autofree gchar *account = nostr_key_get_public(account_secret);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *dir = g_dir_make_tmp("groundhog-public-note-XXXXXX", &error);
  g_assert_no_error(error);
  guint8 raw_key[GH_STORE_KEY_SIZE] = { 0x42 };
  g_autoptr(GBytes) key = g_bytes_new(raw_key, sizeof(raw_key));
  g_autofree gchar *store_id = g_uuid_string_random();
  GhStoreConfig config = { .data_dir = dir, .account_pubkey = account };
  GhStore *store = gh_store_open_with_key(&config, key, store_id,
                                          GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error);
  g_assert_nonnull(store);
  g_autofree gchar *json = original_json(1);
  g_autoptr(GhPublicNote) note = gh_public_note_from_signed_json(json, &error);
  g_assert_no_error(error);
  g_assert_true(gh_store_public_note_put(store, note, &error));
  g_assert_no_error(error);
  g_autoptr(GPtrArray) loaded = gh_store_public_notes_load(store, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(loaded->len, ==, 1);
  GhPublicNote *restored = g_ptr_array_index(loaded, 0);
  g_assert_cmpstr(restored->id, ==, note->id);
  g_assert_cmpstr(restored->event_json, ==, json);
  gh_store_close(store);
  store = gh_store_open_with_key(&config, key, store_id, GH_STORE_OPEN_NONE, &error);
  g_assert_no_error(error);
  g_assert_nonnull(store);
  g_autoptr(GPtrArray) restored_after_reopen = gh_store_public_notes_load(store, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(restored_after_reopen->len, ==, 1);
  restored = g_ptr_array_index(restored_after_reopen, 0);
  g_assert_cmpstr(restored->id, ==, note->id);
  g_assert_true(gh_store_forget(store, NULL, &error));
  g_assert_no_error(error);
  g_autofree gchar *accounts_dir = g_build_filename(dir, "groundhog", "accounts", NULL);
  g_autofree gchar *groundhog_dir = g_build_filename(dir, "groundhog", NULL);
  g_assert_cmpint(g_rmdir(accounts_dir), ==, 0);
  g_assert_cmpint(g_rmdir(groundhog_dir), ==, 0);
  g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void
test_no_write_relays(void)
{
  g_autofree gchar *json = original_json(1);
  g_autoptr(GhPublicNote) note = gh_public_note_from_signed_json(json, NULL);
  GnNostrReference ref = { .type = GN_NOSTR_REFERENCE_EVENT, .id = note->id };
  g_autofree gchar *account = nostr_key_get_public(account_secret);
  g_autoptr(GError) error = NULL;
  g_assert_null(gh_public_post_snapshot_new(note, &ref, FALSE, NULL, account,
                                             1700000001, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/public-post/repost-quote-snapshot", test_repost_quote_snapshot);
  g_test_add_func("/groundhog/public-post/no-write-relays", test_no_write_relays);
  g_test_add_func("/groundhog/public-post/address-repost-signer", test_address_repost_and_signer_mismatch);
  g_test_add_func("/groundhog/public-post/local-store", test_local_store_resolution);
  g_test_add_func("/groundhog/public-post/private-seal-rejected", test_private_seal_rejected);
  return g_test_run();
}
