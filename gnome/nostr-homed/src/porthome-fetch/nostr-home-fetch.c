/*
 * nostr-home-fetch — unprivileged portable-home fetch helper.
 *
 * SPDX-License-Identifier: MIT
 * EXPERIMENTAL. Gated behind NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL.
 * Bead: nostrc-9k4g (h10m Phase 2.5).
 *
 * Contract:
 *   nostr-home-fetch --staging-fd <N> [--allow-insecure]
 *   nostr-home-fetch --staging-dir <path> [--allow-insecure]
 *
 *   * A JSON control payload (see porthome_fetch_ctl.h) is read from
 *     stdin (bounded, refused if larger than 16 KiB).
 *   * A staging fd or directory MUST be provided by the parent broker.
 *     Only one may be given.
 *   * Progress is streamed as one JSON object per line on stdout:
 *       {"bytes":N,"files":K,"phase":"manifest"|"chunk"|"decode"|"done"}\n
 *
 * Steps (design §5, home-from-relay.md):
 *   1. Read + strict-parse the control payload.
 *   2. Open a NostrSimplePool, REQ kind-30078 with #d and author filter,
 *      auto-unsub on EOSE. Verify signature and pubkey match. If no
 *      event arrives before relay_timeout_ms → exit NETWORK_FAIL.
 *   3. Decode the event.content bytes as an nh_porthome sealed CBOR
 *      manifest under home_key. On AEAD failure → DECRYPT_FAIL.
 *      On CBOR schema failure → DECODE_FAIL.
 *   4. Build a fetcher-context around nh_porthome_blossom_t seeded with
 *      the blossom server list. Then materialise into the passed
 *      staging fd via nh_porthome_materialize_into_fd. Progress lines
 *      go out on stdout.
 *   5. On completion emit `{"bytes":TOTAL,"files":F,"phase":"done"}` and
 *      exit 0. Any chunk-level failure that exhausts every server maps
 *      to NETWORK_FAIL (LIMITED) unless it was a hash mismatch (which
 *      is also LIMITED — hostile server, next login retries) except for
 *      an AEAD tag failure on the manifest itself, which is DECRYPT_FAIL.
 *
 * Privilege: this helper is NON-privileged. The parent broker is
 * expected (bead nostrc-ww50) to have dropped privileges before exec,
 * and the helper refuses to run as root.
 */

#define _GNU_SOURCE

#include "porthome_fetch_ctl.h"

#include "nh_porthome_blossom.h"
#include "nh_porthome_crypto.h"
#include "nh_porthome_manifest.h"
#include "nh_porthome_provision.h"

#include <hanami/hanami-blossom-shim.h>

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <openssl/crypto.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

/* libnostr client. */
#include "nostr-simple-pool.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "go.h"

#include <glib.h>

/* ────────────────────────────── progress ────────────────────────── */

static uint64_t g_bytes = 0;
static uint64_t g_files = 0;
static time_t   g_last_emit = 0;
/* Progress fd. In main() we dup() the inherited stdout here, then
 * dup2(/dev/null, STDOUT_FILENO) so any accidental stray printf from
 * dependencies (e.g. libnostr's transport debug lines) is silenced.
 * All progress writes go via g_progress_fd, which is the pipe the
 * broker reads. */
static int g_progress_fd = STDOUT_FILENO;

static void emit_progress(nh_porthome_fetch_phase phase, int force) {
    time_t now = time(NULL);
    if (!force && now == g_last_emit && phase != NH_PORTHOME_FETCH_PHASE_DONE)
        return;
    g_last_emit = now;
    nh_porthome_fetch_progress p = { .bytes = g_bytes,
                                     .files = g_files,
                                     .phase = phase };
    char buf[NH_PORTHOME_FETCH_MAX_PROGRESS_LINE];
    int n = nh_porthome_fetch_progress_format(buf, sizeof buf, &p);
    if (n < 0) return;
    /* Direct write to the saved progress fd so we don't share
     * buffering state with the parent's stdout redirection AND we
     * bypass any stray printf/puts from linked libraries. */
    (void)!write(g_progress_fd, buf, (size_t)n);
    (void)!write(g_progress_fd, "\n", 1);
}

/* ─────────────────────── relay fetch: kind 30078 ─────────────── */

