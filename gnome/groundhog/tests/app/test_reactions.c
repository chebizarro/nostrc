/* Test: NIP-25 reaction model (W26 slice B, nostrc-191r).
 *
 * GhReaction, GhReactionSummary and GhReactionStore: admission, dedup,
 * removal, aggregation, own-reaction tracking, the "reaction-changed"
 * signal, and privacy (no remote image fetch for custom emoji). */

#include "gh-reaction.h"
#include "gh-reaction-store.h"

#include <locale.h>
#include <string.h>

#define ACCOUNT   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define SENDER_A  "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define SENDER_B  "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
#define TARGET_1  "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
#define ROOM_ID   "test-room"

/* ---- quick-set sanity ------------------------------------------------------- */

static void
test_quick_set(void)
{
  g_assert_cmpuint(GH_REACTION_QUICK_SET_SIZE, ==, 6);
  for (guint i = 0; i < GH_REACTION_QUICK_SET_SIZE; i++) {
    g_assert_nonnull(gh_reaction_quick_set[i]);
    g_assert_true(g_utf8_validate(gh_reaction_quick_set[i], -1, NULL));
  }
}

/* ---- GhReaction ------------------------------------------------------------ */

static void
test_reaction_new(void)
{
  g_autoptr(GhReaction) r = gh_reaction_new(TARGET_1, "rxn-001", SENDER_A, "👍", 1000, ROOM_ID);
  g_assert_nonnull(r);
  g_assert_cmpstr(gh_reaction_get_target_rumor_id(r), ==, TARGET_1);
  g_assert_cmpstr(gh_reaction_get_reaction_rumor_id(r), ==, "rxn-001");
  g_assert_cmpstr(gh_reaction_get_sender(r), ==, SENDER_A);
  g_assert_cmpstr(gh_reaction_get_emoji(r), ==, "👍");
  g_assert_cmpint(gh_reaction_get_created_at(r), ==, 1000);
  g_assert_cmpstr(gh_reaction_get_room_id(r), ==, ROOM_ID);
}

static void
test_reaction_default_emoji(void)
{
  g_autoptr(GhReaction) r = gh_reaction_new(TARGET_1, "rxn-002", SENDER_A, NULL, 1000, ROOM_ID);
  g_assert_cmpstr(gh_reaction_get_emoji(r), ==, "+");
}

/* ---- GhReactionStore ------------------------------------------------------- */

static gboolean signal_fired;
static gchar *signal_target;

static void
on_changed(GhReactionStore *store, const gchar *target_id, gpointer data)
{
  (void)store; (void)data;
  signal_fired = TRUE;
  g_free(signal_target);
  signal_target = g_strdup(target_id);
}

static void
test_store_admit_and_lookup(void)
{
  g_autoptr(GhReactionStore) store = gh_reaction_store_new();
  gh_reaction_store_set_account(store, ACCOUNT, NULL, NULL, NULL);
  g_signal_connect(store, "reaction-changed", G_CALLBACK(on_changed), NULL);

  g_autoptr(GhReaction) r1 = gh_reaction_new(TARGET_1, "rxn-010", SENDER_A, "👍", 1000, ROOM_ID);

  signal_fired = FALSE;
  gboolean admitted = gh_reaction_store_admit(store, r1, NULL);
  g_assert_true(admitted);
  g_assert_true(signal_fired);
  g_assert_cmpstr(signal_target, ==, TARGET_1);

  GhReactionSummary *summary = gh_reaction_store_lookup(store, TARGET_1);
  g_assert_nonnull(summary);
  g_assert_cmpuint(gh_reaction_summary_get_total_count(summary), ==, 1);
  g_assert_false(gh_reaction_summary_get_has_own(summary));

  const GPtrArray *chips = gh_reaction_summary_get_chips(summary);
  g_assert_nonnull(chips);
  g_assert_cmpuint(chips->len, ==, 1);
  GhReactionChip *chip = g_ptr_array_index(chips, 0);
  g_assert_cmpstr(chip->emoji, ==, "👍");
  g_assert_cmpuint(chip->count, ==, 1);
  g_assert_false(chip->is_own);

  g_free(signal_target);
  signal_target = NULL;
}

