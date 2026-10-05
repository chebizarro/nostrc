/* test_nm_framing — native-messaging frame round-trip over a pipe
 * (nostrc-jjyp). */
#include "native_messaging.h"

#include <string.h>
#include <unistd.h>

static void make_pipe(int fds[2]) { g_assert_cmpint(pipe(fds), ==, 0); }

static void write_raw(int fd, const void *buf, gsize n) {
  g_assert_cmpint(write(fd, buf, n), ==, (gssize)n);
}

static void test_encode_le_prefix(void) {
  const gchar *json = "{\"id\":\"1\"}";
  g_autoptr(GBytes) b = nm_frame_encode(json, strlen(json));
  gsize n = 0;
  const guint8 *d = g_bytes_get_data(b, &n);
  g_assert_cmpuint(n, ==, strlen(json) + 4);
  g_assert_cmpuint(d[0], ==, strlen(json));
  g_assert_cmpuint(d[1], ==, 0);
  g_assert_cmpuint(d[2], ==, 0);
  g_assert_cmpuint(d[3], ==, 0);
  g_assert_cmpmem(d + 4, n - 4, json, strlen(json));

  const guint8 big[4] = { 0x01, 0x02, 0x03, 0x04 };
  g_assert_cmpuint(nm_frame_decode_length(big), ==, 0x04030201u);
}

static void test_encode_limit(void) {
  g_assert_null(nm_frame_encode("x", NM_MAX_MESSAGE_SIZE + 1));
  gchar *max = g_malloc(NM_MAX_MESSAGE_SIZE);
  memset(max, 'a', NM_MAX_MESSAGE_SIZE);
  g_autoptr(GBytes) b = nm_frame_encode(max, NM_MAX_MESSAGE_SIZE);
  g_assert_nonnull(b);
  g_free(max);
}

static void test_round_trip(void) {
  int fds[2];
  make_pipe(fds);
  const gchar *msgs[] = { "{\"a\":1}", "{\"method\":\"getPublicKey\",\"id\":\"x\"}", "[]" };
  for (gsize i = 0; i < G_N_ELEMENTS(msgs); i++)
    g_assert_cmpint(nm_frame_write(fds[1], msgs[i], strlen(msgs[i])), ==, NM_FRAME_OK);
  close(fds[1]);
  for (gsize i = 0; i < G_N_ELEMENTS(msgs); i++) {
    gsize len = 0;
    NmFrameStatus st;
    g_autofree gchar *m = nm_frame_read(fds[0], &len, &st);
    g_assert_cmpint(st, ==, NM_FRAME_OK);
    g_assert_cmpuint(len, ==, strlen(msgs[i]));
    g_assert_cmpstr(m, ==, msgs[i]);
  }
  NmFrameStatus st;
  g_assert_null(nm_frame_read(fds[0], NULL, &st));
  g_assert_cmpint(st, ==, NM_FRAME_EOF);
  close(fds[0]);
}

typedef struct { int fd; guint8 *buf; gsize n; } Feed;

static gpointer feeder(gpointer data) {
  Feed *f = data;
  gsize off = 0;
  while (off < f->n) {
    gssize w = write(f->fd, f->buf + off, f->n - off);
    if (w <= 0) break;
    off += (gsize)w;
  }
  close(f->fd);
  return NULL;
}

/* An oversized frame is drained so the next frame is still readable. */
static void test_oversized_is_drained(void) {
  int fds[2];
  make_pipe(fds);
  guint32 big = NM_MAX_MESSAGE_SIZE + 10;
  gsize total = 4 + (gsize)big + 4 + 2;
  guint8 *buf = g_malloc0(total);
  buf[0] = big & 0xFF; buf[1] = (big >> 8) & 0xFF; buf[2] = (big >> 16) & 0xFF; buf[3] = (big >> 24) & 0xFF;
  guint8 *tail = buf + 4 + big;
  tail[0] = 2; tail[4] = '{'; tail[5] = '}';

  /* Pipe capacity is far below 1 MiB: feed from a writer thread. */
  Feed f = { fds[1], buf, total };
  GThread *w = g_thread_new("feeder", feeder, &f);

  gsize len = 0;
  NmFrameStatus st;
  gchar *m = nm_frame_read(fds[0], &len, &st);
  g_assert_null(m);
  g_assert_cmpint(st, ==, NM_FRAME_TOO_LARGE);
  g_assert_cmpuint(len, ==, big);

  m = nm_frame_read(fds[0], &len, &st);
  g_assert_cmpint(st, ==, NM_FRAME_OK);
  g_assert_cmpstr(m, ==, "{}");
  g_free(m);

  g_thread_join(w);
  close(fds[0]);
  g_free(buf);
}

/* A length prefix beyond the drain ceiling is treated as a broken peer. */
static void test_absurd_length_is_fatal(void) {
  int fds[2];
  make_pipe(fds);
  const guint8 prefix[4] = { 0xff, 0xff, 0xff, 0x7f };
  write_raw(fds[1], prefix, 4);
  close(fds[1]);
  NmFrameStatus st;
  g_assert_null(nm_frame_read(fds[0], NULL, &st));
  g_assert_cmpint(st, ==, NM_FRAME_IO);
  close(fds[0]);
}

static void test_empty_and_truncated(void) {
  int fds[2];
  make_pipe(fds);
  const guint8 zero[4] = { 0, 0, 0, 0 };
  write_raw(fds[1], zero, 4);
  const guint8 ten[4] = { 10, 0, 0, 0 };
  write_raw(fds[1], ten, 4);
  write_raw(fds[1], "{\"a\"", 4); /* 4 of 10 promised bytes */
  close(fds[1]);

  NmFrameStatus st;
  g_assert_null(nm_frame_read(fds[0], NULL, &st));
  g_assert_cmpint(st, ==, NM_FRAME_EMPTY);
  g_assert_null(nm_frame_read(fds[0], NULL, &st));
  g_assert_cmpint(st, ==, NM_FRAME_IO);
  close(fds[0]);
}

static void test_truncated_prefix(void) {
  int fds[2];
  make_pipe(fds);
  write_raw(fds[1], "\x05\x00", 2);
  close(fds[1]);
  NmFrameStatus st;
  g_assert_null(nm_frame_read(fds[0], NULL, &st));
  g_assert_cmpint(st, ==, NM_FRAME_IO);
  close(fds[0]);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nmh/framing/encode-le-prefix", test_encode_le_prefix);
  g_test_add_func("/nmh/framing/encode-limit", test_encode_limit);
  g_test_add_func("/nmh/framing/round-trip", test_round_trip);
  g_test_add_func("/nmh/framing/oversized-drained", test_oversized_is_drained);
  g_test_add_func("/nmh/framing/absurd-length", test_absurd_length_is_fatal);
  g_test_add_func("/nmh/framing/empty-and-truncated", test_empty_and_truncated);
  g_test_add_func("/nmh/framing/truncated-prefix", test_truncated_prefix);
  return g_test_run();
}
