/*
 * porthome_fuse_fixture.c — operator fixture tool for the Phase 4
 * nostr-home-fuse live acceptance (bead nostrc-1u55).
 *
 * SPDX-License-Identifier: MIT
 *
 * Publishes a small fixture directory the way nostr_home_publisher
 * does — real BUD-02 signer, real chunk encryption, real Blossom
 * uploads, real kind-30078 pointer publish — AND writes a full
 * snapshot.json into the supplied state_dir with every field the
 * FUSE mount requires (kind/mode/mtime_ns/size/content_hash_hex/
 * chunk_addrs_hex/symlink_target). Not a CTest case; operator-driven
 * for the live smoke.
 *
 * Usage:
 *   porthome-fuse-fixture publish \
 *       --fixture-dir <src> \
 *       --state-dir  <dst state dir for snapshot.json>
 *       --seed-hex   <64hex>
 *       --nsec-hex   <64hex>
 *       --d-tag      <string>
 *       --relay      <wss://url>          (repeatable)
 *       --blossom    <https://url>        (repeatable, first is primary)
 *       [--generation <N=1>]
 *       [--skip-publish-pointer]
 *
 * Outputs KEY=VALUE lines on stdout for a shell caller:
 *   HOME_KEY_HEX=<64hex>
 *   ACCOUNT_PUBKEY_HEX=<64hex>
 *   POINTER_EVENT_ID=<64hex>
 *   SNAPSHOT_PATH=<path>
 *   FILE=<rel> SIZE=<n> CHUNKS=<c> SHA256=<hex of source cleartext>
 *   BLOSSOM_ADDR=<rel>:<chunk_idx>:<sealed_sha256_hex>
 *   TOTAL_UPLOADS=<n>
 *
 * Exit codes: 0 ok, 64 args, 65 crypto/fixture, 66 blossom upload,
 * 67 relay publish, 68 snapshot write.
 */

#define _GNU_SOURCE
#include "nh_porthome_crypto.h"
#include "nh_porthome_blossom.h"
#include "nh_syncd.h"
#include <hanami/hanami-types.h>

#include "nostr-event.h"
#include "nostr-relay.h"
#include "nostr-tag.h"
#include "nostr-keys.h"

#include <openssl/sha.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Default chunk size (design §D4 says 4 MiB). Overridable via
 * --chunk-size for rigs whose Blossom PUT ceiling is smaller than
 * the design default. */
#define DEFAULT_CHUNK_SIZE (4u * 1024u * 1024u)

/* ---------- BUD-02 signer (real nsec-based, like nostr_home_publisher) ---------- */

typedef struct { const char *nsec_hex; } nsec_ctx;

static hanami_error_t nsec_sign(const char *evt_json, char **out, void *ud) {
    if (!evt_json || !out || !ud) return HANAMI_ERR_INVALID_ARG;
    nsec_ctx *c = (nsec_ctx *)ud;
    NostrEvent *e = nostr_event_new();
    if (!e) return HANAMI_ERR_NOMEM;
    NostrEventValidationStatus vs =
        nostr_event_deserialize_unsigned(e, evt_json, NULL);
    if (vs != NOSTR_EVENT_VALIDATION_OK) {
        nostr_event_free(e); return HANAMI_ERR_INVALID_ARG;
    }
    if (nostr_event_sign(e, c->nsec_hex) != 0) {
        nostr_event_free(e); return HANAMI_ERR_INVALID_ARG;
    }
    char *s = nostr_event_serialize_compact(e);
    nostr_event_free(e);
    if (!s) return HANAMI_ERR_NOMEM;
    *out = s;
    return HANAMI_OK;
}

static char g_pk_hex[65];
static nsec_ctx g_sctx;
static hanami_signer_t g_signer;

static int install_signer(const char *nsec_hex) {
    char *pk = nostr_key_get_public(nsec_hex);
    if (!pk) return -1;
    strncpy(g_pk_hex, pk, 64); g_pk_hex[64] = '\0'; free(pk);
    g_sctx.nsec_hex = nsec_hex;
    g_signer.pubkey = g_pk_hex;
    g_signer.sign = nsec_sign;
    g_signer.user_data = &g_sctx;
    return 0;
}

