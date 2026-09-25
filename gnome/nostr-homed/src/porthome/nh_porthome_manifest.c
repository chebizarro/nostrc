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
#define K_E_PATH             1u
#define K_E_MODE             2u
#define K_E_UID_HINT         3u
#define K_E_GID_HINT         4u
#define K_E_MTIME_NS         5u
#define K_E_SIZE             6u
#define K_E_KIND             7u
#define K_E_CHUNKS           8u  /* optional (files only) — v1 & v2 */
#define K_E_SYMLINK_TGT      9u  /* optional (symlinks only) — v1 legacy only */
/* Schema v2 additive keys (nostrc-q25o). Opaque byte-strings on the
 * wire; the decoder does NOT parse inside them. Present in every v2
 * entry (10) and additionally in v2 symlink entries (11). */
#define K_E_NAME_SEALED     10u
#define K_E_LINK_TGT_SEALED 11u

#define K_E_MAX             11u

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
    free(e->name_sealed);
    free(e->name_plain);
    free(e->link_target_sealed);
    free(e->link_target_plain);
    memset(e, 0, sizeof(*e));
}

int nh_porthome_manifest_init(nh_porthome_manifest *out,
                              const uint8_t home_root_id[32]) {
    if (!out) return NH_PORTHOME_ERR_ARG;
    memset(out, 0, sizeof(*out));
    /* Backward-compat: init produces v1-shaped manifests (no name_sealed).
     * Existing callers (syncd's pusher, pre-q25o tests) rely on this.
     * New code that wants v2 should call `nh_porthome_manifest_init_v2`
     * OR set `out->version = NH_PORTHOME_MANIFEST_SCHEMA_VERSION_V2`
     * after init and use the `_v2` add helpers. */
    out->version = NH_PORTHOME_MANIFEST_SCHEMA_VERSION_V1;
    if (home_root_id) memcpy(out->home_root_id, home_root_id, 32);
    return NH_PORTHOME_OK;
}

int nh_porthome_manifest_init_v2(nh_porthome_manifest *out,
                                 const uint8_t home_root_id[32]) {
    int rc = nh_porthome_manifest_init(out, home_root_id);
    if (rc != NH_PORTHOME_OK) return rc;
    out->version = NH_PORTHOME_MANIFEST_SCHEMA_VERSION_V2;
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

/* ── Schema v2 add helpers (nostrc-q25o) ────────────────────────────
 *
 * Same wiring as the v1 helpers, plus a plaintext basename validated
 * against the same shape rules `component_is_ok` in nh_porthome_crypto
 * uses for path_enc: 1..255 bytes, not "." / "..", no '/' '\\' NUL.
 * The encoder AEAD-seals it before emitting; the caller passes the
 * plaintext for capture-then-seal. */

static int name_basename_ok(const char *s) {
    if (!s) return 0;
    size_t n = strlen(s);
    if (n == 0 || n > 255) return 0;
    if (n == 1 && s[0] == '.') return 0;
    if (n == 2 && s[0] == '.' && s[1] == '.') return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\0' || c == '/' || c == '\\') return 0;
    }
    return 1;
}

int nh_porthome_manifest_add_file_v2(nh_porthome_manifest *m,
                                     char *path_enc,
                                     char *name,
                                     uint32_t mode,
                                     uint32_t uid_hint,
                                     uint32_t gid_hint,
                                     uint64_t mtime_ns,
                                     uint64_t size,
                                     const nh_porthome_chunk *chunks,
                                     size_t chunks_len) {
    if (!name || !name_basename_ok(name)) return NH_PORTHOME_ERR_PATH;
    int rc = nh_porthome_manifest_add_file(m, path_enc, mode, uid_hint,
                                           gid_hint, mtime_ns, size,
                                           chunks, chunks_len);
    if (rc != NH_PORTHOME_OK) return rc;
    m->entries[m->entries_len - 1].name_plain = name;
    return NH_PORTHOME_OK;
}

