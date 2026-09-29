/* The privacy strings table (privacy charter §1.4, §2.2, D7; item G19,
 * gh-privacy-summary.h): one snapshot per backend (NIP-17, NIP-29, MLS)
 * compared with tests/ui/snapshots/privacy-summary-*.txt, the honesty rules
 * every variant keeps (P4: a relay group is never called encrypted, nothing
 * promises delivery, reading or anonymity, every §1.4 non-goal is said), the
 * conversation header subtitles (§2.2 surface 1) and the safety code. No
 * display is needed.
 *
 * GROUNDHOG_UPDATE_SNAPSHOTS=1 rewrites the snapshot files instead of
 * comparing (then review and commit the diff). */
#include "gh-privacy-summary.h"

#include <string.h>

static gchar *
hex_of(const gchar *seed)
{
  return g_compute_checksum_for_string(G_CHECKSUM_SHA256, seed, -1);
}

/* ---- snapshots -------------------------------------------------------------------------- */

static void
check_snapshot(const gchar *name, const GhPrivacyContext *context)
{
  g_autoptr(GhPrivacySummary) summary = gh_privacy_summary_new(context);
  g_assert_nonnull(summary);
  g_autofree gchar *text = gh_privacy_summary_to_text(summary);
  g_autofree gchar *file = g_strdup_printf("privacy-summary-%s.txt", name);
  g_autofree gchar *path = g_build_filename(GROUNDHOG_TEST_SNAPSHOTS, file, NULL);
  g_autoptr(GError) error = NULL;
  if (g_strcmp0(g_getenv("GROUNDHOG_UPDATE_SNAPSHOTS"), "1") == 0) {
    g_assert_true(g_file_set_contents(path, text, -1, &error));
    g_assert_no_error(error);
    g_test_message("updated %s", path);
    return;
  }
  g_autofree gchar *expected = NULL;
  if (!g_file_get_contents(path, &expected, NULL, &error))
    g_error("missing snapshot %s (%s); run with GROUNDHOG_UPDATE_SNAPSHOTS=1", path,
            error->message);
  if (g_strcmp0(expected, text) != 0)
    g_printerr("--- %s\n%s\n+++ now\n%s\n", path, expected, text);
  g_assert_cmpstr(text, ==, expected);
}

static void
test_snapshot_nip17(void)
{
  GhPrivacyContext context = {
    .backend = GH_PRIVACY_BACKEND_NIP17, .peer_name = "Alice", .n_people = 1,
  };
  check_snapshot("nip17", &context);
}

static void
test_snapshot_nip29(void)
{
  GhPrivacyContext context = {
    .backend = GH_PRIVACY_BACKEND_NIP29, .relay_host = "groups.example",
  };
  check_snapshot("nip29", &context);
}

static void
test_snapshot_mls(void)
{
  GhPrivacyContext context = { .backend = GH_PRIVACY_BACKEND_MLS, .n_people = 4 };
  check_snapshot("mls", &context);
}

/* ---- honesty rules ------------------------------------------------------------------------ */

static GPtrArray *
all_strings(const GhPrivacySummary *summary)
{
  GPtrArray *strings = g_ptr_array_new();
  g_ptr_array_add(strings, summary->subtitle);
  g_ptr_array_add(strings, summary->heading);
  g_ptr_array_add(strings, summary->encrypted);
  for (gchar **line = summary->visible; *line; line++)
    g_ptr_array_add(strings, *line);
  for (gchar **line = summary->unprotected; *line; line++)
    g_ptr_array_add(strings, *line);
  g_ptr_array_add(strings, summary->storage);
  return strings;
}

static gboolean
contains_ci(const gchar *text, const gchar *needle)
{
  g_autofree gchar *folded = g_utf8_strdown(text, -1);
  g_autofree gchar *folded_needle = g_utf8_strdown(needle, -1);
  return strstr(folded, folded_needle) != NULL;
}

static gboolean
has_line_with(GStrv lines, const gchar *needle)
{
  for (guint i = 0; lines[i]; i++)
    if (contains_ci(lines[i], needle))
      return TRUE;
  return FALSE;
}

