#define _GNU_SOURCE /* dladdr, RTLD_NOLOAD */
#include <glib.h>
#include <nostr-gobject-1.0/nostr_nip19.h>
#include <nostr-gtk-1.0/gn-markdown.h>
#include <nostr-utils.h>
#include <channel.h>
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

#ifndef __APPLE__
/* Fails if the shared library containing own_function defines its own
 * libnostr/libgo symbols instead of binding to the process's one copy.
 * dlsym() on a handle searches only that library and its dependencies, so
 * an embedded copy is exactly what it returns. */
static void
assert_no_private_copy(const char *what, const void *own_function)
{
  Dl_info own;
  memset(&own, 0, sizeof own);
  g_assert_cmpint(dladdr(own_function, &own), !=, 0);
  void *library = dlopen(own.dli_fname, RTLD_NOW | RTLD_NOLOAD);
  g_assert_nonnull(library);
  static const char *const symbols[] =
  {
    "nostr_named_mutex_pool", "nostr_memhash", "go_channel_create",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(symbols); i++)
  {
    void *seen = dlsym(library, symbols[i]);
    if (seen == NULL)
      continue;
    Dl_info where;
    memset(&where, 0, sizeof where);
    g_assert_cmpint(dladdr(seen, &where), !=, 0);
    if (where.dli_fbase == own.dli_fbase)
      g_error("%s holds its own copy of %s (%s)", what, symbols[i], own.dli_fname);
  }
  dlclose(library);
}
#endif

static void
test_one_libnostr(void)
{
#ifdef __APPLE__
  g_test_skip("macOS dylibs embed the static libnostr");
  /* The macOS dylib embeds libnostr: see NOSTRC_ARCHIVES_FROM_EXECUTABLE. */
#else
  /* Referencing these puts utils.o and the libgo channel code into this
   * executable: the copy every shared library in the process must use. */
  g_assert_cmpuint(nostr_memhash("x", 1), ==, nostr_memhash("x", 1));
  GoChannel *channel = go_channel_create(1);
  g_assert_nonnull(channel);
  go_channel_unref(channel);
  assert_no_private_copy("nostr_gobject", (const void *)gnostr_nip19_get_type);
  assert_no_private_copy("nostr_gtk_portable", (const void *)gn_markdown_parse);
#endif
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/portable/one-nip19-type", test_one_nip19_type);
  g_test_add_func("/groundhog/portable/one-libnostr", test_one_libnostr);
  return g_test_run();
}