/* ---------- args ---------- */

typedef struct {
    const char *fixture_dir;
    const char *state_dir;
    const char *seed_hex;
    const char *nsec_hex;
    const char *d_tag;
    const char *relays[16];  size_t relays_n;
    const char *blossom[8];  size_t blossom_n;
    uint64_t    generation;
    int         skip_pointer;
    size_t      chunk_size;
} args_t;

static int parse_args(int argc, char **argv, args_t *a) {
    memset(a, 0, sizeof *a);
    a->d_tag = "nostr-homed.home.v1:fuse-live-1u55";
    a->generation = 1;
    a->chunk_size = DEFAULT_CHUNK_SIZE;
    for (int i = 2; i < argc; i++) {
        const char *k = argv[i];
        const char *eq = strchr(k, '=');
        const char *v = NULL;
        if (eq) v = eq + 1;
        else if (i + 1 < argc && strcmp(k, "--skip-publish-pointer") != 0)
            v = argv[++i];
#define M(name) (eq ? strncmp(k, "--" name "=", strlen("--" name "=")) == 0 \
                    : strcmp(k, "--" name) == 0)
        if      (M("fixture-dir")) a->fixture_dir = v;
        else if (M("state-dir"))   a->state_dir   = v;
        else if (M("seed-hex"))    a->seed_hex    = v;
        else if (M("nsec-hex"))    a->nsec_hex    = v;
        else if (M("d-tag"))       a->d_tag       = v;
        else if (M("generation"))  a->generation  = (uint64_t)strtoull(v, NULL, 10);
        else if (M("chunk-size"))  a->chunk_size  = (size_t)strtoull(v, NULL, 10);
        else if (M("relay")) {
            if (a->relays_n >= 16) return -1;
            a->relays[a->relays_n++] = v;
        } else if (M("blossom")) {
            if (a->blossom_n >= 8) return -1;
            a->blossom[a->blossom_n++] = v;
        } else if (!strcmp(k, "--skip-publish-pointer")) {
            a->skip_pointer = 1;
        } else {
            fprintf(stderr, "unknown flag: %s\n", k); return -1;
        }
#undef M
    }
    if (!a->fixture_dir || !a->state_dir || !a->seed_hex || !a->nsec_hex ||
        strlen(a->seed_hex) != 64 || strlen(a->nsec_hex) != 64 ||
        a->blossom_n == 0) return -1;
    /* Relays optional only when --skip-publish-pointer is set. */
    if (!a->skip_pointer && a->relays_n == 0) return -1;
    return 0;
}

/* ---------- helpers ---------- */

static void bytes_to_hex(const uint8_t *b, size_t n, char *out) {
    static const char lc[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i*2]   = lc[(b[i] >> 4) & 0xf];
        out[i*2+1] = lc[b[i] & 0xf];
    }
    out[n*2] = '\0';
}

/* ---------- capture / upload walk ---------- */

typedef struct {
    const char           *root;
    const uint8_t        *home_key;
    nh_porthome_blossom_t *bl;
    json_t               *files;      /* rel -> {kind,mode,mtime_ns,size,
                                                 content_hash_hex,
                                                 chunk_addrs_hex,
                                                 symlink_target} */
    size_t                uploads;    /* total sealed-blob PUTs */
    size_t                chunk_size;
} up_ctx;

static int add_dir(up_ctx *uc, const char *rel, const struct stat *st) {
    json_t *row = json_object();
    if (!row) return -1;
    json_object_set_new(row, "kind", json_string("dir"));
    json_object_set_new(row, "mode", json_integer(st->st_mode & 0777));
    json_object_set_new(row, "uid",  json_integer((json_int_t)st->st_uid));
    json_object_set_new(row, "gid",  json_integer((json_int_t)st->st_gid));
    uint64_t mt = (uint64_t)st->st_mtim.tv_sec * 1000000000ULL
                + (uint64_t)st->st_mtim.tv_nsec;
    json_object_set_new(row, "mtime_ns", json_integer((json_int_t)mt));
    json_object_set_new(row, "size", json_integer(0));
    json_object_set_new(row, "content_hash_hex", json_string(""));
    json_object_set_new(uc->files, rel, row);
    return 0;
}