typedef struct {
    const char *author_hex;
    const char *d_tag;
    GoChannel  *ch;      /* delivers a heap-alloc'd (uint8_t*, size_t) pair */
    int         done;
    GMutex      mutex;
} relay_ctx;

typedef struct {
    uint8_t *bytes;
    size_t   len;
} relay_result;

/* Middleware: filter by kind/author/#d, verify signature, extract raw
 * content bytes. */
static void relay_middleware(NostrIncomingEvent *in, void *user_data) {
    relay_ctx *rc = (relay_ctx *)user_data;
    if (!rc || !in || !in->event) return;

    g_mutex_lock(&rc->mutex);
    if (rc->done) { g_mutex_unlock(&rc->mutex); return; }

    if (nostr_event_get_kind(in->event) != 30078) goto out;

    const char *pk = nostr_event_get_pubkey(in->event);
    if (!pk || strcmp(pk, rc->author_hex) != 0) goto out;

    /* d-tag match. */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(in->event);
    int matched = 0;
    if (tags) {
        for (size_t i = 0; i < nostr_tags_size(tags); i++) {
            NostrTag *t = nostr_tags_get(tags, i);
            if (!t || nostr_tag_size(t) < 2) continue;
            if (g_strcmp0(nostr_tag_get_key(t), "d") == 0 &&
                g_strcmp0(nostr_tag_get_value(t), rc->d_tag) == 0) {
                matched = 1; break;
            }
        }
    }
    if (!matched) goto out;

    /* Verify signature. */
    if (!nostr_event_check_signature(in->event)) goto out;

    const char *content = nostr_event_get_content(in->event);
    if (!content) goto out;

    /* content is a base64 or hex string per Phase 1 helpers; here we
     * treat it as a raw byte payload — Phase 2.5 manifests are put on
     * the wire as the AEAD-sealed CBOR bytes rendered as lowercase hex.
     * (Phase 3 will switch to NIP-44-of-sealed for pubkey-anonymity.) */
    size_t clen = strlen(content);
    uint8_t *raw = NULL;
    size_t   raw_len = 0;
    /* Accept two encodings:
     *   - lowercase hex: even length, all [0-9a-f]
     *   - otherwise raw bytes (the event serializer keeps content
     *     verbatim; a publisher that pushes raw ciphertext into a JSON
     *     string will have base64'd it — rejected here, per D-strict). */
    int is_hex = (clen % 2u == 0u);
    for (size_t i = 0; is_hex && i < clen; i++) {
        char c = content[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            is_hex = 0;
    }
    if (!is_hex || clen < 2) goto out;
    raw_len = clen / 2u;
    raw = (uint8_t *)malloc(raw_len);
    if (!raw) goto out;
    for (size_t i = 0; i < raw_len; i++) {
        unsigned hi = content[2*i], lo = content[2*i + 1];
        unsigned dh = hi <= '9' ? hi - '0' : hi - 'a' + 10;
        unsigned dl = lo <= '9' ? lo - '0' : lo - 'a' + 10;
        raw[i] = (uint8_t)((dh << 4) | dl);
    }

    relay_result *res = (relay_result *)calloc(1, sizeof *res);
    if (!res) { free(raw); goto out; }
    res->bytes = raw;
    res->len   = raw_len;

    if (go_channel_try_send(rc->ch, res) == 0) {
        rc->done = 1;
    } else {
        free(res->bytes);
        free(res);
    }

out:
    g_mutex_unlock(&rc->mutex);
}

/* Returns 0 on success (out_bytes/out_len populated, caller frees).
 * -1 on relay/timeout failure. */
static int relay_fetch_manifest(const nh_porthome_fetch_ctl *c,
                                uint8_t **out_bytes, size_t *out_len)
{
    *out_bytes = NULL; *out_len = 0;

    NostrSimplePool *pool = nostr_simple_pool_new();
    if (!pool) return -1;
    nostr_simple_pool_set_auto_unsub_on_eose(pool, true);

    relay_ctx rc = {0};
    rc.author_hex = c->account_pubkey_hex;
    rc.d_tag      = c->d_tag;
    rc.ch         = go_channel_create(1);
    g_mutex_init(&rc.mutex);

    nostr_simple_pool_set_event_middleware_ex(pool, relay_middleware, &rc);
    nostr_simple_pool_start(pool);

    /* Register every relay. */
    const char *urls[NH_PORTHOME_FETCH_MAX_RELAYS];
    size_t n_urls = c->relays_count;
    for (size_t i = 0; i < n_urls; i++) {
        urls[i] = c->relays[i];
        nostr_simple_pool_ensure_relay(pool, urls[i]);
    }

    NostrFilter *f = nostr_filter_new();
    nostr_filter_add_kind(f, 30078);
    nostr_filter_add_author(f, c->account_pubkey_hex);
    nostr_filter_tags_append(f, "d", c->d_tag, NULL);
    nostr_filter_set_limit(f, 1);
    nostr_simple_pool_query_single(pool, urls, n_urls, *f);
    nostr_filter_free(f);

    /* Wait for the first matching event, or timeout. */
    uint32_t tmo_ms = c->relay_timeout_ms ? c->relay_timeout_ms : 10000u;
    relay_result *res = NULL;
    GoSelectCase cas = {
        .op = GO_SELECT_RECEIVE,
        .chan = rc.ch,
        .value = NULL,
        .recv_buf = (void **)&res,
    };
    GoSelectResult sr = go_select_timeout(&cas, 1, tmo_ms);

    /* Cleanup channel + mutex whether or not we got a result. */
    go_channel_close(rc.ch);
    go_channel_unref(rc.ch);
    g_mutex_clear(&rc.mutex);
    /* Pool is left to leak: SimplePool owns background threads that will
     * be reaped by process exit. This is deliberately equivalent to how
     * relay_fetch.c uses it. */

    if (sr.selected_case != 0 || !sr.ok || !res) {
        return -1;
    }
    *out_bytes = res->bytes;
    *out_len   = res->len;
    free(res);
    return 0;
}

/* ─────────────────── Blossom-backed chunk fetcher ────────────── */

typedef struct {
    nh_porthome_blossom_t *bl;
    uint64_t total_cap_bytes;
    uint64_t running_bytes;
} fetch_ctx_t;

static int fetch_chunk_cb(void *ctx, const char *sha256_hex,
                          uint8_t **out_ct, size_t *out_ct_len)
{
    fetch_ctx_t *fc = (fetch_ctx_t *)ctx;
    if (!fc || !fc->bl) return -1;

    uint8_t *buf = NULL;
    size_t   len = 0;
    int rc = nh_porthome_blossom_fetch(fc->bl, sha256_hex, &buf, &len);
    if (rc != NH_PORTHOME_BLOSSOM_OK) return -1;

    /* Bandwidth accounting. */
    fc->running_bytes += (uint64_t)len;
    if (fc->total_cap_bytes && fc->running_bytes > fc->total_cap_bytes) {
        free(buf);
        return -1;
    }
    g_bytes = fc->running_bytes;
    emit_progress(NH_PORTHOME_FETCH_PHASE_CHUNK, 0);

    /* PNG-shim detection (nostrc-bpum): if the pusher shimmed this blob
     * to bypass a body-sniffing Blossom server, the downloaded bytes
     * begin with the deterministic PNG shim (hanami-blossom-shim.h).
     * Strip HANAMI_BLOSSOM_PNG_SHIM_LEN bytes off the front and shift
     * the ciphertext down. Detection is unambiguous because porthome
     * D4 ciphertext always begins with 0x01; a shimmed blob begins
     * with 0x89 (PNG signature). The downstream AEAD tag check is the
     * belt-and-braces guard against a false-positive detect. */
    if (hanami_blossom_shim_detect(buf, len)) {
        const uint8_t *tail = NULL;
        size_t         tail_len = 0;
        if (hanami_blossom_shim_strip(buf, len, &tail, &tail_len) != HANAMI_OK) {
            free(buf);
            return -1;
        }
        /* In-place shift: tail lives inside buf, so memmove is safe. */
        memmove(buf, tail, tail_len);
        len = tail_len;
    }

    *out_ct = buf;
    *out_ct_len = len;
    return 0;
}

/* ─────────────────── SSRF pre-resolution gate ──────────────────
 *
 * nh_porthome_blossom_url_ok already enforces the URL-string half of
 * the SSRF gate (https-only, no user-info, no obvious loopback literal).
 * libhanami's blossom client wraps libcurl but does not expose a
 * CURLOPT_OPENSOCKETFUNCTION seam, so we DNS-pre-resolve here and
 * refuse any host whose resolved address is not public-unicast.
 *
 * This does NOT defend against active DNS rebinding at request time
 * (the OS resolver may return a different IP later). It DOES defend
 * against the most common misconfigurations (a Blossom URL pointing
 * at a LAN address, a link-local resolver, or 127.0.0.1 via a wildcard
 * DNS entry). The design (§8.2) accepts this — the sandbox drops the
 * helper to `nostr-home-fetch`/nobody so a real DNS-rebinding
 * exploit would still land inside the sandbox, not on the broker.
 */
static int is_ipv4_public_unicast(uint32_t hb) {
    uint8_t a = (uint8_t)(hb >> 24);
    uint8_t b = (uint8_t)(hb >> 16);
    if (a == 0) return 0;                       /* 0.0.0.0/8 */
    if (a == 10) return 0;                      /* 10.0.0.0/8 */
    if (a == 127) return 0;                     /* 127.0.0.0/8 */
    if (a == 169 && b == 254) return 0;         /* 169.254.0.0/16 */
    if (a == 172 && (b & 0xf0) == 16) return 0; /* 172.16.0.0/12 */
    if (a == 192 && b == 168) return 0;         /* 192.168.0.0/16 */
    if (a == 100 && (b & 0xc0) == 64) return 0; /* CGNAT 100.64/10 */
    if ((a & 0xf0) == 0xe0) return 0;           /* 224/4 multicast */
    if ((a & 0xf0) == 0xf0) return 0;           /* 240/4 reserved */
    if (hb == 0xffffffffu) return 0;            /* broadcast */
    return 1;
}

static int is_ipv6_public_unicast(const struct in6_addr *a) {
    const uint8_t *b = a->s6_addr;
    int all_zero = 1;
    for (int i = 0; i < 16; i++) if (b[i]) { all_zero = 0; break; }
    if (all_zero) return 0;                     /* :: */
    int loopback = 1;
    for (int i = 0; i < 15; i++) if (b[i]) { loopback = 0; break; }
    if (loopback && b[15] == 1) return 0;       /* ::1 */
    if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0 && b[4] == 0 &&
        b[5] == 0 && b[6] == 0 && b[7] == 0 && b[8] == 0 && b[9] == 0 &&
        b[10] == 0xff && b[11] == 0xff) {
        uint32_t hb = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
                      ((uint32_t)b[14] << 8) | (uint32_t)b[15];
        return is_ipv4_public_unicast(hb);      /* ::ffff:v4 */
    }
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return 0;  /* fe80::/10 link-local */
    if ((b[0] & 0xfe) == 0xfc) return 0;                  /* fc00::/7 ULA */
    if (b[0] == 0xff) return 0;                           /* ff00::/8 multicast */
    return 1;
}

