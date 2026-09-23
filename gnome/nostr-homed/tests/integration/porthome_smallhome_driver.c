/*
 * porthome_smallhome_driver.c — the C half of the Phase-1 acceptance
 * test. Driven by tests/integration/test_porthome_smallhome.py.
 *
 * Two subcommands:
 *   upload <fixture-dir> <sealed-manifest-out.bin>
 *     Walks <fixture-dir>, chunks each regular file at 4 MiB, encrypts
 *     each chunk under the home_key, uploads to Blossom, builds a
 *     manifest, seals it, and writes the sealed bytes to
 *     <sealed-manifest-out.bin>. Also prints the home_key hex to
 *     stdout so the Python side can inject it into the download run.
 *
 *   download <sealed-manifest-in.bin> <out-dir>
 *     Reads sealed manifest, decrypts+decodes, materializes every
 *     entry into <out-dir> by fetching + decrypting each chunk.
 *
 * Environment:
 *   NH_PORTHOME_BLOSSOM_URLS   comma-separated http(s) URLs
 *   NH_PORTHOME_SEED_HEX       64 hex chars — the seed used to derive
 *                              home_key (test-only; production sources
 *                              this from NIP-46 nip44_decrypt)
 *   NH_PORTHOME_ALLOW_INSECURE=1  required to accept http:// servers
 */

#include "nh_porthome_crypto.h"
#include "nh_porthome_manifest.h"
#include "nh_porthome_blossom.h"
#include <hanami/hanami-types.h>
#include <string.h>

/* Passthrough signer for the acceptance test only: fake_blossom
 * does not verify the BUD-02 kind-24242 signature, so this is
 * sufficient to unblock the wrapper's upload path. Production
 * paths must use a real signer — a NIP-46 client, a local
 * secret-key signer, or the delegate-key signer described in
 * docs/designs/home-from-relay.md §4.3 D9. */
static hanami_error_t passthrough_sign(const char *event_json,
                                       char **out_signed_json,
                                       void *user_data) {
    (void)user_data;
    *out_signed_json = strdup(event_json);
    return *out_signed_json ? HANAMI_OK : HANAMI_ERR_NOMEM;
}
static const hanami_signer_t g_test_signer = {
    .pubkey = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    .sign = passthrough_sign,
    .user_data = NULL,
};

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CHUNK_SIZE (4u * 1024u * 1024u)

static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

static int seed_from_env(uint8_t seed[32]) {
    const char *s = getenv("NH_PORTHOME_SEED_HEX");
    if (!s || strlen(s) != 64) return -1;
    return nh_porthome_from_hex64(s, seed) == 0 ? 0 : -1;
}

/* Parse "https://a,https://b,https://c" into an argv-shaped array. */
static char **split_servers(const char *csv, size_t *out_n) {
    if (!csv) return NULL;
    size_t n = 1;
    for (const char *p = csv; *p; p++) if (*p == ',') n++;
    char **arr = (char **)calloc(n, sizeof(char *));
    if (!arr) return NULL;
    size_t idx = 0;
    const char *start = csv;
    for (const char *p = csv; ; p++) {
        if (*p == ',' || *p == '\0') {
            size_t len = (size_t)(p - start);
            arr[idx] = (char *)malloc(len + 1);
            memcpy(arr[idx], start, len);
            arr[idx][len] = '\0';
            idx++;
            if (*p == '\0') break;
            start = p + 1;
        }
    }
    *out_n = n;
    return arr;
}

