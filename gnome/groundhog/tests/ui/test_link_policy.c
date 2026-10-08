/* The message link policy (privacy charter PD-3, PT-3, §2.1), GTK-free:
 * which text becomes a link, that everything else renders literally, and
 * what a click on each link may do. Nothing here touches the network.
 */
#include "gh-link-policy.h"
#include "gh-display-name.h"

#include <nostr/nip19/nip19.h>
#include <string.h>
#include <stdlib.h>

/* The links of text as "uri|uri|…" for compact assertions. */
static gchar *
links_of(const gchar *text)
{
  g_autoptr(GPtrArray) links = gh_link_policy_find_links(text);
  g_autoptr(GString) out = g_string_new(NULL);
  for (guint i = 0; i < links->len; i++) {
    GhLink *link = g_ptr_array_index(links, i);
    /* Offsets point at exactly the link text. */
    g_assert_cmpint(strncmp(text + link->start, link->uri, link->end - link->start), ==, 0);
    g_assert_cmpuint(strlen(link->uri), ==, link->end - link->start);
    if (i)
      g_string_append_c(out, '|');
    g_string_append(out, link->uri);
  }
  return g_string_free(g_steal_pointer(&out), FALSE);
}

#define assert_links(text, expected)            \
  G_STMT_START {                                \
    g_autofree gchar *found_ = links_of(text);  \
    g_assert_cmpstr(found_, ==, (expected));    \
  } G_STMT_END

/* Every <a href="X">Y</a> in markup has Y == X: a link never shows other
 * text than where it goes (PD-3). */
static void
assert_anchors_show_their_target(const gchar *markup)
{
  g_autoptr(GRegex) anchor = g_regex_new("<a href=\"([^\"]*)\">([^<]*)</a>", 0, 0, NULL);
  g_autoptr(GMatchInfo) match = NULL;
  guint anchors = 0;
  for (g_regex_match(anchor, markup, 0, &match); g_match_info_matches(match);
       g_match_info_next(match, NULL)) {
    g_autofree gchar *href = g_match_info_fetch(match, 1);
    g_autofree gchar *text = g_match_info_fetch(match, 2);
    g_assert_cmpstr(href, ==, text);
    anchors++;
  }
  /* No other anchor markup survives. */
  g_autofree gchar *rest = g_regex_replace_literal(anchor, markup, -1, 0, "", 0, NULL);
  g_assert_null(strstr(rest, "<a"));
  g_assert_cmpuint(anchors, >, 0);
}

/* PT-3: markup in a message renders literally. */
static void
test_markup_is_literal(void)
{
  g_autofree gchar *bold = gh_link_policy_to_markup("<b>bold</b> & <i>x</i>");
  g_assert_cmpstr(bold, ==, "&lt;b&gt;bold&lt;/b&gt; &amp; &lt;i&gt;x&lt;/i&gt;");
  /* An anchor written in the message is text: each address in it becomes a
   * link showing itself, never "bank" text leading to "evil". */
  const gchar *anchor = "<a href=\"https://evil.example\">https://bank.example</a>";
  g_autofree gchar *markup = gh_link_policy_to_markup(anchor);
  g_assert_cmpstr(markup, ==,
                  "&lt;a href=&quot;<a href=\"https://evil.example\">https://evil.example</a>"
                  "&quot;&gt;<a href=\"https://bank.example\">https://bank.example</a>&lt;/a&gt;");
  assert_anchors_show_their_target(markup);
  /* Only the bare addresses become links, each showing itself. */
  g_autofree gchar *linked = gh_link_policy_to_markup("see https://example.com/a?b=1&c=2 now");
  g_assert_cmpstr(linked, ==,
                  "see <a href=\"https://example.com/a?b=1&amp;c=2\">"
                  "https://example.com/a?b=1&amp;c=2</a> now");
  assert_anchors_show_their_target(linked);
  g_autofree gchar *empty = gh_link_policy_to_markup("");
  g_assert_cmpstr(empty, ==, "");
  /* Invalid UTF-8 is shown repaired, with no links. */
  g_autofree gchar *invalid = gh_link_policy_to_markup("https://a.example/\xff");
  g_assert_true(g_utf8_validate(invalid, -1, NULL));
  g_assert_null(strstr(invalid, "<a "));
}

