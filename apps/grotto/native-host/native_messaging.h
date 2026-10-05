/* native_messaging.h - WebExtension native-messaging framing (nostrc-jjyp)
 *
 * Browser <-> host transport used by `nostr-signer-webext-host`:
 * every message is a 32-bit length prefix followed by that many bytes of
 * UTF-8 JSON. The browsers specify "native byte order"; every platform we
 * build for is little-endian, and the prefix is encoded/decoded as LE
 * explicitly so the wire format never depends on the build host.
 *
 * The functions take explicit file descriptors so the unit tests can drive
 * them over a pipe(2) instead of the process's real stdin/stdout.
 */
#ifndef APPS_GNOSTR_SIGNER_NATIVE_HOST_NATIVE_MESSAGING_H
#define APPS_GNOSTR_SIGNER_NATIVE_HOST_NATIVE_MESSAGING_H

#include <glib.h>

G_BEGIN_DECLS

/* Host -> browser: Chromium refuses messages over 1 MiB, so that is the
 * ceiling in both directions (Firefox allows more; no NIP-07 payload needs
 * it). */
#define NM_MAX_MESSAGE_SIZE (1024u * 1024u)

/* An over-limit frame is drained (so the stream stays in sync) only when
 * the claimed size is below this; beyond it the peer is not a browser and
 * the host gives up on the stream. */
#define NM_MAX_DRAIN_SIZE (64u * 1024u * 1024u)

typedef enum {
  NM_FRAME_OK = 0,
  NM_FRAME_EOF,        /* clean end of stream before a length prefix */
  NM_FRAME_IO,         /* read/write error or EOF inside a frame */
  NM_FRAME_TOO_LARGE,  /* frame over NM_MAX_MESSAGE_SIZE (drained if possible) */
  NM_FRAME_EMPTY       /* zero-length frame */
} NmFrameStatus;

/* Encode @json (@len bytes) as one frame. Returns NULL when @len exceeds
 * NM_MAX_MESSAGE_SIZE. */
GBytes *nm_frame_encode(const gchar *json, gsize len);

/* Decode the 4-byte little-endian length prefix at @prefix. */
guint32 nm_frame_decode_length(const guint8 prefix[4]);

/* Read one frame from @fd. On NM_FRAME_OK returns a NUL-terminated buffer
 * (caller g_free()s) and stores its length in @out_len. On
 * NM_FRAME_TOO_LARGE the payload has already been drained when its size
 * was below NM_MAX_DRAIN_SIZE; @out_len carries the claimed size. */
gchar *nm_frame_read(int fd, gsize *out_len, NmFrameStatus *out_status);

/* Write @json as one frame to @fd. */
NmFrameStatus nm_frame_write(int fd, const gchar *json, gsize len);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_NATIVE_HOST_NATIVE_MESSAGING_H */
