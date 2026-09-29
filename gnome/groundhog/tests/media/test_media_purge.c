/* nostrc-5x5b (privacy charter §3.7, §6, P3; G21 follow-up): a downloaded
 * attachment's decrypted plaintext (the encrypted store's media table,
 * gh-store-media.h) goes with the message that named it, in the same
 * transaction, whichever way the message goes: the G07 expiry purge, the
 * retention purge, forget conversation, block and forget, and outbox delete.
 * A file another remaining message also names stays. A download that
 * finishes after its message went keeps nothing.
 *
 * After each deletion the H7 canary scanner (tests/privacy/canary-scan.c)
 * reads the store's files raw (store.db, -wal, -shm) and then every page of
 * store.db and every frame of its -wal decrypted here, with the store's key,
 * exactly as they are on disk: live and free pages and old WAL frames alike
 * (SQLCipher 4 defaults: 4096-byte pages, AES-256-CBC, the IV in the
 * 80-byte reserved tail, page 1's first 16 bytes the salt). Page 1 must
 * decrypt to a valid SQLite header, and the files that survive must be found
 * in the decrypted pages (the positive controls), so "not found" means gone:
 * secure_delete zeroed the freed pages and the checkpoint emptied the WAL.
 * Nothing sleeps: time is a GhClock. */
#include "gh-nip17-file.h"
#include "gh-store-conversations.h"
#include "gh-store-media.h"
#include "canary-scan.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <nostr-keys.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define T0 G_GINT64_CONSTANT(1760000000)
#define DAY (24 * 60 * 60)

static gchar *root;
static gchar *run_id;
static gchar *alice, *bob, *carol, *dave; /* alice is the account */

static gchar *
new_pubkey(void)
{
  char *secret = nostr_key_generate_private();
  char *pubkey = nostr_key_get_public(secret);
  gchar *out = g_strdup(pubkey);
  free(secret);
  free(pubkey);
  return out;
}

static gchar *
room_of(const gchar *a, const gchar *b)
{
  return strcmp(a, b) < 0 ? g_strconcat(a, ",", b, NULL) : g_strconcat(b, ",", a, NULL);
}

static gchar *
x_of(const gchar *label)
{
  g_autofree gchar *seed = g_strdup_printf("%s/%s", run_id, label);
  return g_compute_checksum_for_string(G_CHECKSUM_SHA256, seed, -1);
}

/* A unique plaintext marker for label's file. */
static gchar *
canary_of(const gchar *label)
{
  return g_strdup_printf("G21-CACHED-PLAINTEXT-%s-%s", run_id, label);
}

/* ---- the store ------------------------------------------------------------------- */

typedef struct {
  gchar *dir;
  guint8 key[32];     /* the store key: the test decrypts the files with it */
  GhClock *clock;
  GhStore *store;
  GhStoreConversations *conversations;
} Fixture;

static void
fixture_up(Fixture *f, const gchar *name)
{
  memset(f, 0, sizeof *f);
  f->dir = g_build_filename(root, name, NULL);
  g_assert_cmpint(g_mkdir_with_parents(f->dir, 0700), ==, 0);
  f->clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  for (guint i = 0; i < sizeof f->key; i++)
    f->key[i] = (guint8)g_random_int();
  g_autoptr(GBytes) key = g_bytes_new(f->key, sizeof f->key);
  GhStoreConfig config = { .data_dir = f->dir, .account_pubkey = alice, .clock = f->clock };
  g_autofree gchar *store_id = g_uuid_string_random();
  g_autoptr(GError) error = NULL;
  f->store = gh_store_open_with_key(&config, key, store_id, GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error);
  f->conversations = gh_store_conversations_new(f->store);
}

static void
fixture_down(Fixture *f)
{
  gh_store_conversations_close(f->conversations);
  g_clear_object(&f->conversations);
  gh_store_close(f->store);
  g_assert_true(gh_store_delete_files(f->dir, alice, NULL));
  gh_clock_unref(f->clock);
  g_free(f->dir);
}

