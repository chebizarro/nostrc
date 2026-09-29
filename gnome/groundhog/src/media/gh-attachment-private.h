#ifndef GH_ATTACHMENT_PRIVATE_H
#define GH_ATTACHMENT_PRIVATE_H

#include <glib.h>

G_BEGIN_DECLS

/* For tests (W17 review non-blocking #4): how many times the AES-GCM cipher
 * was run to decrypt, process-wide, since start. AT-2 asserts that it does
 * not move when x does not match, so a reordering that runs the cipher
 * before the x check fails the test. Thread-safe. */
guint gh_attachment_test_get_decrypt_runs(void);

G_END_DECLS
#endif