static int add_symlink(up_ctx *uc, const char *rel,
                       const struct stat *st, const char *tgt) {
    json_t *row = json_object();
    if (!row) return -1;
    json_object_set_new(row, "kind", json_string("symlink"));
    json_object_set_new(row, "mode", json_integer(st->st_mode & 0777));
    json_object_set_new(row, "uid",  json_integer((json_int_t)st->st_uid));
    json_object_set_new(row, "gid",  json_integer((json_int_t)st->st_gid));
    uint64_t mt = (uint64_t)st->st_mtim.tv_sec * 1000000000ULL
                + (uint64_t)st->st_mtim.tv_nsec;
    json_object_set_new(row, "mtime_ns", json_integer((json_int_t)mt));
    json_object_set_new(row, "size", json_integer(0));
    json_object_set_new(row, "content_hash_hex", json_string(""));
    json_object_set_new(row, "symlink_target", json_string(tgt ? tgt : ""));
    json_object_set_new(uc->files, rel, row);
    return 0;
}

static int upload_file(up_ctx *uc, const char *rel, const struct stat *st) {
    char abs[8192];
    snprintf(abs, sizeof abs, "%s/%s", uc->root, rel);
    int fd = open(abs, O_RDONLY);
    if (fd < 0) return -1;

    size_t total = (size_t)st->st_size;
    size_t remaining = total;

    /* Also compute cleartext SHA-256 for the record + emit it on stdout
     * so the shell caller can byte-compare via md5sum/sha256sum. */
    SHA256_CTX shac; SHA256_Init(&shac);

    json_t *chunks_arr = json_array();
    size_t csz = uc->chunk_size ? uc->chunk_size : DEFAULT_CHUNK_SIZE;
    uint8_t *buf = malloc(csz);
    if (!buf) { close(fd); json_decref(chunks_arr); return -1; }

    size_t idx = 0;
    while (remaining > 0) {
        size_t want = remaining > csz ? csz : remaining;
        size_t got  = 0;
        while (got < want) {
            ssize_t r = read(fd, buf + got, want - got);
            if (r <= 0) { free(buf); close(fd); json_decref(chunks_arr); return -1; }
            got += (size_t)r;
        }
        SHA256_Update(&shac, buf, got);

        uint8_t *ct = NULL; size_t ctl = 0; uint8_t sha[32];
        if (nh_porthome_encrypt_chunk(uc->home_key, buf, got,
                                      &ct, &ctl, sha) != 0) {
            free(buf); close(fd); json_decref(chunks_arr); return -1;
        }
        char hex[65];
        int rc = nh_porthome_blossom_upload(uc->bl, ct, ctl, NULL, hex);
        free(ct);
        if (rc != NH_PORTHOME_BLOSSOM_OK) {
            fprintf(stderr, "fixture: blossom_upload rc=%d chunk %zu of %s\n",
                    rc, idx, rel);
            free(buf); close(fd); json_decref(chunks_arr); return -1;
        }
        json_array_append_new(chunks_arr, json_string(hex));
        fprintf(stdout, "BLOSSOM_ADDR=%s:%zu:%s\n", rel, idx, hex);
        uc->uploads++;
        idx++;
        remaining -= got;
    }
    free(buf); close(fd);

    uint8_t ct_sha[32]; char ct_sha_hex[65];
    SHA256_Final(ct_sha, &shac);
    bytes_to_hex(ct_sha, 32, ct_sha_hex);

    json_t *row = json_object();
    json_object_set_new(row, "kind", json_string("file"));
    json_object_set_new(row, "mode", json_integer(st->st_mode & 0777));
    json_object_set_new(row, "uid",  json_integer((json_int_t)st->st_uid));
    json_object_set_new(row, "gid",  json_integer((json_int_t)st->st_gid));
    uint64_t mt = (uint64_t)st->st_mtim.tv_sec * 1000000000ULL
                + (uint64_t)st->st_mtim.tv_nsec;
    json_object_set_new(row, "mtime_ns", json_integer((json_int_t)mt));
    json_object_set_new(row, "size", json_integer((json_int_t)total));
    json_object_set_new(row, "content_hash_hex", json_string(ct_sha_hex));
    json_object_set_new(row, "chunk_addrs_hex", chunks_arr);
    json_object_set_new(uc->files, rel, row);

    fprintf(stdout, "FILE=%s SIZE=%zu CHUNKS=%zu SHA256=%s\n",
            rel, total, idx, ct_sha_hex);
    return 0;
}