static GhNip17File *
file_of(const gchar *label)
{
  GhNip17File *file = g_new0(GhNip17File, 1);
  g_autofree gchar *x = x_of(label);
  file->url = g_strdup_printf("https://blossom.example.com/%s", x);
  file->file_type = g_strdup("image/jpeg");
  file->nonce_size = GH_NIP17_FILE_NONCE_SIZE;
  for (guint i = 0; i < sizeof file->key; i++)
    file->key[i] = (guint8)g_random_int();
  g_strlcpy(file->x, x, sizeof file->x);
  return file;
}

/* A kind-15 message from sender to the account naming label's file, as the
 * inbox admits it. */
static void
admit_file(Fixture *f, const gchar *sender, const gchar *label, gint64 created_at,
           gint64 received_at, gint64 expires_at)
{
  g_autoptr(GhNip17File) file = file_of(label);
  g_autofree gchar *id = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *rumor = gh_nip17_file_rumor_new(sender, alice, file, created_at, expires_at,
                                                    &id, &error);
  g_assert_no_error(error);
  g_autofree gchar *room = room_of(sender, alice);
  g_autofree gchar *wrap = g_compute_checksum_for_string(G_CHECKSUM_SHA256, id, -1);
  const gchar *participants[] = { sender, alice, NULL };
  GhStoreMessage message = {
    .backend = GH_STORE_BACKEND_NIP17,
    .backend_key = room,
    .backend_msg_id = id,
    .wrap_id = wrap,
    .sender_pubkey = sender,
    .kind = GH_NIP17_FILE_KIND,
    .created_at = created_at,
    .received_at = received_at,
    .direction = GH_STORE_DIRECTION_IN,
    .body = file->url,
    .raw_json = rumor,
    .expires_at = expires_at,
    .participants = participants,
    .unread = TRUE,
    .request_state = GH_STORE_REQUEST_ACCEPTED,
  };
  GhStoreAdmitResult result = GH_STORE_ADMIT_EXPIRED;
  g_assert_true(gh_store_admit(f->store, &message, &result, NULL, &error));
  g_assert_no_error(error);
  g_assert_cmpint(result, ==, GH_STORE_ADMIT_STORED);
}

/* The account's own kind-15 message to recipient, queued in the outbox. */
static gint64
send_file(Fixture *f, const gchar *recipient, const gchar *label)
{
  g_autoptr(GhNip17File) file = file_of(label);
  g_autofree gchar *id = NULL;
  g_autoptr(GError) error = NULL;
  const gint64 now = gh_clock_get_unix(f->clock);
  g_autofree gchar *rumor = gh_nip17_file_rumor_new(alice, recipient, file, now, 0, &id, &error);
  g_assert_no_error(error);
  g_autofree gchar *room = room_of(alice, recipient);
  gint64 conversation = 0, outbox = 0;
  g_assert_true(gh_store_ensure_conversation(f->store, GH_STORE_BACKEND_NIP17, room,
                                             GH_STORE_REQUEST_ACCEPTED, &conversation, &error));
  g_autofree gchar *op_id = gh_store_new_op_id();
  GhStoreOutgoing outgoing = {
    .conversation_id = conversation,
    .op_id = op_id,
    .backend_msg_id = id,
    .sender_pubkey = alice,
    .kind = GH_NIP17_FILE_KIND,
    .created_at = now,
    .body = file->url,
    .rumor_json = rumor,
  };
  g_assert_true(gh_store_enqueue(f->store, &outgoing, &outbox, NULL, &error));
  g_assert_no_error(error);
  return outbox;
}

/* What the user's Download keeps: label's plaintext, several pages long so
 * it spans overflow pages, its canary on every page. */
static void
cache_file(Fixture *f, const gchar *label)
{
  g_autofree gchar *canary = canary_of(label);
  GString *plain = g_string_new(NULL);
  while (plain->len < 20 * 1024)
    g_string_append_printf(plain, "%s|%0960u|", canary, (guint)plain->len);
  g_autoptr(GBytes) bytes = g_bytes_new_take(g_strdup(plain->str), plain->len);
  g_string_free(plain, TRUE);
  g_autofree gchar *x = x_of(label);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_media_put(f->store, x, "image/jpeg", bytes, &error));
  g_assert_no_error(error);
}

static gboolean
cached(Fixture *f, const gchar *label)
{
  g_autofree gchar *x = x_of(label);
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = gh_store_media_get(f->store, x, NULL, &error);
  g_assert_no_error(error);
  return bytes != NULL;
}

