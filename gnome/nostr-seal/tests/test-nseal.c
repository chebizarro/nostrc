/* test-nseal.c — nostr-seal container tests (nostrc-da9c).
 * SPDX-License-Identifier: MIT
 *
 *   test-nostr-seal         unit matrix (fast)
 *   test-nostr-seal --big   multi-GB streaming by fd on a sparse file;
 *                           size from $NSEAL_TEST_BIG_BYTES (default 3 GiB + 12345)
 */

#include "nostr-seal.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <keys.h>
#include <nostr-utils.h>

#define CHECK(c) do { if (!(c)) { g_printerr("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define CHECK_ERR(err, want) do { \
    if (!(err) || !g_error_matches((err), NSEAL_ERROR, (want))) { \
      g_printerr("FAIL %s:%d: want NsealError %d, got %s (%d)\n", __FILE__, __LINE__, (int)(want), \
                 (err) ? (err)->message : "success", (err) ? (err)->code : -1); exit(1); } \
    g_clear_error(&(err)); } while (0)

static char *g_tmp;

typedef struct { uint8_t sk[32]; uint8_t pk[32]; } Key;

static void key_new(Key *k) {
  char *skh = nostr_key_generate_private();
  char *pkh = nostr_key_get_public(skh);
  CHECK(skh && pkh);
  CHECK(nostr_hex2bin(k->sk, skh, 32));
  CHECK(nostr_hex2bin(k->pk, pkh, 32));
  free(skh); free(pkh);
}

static char *tpath(const char *name) { return g_build_filename(g_tmp, name, NULL); }

static void write_bytes(const char *path, const uint8_t *d, gsize n) {
  CHECK(g_file_set_contents(path, (const char *)d, (gssize)n, NULL));
}

static GBytes *read_bytes(const char *path) {
  gchar *d = NULL; gsize n = 0;
  CHECK(g_file_get_contents(path, &d, &n, NULL));
  return g_bytes_new_take(d, n);
}

static gboolean seal_file(const char *in, const char *out, const NsealEncryptOptions *o, GError **err) {
  int ifd = open(in, O_RDONLY), ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  CHECK(ifd >= 0 && ofd >= 0);
  gboolean ok = nseal_encrypt_fd(ifd, ofd, o, err);
  close(ifd); close(ofd);
  return ok;
}

static gboolean open_file(const char *in, const char *out, const NsealDecryptOptions *o, GError **err) {
  int ifd = open(in, O_RDONLY), ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  CHECK(ifd >= 0 && ofd >= 0);
  gboolean ok = nseal_decrypt_fd(ifd, ofd, o, err);
  close(ifd); close(ofd);
  return ok;
}

static NsealDecryptOptions as_key(Key *k) {
  NsealDecryptOptions o = { .identity_pubkey = k->pk, .unwrap = nseal_unwrap_with_seckey, .unwrap_data = k->sk };
  return o;
}

static uint8_t *pattern(gsize n, guint seed) {
  uint8_t *b = g_malloc(n ? n : 1);
  guint32 x = seed * 2654435761u + 1;
  for (gsize i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b[i] = (uint8_t)x; }
  return b;
}

/* ─── Round trips ────────────────────────────────────────────────────── */

