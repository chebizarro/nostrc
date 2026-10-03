/* Icon resolution test (nostrc-9juh).
 *
 * Verifies that every symbolic icon name referenced by the app resolves from
 * the bundled GResource, so non-GNOME desktops (which may lack the Adwaita
 * icon theme) see icons.  Runs without a display.
 *
 * GTK4 builtins — icons GTK ships internally (verified via
 * `strings libgtk-4.*.dylib | grep symbolic`) that do NOT need bundling:
 *   edit-copy-symbolic, edit-delete-symbolic, face-smile-symbolic,
 *   list-add-symbolic, list-remove-symbolic, object-select-symbolic,
 *   user-trash-symbolic, window-close-symbolic.
 * If any of these stops shipping with GTK, bundle it and move it to the
 * BUNDLED_ICONS array; the test failure from a missing GResource will catch
 * the regression at build time. */
#include <gio/gio.h>
#include <glib.h>
#include <string.h>

void groundhog_register_resource(void);

/* Every non-GTK-builtin symbolic icon the app references in Blueprint files
 * and C source.  Derived from a full grep audit of data/ui/.blp and
 * src/ui/.c (nostrc-9juh, review finding 4). */
static const char *const BUNDLED_ICONS[] = {
  "action-unavailable-symbolic",
  "alarm-symbolic",
  "audio-x-generic-symbolic",
  "avatar-default-symbolic",
  "changes-prevent-symbolic",
  "channel-secure-symbolic",
  "computer-symbolic",
  "content-loading-symbolic",
  "dialog-password-symbolic",
  "dialog-question-symbolic",
  "dialog-warning-symbolic",
  "document-open-recent-symbolic",
  "document-open-symbolic",
  "emblem-ok-symbolic",
  "emblem-synchronizing-symbolic",
  "go-bottom-symbolic",
  "go-down-symbolic",
  "go-next-symbolic",
  "go-previous-symbolic",
  "go-up-symbolic",
  "help-about-symbolic",
  "image-x-generic-symbolic",
  "mail-attachment-symbolic",
  "mail-read-symbolic",
  "mail-send-receive-symbolic",
  "mail-unread-symbolic",
  "network-offline-symbolic",
  "network-server-symbolic",
  "network-transmit-receive-symbolic",
  "network-workgroup-symbolic",
  "open-menu-symbolic",
  "package-x-generic-symbolic",
  "preferences-system-privacy-symbolic",
  "send-symbolic",
  "send-symbolic-rtl",
  "system-lock-screen-symbolic",
  "system-log-out-symbolic",
  "system-search-symbolic",
  "text-x-generic-symbolic",
  "user-bookmarks-symbolic",
  "video-x-generic-symbolic",
  "view-conceal-symbolic",
  "view-list-bullet-symbolic",
  "view-more-symbolic",
  "view-pin-symbolic",
  "view-refresh-symbolic",
  "x-office-document-symbolic",
};

static void
test_icon_resources(void)
{
  for (guint i = 0; i < G_N_ELEMENTS(BUNDLED_ICONS); i++) {
    g_autofree gchar *path =
        g_strdup_printf("/org/nostr/Groundhog/icons/scalable/actions/%s.svg",
                        BUNDLED_ICONS[i]);
    g_autoptr(GError) error = NULL;
    gboolean found = g_resources_get_info(path, G_RESOURCE_LOOKUP_FLAGS_NONE,
                                          NULL, NULL, &error);
    if (!found)
      g_test_message("missing resource: %s", path);
    g_assert_true(found);
  }
}

/* Sanity-check that each SVG is well-formed enough to start with an XML
 * declaration or an <svg tag. */
static void
test_icon_svg_valid(void)
{
  for (guint i = 0; i < G_N_ELEMENTS(BUNDLED_ICONS); i++) {
    g_autofree gchar *path =
        g_strdup_printf("/org/nostr/Groundhog/icons/scalable/actions/%s.svg",
                        BUNDLED_ICONS[i]);
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) bytes =
        g_resources_lookup_data(path, G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
    g_assert_no_error(error);
    g_assert_nonnull(bytes);
    gsize len = 0;
    const char *data = g_bytes_get_data(bytes, &len);
    g_assert_cmpuint(len, >, 20);
    /* Must start with either <?xml or <svg (possibly with a BOM). */
    gboolean ok = g_str_has_prefix(data, "<?xml") ||
                  g_str_has_prefix(data, "<svg") ||
                  g_str_has_prefix(data, "\xef\xbb\xbf<?xml");
    if (!ok)
      g_test_message("bad SVG header for %s: %.40s", BUNDLED_ICONS[i], data);
    g_assert_true(ok);
  }
}

int
main(int argc, char **argv)
{
  groundhog_register_resource();
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/icons/resources", test_icon_resources);
  g_test_add_func("/groundhog/icons/svg-valid", test_icon_svg_valid);
  return g_test_run();
}
