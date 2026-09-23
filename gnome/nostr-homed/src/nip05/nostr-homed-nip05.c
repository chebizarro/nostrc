/* nostr-homed-nip05 — unprivileged NIP-05 resolver helper.
 *
 * Runs as a dedicated non-root user (default "nobody"; parent drops
 * privileges before exec). ONLY caller today: the auth broker's
 * BEGIN_LOGIN path when the incoming username contains '@' and
 * validates as a NIP-05 address.
 *
 * Contract (mirrors nostr-homed-profile-image):
 *   nostr-homed-nip05 <local@domain>
 * → writes compact JSON to stdout on success:
 *     {"pubkey":"<64-hex>","relays":["wss://...", ...]}
 * → returns a specific exit code per failure class (see nostr_nip05.h)
 *
 * Enforced entirely inside this binary:
 *   1. Root refused (exit 70). The parent already dropped privs; this
 *      is defence in depth.
 *   2. Address argv re-validated (exit 64) so a compromised parent
 *      cannot dial arbitrary hosts by naming them in argv.
 *   3. libcurl transfer:
 *        - CURLOPT_PROTOCOLS_STR = "https"       (no http, no file, no gopher)
 *        - CURLOPT_REDIR_PROTOCOLS_STR = "https"
 *        - CURLOPT_MAXREDIRS = 3
 *        - CURLOPT_TIMEOUT = 10, CURLOPT_CONNECTTIMEOUT = 5
 *        - CURLOPT_MAXFILESIZE = 64 KiB, plus a hard cap in the
 *          write callback (defence in depth)
 *        - CURLOPT_OPENSOCKETFUNCTION verifies the resolved peer
 *          against nh_profile_ssrf_check_sockaddr — refuses RFC1918
 *          / loopback / link-local / etc. The ONLY way to make this
 *          child dial a private destination is a working SSRF in
 *          libcurl itself.
 *        - CURLOPT_NOPROXY = "*", CURLOPT_COOKIEFILE = ""
 *        - CURLOPT_UNRESTRICTED_AUTH = 0 (no auth on redirect)
 *   4. Response body <= 64 KiB, decoded as JSON (jansson).
 *   5. names[<local>] must be present and 64-hex; that's the pubkey.
 *   6. relays[<pubkey>] copied best-effort (bounded to 4 entries).
 *
 * The fetched body NEVER touches disk. */
#define _GNU_SOURCE
#include "nostr_nip05.h"
#include "nostr_profile.h"

#include <curl/curl.h>
#include <errno.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BODY_CAP (64ull * 1024ull)

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
        size_t nc = b->cap ? b->cap : 4096;
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

/* Same SSRF gate as the profile image helper — see profile_sanitize.c
 * for the exhaustive v4+v6 refused ranges. Sharing the check keeps
 * the two greeter-adjacent fetchers on one policy. */
static curl_socket_t open_socket_cb(void *clientp, curlsocktype purpose,
                                    struct curl_sockaddr *address) {
    (void)clientp; (void)purpose;
    if (nh_profile_ssrf_check_sockaddr((const struct sockaddr *)&address->addr) != 0)
        return CURL_SOCKET_BAD;
    return socket(address->family, address->socktype, address->protocol);
}

static int sockopt_cb(void *clientp, curl_socket_t fd, curlsocktype purpose) {
    (void)clientp; (void)fd; (void)purpose;
    return CURL_SOCKOPT_OK;
}

/* Emit the compact success JSON to stdout. On failure we simply
 * write nothing — the parent uses the exit code as the truth signal.
 * A partial write is treated as failure. */
