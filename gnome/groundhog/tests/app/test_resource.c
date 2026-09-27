#include <gio/gio.h>

GResource *groundhog_get_resource(void);
void groundhog_register_resource(void);
void groundhog_unregister_resource(void);

static void
test_resource_registration(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) style = NULL;

  g_assert_null(groundhog_get_resource());
  groundhog_register_resource();
  g_assert_nonnull(groundhog_get_resource());

  style = g_resources_lookup_data("/org/nostr/Groundhog/style.css",
                                  G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
  g_assert_no_error(error);
  g_assert_nonnull(style);
  g_assert_cmpuint(g_bytes_get_size(style), >, 0);

  groundhog_unregister_resource();
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/resource/registration", test_resource_registration);
  return g_test_run();
}