/* Claims no backend may make (P4, PD-1, §1.4, §4.1: no Tor or proxy yet). */
static const gchar *const never_claimed[] = {
  "delivered", "read receipt", "has read", "was read", "anonymous", "untraceable",
  "guarantee", "tor", "proxy", "verified", "deleted everywhere", "100%",
};

static void
assert_honest(const GhPrivacyContext *context)
{
  g_autoptr(GhPrivacySummary) summary = gh_privacy_summary_new(context);
  g_assert_nonnull(summary);
  g_assert_nonnull(summary->subtitle);
  g_assert_nonnull(summary->heading);
  g_assert_nonnull(summary->encrypted);
  g_assert_nonnull(summary->storage);
  g_assert_cmpuint(g_strv_length(summary->visible), >=, 3);
  g_assert_cmpuint(g_strv_length(summary->unprotected), >=, 3);
  g_autoptr(GPtrArray) strings = all_strings(summary);
  for (guint i = 0; i < strings->len; i++) {
    const gchar *text = g_ptr_array_index(strings, i);
    g_assert_true(g_utf8_validate(text, -1, NULL));
    for (guint j = 0; j < G_N_ELEMENTS(never_claimed); j++) {
      /* Whole words only for the short ones ("tor" in "story"). */
      g_autofree gchar *folded = g_utf8_strdown(text, -1);
      g_autofree gchar *pattern = g_strdup_printf("\\b%s\\b", never_claimed[j]);
      if (g_regex_match_simple(pattern, folded, 0, 0))
        g_error("%s claims \"%s\": %s", summary->heading, never_claimed[j], text);
    }
    /* Only a conversation that is end-to-end encrypted is called so. */
    if (!summary->end_to_end && contains_ci(text, "end-to-end encrypt"))
      g_assert_true(contains_ci(text, "not end-to-end") || contains_ci(text, "without end-to-end"));
  }
  /* §1.4 non-goals, said for every kind of conversation. */
  g_assert_true(has_line_with(summary->unprotected, "that you use Nostr"));
  g_assert_true(has_line_with(summary->unprotected, "Timing"));
  g_assert_true(has_line_with(summary->unprotected, "Apps running as you"));
  /* Relays see the IP address: Groundhog has no proxy or Tor support yet. */
  g_assert_true(has_line_with(summary->visible, "IP address"));
}

static void
test_honesty(void)
{
  static const GhPrivacyContext contexts[] = {
    { .backend = GH_PRIVACY_BACKEND_NIP17, .peer_name = "Alice", .n_people = 1 },
    { .backend = GH_PRIVACY_BACKEND_NIP17, .n_people = 1 },
    { .backend = GH_PRIVACY_BACKEND_NIP17, .n_people = 3 },
    { .backend = GH_PRIVACY_BACKEND_NIP17, .n_people = 0 },
    { .backend = GH_PRIVACY_BACKEND_NIP29, .relay_host = "groups.example" },
    { .backend = GH_PRIVACY_BACKEND_NIP29 },
    { .backend = GH_PRIVACY_BACKEND_MLS, .n_people = 4 },
    { .backend = GH_PRIVACY_BACKEND_MLS },
  };
  for (guint i = 0; i < G_N_ELEMENTS(contexts); i++)
    assert_honest(&contexts[i]);
}

