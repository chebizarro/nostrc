#pragma once
#include <gst/sdp/sdp.h>

/* Internal wire policy. Host socket gathering is not a disclosure to the peer. */
gboolean gh_call_ice_address_allowed(const gchar *address);
gboolean gh_call_ice_candidate_allowed(const gchar *candidate, gboolean allow_local, gboolean relay_only);
gchar *gh_call_ice_candidate_for_peer(const gchar *candidate, gboolean allow_local, gboolean relay_only);
gchar *gh_call_ice_sdp_for_peer(const GstSDPMessage *sdp, gboolean allow_local, gboolean relay_only);