/* PT-3: javascript:, data: and file: are never links, nor any other scheme. */
static void
test_dangerous_schemes_are_text(void)
{
  assert_links("javascript:alert(1)", "");
  assert_links("click javascript://%0Aalert(1)", "");
  assert_links("data:text/html;base64,PHNjcmlwdD4=", "");
  assert_links("file:///etc/passwd", "");
  assert_links("ftp://files.example/x mailto:a@b.example ws://relay.example", "");
  /* Only at the start of a word: a scheme glued to other text is not one. */
  assert_links("xhttps://example.com", "");
  assert_links("javascript:https://example.com", "");
  assert_links("https://", "");
  assert_links("https:///path", "");
  g_autofree gchar *markup = gh_link_policy_to_markup("javascript:alert(1)");
  g_assert_cmpstr(markup, ==, "javascript:alert(1)");
  g_assert_cmpint(gh_link_policy_classify("javascript:alert(1)", NULL, NULL), ==,
                  GH_LINK_ACTION_REFUSE);
  g_assert_cmpint(gh_link_policy_classify("data:text/html,x", NULL, NULL), ==,
                  GH_LINK_ACTION_REFUSE);
  g_assert_cmpint(gh_link_policy_classify("file:///etc/passwd", NULL, NULL), ==,
                  GH_LINK_ACTION_REFUSE);
  /* A crafted href is refused unless it is exactly one link. */
  g_assert_cmpint(gh_link_policy_classify("https://a.example b", NULL, NULL), ==,
                  GH_LINK_ACTION_REFUSE);
  g_assert_cmpint(gh_link_policy_classify("", NULL, NULL), ==, GH_LINK_ACTION_REFUSE);
  g_assert_cmpint(gh_link_policy_classify(NULL, NULL, NULL), ==, GH_LINK_ACTION_REFUSE);
}

static void
test_link_boundaries(void)
{
  assert_links("Look: https://example.com.", "https://example.com");
  assert_links("(see https://en.example/wiki/Foo_(bar))", "https://en.example/wiki/Foo_(bar)");
  assert_links("(https://example.com/x)", "https://example.com/x");
  assert_links("“https://example.com/q”", "https://example.com/q");
  assert_links("<https://example.com>", "https://example.com");
  assert_links("'https://example.com'", "https://example.com");
  assert_links("https://example.com/a,b;c", "https://example.com/a,b;c");
  assert_links("two https://a.example http://b.example:8080/p?q#f!",
               "https://a.example|http://b.example:8080/p?q#f");
  assert_links("HTTPS://Example.COM/Path", "HTTPS://Example.COM/Path");
  assert_links("https://[2001:db8::1]:443/x", "https://[2001:db8::1]:443/x");
  /* Bidi overrides (U+202E, written as bytes: GCC rejects an unpaired one in
   * a literal) and zero-width characters end a link. */
  assert_links("https://example.com/" "\xe2\x80\xae" "txt.exe", "https://example.com/");
  assert_links("https://exa" "\xe2\x80\x8b" "mple.com", "https://exa");
  /* Malformed hosts and ports are not links. */
  assert_links("https://-bad.example", "");
  assert_links("https://a..b", "");
  assert_links("https://example.com:http/", "");
  assert_links("https://example.com:123456/", "");
  assert_links("https://[not-ipv6]/", "");
}

