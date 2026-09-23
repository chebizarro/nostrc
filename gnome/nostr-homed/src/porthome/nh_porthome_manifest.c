/*
 * nh_porthome_manifest.c — canonical-CBOR manifest codec.
 * SPDX-License-Identifier: MIT
 */

#include "nh_porthome_manifest.h"
#include "nh_porthome_crypto.h"
#include "nh_porthome_cbor.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ────────────── field key numbers (fixed by schema v1) ────────────── */

/* Manifest map keys. */
#define K_M_VERSION       1u
#define K_M_HOME_ROOT_ID  2u
#define K_M_ENTRIES       3u

/* Entry map keys. */
#define K_E_PATH          1u
#define K_E_MODE          2u
#define K_E_UID_HINT      3u
#define K_E_GID_HINT      4u
#define K_E_MTIME_NS      5u
#define K_E_SIZE          6u
#define K_E_KIND          7u
#define K_E_CHUNKS        8u  /* optional (files only) */
#define K_E_SYMLINK_TGT   9u  /* optional (symlinks only) */

/* Chunk map keys. */
#define K_C_SHA256        1u
#define K_C_SIZE          2u
#define K_C_KEY_ID        3u

/* ────────────── free / init helpers ────────────── */

void nh_porthome_entry_dispose(nh_porthome_entry *e) {
    if (!e) return;
    free(e->path_enc);
    free(e->symlink_target);
    free(e->chunks);
    memset(e, 0, sizeof(*e));
}

int nh_porthome_manifest_init(nh_porthome_manifest *out,
                              const uint8_t home_root_id[32]) {
    if (!out) return NH_PORTHOME_ERR_ARG;
    memset(out, 0, sizeof(*out));
    out->version = NH_PORTHOME_MANIFEST_SCHEMA_VERSION;
    if (home_root_id) memcpy(out->home_root_id, home_root_id, 32);
    return NH_PORTHOME_OK;
}

void nh_porthome_manifest_dispose(nh_porthome_manifest *m) {
    if (!m) return;
    for (size_t i = 0; i < m->entries_len; i++) nh_porthome_entry_dispose(&m->entries[i]);
    free(m->entries);
    memset(m, 0, sizeof(*m));
}

static int append_entry(nh_porthome_manifest *m, nh_porthome_entry *e) {
    if (m->entries_len >= NH_PORTHOME_MAX_ENTRIES) return NH_PORTHOME_ERR_TOO_LARGE;
    nh_porthome_entry *ne = (nh_porthome_entry *)realloc(
        m->entries, sizeof(*ne) * (m->entries_len + 1));
    if (!ne) return NH_PORTHOME_ERR_OOM;
    m->entries = ne;
    m->entries[m->entries_len] = *e;
    m->entries_len++;
    return NH_PORTHOME_OK;
}

int nh_porthome_manifest_add_file(nh_porthome_manifest *m,
                                  char *path_enc,
                                  uint32_t mode,
                                  uint32_t uid_hint,
                                  uint32_t gid_hint,
                                  uint64_t mtime_ns,
                                  uint64_t size,
                                  const nh_porthome_chunk *chunks,
                                  size_t chunks_len) {
    if (!m || !path_enc) return NH_PORTHOME_ERR_ARG;
    if (chunks_len > NH_PORTHOME_MAX_CHUNKS_PER_ENTRY) return NH_PORTHOME_ERR_TOO_LARGE;
    if (chunks_len && !chunks) return NH_PORTHOME_ERR_ARG;

    nh_porthome_entry e;
    memset(&e, 0, sizeof(e));
    e.path_enc = path_enc;
    e.mode = mode;
    e.uid_hint = uid_hint;
    e.gid_hint = gid_hint;
    e.mtime_ns = mtime_ns;
    e.size = size;
    e.kind = NH_PORTHOME_KIND_FILE;
    if (chunks_len) {
        e.chunks = (nh_porthome_chunk *)malloc(sizeof(*chunks) * chunks_len);
        if (!e.chunks) return NH_PORTHOME_ERR_OOM;
        memcpy(e.chunks, chunks, sizeof(*chunks) * chunks_len);
        e.chunks_len = chunks_len;
    }
    int rc = append_entry(m, &e);
    if (rc != NH_PORTHOME_OK) {
        /* On failure the caller retains ownership of `path_enc` — restore. */
        free(e.chunks);
        e.path_enc = NULL; /* returned to caller */
    }
    return rc;
}