static gint64
wal_size(Fixture *f)
{
  g_autofree gchar *wal = g_strconcat(gh_store_get_path(f->store), "-wal", NULL);
  GStatBuf st;
  return g_stat(wal, &st) == 0 ? (gint64)st.st_size : 0;
}

#define PAGE_SIZE 4096
#define PAGE_RESERVE 80 /* SQLCipher 4: 16-byte IV + 64-byte HMAC-SHA512 */
#define WAL_HEADER 32
#define WAL_FRAME_HEADER 24

static guint32
be32(const guint8 *p)
{
  return ((guint32)p[0] << 24) | ((guint32)p[1] << 16) | ((guint32)p[2] << 8) | p[3];
}

/* Decrypts every page of path (store.db, or with wal the frames of its WAL)
 * into scan; returns how many pages it read (0: no such file). */
static guint
scan_decrypted_file(Fixture *f, CanaryScan *scan, const gchar *path, gboolean wal)
{
  gchar *contents = NULL;
  gsize length = 0;
  if (!g_file_get_contents(path, &contents, &length, NULL))
    return 0;
  const gsize header = wal ? WAL_HEADER : 0, frame = wal ? WAL_FRAME_HEADER : 0;
  guint pages = 0;
  for (gsize at = header; at + frame + PAGE_SIZE <= length; at += frame + PAGE_SIZE) {
    const guint8 *page = (const guint8 *)contents + at + frame;
    guint32 pgno = wal ? be32((const guint8 *)contents + at) : (guint32)(at / PAGE_SIZE) + 1;
    const gsize start = pgno == 1 ? 16 : 0; /* page 1 begins with the plaintext salt */
    guint8 out[PAGE_SIZE] = { 0 };
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int n = 0, last = 0;
    g_assert_cmpint(EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, f->key,
                                       page + PAGE_SIZE - PAGE_RESERVE), ==, 1);
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    g_assert_cmpint(EVP_DecryptUpdate(ctx, out + start, &n, page + start,
                                      (int)(PAGE_SIZE - PAGE_RESERVE - start)), ==, 1);
    g_assert_cmpint(EVP_DecryptFinal_ex(ctx, out + start + n, &last), ==, 1);
    EVP_CIPHER_CTX_free(ctx);
    if (pgno == 1) {
      /* The decryption is right: a SQLite header with 4096-byte pages and
       * SQLCipher's 80 reserved bytes. */
      g_assert_cmpuint(((guint)out[16] << 8) | out[17], ==, PAGE_SIZE);
      g_assert_cmpuint(out[20], ==, PAGE_RESERVE);
    }
    g_autofree gchar *source = g_strdup_printf("%s page %u (decrypted)", wal ? "-wal" : "store.db",
                                               pgno);
    canary_scan_bytes(scan, source, out, PAGE_SIZE - PAGE_RESERVE);
    pages++;
  }
  g_free(contents);
  return pages;
}

/* store.db and its WAL, decrypted. */
static void
scan_decrypted(Fixture *f, CanaryScan *scan)
{
  const gchar *path = gh_store_get_path(f->store);
  g_assert_cmpuint(scan_decrypted_file(f, scan, path, FALSE), >, 0);
  g_autofree gchar *wal = g_strconcat(path, "-wal", NULL);
  scan_decrypted_file(f, scan, wal, TRUE);
}

/* H7: none of gone's plaintext anywhere (raw store files; decrypted pages),
 * and each of kept's still in the decrypted pages (control). */
