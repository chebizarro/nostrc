/* native_messaging.c - WebExtension native-messaging framing (nostrc-jjyp)
 *
 * 4-byte little-endian length prefix + UTF-8 JSON, both directions. The
 * request routing that used to live here (and signed in-process straight
 * from the keyring) moved to nm_router.c / nm_provider_nip07.c, which
 * forward every operation to the org.nostr.Signer daemon instead: the host
 * never touches key material.
 */
#include "native_messaging.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

GBytes *nm_frame_encode(const gchar *json, gsize len) {
  if (!json || len > NM_MAX_MESSAGE_SIZE) return NULL;
  guint8 *buf = g_malloc(len + 4);
  buf[0] = (guint8)(len & 0xFF);
  buf[1] = (guint8)((len >> 8) & 0xFF);
  buf[2] = (guint8)((len >> 16) & 0xFF);
  buf[3] = (guint8)((len >> 24) & 0xFF);
  memcpy(buf + 4, json, len);
  return g_bytes_new_take(buf, len + 4);
}

guint32 nm_frame_decode_length(const guint8 prefix[4]) {
  return ((guint32)prefix[0]) | ((guint32)prefix[1] << 8) |
         ((guint32)prefix[2] << 16) | ((guint32)prefix[3] << 24);
}

/* Returns bytes read (== n on success); fewer means EOF, -1 means error. */
static gssize read_full(int fd, void *buf, gsize n) {
  guint8 *p = buf;
  gsize done = 0;
  while (done < n) {
    gssize rd = read(fd, p + done, n - done);
    if (rd < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (rd == 0) break;
    done += (gsize)rd;
  }
  return (gssize)done;
}

static gboolean write_full(int fd, const void *buf, gsize n) {
  const guint8 *p = buf;
  gsize done = 0;
  while (done < n) {
    gssize wr = write(fd, p + done, n - done);
    if (wr < 0) {
      if (errno == EINTR) continue;
      return FALSE;
    }
    done += (gsize)wr;
  }
  return TRUE;
}

gchar *nm_frame_read(int fd, gsize *out_len, NmFrameStatus *out_status) {
  NmFrameStatus st = NM_FRAME_IO;
  gsize len_out = 0;
  gchar *msg = NULL;
  guint8 prefix[4];

  gssize rd = read_full(fd, prefix, sizeof prefix);
  if (rd == 0) {
    st = NM_FRAME_EOF;
    goto out;
  }
  if (rd != (gssize)sizeof prefix) goto out;

  guint32 len = nm_frame_decode_length(prefix);
  len_out = len;
  if (len == 0) {
    st = NM_FRAME_EMPTY;
    goto out;
  }
  if (len > NM_MAX_MESSAGE_SIZE) {
    if (len >= NM_MAX_DRAIN_SIZE) goto out; /* NM_FRAME_IO: give up */
    guint8 sink[4096];
    gsize left = len;
    while (left > 0) {
      gsize chunk = MIN(left, sizeof sink);
      if (read_full(fd, sink, chunk) != (gssize)chunk) goto out;
      left -= chunk;
    }
    st = NM_FRAME_TOO_LARGE;
    goto out;
  }

  msg = g_malloc((gsize)len + 1);
  if (read_full(fd, msg, len) != (gssize)len) {
    g_clear_pointer(&msg, g_free);
    goto out;
  }
  msg[len] = '\0';
  st = NM_FRAME_OK;

out:
  if (out_len) *out_len = len_out;
  if (out_status) *out_status = st;
  return msg;
}

NmFrameStatus nm_frame_write(int fd, const gchar *json, gsize len) {
  g_autoptr(GBytes) frame = nm_frame_encode(json, len);
  if (!frame) return NM_FRAME_TOO_LARGE;
  gsize n = 0;
  const guint8 *data = g_bytes_get_data(frame, &n);
  return write_full(fd, data, n) ? NM_FRAME_OK : NM_FRAME_IO;
}
