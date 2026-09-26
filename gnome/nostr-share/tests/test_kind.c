/* test_kind.c - kind mapper table
 *
 * SPDX-License-Identifier: MIT
 */
#include "ns-kind.h"

#include <glib.h>

static void
test_classify(void)
{
  g_assert_cmpint(ns_kind_classify("text/plain", "a.txt", FALSE, FALSE), ==, NS_CLASS_TEXT);
  g_assert_cmpint(ns_kind_classify("text/plain", "README.md", FALSE, FALSE), ==, NS_CLASS_MARKDOWN);
  g_assert_cmpint(ns_kind_classify("text/markdown", "x", FALSE, FALSE), ==, NS_CLASS_MARKDOWN);
  g_assert_cmpint(ns_kind_classify("text/uri-list", NULL, FALSE, FALSE), ==, NS_CLASS_URL);
  g_assert_cmpint(ns_kind_classify("image/jpeg", "p.jpg", FALSE, FALSE), ==, NS_CLASS_MEDIA);
  g_assert_cmpint(ns_kind_classify("video/mp4", "v.mp4", FALSE, FALSE), ==, NS_CLASS_MEDIA);
  g_assert_cmpint(ns_kind_classify("audio/ogg", "a.ogg", FALSE, FALSE), ==, NS_CLASS_MEDIA);
  g_assert_cmpint(ns_kind_classify("application/pdf", "d.pdf", FALSE, FALSE), ==, NS_CLASS_OTHER_FILE);
  g_assert_cmpint(ns_kind_classify("application/zip", "z.zip", FALSE, FALSE), ==, NS_CLASS_OTHER_FILE);
  g_assert_cmpint(ns_kind_classify("text/calendar", "e.ics", FALSE, FALSE), ==, NS_CLASS_CALENDAR);
  g_assert_cmpint(ns_kind_classify("text/plain", "e.ics", FALSE, FALSE), ==, NS_CLASS_CALENDAR);
  g_assert_cmpint(ns_kind_classify("text/x-vcard", "c.vcf", FALSE, FALSE), ==, NS_CLASS_CONTACT);
  g_assert_cmpint(ns_kind_classify("text/vcard", "c", FALSE, FALSE), ==, NS_CLASS_CONTACT);
  g_assert_cmpint(ns_kind_classify("inode/directory", "/r", TRUE, TRUE), ==, NS_CLASS_GIT_REPO);
  g_assert_cmpint(ns_kind_classify("inode/directory", "/d", TRUE, FALSE), ==, NS_CLASS_DIRECTORY);
  g_assert_cmpint(ns_kind_classify(NULL, NULL, FALSE, FALSE), ==, NS_CLASS_OTHER_FILE);
}

typedef struct {
  NsInputClass cls;
  gint         forced;
  gboolean     ok;
  NsAction     action;
  gint         kind;
} Row;

