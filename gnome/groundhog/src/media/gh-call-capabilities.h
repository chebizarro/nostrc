/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Available even in builds without GStreamer/calls. Does not open devices or
 * sockets. Initialize GStreamer before concurrent media use. On failure, error
 * contains "Calls unavailable: <reason>" for a future call UI to display.
 *
 * Headset routes may pass FALSE. Speaker routes must pass TRUE unless the
 * caller has independently established system echo cancellation. A successful
 * check proves loadable factories, not device access or network connectivity.
 * H.264 negotiation is not covered: the baseline here is Opus + VP8.
 */
gboolean gh_call_capabilities_check(gboolean require_echo_cancellation,
                                    GError **error);

G_END_DECLS
