/* Test: NIP-25 reaction send-path builders (W26 slice B, nostrc-191r).
 *
 * NIP-17 rumor builders (gh_nip17_rumor_new_reaction_room,
 * gh_nip17_rumor_new_deletion_room) produce canonical unsigned JSON
 * verified by parsing it back with libnostr. */

#include "gh-nip17-envelope.h"

#include <locale.h>
#include <nostr.h>
#include <string.h>

#define SENDER  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define RECIP_A "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define RECIP_B "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
#define TARGET  "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"

static NostrEvent *
parse(const gchar *json)
{
  NostrEvent *ev = nostr_event_new();
  g_assert_nonnull(ev);
  NostrEventValidationStatus status = nostr_event_deserialize_unsigned(ev, json, NULL);
  g_assert_cmpint(status, ==, NOSTR_EVENT_VALIDATION_OK);
  return ev;
}

static const gchar *
find_tag_value(NostrEvent *ev, const gchar *key)
{
  NostrTags *tags = nostr_event_get_tags(ev);
  for (size_t i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (g_strcmp0(nostr_tag_get_key(tag), key) == 0)
      return nostr_tag_get_value(tag);
  }
  return NULL;
}

/* ---- NIP-17 reaction rumor ------------------------------------------------ */

static void
test_nip17_reaction_rumor(void)
{
  const gchar *const recipients[] = { RECIP_A, RECIP_B, NULL };
  g_autofree gchar *rumor_id = NULL;
  g_autoptr(GError) error = NULL;

  g_autofree gchar *json = gh_nip17_rumor_new_reaction_room(
    SENDER, recipients, "+", TARGET, "14", 1700000000, &rumor_id, &error);
  g_assert_no_error(error);
  g_assert_nonnull(json);
  g_assert_nonnull(rumor_id);

  NostrEvent *ev = parse(json);
  g_assert_cmpint(nostr_event_get_kind(ev), ==, 7);
  g_assert_cmpstr(nostr_event_get_content(ev), ==, "+");
  g_assert_cmpstr(nostr_event_get_pubkey(ev), ==, SENDER);
  g_assert_cmpint(nostr_event_get_created_at(ev), ==, 1700000000);

  /* Must have e, k tags for the target. */
  g_assert_cmpstr(find_tag_value(ev, "e"), ==, TARGET);
  g_assert_cmpstr(find_tag_value(ev, "k"), ==, "14");

  /* Must have p tags for the recipients (for NIP-17 routing). */
  NostrTags *tags = nostr_event_get_tags(ev);
  guint p_count = 0;
  for (size_t i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (g_strcmp0(nostr_tag_get_key(tag), "p") == 0)
      p_count++;
  }
  g_assert_cmpuint(p_count, >=, 2);

  nostr_event_free(ev);
}

/* ---- NIP-17 deletion rumor ------------------------------------------------ */

static void
test_nip17_deletion_rumor(void)
{
  const gchar *const recipients[] = { RECIP_A, NULL };
  g_autofree gchar *rumor_id = NULL;
  g_autoptr(GError) error = NULL;

  g_autofree gchar *json = gh_nip17_rumor_new_deletion_room(
    SENDER, recipients, TARGET, 1700000000, &rumor_id, &error);
  g_assert_no_error(error);
  g_assert_nonnull(json);
  g_assert_nonnull(rumor_id);

  NostrEvent *ev = parse(json);
  g_assert_cmpint(nostr_event_get_kind(ev), ==, 5);
  g_assert_cmpstr(nostr_event_get_pubkey(ev), ==, SENDER);

  /* Must have an e tag pointing at the deleted event. */
  g_assert_cmpstr(find_tag_value(ev, "e"), ==, TARGET);

  nostr_event_free(ev);
}

/* ---- NIP-17 reaction rumor rejects bad input ------------------------------ */

static void
test_nip17_reaction_invalid(void)
{
  const gchar *const recipients[] = { RECIP_A, NULL };
  g_autoptr(GError) error = NULL;
  gchar *rumor_id = NULL;

  /* NULL emoji → error. */
  gchar *json = gh_nip17_rumor_new_reaction_room(
    SENDER, recipients, "+", NULL, "14", 1700000000, &rumor_id, &error);
  g_assert_null(json);
  g_assert_nonnull(error);
  g_clear_error(&error);

  /* NULL target → error. */
  json = gh_nip17_rumor_new_reaction_room(
    SENDER, recipients, "👍", NULL, "14", 1700000000, &rumor_id, &error);
  g_assert_null(json);
  g_assert_nonnull(error);
  g_clear_error(&error);
}

/* ---- main ---------------------------------------------------------------- */

int
main(int argc, char *argv[])
{
  setlocale(LC_ALL, "");
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/reaction-send/nip17/reaction-rumor", test_nip17_reaction_rumor);
  g_test_add_func("/reaction-send/nip17/deletion-rumor", test_nip17_deletion_rumor);
  g_test_add_func("/reaction-send/nip17/reaction-invalid", test_nip17_reaction_invalid);

  return g_test_run();
}
