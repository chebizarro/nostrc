/* Network guards (no network is used): public-address policy, https-only
 * refusal, NIP-05 domain plausibility and cache, avatar cache pruning and
 * PNG transcoding. */
#include <glib/gstdio.h>
#include <string.h>

#include "nsp-avatar.h"
#include "nsp-http.h"
#include "nsp-nip05.h"
#include "nsp-query.h"
#include "nsp-testutil.h"

#ifdef NSP_HAVE_PIXBUF
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif

static void test_public_addresses(void) {
  struct { const char *a; gboolean pub; } c[] = {
      {"8.8.8.8", TRUE},        {"1.1.1.1", TRUE},         {"10.0.0.1", FALSE},
      {"172.16.5.4", FALSE},    {"172.31.255.255", FALSE}, {"172.32.0.1", TRUE},
      {"192.168.1.1", FALSE},   {"127.0.0.1", FALSE},      {"169.254.1.1", FALSE},
      {"100.64.0.1", FALSE},    {"100.128.0.1", TRUE},     {"0.0.0.0", FALSE},
      {"224.0.0.1", FALSE},     {"255.255.255.255", FALSE}, {"198.18.0.1", FALSE},
      {"2606:4700::1111", TRUE}, {"::1", FALSE},           {"::", FALSE},
      {"fe80::1", FALSE},       {"fd00::1", FALSE},        {"fc00::1", FALSE},
      {"::ffff:10.0.0.1", FALSE}, {"::ffff:8.8.8.8", FALSE}, {"2002:0a00:0001::1", FALSE},
      {"2001:db8::1", FALSE},   {"ff02::1", FALSE},        {"64:ff9b::a00:1", FALSE},
  };
  for (guint i = 0; i < G_N_ELEMENTS(c); i++) {
    g_autoptr(GInetAddress) a = g_inet_address_new_from_string(c[i].a);
    g_assert_nonnull(a);
    if (nsp_inet_address_is_public(a) != c[i].pub) g_error("%s: expected %d", c[i].a, c[i].pub);
  }
  g_assert_false(nsp_inet_address_is_public(NULL));
}

static void on_get(GObject *src, GAsyncResult *res, gpointer user_data) {
  g_autoptr(GError) err = NULL;
  g_assert_null(nsp_http_get_finish(res, NULL, &err));
  g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  *(gboolean *)user_data = TRUE;
}

static void test_https_only(void) {
  g_autoptr(SoupSession) s = nsp_http_session_new(1);
  const char *urls[] = {"http://example.com/a.png", "ftp://x/y", "file:///etc/passwd", "javascript:1"};
  for (guint i = 0; i < G_N_ELEMENTS(urls); i++) {
    gboolean done = FALSE;
    nsp_http_get_async(s, urls[i], NULL, 1024, TRUE, NULL, on_get, &done);
    g_assert_true(nsp_test_wait(&done, 1000));
  }
}

static void test_nip05_domains_and_cache(void) {
  g_assert_true(nsp_nip05_domain_plausible("nos.social"));
  g_assert_true(nsp_nip05_domain_plausible("sub.example.co.uk"));
  const char *bad[] = {"nos", "localhost", "a.local", "x.internal", "1.2.3.4", "a.b1",
                       "a.home.arpa", "router.lan", "", "x.onion", "a."};
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) g_assert_false(nsp_nip05_domain_plausible(bad[i]));

  NspNip05 *r = nsp_nip05_new();
  char pk[65];
  gboolean neg = TRUE;
  g_assert_false(nsp_nip05_lookup_cached(r, "alice@nos.social", pk, &neg));
  g_assert_false(neg);
  g_autofree char *want = nsp_test_pubkey(NSP_TEST_SK_ALICE);
  nsp_nip05_cache_put(r, "alice@nos.social", want);
  g_assert_true(nsp_nip05_lookup_cached(r, "alice@nos.social", pk, &neg));
  g_assert_cmpstr(pk, ==, want);
  nsp_nip05_free(r);
}

static void write_file(const char *path, gsize size, gint64 mtime) {
  g_autofree char *buf = g_malloc0(size);
  g_assert_true(g_file_set_contents(path, buf, (gssize)size, NULL));
  g_autoptr(GFile) f = g_file_new_for_path(path);
  g_assert_true(g_file_set_attribute_uint64(f, G_FILE_ATTRIBUTE_TIME_MODIFIED, (guint64)mtime,
                                            G_FILE_QUERY_INFO_NONE, NULL, NULL));
}