static void
assert_plaintext(Fixture *f, const gchar *what, const gchar *const *gone,
                 const gchar *const *kept)
{
  CanaryScan *scan = canary_scan_new();
  for (guint i = 0; gone && gone[i]; i++) {
    g_autofree gchar *canary = canary_of(gone[i]);
    canary_scan_add(scan, gone[i], canary);
  }
  for (guint i = 0; kept && kept[i]; i++) {
    g_autofree gchar *canary = canary_of(kept[i]);
    canary_scan_add(scan, kept[i], canary);
  }
  /* The files on disk: SQLCipher's output, so no canary at all. */
  guint files = 0;
  canary_scan_tree(scan, f->dir, &files);
  g_assert_cmpuint(files, >, 0);
  g_autofree gchar *where = g_strdup_printf("%s: store files", what);
  g_assert_true(canary_scan_check_clean(scan, where));
  canary_scan_free(scan);

  /* Decrypted, every page and WAL frame on disk: the gone ones nowhere, the
   * kept ones present. */
  CanaryScan *gone_scan = canary_scan_new();
  for (guint i = 0; gone && gone[i]; i++) {
    g_autofree gchar *canary = canary_of(gone[i]);
    canary_scan_add_literal(gone_scan, gone[i], canary);
  }
  scan_decrypted(f, gone_scan);
  g_autofree gchar *pages = g_strdup_printf("%s: decrypted pages", what);
  g_assert_true(canary_scan_check_clean(gone_scan, pages));
  canary_scan_free(gone_scan);
  for (guint i = 0; kept && kept[i]; i++) {
    CanaryScan *control = canary_scan_new();
    g_autofree gchar *canary = canary_of(kept[i]);
    canary_scan_add_literal(control, kept[i], canary);
    scan_decrypted(f, control);
    if (canary_scan_get_hits(control)->len == 0)
      g_error("%s: the control %s is not in the decrypted pages", what, kept[i]);
    canary_scan_free(control);
  }
}

/* ---- tests ----------------------------------------------------------------------- */

/* G07 expiry: a disappearing photo's cached plaintext goes with it, in the
 * purge's transaction, and leaves the WAL at the purge's checkpoint; a
 * permanent message's file and a file a permanent message also names
 * stay. A download that finishes afterwards keeps nothing. */
