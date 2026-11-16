#ifndef FIPSD_TEST_TMPDIR_H
#define FIPSD_TEST_TMPDIR_H

/* Call on the main thread. Creates $TMPDIR/fd-XXXXXX (or /tmp/fd-XXXXXX)
 * with mkdtemp; the returned path and any remaining contents are owned by an
 * atexit cleanup. Existing explicit unlink/rmdir assertions may stay in place.
 * Pass the longest socket suffix, including its leading '/', or NULL for
 * file-only tests. Before any bind, strlen(dir) + strlen(suffix) must be less
 * than sizeof(sockaddr_un.sun_path), leaving room for NUL (104 bytes on macOS,
 * 108 on Linux). Failure exits with the full offending path and limit. */
const char *fipsd_test_tmpdir(const char *socket_suffix);

#endif
