/*
 * nostr_home_publisher.c — bead nostrc-9k4g.1
 *
 * OPERATOR / TEST TOOL. Not installed in the base package.
 *
 * Publishes a portable-home from a live fixture directory to a real
 * relay + real Blossom server(s) so the nostr-home-fetch helper's
 * positive-path round-trip can be acceptance-tested end-to-end (this
 * mirrors tests/integ/fake_relay_fixture + fake_blossom but hits real
 * infra). Wraps the same libnostr_porthome primitives that
 * porthome_smallhome_driver.c uses for the fake-infra path, plus the
 * one extra step: sign and publish the kind-30078 pointer.
 *
 * Usage:
 *   nostr-home-publisher publish \
 *       --fixture-dir <src> \
 *       --seed-hex <64hex>  (drives the home_key derivation)
 *       --nsec-hex <64hex>  (secp256k1 for signing the kind-30078)
 *       --d-tag <string>    (default nostr-homed.home.v1:personal)
 *       --relay <wss://url> (repeatable, at least 1)
 *       --blossom <https://url> (repeatable, at least 1)
 *
 * Outputs, on stdout, one KEY=VALUE line per fact so a shell caller can
 * eval them:
 *   HOME_KEY_HEX=<64hex>
 *   ACCOUNT_PUBKEY_HEX=<64hex>
 *   SEALED_MANIFEST_HEX=<lc-hex ... N chars>
 *   SEALED_MANIFEST_BYTES=<N>
 *   POINTER_EVENT_ID=<64hex>
 *   D_TAG=<string>
 *
 * Exit codes:
 *   0   success (event confirmed OK from >= 1 relay)
 *   64  bad arguments
 *   65  fixture / crypto failure
 *   66  Blossom upload failure
 *   67  relay publish failure
 */

#include "nh_porthome_crypto.h"
#include "nh_porthome_manifest.h"
#include "nh_porthome_blossom.h"
#include <hanami/hanami-types.h>

#include "nostr-event.h"
#include "nostr-relay.h"
#include "nostr-tag.h"
#include "nostr-keys.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK_SIZE (4u * 1024u * 1024u)

static void die(int code, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
    exit(code);
}

/* Real BUD-02 signer for Blossom: parses the kind-24242 auth event
 * JSON built by libhanami, signs it with the operator-supplied nsec
 * via libnostr, and serialises the signed event back. blossom.sharegap.net
 * enforces BUD-02 (401 Unauthorized without a valid signature). */
typedef struct nsec_signer_ctx {
    const char *nsec_hex;
} nsec_signer_ctx;

static hanami_error_t nsec_sign(const char *event_json,
                                char **out_signed_json,
                                void *user_data) {
    if (!event_json || !out_signed_json || !user_data) return HANAMI_ERR_INVALID_ARG;
    *out_signed_json = NULL;
    nsec_signer_ctx *ctx = (nsec_signer_ctx *)user_data;
    NostrEvent *e = nostr_event_new();
    if (!e) return HANAMI_ERR_NOMEM;
    /* deserialize_unsigned accepts events without id/sig — exactly what
     * hanami-bud02-auth hands us. */
    NostrEventValidationStatus vs = nostr_event_deserialize_unsigned(e, event_json, NULL);
    if (vs != NOSTR_EVENT_VALIDATION_OK) {
        nostr_event_free(e);
        return HANAMI_ERR_INVALID_ARG;
    }
    if (nostr_event_sign(e, ctx->nsec_hex) != 0) {
        nostr_event_free(e);
        return HANAMI_ERR_INVALID_ARG;
    }
    char *out = nostr_event_serialize_compact(e);
    nostr_event_free(e);
    if (!out) return HANAMI_ERR_NOMEM;
    *out_signed_json = out;
    return HANAMI_OK;
}

static char g_signer_pubkey_hex[65];
static nsec_signer_ctx g_signer_ctx;
static hanami_signer_t g_test_signer;

static void install_signer(const char *nsec_hex) {
    char *pk = nostr_key_get_public(nsec_hex);
    if (!pk) {
        fprintf(stderr, "publisher: nostr_key_get_public failed\n");
        exit(65);
    }
    strncpy(g_signer_pubkey_hex, pk, 64);
    g_signer_pubkey_hex[64] = '\0';
    free(pk);
    g_signer_ctx.nsec_hex = nsec_hex;
    g_test_signer.pubkey = g_signer_pubkey_hex;
    g_test_signer.sign = nsec_sign;
    g_test_signer.user_data = &g_signer_ctx;
}

/* ---------- args ---------- */

typedef struct {
    const char *fixture_dir;
    const char *seed_hex;
    const char *nsec_hex;
    const char *d_tag;
    const char *relays[16];
    size_t      relays_n;
    const char *blossom[16];
    size_t      blossom_n;
} args_t;