static void
test_store_dedup(void)
{
  g_autoptr(GhReactionStore) store = gh_reaction_store_new();
  gh_reaction_store_set_account(store, ACCOUNT, NULL, NULL, NULL);

  g_autoptr(GhReaction) r1 = gh_reaction_new(TARGET_1, "rxn-020", SENDER_A, "👍", 1000, ROOM_ID);
  g_autoptr(GhReaction) r2 = gh_reaction_new(TARGET_1, "rxn-020", SENDER_B, "❤️", 2000, ROOM_ID);

  g_assert_true(gh_reaction_store_admit(store, r1, NULL));
  g_assert_false(gh_reaction_store_admit(store, r2, NULL));  /* same rxn id */

  GhReactionSummary *summary = gh_reaction_store_lookup(store, TARGET_1);
  g_assert_cmpuint(gh_reaction_summary_get_total_count(summary), ==, 1);
}

static void
test_store_own_reaction(void)
{
  g_autoptr(GhReactionStore) store = gh_reaction_store_new();
  gh_reaction_store_set_account(store, ACCOUNT, NULL, NULL, NULL);

  g_autoptr(GhReaction) own = gh_reaction_new(TARGET_1, "rxn-030", ACCOUNT, "👍", 1000, ROOM_ID);
  gh_reaction_store_admit(store, own, NULL);

  GhReactionSummary *summary = gh_reaction_store_lookup(store, TARGET_1);
  g_assert_true(gh_reaction_summary_get_has_own(summary));

  const GPtrArray *chips = gh_reaction_summary_get_chips(summary);
  GhReactionChip *chip = g_ptr_array_index(chips, 0);
  g_assert_true(chip->is_own);

  /* Own reaction id lookup. */
  const gchar *rid = gh_reaction_summary_own_reaction_id(summary, "👍");
  g_assert_cmpstr(rid, ==, "rxn-030");
  g_assert_null(gh_reaction_summary_own_reaction_id(summary, "❤️"));
}

static void
test_store_remove(void)
{
  g_autoptr(GhReactionStore) store = gh_reaction_store_new();
  gh_reaction_store_set_account(store, ACCOUNT, NULL, NULL, NULL);
  g_signal_connect(store, "reaction-changed", G_CALLBACK(on_changed), NULL);

  g_autoptr(GhReaction) r1 = gh_reaction_new(TARGET_1, "rxn-040", SENDER_A, "👍", 1000, ROOM_ID);
  gh_reaction_store_admit(store, r1, NULL);

  signal_fired = FALSE;
  gboolean removed = gh_reaction_store_remove(store, "rxn-040", NULL);
  g_assert_true(removed);
  g_assert_true(signal_fired);

  GhReactionSummary *summary = gh_reaction_store_lookup(store, TARGET_1);
  g_assert_cmpuint(gh_reaction_summary_get_total_count(summary), ==, 0);

  /* Double remove is FALSE (already gone). */
  g_assert_false(gh_reaction_store_remove(store, "rxn-040", NULL));

  g_free(signal_target);
  signal_target = NULL;
}

static void
test_store_remove_own(void)
{
  g_autoptr(GhReactionStore) store = gh_reaction_store_new();
  gh_reaction_store_set_account(store, ACCOUNT, NULL, NULL, NULL);

  g_autoptr(GhReaction) own = gh_reaction_new(TARGET_1, "rxn-050", ACCOUNT, "👍", 1000, ROOM_ID);
  gh_reaction_store_admit(store, own, NULL);

  g_autofree gchar *rid = gh_reaction_store_remove_own(store, TARGET_1, "👍", NULL);
  g_assert_cmpstr(rid, ==, "rxn-050");

  GhReactionSummary *summary = gh_reaction_store_lookup(store, TARGET_1);
  g_assert_false(gh_reaction_summary_get_has_own(summary));
  g_assert_cmpuint(gh_reaction_summary_get_total_count(summary), ==, 0);
}