static void free_servers(char **s, size_t n) {
    if (!s) return;
    for (size_t i = 0; i < n; i++) free(s[i]);
    free(s);
}

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *b = (uint8_t *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    if (n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *out_len = (size_t)n;
    return b;
}

static int spew(const char *path, const uint8_t *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = 0;
    if (len) w = fwrite(buf, 1, len, f);
    int ok = (w == len);
    if (fflush(f) != 0) ok = 0;
    if (fsync(fileno(f)) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

/* Recursively walk a directory tree, invoking cb(rel_path, kind, stat) for
 * regular files and directories. Symlinks are recorded but not followed. */
typedef int (*walk_cb)(void *ud, const char *rel, const struct stat *st,
                       nh_porthome_entry_kind kind,
                       const char *symlink_target);

static int walk_rec(const char *root, const char *rel, walk_cb cb, void *ud) {
    char abs_path[8192];
    if (rel[0])
        snprintf(abs_path, sizeof(abs_path), "%s/%s", root, rel);
    else
        snprintf(abs_path, sizeof(abs_path), "%s", root);

    DIR *d = opendir(abs_path);
    if (!d) return -1;
    struct dirent *de;
    int rc = 0;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char child_rel[8192];
        if (rel[0])
            snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, de->d_name);
        else
            snprintf(child_rel, sizeof(child_rel), "%s", de->d_name);
        char child_abs[16384];
        snprintf(child_abs, sizeof(child_abs), "%s/%s", root, child_rel);
        struct stat st;
        if (lstat(child_abs, &st) != 0) { rc = -1; break; }
        if (S_ISDIR(st.st_mode)) {
            if (cb(ud, child_rel, &st, NH_PORTHOME_KIND_DIR, NULL) != 0) { rc = -1; break; }
            if (walk_rec(root, child_rel, cb, ud) != 0) { rc = -1; break; }
        } else if (S_ISREG(st.st_mode)) {
            if (cb(ud, child_rel, &st, NH_PORTHOME_KIND_FILE, NULL) != 0) { rc = -1; break; }
        } else if (S_ISLNK(st.st_mode)) {
            char tgt[8192];
            ssize_t n = readlink(child_abs, tgt, sizeof(tgt) - 1);
            if (n < 0) { rc = -1; break; }
            tgt[n] = '\0';
            if (cb(ud, child_rel, &st, NH_PORTHOME_KIND_SYMLINK, tgt) != 0) { rc = -1; break; }
        } else {
            /* skip devices/sockets/fifos — design §2.3 */
        }
    }
    closedir(d);
    return rc;
}

/* ────────────── upload mode ────────────── */

typedef struct {
    const char *fixture_root;
    const uint8_t *home_key;
    nh_porthome_blossom_t *bl;
    nh_porthome_manifest *m;
} upload_ctx;

static int upload_file_chunks(upload_ctx *uc, const char *rel,
                              const struct stat *st,
                              nh_porthome_chunk **out_chunks,
                              size_t *out_n) {
    char abs_path[8192];
    snprintf(abs_path, sizeof(abs_path), "%s/%s", uc->fixture_root, rel);
    int fd = open(abs_path, O_RDONLY);
    if (fd < 0) return -1;

    size_t remaining = (size_t)st->st_size;
    size_t n_chunks = (remaining + CHUNK_SIZE - 1) / CHUNK_SIZE;
    if (remaining == 0) n_chunks = 0;
    nh_porthome_chunk *chs = NULL;
    if (n_chunks) {
        chs = (nh_porthome_chunk *)calloc(n_chunks, sizeof(*chs));
        if (!chs) { close(fd); return -1; }
    }

    uint8_t *buf = (uint8_t *)malloc(CHUNK_SIZE);
    if (!buf) { close(fd); free(chs); return -1; }

    size_t idx = 0;
    while (remaining > 0) {
        size_t want = remaining > CHUNK_SIZE ? CHUNK_SIZE : remaining;
        size_t got = 0;
        while (got < want) {
            ssize_t r = read(fd, buf + got, want - got);
            if (r <= 0) { free(buf); close(fd); free(chs); return -1; }
            got += (size_t)r;
        }
        uint8_t *ct = NULL; size_t ctlen = 0; uint8_t sha[32];
        if (nh_porthome_encrypt_chunk(uc->home_key, buf, got,
                                      &ct, &ctlen, sha) != 0) {
            free(buf); close(fd); free(chs); return -1;
        }
        char hex[65];
        int rc = nh_porthome_blossom_upload(uc->bl, ct, ctlen, NULL, hex);
        free(ct);
        if (rc != NH_PORTHOME_BLOSSOM_OK) {
            fprintf(stderr, "upload of chunk %zu of %s failed rc=%d\n", idx, rel, rc);
            free(buf); close(fd); free(chs); return -1;
        }
        memcpy(chs[idx].sha256, sha, 32);
        chs[idx].size = (uint32_t)ctlen;
        chs[idx].chunk_key_id = 0;
        idx++;
        remaining -= got;
    }
    free(buf); close(fd);
    *out_chunks = chs;
    *out_n = idx;
    return 0;
}

static int add_entry_cb(void *ud, const char *rel, const struct stat *st,
                        nh_porthome_entry_kind kind,
                        const char *symlink_target) {
    upload_ctx *uc = (upload_ctx *)ud;
    char *penc = NULL;
    if (nh_porthome_encrypt_path(uc->home_key, rel, &penc) != 0) return -1;
    uint32_t mode = st->st_mode & 07777;
    mode &= 0777; /* strip setuid/setgid/sticky per design */
    uint64_t mtime_ns = (uint64_t)st->st_mtim.tv_sec * 1000000000ULL
                      + (uint64_t)st->st_mtim.tv_nsec;
    if (kind == NH_PORTHOME_KIND_DIR) {
        return nh_porthome_manifest_add_dir(uc->m, penc, mode,
            (uint32_t)st->st_uid, (uint32_t)st->st_gid, mtime_ns);
    } else if (kind == NH_PORTHOME_KIND_SYMLINK) {
        char *tgt = strdup(symlink_target ? symlink_target : "");
        if (!tgt) { free(penc); return -1; }
        return nh_porthome_manifest_add_symlink(uc->m, penc, mode,
            (uint32_t)st->st_uid, (uint32_t)st->st_gid, mtime_ns, tgt);
    } else if (kind == NH_PORTHOME_KIND_FILE) {
        nh_porthome_chunk *chs = NULL; size_t nc = 0;
        if (upload_file_chunks(uc, rel, st, &chs, &nc) != 0) { free(penc); return -1; }
        int rc = nh_porthome_manifest_add_file(uc->m, penc, mode,
            (uint32_t)st->st_uid, (uint32_t)st->st_gid,
            mtime_ns, (uint64_t)st->st_size, chs, nc);
        free(chs);
        return rc;
    }
    free(penc);
    return 0;
}

static int cmd_upload(const char *fixture_root, const char *sealed_out) {
    uint8_t seed[32]; if (seed_from_env(seed) != 0) die("missing NH_PORTHOME_SEED_HEX");
    uint8_t home_key[32];
    if (nh_porthome_key_derive(seed, home_key) != 0) die("key_derive");

    const char *csv = getenv("NH_PORTHOME_BLOSSOM_URLS");
    if (!csv) die("missing NH_PORTHOME_BLOSSOM_URLS");
    size_t n_srv = 0;
    char **srv = split_servers(csv, &n_srv);
    if (!srv) die("split_servers");

    nh_porthome_blossom_opts_t opts = {0};
    opts.servers = (const char *const *)srv;
    opts.n_servers = n_srv;
    opts.max_blob_bytes = 32u * 1024u * 1024u; /* generous for the 5 MiB fixture */
    nh_porthome_blossom_t *bl = NULL;
    if (nh_porthome_blossom_new(&opts, &g_test_signer, &bl) != 0) die("blossom_new");

    nh_porthome_manifest m;
    uint8_t root_id[32]; memcpy(root_id, seed, 32);
    if (nh_porthome_manifest_init(&m, root_id) != 0) die("manifest_init");

    upload_ctx uc = { fixture_root, home_key, bl, &m };
    if (walk_rec(fixture_root, "", add_entry_cb, &uc) != 0) die("walk");

    uint8_t *sealed = NULL; size_t sealed_len = 0;
    if (nh_porthome_manifest_encode_sealed(&m, home_key, &sealed, &sealed_len) != 0)
        die("encode_sealed");
    if (spew(sealed_out, sealed, sealed_len) != 0) die("spew sealed");

    /* Print home_key so the download side can be handed the same key. */
    char hex[65]; nh_porthome_hex64(home_key, hex);
    printf("HOME_KEY_HEX=%s\n", hex);
    printf("SEALED_BYTES=%zu\n", sealed_len);
    printf("ENTRIES=%zu\n", m.entries_len);
    fflush(stdout);

    nh_porthome_manifest_dispose(&m);
    nh_porthome_blossom_free(bl);
    free_servers(srv, n_srv);
    free(sealed);
    return 0;
}

/* ────────────── download mode ────────────── */

static int ensure_dir_recursive(const char *path) {
    /* Create every parent of `path` (which is itself a dir path). */
    char tmp[8192];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* Decrypt every hex-encoded path component using a lookup table built
 * from the sender's plaintext paths — but we don't have that here.
 * For the acceptance test we don't need to *invert* the encrypted path,
 * we just need each recipient to write to a directory keyed by the same
 * encrypted-path form as the sender. That gives us a stable per-file
 * output identity to compare on. */

static int cmd_download(const char *sealed_in, const char *out_root) {
    const char *hk_hex = getenv("NH_PORTHOME_HOME_KEY_HEX");
    if (!hk_hex || strlen(hk_hex) != 64) die("missing NH_PORTHOME_HOME_KEY_HEX");
    uint8_t home_key[32];
    if (nh_porthome_from_hex64(hk_hex, home_key) != 0) die("home_key parse");

    const char *csv = getenv("NH_PORTHOME_BLOSSOM_URLS");
    if (!csv) die("missing NH_PORTHOME_BLOSSOM_URLS");
    size_t n_srv = 0;
    char **srv = split_servers(csv, &n_srv);
    if (!srv) die("split_servers");
    nh_porthome_blossom_opts_t opts = {0};
    opts.servers = (const char *const *)srv;
    opts.n_servers = n_srv;
    opts.max_blob_bytes = 32u * 1024u * 1024u;
    nh_porthome_blossom_t *bl = NULL;
    if (nh_porthome_blossom_new(&opts, &g_test_signer, &bl) != 0) die("blossom_new");

    size_t sealed_len = 0;
    uint8_t *sealed = slurp(sealed_in, &sealed_len);
    if (!sealed) die("slurp sealed");
    nh_porthome_manifest *m = NULL;
    if (nh_porthome_manifest_decode_sealed(sealed, sealed_len, home_key, &m) != 0)
        die("decode_sealed");
    free(sealed);

    if (mkdir(out_root, 0700) != 0 && errno != EEXIST) die("mkdir out_root");

    for (size_t i = 0; i < m->entries_len; i++) {
        const nh_porthome_entry *e = &m->entries[i];
        char target[8192];
        snprintf(target, sizeof(target), "%s/%s", out_root, e->path_enc);
        if (e->kind == NH_PORTHOME_KIND_DIR) {
            if (ensure_dir_recursive(target) != 0) die("mkdir %s", target);
        } else if (e->kind == NH_PORTHOME_KIND_SYMLINK) {
            /* Make sure parent exists. */
            char parent[8192]; snprintf(parent, sizeof(parent), "%s", target);
            char *slash = strrchr(parent, '/');
            if (slash) { *slash = '\0'; ensure_dir_recursive(parent); }
            if (symlink(e->symlink_target, target) != 0 && errno != EEXIST)
                die("symlink %s -> %s: %s", target, e->symlink_target, strerror(errno));
        } else if (e->kind == NH_PORTHOME_KIND_FILE) {
            char parent[8192]; snprintf(parent, sizeof(parent), "%s", target);
            char *slash = strrchr(parent, '/');
            if (slash) { *slash = '\0'; ensure_dir_recursive(parent); }
            FILE *f = fopen(target, "wb");
            if (!f) die("fopen %s: %s", target, strerror(errno));
            for (size_t j = 0; j < e->chunks_len; j++) {
                char hex[65];
                nh_porthome_hex64(e->chunks[j].sha256, hex);
                uint8_t *sealed_chunk = NULL; size_t schunk_len = 0;
                int rc = nh_porthome_blossom_fetch(bl, hex, &sealed_chunk, &schunk_len);
                if (rc != NH_PORTHOME_BLOSSOM_OK) die("fetch chunk %s rc=%d", hex, rc);
                uint8_t *pt = NULL; size_t pt_len = 0;
                if (nh_porthome_decrypt_chunk(home_key, sealed_chunk, schunk_len,
                                              &pt, &pt_len) != 0) die("decrypt chunk %s", hex);
                free(sealed_chunk);
                if (pt_len && fwrite(pt, 1, pt_len, f) != pt_len) die("write %s", target);
                free(pt);
            }
            if (fflush(f) != 0) die("fflush %s", target);
            if (fsync(fileno(f)) != 0) die("fsync %s", target);
            if (fclose(f) != 0) die("fclose %s", target);
            /* Restore mode (best-effort). */
            chmod(target, (mode_t)(e->mode & 0777));
        }
    }

    nh_porthome_manifest_dispose(m); free(m);
    nh_porthome_blossom_free(bl);
    free_servers(srv, n_srv);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr,
            "usage: %s upload   <fixture-dir>          <sealed-out.bin>\n"
            "       %s download <sealed-in.bin>        <out-dir>\n",
            argv[0], argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "upload") == 0)   return cmd_upload(argv[2], argv[3]);
    if (strcmp(argv[1], "download") == 0) return cmd_download(argv[2], argv[3]);
    die("unknown subcommand: %s", argv[1]);
    return 2;
}
