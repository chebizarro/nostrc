/* nostr-homed-profile-image — unprivileged image-downloader helper.
 *
 * Runs as a dedicated non-root user (default "nobody"; parent drops
 * privileges before exec). ONLY caller: the profile refresh
 * orchestration (nh_profile_download_image → fork/exec us).
 *
 * Contract:
 *   nostr-homed-profile-image <https-url> <out-png-path>
 *
 * Enforced entirely inside this binary — the parent MUST NOT rely on
 * anything below being validated elsewhere:
 *   1. URL must satisfy nh_profile_validate_picture_url (https-only,
 *      no userinfo, no whitespace/control chars, <= 1 KiB).
 *   2. libcurl transfer:
 *        - CURLOPT_PROTOCOLS_STR = "https"
 *        - CURLOPT_MAXREDIRS = 3 with CURLOPT_FOLLOWLOCATION
 *        - CURLOPT_REDIR_PROTOCOLS_STR = "https"
 *        - CURLOPT_TIMEOUT = 10, CURLOPT_CONNECTTIMEOUT = 5
 *        - CURLOPT_MAXFILESIZE = 2 MiB, plus a hard cap in
 *          the write callback (defence in depth)
 *        - CURLOPT_OPENSOCKETFUNCTION + CURLOPT_SOCKOPTFUNCTION
 *          verify the resolved peer IP against
 *          nh_profile_ssrf_check_sockaddr and refuse RFC1918 /
 *          loopback / link-local / etc. — the ONLY way to make the
 *          child dial a private destination is a working SSRF in
 *          libcurl itself.
 *   3. Response content-type MUST start with "image/".
 *   4. Downloaded bytes are decoded via GdkPixbuf (any format the
 *      installed pixbuf loaders support), scaled to a bounded box
 *      (<= 512x512, aspect-preserving), and written as PNG to
 *      @out-png-path (mode 0644, atomic rename). The raw remote
 *      bytes NEVER touch the destination path.
 *
 * Exit codes:
 *   0   success
 *   64  argument / URL validation refused
 *   65  SSRF check refused peer address
 *   66  HTTP/network failure or size cap exceeded
 *   67  wrong content-type (not image/...)
 *   68  GdkPixbuf could not decode the bytes
 *   69  filesystem write failure
 *   70  privilege check refused (still running as root)
 *   71  internal / OOM
 */
#define _GNU_SOURCE
#include "nostr_profile.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BODY_CAP  (2ull * 1024ull * 1024ull)
#define IMG_BOUND 512

typedef struct {
  unsigned char *data;
  size_t len;
  size_t cap;
} body_buf;

static size_t body_write(void *ptr, size_t sz, size_t nmemb, void *ud) {
  body_buf *b = ud;
  size_t n = sz * nmemb;
  if (b->len + n > BODY_CAP) return 0; /* triggers CURLE_WRITE_ERROR */
  if (b->len + n > b->cap) {
    size_t nc = b->cap ? b->cap : 8192;
    while (nc < b->len + n) nc *= 2;
    if (nc > BODY_CAP) nc = BODY_CAP + 1;
    unsigned char *nd = realloc(b->data, nc);
    if (!nd) return 0;
    b->data = nd;
    b->cap = nc;
  }
  memcpy(b->data + b->len, ptr, n);
  b->len += n;
  return n;
}

/* libcurl socket callbacks: reject any resolved peer that fails the
 * SSRF address check BEFORE any application bytes are sent. */
static curl_socket_t open_socket_cb(void *clientp,
                                    curlsocktype purpose,
                                    struct curl_sockaddr *address) {
  (void)clientp; (void)purpose;
  if (nh_profile_ssrf_check_sockaddr((const struct sockaddr *)&address->addr) != 0) {
    /* Signal a hard failure via CURL_SOCKET_BAD; libcurl surfaces
     * CURLE_COULDNT_CONNECT to us. */
    return CURL_SOCKET_BAD;
  }
  return socket(address->family, address->socktype, address->protocol);
}

static int sockopt_cb(void *clientp, curl_socket_t curlfd,
                      curlsocktype purpose) {
  (void)clientp; (void)curlfd; (void)purpose;
  return CURL_SOCKOPT_OK;
}

