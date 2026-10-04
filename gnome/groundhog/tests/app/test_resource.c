#include <gio/gio.h>

GResource *groundhog_get_resource(void);
void groundhog_register_resource(void);
void groundhog_unregister_resource(void);

static void
assert_resource(const char *path)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = g_resources_lookup_data(path, G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
  g_assert_no_error(error);
  g_assert_nonnull(bytes);
  g_assert_cmpuint(g_bytes_get_size(bytes), >, 0);
}

static void
test_resource_registration(void)
{
  /* The compiled Blueprint templates in data/ui, which the UI loads by path. */
  static const char *ui[] = {
    "/org/nostr/Groundhog/ui/gh-window.ui",
    "/org/nostr/Groundhog/ui/gh-sidebar-page.ui",
    "/org/nostr/Groundhog/ui/gh-content-page.ui",
    "/org/nostr/Groundhog/ui/gh-onboarding-view.ui",
    "/org/nostr/Groundhog/ui/gh-onboarding-row.ui",
    "/org/nostr/Groundhog/ui/gh-account-ui.ui",
    "/org/nostr/Groundhog/ui/gh-conversation-row.ui",
    "/org/nostr/Groundhog/ui/gh-shortcuts-window.ui",
    "/org/nostr/Groundhog/ui/gh-conversation-view.ui",
    "/org/nostr/Groundhog/ui/gh-message-row.ui",
    "/org/nostr/Groundhog/ui/gh-message-list-item.ui",
    "/org/nostr/Groundhog/ui/gh-day-separator.ui",
    "/org/nostr/Groundhog/ui/gh-delivery-details.ui",
  };

  g_assert_null(groundhog_get_resource());
  groundhog_register_resource();
  g_assert_nonnull(groundhog_get_resource());

  assert_resource("/org/nostr/Groundhog/style.css");
  /* The reviewed onboarding relay suggestions (charter D4). */
  assert_resource("/org/nostr/Groundhog/relay-suggestions.json");
  assert_resource("/org/nostr/Groundhog/blossom-media-suggestions.txt");
  for (guint i = 0; i < G_N_ELEMENTS(ui); i++)
    assert_resource(ui[i]);
  /* Bundled symbolic icons for hosts whose icon theme lacks them (the
   * account icon: nostrc-qp24.70). */
  assert_resource("/org/nostr/Groundhog/icons/scalable/actions/send-symbolic.svg");
  assert_resource("/org/nostr/Groundhog/icons/scalable/actions/avatar-default-symbolic.svg");

  groundhog_unregister_resource();
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/resource/registration", test_resource_registration);
  return g_test_run();
}