static void
test_backends(void)
{
  GhPrivacyContext nip17 = { .backend = GH_PRIVACY_BACKEND_NIP17, .peer_name = "Alice",
                             .n_people = 1 };
  g_autoptr(GhPrivacySummary) dm = gh_privacy_summary_new(&nip17);
  g_assert_true(dm->end_to_end);
  g_assert_cmpstr(dm->heading, ==, "End-to-end encrypted");
  g_assert_cmpstr(dm->icon_name, ==, "channel-secure-symbolic");
  /* §2.2: the recipient's relays learn arrivals, never the sender; the
   * other person never learns when you read (PD-1). */
  g_assert_true(has_line_with(dm->visible, "Alice's message relays"));
  g_assert_true(has_line_with(dm->visible, "not who sent them"));
  g_assert_true(has_line_with(dm->visible, "never tells anyone when you read or type"));
  g_assert_true(has_line_with(dm->unprotected, "What Alice does with your messages"));
  g_assert_true(has_line_with(dm->unprotected, "may keep copies"));
  /* D7: an honest single-device copy. */
  g_assert_true(contains_ci(dm->storage, "on this device only"));
  g_assert_true(contains_ci(dm->storage, "downloaded again"));

  GhPrivacyContext unnamed = { .backend = GH_PRIVACY_BACKEND_NIP17, .n_people = 1 };
  g_autoptr(GhPrivacySummary) other = gh_privacy_summary_new(&unnamed);
  g_assert_true(contains_ci(other->encrypted, "the other person"));
  g_assert_true(has_line_with(other->visible, "The other person sees what you write"));
  g_assert_true(has_line_with(other->unprotected, "What the other person does"));

  GhPrivacyContext group = { .backend = GH_PRIVACY_BACKEND_NIP17, .n_people = 3 };
  g_autoptr(GhPrivacySummary) room = gh_privacy_summary_new(&group);
  g_assert_true(contains_ci(room->encrypted, "people in this conversation"));
  g_assert_true(has_line_with(room->visible, "Each person's message relays"));

  GhPrivacyContext self = { .backend = GH_PRIVACY_BACKEND_NIP17, .n_people = 0 };
  g_autoptr(GhPrivacySummary) notes = gh_privacy_summary_new(&self);
  g_assert_true(contains_ci(notes->encrypted, "Only you can read"));
  g_assert_false(has_line_with(notes->visible, "sees what you write"));
  g_assert_false(has_line_with(notes->unprotected, "does with your messages"));

  GhPrivacyContext relay = { .backend = GH_PRIVACY_BACKEND_NIP29, .relay_host = "groups.example" };
  g_autoptr(GhPrivacySummary) nip29 = gh_privacy_summary_new(&relay);
  g_assert_false(nip29->end_to_end);
  g_assert_cmpstr(nip29->heading, ==, "Not end-to-end encrypted");
  g_assert_cmpstr(nip29->icon_name, ==, "dialog-warning-symbolic");
  /* §2.2 example copy. */
  g_assert_true(has_line_with(nip29->visible, "The operators of groups.example can read every "
                                              "message and see the member list"));
  GhPrivacyContext hostless = { .backend = GH_PRIVACY_BACKEND_NIP29 };
  g_autoptr(GhPrivacySummary) nip29_unknown = gh_privacy_summary_new(&hostless);
  g_assert_true(has_line_with(nip29_unknown->visible, "the group's relay"));

  GhPrivacyContext mls = { .backend = GH_PRIVACY_BACKEND_MLS, .n_people = 4 };
  g_autoptr(GhPrivacySummary) group_mls = gh_privacy_summary_new(&mls);
  g_assert_true(group_mls->end_to_end);
  /* §1.4: one device per MLS identity; D7: no history recovery. */
  g_assert_true(has_line_with(group_mls->unprotected, "this device only"));
  g_assert_true(has_line_with(group_mls->unprotected, "can't be downloaded again"));
  g_assert_true(has_line_with(group_mls->visible, "don't see who the members are"));

  GhPrivacyContext unknown = { .backend = 7 };
  g_assert_null(gh_privacy_summary_new(&unknown));
  g_assert_null(gh_privacy_summary_dup_subtitle(&unknown));
}

/* The storage line says what Groundhog itself keeps: never "encrypted on
 * this device" when it keeps nothing there. */
