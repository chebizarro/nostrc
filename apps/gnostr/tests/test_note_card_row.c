/**
 * Note Card Row Binding Logic Unit Tests
 *
 * Tests for the binding_id lifecycle logic that determines whether
 * setters like set_content actually modify the widget.
 *
 * These tests verify the logic that was causing blank cards in the
 * repo browser when prepare_for_bind wasn't being called.
 */

#include <glib.h>
#include <stdint.h>

/* Minimal mock of the binding logic for testing without GTK dependencies.
 * This tests the same logic that's in note_card_row.c */

typedef struct {
  uint64_t binding_id;
  int disposed;
  char *content_text;
} MockNoteCard;

static uint64_t mock_binding_id_counter = 1;

static MockNoteCard *mock_card_new(void) {
  MockNoteCard *card = g_new0(MockNoteCard, 1);
  card->binding_id = 0;  /* Starts unbound */
  card->disposed = FALSE;
  card->content_text = NULL;
  return card;
}

static void mock_card_free(MockNoteCard *card) {
  if (!card) return;
  g_free(card->content_text);
  g_free(card);
}

static void mock_prepare_for_bind(MockNoteCard *card) {
  if (!card) return;
  card->disposed = FALSE;
  card->binding_id = mock_binding_id_counter++;
}

static void mock_prepare_for_unbind(MockNoteCard *card) {
  if (!card) return;
  card->disposed = TRUE;
  card->binding_id = 0;
}

static int mock_is_bound(MockNoteCard *card) {
  return card && card->binding_id != 0;
}

/* Mimics the guard logic in nostr_gtk_note_card_row_set_content */
static void mock_set_content(MockNoteCard *card, const char *content) {
  if (!card) return;
  if (card->disposed) return;
  if (card->binding_id == 0) return;  /* THIS IS THE KEY CHECK */

  g_free(card->content_text);
  card->content_text = g_strdup(content);
}

/* Test: Binding ID is properly set by prepare_for_bind */
static void test_binding_id_lifecycle(void) {
  MockNoteCard *card = mock_card_new();

  /* Initially unbound */
  g_assert_cmpuint(card->binding_id, ==, 0);
  g_assert_false(mock_is_bound(card));

  /* After prepare_for_bind, should be bound with non-zero ID */
  mock_prepare_for_bind(card);
  g_assert_cmpuint(card->binding_id, >, 0);
  g_assert_true(mock_is_bound(card));

  uint64_t first_id = card->binding_id;

  /* After unbind, should be unbound again */
  mock_prepare_for_unbind(card);
  g_assert_cmpuint(card->binding_id, ==, 0);
  g_assert_false(mock_is_bound(card));

  /* Rebind should get a new unique ID */
  mock_prepare_for_bind(card);
  g_assert_cmpuint(card->binding_id, >, 0);
  g_assert_cmpuint(card->binding_id, !=, first_id);

  mock_card_free(card);
}

/* Test: set_content only works when bound */
static void test_set_content_requires_binding(void) {
  MockNoteCard *card = mock_card_new();

  /* Without prepare_for_bind, set_content should be a no-op */
  mock_set_content(card, "Test content");
  g_assert_null(card->content_text);

  /* After prepare_for_bind, set_content should work */
  mock_prepare_for_bind(card);
  mock_set_content(card, "Test content");
  g_assert_nonnull(card->content_text);
  g_assert_cmpstr(card->content_text, ==, "Test content");

  /* After unbind, set_content should be a no-op again */
  mock_prepare_for_unbind(card);
  g_free(card->content_text);
  card->content_text = NULL;
  mock_set_content(card, "New content");
  g_assert_null(card->content_text);

  mock_card_free(card);
}

/* Test: disposed flag blocks set_content */
static void test_disposed_blocks_set_content(void) {
  MockNoteCard *card = mock_card_new();
  mock_prepare_for_bind(card);

  /* Should work initially */
  mock_set_content(card, "First content");
  g_assert_cmpstr(card->content_text, ==, "First content");

  /* Set disposed but keep binding_id non-zero (edge case) */
  card->disposed = TRUE;
  mock_set_content(card, "Second content");
  /* Content should NOT change */
  g_assert_cmpstr(card->content_text, ==, "First content");

  mock_card_free(card);
}

/* Test: Multiple cards get unique IDs */
static void test_unique_binding_ids(void) {
  MockNoteCard *card1 = mock_card_new();
  MockNoteCard *card2 = mock_card_new();
  MockNoteCard *card3 = mock_card_new();

  mock_prepare_for_bind(card1);
  mock_prepare_for_bind(card2);
  mock_prepare_for_bind(card3);

  /* All IDs should be unique */
  g_assert_cmpuint(card1->binding_id, !=, card2->binding_id);
  g_assert_cmpuint(card2->binding_id, !=, card3->binding_id);
  g_assert_cmpuint(card1->binding_id, !=, card3->binding_id);

  /* All IDs should be non-zero */
  g_assert_cmpuint(card1->binding_id, >, 0);
  g_assert_cmpuint(card2->binding_id, >, 0);
  g_assert_cmpuint(card3->binding_id, >, 0);

  mock_card_free(card1);
  mock_card_free(card2);
  mock_card_free(card3);
}

/* Test: Simulates the repo browser bug - creating card without prepare_for_bind */
static void test_repo_browser_bug_simulation(void) {
  /* This simulates the BROKEN code path (before the fix):
   * card = new()
   * set_content(card, ...) <- fails silently because binding_id == 0
   */
  MockNoteCard *broken_card = mock_card_new();
  mock_set_content(broken_card, "📦 TestRepo\n\nDescription\n\n🔗 https://example.com");
  g_assert_null(broken_card->content_text);  /* Content NOT set - blank card! */
  mock_card_free(broken_card);

  /* This simulates the FIXED code path (after the fix):
   * card = new()
   * prepare_for_bind(card)
   * set_content(card, ...) <- succeeds because binding_id > 0
   */
  MockNoteCard *fixed_card = mock_card_new();
  mock_prepare_for_bind(fixed_card);
  mock_set_content(fixed_card, "📦 TestRepo\n\nDescription\n\n🔗 https://example.com");
  g_assert_nonnull(fixed_card->content_text);  /* Content IS set - card displays! */
  g_assert_true(g_str_has_prefix(fixed_card->content_text, "📦 TestRepo"));
  mock_card_free(fixed_card);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/note_card_binding/lifecycle", test_binding_id_lifecycle);
  g_test_add_func("/note_card_binding/set_content_requires_binding", test_set_content_requires_binding);
  g_test_add_func("/note_card_binding/disposed_blocks_set_content", test_disposed_blocks_set_content);
  g_test_add_func("/note_card_binding/unique_ids", test_unique_binding_ids);
  g_test_add_func("/note_card_binding/repo_browser_bug_simulation", test_repo_browser_bug_simulation);

  return g_test_run();
}