int nh_porthome_manifest_add_dir(nh_porthome_manifest *m,
                                 char *path_enc,
                                 uint32_t mode,
                                 uint32_t uid_hint,
                                 uint32_t gid_hint,
                                 uint64_t mtime_ns) {
    if (!m || !path_enc) return NH_PORTHOME_ERR_ARG;
    nh_porthome_entry e;
    memset(&e, 0, sizeof(e));
    e.path_enc = path_enc;
    e.mode = mode;
    e.uid_hint = uid_hint;
    e.gid_hint = gid_hint;
    e.mtime_ns = mtime_ns;
    e.kind = NH_PORTHOME_KIND_DIR;
    return append_entry(m, &e);
}

int nh_porthome_manifest_add_symlink(nh_porthome_manifest *m,
                                     char *path_enc,
                                     uint32_t mode,
                                     uint32_t uid_hint,
                                     uint32_t gid_hint,
                                     uint64_t mtime_ns,
                                     char *symlink_target) {
    if (!m || !path_enc || !symlink_target) return NH_PORTHOME_ERR_ARG;
    if (strlen(symlink_target) > NH_PORTHOME_MAX_SYMLINK_LEN) return NH_PORTHOME_ERR_TOO_LARGE;
    nh_porthome_entry e;
    memset(&e, 0, sizeof(e));
    e.path_enc = path_enc;
    e.mode = mode;
    e.uid_hint = uid_hint;
    e.gid_hint = gid_hint;
    e.mtime_ns = mtime_ns;
    e.kind = NH_PORTHOME_KIND_SYMLINK;
    e.symlink_target = symlink_target;
    return append_entry(m, &e);
}

/* ────────────── path validity (post-decode) ──────────────
 *
 * The encrypted-path form is a sequence of 48-hex components joined
 * with '/'. A well-formed encoder never emits '..', '.', or an absolute
 * path — but we validate the decoded field anyway (design §8.2 malicious
 * manifest defence).
 */

static int path_ok(const uint8_t *p, size_t n) {
    if (n == 0) return 0;
    if (n > NH_PORTHOME_MAX_PATH_LEN) return 0;
    if (p[0] == '/') return 0;        /* absolute */
    if (p[n - 1] == '/') return 0;    /* trailing slash */
    /* NUL byte / backslash rejected inside components. */
    for (size_t i = 0; i < n; i++) {
        if (p[i] == 0 || p[i] == '\\') return 0;
    }
    /* Reject "." / ".." / any "/./" / "/../" / "/./..." patterns. */
    if (n == 1 && p[0] == '.') return 0;
    if (n == 2 && p[0] == '.' && p[1] == '.') return 0;
    for (size_t i = 0; i < n; i++) {
        int at_start = (i == 0) || (p[i - 1] == '/');
        if (!at_start) continue;
        /* component starts here — measure length until '/' or end */
        size_t j = i;
        while (j < n && p[j] != '/') j++;
        size_t cl = j - i;
        if (cl == 0) return 0;               /* '//' */
        if (cl == 1 && p[i] == '.') return 0;
        if (cl == 2 && p[i] == '.' && p[i + 1] == '.') return 0;
        i = j;
    }
    return 1;
}

/* ────────────── encoder ──────────────
 *
 * Manifest layout, canonical form:
 *   map(3):
 *     1 -> uint(version)
 *     2 -> bstr(home_root_id, 32)
 *     3 -> array(entries_len) of entry-map
 *
 * Entry map:
 *   n pairs, keys strictly ascending in {1,2,3,4,5,6,7} plus optional
 *   8 (chunks) for files and 9 (symlink_target) for symlinks.
 *
 * Chunk map (inside a file entry's chunks array):
 *   map(3): 1 -> bstr(32); 2 -> uint(size); 3 -> uint(chunk_key_id)
 */

static int encode_chunk(nhpc_enc *e, const nh_porthome_chunk *c) {
    if (nhpc_write_map_hdr(e, 3)) return -1;
    if (nhpc_write_uint(e, K_C_SHA256)) return -1;
    if (nhpc_write_bstr(e, c->sha256, sizeof(c->sha256))) return -1;
    if (nhpc_write_uint(e, K_C_SIZE)) return -1;
    if (nhpc_write_uint(e, c->size)) return -1;
    if (nhpc_write_uint(e, K_C_KEY_ID)) return -1;
    if (nhpc_write_uint(e, c->chunk_key_id)) return -1;
    return 0;
}