static void
test_storage(void)
{
  static const GhPrivacyBackend backends[] = {
    GH_PRIVACY_BACKEND_NIP17, GH_PRIVACY_BACKEND_NIP29, GH_PRIVACY_BACKEND_MLS,
  };
  for (guint i = 0; i < G_N_ELEMENTS(backends); i++) {
    GhPrivacyContext saved = { .backend = backends[i], .n_people = 1 };
    g_autoptr(GhPrivacySummary) on_disk = gh_privacy_summary_new(&saved);
    g_assert_true(contains_ci(on_disk->storage, "encrypted on this device"));

    GhPrivacyContext memory = saved;
    memory.storage = GH_PRIVACY_STORAGE_MEMORY;
    assert_honest(&memory);
    g_autoptr(GhPrivacySummary) in_memory = gh_privacy_summary_new(&memory);
    g_assert_false(contains_ci(in_memory->storage, "encrypted on this device"));
    g_assert_true(contains_ci(in_memory->storage, "in memory only"));

    GhPrivacyContext none = saved;
    none.storage = GH_PRIVACY_STORAGE_NONE;
    assert_honest(&none);
    g_autoptr(GhPrivacySummary) unsaved = gh_privacy_summary_new(&none);
    g_assert_false(contains_ci(unsaved->storage, "encrypted on this device"));
    g_assert_true(contains_ci(unsaved->storage, "isn't saving messages"));

    /* What holds elsewhere is still said. */
    if (backends[i] == GH_PRIVACY_BACKEND_NIP17)
      g_assert_true(contains_ci(unsaved->storage, "downloaded again from your relays"));
    else if (backends[i] == GH_PRIVACY_BACKEND_NIP29)
      g_assert_true(contains_ci(unsaved->storage, "operators can read"));
    else
      g_assert_true(contains_ci(unsaved->storage, "can't be downloaded again"));
  }
}

/* §2.2 surface 1, and the request variants G12's header used before. */
static void
test_subtitles(void)
{
  struct {
    GhPrivacyContext context;
    const gchar *subtitle;
  } cases[] = {
    { { .backend = GH_PRIVACY_BACKEND_NIP17, .n_people = 1 }, "Private · end-to-end encrypted" },
    { { .backend = GH_PRIVACY_BACKEND_NIP17, .is_request = TRUE },
      "Message request · end-to-end encrypted" },
    { { .backend = GH_PRIVACY_BACKEND_NIP17, .is_request = TRUE, .subject = "Hi" },
      "“Hi” · Message request · end-to-end encrypted" },
    { { .backend = GH_PRIVACY_BACKEND_NIP17, .is_request = TRUE, .subject = "" },
      "Message request · end-to-end encrypted" },
    /* Only a request shows its subject in the subtitle. */
    { { .backend = GH_PRIVACY_BACKEND_NIP17, .subject = "Hi" }, "Private · end-to-end encrypted" },
    { { .backend = GH_PRIVACY_BACKEND_NIP29 }, "Relay group · not end-to-end encrypted" },
    { { .backend = GH_PRIVACY_BACKEND_MLS }, "Encrypted group" },
    { { .backend = GH_PRIVACY_BACKEND_MLS, .n_people = 1 }, "Encrypted group · 1 member" },
    { { .backend = GH_PRIVACY_BACKEND_MLS, .n_people = 4 }, "Encrypted group · 4 members" },
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_autofree gchar *subtitle = gh_privacy_summary_dup_subtitle(&cases[i].context);
    g_assert_cmpstr(subtitle, ==, cases[i].subtitle);
    g_autoptr(GhPrivacySummary) summary = gh_privacy_summary_new(&cases[i].context);
    g_assert_cmpstr(summary->subtitle, ==, cases[i].subtitle);
  }
}

/* ---- safety code -------------------------------------------------------------------------- */