int nh_porthome_manifest_add_dir_v2(nh_porthome_manifest *m,
                                    char *path_enc,
                                    char *name,
                                    uint32_t mode,
                                    uint32_t uid_hint,
                                    uint32_t gid_hint,
                                    uint64_t mtime_ns) {
    if (!name || !name_basename_ok(name)) return NH_PORTHOME_ERR_PATH;
    int rc = nh_porthome_manifest_add_dir(m, path_enc, mode, uid_hint,
                                          gid_hint, mtime_ns);
    if (rc != NH_PORTHOME_OK) return rc;
    m->entries[m->entries_len - 1].name_plain = name;
    return NH_PORTHOME_OK;
}

int nh_porthome_manifest_add_symlink_v2(nh_porthome_manifest *m,
                                        char *path_enc,
                                        char *name,
                                        uint32_t mode,
                                        uint32_t uid_hint,
                                        uint32_t gid_hint,
                                        uint64_t mtime_ns,
                                        char *symlink_target) {
    if (!name || !name_basename_ok(name)) return NH_PORTHOME_ERR_PATH;
    /* Duplicate symlink_target into link_target_plain — the manifest
     * needs BOTH copies for round-trip so `symlink_target` stays the
     * downstream-consumer field (materialiser reads it) and
     * `link_target_plain` mirrors it for the sealer. Freeing is
     * disposed in nh_porthome_entry_dispose. */
    char *tgt_dup = strdup(symlink_target ? symlink_target : "");
    if (!tgt_dup) return NH_PORTHOME_ERR_OOM;
    int rc = nh_porthome_manifest_add_symlink(m, path_enc, mode, uid_hint,
                                              gid_hint, mtime_ns, symlink_target);
    if (rc != NH_PORTHOME_OK) { free(tgt_dup); return rc; }
    nh_porthome_entry *last = &m->entries[m->entries_len - 1];
    last->name_plain = name;
    last->link_target_plain = tgt_dup;
    return NH_PORTHOME_OK;
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

/* Emit an entry. Layout depends on `emit_v2`:
 *
 *   v1 (parse-forward — encoder never uses this in production):
 *     keys 1..7 + 8 (files) / 9 (symlinks).
 *
 *   v2 (nostrc-q25o):
 *     keys 1..7 + 8 (files) + 10 (name_sealed) + 11 (link_tgt_sealed
 *     for symlinks). Key 9 (plaintext K_E_SYMLINK_TGT) is NOT emitted:
 *     the plaintext moved into the sealed slot.
 *
 * `emit_v2` must be TRUE whenever the entry carries name_sealed (i.e.
 * whenever the caller staged plaintext names before encode). The mixed
 * case — some v2 entries and some v1-shaped — is not supported by the
 * encoder; that would produce a v2 manifest with entries that fail the
 * v2-name-required decode gate. */
static int encode_entry(nhpc_enc *e, const nh_porthome_entry *en,
                        int emit_v2) {
    /* Count keys strictly. */
    uint64_t n = 7;
    if (en->kind == NH_PORTHOME_KIND_FILE)    n++;                    /* 8: chunks */
    if (!emit_v2 && en->kind == NH_PORTHOME_KIND_SYMLINK) n++;        /* 9: legacy symlink_target */
    if (emit_v2) {
        n++;                                                          /* 10: name_sealed */
        if (en->kind == NH_PORTHOME_KIND_SYMLINK) n++;                /* 11: link_target_sealed */
    }
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
    } else if (!emit_v2 && en->kind == NH_PORTHOME_KIND_SYMLINK) {
        if (nhpc_write_uint(e, K_E_SYMLINK_TGT)) return -1;
        if (nhpc_write_tstr(e, en->symlink_target ? en->symlink_target : "")) return -1;
    }
    if (emit_v2) {
        /* Name-sealed slot MUST be populated on v2 emit; a caller who
         * forgot to seal names before encoding will trip this. */
        if (!en->name_sealed || en->name_sealed_len == 0) return -1;
        if (nhpc_write_uint(e, K_E_NAME_SEALED)) return -1;
        if (nhpc_write_bstr(e, en->name_sealed, en->name_sealed_len)) return -1;
        if (en->kind == NH_PORTHOME_KIND_SYMLINK) {
            if (!en->link_target_sealed || en->link_target_sealed_len == 0) return -1;
            if (nhpc_write_uint(e, K_E_LINK_TGT_SEALED)) return -1;
            if (nhpc_write_bstr(e, en->link_target_sealed, en->link_target_sealed_len)) return -1;
        }
    }
    return 0;
}