static int parse_args(int argc, char **argv, args_t *a) {
    memset(a, 0, sizeof *a);
    a->d_tag = "nostr-homed.home.v1:personal";
    for (int i = 2; i < argc; i++) {
        const char *k = argv[i];
        if (i + 1 >= argc && strchr(k, '=') == NULL) return -1;
        const char *v = NULL;
        const char *eq = strchr(k, '=');
        if (eq) v = eq + 1;
        else { v = argv[++i]; }
#define M(name) (eq ? strncmp(k, "--" name "=", strlen("--" name "=")) == 0 \
                    : strcmp(k, "--" name) == 0)
        if      (M("fixture-dir")) a->fixture_dir = v;
        else if (M("seed-hex"))    a->seed_hex    = v;
        else if (M("nsec-hex"))    a->nsec_hex    = v;
        else if (M("d-tag"))       a->d_tag       = v;
        else if (M("relay")) {
            if (a->relays_n >= 16) return -1;
            a->relays[a->relays_n++] = v;
        } else if (M("blossom")) {
            if (a->blossom_n >= 16) return -1;
            a->blossom[a->blossom_n++] = v;
        } else {
            fprintf(stderr, "unknown flag: %s\n", k);
            return -1;
        }
#undef M
    }
    if (!a->fixture_dir || !a->seed_hex || !a->nsec_hex ||
        strlen(a->seed_hex) != 64 || strlen(a->nsec_hex) != 64 ||
        a->relays_n == 0 || a->blossom_n == 0) return -1;
    return 0;
}

/* ---------- capture walk (same shape as porthome_smallhome_driver) ---------- */

typedef struct {
    const char *root;
    const uint8_t *home_key;
    nh_porthome_blossom_t *bl;
    nh_porthome_manifest *m;
} up_ctx;

static int upload_chunks(up_ctx *uc, const char *rel,
                         const struct stat *st,
                         nh_porthome_chunk **out_chunks, size_t *out_n) {
    char abs[8192];
    snprintf(abs, sizeof abs, "%s/%s", uc->root, rel);
    int fd = open(abs, O_RDONLY);
    if (fd < 0) return -1;

    size_t remaining = (size_t)st->st_size;
    size_t n_chunks  = remaining ? (remaining + CHUNK_SIZE - 1) / CHUNK_SIZE : 0;
    nh_porthome_chunk *chs = NULL;
    if (n_chunks) {
        chs = calloc(n_chunks, sizeof *chs);
        if (!chs) { close(fd); return -1; }
    }
    uint8_t *buf = malloc(CHUNK_SIZE);
    if (!buf) { close(fd); free(chs); return -1; }
    size_t idx = 0;
    while (remaining > 0) {
        size_t want = remaining > CHUNK_SIZE ? CHUNK_SIZE : remaining;
        size_t got  = 0;
        while (got < want) {
            ssize_t r = read(fd, buf + got, want - got);
            if (r <= 0) { free(buf); close(fd); free(chs); return -1; }
            got += (size_t)r;
        }
        uint8_t *ct = NULL; size_t ctl = 0; uint8_t sha[32];
        if (nh_porthome_encrypt_chunk(uc->home_key, buf, got,
                                      &ct, &ctl, sha) != 0) {
            free(buf); close(fd); free(chs); return -1;
        }
        char hex[65];
        int rc = nh_porthome_blossom_upload(uc->bl, ct, ctl, NULL, hex);
        free(ct);
        if (rc != NH_PORTHOME_BLOSSOM_OK) {
            fprintf(stderr, "blossom_upload rc=%d for chunk %zu of %s\n",
                    rc, idx, rel);
            free(buf); close(fd); free(chs); return -1;
        }
        memcpy(chs[idx].sha256, sha, 32);
        chs[idx].size = (uint32_t)ctl;
        chs[idx].chunk_key_id = 0;
        idx++;
        remaining -= got;
    }
    free(buf); close(fd);
    *out_chunks = chs;
    *out_n = idx;
    return 0;
}

typedef int (*walk_cb_fn)(void *ud, const char *rel, const struct stat *st,
                          nh_porthome_entry_kind kind, const char *tgt);