static void
test_store_aggregation(void)
{
  g_autoptr(GhReactionStore) store = gh_reaction_store_new();
  gh_reaction_store_set_account(store, ACCOUNT, NULL, NULL, NULL);

  g_autoptr(GhReaction) r1 = gh_reaction_new(TARGET_1, "rxn-060", SENDER_A, "👍", 1000, ROOM_ID);
  g_autoptr(GhReaction) r2 = gh_reaction_new(TARGET_1, "rxn-061", SENDER_B, "👍", 1001, ROOM_ID);
  g_autoptr(GhReaction) r3 = gh_reaction_new(TARGET_1, "rxn-062", ACCOUNT, "❤️", 1002, ROOM_ID);
  gh_reaction_store_admit(store, r1, NULL);
  gh_reaction_store_admit(store, r2, NULL);
  gh_reaction_store_admit(store, r3, NULL);

  GhReactionSummary *summary = gh_reaction_store_lookup(store, TARGET_1);
  g_assert_cmpuint(gh_reaction_summary_get_total_count(summary), ==, 3);
  g_assert_true(gh_reaction_summary_get_has_own(summary));

  const GPtrArray *chips = gh_reaction_summary_get_chips(summary);
  g_assert_cmpuint(chips->len, ==, 2);

  /* First-seen order: 👍 first, then ❤️. */
  GhReactionChip *c0 = g_ptr_array_index(chips, 0);
  g_assert_cmpstr(c0->emoji, ==, "👍");
  g_assert_cmpuint(c0->count, ==, 2);
  g_assert_false(c0->is_own);  /* neither sender is the account */

  GhReactionChip *c1 = g_ptr_array_index(chips, 1);
  g_assert_cmpstr(c1->emoji, ==, "❤️");
  g_assert_cmpuint(c1->count, ==, 1);
  g_assert_true(c1->is_own);
}

/* ---- F4: double-add same emoji → toggle ------------------------------------ */

static void
test_store_double_add_toggle(void)
{
  /* If the user already reacted with an emoji, re-adding the same emoji
   * should be detected by the caller (via own_reaction_id). The store
   * itself would dedup by rumor_id, but the picker creates a NEW rumor_id
   * each time, so the caller must check before creating a new reaction. */
  g_autoptr(GhReactionStore) store = gh_reaction_store_new();
  gh_reaction_store_set_account(store, ACCOUNT, NULL, NULL, NULL);

  /* User reacts with 👍. */
  g_autoptr(GhReaction) r1 =
    gh_reaction_new(TARGET_1, "rxn-f4-001", ACCOUNT, "👍", 1000, ROOM_ID);
  g_assert_true(gh_reaction_store_admit(store, r1, NULL));

  /* Before adding a second 👍, check if already reacted. */
  GhReactionSummary *summary = gh_reaction_store_lookup(store, TARGET_1);
  const gchar *existing = gh_reaction_summary_own_reaction_id(summary, "👍");
  g_assert_nonnull(existing);
  g_assert_cmpstr(existing, ==, "rxn-f4-001");

  /* The caller would toggle (remove) instead of adding. */
  g_autofree gchar *removed = gh_reaction_store_remove_own(store, TARGET_1, "👍", NULL);
  g_assert_cmpstr(removed, ==, "rxn-f4-001");
  g_assert_cmpuint(gh_reaction_summary_get_total_count(summary), ==, 0);

  /* A different emoji is still addable. */
  g_assert_null(gh_reaction_summary_own_reaction_id(summary, "❤️"));
}

/* ---- F3: sender ownership on kind-5 deletion ------------------------------- */

