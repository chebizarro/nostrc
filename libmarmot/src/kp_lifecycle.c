/*
 * libmarmot - KeyPackage transport lifecycle (nostrc-0bdg)
 *
 * When the private material of the account's own KeyPackages is deleted
 * (marmot foundation/key-packages.md "Selection and lifecycle", "Failure
 * behavior"; transports/nostr.md "KeyPackage publication", "Publish
 * targets and acknowledgements"):
 *
 *  - Replacement is acknowledgement-tied. Every KeyPackage reuses the
 *    account's one publication slot (`d`), so a newer one replaces the older
 *    ones on relays. Only when a relay accepted a newer KeyPackage (NIP-01
 *    OK true, marmot_key_package_confirm_published()) is the private
 *    material of every older one deleted -- the confirmed replacement bound,
 *    for last-resort KeyPackages too. Until then a delayed Welcome to the old
 *    one still opens; after it, such a Welcome fails (the spec's deliberate
 *    confidentiality-versus-availability trade-off).
 *  - A newer KeyPackage made but not confirmed yet (its publish failed, or
 *    is in flight) stays: it may have reached a relay without its OK.
 *  - A successful Welcome deletes the private material of a consumed
 *    non-last-resort KeyPackage at once; a last-resort one survives use
 *    until the bound above, or its Lifetime.
 *  - At the Lifetime's not_after the private material goes in any state
 *    (marmot_key_package_sweep_expired()).
 *  - A failed Welcome changes nothing.
 *
 * The slot's entries are kept per account in the backend's MLS key store
 * (label "kp_life", key the owner pubkey), as the slot id itself is: no
 * storage schema change. An account whose KeyPackages predate the record
 * gets one seeded from find_key_packages_by_pubkey() (oldest first, all
 * unconfirmed), so a confirmed replacement retires them too.
 *
 * SPDX-License-Identifier: MIT
 */

#include "kp_lifecycle.h"
#include "kp_profile.h"
#include "mls/mls-internal.h"
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

#define KP_LIFE_LABEL     "kp_life"
#define KP_LIFE_VERSION   1
#define KP_LIFE_MAX       32       /* entries per account, at most */
#define KP_LIFE_ENTRY_LEN (32 + 8 + 8 + 1)
#define KP_LIFE_HEAD_LEN  (1 + 8 + 8 + 2)

enum {
    KP_LIFE_LAST_RESORT = 1 << 0,
    KP_LIFE_CONFIRMED   = 1 << 1,   /* a relay accepted it */
    KP_LIFE_CONSUMED    = 1 << 2,   /* a Welcome to it was joined */
};

typedef struct {
    uint8_t  ref[32];
    uint64_t seq;          /* creation order within the account */
    uint64_t not_after;    /* Lifetime; 0: unknown */
    uint8_t  flags;
} KpLifeEntry;

typedef struct {
    uint64_t    next_seq;
    int64_t     last_created_at;   /* the slot's newest event created_at */
    size_t      count;
    KpLifeEntry entries[KP_LIFE_MAX];
} KpLife;

/* ── Last-resort marker ───────────────────────────────────────────────── */

bool
marmot_kp_is_last_resort(const MlsKeyPackage *kp)
{
    if (!kp) return false;
    const uint8_t *data = NULL;
    size_t len = 0, count = 0;
    /* Legacy (MDK 0.8 profile): the last_resort extension type. */
    if (marmot_extensions_find(kp->extensions_data, kp->extensions_len, 0x000A, &data, &len,
                               &count) == 0 && count > 0)
        return true;
    /* Adopted: an empty last_resort_key_package entry of the KeyPackage's
     * own app_data_dictionary. */
    if (marmot_extensions_find(kp->extensions_data, kp->extensions_len,
                               MARMOT_EXT_APP_DATA_DICTIONARY, &data, &len, &count) != 0 ||
        count != 1)
        return false;
    MarmotComponentData *entries = NULL;
    size_t n = 0;
    if (marmot_app_data_dict_parse(data, len, &entries, &n) != 0) return false;
    bool found = false;
    for (size_t i = 0; i < n; i++)
        if (entries[i].component_id == MARMOT_COMPONENT_LAST_RESORT_KP && entries[i].len == 0)
            found = true;
    free(entries);
    return found;
}