int nh_porthome_manifest_encode(const nh_porthome_manifest *m,
                                uint8_t **out, size_t *out_len) {
    if (!m || !out || !out_len) return NH_PORTHOME_ERR_ARG;
    if (m->entries_len > NH_PORTHOME_MAX_ENTRIES) return NH_PORTHOME_ERR_TOO_LARGE;
    if (m->version != NH_PORTHOME_MANIFEST_SCHEMA_VERSION_V1 &&
        m->version != NH_PORTHOME_MANIFEST_SCHEMA_VERSION_V2)
        return NH_PORTHOME_ERR_VERSION;

    int emit_v2 = (m->version == NH_PORTHOME_MANIFEST_SCHEMA_VERSION_V2);

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
        if (encode_entry(&e, &m->entries[i], emit_v2)) goto fail;

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
    /* 7 required + up to:
     *   v1: K_E_CHUNKS or K_E_SYMLINK_TGT (1 optional) → 8
     *   v2: K_E_CHUNKS + K_E_NAME_SEALED [+ K_E_LINK_TGT_SEALED] → up to 10.
     * A hostile encoder that emits BOTH K_E_SYMLINK_TGT (9) and
     * K_E_LINK_TGT_SEALED (11) on the same symlink would land here
     * with n==10 which is legal; the kind check later rejects that
     * (kind==SYMLINK requires exactly one target field, per the
     * cross-check block). */
    if (n < 7 || n > 10) return -1;

    /* Read pairs, enforce strictly ascending keys within known set. */
    int seen[K_E_MAX + 2] = {0};
    uint64_t last_key = 0; int have_prev = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t k;
        if (nhpc_read_uint(d, &k)) return -1;
        if (k < 1 || k > K_E_MAX) return -1;
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
        case K_E_NAME_SEALED: {
            /* Opaque AEAD blob. Only sanity-check size — the strict
             * CBOR decoder does not open it. `open_names` will. */
            const uint8_t *b; size_t bl;
            if (nhpc_read_bstr(d, &b, &bl)) return -1;
            if (bl == 0) return -1;
            /* Upper bound: 255-byte basename + AEAD overhead. Allow a
             * little slack for future UTF-8 sizes. */
            if (bl > 512) return -1;
            uint8_t *cp = (uint8_t *)malloc(bl);
            if (!cp) return -1;
            memcpy(cp, b, bl);
            out->name_sealed = cp;
            out->name_sealed_len = bl;
            break;
        }
        case K_E_LINK_TGT_SEALED: {
            const uint8_t *b; size_t bl;
            if (nhpc_read_bstr(d, &b, &bl)) return -1;
            if (bl == 0) return -1;
            /* NH_PORTHOME_MAX_SYMLINK_LEN + AEAD overhead. */
            if (bl > NH_PORTHOME_MAX_SYMLINK_LEN + 64u) return -1;
            uint8_t *cp = (uint8_t *)malloc(bl);
            if (!cp) return -1;
            memcpy(cp, b, bl);
            out->link_target_sealed = cp;
            out->link_target_sealed_len = bl;
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
        if (out->link_target_sealed) return -1;
    } else if (out->kind == NH_PORTHOME_KIND_DIR) {
        if (out->chunks_len) return -1;
        if (out->symlink_target) return -1;
        if (out->link_target_sealed) return -1;
        if (out->size != 0) return -1;
    } else if (out->kind == NH_PORTHOME_KIND_SYMLINK) {
        if (out->chunks_len) return -1;
        if (out->size != 0) return -1;
        /* Exactly one target flavour: v1 plaintext (K_E_SYMLINK_TGT)
         * OR v2 sealed (K_E_LINK_TGT_SEALED). Both is a mixed-schema
         * smuggle attempt — refuse. Neither is a broken entry. */
        int has_v1 = out->symlink_target != NULL;
        int has_v2 = out->link_target_sealed != NULL;
        if (has_v1 == has_v2) return -1;
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
            /* Accept both v1 (parse-forward for pre-q25o pushes) and
             * v2 (current). Anything else is a refuse. */
            if (v != NH_PORTHOME_MANIFEST_SCHEMA_VERSION_V1 &&
                v != NH_PORTHOME_MANIFEST_SCHEMA_VERSION_V2) goto bad;
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

    /* Version/entry consistency (schema v2 gate — nostrc-q25o).
     *
     * v1: every entry MUST NOT carry name_sealed or link_target_sealed.
     *     Symlink entries MUST carry the legacy plaintext symlink_target
     *     (already enforced per-entry above).
     * v2: every entry MUST carry name_sealed. Symlink entries MUST
     *     carry link_target_sealed (already partly enforced per-entry
     *     via the exactly-one-flavour rule). K_E_SYMLINK_TGT must NOT
     *     appear in v2 — the plaintext moved into the sealed slot. */
    for (size_t j = 0; j < m->entries_len; j++) {
        const nh_porthome_entry *e = &m->entries[j];
        if (m->version == NH_PORTHOME_MANIFEST_SCHEMA_VERSION_V1) {
            if (e->name_sealed || e->link_target_sealed) goto bad;
        } else { /* v2 */
            if (!e->name_sealed) goto bad;
            if (e->kind == NH_PORTHOME_KIND_SYMLINK) {
                if (!e->link_target_sealed) goto bad;
                if (e->symlink_target) goto bad;
            }
        }
    }

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
    /* Seal plaintext name material into name_sealed / link_target_sealed
     * before the CBOR encoder emits them. The `const` on m is a lie
     * here — we cast it away because sealing is deterministic and
     * idempotent under a fixed home_key (D4 convergence), so no caller
     * observes a semantic mutation. */
    int rc = nh_porthome_manifest_seal_names((nh_porthome_manifest *)m,
                                             home_key);
    if (rc != NH_PORTHOME_OK) return rc;
    uint8_t *cbor = NULL; size_t cbor_len = 0;
    rc = nh_porthome_manifest_encode(m, &cbor, &cbor_len);
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
    if (rc != NH_PORTHOME_OK) return rc;
    /* Open plaintext names for v2 manifests. v1 manifests have no
     * sealed material; `open_names` returns OK with plaintext members
     * left NULL and the fetch materialiser detects that and skips its
     * rename walk (nostrc-bms6). A decrypt failure OR a path-smuggling
     * basename ("../etc/passwd", NUL, '/') aborts the whole decode. */
    int orc = nh_porthome_manifest_open_names(*out, home_key);
    if (orc != NH_PORTHOME_OK) {
        nh_porthome_manifest_dispose(*out);
        free(*out);
        *out = NULL;
        return orc;
    }
    return NH_PORTHOME_OK;
}

/* ────────────── schema-v2 seal / open names ──────────────
 *
 * `seal_names` walks the manifest and, for every entry with a
 * `name_plain` set, replaces `name_sealed` with a fresh AEAD blob
 * under home_key + salt="porthome/v1/name". For symlink entries with
 * `link_target_plain` (or `symlink_target`) set, does the same for
 * `link_target_sealed`. Idempotent-ish: repeated calls produce the
 * same bytes (convergent AEAD).
 *
 * `open_names` reverses. Every decrypted basename is re-validated
 * against `name_basename_ok` — a hostile manifest whose sealed slot
 * decrypts to "../etc/passwd" is refused with NH_PORTHOME_ERR_PATH.
 * Link targets are validated the same way as `apply_symlink`'s target
 * gate (non-empty, no absolute paths, no NUL, ≤ MAX_SYMLINK_LEN). */

int nh_porthome_manifest_seal_names(nh_porthome_manifest *m,
                                    const uint8_t home_key[32]) {
    if (!m || !home_key) return NH_PORTHOME_ERR_ARG;
    for (size_t i = 0; i < m->entries_len; i++) {
        nh_porthome_entry *e = &m->entries[i];
        if (e->name_plain) {
            uint8_t *ns = NULL; size_t nsl = 0;
            int rc = nh_porthome_encrypt_name_field(home_key,
                (const uint8_t *)e->name_plain, strlen(e->name_plain),
                &ns, &nsl);
            if (rc != NH_PORTHOME_OK) return rc;
            free(e->name_sealed);
            e->name_sealed = ns;
            e->name_sealed_len = nsl;
        }
        if (e->kind == NH_PORTHOME_KIND_SYMLINK) {
            const char *tgt = e->link_target_plain
                              ? e->link_target_plain
                              : e->symlink_target;
            if (tgt) {
                uint8_t *ts = NULL; size_t tsl = 0;
                int rc = nh_porthome_encrypt_name_field(home_key,
                    (const uint8_t *)tgt, strlen(tgt), &ts, &tsl);
                if (rc != NH_PORTHOME_OK) return rc;
                free(e->link_target_sealed);
                e->link_target_sealed = ts;
                e->link_target_sealed_len = tsl;
            }
        }
    }
    return NH_PORTHOME_OK;
}

int nh_porthome_manifest_open_names(nh_porthome_manifest *m,
                                    const uint8_t home_key[32]) {
    if (!m || !home_key) return NH_PORTHOME_ERR_ARG;
    for (size_t i = 0; i < m->entries_len; i++) {
        nh_porthome_entry *e = &m->entries[i];
        if (e->name_sealed && !e->name_plain) {
            uint8_t *pt = NULL; size_t pt_len = 0;
            int rc = nh_porthome_decrypt_name_field(home_key,
                e->name_sealed, e->name_sealed_len, &pt, &pt_len);
            if (rc != NH_PORTHOME_OK) return NH_PORTHOME_ERR_CRYPTO;
            /* Length + basename-shape gate. Rejects dot-dot / NUL /
             * '/' / '\\' / "." / "..". */
            if (pt_len == 0 || pt_len > 255) { free(pt); return NH_PORTHOME_ERR_PATH; }
            char *s = (char *)malloc(pt_len + 1);
            if (!s) { free(pt); return NH_PORTHOME_ERR_OOM; }
            memcpy(s, pt, pt_len); s[pt_len] = '\0';
            free(pt);
            if (!name_basename_ok(s)) { free(s); return NH_PORTHOME_ERR_PATH; }
            e->name_plain = s;
        }
        if (e->kind == NH_PORTHOME_KIND_SYMLINK &&
            e->link_target_sealed && !e->link_target_plain) {
            uint8_t *pt = NULL; size_t pt_len = 0;
            int rc = nh_porthome_decrypt_name_field(home_key,
                e->link_target_sealed, e->link_target_sealed_len,
                &pt, &pt_len);
            if (rc != NH_PORTHOME_OK) return NH_PORTHOME_ERR_CRYPTO;
            if (pt_len == 0 || pt_len > NH_PORTHOME_MAX_SYMLINK_LEN) {
                free(pt); return NH_PORTHOME_ERR_PATH;
            }
            /* Reject absolute targets and NUL bytes. Relative-with-".."
             * is allowed (a legitimate symlink like "../shared") but the
             * apply_symlink gate in the provisioner refuses absolute
             * targets; belt-and-braces here too. */
            if (pt[0] == '/') { free(pt); return NH_PORTHOME_ERR_PATH; }
            for (size_t j = 0; j < pt_len; j++) {
                if (pt[j] == '\0') { free(pt); return NH_PORTHOME_ERR_PATH; }
            }
            char *s = (char *)malloc(pt_len + 1);
            if (!s) { free(pt); return NH_PORTHOME_ERR_OOM; }
            memcpy(s, pt, pt_len); s[pt_len] = '\0';
            free(pt);
            e->link_target_plain = s;
            /* Downstream consumers (apply_symlink in the materialiser)
             * read e->symlink_target — mirror the plaintext there so
             * v2 pointers work unchanged. */
            if (!e->symlink_target) e->symlink_target = strdup(s);
            if (!e->symlink_target) return NH_PORTHOME_ERR_OOM;
        }
    }
    return NH_PORTHOME_OK;
}
