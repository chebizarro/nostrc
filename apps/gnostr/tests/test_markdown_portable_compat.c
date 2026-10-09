#include <glib.h>
#include "util/markdown_pango.h"

static void golden(void) {
  struct { const char *input; const char *markup; } cases[] = {
    { "Hello", "Hello\n" },
    { "**bold** and *italic*", "<b>bold</b> and <i>italic</i>\n" },
    { "`<code>`", "<tt>&lt;code&gt;</tt>\n" },
    { "[label](https://example.org)", "label (https://example.org)\n" },
    { "- item", "  • item\n" },
    { "> quote", "<span alpha=\"80%\" style=\"italic\">quote</span>\n" },
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_autofree char *actual = markdown_to_pango(cases[i].input, 0);
    g_assert_cmpstr(actual, ==, cases[i].markup);
  }
}
static void invalid_utf8(void) {
  g_autofree char *actual = markdown_to_pango("bad\xff", 0);
  g_assert_true(g_utf8_validate(actual, -1, NULL));
}
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/gnostr/markdown/portable-golden", golden);
  g_test_add_func("/gnostr/markdown/invalid-utf8", invalid_utf8);
  return g_test_run();
}