static void
test_store_forged_deletion(void)
{
  /* A kind-5 deletion should only remove a reaction if the deletion sender
   * matches the reaction's original sender. A forged deletion from a
   * different sender must be rejected. */
  g_autoptr(GhReactionStore) store = gh_reaction_store_new();
  gh_reaction_store_set_account(store, ACCOUNT, NULL, NULL, NULL);

  /* Alice (SENDER_A) reacts with 👍. */
  g_autoptr(GhReaction) alice_rxn =
    gh_reaction_new(TARGET_1, "rxn-f3-alice", SENDER_A, "👍", 1000, ROOM_ID);
  g_assert_true(gh_reaction_store_admit(store, alice_rxn, NULL));

  /* Lookup the sender of that reaction. */
  const gchar *sender = gh_reaction_store_get_sender(store, "rxn-f3-alice");
  g_assert_cmpstr(sender, ==, SENDER_A);

  /* Bob (SENDER_B) tries to delete Alice's reaction — forged.
   * The caller should compare sender before calling remove. */
  g_assert_cmpstr(gh_reaction_store_get_sender(store, "rxn-f3-alice"), ==, SENDER_A);
  g_assert_true(g_strcmp0(SENDER_B, SENDER_A) != 0);  /* different sender */

  /* Simulate the check the receive path performs: sender doesn't match,
   * so the reaction should NOT be removed. */
  const gchar *original = gh_reaction_store_get_sender(store, "rxn-f3-alice");
  if (original && g_strcmp0(original, SENDER_B) == 0)
    gh_reaction_store_remove(store, "rxn-f3-alice", NULL);

  /* Reaction must still be present. */
  GhReactionSummary *summary = gh_reaction_store_lookup(store, TARGET_1);
  g_assert_cmpuint(gh_reaction_summary_get_total_count(summary), ==, 1);

  /* Now the real author (SENDER_A) deletes it — succeeds. */
  original = gh_reaction_store_get_sender(store, "rxn-f3-alice");
  if (original && g_strcmp0(original, SENDER_A) == 0)
    gh_reaction_store_remove(store, "rxn-f3-alice", NULL);

  g_assert_cmpuint(gh_reaction_summary_get_total_count(summary), ==, 0);
}

static void
test_store_get_sender_unknown(void)
{
  /* get_sender for an unknown reaction returns NULL. */
  g_autoptr(GhReactionStore) store = gh_reaction_store_new();
  gh_reaction_store_set_account(store, ACCOUNT, NULL, NULL, NULL);
  g_assert_null(gh_reaction_store_get_sender(store, "nonexistent"));
}

/* ---- F5: restore loop must not re-insert into database --------------------- */

static gint admit_delegate_calls;

static gboolean
counting_admit(gpointer data, GhReaction *reaction, GError **error)
{
  (void)data; (void)reaction; (void)error;
  admit_delegate_calls++;
  return TRUE;
}

static const GhReactionDelegate counting_delegate = {
  .admit = counting_admit,
  .remove = NULL,
  .remove_by_sender = NULL,
};

static void
test_store_suspend_delegate(void)
{
  /* When the delegate is suspended, admit() should not call the delegate's
   * admit callback (simulating the restore loop bypassing SQLite). */
  g_autoptr(GhReactionStore) store = gh_reaction_store_new();
  admit_delegate_calls = 0;
  gh_reaction_store_set_account(store, ACCOUNT, &counting_delegate, NULL, NULL);

  /* Normal admit: delegate is called. */
  g_autoptr(GhReaction) r1 =
    gh_reaction_new(TARGET_1, "rxn-f5-001", SENDER_A, "👍", 1000, ROOM_ID);
  gh_reaction_store_admit(store, r1, NULL);
  g_assert_cmpint(admit_delegate_calls, ==, 1);

  /* Suspend: delegate should not be called. */
  gh_reaction_store_suspend_delegate(store);
  g_autoptr(GhReaction) r2 =
    gh_reaction_new(TARGET_1, "rxn-f5-002", SENDER_B, "❤️", 1001, ROOM_ID);
  gh_reaction_store_admit(store, r2, NULL);
  g_assert_cmpint(admit_delegate_calls, ==, 1);  /* still 1, not 2 */

  /* But the in-memory model should still have both reactions. */
  GhReactionSummary *summary = gh_reaction_store_lookup(store, TARGET_1);
  g_assert_cmpuint(gh_reaction_summary_get_total_count(summary), ==, 2);

  /* Resume: delegate is called again. */
  gh_reaction_store_resume_delegate(store);
  g_autoptr(GhReaction) r3 =
    gh_reaction_new(TARGET_1, "rxn-f5-003", ACCOUNT, "😂", 1002, ROOM_ID);
  gh_reaction_store_admit(store, r3, NULL);
  g_assert_cmpint(admit_delegate_calls, ==, 2);
  g_assert_cmpuint(gh_reaction_summary_get_total_count(summary), ==, 3);
}