static void
test_resolve_table(void)
{
  const Row rows[] = {
    /* plain text: always a note by default; article only on request */
    { NS_CLASS_TEXT, 0, TRUE, NS_ACTION_NOTE, 1 },
    { NS_CLASS_TEXT, 1, TRUE, NS_ACTION_NOTE, 1 },
    { NS_CLASS_TEXT, 30023, TRUE, NS_ACTION_ARTICLE, 30023 },
    { NS_CLASS_TEXT, 1063, FALSE, 0, 0 },
    /* markdown */
    { NS_CLASS_MARKDOWN, 0, TRUE, NS_ACTION_ARTICLE, 30023 },
    { NS_CLASS_MARKDOWN, 1, TRUE, NS_ACTION_NOTE, 1 },
    { NS_CLASS_MARKDOWN, 30617, FALSE, 0, 0 },
    /* URL */
    { NS_CLASS_URL, 0, TRUE, NS_ACTION_NOTE, 1 },
    { NS_CLASS_URL, 30023, FALSE, 0, 0 },
    /* media */
    { NS_CLASS_MEDIA, 0, TRUE, NS_ACTION_MEDIA_NOTE, 1 },
    { NS_CLASS_MEDIA, 1, TRUE, NS_ACTION_MEDIA_NOTE, 1 },
    { NS_CLASS_MEDIA, 1063, TRUE, NS_ACTION_FILE_METADATA, 1063 },
    { NS_CLASS_MEDIA, 30023, FALSE, 0, 0 },
    /* other files */
    { NS_CLASS_OTHER_FILE, 0, TRUE, NS_ACTION_FILE_METADATA, 1063 },
    { NS_CLASS_OTHER_FILE, 1, TRUE, NS_ACTION_MEDIA_NOTE, 1 },
    /* git */
    { NS_CLASS_GIT_REPO, 0, TRUE, NS_ACTION_GIT_REPO, 30617 },
    { NS_CLASS_GIT_REPO, 30617, TRUE, NS_ACTION_GIT_REPO, 30617 },
    { NS_CLASS_GIT_REPO, 1, FALSE, 0, 0 },
    /* nostr-dav hand-off: no kind of ours, no override */
    { NS_CLASS_CALENDAR, 0, TRUE, NS_ACTION_DAV_CALENDAR, 0 },
    { NS_CLASS_CONTACT, 0, TRUE, NS_ACTION_DAV_CONTACT, 0 },
    { NS_CLASS_CALENDAR, 31923, FALSE, 0, 0 },
    /* plain directories are refused */
    { NS_CLASS_DIRECTORY, 0, FALSE, 0, 0 },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(rows); i++) {
    const Row *r = &rows[i];
    NsAction a = 99;
    GError *err = NULL;
    gboolean ok = ns_kind_resolve(r->cls, r->forced, &a, &err);
    g_test_message("row %" G_GSIZE_FORMAT ": %s forced=%d",
                   i, ns_class_name(r->cls), r->forced);
    g_assert_cmpint(ok, ==, r->ok);
    if (ok) {
      g_assert_no_error(err);
      g_assert_cmpint(a, ==, r->action);
      g_assert_cmpint(ns_action_kind(a), ==, r->kind);
    } else {
      g_assert_nonnull(err);
      g_assert_true(err->domain == NS_ERROR);
      g_clear_error(&err);
    }
  }
}

static void
test_choices(void)
{
  gint k[3];
  g_assert_cmpuint(ns_kind_choices(NS_CLASS_MEDIA, k), ==, 2);
  g_assert_cmpint(k[0], ==, 1);
  g_assert_cmpint(k[1], ==, 1063);
  g_assert_cmpuint(ns_kind_choices(NS_CLASS_TEXT, k), ==, 2);
  g_assert_cmpint(k[0], ==, 1);
  g_assert_cmpuint(ns_kind_choices(NS_CLASS_MARKDOWN, k), ==, 2);
  g_assert_cmpint(k[0], ==, 30023);
  g_assert_cmpuint(ns_kind_choices(NS_CLASS_CALENDAR, k), ==, 0);
  /* Every offered choice must resolve, and the first is the default. */
  for (NsInputClass c = NS_CLASS_TEXT; c <= NS_CLASS_OTHER_FILE; c++) {
    guint n = ns_kind_choices(c, k);
    for (guint i = 0; i < n; i++) {
      NsAction a, def;
      g_assert_true(ns_kind_resolve(c, k[i], &a, NULL));
      g_assert_cmpint(ns_action_kind(a), ==, k[i]);
      if (i == 0) {
        g_assert_true(ns_kind_resolve(c, 0, &def, NULL));
        g_assert_cmpint(ns_action_kind(def), ==, k[0]);
      }
    }
  }
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nostr-share/kind/classify", test_classify);
  g_test_add_func("/nostr-share/kind/resolve-table", test_resolve_table);
  g_test_add_func("/nostr-share/kind/choices", test_choices);
  return g_test_run();
}