static int walk(const char *root, const char *rel, walk_cb_fn cb, void *ud) {
    char abs[8192];
    if (rel[0]) snprintf(abs, sizeof abs, "%s/%s", root, rel);
    else        snprintf(abs, sizeof abs, "%s", root);
    DIR *d = opendir(abs);
    if (!d) return -1;
    struct dirent *de;
    int rc = 0;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char child[8192];
        if (rel[0]) snprintf(child, sizeof child, "%s/%s", rel, de->d_name);
        else        snprintf(child, sizeof child, "%s", de->d_name);
        char ca[16384];
        snprintf(ca, sizeof ca, "%s/%s", root, child);
        struct stat st;
        if (lstat(ca, &st) != 0) { rc = -1; break; }
        if (S_ISDIR(st.st_mode)) {
            if (cb(ud, child, &st, NH_PORTHOME_KIND_DIR, NULL) != 0) { rc = -1; break; }
            if (walk(root, child, cb, ud) != 0) { rc = -1; break; }
        } else if (S_ISREG(st.st_mode)) {
            if (cb(ud, child, &st, NH_PORTHOME_KIND_FILE, NULL) != 0) { rc = -1; break; }
        } else if (S_ISLNK(st.st_mode)) {
            char tgt[8192];
            ssize_t n = readlink(ca, tgt, sizeof tgt - 1);
            if (n < 0) { rc = -1; break; }
            tgt[n] = '\0';
            if (cb(ud, child, &st, NH_PORTHOME_KIND_SYMLINK, tgt) != 0) { rc = -1; break; }
        }
        /* skip devices/sockets/fifos */
    }
    closedir(d);
    return rc;
}

static int add_entry(void *ud, const char *rel, const struct stat *st,
                     nh_porthome_entry_kind kind, const char *tgt) {
    up_ctx *uc = (up_ctx *)ud;
    char *penc = NULL;
    if (nh_porthome_encrypt_path(uc->home_key, rel, &penc) != 0) return -1;
    uint32_t mode = st->st_mode & 0777;
    uint64_t mt_ns = (uint64_t)st->st_mtim.tv_sec * 1000000000ULL
                   + (uint64_t)st->st_mtim.tv_nsec;
    if (kind == NH_PORTHOME_KIND_DIR)
        return nh_porthome_manifest_add_dir(uc->m, penc, mode,
            (uint32_t)st->st_uid, (uint32_t)st->st_gid, mt_ns);
    if (kind == NH_PORTHOME_KIND_SYMLINK) {
        char *dup = strdup(tgt ? tgt : "");
        if (!dup) { free(penc); return -1; }
        return nh_porthome_manifest_add_symlink(uc->m, penc, mode,
            (uint32_t)st->st_uid, (uint32_t)st->st_gid, mt_ns, dup);
    }
    if (kind == NH_PORTHOME_KIND_FILE) {
        nh_porthome_chunk *chs = NULL; size_t nc = 0;
        if (upload_chunks(uc, rel, st, &chs, &nc) != 0) { free(penc); return -1; }
        int rc = nh_porthome_manifest_add_file(uc->m, penc, mode,
            (uint32_t)st->st_uid, (uint32_t)st->st_gid, mt_ns,
            (uint64_t)st->st_size, chs, nc);
        free(chs);
        return rc;
    }
    free(penc);
    return 0;
}

/* ---------- kind-30078 pointer publish ---------- */

static void bytes_to_hex(const uint8_t *b, size_t n, char *out) {
    static const char lc[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = lc[(b[i] >> 4) & 0xf];
        out[i * 2 + 1] = lc[b[i] & 0xf];
    }
    out[n * 2] = '\0';
}

static int publish_pointer(const args_t *a,
                           const uint8_t *sealed, size_t sealed_len,
                           char out_event_id[65]) {
    /* content = lowercase hex(sealed). The fetch helper accepts either
     * lc-hex or raw; hex is JSON-safe. Cap: relay accepts up to ~64 KiB. */
    char *content = malloc(sealed_len * 2 + 1);
    if (!content) return -1;
    bytes_to_hex(sealed, sealed_len, content);

    NostrEvent *e = nostr_event_new();
    if (!e) { free(content); return -1; }

    /* Derive account pubkey from nsec. */
    char *pk = nostr_key_get_public(a->nsec_hex);
    if (!pk) { nostr_event_free(e); free(content); return -1; }
    nostr_event_set_pubkey(e, pk);
    nostr_event_set_kind(e, 30078);
    nostr_event_set_created_at(e, (int64_t)time(NULL));
    nostr_event_set_content(e, content);

    NostrTags *tags = nostr_tags_new(3,
        nostr_tag_new("d", a->d_tag, NULL),
        nostr_tag_new("client", "nostr-home-publisher", NULL),
        nostr_tag_new("alt", "encrypted portable home pointer", NULL));
    if (!tags) {
        free(pk); nostr_event_free(e); free(content); return -1;
    }
    e->tags = tags;

    if (nostr_event_sign(e, a->nsec_hex) != 0) {
        free(pk); nostr_event_free(e); free(content); return -1;
    }
    free(content);
    if (e->id) {
        strncpy(out_event_id, e->id, 64);
        out_event_id[64] = '\0';
    } else {
        out_event_id[0] = '\0';
    }
    /* Print account pubkey now — the caller may want it for the
     * subsequent PROVISION_HOME flow. */
    printf("ACCOUNT_PUBKEY_HEX=%s\n", pk);
    free(pk);

    /* Publish to every relay; success if ANY returns OK. */
    int any_ok = 0;
    for (size_t i = 0; i < a->relays_n; i++) {
        Error *err = NULL;
        NostrRelay *r = nostr_relay_new(NULL, a->relays[i], &err);
        if (!r) {
            fprintf(stderr, "publisher: nostr_relay_new(%s) failed\n",
                    a->relays[i]);
            continue;
        }
        if (!nostr_relay_connect(r, &err)) {
            fprintf(stderr, "publisher: connect(%s) failed\n", a->relays[i]);
            nostr_relay_free(r);
            continue;
        }
        bool ok = nostr_relay_publish_and_wait(r, e, 10000, &err);
        if (ok) {
            fprintf(stderr, "publisher: %s OK\n", a->relays[i]);
            any_ok = 1;
        } else {
            fprintf(stderr, "publisher: %s FAILED\n", a->relays[i]);
        }
        nostr_relay_free(r);
    }

    nostr_event_free(e);
    return any_ok ? 0 : -1;
}