/* Extract the hostname from an https:// URL. Writes up to @cap-1 bytes
 * to @host and NUL-terminates. Returns 0 on success. */
static int url_host(const char *url, char *host, size_t cap) {
    if (!url || strncasecmp(url, "https://", 8) != 0) {
        /* Also accept http:// when NH_PORTHOME_ALLOW_INSECURE is set. */
        if (strncasecmp(url, "http://", 7) != 0) return -1;
    }
    const char *p = url + (strncasecmp(url, "https://", 8) == 0 ? 8 : 7);
    /* Strip user-info (should not be present — the URL sanity check
     * refuses it, but belt and braces). Userinfo may contain ':' so we
     * end the search at '/' only. */
    {
        const char *slash = strchr(p, '/');
        const char *at = strchr(p, '@');
        if (at && (!slash || at < slash)) p = at + 1;
    }
    /* IPv6 literal? */
    size_t hlen = 0;
    if (*p == '[') {
        p++;
        while (*p && *p != ']') {
            if (hlen + 1 >= cap) return -1;
            host[hlen++] = *p++;
        }
        if (*p != ']') return -1;
    } else {
        while (*p && *p != ':' && *p != '/') {
            if (hlen + 1 >= cap) return -1;
            host[hlen++] = *p++;
        }
    }
    host[hlen] = '\0';
    return hlen > 0 ? 0 : -1;
}