static int walk_and_upload(const char *root, const char *rel, up_ctx *uc) {
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
            if (add_dir(uc, child, &st) != 0) { rc = -1; break; }
            if (walk_and_upload(root, child, uc) != 0) { rc = -1; break; }
        } else if (S_ISREG(st.st_mode)) {
            if (upload_file(uc, child, &st) != 0) { rc = -1; break; }
        } else if (S_ISLNK(st.st_mode)) {
            char tgt[8192];
            ssize_t n = readlink(ca, tgt, sizeof tgt - 1);
            if (n < 0) { rc = -1; break; }
            tgt[n] = '\0';
            if (add_symlink(uc, child, &st, tgt) != 0) { rc = -1; break; }
        } /* skip other types */
    }
    closedir(d);
    return rc;
}

/* ---------- snapshot writer (matches nh_syncd_state schema v1) ---------- */

static int mkdirp(const char *path) {
    char *tmp = strdup(path);
    if (!tmp) return -1;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0700); *p = '/'; }
    }
    mkdir(tmp, 0700);
    free(tmp);
    return 0;
}

static int write_snapshot(const args_t *a, const char *account_pk_hex,
                          json_t *files) {
    if (mkdirp(a->state_dir) != 0) return -1;

    /* root_id_hex: sha256 of the fixture dir path — any 64-hex is
     * acceptable to nh_syncd_state_load. We derive from seed for
     * determinism across generations. */
    uint8_t root_id[32];
    /* deterministic: sha256("fuse-fixture-root:" + d_tag). */
    SHA256_CTX c; SHA256_Init(&c);
    SHA256_Update(&c, "fuse-fixture-root:", 18);
    SHA256_Update(&c, a->d_tag, strlen(a->d_tag));
    SHA256_Final(root_id, &c);
    char root_id_hex[65]; bytes_to_hex(root_id, 32, root_id_hex);

    json_t *root = json_object();
    json_object_set_new(root, "schema", json_integer(1));
    json_object_set_new(root, "generation",
                        json_integer((json_int_t)a->generation));
    json_object_set_new(root, "root", json_string(a->fixture_dir));
    json_object_set_new(root, "d_tag", json_string(a->d_tag));
    json_object_set_new(root, "account_pubkey_hex",
                        json_string(account_pk_hex));
    json_object_set_new(root, "root_id_hex", json_string(root_id_hex));
    json_object_set(root, "files", files);

    char path[1024];
    snprintf(path, sizeof path, "%s/snapshot.json", a->state_dir);
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int rc = json_dump_file(root, tmp, JSON_INDENT(2) | JSON_SORT_KEYS);
    json_decref(root);
    if (rc != 0) return -1;
    if (rename(tmp, path) != 0) return -1;

    /* Sibling generation file. */
    char gpath[1024]; snprintf(gpath, sizeof gpath, "%s/generation", a->state_dir);
    char gtmp[1024];  snprintf(gtmp,  sizeof gtmp,  "%s.tmp", gpath);
    FILE *gf = fopen(gtmp, "w");
    if (!gf) return -1;
    fprintf(gf, "%llu\n", (unsigned long long)a->generation);
    fclose(gf);
    rename(gtmp, gpath);

    fprintf(stdout, "SNAPSHOT_PATH=%s\n", path);
    return 0;
}

/* ---------- kind-30078 pointer publish (optional) ---------- */