static void test_sizes(void) {
  Key a; key_new(&a);
  const guint log2 = 12;
  const gsize C = (gsize)1 << log2;
  const gsize sizes[] = { 0, 1, C - 1, C, C + 1, 3 * C + 5, 8 * C };
  for (gsize k = 0; k < G_N_ELEMENTS(sizes); k++) {
    gsize n = sizes[k];
    g_autofree uint8_t *data = pattern(n, (guint)k);
    g_autofree char *pin = tpath("sz.in"), *pse = tpath("sz.nsealed"), *pout = tpath("sz.out");
    write_bytes(pin, data, n);
    const uint8_t (*r)[32] = &a.pk;
    NsealEncryptOptions eo = { .recipients = r, .n_recipients = 1, .chunk_log2 = log2 };
    GError *err = NULL;
    CHECK(seal_file(pin, pse, &eo, &err));
    NsealDecryptOptions dopt = as_key(&a);
    CHECK(open_file(pse, pout, &dopt, &err));
    g_autoptr(GBytes) back = read_bytes(pout);
    CHECK(g_bytes_get_size(back) == n);
    CHECK(n == 0 || memcmp(g_bytes_get_data(back, NULL), data, n) == 0);

    /* Geometry: header + Σ(8 + len + 29) + sealed index + 48-byte footer. */
    g_autoptr(GBytes) sealed = read_bytes(pse);
    gsize chunks = (n + C - 1) / C;
    gsize hdr = 44 + 3 + 32 + 32 + 132;
    gsize idx = 29 + 60 + 32 * chunks;
    CHECK(g_bytes_get_size(sealed) == hdr + n + chunks * 37 + idx + 48);
  }
  g_print("ok sizes (0, 1, C-1, exactly one chunk, C+1, 3C+5, 8C)\n");
}

static void test_default_chunk(void) {
  Key a; key_new(&a);
  gsize n = ((gsize)1 << NSEAL_CHUNK_LOG2_DEFAULT);  /* exactly one default chunk */
  g_autofree uint8_t *data = pattern(n, 7);
  g_autofree char *pin = tpath("dc.in"), *pse = tpath("dc.nsealed"), *pout = tpath("dc.out");
  write_bytes(pin, data, n);
  const uint8_t (*r)[32] = &a.pk;
  NsealEncryptOptions eo = { .recipients = r, .n_recipients = 1 };
  GError *err = NULL;
  CHECK(seal_file(pin, pse, &eo, &err));
  int fd = open(pse, O_RDONLY);
  g_autoptr(NsealHeader) h = nseal_header_read_fd(fd, &err);
  close(fd);
  CHECK(h && nseal_header_chunk_log2(h) == NSEAL_CHUNK_LOG2_DEFAULT);
  NsealDecryptOptions dopt = as_key(&a);
  CHECK(open_file(pse, pout, &dopt, &err));
  g_autoptr(GBytes) back = read_bytes(pout);
  CHECK(g_bytes_get_size(back) == n && memcmp(g_bytes_get_data(back, NULL), data, n) == 0);
  g_print("ok exactly one default (1 MiB) chunk\n");
}

