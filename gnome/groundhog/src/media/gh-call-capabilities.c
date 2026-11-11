/* SPDX-License-Identifier: MIT */
#include "gh-call-capabilities.h"

#ifdef GROUNDHOG_HAVE_CALLS
#include <gst/gst.h>

static gboolean
check_factory(const gchar *name, GError **error)
{
  GstElementFactory *factory = gst_element_factory_find(name);
  GstPluginFeature *loaded = factory
      ? gst_plugin_feature_load(GST_PLUGIN_FEATURE(factory)) : NULL;
  if (factory)
    gst_object_unref(factory);
  if (!loaded) {
    g_set_error(error, g_quark_from_static_string("gh-call-capabilities"), 1,
                "Calls unavailable: missing GStreamer element '%s' "
                "(install its runtime plugin)", name);
    return FALSE;
  }
  gst_object_unref(loaded);
  return TRUE;
}
#endif

gboolean
gh_call_capabilities_check(gboolean require_echo_cancellation, GError **error)
{
#ifdef GROUNDHOG_HAVE_CALLS
  GError *init_error = NULL;
  if (!gst_init_check(NULL, NULL, &init_error)) {
    g_set_error(error, g_quark_from_static_string("gh-call-capabilities"), 1,
                "Calls unavailable: GStreamer initialization failed: %s",
                init_error->message);
    g_clear_error(&init_error);
    return FALSE;
  }
  static const gchar *required[] = {
    "webrtcbin", "nicesrc", "nicesink", "dtlssrtpenc", "dtlssrtpdec",
    "srtpenc", "srtpdec", "rtpbin", "rtpopuspay", "rtpopusdepay",
    "opusenc", "opusdec", "rtpvp8pay", "rtpvp8depay", "vp8enc", "vp8dec",
    "audioconvert", "audioresample", "videoconvert", "queue", NULL
  };
  for (const gchar **name = required; *name; name++)
    if (!check_factory(*name, error))
      return FALSE;
  if (require_echo_cancellation &&
      (!check_factory("webrtcdsp", error) ||
       !check_factory("webrtcechoprobe", error)))
    return FALSE;
  return TRUE;
#else
  (void)require_echo_cancellation;
  g_set_error_literal(error, g_quark_from_static_string("gh-call-capabilities"), 1,
                      "Calls unavailable: built without GStreamer WebRTC/SDP "
                      "and libnice support (GROUNDHOG_ENABLE_CALLS)");
  return FALSE;
#endif
}