/* ── Record (de)serialization ─────────────────────────────────────────── */

static void
put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; }
}

static uint64_t
get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static MarmotError
life_save(Marmot *m, const uint8_t owner[32], const KpLife *life)
{
    uint8_t buf[KP_LIFE_HEAD_LEN + KP_LIFE_MAX * KP_LIFE_ENTRY_LEN];
    buf[0] = KP_LIFE_VERSION;
    put_u64(buf + 1, life->next_seq);
    put_u64(buf + 9, (uint64_t)life->last_created_at);
    buf[17] = (uint8_t)(life->count >> 8);
    buf[18] = (uint8_t)life->count;
    uint8_t *p = buf + KP_LIFE_HEAD_LEN;
    for (size_t i = 0; i < life->count; i++, p += KP_LIFE_ENTRY_LEN) {
        const KpLifeEntry *e = &life->entries[i];
        memcpy(p, e->ref, 32);
        put_u64(p + 32, e->seq);
        put_u64(p + 40, e->not_after);
        p[48] = e->flags;
    }
    return m->storage->mls_store(m->storage->ctx, KP_LIFE_LABEL, owner, 32, buf,
                                 KP_LIFE_HEAD_LEN + life->count * KP_LIFE_ENTRY_LEN);
}

/* *found false (and an empty record) when the account has none yet. */
static MarmotError
life_load(Marmot *m, const uint8_t owner[32], KpLife *life, bool *found)
{
    memset(life, 0, sizeof(*life));
    *found = false;
    uint8_t *data = NULL;
    size_t len = 0;
    MarmotError err = m->storage->mls_load(m->storage->ctx, KP_LIFE_LABEL, owner, 32, &data,
                                           &len);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND) {
        free(data);
        return MARMOT_OK;
    }
    if (err != MARMOT_OK) {
        free(data);
        return err;
    }
    err = MARMOT_ERR_STORAGE;
    if (!data || len < KP_LIFE_HEAD_LEN || data[0] != KP_LIFE_VERSION) goto out;
    size_t count = ((size_t)data[17] << 8) | data[18];
    if (count > KP_LIFE_MAX || len != KP_LIFE_HEAD_LEN + count * KP_LIFE_ENTRY_LEN) goto out;
    life->next_seq = get_u64(data + 1);
    life->last_created_at = (int64_t)get_u64(data + 9);
    life->count = count;
    const uint8_t *p = data + KP_LIFE_HEAD_LEN;
    for (size_t i = 0; i < count; i++, p += KP_LIFE_ENTRY_LEN) {
        KpLifeEntry *e = &life->entries[i];
        memcpy(e->ref, p, 32);
        e->seq = get_u64(p + 32);
        e->not_after = get_u64(p + 40);
        e->flags = p[48];
    }
    *found = true;
    err = MARMOT_OK;
out:
    free(data);
    return err;
}

/* ── Private material ─────────────────────────────────────────────────── */

static MarmotError
delete_or_absent(Marmot *m, const char *label, const uint8_t ref[32])
{
    MarmotError err = m->storage->mls_delete(m->storage->ctx, label, ref, 32);
    return err == MARMOT_ERR_STORAGE_NOT_FOUND ? MARMOT_OK : err;
}

/* The init, encryption and signature private keys and the stored
 * KeyPackage of @ref. */
static MarmotError
delete_private(Marmot *m, const uint8_t ref[32])
{
    MarmotError err = delete_or_absent(m, "kp_priv", ref);
    if (err == MARMOT_OK) err = delete_or_absent(m, "kp_full", ref);
    return err;
}

