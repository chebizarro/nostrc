/* Result-meta formatting and sanitizing (NOT markup-escaping: the Shell
 * escapes descriptions itself), truncation, relative time, item parsing. */
#include <string.h>

#include "nd-event.h"
#include "nsp-item.h"
#include "nsp-testutil.h"
#include "nsp-text.h"

#define NOW 1790000000

static void test_sanitize(void) {
  struct { const char *in, *out; } cases[] = {
      {"a\nb\tc\r\n  d", "a b c d"},
      {"   padded   ", "padded"},
      {"<b>bold</b> & <i>x</i>", "<b>bold</b> & <i>x</i>"}, /* raw: Shell escapes */
      {"\xe2\x80\xae" "evil\xe2\x80\xac", "evil"},           /* RLO / PDF stripped */
      {"zero\xe2\x80\x8bwidth\xef\xbb\xbf", "zerowidth"},    /* ZWSP, BOM */
      {"bell\x07" "char", "bell char"},
      {"nel\xc2\x85line", "nel line"},                       /* C1 control */
      {"para\xe2\x80\xa9sep", "para sep"},
      {"", ""},
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_autofree char *s = nsp_text_sanitize(cases[i].in, 0);
    g_assert_cmpstr(s, ==, cases[i].out);
  }
  g_autofree char *nul = nsp_text_sanitize(NULL, 10);
  g_assert_cmpstr(nul, ==, "");
  g_autofree char *bad = nsp_text_sanitize("ok\xff\xfe" "end", 0);
  g_assert_true(g_utf8_validate(bad, -1, NULL));
  g_assert_true(g_str_has_prefix(bad, "ok") && g_str_has_suffix(bad, "end"));
}

static void test_truncate(void) {
  g_autoptr(GString) s = g_string_new(NULL);
  for (int i = 0; i < 100; i++) g_string_append(s, "\xc3\xa9"); /* é */
  g_autofree char *t = nsp_text_sanitize(s->str, 80);
  g_assert_cmpint(g_utf8_strlen(t, -1), ==, 81); /* 80 + … */
  g_assert_true(g_str_has_suffix(t, "\xe2\x80\xa6"));

  g_autoptr(GString) e = g_string_new(NULL);
  for (int i = 0; i < 80; i++) g_string_append(e, "\xf0\x9f\x98\x80"); /* 😀 ×80 */
  g_autofree char *exact = nsp_text_sanitize(e->str, 80);
  g_assert_cmpint(g_utf8_strlen(exact, -1), ==, 80); /* exactly fits: no … */
  g_assert_false(g_str_has_suffix(exact, "\xe2\x80\xa6"));
  g_string_append(e, "!");
  g_autofree char *over = nsp_text_sanitize(e->str, 80);
  g_assert_cmpint(g_utf8_strlen(over, -1), ==, 81);

  /* collapsed whitespace does not count twice */
  g_autofree char *ws = nsp_text_sanitize("ab\n\n\n\ncd", 5);
  g_assert_cmpstr(ws, ==, "ab cd");
}

static void test_relative_time(void) {
  struct { gint64 ago; const char *want; } c[] = {
      {-100, "just now"}, {0, "just now"}, {59, "just now"}, {60, "1 min ago"},
      {3599, "59 min ago"}, {3600, "1 h ago"}, {86399, "23 h ago"}, {86400, "1 d ago"},
      {6 * 86400, "6 d ago"},
  };
  for (guint i = 0; i < G_N_ELEMENTS(c); i++) {
    g_autofree char *s = nsp_text_relative_time(NOW - c[i].ago, NOW);
    g_assert_cmpstr(s, ==, c[i].want);
  }
  g_autofree char *old = nsp_text_relative_time(1700000000, NOW);
  g_assert_cmpstr(old, ==, "2023-11-14");
}

