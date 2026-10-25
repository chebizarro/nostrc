/* SPDX-License-Identifier: MIT */
#include <porthome/dir-node.h>
#include <porthome/cbor.h>
#include <openssl/crypto.h>
#include <stdlib.h>
#include <string.h>

static int tag_valid(const char name[PORTHOME_NAME_HEX_LEN]) {
    for (size_t i = 0; i < 48; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') ||
              (name[i] >= 'a' && name[i] <= 'f'))) return 0;
    return name[48] == 0;
}
static int utf8_valid(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = s[i++], count, value, minimum;
        if (c < 0x80) continue;
        if (c >= 0xc2 && c <= 0xdf) { count=1; value=c&31; minimum=0x80; }
        else if (c >= 0xe0 && c <= 0xef) { count=2; value=c&15; minimum=0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { count=3; value=c&7; minimum=0x10000; }
        else return 0;
        if (count > n-i) return 0;
        while (count--) {
            unsigned next = s[i++];
            if ((next&0xc0) != 0x80) return 0;
            value = (value<<6) | (next&63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return 0;
    }
    return 1;
}
static int name_valid(const char s[256]) {
    size_t n = strnlen(s,256);
    return n && n <= 255 && !strchr(s,'/') && !strchr(s,'\\') &&
        strcmp(s,".") && strcmp(s,"..") && utf8_valid((const unsigned char *)s,n);
}
static int link_valid(const char *s) {
    if (!s) return 0;
    size_t n = strnlen(s, PORTHOME_MAX_SYMLINK_LEN + 1);
    if (!n || n > PORTHOME_MAX_SYMLINK_LEN || s[0] == '/' || s[n-1] == '/' ||
        !utf8_valid((const unsigned char *)s,n)) return 0;
    const char *start = s;
    for (size_t i = 0; i <= n; ++i) {
        if (s[i] == '\\') return 0;
        if (s[i] == '/' || !s[i]) {
            size_t part = (size_t)(s + i - start);
            if (!part || (part == 1 && start[0] == '.') ||
                (part == 2 && start[0] == '.' && start[1] == '.')) return 0;
            start = s + i + 1;
        }
    }
    return 1;
}
static int compare_names(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static int node_valid(const porthome_dir_node *n) {
    if (!n || !n->epoch || (n->entries_len && !n->entries)) return PORTHOME_ERR_ARG;
    if (n->entries_len > PORTHOME_MAX_ENTRIES) return PORTHOME_ERR_TOO_LARGE;
    if (n->vault) {
        const porthome_vault_info *v = n->vault;
        if (v->writers_len > PORTHOME_VAULT_MAX_MEMBERS ||
            v->readers_len > PORTHOME_VAULT_MAX_MEMBERS) return PORTHOME_ERR_TOO_LARGE;
        if ((v->writers_len && !v->writers) || (v->readers_len && !v->readers)) return PORTHOME_ERR_ARG;
    }
    for (size_t i = 0; i < n->entries_len; ++i) {
        const porthome_node_entry *e = &n->entries[i];
        if (!tag_valid(e->name_enc) || !name_valid(e->name_plain) || e->has_key_override < 0 || e->has_key_override > 1)
            return PORTHOME_ERR_ARG;
        if (e->kind == PORTHOME_KIND_DIR) {
            if (!e->child_epoch || e->chunks_len || e->chunks || e->symlink_target || e->size_plain)
                return PORTHOME_ERR_ARG;
        } else if (e->kind == PORTHOME_KIND_FILE) {
            if (e->symlink_target || e->child_epoch || e->has_key_override ||
                (e->chunks_len && !e->chunks) || (e->size_plain && !e->chunks_len)) return PORTHOME_ERR_ARG;
            if (e->chunks_len > PORTHOME_NODE_MAX_CHUNKS) return PORTHOME_ERR_TOO_LARGE;
            uint64_t total = 0;
            for (size_t j = 0; j < e->chunks_len; ++j) {
                const porthome_node_chunk *c = &e->chunks[j];
                if (!c->chunk_key_id || c->size_sealed < PORTHOME_SEAL_OVERHEAD ||
                    UINT64_MAX-total < c->size_plain) return PORTHOME_ERR_ARG;
                total += c->size_plain;
            }
            if (total != e->size_plain) return PORTHOME_ERR_ARG;
        } else if (e->kind == PORTHOME_KIND_SYMLINK) {
            if (e->chunks || e->chunks_len || e->child_epoch || e->has_key_override ||
                e->size_plain || !link_valid(e->symlink_target)) return PORTHOME_ERR_ARG;
        } else return PORTHOME_ERR_ARG;
    }
    if (n->entries_len > 1) {
        const char **names = malloc(n->entries_len * sizeof(*names));
        if (!names) return PORTHOME_ERR_OOM;
        for (size_t i = 0; i < n->entries_len; ++i) names[i] = n->entries[i].name_enc;
        qsort(names, n->entries_len, sizeof(*names), compare_names);
        int duplicate = 0;
        for (size_t i = 1; i < n->entries_len; ++i)
            if (!strcmp(names[i-1], names[i])) { duplicate = 1; break; }
        if (!duplicate) {
            for(size_t i=0;i<n->entries_len;i++)names[i]=n->entries[i].name_plain;
            qsort(names,n->entries_len,sizeof(*names),compare_names);
            for(size_t i=1;i<n->entries_len;i++)if(!strcmp(names[i-1],names[i])){duplicate=1;break;}
        }
        free(names);
        if (duplicate) return PORTHOME_ERR_ARG;
    }
    return PORTHOME_OK;
}

#define WU(k,v) do { porthome_cbor_write_uint(&e,k); porthome_cbor_write_uint(&e,v); } while (0)
#define WB(k,v) do { porthome_cbor_write_uint(&e,k); porthome_cbor_write_bstr(&e,v,32); } while (0)
int porthome_dir_node_encode(const porthome_dir_node *n, uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!out || !out_len) return PORTHOME_ERR_ARG;
    int rc = node_valid(n);
    if (rc) return rc;
    porthome_cbor_enc e;
    if (porthome_cbor_enc_init(&e, 256)) return PORTHOME_ERR_OOM;
    porthome_cbor_write_map_hdr(&e, n->vault ? 5 : 4);
    WU(0,PORTHOME_DIR_NODE_VERSION); WB(1,n->node_id); WU(2,n->epoch);
    porthome_cbor_write_uint(&e,3); porthome_cbor_write_array_hdr(&e,n->entries_len);
    for (size_t i = 0; i < n->entries_len && !e.err; ++i) {
        const porthome_node_entry *v = &n->entries[i];
        porthome_cbor_write_map_hdr(&e,7);
        porthome_cbor_write_uint(&e,0); porthome_cbor_write_tstr(&e,v->name_enc);
        WU(1,v->kind); WU(2,v->mode); WU(3,v->mtime_ns); WU(4,v->size_plain);
        porthome_cbor_write_uint(&e,5);
        if (v->kind == PORTHOME_KIND_DIR) {
            porthome_cbor_write_map_hdr(&e,v->has_key_override ? 4 : 3);
            WB(0,v->node_id); WU(1,v->child_epoch); WB(2,v->manifest_sha256);
            if (v->has_key_override) WB(3,v->key_override);
        } else if (v->kind == PORTHOME_KIND_FILE) {
            porthome_cbor_write_array_hdr(&e,v->chunks_len);
            for (size_t j = 0; j < v->chunks_len; ++j) {
                porthome_cbor_write_map_hdr(&e,4);
                WB(0,v->chunks[j].sha256); WU(1,v->chunks[j].size_sealed); WU(2,v->chunks[j].chunk_key_id); WU(3,v->chunks[j].size_plain);
            }
        } else porthome_cbor_write_tstr(&e,v->symlink_target);
        porthome_cbor_write_uint(&e,6); porthome_cbor_write_tstr(&e,v->name_plain);
        if (e.len > PORTHOME_MAX_CBOR_BYTES) { rc = PORTHOME_ERR_TOO_LARGE; goto done; }
    }
    if (n->vault) {
        const porthome_vault_info *v = n->vault;
        porthome_cbor_write_uint(&e,4); porthome_cbor_write_map_hdr(&e,5);
        WB(0,v->vault_id); WB(1,v->owner);
        porthome_cbor_write_uint(&e,2); porthome_cbor_write_array_hdr(&e,v->writers_len);
        for (size_t i = 0; i < v->writers_len; ++i) porthome_cbor_write_bstr(&e,v->writers[i],32);
        porthome_cbor_write_uint(&e,3); porthome_cbor_write_array_hdr(&e,v->readers_len);
        for (size_t i = 0; i < v->readers_len; ++i) porthome_cbor_write_bstr(&e,v->readers[i],32);
        WU(4,v->grants_rev);
    }
    if (e.len > PORTHOME_MAX_CBOR_BYTES) rc = PORTHOME_ERR_TOO_LARGE;
    else rc = porthome_cbor_enc_take(&e,out,out_len) ? PORTHOME_ERR_OOM : PORTHOME_OK;
done:
    if (e.buf) OPENSSL_cleanse(e.buf,e.cap);
    porthome_cbor_enc_dispose(&e);
    return rc;
}
#undef WU
#undef WB

static int key(porthome_cbor_dec *d, uint64_t want) {
    uint64_t got;
    return porthome_cbor_read_uint(d,&got) || got != want;
}
static int bytes32(porthome_cbor_dec *d, uint8_t out[32]) {
    const uint8_t *p; size_t n;
    if (porthome_cbor_read_bstr(d,&p,&n) || n != 32) return -1;
    memcpy(out,p,32); return 0;
}
static int members(porthome_cbor_dec *d, uint8_t (**out)[32], size_t *len) {
    uint64_t n;
    if (porthome_cbor_read_array_hdr(d,&n) || n > PORTHOME_VAULT_MAX_MEMBERS || n > (d->len-d->pos)/34) return -1;
    if (n && !(*out = calloc((size_t)n,32))) return PORTHOME_ERR_OOM;
    *len = (size_t)n;
    for (size_t i = 0; i < *len; ++i) if (bytes32(d,(*out)[i])) return -1;
    return 0;
}
static int vault_read(porthome_cbor_dec *d, porthome_vault_info *v) {
    uint64_t n;
    if (porthome_cbor_read_map_hdr(d,&n) || n != 5 ||
        key(d,0) || bytes32(d,v->vault_id) || key(d,1) || bytes32(d,v->owner) || key(d,2)) return -1;
    int rc = members(d,&v->writers,&v->writers_len);
    if (rc) return rc;
    if (key(d,3)) return -1;
    rc = members(d,&v->readers,&v->readers_len);
    if (rc) return rc;
    return key(d,4) || porthome_cbor_read_uint(d,&v->grants_rev);
}
static int entry_read(porthome_cbor_dec *d, porthome_node_entry *v) {
    uint64_t n, value; const uint8_t *s; size_t len;
    if (porthome_cbor_read_map_hdr(d,&n) || n != 7 || key(d,0) ||
        porthome_cbor_read_tstr(d,&s,&len) || len != 48) return -1;
    memcpy(v->name_enc,s,48);
    if (key(d,1) || porthome_cbor_read_uint(d,&value) || value < 1 || value > 3) return -1;
    v->kind = (porthome_entry_kind)value;
    if (key(d,2) || porthome_cbor_read_uint(d,&value) || value > UINT32_MAX) return -1;
    v->mode = (uint32_t)value;
    if (key(d,3) || porthome_cbor_read_uint(d,&v->mtime_ns) ||
        key(d,4) || porthome_cbor_read_uint(d,&v->size_plain) || key(d,5)) return -1;
    if (v->kind == PORTHOME_KIND_DIR) {
        if (porthome_cbor_read_map_hdr(d,&n) || (n != 3 && n != 4) ||
            key(d,0) || bytes32(d,v->node_id) || key(d,1) || porthome_cbor_read_uint(d,&v->child_epoch) ||
            key(d,2) || bytes32(d,v->manifest_sha256)) return -1;
        if (n == 4) {
            v->has_key_override = 1;
            if (key(d,3) || bytes32(d,v->key_override)) return -1;
        }
    } else if (v->kind == PORTHOME_KIND_FILE) {
        if (porthome_cbor_read_array_hdr(d,&n) || n > PORTHOME_NODE_MAX_CHUNKS ||
            n > (d->len-d->pos)/40) return -1;
        if (n && !(v->chunks = calloc((size_t)n,sizeof(*v->chunks)))) return PORTHOME_ERR_OOM;
        v->chunks_len = (size_t)n;
        for (size_t j = 0; j < v->chunks_len; ++j) {
            porthome_node_chunk *c = &v->chunks[j];
            if (porthome_cbor_read_map_hdr(d,&n) || n != 4 || key(d,0) || bytes32(d,c->sha256) ||
                key(d,1) || porthome_cbor_read_uint(d,&c->size_sealed) ||
                key(d,2) || porthome_cbor_read_uint(d,&c->chunk_key_id) ||
                key(d,3) || porthome_cbor_read_uint(d,&c->size_plain)) return -1;
        }
    } else {
        if (porthome_cbor_read_tstr(d,&s,&len) || !len || len > PORTHOME_MAX_SYMLINK_LEN || memchr(s,0,len)) return -1;
        v->symlink_target = malloc(len+1);
        if (!v->symlink_target) return PORTHOME_ERR_OOM;
        memcpy(v->symlink_target,s,len); v->symlink_target[len] = 0;
    }
    if(key(d,6) || porthome_cbor_read_tstr(d,&s,&len) || !len || len>255 || memchr(s,0,len))return -1;
    memcpy(v->name_plain,s,len);v->name_plain[len]=0;
    return 0;
}
void porthome_dir_node_dispose(porthome_dir_node *n) {
    if (!n) return;
    for (size_t i = 0; i < n->entries_len; ++i) {
        free(n->entries[i].chunks); free(n->entries[i].symlink_target);
    }
    if (n->entries) OPENSSL_cleanse(n->entries,n->entries_len * sizeof(*n->entries));
    free(n->entries);
    if (n->vault) { free(n->vault->writers); free(n->vault->readers); free(n->vault); }
    OPENSSL_cleanse(n,sizeof(*n));
}
int porthome_dir_node_decode(const uint8_t *cbor, size_t len, porthome_dir_node **out) {
    if (out) *out = NULL;
    if (!cbor || !out) return PORTHOME_ERR_ARG;
    if (len > PORTHOME_MAX_CBOR_BYTES) return PORTHOME_ERR_TOO_LARGE;
    porthome_cbor_dec d; porthome_cbor_dec_init(&d,cbor,len);
    uint64_t count, version, entries;
    if (porthome_cbor_read_map_hdr(&d,&count) || (count != 4 && count != 5) ||
        key(&d,0) || porthome_cbor_read_uint(&d,&version)) return PORTHOME_ERR_ARG;
    if (version != PORTHOME_DIR_NODE_VERSION) return PORTHOME_ERR_VERSION;
    porthome_dir_node *n = calloc(1,sizeof(*n));
    if (!n) return PORTHOME_ERR_OOM;
    int rc = PORTHOME_ERR_ARG;
    if (key(&d,1) || bytes32(&d,n->node_id) || key(&d,2) || porthome_cbor_read_uint(&d,&n->epoch) ||
        key(&d,3) || porthome_cbor_read_array_hdr(&d,&entries) || entries > PORTHOME_MAX_ENTRIES ||
        entries > (d.len-d.pos)/48) goto done;
    if (entries && !(n->entries = calloc((size_t)entries,sizeof(*n->entries)))) { rc = PORTHOME_ERR_OOM; goto done; }
    n->entries_len = (size_t)entries;
    for (size_t i = 0; i < n->entries_len; ++i) {
        int nested = entry_read(&d,&n->entries[i]);
        if (nested) { rc = nested == PORTHOME_ERR_OOM ? PORTHOME_ERR_OOM : PORTHOME_ERR_ARG; goto done; }
    }
    if (count == 5) {
        if (key(&d,4)) goto done;
        n->vault = calloc(1,sizeof(*n->vault));
        if (!n->vault) { rc = PORTHOME_ERR_OOM; goto done; }
        int nested = vault_read(&d,n->vault);
        if (nested) { rc = nested == PORTHOME_ERR_OOM ? PORTHOME_ERR_OOM : PORTHOME_ERR_ARG; goto done; }
    }
    if (!porthome_cbor_dec_at_end(&d)) goto done;
    rc = node_valid(n);
    if (!rc) { *out = n; return PORTHOME_OK; }
done:
    porthome_dir_node_dispose(n); free(n); return rc;
}
int porthome_dir_node_seal(const porthome_dir_node *n, const uint8_t node_key[32],
    uint8_t **out, size_t *out_len, uint8_t address[32]) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!node_key || !out || !out_len) return PORTHOME_ERR_ARG;
    uint8_t *cbor = NULL; size_t len = 0;
    int rc = porthome_dir_node_encode(n,&cbor,&len);
    if (!rc) rc = porthome_encrypt_manifest_v2(node_key,cbor,len,out,out_len,address);
    if (cbor) OPENSSL_cleanse(cbor,len);
    free(cbor); return rc;
}
int porthome_dir_node_open(const porthome_ring_t *ring, uint64_t epoch,
    const uint8_t *blob, size_t len, porthome_dir_node **out) {
    if (out) *out = NULL;
    if (!out) return PORTHOME_ERR_ARG;
    if (len > PORTHOME_MAX_CBOR_BYTES + PORTHOME_SEAL_OVERHEAD) return PORTHOME_ERR_TOO_LARGE;
    uint8_t *cbor = NULL; size_t size = 0;
    int rc = porthome_decrypt_manifest_v2(ring,epoch,blob,len,&cbor,&size);
    if (!rc) rc = porthome_dir_node_decode(cbor,size,out);
    if (!rc && (*out)->epoch != epoch) {
        porthome_dir_node_dispose(*out); free(*out); *out = NULL; rc = PORTHOME_ERR_EPOCH;
    }
    if (cbor) OPENSSL_cleanse(cbor,size);
    free(cbor); return rc;
}