static void
test_expiry_purge(void)
{
  Fixture f;
  fixture_up(&f, "expiry");
  admit_file(&f, bob, "expiring", T0 - 60, T0 - 60, T0 + 10);
  admit_file(&f, bob, "permanent", T0 - 50, T0 - 50, 0);
  admit_file(&f, bob, "forwarded", T0 - 40, T0 - 40, T0 + 10);
  admit_file(&f, carol, "forwarded", T0 - 30, T0 - 30, 0); /* the same file, kept by Carol's */
  cache_file(&f, "expiring");
  cache_file(&f, "permanent");
  cache_file(&f, "forwarded");

  /* Nothing expired yet: nothing goes. */
  GhStorePurgeStats stats;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_conversations_purge(f.conversations, 0, &stats, NULL, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(stats.n_media, ==, 0);
  g_assert_true(cached(&f, "expiring"));

  gh_clock_fake_advance(f.clock, 15 * G_USEC_PER_SEC);
  g_auto(GStrv) purged = NULL;
  g_assert_true(gh_store_conversations_purge(f.conversations, 0, &stats, &purged, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(stats.n_expired, ==, 2);
  g_assert_cmpuint(stats.n_media, ==, 1);
  g_assert_true(stats.checkpointed);
  g_assert_cmpint(wal_size(&f), ==, 0);
  g_assert_false(cached(&f, "expiring"));
  g_assert_true(cached(&f, "permanent"));
  g_assert_true(cached(&f, "forwarded"));
  const gchar *gone[] = { "expiring", NULL }, *kept[] = { "permanent", "forwarded", NULL };
  assert_plaintext(&f, "expiry purge", gone, kept);

  /* The Download that was still running when it expired keeps nothing. */
  g_autofree gchar *x = x_of("expiring");
  g_autoptr(GBytes) late = g_bytes_new_static("late plaintext bytes", 20);
  g_assert_false(gh_store_media_put(f.store, x, NULL, late, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND);
  g_assert_false(cached(&f, "expiring"));
  fixture_down(&f);
}

/* Retention ("Keep Messages: 30 Days"): the same, by received_at. */
static void
test_retention_purge(void)
{
  Fixture f;
  fixture_up(&f, "retention");
  admit_file(&f, bob, "old", T0 - 40 * DAY, T0 - 40 * DAY, 0);
  admit_file(&f, bob, "recent", T0 - DAY, T0 - DAY, 0);
  cache_file(&f, "old");
  cache_file(&f, "recent");
  GhStorePurgeStats stats;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_purge(f.store, T0 - 30 * DAY, &stats, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(stats.n_retention, ==, 1);
  g_assert_cmpuint(stats.n_media, ==, 1);
  g_assert_true(stats.checkpointed);
  g_assert_cmpint(wal_size(&f), ==, 0);
  g_assert_false(cached(&f, "old"));
  g_assert_true(cached(&f, "recent"));
  const gchar *gone[] = { "old", NULL }, *kept[] = { "recent", NULL };
  assert_plaintext(&f, "retention purge", gone, kept);
  fixture_down(&f);
}

/* Forget conversation: its files' plaintext goes; another room's stays. */
static void
test_forget(void)
{
  Fixture f;
  fixture_up(&f, "forget");
  admit_file(&f, carol, "carols", T0 - 60, T0 - 60, 0);
  admit_file(&f, bob, "bobs", T0 - 50, T0 - 50, 0);
  cache_file(&f, "carols");
  cache_file(&f, "bobs");
  g_autofree gchar *room = room_of(carol, alice);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_conversations_forget(f.conversations, room, &error));
  g_assert_no_error(error);
  g_assert_cmpint(wal_size(&f), ==, 0);
  g_assert_false(cached(&f, "carols"));
  g_assert_true(cached(&f, "bobs"));
  const gchar *gone[] = { "carols", NULL }, *kept[] = { "bobs", NULL };
  assert_plaintext(&f, "forget", gone, kept);
  fixture_down(&f);
}

/* Block and forget (Message Requests): the same. */
static void
test_block_and_forget(void)
{
  Fixture f;
  fixture_up(&f, "block");
  admit_file(&f, dave, "request", T0 - 60, T0 - 60, 0);
  admit_file(&f, bob, "bobs", T0 - 50, T0 - 50, 0);
  cache_file(&f, "request");
  cache_file(&f, "bobs");
  g_autofree gchar *room = room_of(dave, alice);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_conversations_block_and_forget(f.conversations, room, &error));
  g_assert_no_error(error);
  gboolean blocked = FALSE;
  g_assert_true(gh_store_conversations_is_blocked(f.conversations, room, &blocked, &error));
  g_assert_true(blocked);
  g_assert_cmpint(wal_size(&f), ==, 0);
  g_assert_false(cached(&f, "request"));
  g_assert_true(cached(&f, "bobs"));
  const gchar *gone[] = { "request", NULL }, *kept[] = { "bobs", NULL };
  assert_plaintext(&f, "block and forget", gone, kept);
  fixture_down(&f);
}

/* Deleting an unsettled outgoing file message: its cached copy goes. */
static void
test_outbox_delete(void)
{
  Fixture f;
  fixture_up(&f, "outbox");
  gint64 outbox = send_file(&f, bob, "sent");
  admit_file(&f, bob, "received", T0 - 50, T0 - 50, 0);
  cache_file(&f, "sent");
  cache_file(&f, "received");
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_outbox_delete(f.store, outbox, &error));
  g_assert_no_error(error);
  g_assert_cmpint(wal_size(&f), ==, 0);
  g_assert_false(cached(&f, "sent"));
  g_assert_true(cached(&f, "received"));
  const gchar *gone[] = { "sent", NULL }, *kept[] = { "received", NULL };
  assert_plaintext(&f, "outbox delete", gone, kept);
  fixture_down(&f);
}

/* Control for the page scan: with secure_delete switched off on this
 * connection, a forgotten file's row goes but its bytes stay in the freed
 * pages, and the decrypted scan finds them there. So the scan reads free
 * pages, and the store's secure_delete (ST-5) is what clears them. */
static void
test_secure_delete_control(void)
{
  Fixture f;
  fixture_up(&f, "control");
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_exec(f.store, "PRAGMA secure_delete=OFF", &error));
  admit_file(&f, carol, "unzeroed", T0 - 60, T0 - 60, 0);
  cache_file(&f, "unzeroed");
  g_autofree gchar *room = room_of(carol, alice);
  g_assert_true(gh_store_conversations_forget(f.conversations, room, &error));
  g_assert_no_error(error);
  g_assert_false(cached(&f, "unzeroed"));
  g_assert_cmpint(wal_size(&f), ==, 0);
  CanaryScan *scan = canary_scan_new();
  g_autofree gchar *canary = canary_of("unzeroed");
  canary_scan_add_literal(scan, "unzeroed", canary);
  scan_decrypted(&f, scan);
  g_assert_cmpuint(canary_scan_get_hits(scan)->len, >, 0);
  canary_scan_free(scan);
  fixture_down(&f);
}

/* The cache holds only what a stored message names. */
static void
test_put_needs_message(void)
{
  Fixture f;
  fixture_up(&f, "orphan");
  g_autofree gchar *x = x_of("nobody");
  g_autoptr(GBytes) bytes = g_bytes_new_static("orphan plaintext", 16);
  g_autoptr(GError) error = NULL;
  g_assert_false(gh_store_media_put(f.store, x, NULL, bytes, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND);
  g_clear_error(&error);
  g_assert_false(cached(&f, "nobody"));
  /* A kind-14 message, even one with an x tag of that hash, names no file. */
  g_autofree gchar *room = room_of(bob, alice);
  g_autofree gchar *text = g_strdup_printf("see %s", x);
  g_autofree gchar *raw = g_strdup_printf("{\"kind\":14,\"tags\":[[\"x\",\"%s\"]],"
                                          "\"content\":\"hi\"}", x);
  g_autofree gchar *id = g_compute_checksum_for_string(G_CHECKSUM_SHA256, text, -1);
  g_autofree gchar *wrap = g_compute_checksum_for_string(G_CHECKSUM_SHA256, raw, -1);
  const gchar *participants[] = { bob, alice, NULL };
  GhStoreMessage message = {
    .backend = GH_STORE_BACKEND_NIP17, .backend_key = room, .backend_msg_id = id,
    .wrap_id = wrap, .sender_pubkey = bob, .kind = 14, .created_at = T0 - 5,
    .direction = GH_STORE_DIRECTION_IN, .body = text, .raw_json = raw,
    .participants = participants,
  };
  g_assert_true(gh_store_admit(f.store, &message, NULL, NULL, &error));
  g_assert_false(gh_store_media_put(f.store, x, NULL, bytes, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND);
  fixture_down(&f);
}

/* ---- main ------------------------------------------------------------------------ */

static void
remove_tree(const gchar *path)
{
  GDir *dir = g_dir_open(path, 0, NULL);
  if (dir) {
    const gchar *name;
    while ((name = g_dir_read_name(dir))) {
      g_autofree gchar *child = g_build_filename(path, name, NULL);
      if (g_file_test(child, G_FILE_TEST_IS_DIR) && !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
        remove_tree(child);
      else
        g_unlink(child);
    }
    g_dir_close(dir);
  }
  g_rmdir(path);
}

int
main(int argc, char **argv)
{
  const gchar *base = g_getenv("TMPDIR");
  gchar *template = g_build_filename(base && *base ? base : "/tmp",
                                     "groundhog-media-purge-XXXXXX", NULL);
  if (!mkdtemp(template))
    g_error("cannot create the test root: %s", g_strerror(errno));
  char *real = realpath(template, NULL);
  g_assert_nonnull(real);
  root = g_strdup(real);
  free(real);
  g_free(template);
  g_test_init(&argc, &argv, NULL);
  g_autofree gchar *uuid = g_uuid_string_random();
  run_id = g_strndup(uuid, 8);
  alice = new_pubkey();
  bob = new_pubkey();
  carol = new_pubkey();
  dave = new_pubkey();
  g_test_add_func("/groundhog/media-purge/expiry", test_expiry_purge);
  g_test_add_func("/groundhog/media-purge/retention", test_retention_purge);
  g_test_add_func("/groundhog/media-purge/forget", test_forget);
  g_test_add_func("/groundhog/media-purge/block-and-forget", test_block_and_forget);
  g_test_add_func("/groundhog/media-purge/outbox-delete", test_outbox_delete);
  g_test_add_func("/groundhog/media-purge/put-needs-message", test_put_needs_message);
  g_test_add_func("/groundhog/media-purge/secure-delete-control", test_secure_delete_control);
  int status = g_test_run();
  remove_tree(root);
  g_free(root);
  g_free(run_id);
  g_free(alice);
  g_free(bob);
  g_free(carol);
  g_free(dave);
  return status;
}