/* PT-3: an IDN host needs confirmation showing punycode; so does http:. */
static void
test_confirmation(void)
{
  GhLinkConfirmReasons reasons = GH_LINK_CONFIRM_NONE;
  g_autofree gchar *open = NULL;
  g_assert_cmpint(gh_link_policy_classify("https://example.com/a b", &reasons, &open), ==,
                  GH_LINK_ACTION_REFUSE);
  g_assert_null(open);

  g_assert_cmpint(gh_link_policy_classify("https://Example.com/Path?x=1", &reasons, &open), ==,
                  GH_LINK_ACTION_OPEN);
  g_assert_cmpint(reasons, ==, GH_LINK_CONFIRM_NONE);
  g_assert_cmpstr(open, ==, "https://example.com/Path?x=1");
  g_clear_pointer(&open, g_free);

  g_assert_cmpint(gh_link_policy_classify("http://example.com/", &reasons, &open), ==,
                  GH_LINK_ACTION_CONFIRM);
  g_assert_cmpint(reasons, ==, GH_LINK_CONFIRM_INSECURE);
  g_assert_cmpstr(open, ==, "http://example.com/");
  g_clear_pointer(&open, g_free);

  /* A Cyrillic look-alike of apple.com. */
  g_assert_cmpint(gh_link_policy_classify("https://аpple.com/login", &reasons, &open), ==,
                  GH_LINK_ACTION_CONFIRM);
  g_assert_cmpint(reasons, ==, GH_LINK_CONFIRM_IDN);
  g_assert_cmpstr(open, ==, "https://xn--pple-43d.com/login");
  g_clear_pointer(&open, g_free);
  assert_links("pay at https://аpple.com/login now", "https://аpple.com/login");

  /* Already punycode is still an IDN host. */
  g_assert_cmpint(gh_link_policy_classify("https://xn--pple-43d.com/", &reasons, &open), ==,
                  GH_LINK_ACTION_CONFIRM);
  g_assert_cmpint(reasons, ==, GH_LINK_CONFIRM_IDN);
  g_clear_pointer(&open, g_free);

  /* "bank.example@" is a user name; the host is evil.example. */
  g_assert_cmpint(gh_link_policy_classify("http://bank.example@evil.example/", &reasons, &open),
                  ==, GH_LINK_ACTION_CONFIRM);
  g_assert_cmpint(reasons, ==, GH_LINK_CONFIRM_INSECURE | GH_LINK_CONFIRM_USER_INFO);
  g_assert_cmpstr(open, ==, "http://bank.example@evil.example/");
  g_autofree gchar *host = gh_link_policy_dup_host("http://bank.example@evil.example/");
  g_assert_cmpstr(host, ==, "evil.example");
  g_clear_pointer(&open, g_free);

  /* Non-ASCII in the path is percent-encoded for opening, not a reason. */
  g_assert_cmpint(gh_link_policy_classify("https://example.com/café", &reasons, &open), ==,
                  GH_LINK_ACTION_OPEN);
  g_assert_cmpstr(open, ==, "https://example.com/caf%C3%A9");
  g_clear_pointer(&open, g_free);
}

static void
test_nostr_uris(void)
{
  const gchar *npub = "nostr:npub180cvv07tjdrrgpa0j7j7tmnyl2yr6yr7l8j4s3evf6u64th6gkwsyjh6w6";
  g_autofree gchar *text = g_strdup_printf("say hi to %s!", npub);
  assert_links(text, npub);
  g_assert_cmpint(gh_link_policy_classify(npub, NULL, NULL), ==, GH_LINK_ACTION_NOSTR);
  assert_links("nostr:nprofile1qqsrhuxx8l9ex335q7he0f09aej04zpazpl0ne2cgukyawd24mayt8gpp4mhxue69",
               "nostr:nprofile1qqsrhuxx8l9ex335q7he0f09aej04zpazpl0ne2cgukyawd24mayt8gpp4mhxue69");
  /* A secret key is never a link. */
  assert_links("nostr:nsec1vl029mgpspedva04g90vltkh6fvh240zqtv9k0t9af8935ke9laqsnlfe5", "");
  g_assert_cmpint(gh_link_policy_classify(
                    "nostr:nsec1vl029mgpspedva04g90vltkh6fvh240zqtv9k0t9af8935ke9laqsnlfe5",
                    NULL, NULL), ==, GH_LINK_ACTION_REFUSE);
  /* Not bech32, too short, glued to other letters, or not an entity. */
  assert_links("nostr:npub1", "");
  assert_links("nostr:npub1qqqqqqB", "");
  assert_links("nostr:npub1qqqqqqqqib", "");
  assert_links("nostr:relay.example", "");
  assert_links("xnostr:npub1qqqqqqqq", "");
  /* Bare npubs stay text: only nostr: URIs are links. */
  assert_links("npub180cvv07tjdrrgpa0j7j7tmnyl2yr6yr7l8j4s3evf6u64th6gkwsyjh6w6", "");
  /* A nostr: link is never a web preview. */
  g_assert_false(gh_link_policy_can_preview(npub));
}