static void test_multi_recipient_and_wrong(void) {
  Key k[3], outsider; for (int i = 0; i < 3; i++) key_new(&k[i]);
  key_new(&outsider);
  uint8_t r[3][32];
  for (int i = 0; i < 3; i++) memcpy(r[i], k[i].pk, 32);
  gsize n = 10000;
  g_autofree uint8_t *data = pattern(n, 3);
  g_autofree char *pin = tpath("mr.in"), *pse = tpath("mr.nsealed"), *pout = tpath("mr.out");
  write_bytes(pin, data, n);
  NsealEncryptOptions eo = { .recipients = (const uint8_t (*)[32])r, .n_recipients = 3, .chunk_log2 = 12 };
  GError *err = NULL;
  CHECK(seal_file(pin, pse, &eo, &err));

  int fd = open(pse, O_RDONLY);
  g_autoptr(NsealHeader) h = nseal_header_read_fd(fd, &err);
  close(fd);
  CHECK(h && nseal_header_n_stanzas(h) == 3 && !nseal_header_is_passphrase(h));
  for (int i = 0; i < 3; i++) {
    CHECK(nseal_header_has_recipient(h, k[i].pk));
    CHECK(memcmp(nseal_header_stanza_recipient(h, (gsize)i), k[i].pk, 32) == 0);
  }
  CHECK(!nseal_header_has_recipient(h, outsider.pk));

  for (int i = 0; i < 3; i++) {
    NsealDecryptOptions dopt = as_key(&k[i]);
    CHECK(open_file(pse, pout, &dopt, &err));
    g_autoptr(GBytes) back = read_bytes(pout);
    CHECK(g_bytes_get_size(back) == n && memcmp(g_bytes_get_data(back, NULL), data, n) == 0);
  }

  /* Not listed at all. */
  NsealDecryptOptions dopt = as_key(&outsider);
  CHECK(!open_file(pse, pout, &dopt, &err));
  CHECK_ERR(err, NSEAL_ERROR_NOT_A_RECIPIENT);

  /* Claims to be recipient 0 but holds the outsider's secret key. */
  NsealDecryptOptions liar = { .identity_pubkey = k[0].pk, .unwrap = nseal_unwrap_with_seckey,
                               .unwrap_data = outsider.sk };
  CHECK(!open_file(pse, pout, &liar, &err));
  CHECK_ERR(err, NSEAL_ERROR_UNWRAP);

  /* No identity and no passphrase. */
  NsealDecryptOptions none = {0};
  CHECK(!open_file(pse, pout, &none, &err));
  CHECK_ERR(err, NSEAL_ERROR_NOT_A_RECIPIENT);

  /* Duplicate recipient / no recipient / both lanes are refused. */
  uint8_t dup[2][32]; memcpy(dup[0], k[0].pk, 32); memcpy(dup[1], k[0].pk, 32);
  NsealEncryptOptions bad = { .recipients = (const uint8_t (*)[32])dup, .n_recipients = 2 };
  CHECK(!seal_file(pin, pse, &bad, &err)); CHECK_ERR(err, NSEAL_ERROR_ARG);
  NsealEncryptOptions empty = {0};
  CHECK(!seal_file(pin, pse, &empty, &err)); CHECK_ERR(err, NSEAL_ERROR_ARG);
  NsealEncryptOptions both = { .recipients = (const uint8_t (*)[32])r, .n_recipients = 1, .passphrase = "x" };
  CHECK(!seal_file(pin, pse, &both, &err)); CHECK_ERR(err, NSEAL_ERROR_ARG);
  /* Off-curve recipient (x = 5). */
  uint8_t off[1][32] = {{0}}; off[0][31] = 5;
  NsealEncryptOptions offc = { .recipients = (const uint8_t (*)[32])off, .n_recipients = 1 };
  CHECK(!seal_file(pin, pse, &offc, &err)); CHECK_ERR(err, NSEAL_ERROR_ARG);
  g_print("ok multi-recipient (3), wrong-recipient, wrong-key, bad options\n");
}

/* ─── Tamper matrix ──────────────────────────────────────────────────── */

/* MUT_B64: replace a base64 char with a different valid one, so the strict
 * header parser accepts it and the MAC / header hash has to catch it. */
typedef enum { MUT_NONE, MUT_FLIP, MUT_TRUNC, MUT_APPEND, MUT_SWAP, MUT_B64 } MutKind;

static void expect_tamper(const char *label, const uint8_t *sealed, gsize len, MutKind kind,
                          gsize off, gsize off2, gsize swap_len, Key *who, gint want_code,
                          gboolean expect_empty_output) {
  g_autofree char *p = tpath("tamper.nsealed"), *pout = tpath("tamper.out");
  GByteArray *m = g_byte_array_new();
  g_byte_array_append(m, sealed, (guint)len);
  switch (kind) {
    case MUT_NONE: break;
    case MUT_FLIP: m->data[off] ^= 0x01; break;
    case MUT_B64: m->data[off] = m->data[off] == 'A' ? 'B' : 'A'; break;
    case MUT_TRUNC: g_byte_array_set_size(m, (guint)(len - off)); break;
    case MUT_APPEND: { uint8_t z = 0; g_byte_array_append(m, &z, 1); break; }
    case MUT_SWAP: {
      uint8_t *tmp = g_malloc(swap_len);
      memcpy(tmp, m->data + off, swap_len);
      memmove(m->data + off, m->data + off2, swap_len);
      memcpy(m->data + off2, tmp, swap_len);
      g_free(tmp);
      break;
    }
  }
  write_bytes(p, m->data, m->len);
  g_byte_array_unref(m);
  GError *err = NULL;
  NsealDecryptOptions dopt = as_key(who);
  gboolean ok = open_file(p, pout, &dopt, &err);
  if (ok) { g_printerr("FAIL tamper '%s' was accepted\n", label); exit(1); }
  if (want_code >= 0 && !g_error_matches(err, NSEAL_ERROR, want_code)) {
    g_printerr("FAIL tamper '%s': want code %d, got %d: %s\n", label, want_code, err->code, err->message);
    exit(1);
  }
  if (expect_empty_output) {
    struct stat st; CHECK(g_stat(pout, &st) == 0);
    if (st.st_size != 0) { g_printerr("FAIL tamper '%s' leaked %lld bytes\n", label, (long long)st.st_size); exit(1); }
  }
  g_clear_error(&err);
}