static int encode_entry(nhpc_enc *e, const nh_porthome_entry *en) {
    /* Fixed 7 keys + optional chunks + optional symlink_target. */
    uint64_t n = 7;
    if (en->kind == NH_PORTHOME_KIND_FILE)    n++;
    if (en->kind == NH_PORTHOME_KIND_SYMLINK) n++;
    if (nhpc_write_map_hdr(e, n)) return -1;

    if (nhpc_write_uint(e, K_E_PATH)) return -1;
    if (nhpc_write_tstr(e, en->path_enc ? en->path_enc : "")) return -1;
    if (nhpc_write_uint(e, K_E_MODE))     return -1;
    if (nhpc_write_uint(e, en->mode))     return -1;
    if (nhpc_write_uint(e, K_E_UID_HINT)) return -1;
    if (nhpc_write_uint(e, en->uid_hint)) return -1;
    if (nhpc_write_uint(e, K_E_GID_HINT)) return -1;
    if (nhpc_write_uint(e, en->gid_hint)) return -1;
    if (nhpc_write_uint(e, K_E_MTIME_NS)) return -1;
    if (nhpc_write_uint(e, en->mtime_ns)) return -1;
    if (nhpc_write_uint(e, K_E_SIZE))     return -1;
    if (nhpc_write_uint(e, en->size))     return -1;
    if (nhpc_write_uint(e, K_E_KIND))     return -1;
    if (nhpc_write_uint(e, (uint64_t)en->kind)) return -1;

    if (en->kind == NH_PORTHOME_KIND_FILE) {
        if (nhpc_write_uint(e, K_E_CHUNKS)) return -1;
        if (nhpc_write_array_hdr(e, en->chunks_len)) return -1;
        for (size_t i = 0; i < en->chunks_len; i++)
            if (encode_chunk(e, &en->chunks[i])) return -1;
    } else if (en->kind == NH_PORTHOME_KIND_SYMLINK) {
        if (nhpc_write_uint(e, K_E_SYMLINK_TGT)) return -1;
        if (nhpc_write_tstr(e, en->symlink_target ? en->symlink_target : "")) return -1;
    }
    return 0;
}

int nh_porthome_manifest_encode(const nh_porthome_manifest *m,
                                uint8_t **out, size_t *out_len) {
    if (!m || !out || !out_len) return NH_PORTHOME_ERR_ARG;
    if (m->entries_len > NH_PORTHOME_MAX_ENTRIES) return NH_PORTHOME_ERR_TOO_LARGE;

    nhpc_enc e;
    if (nhpc_enc_init(&e, 512)) return NH_PORTHOME_ERR_OOM;

    /* map(3) — version, home_root_id, entries */
    if (nhpc_write_map_hdr(&e, 3)) goto fail;
    if (nhpc_write_uint(&e, K_M_VERSION)) goto fail;
    if (nhpc_write_uint(&e, m->version)) goto fail;
    if (nhpc_write_uint(&e, K_M_HOME_ROOT_ID)) goto fail;
    if (nhpc_write_bstr(&e, m->home_root_id, sizeof(m->home_root_id))) goto fail;
    if (nhpc_write_uint(&e, K_M_ENTRIES)) goto fail;
    if (nhpc_write_array_hdr(&e, m->entries_len)) goto fail;
    for (size_t i = 0; i < m->entries_len; i++)
        if (encode_entry(&e, &m->entries[i])) goto fail;

    if (e.len > NH_PORTHOME_MAX_CBOR_BYTES) goto fail; /* refuse >1 MiB */

    if (nhpc_enc_take(&e, out, out_len)) return NH_PORTHOME_ERR_OOM;
    return NH_PORTHOME_OK;

fail:
    nhpc_enc_dispose(&e);
    *out = NULL; *out_len = 0;
    return NH_PORTHOME_ERR_TOO_LARGE;
}

/* ────────────── decoder (strict) ────────────── */

static int decode_chunk(nhpc_dec *d, nh_porthome_chunk *out) {
    uint64_t n;
    if (nhpc_read_map_hdr(d, &n)) return -1;
    if (n != 3) return -1;
    uint64_t last_key = 0; int have_prev = 0;
    int seen[4] = {0}; /* keys 1..3 */
    memset(out, 0, sizeof(*out));
    for (uint64_t i = 0; i < 3; i++) {
        uint64_t k;
        if (nhpc_read_uint(d, &k)) return -1;
        if (k < 1 || k > 3) return -1;
        if (have_prev && k <= last_key) return -1; /* strictly ascending */
        if (seen[k]) return -1;
        seen[k] = 1; last_key = k; have_prev = 1;
        switch (k) {
        case K_C_SHA256: {
            const uint8_t *b; size_t bl;
            if (nhpc_read_bstr(d, &b, &bl) || bl != 32) return -1;
            memcpy(out->sha256, b, 32);
            break;
        }
        case K_C_SIZE: {
            uint64_t v;
            if (nhpc_read_uint(d, &v) || v > UINT32_MAX) return -1;
            out->size = (uint32_t)v;
            break;
        }
        case K_C_KEY_ID: {
            uint64_t v;
            if (nhpc_read_uint(d, &v) || v > UINT32_MAX) return -1;
            if (v != 0) return -1; /* Phase 1: reserved, must be zero. */
            out->chunk_key_id = (uint32_t)v;
            break;
        }
        default: return -1;
        }
    }
    return 0;
}

