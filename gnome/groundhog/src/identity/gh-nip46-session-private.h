#ifndef GH_NIP46_SESSION_PRIVATE_H
#define GH_NIP46_SESSION_PRIVATE_H

#include "gh-nip46-session.h"

/* Test clock compression; call before start. Not installed or used by the UI. */
void gh_nip46_session_set_test_deadlines(GhNip46Session *self,
                                         guint publish_seconds,
                                         guint approval_seconds,
                                         guint auth_url_seconds,
                                         guint pair_seconds);

/* Test clock compression for the listen grace, the QR ack window and the
 * publish retry backoff base. Before start. */
void gh_nip46_session_set_test_windows(GhNip46Session *self, guint listen_grace_ms,
                                       guint ack_window_ms, guint retry_base_ms);

#endif
