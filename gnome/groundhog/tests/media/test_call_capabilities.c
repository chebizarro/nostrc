/* SPDX-License-Identifier: MIT */
#include "gh-call-capabilities.h"
#ifdef GROUNDHOG_HAVE_CALLS
#include <gst/gst.h>
#endif

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  GError *error = NULL;
#ifdef GROUNDHOG_HAVE_CALLS
  g_assert_true(gst_init_check(NULL, NULL, &error));
  g_assert_no_error(error);
  /* A package qualification must fail, not skip, if the baseline is absent. */
  if (!gh_call_capabilities_check(FALSE, &error)) {
    g_printerr("%s\n", error->message);
    g_clear_error(&error);
    gst_deinit();
    return 1;
  }
  g_assert_no_error(error);
  const gchar *required[] = {
    "webrtcbin", "nicesrc", "nicesink", "dtlssrtpenc", "dtlssrtpdec",
    "srtpenc", "srtpdec", "rtpbin", "rtpopuspay", "rtpopusdepay",
    "opusenc", "opusdec", "rtpvp8pay", "rtpvp8depay", "vp8enc", "vp8dec",
    "audioconvert", "audioresample", "videoconvert", "queue", NULL
  };
  GstRegistry *registry = gst_registry_get();
  for (const gchar **name = required; *name; name++) {
    GstElementFactory *factory = gst_element_factory_find(*name);
    g_assert_nonnull(factory);
    gst_registry_remove_feature(registry, GST_PLUGIN_FEATURE(factory));
    g_assert_false(gh_call_capabilities_check(FALSE, &error));
    g_assert_nonnull(strstr(error->message, "Calls unavailable:"));
    g_assert_nonnull(strstr(error->message, *name));
    g_print("missing %s: %s\n", *name, error->message);
    g_clear_error(&error);
    g_assert_true(gst_registry_add_feature(registry, GST_PLUGIN_FEATURE(factory)));
    gst_object_unref(factory);
    g_assert_true(gh_call_capabilities_check(FALSE, &error));
    g_assert_no_error(error);
  }
  gboolean echo = gh_call_capabilities_check(TRUE, &error);
  g_print("echo cancellation: %s\n", echo ? "available" : error->message);
  g_clear_error(&error);
  /* The speaker route must fail closed when either half of AEC is absent. */
  const gchar *aec[] = {"webrtcdsp", "webrtcechoprobe", NULL};
  for (const gchar **name = aec; *name; name++) {
    GstElementFactory *factory = gst_element_factory_find(*name);
    if (factory)
      gst_registry_remove_feature(registry, GST_PLUGIN_FEATURE(factory));
    g_assert_false(gh_call_capabilities_check(TRUE, &error));
    g_assert_nonnull(strstr(error->message, "Calls unavailable:"));
    g_clear_error(&error);
    g_assert_true(gh_call_capabilities_check(FALSE, &error));
    g_assert_no_error(error);
    if (factory) {
      g_assert_true(gst_registry_add_feature(registry, GST_PLUGIN_FEATURE(factory)));
      gst_object_unref(factory);
    }
  }
  gst_deinit();
#else
  g_assert_false(gh_call_capabilities_check(FALSE, &error));
  g_assert_nonnull(strstr(error->message, "Calls unavailable: built without"));
  g_print("%s\n", error->message);
  g_clear_error(&error);
#endif
  return 0;
}
