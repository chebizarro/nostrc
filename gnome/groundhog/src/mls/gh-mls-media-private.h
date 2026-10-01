#ifndef GH_MLS_MEDIA_PRIVATE_H
#define GH_MLS_MEDIA_PRIVATE_H

#include <glib.h>

G_BEGIN_DECLS

/* Tests: bytes of decrypted attachments wiped on free so far. */
gsize gh_mls_media_test_wiped_bytes(void);

G_END_DECLS

#endif