static MarmotError
has_private(Marmot *m, const uint8_t ref[32], bool *out)
{
    uint8_t *data = NULL;
    size_t len = 0;
    MarmotError err = m->storage->mls_load(m->storage->ctx, "kp_priv", ref, 32, &data, &len);
    if (data) {
        sodium_memzero(data, len);
        free(data);
    }
    *out = err == MARMOT_OK;
    return err == MARMOT_ERR_STORAGE_NOT_FOUND ? MARMOT_OK : err;
}

static void
life_remove_at(KpLife *life, size_t i)
{
    memmove(&life->entries[i], &life->entries[i + 1],
            (life->count - i - 1) * sizeof(KpLifeEntry));
    life->count--;
}

static KpLifeEntry *
life_find(KpLife *life, const uint8_t ref[32])
{
    for (size_t i = 0; i < life->count; i++)
        if (memcmp(life->entries[i].ref, ref, 32) == 0) return &life->entries[i];
    return NULL;
}

/* ── Seeding (KeyPackages made before the record existed) ─────────────── */

static int
info_older(const void *a, const void *b)
{
    const MarmotKeyPackageInfo *x = *(MarmotKeyPackageInfo *const *)a;
    const MarmotKeyPackageInfo *y = *(MarmotKeyPackageInfo *const *)b;
    /* The active one is the newest (creation deactivates the others);
     * created_at has only second resolution. */
    if (x->active != y->active) return x->active ? 1 : -1;
    if (x->created_at != y->created_at) return x->created_at < y->created_at ? -1 : 1;
    return memcmp(x->ref, y->ref, 32);
}

/* An entry for every stored KeyPackage of @owner that still has its private
 * material: oldest first, unconfirmed; Lifetime and last-resort marker from
 * the stored KeyPackage when it parses. */
static MarmotError
life_seed(Marmot *m, const uint8_t owner[32], KpLife *life)
{
    if (!m->storage->find_key_packages_by_pubkey) return MARMOT_OK;
    MarmotKeyPackageInfo **infos = NULL;
    size_t n = 0;
    MarmotError err = m->storage->find_key_packages_by_pubkey(m->storage->ctx, owner, &infos, &n);
    if (err != MARMOT_OK) return err;
    if (n > 1) qsort(infos, n, sizeof(*infos), info_older);
    for (size_t i = 0; i < n && err == MARMOT_OK; i++) {
        bool present = false;
        err = has_private(m, infos[i]->ref, &present);
        if (err != MARMOT_OK || !present || life_find(life, infos[i]->ref)) continue;
        KpLifeEntry e;
        memset(&e, 0, sizeof(e));
        memcpy(e.ref, infos[i]->ref, 32);
        uint8_t *kp_data = NULL;
        size_t kp_len = 0;
        if (m->storage->mls_load(m->storage->ctx, "kp_full", infos[i]->ref, 32, &kp_data,
                                 &kp_len) == MARMOT_OK && kp_data) {
            MlsKeyPackage kp;
            memset(&kp, 0, sizeof(kp));
            MlsTlsReader reader;
            mls_tls_reader_init(&reader, kp_data, kp_len);
            if (mls_key_package_deserialize(&reader, &kp) == 0) {
                e.not_after = kp.leaf_node.lifetime_not_after;
                if (marmot_kp_is_last_resort(&kp)) e.flags |= KP_LIFE_LAST_RESORT;
            }
            mls_key_package_clear(&kp);
        }
        free(kp_data);
        if (life->count == KP_LIFE_MAX) {
            /* More than the record holds: the oldest go now (none of them
             * was ever confirmed as far as libmarmot knows). */
            err = delete_private(m, life->entries[0].ref);
            if (err != MARMOT_OK) break;
            life_remove_at(life, 0);
        }
        e.seq = life->next_seq++;
        life->entries[life->count++] = e;
        if (infos[i]->created_at > life->last_created_at)
            life->last_created_at = infos[i]->created_at;
    }
    for (size_t i = 0; i < n; i++) marmot_key_package_info_free(infos[i]);
    free(infos);
    return err;
}

