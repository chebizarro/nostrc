/* W17 review B1 regression (the reviewer's /tmp/w17scratch/repro_cache.c as
 * a test): the decrypted-attachment cache must be bound to the file (x, key
 * and nonce), never to x alone.
 *
 * Bob sends the account a disappearing file, which the account downloads, so
 * it is cached. Mallory knows only its x (she runs the Blossom server that
 * holds it, and serves the real ciphertext) and sends the account a kind-15
 * message with that x, a key of her own and no ox. Her message must:
 *  - miss the cache: Download goes to the network (so "no request" never
 *    tells her the account has the file) and fails as damaged, since her key
 *    does not decrypt it, never returning Bob's plaintext;
 *  - not keep Bob's plaintext once Bob's message expires.
 * A message with Bob's key but another nonce misses too.
 *
 * Only APIs that predate the fix are used, so the same test built against
 * the code before it fails. Downloads run in Tor mode through the SOCKS5
 * fixture (H3) to a .onion served by the local Blossom fixture, as a Tor
 * user's would. Nothing sleeps: waits spin the main context; time is a
 * fake GhClock. */
#include "gh-attachment.h"
#include "gh-store-media.h"
#include "blossom-fixture.h"
#include "socks5-fixture.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <nostr-keys.h>
#include <stdlib.h>
#include <string.h>

#define ONION "groundhogtestcachebindqw4yvkpvrbwe5wtamgzhhdrhm5k2ww7m3tgdwbz.onion"
#define T0 G_GINT64_CONSTANT(1760000000)
#define DAY (24 * 60 * 60)

static gchar *alice, *bob, *mallory;

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

/* A kind-15 message from sender to the account carrying file. */
static void
admit(GhStore *store, const gchar *sender, const GhNip17File *file, gint64 created_at,
      gint64 expires_at)
{
  g_autofree gchar *id = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *rumor = gh_nip17_file_rumor_new(sender, alice, file, created_at, expires_at,
                                                    &id, &error);
  g_assert_no_error(error);
  g_autofree gchar *room = strcmp(alice, sender) < 0 ? g_strconcat(alice, ",", sender, NULL)
                                                     : g_strconcat(sender, ",", alice, NULL);
  g_autofree gchar *wrap = g_compute_checksum_for_string(G_CHECKSUM_SHA256, id, -1);
  const gchar *participants[] = { alice, sender, NULL };
  GhStoreMessage message = {
    .backend = GH_STORE_BACKEND_NIP17, .backend_key = room, .backend_msg_id = id,
    .wrap_id = wrap, .sender_pubkey = sender, .kind = GH_NIP17_FILE_KIND,
    .created_at = created_at, .direction = GH_STORE_DIRECTION_IN, .body = file->url,
    .raw_json = rumor, .participants = participants, .expires_at = expires_at,
  };
  g_assert_true(gh_store_admit(store, &message, NULL, NULL, &error));
  g_assert_no_error(error);
}

typedef struct {
  gboolean done;
  GBytes *bytes;
  gboolean cached;
  GError *error;
} Result;

static gboolean
done(gpointer data)
{
  return ((Result *)data)->done;
}

static void
on_download(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  Result *r = data;
  r->bytes = gh_attachment_download_finish(result, &r->cached, &r->error);
  r->done = TRUE;
}

static void
download(GhBlossomClient *client, GhStore *store, const GhNip17File *file, Result *r)
{
  g_clear_pointer(&r->bytes, g_bytes_unref);
  g_clear_error(&r->error);
  memset(r, 0, sizeof *r);
  gh_attachment_download_async(client, store, file, NULL, on_download, r);
  while (!done(r))
    g_main_context_iteration(NULL, TRUE);
}

static gint64
media_total(GhStore *store)
{
  gint64 total = -1;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_media_get_total(store, &total, &error));
  g_assert_no_error(error);
  return total;
}

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

