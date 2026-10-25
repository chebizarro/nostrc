/* SPDX-License-Identifier: MIT */
#include "vc.h"
#include <porthome/cbor.h>
#include <porthome/crypto.h>
#include <nostr-keys.h>
#include <nostr-event.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

void vc_error(vc_app *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->error, sizeof a->error, fmt, ap);
    va_end(ap);
}
static int private_stat(int fd, int directory)
{
    struct stat s;
    return fstat(fd, &s) || s.st_uid != geteuid() || (s.st_mode & 077) ||
           (directory ? !S_ISDIR(s.st_mode) : !S_ISREG(s.st_mode));
}
int vc_private_read(int dirfd, const char *name, size_t max, uint8_t **out, size_t *len)
{
    *out = NULL;
    *len = 0;
    int fd = openat(dirfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return VAULT_ERR_IO;
    struct stat st;
    int rc = VAULT_ERR_IO;
    if (private_stat(fd, 0) || fstat(fd, &st) || st.st_size < 0 || (uint64_t)st.st_size > max)
        goto done;
    size_t n = (size_t)st.st_size;
    uint8_t *b = calloc(n + 1, 1);
    if (!b) {
        rc = VAULT_ERR_OOM;
        goto done;
    }
    size_t p = 0;
    while (p < n) {
        ssize_t r = read(fd, b + p, n - p);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            OPENSSL_clear_free(b, n + 1);
            goto done;
        }
        p += (size_t)r;
    }
    uint8_t extra;
    if (read(fd, &extra, 1) != 0) {
        OPENSSL_clear_free(b, n + 1);
        goto done;
    }
    *out = b;
    *len = n;
    rc = 0;
done:
    close(fd);
    return rc;
}
int vc_private_write(int dirfd, const char *name, const uint8_t *b, size_t n)
{
    char tmp[96];
    uint8_t rnd[8];
    if (RAND_bytes(rnd, sizeof rnd) != 1)
        return VAULT_ERR_IO;
    char h[17];
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 8; i++) {
        h[2 * i] = hex[rnd[i] >> 4];
        h[2 * i + 1] = hex[rnd[i] & 15];
    }
    h[16] = 0;
    snprintf(tmp, sizeof tmp, ".tmp-%s", h);
    int fd = openat(dirfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return VAULT_ERR_IO;
    size_t p = 0;
    int rc = VAULT_ERR_IO;
    while (p < n) {
        ssize_t w = write(fd, b + p, n - p);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            goto done;
        p += (size_t)w;
    }
    if (fsync(fd) || renameat(dirfd, tmp, dirfd, name) || fsync(dirfd))
        goto done;
    rc = 0;
done:
    close(fd);
    if (rc)
        unlinkat(dirfd, tmp, 0);
    return rc;
}
static int immutable(int fd, const char *name, const uint8_t *b, size_t n)
{
    struct stat st;
    if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        uint8_t *old = NULL;
        size_t z = 0;
        int rc = vc_private_read(fd, name, VC_MAX_STATE, &old, &z);
        if (!rc && (z != n || CRYPTO_memcmp(old, b, n)))
            rc = VAULT_ERR_CONFLICT;
        OPENSSL_clear_free(old, z + 1);
        return rc;
    }
    if (errno != ENOENT)
        return VAULT_ERR_IO;
    return vc_private_write(fd, name, b, n);
}
static int subdir(int fd, const char *name)
{
    if (mkdirat(fd, name, 0700) && errno != EEXIST)
        return -1;
    int d = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (d >= 0 && private_stat(d, 1)) {
        close(d);
        d = -1;
    }
    return d;
}
int vc_state_open(vc_app *a, int create)
{
    vc_state *s = &a->state;
    s->dirfd = s->lockfd = s->objectsfd = s->acksfd = s->ringsfd = -1;
    s->current = SIZE_MAX;
    if (create && mkdir(a->options.state_path, 0700) && errno != EEXIST)
        return VAULT_ERR_IO;
    s->dirfd = open(a->options.state_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (s->dirfd < 0 || private_stat(s->dirfd, 1)) {
        vc_error(a, "state directory must be owned, private and not a symlink");
        return VAULT_ERR_IO;
    }
    s->lockfd = openat(s->dirfd, ".lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (s->lockfd < 0 || private_stat(s->lockfd, 0) || flock(s->lockfd, LOCK_EX | LOCK_NB)) {
        vc_error(a, "state is locked or unsafe");
        return VAULT_ERR_IO;
    }
    s->objectsfd = subdir(s->dirfd, "objects");
    s->acksfd = subdir(s->dirfd, "acks");
    s->ringsfd = subdir(s->dirfd, "rings");
    return s->objectsfd < 0 || s->acksfd < 0 || s->ringsfd < 0 ? VAULT_ERR_IO : 0;
}
int vc_identity_load(vc_app *a)
{
    uint8_t *b = NULL;
    size_t n = 0;
    int rc = vc_private_read(AT_FDCWD, a->options.key_file, 66, &b, &n);
    if (rc)
        return rc;
    if (n == 32)
        memcpy(a->sk, b, 32);
    else {
        if (n == 65 && b[64] == '\n')
            b[--n] = 0;
        if (n != 64 || porthome_from_hex64((char *)b, a->sk))
            rc = VAULT_ERR_ARG;
    }
    OPENSSL_clear_free(b, n + 1);
    if (rc)
        return rc;
    porthome_hex64(a->sk, a->secret);
    char *pub = nostr_key_get_public(a->secret);
    if (!pub)
        return VAULT_ERR_ARG;
    rc = porthome_from_hex64(pub, a->pub);
    if (!rc)
        memcpy(a->public_key, pub, 65);
    free(pub);
    return rc;
}
int vc_cache_put(vc_app *a, const uint8_t hash[32], const uint8_t *b, size_t n)
{
    uint8_t actual[32];
    SHA256(b, n, actual);
    if (CRYPTO_memcmp(actual, hash, 32))
        return VAULT_ERR_AUTH;
    char h[65];
    porthome_hex64(hash, h);
    return immutable(a->state.objectsfd, h, b, n);
}
int vc_cache_get(vc_app *a, const uint8_t hash[32], uint8_t **b, size_t *n)
{
    char h[65];
    porthome_hex64(hash, h);
    int rc = vc_private_read(a->state.objectsfd, h, VC_MAX_STATE, b, n);
    uint8_t actual[32];
    if (!rc) {
        SHA256(*b, *n, actual);
        if (CRYPTO_memcmp(actual, hash, 32)) {
            free(*b);
            *b = NULL;
            *n = 0;
            rc = VAULT_ERR_AUTH;
        }
    }
    return rc;
}
static void be64(uint8_t *b, uint64_t v)
{
    for (int i = 7; i >= 0; i--) {
        b[i] = (uint8_t)v;
        v >>= 8;
    }
}
static uint64_t un64(const uint8_t *b)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | b[i];
    return v;
}
int vc_ack_get(vc_app *a, const uint8_t hash[32], uint64_t *v)
{
    char h[65];
    porthome_hex64(hash, h);
    uint8_t *b = NULL;
    size_t n = 0;
    *v = 0;
    struct stat st;
    if (fstatat(a->state.acksfd, h, &st, AT_SYMLINK_NOFOLLOW) && errno == ENOENT)
        return 0;
    int rc = vc_private_read(a->state.acksfd, h, 8, &b, &n);
    if (!rc && n != 8)
        rc = VAULT_ERR_ARG;
    if (!rc)
        *v = un64(b);
    free(b);
    return rc;
}
int vc_ack_put(vc_app *a, const uint8_t hash[32], uint64_t v)
{
    char h[65];
    porthome_hex64(hash, h);
    uint8_t b[8];
    be64(b, v);
    return vc_private_write(a->state.acksfd, h, b, 8);
}
int vc_head_add(vc_app *a, const NostrEvent *e, uint64_t epoch, size_t *index)
{
    vc_state *s = &a->state;
    const char *id = nostr_event_get_id((NostrEvent *)e);
    if (!id || !nostr_event_check_signature((NostrEvent *)e))
        return VAULT_ERR_AUTH;
    for (size_t i = 0; i < s->heads_len; i++)
        if (!strcmp(nostr_event_get_id(s->heads[i].event), id)) {
            if (s->heads[i].epoch != epoch)
                return VAULT_ERR_CONFLICT;
            if (index)
                *index = i;
            return 0;
        }
    if (s->heads_len == VAULT_MAX_HEADS)
        return VAULT_ERR_INCOMPLETE;
    char *json = nostr_event_serialize(e);
    NostrEvent *copy = nostr_event_new();
    int rc = !json || !copy ? VAULT_ERR_OOM : nostr_event_deserialize(copy, json);
    free(json);
    if (rc) {
        nostr_event_free(copy);
        return VAULT_ERR_ARG;
    }
    size_t i = s->heads_len++;
    s->heads[i] = (vc_head){copy, epoch};
    if (index)
        *index = i;
    return 0;
}
int vc_set_outgoing(vc_app *a, const NostrEvent *e, uint64_t epoch)
{
    if (a->state.outgoing || a->state.intent)
        return VAULT_ERR_AGAIN;
    char *json = nostr_event_serialize(e);
    NostrEvent *copy = nostr_event_new();
    int rc = !json || !copy ? VAULT_ERR_OOM : nostr_event_deserialize(copy, json);
    free(json);
    if (rc) {
        nostr_event_free(copy);
        return VAULT_ERR_ARG;
    }
    a->state.outgoing = copy;
    a->state.outgoing_epoch = epoch;
    a->state.ack = a->state.intent = a->state.rejected = 0;
    return vc_state_save(a);
}
#define WU(v) porthome_cbor_write_uint(&e, (v))
#define WA(v) porthome_cbor_write_array_hdr(&e, (v))
#define WB(b, n) porthome_cbor_write_bstr(&e, (b), (n))
#define WT(s) porthome_cbor_write_tstr(&e, (s))
static int ring_load(vc_app *a, const uint8_t id[32], vault_ring *out)
{
    char name[65];
    porthome_hex64(id, name);
    uint8_t *b = NULL;
    size_t n = 0;
    int rc = vc_private_read(a->state.ringsfd, name, 200000, &b, &n);
    if (rc)
        return VAULT_ERR_INCOMPLETE;
    porthome_cbor_dec d;
    porthome_cbor_dec_init(&d, b, n);
    uint64_t count;
    porthome_ring_entry_t *entries = NULL;
    if (porthome_cbor_read_array_hdr(&d, &count) || !count || count > 4096) {
        rc = VAULT_ERR_ARG;
        goto done;
    }
    entries = calloc(count, sizeof *entries);
    if (!entries) {
        rc = VAULT_ERR_OOM;
        goto done;
    }
    for (size_t i = 0; i < count; i++) {
        uint64_t z;
        const uint8_t *key;
        size_t len;
        if (porthome_cbor_read_array_hdr(&d, &z) || z != 2 ||
            porthome_cbor_read_uint(&d, &entries[i].epoch) ||
            porthome_cbor_read_bstr(&d, &key, &len) || len != 32) {
            rc = VAULT_ERR_ARG;
            goto done;
        }
        memcpy(entries[i].node_key, key, 32);
    }
    if (!porthome_cbor_dec_at_end(&d)) {
        rc = VAULT_ERR_ARG;
        goto done;
    }
    porthome_ring_t view = {entries, count};
    rc = vault_ring_copy(out, &view);
done:
    if (entries)
        OPENSSL_clear_free(entries, count * sizeof *entries);
    OPENSSL_clear_free(b, n + 1);
    return rc;
}
int vc_held_ring(void *ctx, const uint8_t id[32], uint64_t epoch, porthome_ring_t *out)
{
    vc_app *a = ctx;
    memset(out, 0, sizeof *out);
    vault_ring_dispose(&a->resolved_ring);
    int rc = ring_load(a, id, &a->resolved_ring);
    if (rc)
        return rc;
    *out = vault_ring_view(&a->resolved_ring);
    size_t n = 0;
    while (n < out->count && out->entries[n].epoch <= epoch)
        n++;
    out->count = n;
    return n ? 0 : VAULT_ERR_EPOCH;
}
static int ring_retain(vc_app *a, const uint8_t id[32], const porthome_ring_t *held)
{
    vault_ring old = {0};
    int rc = ring_load(a, id, &old);
    if (rc && rc != VAULT_ERR_INCOMPLETE)
        return rc;
    size_t max = old.count + held->count;
    if (max > 8192) {
        vault_ring_dispose(&old);
        return VAULT_ERR_ARG;
    }
    porthome_ring_entry_t *entries = calloc(max, sizeof *entries);
    if (!entries) {
        vault_ring_dispose(&old);
        return VAULT_ERR_OOM;
    }
    size_t n = old.count;
    if (n)
        memcpy(entries, old.entries, n * sizeof *entries);
    for (size_t i = 0; i < held->count; i++) {
        size_t at = 0;
        while (at < n && entries[at].epoch < held->entries[i].epoch)
            at++;
        if (at < n && entries[at].epoch == held->entries[i].epoch) {
            if (CRYPTO_memcmp(entries[at].node_key, held->entries[i].node_key, 32)) {
                rc = VAULT_ERR_CONFLICT;
                goto done;
            }
        } else {
            memmove(entries + at + 1, entries + at, (n - at) * sizeof *entries);
            entries[at] = held->entries[i];
            n++;
        }
    }
    if (old.count == n && !CRYPTO_memcmp(old.entries, entries, n * sizeof *entries)) {
        rc = 0;
        goto done;
    }
    {
        porthome_ring_t view = {entries, n};
        vault_ring valid = {0};
        rc = vault_ring_copy(&valid, &view);
        vault_ring_dispose(&valid);
        if (rc)
            goto done;
        porthome_cbor_enc e;
        if (porthome_cbor_enc_init(&e, 200000)) {
            rc = VAULT_ERR_OOM;
            goto done;
        }
        porthome_cbor_write_array_hdr(&e, n);
        for (size_t i = 0; i < n; i++) {
            porthome_cbor_write_array_hdr(&e, 2);
            porthome_cbor_write_uint(&e, entries[i].epoch);
            porthome_cbor_write_bstr(&e, entries[i].node_key, 32);
        }
        char name[65];
        porthome_hex64(id, name);
        rc = e.err ? VAULT_ERR_ARG : vc_private_write(a->state.ringsfd, name, e.buf, e.len);
        OPENSSL_cleanse(e.buf, e.cap);
        porthome_cbor_enc_dispose(&e);
    }
done:
    OPENSSL_clear_free(entries, max * sizeof *entries);
    vault_ring_dispose(&old);
    return rc;
}
int vc_state_save(vc_app *a)
{
    vc_state *s = &a->state;
    porthome_cbor_enc e;
    /* Private state is bounded; allocate once so encoder growth never leaves old secret copies. */
    if (porthome_cbor_enc_init(&e, VC_MAX_STATE))
        return VAULT_ERR_OOM;
    WA(25);
    WU(1);
    WA(s->relays_len);
    for (size_t i = 0; i < s->relays_len; i++)
        WT(s->relays[i]);
    WA(s->blossom_len);
    for (size_t i = 0; i < s->blossom_len; i++)
        WT(s->blossom[i]);
    WA(vault_tree_count(s->tree));
    for (size_t i = 0; i < vault_tree_count(s->tree); i++) {
        porthome_ring_t ring;
        const porthome_dir_node *node = vault_tree_node_at(s->tree, i, &ring);
        uint8_t *b = NULL;
        size_t n = 0;
        int rc = porthome_dir_node_encode(node, &b, &n);
        if (rc) {
            e.err = rc;
            break;
        }
        if (e.len > VC_MAX_STATE - 2048 || n > VC_MAX_STATE - 2048 - e.len ||
            ring.count > (VC_MAX_STATE - 2048 - e.len - n) / 44) {
            OPENSSL_clear_free(b, n);
            e.err = 1;
            break;
        }
        WA(2);
        WB(b, n);
        OPENSSL_clear_free(b, n);
        WA(ring.count);
        for (size_t j = 0; j < ring.count; j++) {
            WA(2);
            WU(ring.entries[j].epoch);
            WB(ring.entries[j].node_key, 32);
        }
    }
    WA(s->heads_len);
    for (size_t i = 0; i < s->heads_len; i++) {
        char *json = nostr_event_serialize(s->heads[i].event);
        if (!json) {
            e.err = 1;
            break;
        }
        if (e.len > VC_MAX_STATE - 1024 || strlen(json) > VC_MAX_STATE - 1024 - e.len) {
            free(json);
            e.err = 1;
            break;
        }
        WA(2);
        WU(s->heads[i].epoch);
        WT(json);
        free(json);
    }
    WU(s->current);
    WU(s->phase);
    WB(s->member, 32);
    WU(s->cap);
    WU(s->history);
    WU(s->eager);
    WU(s->migrating);
    WU(s->rotation);
    char *json = s->outgoing ? nostr_event_serialize(s->outgoing) : strdup("");
    if (json) {
        if (e.len > VC_MAX_STATE - 1024 || strlen(json) > VC_MAX_STATE - 1024 - e.len)
            e.err = 1;
        WT(json);
        free(json);
    } else
        e.err = 1;
    WU(s->outgoing_epoch);
    WU(s->ack);
    WU(s->intent);
    WU(s->rejected);
    WU(s->pending_blob);
    WB(s->blob_hash, 32);
    WU(s->blob_server);
    WT(s->last_grant);
    WU(s->distribution);
    WU(s->delivery_cursor);
    WU(s->needs_publication);
    int rc = e.err || e.len > VC_MAX_STATE ? VAULT_ERR_ARG
                                           : vc_private_write(s->dirfd, "state.cbor", e.buf, e.len);
    OPENSSL_cleanse(e.buf, e.cap);
    porthome_cbor_enc_dispose(&e);
    /* Commit the proposal before archiving its epochs: an interrupted save
     * must not reserve a random key for an uncommitted future epoch. */
    for (size_t i = 0; !rc && i < vault_tree_count(s->tree); i++) {
        porthome_ring_t r;
        const porthome_dir_node *node = vault_tree_node_at(s->tree, i, &r);
        rc = ring_retain(a, node->node_id, &r);
    }
    return rc;
}
static int arr(porthome_cbor_dec *d, uint64_t expect)
{
    uint64_t n;
    return porthome_cbor_read_array_hdr(d, &n) || n != expect ? VAULT_ERR_ARG : 0;
}
static int text(porthome_cbor_dec *d, char **out, size_t max)
{
    const uint8_t *p;
    size_t n;
    if (porthome_cbor_read_tstr(d, &p, &n) || n > max || memchr(p, 0, n))
        return VAULT_ERR_ARG;
    *out = malloc(n + 1);
    if (!*out)
        return VAULT_ERR_OOM;
    memcpy(*out, p, n);
    (*out)[n] = 0;
    return 0;
}
static int bytes32(porthome_cbor_dec *d, uint8_t out[32])
{
    const uint8_t *b;
    size_t n;
    if (porthome_cbor_read_bstr(d, &b, &n) || n != 32)
        return VAULT_ERR_ARG;
    memcpy(out, b, 32);
    return 0;
}
#define RU(v)                                                                                      \
    do {                                                                                           \
        uint64_t _vc_value;                                                                        \
        if (porthome_cbor_read_uint(&d, &_vc_value)) {                                             \
            rc = VAULT_ERR_ARG;                                                                    \
            goto done;                                                                             \
        }                                                                                          \
        (v) = _vc_value;                                                                           \
        if ((uint64_t)(v) != _vc_value) {                                                          \
            rc = VAULT_ERR_ARG;                                                                    \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
#define REQUIRE(test)                                                                              \
    do {                                                                                           \
        if (test) {                                                                                \
            rc = VAULT_ERR_ARG;                                                                    \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
int vc_state_load(vc_app *a)
{
    vc_state *s = &a->state;
    uint8_t *b = NULL;
    size_t n = 0;
    int rc = vc_private_read(s->dirfd, "state.cbor", VC_MAX_STATE, &b, &n);
    if (rc)
        return rc;
    porthome_cbor_dec d;
    porthome_cbor_dec_init(&d, b, n);
    vault_tree_node *nodes = NULL;
    size_t count = 0;
    uint64_t x;
    REQUIRE(arr(&d, 25));
    RU(x);
    REQUIRE(x != 1);
    REQUIRE(porthome_cbor_read_array_hdr(&d, &x) || !x || x > VC_MAX_ENDPOINTS);
    s->relays_len = x;
    for (size_t i = 0; i < x; i++)
        REQUIRE(text(&d, &s->relays[i], VAULT_MAX_URL));
    REQUIRE(porthome_cbor_read_array_hdr(&d, &x) || !x || x > VC_MAX_ENDPOINTS);
    s->blossom_len = x;
    for (size_t i = 0; i < x; i++)
        REQUIRE(text(&d, &s->blossom[i], VAULT_MAX_URL));
    REQUIRE(porthome_cbor_read_array_hdr(&d, &x) || !x || x > VAULT_MAX_OBJECTS);
    count = x;
    nodes = calloc(count, sizeof *nodes);
    if (!nodes) {
        rc = VAULT_ERR_OOM;
        goto done;
    }
    for (size_t i = 0; i < count; i++) {
        const uint8_t *p;
        size_t z;
        porthome_dir_node *node = NULL;
        REQUIRE(arr(&d, 2) || porthome_cbor_read_bstr(&d, &p, &z) ||
                porthome_dir_node_decode(p, z, &node));
        nodes[i].node = node;
        REQUIRE(porthome_cbor_read_array_hdr(&d, &x) || !x || x > 4096);
        porthome_ring_entry_t *entries = calloc(x, sizeof *entries);
        if (!entries) {
            rc = VAULT_ERR_OOM;
            goto done;
        }
        nodes[i].ring = (porthome_ring_t){entries, x};
        for (size_t j = 0; j < nodes[i].ring.count; j++) {
            REQUIRE(arr(&d, 2));
            RU(entries[j].epoch);
            REQUIRE(bytes32(&d, entries[j].node_key));
        }
    }
    REQUIRE(vault_tree_new(nodes, count, &s->tree));
    REQUIRE(porthome_cbor_read_array_hdr(&d, &x) || x > VAULT_MAX_HEADS);
    for (size_t i = 0; i < x; i++) {
        char *json = NULL;
        uint64_t epoch;
        REQUIRE(arr(&d, 2));
        RU(epoch);
        REQUIRE(text(&d, &json, VC_MAX_STATE));
        NostrEvent *ev = nostr_event_new();
        int bad = !ev || nostr_event_deserialize(ev, json) || vc_head_add(a, ev, epoch, NULL);
        free(json);
        nostr_event_free(ev);
        REQUIRE(bad);
    }
    RU(s->current);
    RU(s->phase);
    REQUIRE(bytes32(&d, s->member));
    RU(s->cap);
    RU(s->history);
    RU(s->eager);
    RU(s->migrating);
    RU(s->rotation);
    char *json = NULL;
    REQUIRE(text(&d, &json, VC_MAX_STATE));
    if (*json) {
        s->outgoing = nostr_event_new();
        int bad = !s->outgoing || nostr_event_deserialize(s->outgoing, json) ||
                  !nostr_event_check_signature(s->outgoing);
        free(json);
        REQUIRE(bad);
    } else
        free(json);
    RU(s->outgoing_epoch);
    RU(s->ack);
    RU(s->intent);
    RU(s->rejected);
    RU(s->pending_blob);
    REQUIRE(bytes32(&d, s->blob_hash));
    RU(s->blob_server);
    json = NULL;
    REQUIRE(text(&d, &json, 64));
    strcpy(s->last_grant, json);
    free(json);
    RU(s->distribution);
    RU(s->delivery_cursor);
    RU(s->needs_publication);
    REQUIRE(!porthome_cbor_dec_at_end(&d) || s->phase > 3 || s->history > 1 || s->eager > 1 ||
            s->migrating > 1 || s->rotation > 1 || s->distribution > 1 ||
            s->needs_publication > 1 || s->pending_blob > 1 || s->blob_server >= s->blossom_len ||
            (s->current != SIZE_MAX && s->current >= s->heads_len) ||
            ((s->ack | s->intent | s->rejected) >> s->relays_len));
    rc = 0;
    /* Complete archival after a crash following state.cbor's atomic commit. */
    for (size_t i = 0; !rc && i < vault_tree_count(s->tree); i++) {
        porthome_ring_t r;
        const porthome_dir_node *node = vault_tree_node_at(s->tree, i, &r);
        rc = ring_retain(a, node->node_id, &r);
    }
done:
    for (size_t i = 0; i < count && nodes; i++) {
        porthome_dir_node *node = (porthome_dir_node *)nodes[i].node;
        if (node) {
            porthome_dir_node_dispose(node);
            free(node);
        }
        if (nodes[i].ring.entries)
            OPENSSL_clear_free((void *)nodes[i].ring.entries,
                               nodes[i].ring.count * sizeof *nodes[i].ring.entries);
    }
    free(nodes);
    OPENSSL_clear_free(b, n + 1);
    return rc;
}
void vc_state_free(vc_state *s)
{
    vault_tree_free(s->tree);
    for (size_t i = 0; i < s->heads_len; i++)
        nostr_event_free(s->heads[i].event);
    nostr_event_free(s->outgoing);
    for (size_t i = 0; i < s->relays_len; i++)
        free(s->relays[i]);
    for (size_t i = 0; i < s->blossom_len; i++)
        free(s->blossom[i]);
    int fds[] = {s->ringsfd, s->acksfd, s->objectsfd, s->lockfd, s->dirfd};
    for (size_t i = 0; i < sizeof fds / sizeof fds[0]; i++)
        if (fds[i] >= 0)
            close(fds[i]);
    OPENSSL_cleanse(s, sizeof *s);
}
