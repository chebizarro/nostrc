#include <glib.h>
#include <nostr-gobject-1.0/nostr_nip19.h>
#include <nostr-gtk-1.0/gn-markdown.h>
#include <dlfcn.h>
#include <string.h>

#ifndef PORTABLE_LIBRARY_PATH
#error PORTABLE_LIBRARY_PATH must name the built portable library
#endif

static void
test_one_nip19_type(void)
{
  /* Exercise the parser in the same process that owns Groundhog's GObject
   * types, then look up the type entry point through the portable DSO.
   * A private static copy there would have a different function address. */
  g_autoptr(GnMarkdownDocument) document = gn_markdown_parse("**safe**", -1);
  g_assert_nonnull(document);
  g_assert_cmpuint(document->tokens->len, >, 0);

  void *library = dlopen(PORTABLE_LIBRARY_PATH, RTLD_NOW | RTLD_LOCAL);
  g_assert_nonnull(library);
  void *symbol = dlsym(library, "gnostr_nip19_get_type");
  g_assert_nonnull(symbol);
  GType (*through_portable)(void) = NULL;
  g_assert_cmpuint(sizeof through_portable, ==, sizeof symbol);
  memcpy(&through_portable, &symbol, sizeof symbol);
  GType direct = gnostr_nip19_get_type();
  GType indirect = through_portable();
  g_assert_cmpuint(direct, !=, G_TYPE_INVALID);
  g_assert_cmpuint(indirect, ==, direct);
  g_assert_cmpstr(g_type_name(direct), ==, "GNostrNip19");
  dlclose(library);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/portable/one-nip19-type", test_one_nip19_type);
  return g_test_run();
}