static void
test_safety_code(void)
{
  g_autofree gchar *alice = hex_of("alice");
  g_autofree gchar *bob = hex_of("bob");
  g_autofree gchar *carol = hex_of("carol");
  guint8 ab[GH_PRIVACY_SAFETY_CODE_LENGTH], ba[GH_PRIVACY_SAFETY_CODE_LENGTH];
  guint8 ac[GH_PRIVACY_SAFETY_CODE_LENGTH];
  g_assert_true(gh_privacy_safety_code(alice, bob, ab));
  g_assert_true(gh_privacy_safety_code(bob, alice, ba));
  g_assert_true(gh_privacy_safety_code(alice, carol, ac));
  /* Both sides compute the same code; another pair gets another one. */
  g_assert_cmpmem(ab, sizeof ab, ba, sizeof ba);
  g_assert_true(memcmp(ab, ac, sizeof ab) != 0);
  /* A vector computed independently (Python: SHA-256 of the domain string,
   * the lower key, the higher key; the first 60 bits, 6 at a time). */
  static const guint8 expected[GH_PRIVACY_SAFETY_CODE_LENGTH] = {
    40, 40, 11, 10, 57, 62, 55, 6, 61, 62,
  };
  g_assert_cmpmem(ab, sizeof ab, expected, sizeof expected);
  g_autofree gchar *text = gh_privacy_safety_code_to_text(ab);
  g_assert_cmpstr(text, ==, "Gift Gift Turtle Penguin Guitar Folder Trophy Elephant Headphones "
                            "Folder");
  g_autofree gchar *account = hex_of("account-a");
  g_autofree gchar *peer = hex_of("peer-0");
  g_assert_true(gh_privacy_safety_code(peer, account, ab));
  g_autofree gchar *text2 = gh_privacy_safety_code_to_text(ab);
  g_assert_cmpstr(text2, ==, "Gift Cake Flag Gift Pin Key Glasses Unicorn Trophy Robot");

  /* Upper-case hex names the same key. */
  g_autofree gchar *upper = g_ascii_strup(alice, -1);
  g_assert_true(gh_privacy_safety_code(upper, bob, ba));
  g_assert_true(gh_privacy_safety_code(alice, bob, ab));
  g_assert_cmpmem(ab, sizeof ab, ba, sizeof ba);

  /* Not a key, or the same key twice: no code. */
  g_assert_false(gh_privacy_safety_code(alice, alice, ab));
  g_assert_false(gh_privacy_safety_code("abc", bob, ab));
  g_autofree gchar *bad = g_strdup(alice);
  bad[10] = 'x';
  g_assert_false(gh_privacy_safety_code(bad, bob, ab));
  g_assert_false(gh_privacy_safety_code(NULL, bob, ab));

  /* 64 distinct symbols, each with a name. */
  g_autoptr(GHashTable) emoji = g_hash_table_new(g_str_hash, g_str_equal);
  g_autoptr(GHashTable) names = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint i = 0; i < GH_PRIVACY_SAFETY_SYMBOLS; i++) {
    const gchar *e = gh_privacy_safety_symbol_emoji(i);
    const gchar *n = gh_privacy_safety_symbol_name(i);
    g_assert_true(e && *e && g_utf8_validate(e, -1, NULL));
    g_assert_true(n && *n);
    g_assert_true(g_hash_table_add(emoji, (gpointer)e));
    g_assert_true(g_hash_table_add(names, (gpointer)n));
  }
}

static void
test_format_key(void)
{
  const gchar *npub = "npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d";
  g_autofree gchar *formatted = gh_privacy_format_key(npub);
  g_assert_cmpstr(formatted, ==, "npub1 0xlx vlhe mja6 c4dq v22u apct qupf hlxm 9h8z 3k2e 72q4 "
                                 "k9hc z7vq pkge 6d");
  /* Nothing is lost or reordered. */
  g_auto(GStrv) groups = g_strsplit(formatted, " ", -1);
  g_autofree gchar *joined = g_strjoinv("", groups);
  g_assert_cmpstr(joined, ==, npub);
  g_autofree gchar *plain = gh_privacy_format_key("abcdefghij");
  g_assert_cmpstr(plain, ==, "abcd efgh ij");
  g_autofree gchar *empty = gh_privacy_format_key("");
  g_assert_cmpstr(empty, ==, "");
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/privacy-summary/snapshot/nip17", test_snapshot_nip17);
  g_test_add_func("/groundhog/privacy-summary/snapshot/nip29", test_snapshot_nip29);
  g_test_add_func("/groundhog/privacy-summary/snapshot/mls", test_snapshot_mls);
  g_test_add_func("/groundhog/privacy-summary/honesty", test_honesty);
  g_test_add_func("/groundhog/privacy-summary/backends", test_backends);
  g_test_add_func("/groundhog/privacy-summary/storage", test_storage);
  g_test_add_func("/groundhog/privacy-summary/subtitles", test_subtitles);
  g_test_add_func("/groundhog/privacy-summary/safety-code", test_safety_code);
  g_test_add_func("/groundhog/privacy-summary/format-key", test_format_key);
  return g_test_run();
}