static int decode_entry(nhpc_dec *d, nh_porthome_entry *out) {
    memset(out, 0, sizeof(*out));
    uint64_t n;
    if (nhpc_read_map_hdr(d, &n)) return -1;
    /* 7 required + up to 2 optional. */
    if (n < 7 || n > 9) return -1;

    /* Read pairs, enforce strictly ascending keys within known set. */
    int seen[10] = {0};
    uint64_t last_key = 0; int have_prev = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t k;
        if (nhpc_read_uint(d, &k)) return -1;
        if (k < 1 || k > 9) return -1;
        if (have_prev && k <= last_key) return -1;
        if (seen[k]) return -1;
        seen[k] = 1; last_key = k; have_prev = 1;
        switch (k) {
        case K_E_PATH: {
            const uint8_t *b; size_t bl;
            if (nhpc_read_tstr(d, &b, &bl)) return -1;
            if (!path_ok(b, bl)) return -1;
            char *s = (char *)malloc(bl + 1);
            if (!s) return -1;
            memcpy(s, b, bl); s[bl] = '\0';
            out->path_enc = s;
            break;
        }
        case K_E_MODE: {
            uint64_t v; if (nhpc_read_uint(d, &v) || v > 0xFFFFu) return -1;
            out->mode = (uint32_t)v; break;
        }
        case K_E_UID_HINT: {
            uint64_t v; if (nhpc_read_uint(d, &v) || v > UINT32_MAX) return -1;
            out->uid_hint = (uint32_t)v; break;
        }
        case K_E_GID_HINT: {
            uint64_t v; if (nhpc_read_uint(d, &v) || v > UINT32_MAX) return -1;
            out->gid_hint = (uint32_t)v; break;
        }
        case K_E_MTIME_NS: {
            uint64_t v; if (nhpc_read_uint(d, &v)) return -1;
            out->mtime_ns = v; break;
        }
        case K_E_SIZE: {
            uint64_t v; if (nhpc_read_uint(d, &v)) return -1;
            out->size = v; break;
        }
        case K_E_KIND: {
            uint64_t v; if (nhpc_read_uint(d, &v)) return -1;
            if (v != NH_PORTHOME_KIND_FILE &&
                v != NH_PORTHOME_KIND_DIR &&
                v != NH_PORTHOME_KIND_SYMLINK) return -1;
            out->kind = (nh_porthome_entry_kind)v;
            break;
        }
        case K_E_CHUNKS: {
            uint64_t ac;
            if (nhpc_read_array_hdr(d, &ac)) return -1;
            if (ac > NH_PORTHOME_MAX_CHUNKS_PER_ENTRY) return -1;
            if (ac == 0) { out->chunks = NULL; out->chunks_len = 0; break; }
            nh_porthome_chunk *arr = (nh_porthome_chunk *)calloc(ac, sizeof(*arr));
            if (!arr) return -1;
            for (uint64_t j = 0; j < ac; j++) {
                if (decode_chunk(d, &arr[j])) { free(arr); return -1; }
            }
            out->chunks = arr; out->chunks_len = (size_t)ac;
            break;
        }
        case K_E_SYMLINK_TGT: {
            const uint8_t *b; size_t bl;
            if (nhpc_read_tstr(d, &b, &bl)) return -1;
            if (bl == 0 || bl > NH_PORTHOME_MAX_SYMLINK_LEN) return -1;
            for (size_t j = 0; j < bl; j++) if (b[j] == 0) return -1;
            char *s = (char *)malloc(bl + 1);
            if (!s) return -1;
            memcpy(s, b, bl); s[bl] = '\0';
            out->symlink_target = s;
            break;
        }
        default: return -1;
        }
    }

    /* Structural cross-checks. */
    if (!out->path_enc) return -1;
    /* kind consistency */
    if (out->kind == NH_PORTHOME_KIND_FILE) {
        /* files may have zero chunks iff size == 0. */
        if (out->size > 0 && (out->chunks_len == 0)) return -1;
        if (out->symlink_target) return -1;
    } else if (out->kind == NH_PORTHOME_KIND_DIR) {
        if (out->chunks_len) return -1;
        if (out->symlink_target) return -1;
        if (out->size != 0) return -1;
    } else if (out->kind == NH_PORTHOME_KIND_SYMLINK) {
        if (out->chunks_len) return -1;
        if (!out->symlink_target) return -1;
        if (out->size != 0) return -1;
    } else {
        return -1;
    }
    return 0;
}