/* Returns 0 iff every resolved sockaddr for @host is public-unicast.
 * Returns -1 on any private/reserved address OR resolution failure. */
static int host_all_addrs_public(const char *host) {
    if (!host || !*host) return -1;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    int gerr = getaddrinfo(host, NULL, &hints, &res);
    if (gerr != 0 || !res) return -1;
    int ok = 1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            const struct sockaddr_in *in = (const struct sockaddr_in *)ai->ai_addr;
            if (!is_ipv4_public_unicast(ntohl(in->sin_addr.s_addr))) { ok = 0; break; }
        } else if (ai->ai_family == AF_INET6) {
            const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)ai->ai_addr;
            if (!is_ipv6_public_unicast(&in6->sin6_addr)) { ok = 0; break; }
        } else {
            ok = 0; break;
        }
    }
    freeaddrinfo(res);
    return ok ? 0 : -1;
}

/* Pre-resolve every Blossom server host from the control payload;
 * refuse the whole run if ANY host resolves to a private / link-local
 * / loopback address. This is a syntactic + name-resolution SSRF gate
 * BEFORE libhanami-blossom opens its libcurl handle. Returns 0 iff
 * all hosts passed. */
static int ssrf_precheck_servers(const nh_porthome_fetch_ctl *c) {
    for (size_t i = 0; i < c->blossom_servers_count; i++) {
        char host[256];
        if (url_host(c->blossom_servers[i], host, sizeof host) != 0) {
            fprintf(stderr, "nostr-home-fetch: cannot parse host from %s\n",
                    c->blossom_servers[i]);
            return -1;
        }
        if (host_all_addrs_public(host) != 0) {
            fprintf(stderr,
                    "nostr-home-fetch: SSRF refuse blossom server host=%s (private/link-local/loopback address)\n",
                    host);
            return -1;
        }
    }
    return 0;
}