/* ---------- main ---------- */

static int cmd_publish(int argc, char **argv) {
    args_t a;
    if (parse_args(argc, argv, &a) != 0) return 64;

    uint8_t seed[32];
    if (nh_porthome_from_hex64(a.seed_hex, seed) != 0) return 64;

    uint8_t home_key[32];
    if (nh_porthome_key_derive(seed, home_key) != 0) return 65;

    /* Install the real BUD-02 signer using the operator's nsec so
     * blossom.sharegap.net (and any spec-compliant server) accepts
     * PUTs. */
    install_signer(a.nsec_hex);

    nh_porthome_blossom_opts_t bopts = {0};
    bopts.servers    = a.blossom;
    bopts.n_servers  = a.blossom_n;
    bopts.max_blob_bytes = 32u * 1024u * 1024u;
    nh_porthome_blossom_t *bl = NULL;
    if (nh_porthome_blossom_new(&bopts, &g_test_signer, &bl) != 0) return 65;

    nh_porthome_manifest m;
    uint8_t root_id[32]; memcpy(root_id, seed, 32);
    if (nh_porthome_manifest_init(&m, root_id) != 0) {
        nh_porthome_blossom_free(bl); return 65;
    }

    up_ctx uc = { a.fixture_dir, home_key, bl, &m };
    if (walk(a.fixture_dir, "", add_entry, &uc) != 0) {
        nh_porthome_manifest_dispose(&m);
        nh_porthome_blossom_free(bl);
        return 66;
    }

    uint8_t *sealed = NULL; size_t sealed_len = 0;
    if (nh_porthome_manifest_encode_sealed(&m, home_key,
                                           &sealed, &sealed_len) != 0) {
        nh_porthome_manifest_dispose(&m);
        nh_porthome_blossom_free(bl);
        return 65;
    }

    char hk_hex[65];
    nh_porthome_hex64(home_key, hk_hex);
    printf("HOME_KEY_HEX=%s\n", hk_hex);
    printf("SEALED_MANIFEST_BYTES=%zu\n", sealed_len);
    /* Emit the sealed manifest hex on stdout so an operator can hand it
     * to a debugger if needed; kept last (line-length friendly). */
    char *hex_dump = malloc(sealed_len * 2 + 1);
    if (hex_dump) {
        bytes_to_hex(sealed, sealed_len, hex_dump);
        printf("SEALED_MANIFEST_HEX=%s\n", hex_dump);
        free(hex_dump);
    }
    printf("D_TAG=%s\n", a.d_tag);

    char eid[65] = {0};
    int prc = publish_pointer(&a, sealed, sealed_len, eid);
    if (prc == 0) printf("POINTER_EVENT_ID=%s\n", eid);
    fflush(stdout);

    /* Wipe the derived keys before exiting. */
    memset(home_key, 0, sizeof home_key);
    memset(seed, 0, sizeof seed);

    free(sealed);
    nh_porthome_manifest_dispose(&m);
    nh_porthome_blossom_free(bl);
    return prc == 0 ? 0 : 67;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "publish") == 0)
        return cmd_publish(argc, argv);
    fprintf(stderr,
        "usage: %s publish --fixture-dir <d> --seed-hex <64h>\n"
        "                   --nsec-hex <64h> [--d-tag <s>]\n"
        "                   --relay <wss://> [--relay ...]\n"
        "                   --blossom <https://> [--blossom ...]\n",
        argv[0]);
    return 64;
}