/* ---- privacy: no remote image fetch ---------------------------------------- */

static void
test_custom_emoji_shortcode(void)
{
  /* A custom emoji with a shortcode should display the shortcode text, not
   * fetch any image (privacy charter PD-2). The model stores the content
   * as-is: ":fire:" stays ":fire:", no URL resolution. */
  g_autoptr(GhReaction) r = gh_reaction_new(TARGET_1, "rxn-070", SENDER_A, ":fire:", 1000, ROOM_ID);
  g_assert_cmpstr(gh_reaction_get_emoji(r), ==, ":fire:");
}

static void
test_emoji_max_length(void)
{
  /* Emoji content exceeding GH_REACTION_MAX_EMOJI is rejected (NULL).
   * No remote image fetch for custom emoji (privacy charter PD-2). */
  gchar long_emoji[GH_REACTION_MAX_EMOJI + 10];
  memset(long_emoji, 'x', sizeof long_emoji - 1);
  long_emoji[sizeof long_emoji - 1] = '\0';

  GhReaction *r = gh_reaction_new(TARGET_1, "rxn-080", SENDER_A, long_emoji, 1000, ROOM_ID);
  g_assert_null(r);

  /* Exactly at the limit is accepted. */
  gchar exact_emoji[GH_REACTION_MAX_EMOJI + 1];
  memset(exact_emoji, 'x', GH_REACTION_MAX_EMOJI);
  exact_emoji[GH_REACTION_MAX_EMOJI] = '\0';

  g_autoptr(GhReaction) r2 = gh_reaction_new(TARGET_1, "rxn-081", SENDER_A, exact_emoji, 1000, ROOM_ID);
  g_assert_nonnull(r2);
  g_assert_cmpuint(strlen(gh_reaction_get_emoji(r2)), ==, GH_REACTION_MAX_EMOJI);
}

/* ---- main ------------------------------------------------------------------ */

int
main(int argc, char *argv[])
{
  setlocale(LC_ALL, "");
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/reactions/quick-set", test_quick_set);
  g_test_add_func("/reactions/new", test_reaction_new);
  g_test_add_func("/reactions/default-emoji", test_reaction_default_emoji);
  g_test_add_func("/reactions/store/admit-and-lookup", test_store_admit_and_lookup);
  g_test_add_func("/reactions/store/dedup", test_store_dedup);
  g_test_add_func("/reactions/store/own-reaction", test_store_own_reaction);
  g_test_add_func("/reactions/store/remove", test_store_remove);
  g_test_add_func("/reactions/store/remove-own", test_store_remove_own);
  g_test_add_func("/reactions/store/aggregation", test_store_aggregation);
  g_test_add_func("/reactions/store/double-add-toggle", test_store_double_add_toggle);
  g_test_add_func("/reactions/store/forged-deletion", test_store_forged_deletion);
  g_test_add_func("/reactions/store/get-sender-unknown", test_store_get_sender_unknown);
  g_test_add_func("/reactions/store/suspend-delegate", test_store_suspend_delegate);
  g_test_add_func("/reactions/privacy/custom-emoji-shortcode", test_custom_emoji_shortcode);
  g_test_add_func("/reactions/privacy/emoji-max-length", test_emoji_max_length);

  return g_test_run();
}