static MarmotError
life_open(Marmot *m, const uint8_t owner[32], KpLife *life)
{
    bool found = false;
    MarmotError err = life_load(m, owner, life, &found);
    if (err == MARMOT_OK && !found) err = life_seed(m, owner, life);
    return err;
}

static bool
storage_ready(const Marmot *m)
{
    return m && m->storage && m->storage->mls_store && m->storage->mls_load &&
           m->storage->mls_delete;
}

/* ── Internal hooks ───────────────────────────────────────────────────── */

MarmotError
marmot_kp_lifecycle_created_at(Marmot *m, const uint8_t owner[32], int64_t now,
                               int64_t *out)
{
    if (!storage_ready(m) || !owner || !out) return MARMOT_ERR_STORAGE;
    KpLife life;
    MarmotError err = life_open(m, owner, &life);
    if (err != MARMOT_OK) return err;
    *out = now > life.last_created_at ? now : life.last_created_at + 1;
    return MARMOT_OK;
}

MarmotError
marmot_kp_lifecycle_register(Marmot *m, const uint8_t owner[32], const uint8_t ref[32],
                             uint64_t not_after, bool last_resort, int64_t created_at)
{
    if (!storage_ready(m) || !owner || !ref) return MARMOT_ERR_STORAGE;
    KpLife life;
    MarmotError err = life_open(m, owner, &life);
    if (err != MARMOT_OK) return err;
    if (created_at > life.last_created_at) life.last_created_at = created_at;
    if (life_find(&life, ref)) return life_save(m, owner, &life);
    /* Bounded: a long run of unconfirmed publishes drops the oldest
     * entries, never the newest confirmed one (it is still published). */
    while (life.count >= KP_LIFE_MAX) {
        size_t newest_confirmed = SIZE_MAX;
        for (size_t i = 0; i < life.count; i++)
            if (life.entries[i].flags & KP_LIFE_CONFIRMED) newest_confirmed = i;
        size_t victim = newest_confirmed == 0 ? 1 : 0;
        err = delete_private(m, life.entries[victim].ref);
        if (err != MARMOT_OK) return err;
        life_remove_at(&life, victim);
    }
    KpLifeEntry *e = &life.entries[life.count++];
    memset(e, 0, sizeof(*e));
    memcpy(e->ref, ref, 32);
    e->seq = life.next_seq++;
    e->not_after = not_after;
    e->flags = last_resort ? KP_LIFE_LAST_RESORT : 0;
    return life_save(m, owner, &life);
}

MarmotError
marmot_kp_lifecycle_consumed(Marmot *m, const MarmotKpUse *use)
{
    if (!use || !use->have) return MARMOT_OK;
    if (!storage_ready(m)) return MARMOT_ERR_STORAGE;
    KpLife life;
    bool found = false;
    MarmotError err = life_load(m, use->owner, &life, &found);
    if (err != MARMOT_OK) return err;
    KpLifeEntry *e = found ? life_find(&life, use->ref) : NULL;
    if (use->last_resort) {
        /* It may serve further Welcomes until the bound. */
        if (!e) return MARMOT_OK;
        e->flags |= KP_LIFE_CONSUMED;
        return life_save(m, use->owner, &life);
    }
    /* A consumed non-last-resort init key MUST go after successful Welcome
     * processing. */
    err = delete_private(m, use->ref);
    if (err != MARMOT_OK || !e) return err;
    life_remove_at(&life, (size_t)(e - life.entries));
    return life_save(m, use->owner, &life);
}

/* ── Public API ───────────────────────────────────────────────────────── */