static void test_tamper(void) {
  Key a, b; key_new(&a); key_new(&b);
  uint8_t r[2][32]; memcpy(r[0], a.pk, 32); memcpy(r[1], b.pk, 32);
  const gsize C = 4096, n = 3 * C;       /* three full chunks, same length */
  g_autofree uint8_t *data = pattern(n, 11);
  g_autofree char *pin = tpath("t.in"), *pse = tpath("t.nsealed");
  write_bytes(pin, data, n);
  NsealEncryptOptions eo = { .recipients = (const uint8_t (*)[32])r, .n_recipients = 2, .chunk_log2 = 12 };
  GError *err = NULL;
  CHECK(seal_file(pin, pse, &eo, &err));
  g_autoptr(GBytes) sb = read_bytes(pse);
  gsize len = 0; const uint8_t *s = g_bytes_get_data(sb, &len);

  const gsize st = 3 + 32 + 32 + 132;
  const gsize hdr = 44 + 2 * st;
  const gsize clen = 8 + C + 29;
  const gsize body_end = hdr + 3 * clen;

  /* Header: magic, version, flags, chunk size, stanza count. */
  expect_tamper("magic", s, len, MUT_FLIP, 0, 0, 0, &a, NSEAL_ERROR_FORMAT, TRUE);
  expect_tamper("version", s, len, MUT_FLIP, 7, 0, 0, &a, NSEAL_ERROR_UNSUPPORTED, TRUE);
  expect_tamper("flags", s, len, MUT_FLIP, 8, 0, 0, &a, NSEAL_ERROR_UNSUPPORTED, TRUE);
  expect_tamper("chunk_log2", s, len, MUT_FLIP, 9, 0, 0, &a, -1, TRUE);
  expect_tamper("stanza count", s, len, MUT_FLIP, 11, 0, 0, &a, -1, TRUE);
  expect_tamper("key commitment", s, len, MUT_FLIP, 12 + 5, 0, 0, &a, NSEAL_ERROR_INTEGRITY, TRUE);
  /* My own stanza's payload → the NIP-44 MAC fails. */
  expect_tamper("own stanza payload", s, len, MUT_B64, 44 + 3 + 64 + 40, 0, 0, &a, NSEAL_ERROR_UNWRAP, TRUE);
  /* Someone else's stanza → my unwrap works, the index's header hash doesn't. */
  expect_tamper("other stanza", s, len, MUT_B64, 44 + st + 3 + 64 + 40, 0, 0, &a, NSEAL_ERROR_INTEGRITY, TRUE);
  /* A non-base64 byte in a stanza is a malformed header. */
  {
    GByteArray *m = g_byte_array_new();
    g_byte_array_append(m, s, (guint)len);
    m->data[44 + 3 + 64 + 40] = '*';
    expect_tamper("stanza non-b64", m->data, m->len, MUT_NONE, 0, 0, 0, &a, NSEAL_ERROR_FORMAT, TRUE);
    g_byte_array_unref(m);
  }
  /* Body. */
  expect_tamper("chunk 0 byte", s, len, MUT_FLIP, hdr + 100, 0, 0, &a, NSEAL_ERROR_INTEGRITY, TRUE);
  expect_tamper("chunk 0 tag", s, len, MUT_FLIP, hdr + clen - 1, 0, 0, &a, NSEAL_ERROR_INTEGRITY, TRUE);
  expect_tamper("chunk 2 byte", s, len, MUT_FLIP, hdr + 2 * clen + 50, 0, 0, &a, NSEAL_ERROR_INTEGRITY, FALSE);
  expect_tamper("swap chunks 0/1", s, len, MUT_SWAP, hdr, hdr + clen, clen, &a, NSEAL_ERROR_INTEGRITY, TRUE);
  /* Index, footer, length. */
  expect_tamper("index byte", s, len, MUT_FLIP, body_end + 40, 0, 0, &a, NSEAL_ERROR_INTEGRITY, TRUE);
  expect_tamper("footer length", s, len, MUT_FLIP, len - 48 + 7, 0, 0, &a, NSEAL_ERROR_INTEGRITY, TRUE);
  expect_tamper("footer address", s, len, MUT_FLIP, len - 48 + 8, 0, 0, &a, NSEAL_ERROR_INTEGRITY, TRUE);
  expect_tamper("footer magic", s, len, MUT_FLIP, len - 1, 0, 0, &a, NSEAL_ERROR_INTEGRITY, TRUE);
  expect_tamper("truncate 1", s, len, MUT_TRUNC, 1, 0, 0, &a, NSEAL_ERROR_INTEGRITY, TRUE);
  expect_tamper("truncate chunk", s, len, MUT_TRUNC, len - hdr - clen, 0, 0, &a, NSEAL_ERROR_INTEGRITY, TRUE);
  expect_tamper("append 1", s, len, MUT_APPEND, 0, 0, 0, &a, NSEAL_ERROR_INTEGRITY, TRUE);
  /* Drop the last chunk and splice the original index back on: the index
   * names three chunks, the body has two. */
  {
    GByteArray *m = g_byte_array_new();
    g_byte_array_append(m, s, (guint)(hdr + 2 * clen));
    g_byte_array_append(m, s + body_end, (guint)(len - body_end));
    expect_tamper("drop last chunk", m->data, m->len, MUT_NONE, 0, 0, 0, &a, NSEAL_ERROR_INTEGRITY, TRUE);
    g_byte_array_unref(m);
  }

  /* Transplant: A's stanza from another file (different file key, valid
   * NIP-44 wrap for A) into this header — A unwraps fine, the key
   * commitment does not match. Models a sender giving recipients
   * different keys. */
  {
    g_autofree char *pse2 = tpath("t2.nsealed");
    CHECK(seal_file(pin, pse2, &eo, &err));
    g_autoptr(GBytes) sb2 = read_bytes(pse2);
    const uint8_t *s2 = g_bytes_get_data(sb2, NULL);
    GByteArray *m = g_byte_array_new();
    g_byte_array_append(m, s, (guint)len);
    memcpy(m->data + 44, s2 + 44, st);
    expect_tamper("transplanted stanza", m->data, m->len, MUT_NONE, 0, 0, 0, &a, NSEAL_ERROR_INTEGRITY, TRUE);
    /* B still opens the (modified) file? No: the header hash changed too. */
    expect_tamper("transplanted stanza (other)", m->data, m->len, MUT_NONE, 0, 0, 0, &b, NSEAL_ERROR_INTEGRITY, TRUE);
    g_byte_array_unref(m);
  }

  /* Identical plaintext chunks must not produce identical sealed chunks
   * (the chunk index is framed into each porthome plaintext). */
  {
    g_autofree uint8_t *zeros = g_malloc0(3 * C);
    g_autofree char *zin = tpath("z.in"), *zse = tpath("z.nsealed");
    write_bytes(zin, zeros, 3 * C);
    const uint8_t (*one)[32] = &a.pk;
    NsealEncryptOptions zo = { .recipients = one, .n_recipients = 1, .chunk_log2 = 12 };
    CHECK(seal_file(zin, zse, &zo, &err));
    g_autoptr(GBytes) zb = read_bytes(zse);
    const uint8_t *z = g_bytes_get_data(zb, NULL);
    const gsize zh = 44 + st;
    CHECK(memcmp(z + zh, z + zh + clen, clen) != 0);
    CHECK(memcmp(z + zh + clen, z + zh + 2 * clen, clen) != 0);
  }
  g_print("ok tamper matrix (header, stanzas, chunks, swap, index, footer, truncation, append)\n");
}