static void test_short_npub(void) {
  g_autofree char *pk = nsp_test_pubkey(NSP_TEST_SK_ALICE);
  g_autofree char *s = nsp_text_short_npub(pk);
  g_assert_true(g_str_has_prefix(s, "npub1"));
  g_assert_nonnull(strstr(s, "\xe2\x80\xa6"));
  g_assert_cmpint(g_utf8_strlen(s, -1), ==, 12 + 1 + 6);
  g_assert_null(nsp_text_short_npub("xyz"));
}

static NspItem *item_from(const char *json) {
  g_autoptr(NdEvent) ev = nd_event_parse(json, -1, NULL);
  g_assert_nonnull(ev);
  return nsp_item_new_from_event(ev);
}

static void test_profile_meta(void) {
  g_autofree char *json = nsp_test_event(
      NSP_TEST_SK_ALICE, 0, NOW - 10,
      "{\"name\":\"alice\",\"display_name\":\"Alice \\u202eW\\nonder\",\"nip05\":\"Alice@Nos.Social\","
      "\"picture\":\"https://img.example.com/a.png\",\"about\":\"likes <b>tags</b>\"}",
      NULL);
  g_autoptr(NspItem) it = item_from(json);
  g_assert_nonnull(it);
  g_assert_true(nsp_item_is_profile(it));
  g_assert_true(g_str_has_prefix(it->uri, "nostr:npub1"));
  g_assert_cmpstr(it->nip05, ==, "alice@nos.social");
  g_assert_cmpstr(it->picture, ==, "https://img.example.com/a.png");

  g_autofree char *name = NULL, *desc = NULL;
  nsp_item_meta_text(it, NULL, TRUE, NOW, &name, &desc);
  g_assert_cmpstr(name, ==, "Alice W onder");
  g_assert_true(g_str_has_prefix(desc, "\xe2\x9c\x93 alice@nos.social \xc2\xb7 npub1"));

  g_autofree char *n2 = NULL, *d2 = NULL;
  nsp_item_meta_text(it, NULL, FALSE, NOW, &n2, &d2);
  g_assert_true(g_str_has_prefix(d2, "alice@nos.social \xc2\xb7 npub1")); /* no ✓ */

  /* fallbacks: name only; nothing at all → short npub; http picture dropped */
  g_autofree char *j3 = nsp_test_event(NSP_TEST_SK_BOB, 0, NOW, "{\"name\":\"bob\",\"picture\":\"http://x/y\"}", NULL);
  g_autoptr(NspItem) b = item_from(j3);
  g_assert_null(b->picture);
  g_autofree char *n3 = NULL, *d3 = NULL;
  nsp_item_meta_text(b, NULL, FALSE, NOW, &n3, &d3);
  g_assert_cmpstr(n3, ==, "bob");
  g_assert_true(g_str_has_prefix(d3, "npub1"));
  g_autofree char *j4 = nsp_test_event(NSP_TEST_SK_BOB, 0, NOW, "not json", NULL);
  g_autoptr(NspItem) c = item_from(j4);
  g_autofree char *n4 = NULL, *d4 = NULL;
  nsp_item_meta_text(c, NULL, FALSE, NOW, &n4, &d4);
  g_assert_true(g_str_has_prefix(n4, "npub1"));
}