static MarmotError
confirm_impl(Marmot *m, const uint8_t owner[32], const uint8_t ref[32])
{
    KpLife life;
    MarmotError err = life_open(m, owner, &life);
    if (err != MARMOT_OK) return err;
    KpLifeEntry *target = life_find(&life, ref);
    if (!target) return MARMOT_ERR_KEY_NOT_FOUND;
    uint64_t seq = target->seq;
    target->flags |= KP_LIFE_CONFIRMED;
    /* Every older KeyPackage of the slot is superseded on the relays. */
    for (size_t i = 0; i < life.count;) {
        if (life.entries[i].seq < seq) {
            err = delete_private(m, life.entries[i].ref);
            if (err != MARMOT_OK) return err;
            life_remove_at(&life, i);
        } else {
            i++;
        }
    }
    return life_save(m, owner, &life);
}

MarmotError
marmot_key_package_confirm_published(Marmot *m, const uint8_t owner_pubkey[32],
                                     const uint8_t key_package_ref[32])
{
    if (!m || !owner_pubkey || !key_package_ref) return MARMOT_ERR_INVALID_ARG;
    if (!storage_ready(m)) return MARMOT_ERR_STORAGE;
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    return marmot_txn_end(m, confirm_impl(m, owner_pubkey, key_package_ref));
}

static MarmotError
sweep_impl(Marmot *m, const uint8_t owner[32], int64_t now, size_t *deleted)
{
    KpLife life;
    MarmotError err = life_open(m, owner, &life);
    if (err != MARMOT_OK) return err;
    uint64_t t = (uint64_t)(now > 0 ? now : marmot_now());
    size_t n = 0;
    for (size_t i = 0; i < life.count;) {
        uint64_t not_after = life.entries[i].not_after;
        if (not_after != 0 && not_after <= t) {
            err = delete_private(m, life.entries[i].ref);
            if (err != MARMOT_OK) return err;
            life_remove_at(&life, i);
            n++;
        } else {
            i++;
        }
    }
    if (deleted) *deleted = n;
    /* Nothing expired: no write (Groundhog sweeps at every publish check). */
    return n > 0 ? life_save(m, owner, &life) : MARMOT_OK;
}

MarmotError
marmot_key_package_sweep_expired(Marmot *m, const uint8_t owner_pubkey[32], int64_t now,
                                 size_t *out_deleted)
{
    if (out_deleted) *out_deleted = 0;
    if (!m || !owner_pubkey) return MARMOT_ERR_INVALID_ARG;
    if (!storage_ready(m)) return MARMOT_ERR_STORAGE;
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    size_t n = 0;
    err = marmot_txn_end(m, sweep_impl(m, owner_pubkey, now, &n));
    if (err == MARMOT_OK && out_deleted) *out_deleted = n;
    return err;
}

MarmotError
marmot_key_package_next_expiry(Marmot *m, const uint8_t owner_pubkey[32],
                               int64_t *out_not_after)
{
    if (out_not_after) *out_not_after = 0;
    if (!m || !owner_pubkey || !out_not_after) return MARMOT_ERR_INVALID_ARG;
    if (!storage_ready(m)) return MARMOT_ERR_STORAGE;
    KpLife life;
    MarmotError err = life_open(m, owner_pubkey, &life);
    if (err != MARMOT_OK) return err;
    uint64_t earliest = 0;
    for (size_t i = 0; i < life.count; i++) {
        uint64_t t = life.entries[i].not_after;
        if (t != 0 && (earliest == 0 || t < earliest)) earliest = t;
    }
    *out_not_after = (int64_t)earliest;
    return MARMOT_OK;
}

MarmotError
marmot_key_package_has_private_key(Marmot *m, const uint8_t key_package_ref[32],
                                   bool *out_present)
{
    if (out_present) *out_present = false;
    if (!m || !key_package_ref || !out_present) return MARMOT_ERR_INVALID_ARG;
    if (!m->storage || !m->storage->mls_load) return MARMOT_ERR_STORAGE;
    return has_private(m, key_package_ref, out_present);
}
