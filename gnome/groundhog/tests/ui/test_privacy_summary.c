/* The privacy strings table (privacy charter §1.4, §2.2, D7; item G19,
 * gh-privacy-summary.h): one snapshot per backend (NIP-17, NIP-29, MLS)
 * compared with tests/ui/snapshots/privacy-summary-*.txt, the honesty rules
 * every variant keeps (P4: a relay group is never called encrypted, nothing
 * promises delivery, reading or anonymity, every §1.4 non-goal is said), the
 * conversation header subtitles (§2.2 surface 1) and the per-person safety codes. No
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
  /* qp24.13: relays hold ciphertext only; members see who is in the group. */
  g_assert_true(has_line_with(group_mls->visible, "store only encrypted messages"));
  g_assert_true(has_line_with(group_mls->visible, "the member list"));
  g_assert_nonnull(strstr(group_mls->encrypted, "current members only"));

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

/* ---- safety codes ------------------------------------------------------------------------- */

/* Per-person codes (W15 review B2): each from one key, 32 digits in groups
 * of four, the same for any spelling of the key, pinned by a vector computed
 * independently (Python hashlib: SHA-512 over the domain and the raw key,
 * then 5199 times over digest and key; five bytes per group, mod 10000). */
static void
test_safety_codes(void)
{
  g_autofree gchar *alice = hex_of("alice");
  g_autofree gchar *bob = hex_of("bob");
  g_autofree gchar *code_a = gh_privacy_fingerprint(alice);
  g_autofree gchar *code_b = gh_privacy_fingerprint(bob);
  g_assert_cmpstr(code_a, ==, "5357 3888 9924 0357 2766 6605 5645 8258");
  g_assert_cmpstr(code_b, ==, "9503 8612 6753 4210 4890 9238 1900 3763");
  g_assert_cmpuint(strlen(code_a), ==, GH_PRIVACY_FINGERPRINT_DIGITS +
                   GH_PRIVACY_FINGERPRINT_DIGITS / GH_PRIVACY_FINGERPRINT_GROUP - 1);
  g_auto(GStrv) groups = g_strsplit(code_a, " ", -1);
  g_assert_cmpuint(g_strv_length(groups), ==,
                   GH_PRIVACY_FINGERPRINT_DIGITS / GH_PRIVACY_FINGERPRINT_GROUP);
  for (guint i = 0; groups[i]; i++) {
    g_assert_cmpuint(strlen(groups[i]), ==, GH_PRIVACY_FINGERPRINT_GROUP);
    for (const gchar *c = groups[i]; *c; c++)
      g_assert_true(g_ascii_isdigit(*c));
  }
  /* At least ~100 bits shown: 32 decimal digits are 106. */
  g_assert_cmpfloat(GH_PRIVACY_FINGERPRINT_DIGITS * G_LN10 / G_LN2, >=, 100.0);

  /* Upper-case hex names the same key; anything else has no code. */
  g_autofree gchar *upper = g_ascii_strup(alice, -1);
  g_autofree gchar *code_upper = gh_privacy_fingerprint(upper);
  g_assert_cmpstr(code_upper, ==, code_a);
  g_assert_null(gh_privacy_fingerprint("abc"));
  g_assert_null(gh_privacy_fingerprint(NULL));
  g_autofree gchar *bad = g_strdup(alice);
  bad[10] = 'x';
  g_assert_null(gh_privacy_fingerprint(bad));

  /* Read aloud digit by digit, with a pause between groups. */
  g_autofree gchar *spoken = gh_privacy_fingerprint_spoken("0123 4567");
  g_assert_cmpstr(spoken, ==, "0 1 2 3, 4 5 6 7");
}

/* The man in the middle the review describes: Mallory shows Alice K1 as
 * Bob and Bob K2 as Alice, choosing both freely. Alice's device shows
 * "their code" F(K1) and "your code" F(A); Bob's shows F(K2) and F(B). Each
 * comparison pits one code Mallory chooses against one fixed code of a real
 * key, so matching needs a second preimage (2^106), not a collision between
 * two sets she generates (2^30 per side for the old 60-bit combined code):
 * no key of hers matches, and what Alice reads as her own code never depends
 * on the key she holds for Bob. */
static void
test_safety_codes_mitm(void)
{
  g_autofree gchar *alice = hex_of("alice");
  g_autofree gchar *bob = hex_of("bob");
  g_autofree gchar *code_a = gh_privacy_fingerprint(alice);
  g_autofree gchar *code_b = gh_privacy_fingerprint(bob);
  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_hash_table_add(seen, g_strdup(code_a));
  g_hash_table_add(seen, g_strdup(code_b));
  for (guint i = 0; i < 128; i++) {
    g_autofree gchar *seed = g_strdup_printf("mallory-%u", i);
    g_autofree gchar *key = hex_of(seed);
    gchar *code = gh_privacy_fingerprint(key);
    /* F(K1) against F(B) and F(K2) against F(A): never equal, and no two
     * of her keys share a code either. */
    g_assert_cmpstr(code, !=, code_a);
    g_assert_cmpstr(code, !=, code_b);
    g_assert_true(g_hash_table_add(seen, code));
  }
  /* Alice's own code is the same whoever she believes Bob is. */
  g_autofree gchar *again = gh_privacy_fingerprint(alice);
  g_assert_cmpstr(again, ==, code_a);
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
  g_test_add_func("/groundhog/privacy-summary/safety-codes", test_safety_codes);
  g_test_add_func("/groundhog/privacy-summary/safety-codes-mitm", test_safety_codes_mitm);
  g_test_add_func("/groundhog/privacy-summary/format-key", test_format_key);
  return g_test_run();
}