/* ─── Passphrase ─────────────────────────────────────────────────────── */

static void test_passphrase(void) {
  gsize n = 5000;
  g_autofree uint8_t *data = pattern(n, 5);
  g_autofree char *pin = tpath("pp.in"), *pse = tpath("pp.nsealed"), *pout = tpath("pp.out");
  write_bytes(pin, data, n);
  /* Composed é (U+00E9); decrypt below with the decomposed e + U+0301. */
  NsealEncryptOptions eo = { .passphrase = "correct horse caf\xc3\xa9", .chunk_log2 = 12 };
  GError *err = NULL;
  CHECK(seal_file(pin, pse, &eo, &err));
  int fd = open(pse, O_RDONLY);
  g_autoptr(NsealHeader) h = nseal_header_read_fd(fd, &err);
  close(fd);
  CHECK(h && nseal_header_is_passphrase(h) && nseal_header_stanza_recipient(h, 0) == NULL);

  NsealDecryptOptions dopt = { .passphrase = "correct horse cafe\xcc\x81" };
  CHECK(open_file(pse, pout, &dopt, &err));
  g_autoptr(GBytes) back = read_bytes(pout);
  CHECK(g_bytes_get_size(back) == n && memcmp(g_bytes_get_data(back, NULL), data, n) == 0);

  NsealDecryptOptions wrong = { .passphrase = "correct horse" };
  CHECK(!open_file(pse, pout, &wrong, &err));
  CHECK_ERR(err, NSEAL_ERROR_UNWRAP);

  Key a; key_new(&a);
  NsealDecryptOptions as_npub = as_key(&a);
  CHECK(!open_file(pse, pout, &as_npub, &err));
  CHECK_ERR(err, NSEAL_ERROR_NOT_A_RECIPIENT);

  /* A hostile work factor is refused before scrypt runs. */
  NsealDecryptOptions capped = { .passphrase = "correct horse caf\xc3\xa9", .max_log_n = 15 };
  CHECK(!open_file(pse, pout, &capped, &err));
  CHECK_ERR(err, NSEAL_ERROR_UNSUPPORTED);

  /* Tampering the ncryptsec stanza breaks bech32 → malformed header. */
  g_autoptr(GBytes) sb = read_bytes(pse);
  gsize len = 0; const uint8_t *s = g_bytes_get_data(sb, &len);
  expect_tamper("ncryptsec", s, len, MUT_FLIP, 44 + 3 + 40, 0, 0, &a, NSEAL_ERROR_FORMAT, TRUE);

  NsealEncryptOptions weak = { .passphrase = "x", .log_n = 12 };
  CHECK(!seal_file(pin, pse, &weak, &err)); CHECK_ERR(err, NSEAL_ERROR_ARG);
  g_print("ok passphrase round-trip (NFKC), wrong passphrase, work-factor cap\n");
}