int nh_porthome_manifest_decode(const uint8_t *cbor, size_t cbor_len,
                                nh_porthome_manifest **out_pm) {
    if (!cbor || !out_pm) return NH_PORTHOME_ERR_ARG;
    *out_pm = NULL;
    if (cbor_len == 0 || cbor_len > NH_PORTHOME_MAX_CBOR_BYTES)
        return NH_PORTHOME_ERR_TOO_LARGE;

    nhpc_dec d; nhpc_dec_init(&d, cbor, cbor_len);
    uint64_t n;
    if (nhpc_read_map_hdr(&d, &n)) return NH_PORTHOME_ERR_ARG;
    if (n != 3) return NH_PORTHOME_ERR_ARG;

    nh_porthome_manifest *m = (nh_porthome_manifest *)calloc(1, sizeof(*m));
    if (!m) return NH_PORTHOME_ERR_OOM;

    int seen[4] = {0}; uint64_t last_key = 0; int have_prev = 0;
    for (uint64_t i = 0; i < 3; i++) {
        uint64_t k;
        if (nhpc_read_uint(&d, &k)) goto bad;
        if (k < 1 || k > 3) goto bad;
        if (have_prev && k <= last_key) goto bad;
        if (seen[k]) goto bad;
        seen[k] = 1; last_key = k; have_prev = 1;
        switch (k) {
        case K_M_VERSION: {
            uint64_t v;
            if (nhpc_read_uint(&d, &v)) goto bad;
            if (v != NH_PORTHOME_MANIFEST_SCHEMA_VERSION) goto bad;
            m->version = (uint32_t)v;
            break;
        }
        case K_M_HOME_ROOT_ID: {
            const uint8_t *b; size_t bl;
            if (nhpc_read_bstr(&d, &b, &bl) || bl != 32) goto bad;
            memcpy(m->home_root_id, b, 32);
            break;
        }
        case K_M_ENTRIES: {
            uint64_t ec;
            if (nhpc_read_array_hdr(&d, &ec)) goto bad;
            if (ec > NH_PORTHOME_MAX_ENTRIES) goto bad;
            if (ec) {
                m->entries = (nh_porthome_entry *)calloc(ec, sizeof(*m->entries));
                if (!m->entries) goto bad;
            }
            for (uint64_t j = 0; j < ec; j++) {
                if (decode_entry(&d, &m->entries[j])) goto bad;
                m->entries_len = (size_t)(j + 1);
            }
            break;
        }
        default: goto bad;
        }
    }

    if (!nhpc_dec_at_end(&d)) goto bad; /* refuse trailing bytes */

    *out_pm = m;
    return NH_PORTHOME_OK;

bad:
    nh_porthome_manifest_dispose(m);
    free(m);
    return NH_PORTHOME_ERR_ARG;
}

/* ────────────── convenience: sealed round-trip ────────────── */

int nh_porthome_manifest_encode_sealed(const nh_porthome_manifest *m,
                                       const uint8_t home_key[32],
                                       uint8_t **out, size_t *out_len) {
    if (!m || !home_key || !out || !out_len) return NH_PORTHOME_ERR_ARG;
    *out = NULL; *out_len = 0;
    uint8_t *cbor = NULL; size_t cbor_len = 0;
    int rc = nh_porthome_manifest_encode(m, &cbor, &cbor_len);
    if (rc != NH_PORTHOME_OK) return rc;
    rc = nh_porthome_encrypt_manifest(home_key, cbor, cbor_len, out, out_len);
    free(cbor);
    return rc;
}

int nh_porthome_manifest_decode_sealed(const uint8_t *sealed, size_t sealed_len,
                                       const uint8_t home_key[32],
                                       nh_porthome_manifest **out) {
    if (!sealed || !home_key || !out) return NH_PORTHOME_ERR_ARG;
    *out = NULL;
    uint8_t *pt = NULL; size_t pt_len = 0;
    int rc = nh_porthome_decrypt_manifest(home_key, sealed, sealed_len, &pt, &pt_len);
    if (rc != NH_PORTHOME_OK) return rc;
    rc = nh_porthome_manifest_decode(pt, pt_len, out);
    free(pt);
    return rc;
}