static void test_avatar_cache(void) {
  g_autofree char *dir = g_dir_make_tmp("nsp-avatars-XXXXXX", NULL);
  /* 10 files x 1000 B, oldest first */
  for (int i = 0; i < 10; i++) {
    g_autofree char *p = g_strdup_printf("%s/%02d.png", dir, i);
    write_file(p, 1000, 1700000000 + i);
  }
  g_autofree char *other = g_build_filename(dir, "keep.txt", NULL);
  g_assert_true(g_file_set_contents(other, "x", 1, NULL));
  g_assert_cmpuint(nsp_avatars_prune(dir, 100000, 100), ==, 0); /* under limits */
  g_assert_cmpuint(nsp_avatars_prune(dir, 5000, 100), ==, 6);   /* down to 80 % = 4000 B */
  for (int i = 0; i < 10; i++) {
    g_autofree char *p = g_strdup_printf("%s/%02d.png", dir, i);
    g_assert_cmpint(g_file_test(p, G_FILE_TEST_EXISTS), ==, i >= 6);
  }
  g_assert_true(g_file_test(other, G_FILE_TEST_EXISTS)); /* only *.png pruned */
  g_assert_cmpuint(nsp_avatars_prune(dir, 100000, 2), ==, 3); /* file-count limit */

  /* lookup: cached → GFileIcon; miss with fetching off → NULL, nothing queued */
  NspAvatars *a = nsp_avatars_new(dir, FALSE, NULL);
  const char *url = "https://img.example.com/alice.png";
  g_assert_null(nsp_avatars_lookup(a, url));
  g_assert_cmpuint(nsp_avatars_pending(a), ==, 0);
  g_assert_null(nsp_avatars_lookup(a, "http://img.example.com/alice.png"));
  g_autofree char *path = nsp_avatars_path(a, url);
  g_assert_true(g_str_has_prefix(path, dir) && g_str_has_suffix(path, ".png"));
  g_assert_true(g_file_set_contents(path, "png", 3, NULL));
  g_autoptr(GIcon) icon = nsp_avatars_lookup(a, url);
  g_assert_true(G_IS_FILE_ICON(icon));
  nsp_avatars_free(a);

  g_autoptr(GFile) d = g_file_new_for_path(dir);
  g_autoptr(GFileEnumerator) en = g_file_enumerate_children(d, G_FILE_ATTRIBUTE_STANDARD_NAME, 0, NULL, NULL);
  GFileInfo *fi;
  while ((fi = g_file_enumerator_next_file(en, NULL, NULL))) {
    g_autofree char *p = g_build_filename(dir, g_file_info_get_name(fi), NULL);
    g_unlink(p);
    g_object_unref(fi);
  }
  g_rmdir(dir);
}

static void test_transcode(void) {
#ifdef NSP_HAVE_PIXBUF
  g_autofree char *dir = g_dir_make_tmp("nsp-tx-XXXXXX", NULL);
  g_autofree char *out = g_build_filename(dir, "a.png", NULL);
  g_autoptr(GdkPixbuf) src = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 200, 100);
  gdk_pixbuf_fill(src, 0xff0000ff);
  gchar *png = NULL;
  gsize len = 0;
  g_assert_true(gdk_pixbuf_save_to_buffer(src, &png, &len, "png", NULL, NULL));
  g_autoptr(GBytes) b = g_bytes_new_take(png, len);
  g_autoptr(GError) err = NULL;
  g_assert_true(nsp_avatar_transcode(b, out, &err));
  g_assert_no_error(err);
  g_autoptr(GdkPixbuf) got = gdk_pixbuf_new_from_file(out, &err);
  g_assert_no_error(err);
  g_assert_cmpint(gdk_pixbuf_get_width(got), ==, NSP_AVATAR_SIZE);
  g_assert_cmpint(gdk_pixbuf_get_height(got), ==, NSP_AVATAR_SIZE);

  g_autoptr(GBytes) junk = g_bytes_new_static("<svg onload=alert(1)", 20);
  g_autofree char *out2 = g_build_filename(dir, "b.png", NULL);
  g_assert_false(nsp_avatar_transcode(junk, out2, NULL));
  g_assert_false(g_file_test(out2, G_FILE_TEST_EXISTS));
  g_unlink(out);
  g_rmdir(dir);
#else
  g_test_skip("built without gdk-pixbuf");
#endif
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nsp/net/public-addresses", test_public_addresses);
  g_test_add_func("/nsp/net/https-only", test_https_only);
  g_test_add_func("/nsp/net/nip05", test_nip05_domains_and_cache);
  g_test_add_func("/nsp/net/avatar-cache", test_avatar_cache);
  g_test_add_func("/nsp/net/avatar-transcode", test_transcode);
  return g_test_run();
}