/* ─── Pipe input ─────────────────────────────────────────────────────── */

typedef struct { int fd; const uint8_t *d; gsize n; } Feed;
static void *feed_thread(void *p) {
  Feed *f = p;
  gsize off = 0;
  while (off < f->n) {  /* dribble in odd-sized writes */
    gsize w = MIN((gsize)777, f->n - off);
    ssize_t k = write(f->fd, f->d + off, w);
    if (k <= 0) break;
    off += (gsize)k;
  }
  close(f->fd);
  return NULL;
}

static void test_pipe_input(void) {
  Key a; key_new(&a);
  gsize n = 5 * 4096 + 123;
  g_autofree uint8_t *data = pattern(n, 9);
  int p[2]; CHECK(pipe(p) == 0);
  Feed f = { p[1], data, n };
  pthread_t t; CHECK(pthread_create(&t, NULL, feed_thread, &f) == 0);
  g_autofree char *pse = tpath("pipe.nsealed"), *pout = tpath("pipe.out");
  int ofd = open(pse, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  const uint8_t (*r)[32] = &a.pk;
  NsealEncryptOptions eo = { .recipients = r, .n_recipients = 1, .chunk_log2 = 12 };
  GError *err = NULL;
  CHECK(nseal_encrypt_fd(p[0], ofd, &eo, &err));
  close(ofd); close(p[0]);
  pthread_join(t, NULL);
  NsealDecryptOptions dopt = as_key(&a);
  CHECK(open_file(pse, pout, &dopt, &err));
  g_autoptr(GBytes) back = read_bytes(pout);
  CHECK(g_bytes_get_size(back) == n && memcmp(g_bytes_get_data(back, NULL), data, n) == 0);

  /* Decrypt refuses a non-seekable input. */
  int q[2]; CHECK(pipe(q) == 0);
  g_autoptr(GBytes) sealed = read_bytes(pse);
  Feed f2 = { q[1], g_bytes_get_data(sealed, NULL), g_bytes_get_size(sealed) };
  pthread_t t2; CHECK(pthread_create(&t2, NULL, feed_thread, &f2) == 0);
  int nul = open("/dev/null", O_WRONLY);
  CHECK(!nseal_decrypt_fd(q[0], nul, &dopt, &err));
  g_clear_error(&err);
  close(q[0]); close(nul);
  pthread_join(t2, NULL);
  g_print("ok pipe (non-seekable) input; decrypt refuses a pipe\n");
}

/* ─── Helpers ────────────────────────────────────────────────────────── */

static void test_pubkey_parse(void) {
  Key a; key_new(&a);
  g_autofree char *npub = nseal_pubkey_to_npub(a.pk);
  CHECK(npub && g_str_has_prefix(npub, "npub1"));
  uint8_t back[32]; GError *err = NULL;
  CHECK(nseal_parse_pubkey(npub, back, &err) && memcmp(back, a.pk, 32) == 0);
  g_autofree char *uri = g_strconcat("nostr:", npub, NULL);
  CHECK(nseal_parse_pubkey(uri, back, &err) && memcmp(back, a.pk, 32) == 0);
  char hex[65];
  for (int i = 0; i < 32; i++) g_snprintf(hex + 2 * i, 3, "%02X", a.pk[i]);
  CHECK(nseal_parse_pubkey(hex, back, &err) && memcmp(back, a.pk, 32) == 0);
  CHECK(!nseal_parse_pubkey("npub1notreally", back, &err)); CHECK_ERR(err, NSEAL_ERROR_ARG);
  CHECK(!nseal_parse_pubkey("0000000000000000000000000000000000000000000000000000000000000005", back, &err));
  CHECK_ERR(err, NSEAL_ERROR_ARG);
  g_print("ok npub / nostr: / hex parsing, off-curve rejected\n");
}

/* ─── Multi-GB streaming ─────────────────────────────────────────────── */

typedef struct { int fd; guint64 count; gboolean nonzero; } Drain;
static void *drain_thread(void *p) {
  Drain *d = p;
  static uint8_t buf[1 << 16];
  for (;;) {
    ssize_t k = read(d->fd, buf, sizeof buf);
    if (k <= 0) break;
    for (ssize_t i = 0; i < k; i++) if (buf[i]) { d->nonzero = TRUE; break; }
    d->count += (guint64)k;
  }
  return NULL;
}

static long peak_rss_mib(void) {
  struct rusage ru; getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
  return (long)(ru.ru_maxrss / (1024 * 1024));
#else
  return (long)(ru.ru_maxrss / 1024);
#endif
}

static void test_big(void) {
  const char *env = g_getenv("NSEAL_TEST_BIG_BYTES");
  guint64 n = env ? g_ascii_strtoull(env, NULL, 10) : ((guint64)3 << 30) + 12345;
  Key a; key_new(&a);
  g_autofree char *pin = tpath("big.sparse"), *pse = tpath("big.nsealed");
  int ifd = open(pin, O_RDWR | O_CREAT | O_TRUNC, 0600);
  CHECK(ifd >= 0 && ftruncate(ifd, (off_t)n) == 0);
  CHECK(lseek(ifd, 0, SEEK_SET) == 0);
  int ofd = open(pse, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  const uint8_t (*r)[32] = &a.pk;
  NsealEncryptOptions eo = { .recipients = r, .n_recipients = 1 };
  GError *err = NULL;
  gint64 t0 = g_get_monotonic_time();
  if (!nseal_encrypt_fd(ifd, ofd, &eo, &err)) { g_printerr("big encrypt: %s\n", err->message); exit(1); }
  close(ifd); close(ofd);
  gint64 t1 = g_get_monotonic_time();
  struct stat st; CHECK(g_stat(pse, &st) == 0);
  guint64 chunks = (n + (1u << 20) - 1) >> 20;
  CHECK((guint64)st.st_size == 44 + 199 + n + chunks * 37 + 29 + 60 + 32 * chunks + 48);

  int p[2]; CHECK(pipe(p) == 0);
  Drain d = { p[0], 0, FALSE };
  pthread_t t; CHECK(pthread_create(&t, NULL, drain_thread, &d) == 0);
  int sfd = open(pse, O_RDONLY);
  NsealDecryptOptions dopt = as_key(&a);
  gboolean ok = nseal_decrypt_fd(sfd, p[1], &dopt, &err);
  close(sfd); close(p[1]);
  pthread_join(t, NULL);
  close(p[0]);
  if (!ok) { g_printerr("big decrypt: %s\n", err->message); exit(1); }
  gint64 t2 = g_get_monotonic_time();
  CHECK(d.count == n && !d.nonzero);
  long rss = peak_rss_mib();
  g_print("ok big: %" G_GUINT64_FORMAT " bytes (%" G_GUINT64_FORMAT " chunks) sealed %.1fs, opened %.1fs, peak RSS %ld MiB\n",
          n, chunks, (double)(t1 - t0) / 1e6, (double)(t2 - t1) / 1e6, rss);
  CHECK(rss < 256);
  g_unlink(pse);
  g_unlink(pin);
}

static void rm_rf(const char *dir) {
  GDir *d = g_dir_open(dir, 0, NULL);
  if (d) {
    const char *name;
    while ((name = g_dir_read_name(d))) { g_autofree char *p = g_build_filename(dir, name, NULL); g_unlink(p); }
    g_dir_close(d);
  }
  g_rmdir(dir);
}

int main(int argc, char **argv) {
  /* The pipe tests close a read end while a feeder thread may still be
   * writing; that must surface as EPIPE in the feeder, not kill the test. */
  signal(SIGPIPE, SIG_IGN);
  const char *base = g_getenv("NSEAL_TEST_TMPDIR");
  g_autofree char *tmpl = g_build_filename(base ? base : g_get_tmp_dir(), "nseal-testXXXXXX", NULL);
  g_tmp = g_mkdtemp(tmpl);
  CHECK(g_tmp != NULL);
  if (argc > 1 && g_str_equal(argv[1], "--big")) {
    test_big();
  } else {
    test_pubkey_parse();
    test_sizes();
    test_default_chunk();
    test_multi_recipient_and_wrong();
    test_tamper();
    test_passphrase();
    test_pipe_input();
  }
  rm_rf(g_tmp);
  g_print("test-nostr-seal: PASS\n");
  return 0;
}