static void
test_same_x_other_key(void)
{
  /* The account's encrypted store, on a fake clock. */
  g_autofree gchar *dir = g_dir_make_tmp("groundhog-cache-binding-XXXXXX", NULL);
  g_assert_nonnull(dir);
  guint8 raw[32];
  for (guint i = 0; i < sizeof raw; i++)
    raw[i] = (guint8)g_random_int();
  g_autoptr(GBytes) store_key = g_bytes_new(raw, sizeof raw);
  GhClock *clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  GhStoreConfig config = { .data_dir = dir, .account_pubkey = alice, .clock = clock };
  g_autofree gchar *store_id = g_uuid_string_random();
  g_autoptr(GError) error = NULL;
  GhStore *store = gh_store_open_with_key(&config, store_key, store_id, GH_STORE_OPEN_CREATE,
                                          &error);
  g_assert_no_error(error);

  /* Tor mode: the SOCKS5 fixture in front of the Blossom fixture. */
  BlossomFixture *blossom = blossom_fixture_new();
  Socks5Fixture *socks = socks5_fixture_new();
  socks5_fixture_set_domain_port(socks, blossom_fixture_port(blossom));
  GSettings *settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "network-mode", "tor");
  g_settings_set_string(settings, "tor-socks-address", socks5_fixture_address(socks));
  GhNetHttp *http = gh_net_http_new(settings);
  GhBlossomClient *client = gh_blossom_client_new(settings, http);

  /* Bob's private file, on the Blossom server; his message disappears after
   * a day. */
  g_autofree gchar *uuid = g_uuid_string_random();
  g_autofree gchar *secret = g_strdup_printf("BOBS-PRIVATE-PHOTO-PLAINTEXT-%s", uuid);
  g_autoptr(GBytes) photo = g_bytes_new(secret, strlen(secret));
  g_autoptr(GhAttachmentPrepared) prepared =
    gh_attachment_prepare(photo, "application/pdf", 1 << 20, &error);
  g_assert_no_error(error);
  g_autoptr(GhAttachmentSealed) sealed = gh_attachment_encrypt(prepared, &error);
  g_assert_no_error(error);
  blossom_fixture_put_blob(blossom, sealed->file->x, sealed->ciphertext);
  g_autoptr(GhNip17File) bobs = gh_nip17_file_copy(sealed->file);
  bobs->url = g_strdup_printf("http://" ONION ":%u/%s", blossom_fixture_port(blossom), bobs->x);
  admit(store, bob, bobs, T0 - 10, T0 + DAY);

  /* The account downloads it: cached; a second Download needs no request. */
  Result r = { 0 };
  download(client, store, bobs, &r);
  g_assert_no_error(r.error);
  g_assert_true(g_bytes_equal(r.bytes, photo));
  g_assert_false(r.cached);
  download(client, store, bobs, &r);
  g_assert_no_error(r.error);
  g_assert_true(r.cached);
  g_assert_cmpuint(blossom_fixture_count(blossom, "GET"), ==, 1);

  /* Mallory knows only x: her key, no ox, her server's (real) ciphertext. */
  g_autoptr(GhNip17File) forged = g_new0(GhNip17File, 1);
  forged->url = g_strdup(bobs->url);
  forged->file_type = g_strdup("image/jpeg");
  forged->nonce_size = GH_NIP17_FILE_NONCE_SIZE;
  memset(forged->key, 0x42, sizeof forged->key);
  g_strlcpy(forged->x, bobs->x, sizeof forged->x);
  admit(store, mallory, forged, T0 - 5, 0);

  /* Control: her key does not open the file. */
  g_autoptr(GError) control = NULL;
  g_assert_null(gh_attachment_decrypt(forged, sealed->ciphertext, &control));
  g_assert_error(control, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_DAMAGED);

  /* Her message's Download: not Bob's plaintext from the cache, but a
   * request (no presence oracle) and "damaged". */
  download(client, store, forged, &r);
  if (r.bytes && g_bytes_equal(r.bytes, photo))
    g_test_message("B1: Mallory's message was served Bob's plaintext (cached=%d)", r.cached);
  g_assert_null(r.bytes);
  g_assert_false(r.cached);
  g_assert_error(r.error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_DAMAGED);
  g_assert_cmpuint(blossom_fixture_count(blossom, "GET"), ==, 2);

  /* Bob's key with another nonce is another file too. */
  g_autoptr(GhNip17File) renonced = gh_nip17_file_copy(bobs);
  renonced->nonce[0] ^= 0x01;
  download(client, store, renonced, &r);
  g_assert_null(r.bytes);
  g_assert_false(r.cached);
  g_assert_error(r.error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_DAMAGED);
  g_assert_cmpuint(blossom_fixture_count(blossom, "GET"), ==, 3);

  /* Bob's message expires: its plaintext goes, whatever Mallory's names. */
  g_assert_cmpint(media_total(store), >, 0);
  gh_clock_fake_advance(clock, (gint64)2 * DAY * G_USEC_PER_SEC);
  GhStorePurgeStats stats = { 0 };
  g_assert_true(gh_store_purge_full(store, 0, NULL, &stats, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(stats.n_expired, ==, 1);
  g_assert_cmpuint(stats.n_media, ==, 1);
  g_assert_cmpint(media_total(store), ==, 0);
  /* And Mallory's message still gets nothing but "damaged". */
  download(client, store, forged, &r);
  g_assert_null(r.bytes);
  g_assert_error(r.error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_DAMAGED);
  g_clear_error(&r.error);

  g_object_unref(client);
  g_object_unref(http);
  g_settings_reset(settings, "network-mode");
  g_settings_reset(settings, "tor-socks-address");
  g_object_unref(settings);
  while (g_main_context_iteration(NULL, FALSE))
    ;
  socks5_fixture_free(socks);
  blossom_fixture_free(blossom);
  gh_store_close(store);
  gh_clock_unref(clock);
  remove_tree(dir);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  alice = new_pubkey();
  bob = new_pubkey();
  mallory = new_pubkey();
  g_test_add_func("/groundhog/media-cache-binding/same-x-other-key", test_same_x_other_key);
  int status = g_test_run();
  g_free(alice);
  g_free(bob);
  g_free(mallory);
  return status;
}