/* §2.1: previews only for https, and only ever offered, never fetched here. */
static void
test_preview_eligibility(void)
{
  g_assert_true(gh_link_policy_can_preview("https://example.com/a"));
  g_assert_true(gh_link_policy_can_preview("https://аpple.com/"));
  g_assert_false(gh_link_policy_can_preview("http://example.com/"));
  g_assert_false(gh_link_policy_can_preview("https://user@example.com/"));
  g_assert_false(gh_link_policy_can_preview("javascript:alert(1)"));
  g_assert_false(gh_link_policy_can_preview("https://example.com/ x"));
  g_autofree gchar *first =
    gh_link_policy_dup_preview_uri("http://plain.example then https://secure.example/p.");
  g_assert_cmpstr(first, ==, "https://secure.example/p");
  g_autofree gchar *none = gh_link_policy_dup_preview_uri("no links here, <b>none</b>");
  g_assert_null(none);
  g_autofree gchar *host = gh_link_policy_dup_host("https://Sub.Example.COM:8443/x");
  g_assert_cmpstr(host, ==, "sub.example.com");
  g_assert_null(gh_link_policy_dup_host("nostr:npub1qqqqqqqq"));
}

static const gchar *
mention_name(const gchar *pubkey, gpointer data)
{
  return g_strcmp0(pubkey, data) == 0 ? "Alice & Bob" : NULL;
}

static void
test_mention_display_names(void)
{
  guint8 key[32];
  memset(key, 0xaa, sizeof key);
  char *npub = NULL;
  g_assert_cmpint(nostr_nip19_encode_npub(key, &npub), ==, 0);
  g_autofree gchar *uri = g_strconcat("nostr:", npub, NULL);
  free(npub);
  const gchar *account =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  gh_display_name_set_resolver(mention_name, (gpointer) account);
  g_autofree gchar *named = gh_link_policy_to_mention_markup(uri, NULL);
  g_autofree gchar *own = gh_link_policy_to_mention_markup(uri, account);
  g_assert_nonnull(strstr(named, "@Alice &amp; Bob</a>"));
  g_assert_nonnull(strstr(own, "<b>@Alice &amp; Bob</b>"));
  g_assert_nonnull(strstr(own, uri));
  gh_display_name_set_resolver(NULL, NULL);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/link-policy/markup-is-literal", test_markup_is_literal);
  g_test_add_func("/groundhog/link-policy/dangerous-schemes-are-text",
                  test_dangerous_schemes_are_text);
  g_test_add_func("/groundhog/link-policy/boundaries", test_link_boundaries);
  g_test_add_func("/groundhog/link-policy/confirmation", test_confirmation);
  g_test_add_func("/groundhog/link-policy/nostr-uris", test_nostr_uris);
  g_test_add_func("/groundhog/link-policy/preview-eligibility", test_preview_eligibility);
  g_test_add_func("/groundhog/link-policy/mention-display-names", test_mention_display_names);
  return g_test_run();
}