static void test_note_meta(void) {
  g_autofree char *pj = nsp_test_event(NSP_TEST_SK_ALICE, 0, NOW, "{\"display_name\":\"Alice\"}", NULL);
  g_autoptr(NspItem) author = item_from(pj);
  g_autoptr(GString) body = g_string_new("<script>alert(1)</script> &amp;\n\nsecond line ");
  for (int i = 0; i < 20; i++) g_string_append(body, "\xe6\x97\xa5\xe6\x9c\xac "); /* 日本 */
  g_autofree char *nj = nsp_test_event(NSP_TEST_SK_ALICE, 1, NOW - 300, body->str, NULL);
  g_autoptr(NspItem) note = item_from(nj);
  g_assert_false(nsp_item_is_profile(note));
  g_assert_true(g_str_has_prefix(note->uri, "nostr:nevent1"));

  g_autofree char *name = NULL, *desc = NULL;
  nsp_item_meta_text(note, author, FALSE, NOW, &name, &desc);
  g_assert_cmpstr(name, ==, "Alice \xc2\xb7 5 min ago");
  g_assert_true(g_str_has_prefix(desc, "<script>alert(1)</script> &amp; second line "));
  g_assert_null(strchr(desc, '\n'));
  /* <= 80 code points + "…" (a space is never left dangling before it) */
  g_assert_cmpint(g_utf8_strlen(desc, -1), <=, NSP_NOTE_SNIPPET_CHARS + 1);
  g_assert_cmpint(g_utf8_strlen(desc, -1), >=, NSP_NOTE_SNIPPET_CHARS);
  g_assert_true(g_str_has_suffix(desc, "\xe2\x80\xa6"));
  g_assert_false(g_str_has_suffix(desc, " \xe2\x80\xa6"));

  /* unknown author → short npub */
  g_autofree char *n2 = NULL, *d2 = NULL;
  nsp_item_meta_text(note, NULL, FALSE, NOW, &n2, &d2);
  g_assert_true(g_str_has_prefix(n2, "npub1"));
}

static void test_article_and_bare(void) {
  const char *tags[] = {"d", "post-1", "title", "My\nTitle", "summary", "sum", NULL};
  g_autofree char *j = nsp_test_event(NSP_TEST_SK_BOB, 30023, NOW - 7200, "long body", tags);
  g_autoptr(NspItem) a = item_from(j);
  g_assert_true(g_str_has_prefix(a->uri, "nostr:naddr1"));
  g_autofree char *name = NULL, *desc = NULL;
  nsp_item_meta_text(a, NULL, FALSE, NOW, &name, &desc);
  g_assert_cmpstr(desc, ==, "My Title");
  g_assert_true(g_str_has_suffix(name, "2 h ago"));

  g_autofree char *pk = nsp_test_pubkey(NSP_TEST_SK_ALICE);
  NdTarget t = {.entity = ND_ENTITY_PROFILE, .pubkey_hex = pk, .kind = 0};
  g_autoptr(NspItem) bare = nsp_item_new_bare(&t, "alice@nos.social");
  g_assert_true(nsp_item_is_profile(bare));
  g_autofree char *bn = NULL, *bd = NULL;
  nsp_item_meta_text(bare, NULL, TRUE, NOW, &bn, &bd);
  g_assert_cmpstr(bn, ==, "Open Nostr profile");
  g_assert_true(g_str_has_prefix(bd, "\xe2\x9c\x93 alice@nos.social \xc2\xb7 npub1"));
}

static void test_rejects_forged(void) {
  g_autofree char *j = nsp_test_event(NSP_TEST_SK_ALICE, 1, NOW, "original", NULL);
  g_autofree char *forged = NULL;
  {
    GString *s = g_string_new(j);
    g_string_replace(s, "original", "tampered", 1);
    forged = g_string_free(s, FALSE);
  }
  g_autoptr(NdEvent) ev = nd_event_parse(forged, -1, NULL);
  g_assert_nonnull(ev);
  g_assert_false(ev->validated);
  g_assert_null(nsp_item_new_from_event(ev));
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nsp/meta/sanitize", test_sanitize);
  g_test_add_func("/nsp/meta/truncate", test_truncate);
  g_test_add_func("/nsp/meta/relative-time", test_relative_time);
  g_test_add_func("/nsp/meta/short-npub", test_short_npub);
  g_test_add_func("/nsp/meta/profile", test_profile_meta);
  g_test_add_func("/nsp/meta/note", test_note_meta);
  g_test_add_func("/nsp/meta/article-bare", test_article_and_bare);
  g_test_add_func("/nsp/meta/rejects-forged", test_rejects_forged);
  return g_test_run();
}