static int write_atomic_png(const char *path, const unsigned char *bytes,
                            size_t len) {
  char tmp[1024];
  int n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
  if (n <= 0 || (size_t)n >= sizeof tmp) return -1;
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return -1;
  size_t off = 0;
  while (off < len) {
    ssize_t w = write(fd, bytes + off, len - off);
    if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return -1; }
    off += (size_t)w;
  }
  if (close(fd) != 0) { unlink(tmp); return -1; }
  if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <https-url> <out-png-path>\n", argv[0]);
    return 64;
  }
  const char *url = argv[1];
  const char *out_path = argv[2];

  if (getuid() == 0 || geteuid() == 0) {
    fprintf(stderr, "nostr-homed-profile-image: refuse to run as root\n");
    return 70;
  }

  if (nh_profile_validate_picture_url(url) != 0) {
    fprintf(stderr, "nostr-homed-profile-image: URL refused (scheme/shape)\n");
    return 64;
  }

  body_buf body = {0};
  CURL *c = curl_easy_init();
  if (!c) return 71;
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 3L);
#if LIBCURL_VERSION_NUM >= 0x075500 /* 7.85.0 */
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
  curl_easy_setopt(c, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
  curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(c, CURLOPT_MAXFILESIZE, (long)BODY_CAP);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, body_write);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(c, CURLOPT_OPENSOCKETFUNCTION, open_socket_cb);
  curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, sockopt_cb);
  /* Disable DNS reuse across processes and shorten the resolve timeout
   * so a hostile relay-served hostname can't stall us. */
  curl_easy_setopt(c, CURLOPT_DNS_CACHE_TIMEOUT, 0L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "nostr-homed-profile/1 (libcurl)");
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
  /* No cookies, no proxy env pickup for a login-time helper. */
  curl_easy_setopt(c, CURLOPT_COOKIEFILE, "");
  curl_easy_setopt(c, CURLOPT_NOPROXY, "*");
  /* Belt-and-braces: forbid HTTP auth headers being echoed on redirect. */
  curl_easy_setopt(c, CURLOPT_UNRESTRICTED_AUTH, 0L);

  CURLcode rc = curl_easy_perform(c);
  int exit_rc = 0;
  if (rc == CURLE_COULDNT_CONNECT) {
    /* Most likely our openosocket callback refused an SSRF peer. */
    exit_rc = 65;
    goto out;
  }
  if (rc != CURLE_OK) {
    fprintf(stderr, "nostr-homed-profile-image: curl: %s\n",
            curl_easy_strerror(rc));
    exit_rc = 66;
    goto out;
  }
  long http = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
  if (http < 200 || http >= 300) { exit_rc = 66; goto out; }
  char *ct = NULL;
  curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct);
  if (!ct || strncasecmp(ct, "image/", 6) != 0) { exit_rc = 67; goto out; }
  if (body.len == 0) { exit_rc = 66; goto out; }
  if (body.len > BODY_CAP) { exit_rc = 66; goto out; }

  /* Decode + re-encode via GdkPixbuf. Bounded box, aspect-preserving. */
  GError *gerr = NULL;
  GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
  if (!loader) { exit_rc = 71; goto out; }
  if (!gdk_pixbuf_loader_write(loader, body.data, body.len, &gerr)) {
    g_clear_error(&gerr);
    g_object_unref(loader);
    exit_rc = 68;
    goto out;
  }
  if (!gdk_pixbuf_loader_close(loader, &gerr)) {
    g_clear_error(&gerr);
    g_object_unref(loader);
    exit_rc = 68;
    goto out;
  }
  GdkPixbuf *pb = gdk_pixbuf_loader_get_pixbuf(loader);
  if (!pb) { g_object_unref(loader); exit_rc = 68; goto out; }
  g_object_ref(pb);
  g_object_unref(loader);

  int w = gdk_pixbuf_get_width(pb);
  int h = gdk_pixbuf_get_height(pb);
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
    g_object_unref(pb); exit_rc = 68; goto out;
  }
  GdkPixbuf *scaled = pb;
  if (w > IMG_BOUND || h > IMG_BOUND) {
    double sx = (double)IMG_BOUND / (double)w;
    double sy = (double)IMG_BOUND / (double)h;
    double s = sx < sy ? sx : sy;
    int nw = (int)(w * s); if (nw < 1) nw = 1;
    int nh = (int)(h * s); if (nh < 1) nh = 1;
    scaled = gdk_pixbuf_scale_simple(pb, nw, nh, GDK_INTERP_BILINEAR);
    g_object_unref(pb);
    if (!scaled) { exit_rc = 71; goto out; }
  }

  gchar *png_bytes = NULL;
  gsize png_len = 0;
  if (!gdk_pixbuf_save_to_buffer(scaled, &png_bytes, &png_len, "png", &gerr,
                                 "compression", "6", NULL)) {
    g_clear_error(&gerr);
    g_object_unref(scaled);
    exit_rc = 68;
    goto out;
  }
  g_object_unref(scaled);
  int wrc = write_atomic_png(out_path, (const unsigned char *)png_bytes, png_len);
  g_free(png_bytes);
  if (wrc != 0) { exit_rc = 69; goto out; }
  exit_rc = 0;

out:
  curl_easy_cleanup(c);
  free(body.data);
  return exit_rc;
}