/* ─────────────────────────── main ─────────────────────────────── */

static int read_stdin_bounded(uint8_t **out, size_t *out_len, size_t cap) {
    size_t alloc = 4096;
    if (alloc > cap) alloc = cap;
    uint8_t *buf = (uint8_t *)malloc(alloc);
    if (!buf) return -1;
    size_t off = 0;
    for (;;) {
        if (off == alloc) {
            if (alloc >= cap) { free(buf); return -1; }
            size_t na = alloc * 2u;
            if (na > cap) na = cap;
            uint8_t *nb = (uint8_t *)realloc(buf, na);
            if (!nb) { free(buf); return -1; }
            buf = nb; alloc = na;
        }
        ssize_t n = read(STDIN_FILENO, buf + off, alloc - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf); return -1;
        }
        if (n == 0) break;
        off += (size_t)n;
    }
    *out = buf; *out_len = off;
    return 0;
}

static int open_staging_dir(const char *path) {
    if (!path || !path[0]) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return -1;
    return open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

static void usage(const char *arg0) {
    fprintf(stderr,
        "usage: %s (--staging-fd N | --staging-dir PATH) [--allow-insecure]\n"
        "  Control JSON is read from stdin (max 16 KiB).\n",
        arg0);
}

int main(int argc, char **argv) {
    /* Refuse to run as root. The parent broker (bead nostrc-ww50) will
     * drop privileges before exec; running as root is a bug. */
    if (getuid() == 0 || geteuid() == 0) {
        fprintf(stderr, "nostr-home-fetch: refuse to run as root\n");
        return NH_PORTHOME_FETCH_EXIT_ARG;
    }

    int staging_fd = -1;
    const char *staging_dir = NULL;
    int cli_allow_insecure = 0;

    static struct option opts[] = {
        {"staging-fd",     required_argument, 0, 'f'},
        {"staging-dir",    required_argument, 0, 'd'},
        {"allow-insecure", no_argument,       0, 'i'},
        {"help",           no_argument,       0, 'h'},
        {0, 0, 0, 0},
    };
    for (;;) {
        int idx = 0;
        int c = getopt_long(argc, argv, "f:d:ih", opts, &idx);
        if (c == -1) break;
        switch (c) {
        case 'f': {
            char *end = NULL;
            long v = strtol(optarg, &end, 10);
            if (!end || *end != '\0' || v < 0 || v > 1024) {
                usage(argv[0]);
                return NH_PORTHOME_FETCH_EXIT_ARG;
            }
            staging_fd = (int)v;
            break;
        }
        case 'd':
            staging_dir = optarg;
            break;
        case 'i':
            cli_allow_insecure = 1;
            break;
        case 'h':
            usage(argv[0]);
            return NH_PORTHOME_FETCH_EXIT_OK;
        default:
            usage(argv[0]);
            return NH_PORTHOME_FETCH_EXIT_ARG;
        }
    }
    if ((staging_fd < 0 && !staging_dir) ||
        (staging_fd >= 0 && staging_dir)) {
        usage(argv[0]);
        return NH_PORTHOME_FETCH_EXIT_ARG;
    }

    /* Detach stdout: save it as the private progress channel, then
     * point real stdout at /dev/null. Stray printf/puts from linked
     * libraries (libnostr transport diagnostics, etc.) is silenced
     * so it can't corrupt the broker's progress lexer. */
    {
        int saved = dup(STDOUT_FILENO);
        if (saved < 0) {
            fprintf(stderr, "nostr-home-fetch: dup stdout failed\n");
            return NH_PORTHOME_FETCH_EXIT_INTERNAL;
        }
        int dn = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (dn < 0) { close(saved); return NH_PORTHOME_FETCH_EXIT_INTERNAL; }
        if (dup2(dn, STDOUT_FILENO) < 0) {
            close(dn); close(saved);
            return NH_PORTHOME_FETCH_EXIT_INTERNAL;
        }
        close(dn);
        int fl = fcntl(saved, F_GETFD);
        if (fl >= 0) (void)fcntl(saved, F_SETFD, fl | FD_CLOEXEC);
        g_progress_fd = saved;
    }

    /* Read the control payload. */
    uint8_t *ctl_bytes = NULL;
    size_t   ctl_len   = 0;
    if (read_stdin_bounded(&ctl_bytes, &ctl_len,
                           NH_PORTHOME_FETCH_MAX_CTL_BYTES) != 0) {
        fprintf(stderr, "nostr-home-fetch: stdin read failed\n");
        return NH_PORTHOME_FETCH_EXIT_ARG;
    }
    nh_porthome_fetch_ctl ctl = {0};
    nh_porthome_fetch_ctl_status ps =
        nh_porthome_fetch_ctl_parse((const char *)ctl_bytes, ctl_len, &ctl);
    /* Wipe the raw payload — it contains the 32-byte home_key. */
    if (ctl_bytes) OPENSSL_cleanse(ctl_bytes, ctl_len);
    free(ctl_bytes);
    if (ps != NH_PORTHOME_FETCH_CTL_OK) {
        fprintf(stderr, "nostr-home-fetch: ctl parse: %s\n",
                nh_porthome_fetch_ctl_strerror(ps));
        return NH_PORTHOME_FETCH_EXIT_ARG;
    }
    /* The CLI --allow-insecure is an operator-side sanity guard; it must
     * match the payload's allow_insecure flag if the payload says true.
     * We deliberately do NOT allow the CLI to *upgrade* — the payload is
     * authoritative once accepted. */
    if (ctl.allow_insecure && !cli_allow_insecure) {
        fprintf(stderr,
                "nostr-home-fetch: allow_insecure in payload but --allow-insecure not set\n");
        OPENSSL_cleanse(&ctl, sizeof ctl);
        return NH_PORTHOME_FETCH_EXIT_ARG;
    }
    if (ctl.allow_insecure) setenv("NH_PORTHOME_ALLOW_INSECURE", "1", 1);

    /* Resolve staging fd. */
    int close_staging = 0;
    if (staging_fd < 0) {
        staging_fd = open_staging_dir(staging_dir);
        if (staging_fd < 0) {
            fprintf(stderr, "nostr-home-fetch: staging-dir open failed\n");
            OPENSSL_cleanse(&ctl, sizeof ctl);
            return NH_PORTHOME_FETCH_EXIT_ARG;
        }
        close_staging = 1;
    }

    /* Derive home_key from home_key_hex. */
    uint8_t home_key[NH_PORTHOME_KEY_LEN];
    if (nh_porthome_from_hex64(ctl.home_key_hex, home_key) != 0) {
        fprintf(stderr, "nostr-home-fetch: home_key_hex decode failed\n");
        OPENSSL_cleanse(&ctl, sizeof ctl);
        if (close_staging) close(staging_fd);
        return NH_PORTHOME_FETCH_EXIT_INTERNAL;
    }

    /* Env override for the relay pointer timeout. Prevents an operator
     * from re-signing the broker config when a specific site needs a
     * longer window (slow bunker, high-latency link). Larger of the
     * two wins so the payload default (from auth.conf) is a floor.
     * NOSTR_HOMED_PORTHOME_POINTER_TIMEOUT_MS = uint32 milliseconds. */
    {
        const char *e = getenv("NOSTR_HOMED_PORTHOME_POINTER_TIMEOUT_MS");
        if (e && *e) {
            char *endp = NULL;
            unsigned long v = strtoul(e, &endp, 10);
            if (endp && *endp == '\0' && v > 0 && v <= 600000ul) {
                if ((uint32_t)v > ctl.relay_timeout_ms)
                    ctl.relay_timeout_ms = (uint32_t)v;
            }
        }
    }

    /* SSRF pre-resolution gate — refuse before any bytes cross the
     * network if a Blossom server host resolves to a private address.
     * The URL-syntax half was enforced by the broker via
     * nh_porthome_blossom_url_ok when it accepted the payload; this
     * belt-and-braces gate defends against a DNS entry that maps a
     * public-looking name to a LAN target. */
    if (ssrf_precheck_servers(&ctl) != 0) {
        OPENSSL_cleanse(home_key, sizeof home_key);
        OPENSSL_cleanse(&ctl, sizeof ctl);
        if (close_staging) close(staging_fd);
        return NH_PORTHOME_FETCH_EXIT_SSRF;
    }

    /* Phase 1: relay fetch. */
    emit_progress(NH_PORTHOME_FETCH_PHASE_MANIFEST, 1);
    uint8_t *sealed_manifest = NULL;
    size_t   sealed_len      = 0;
    int rrc = relay_fetch_manifest(&ctl, &sealed_manifest, &sealed_len);
    if (rrc != 0) {
        fprintf(stderr, "nostr-home-fetch: relay fetch: no matching event\n");
        OPENSSL_cleanse(home_key, sizeof home_key);
        OPENSSL_cleanse(&ctl, sizeof ctl);
        if (close_staging) close(staging_fd);
        return NH_PORTHOME_FETCH_EXIT_NETWORK_FAIL;
    }
    g_bytes += (uint64_t)sealed_len;
    emit_progress(NH_PORTHOME_FETCH_PHASE_DECODE, 1);

    /* Build the Blossom client. */
    const char *servers[NH_PORTHOME_FETCH_MAX_SERVERS];
    for (size_t i = 0; i < ctl.blossom_servers_count; i++)
        servers[i] = ctl.blossom_servers[i];

    nh_porthome_blossom_opts_t bopts = {
        .servers = servers,
        .n_servers = ctl.blossom_servers_count,
        .timeout_seconds = ctl.per_file_timeout_sec ?
                           (long)ctl.per_file_timeout_sec : 30L,
        .max_retries = 3,
        .max_blob_bytes = ctl.bandwidth_cap_bytes ?
                          (size_t)ctl.bandwidth_cap_bytes :
                          NH_PORTHOME_BLOSSOM_DEFAULT_MAX_BLOB_BYTES,
    };
    nh_porthome_blossom_t *bl = NULL;
    /* Signer is NULL: BUD-01 reads (GET/HEAD) are unauthenticated per
     * spec — the helper never uploads. */
    if (nh_porthome_blossom_new(&bopts, NULL, &bl) != NH_PORTHOME_BLOSSOM_OK) {
        fprintf(stderr, "nostr-home-fetch: Blossom client init failed\n");
        free(sealed_manifest);
        OPENSSL_cleanse(home_key, sizeof home_key);
        OPENSSL_cleanse(&ctl, sizeof ctl);
        if (close_staging) close(staging_fd);
        return NH_PORTHOME_FETCH_EXIT_ARG;
    }

    fetch_ctx_t fc = { .bl = bl,
                       .total_cap_bytes = ctl.max_total_bytes,
                       .running_bytes = g_bytes };

    nh_porthome_prov_opts pv = {
        .local_uid = getuid(),
        .local_gid = getgid(),
        .max_bytes_per_load = ctl.bandwidth_cap_bytes,
        .load_timeout_sec   = ctl.per_file_timeout_sec,
        .max_total_bytes    = ctl.max_total_bytes,
        .max_entries        = 0, /* default */
        .progress_path      = NULL, /* progress goes on stdout, not a file */
    };

    /* Decode + materialize + rename walk (schema v2 — nostrc-q25o/bms6).
     *
     * We inline the three phases here (rather than calling the
     * `_sealed_into_fd` wrapper) because the post-materialise rename
     * walk needs the decoded manifest object. Semantics preserved:
     *   - decode fail → LIMITED (never touch existing home).
     *   - materialise → same status mapping as before.
     *   - rename walk fail → distinct EXIT_RENAME (78); the tree is
     *     partly renamed but the parent broker discards the mktemp
     *     destination on any non-zero exit. */
    nh_porthome_manifest *m_decoded = NULL;
    int mrc = nh_porthome_manifest_decode_sealed(sealed_manifest, sealed_len,
                                                 home_key, &m_decoded);
    free(sealed_manifest);

    nh_porthome_prov_status pr;
    if (mrc != NH_PORTHOME_OK || !m_decoded) {
        pr = NH_PORTHOME_PROV_LIMITED;
        fprintf(stderr,
                "nostr-home-fetch: manifest decode failed (rc=%d) — LIMITED\n",
                mrc);
    } else {
        pr = nh_porthome_materialize_into_fd(
            staging_fd, m_decoded, home_key,
            fetch_chunk_cb, &fc, &pv, NULL);
    }
    nh_porthome_blossom_free(bl);

    int rc = NH_PORTHOME_FETCH_EXIT_OK;
    switch (pr) {
    case NH_PORTHOME_PROV_OK:
        /* Post-materialise rename walk: v2 manifests get the path_enc →
         * plaintext-basename rename here; v1 manifests short-circuit
         * with renamed=0 missed=m->entries_len (see
         * nh_porthome_rename_walk). */
        {
            size_t renamed = 0, missed = 0;
            int rw = nh_porthome_rename_walk(staging_fd, m_decoded,
                                             &renamed, &missed);
            if (rw != NH_PORTHOME_OK) {
                fprintf(stderr,
                        "nostr-home-fetch: rename walk failed rc=%d "
                        "(renamed=%zu missed=%zu)\n",
                        rw, renamed, missed);
                rc = NH_PORTHOME_FETCH_EXIT_RENAME;
                break;
            }
            fprintf(stderr,
                    "nostr-home-fetch: renamed %zu entries (missed=%zu)\n",
                    renamed, missed);
        }
        g_bytes = fc.running_bytes;
        emit_progress(NH_PORTHOME_FETCH_PHASE_DONE, 1);
        rc = NH_PORTHOME_FETCH_EXIT_OK;
        break;
    case NH_PORTHOME_PROV_LIMITED:
        /* Includes: manifest AEAD failure (in _sealed_into_fd), CBOR
         * decode, chunk fetch exhaustion, per-file cap. All map to
         * LIMITED so the broker never overwrites an existing home. */
        rc = NH_PORTHOME_FETCH_EXIT_NETWORK_FAIL;
        break;
    case NH_PORTHOME_PROV_INVARIANT:
        rc = NH_PORTHOME_FETCH_EXIT_DECRYPT_FAIL;
        break;
    case NH_PORTHOME_PROV_BUDGET:
        rc = NH_PORTHOME_FETCH_EXIT_SIZE_CAP;
        break;
    case NH_PORTHOME_PROV_IO:
        rc = NH_PORTHOME_FETCH_EXIT_INTERNAL;
        break;
    case NH_PORTHOME_PROV_OOM:
        rc = NH_PORTHOME_FETCH_EXIT_INTERNAL;
        break;
    case NH_PORTHOME_PROV_ARG:
    default:
        rc = NH_PORTHOME_FETCH_EXIT_ARG;
        break;
    }

    if (m_decoded) {
        nh_porthome_manifest_dispose(m_decoded);
        free(m_decoded);
    }
    OPENSSL_cleanse(home_key, sizeof home_key);
    OPENSSL_cleanse(&ctl, sizeof ctl);
    if (close_staging) close(staging_fd);
    return rc;
}