static int publish_pointer(const args_t *a, const char *nsec_hex,
                           char out_event_id[65]) {
    /* content is arbitrary here — the FUSE mount does not read the
     * pointer event; it reads snapshot.json. We include a small JSON
     * body so an operator can inspect the event. */
    char content[512];
    snprintf(content, sizeof content,
             "{\"schema\":1,\"d_tag\":\"%s\",\"gen\":%llu}",
             a->d_tag, (unsigned long long)a->generation);

    NostrEvent *e = nostr_event_new();
    if (!e) return -1;
    char *pk = nostr_key_get_public(nsec_hex);
    if (!pk) { nostr_event_free(e); return -1; }
    nostr_event_set_pubkey(e, pk);
    free(pk);
    nostr_event_set_kind(e, 30078);
    nostr_event_set_created_at(e, (int64_t)time(NULL));
    nostr_event_set_content(e, content);

    NostrTags *tags = nostr_tags_new(3,
        nostr_tag_new("d", a->d_tag, NULL),
        nostr_tag_new("client", "porthome-fuse-fixture", NULL),
        nostr_tag_new("alt", "porthome fuse live-smoke pointer", NULL));
    if (!tags) { nostr_event_free(e); return -1; }
    e->tags = tags;

    if (nostr_event_sign(e, nsec_hex) != 0) { nostr_event_free(e); return -1; }
    if (e->id) { strncpy(out_event_id, e->id, 64); out_event_id[64] = '\0'; }

    int any_ok = 0;
    for (size_t i = 0; i < a->relays_n; i++) {
        Error *err = NULL;
        NostrRelay *r = nostr_relay_new(NULL, a->relays[i], &err);
        if (!r) continue;
        if (!nostr_relay_connect(r, &err)) { nostr_relay_free(r); continue; }
        bool ok = nostr_relay_publish_and_wait(r, e, 10000, &err);
        fprintf(stderr, "fixture: %s %s\n", a->relays[i], ok ? "OK" : "FAIL");
        if (ok) any_ok = 1;
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
    if (install_signer(a.nsec_hex) != 0) return 65;

    nh_porthome_blossom_opts_t bopts = {0};
    bopts.servers = a.blossom;
    bopts.n_servers = a.blossom_n;
    bopts.max_blob_bytes = 32u * 1024u * 1024u;
    nh_porthome_blossom_t *bl = NULL;
    if (nh_porthome_blossom_new(&bopts, &g_signer, &bl) != 0) return 65;

    json_t *files = json_object();
    up_ctx uc = { a.fixture_dir, home_key, bl, files, 0, a.chunk_size };
    if (walk_and_upload(a.fixture_dir, "", &uc) != 0) {
        json_decref(files); nh_porthome_blossom_free(bl); return 66;
    }

    if (write_snapshot(&a, g_pk_hex, files) != 0) {
        json_decref(files); nh_porthome_blossom_free(bl); return 68;
    }
    json_decref(files);

    char hk_hex[65]; nh_porthome_hex64(home_key, hk_hex);
    printf("HOME_KEY_HEX=%s\n", hk_hex);
    printf("ACCOUNT_PUBKEY_HEX=%s\n", g_pk_hex);
    printf("TOTAL_UPLOADS=%zu\n", uc.uploads);
    printf("D_TAG=%s\n", a.d_tag);
    printf("GENERATION=%llu\n", (unsigned long long)a.generation);
    printf("CHUNK_SIZE=%zu\n", uc.chunk_size);

    int prc = 0;
    if (!a.skip_pointer) {
        char eid[65] = {0};
        prc = publish_pointer(&a, a.nsec_hex, eid);
        if (prc == 0 && eid[0]) printf("POINTER_EVENT_ID=%s\n", eid);
    }

    memset(home_key, 0, sizeof home_key);
    memset(seed, 0, sizeof seed);
    nh_porthome_blossom_free(bl);
    fflush(stdout);
    return prc == 0 ? 0 : 67;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "publish") == 0)
        return cmd_publish(argc, argv);
    fprintf(stderr,
        "usage: %s publish --fixture-dir <d> --state-dir <d>\n"
        "                   --seed-hex <64h> --nsec-hex <64h>\n"
        "                   --blossom <https://> [--blossom ...]\n"
        "                   [--relay <wss://> ...]\n"
        "                   [--d-tag <s>] [--generation <N>]\n"
        "                   [--skip-publish-pointer]\n",
        argv[0]);
    return 64;
}