static int emit_success(const nh_nip05_result *r) {
    json_t *root = json_object();
    json_t *relays = json_array();
    if (!root || !relays) {
        if (root) json_decref(root);
        if (relays) json_decref(relays);
        return -1;
    }
    for (size_t i = 0; i < r->relays_count; i++) {
        json_array_append_new(relays, json_string(r->relays[i]));
    }
    if (json_object_set_new(root, "pubkey", json_string(r->pubkey_hex)) != 0 ||
        json_object_set_new(root, "relays", relays) != 0) {
        json_decref(root);
        return -1;
    }
    char *j = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    if (!j) return -1;
    size_t len = strlen(j);
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(1, j + off, len - off);
        if (w < 0) { if (errno == EINTR) continue; free(j); return -1; }
        off += (size_t)w;
    }
    free(j);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <local@domain>\n", argv[0]);
        return NH_NIP05_ERR_ARG;
    }
    if (getuid() == 0 || geteuid() == 0) {
        fprintf(stderr, "nostr-homed-nip05: refuse to run as root\n");
        return NH_NIP05_ERR_PRIV;
    }
    nh_nip05_address addr;
    if (nh_nip05_parse(argv[1], &addr) != 0) {
        fprintf(stderr, "nostr-homed-nip05: address failed validation\n");
        return NH_NIP05_ERR_ARG;
    }

    char url[NH_NIP05_DOMAIN_MAX + NH_NIP05_LOCAL_MAX * 3 + 64];
    if (nh_nip05_wellknown_url(&addr, url, sizeof url) != 0)
        return NH_NIP05_ERR_ARG;

    body_buf body = {0};
    CURL *c = curl_easy_init();
    if (!c) return NH_NIP05_ERR_INTERNAL;
    int exit_rc = NH_NIP05_ERR_TRANSPORT;

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
    curl_easy_setopt(c, CURLOPT_DNS_CACHE_TIMEOUT, 0L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "nostr-homed-nip05/1 (libcurl)");
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_COOKIEFILE, "");
    curl_easy_setopt(c, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(c, CURLOPT_UNRESTRICTED_AUTH, 0L);
    /* NIP-05 §"CORS" recommends `Access-Control-Allow-Origin: *`; we
     * ask for JSON explicitly so a mis-configured issuer that serves
     * text/html can be flagged distinctly. */
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    if (hdrs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);

    /* Test seam: honour NH_NIP05_TEST_INSECURE=1 to also allow http://
     * (unit tests behind a tiny local HTTP server). Never honoured in
     * production because the parent's minimal envp doesn't set it. */
    const char *insecure = getenv("NH_NIP05_TEST_INSECURE");
    if (insecure && !strcmp(insecure, "1")) {
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
        curl_easy_setopt(c, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
        curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS,
                         CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
        /* And swap the scheme in the URL if the test env asked for
         * http://<domain>. The unit test rewrites the URL explicitly
         * via NH_NIP05_TEST_URL to bypass DNS. */
        const char *override = getenv("NH_NIP05_TEST_URL");
        if (override && *override) curl_easy_setopt(c, CURLOPT_URL, override);
    }

    CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_COULDNT_CONNECT) { exit_rc = NH_NIP05_ERR_SSRF; goto out; }
    if (rc != CURLE_OK) {
        fprintf(stderr, "nostr-homed-nip05: curl: %s\n", curl_easy_strerror(rc));
        exit_rc = NH_NIP05_ERR_TRANSPORT;
        goto out;
    }
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    if (http < 200 || http >= 300) { exit_rc = NH_NIP05_ERR_TRANSPORT; goto out; }
    char *ct = NULL;
    curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct);
    /* Some CDNs mislabel .well-known/nostr.json as text/plain; we
     * accept both text/plain and any content-type substring that
     * contains "json" (application/json, application/vnd.api+json,
     * ...). */
    if (ct && strncasecmp(ct, "text/plain", 10) != 0 &&
        !strcasestr(ct, "json")) {
        exit_rc = NH_NIP05_ERR_CONTENT;
        goto out;
    }
    if (body.len == 0 || body.len > BODY_CAP) {
        exit_rc = NH_NIP05_ERR_TRANSPORT;
        goto out;
    }

    nh_nip05_result result;
    nh_nip05_rc prc = nh_nip05_parse_wellknown((const char *)body.data,
                                                body.len,
                                                addr.local, &result);
    if (prc != NH_NIP05_OK) { exit_rc = (int)prc; goto out; }
    if (emit_success(&result) != 0) { exit_rc = NH_NIP05_ERR_INTERNAL; goto out; }
    exit_rc = NH_NIP05_OK;

out:
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    free(body.data);
    return exit_rc;
}
