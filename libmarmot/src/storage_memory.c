/*
 * libmarmot - In-memory storage backend
 *
 * Simple storage implementation for testing and ephemeral use.
 * Stores all data in malloc'd arrays. Not thread-safe.
 *
 * SPDX-License-Identifier: MIT
 */

#include <marmot/marmot-storage.h>
#include <stdlib.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Internal context
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    /* Groups */
    MarmotGroup **groups;
    size_t group_count;
    size_t group_cap;

    /* Messages */
    MarmotMessage **messages;
    size_t msg_count;
    size_t msg_cap;

    /* Welcomes */
    MarmotWelcome **welcomes;
    size_t welcome_count;
    size_t welcome_cap;

    /* MLS key store: simple label+key → value map */
    struct mls_entry {
        char    *label;
        uint8_t *key;
        size_t   key_len;
        uint8_t *value;
        size_t   value_len;
    } *mls_entries;
    size_t mls_count;
    size_t mls_cap;

    /* Exporter secrets */
    MarmotExporterSecret *secrets;
    size_t secret_count;
    size_t secret_cap;
} MemCtx;

/* ── Deep copy helpers ─────────────────────────────────────────────────── */

static char *
xstrdup(const char *s)
{
    if (!s) return NULL;
    return strdup(s);
}

static uint8_t *
xmemdup(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return NULL;
    uint8_t *out = malloc(len);
    if (out) memcpy(out, data, len);
    return out;
}

static MarmotGroup *
group_deep_copy(const MarmotGroup *src)
{
    MarmotGroup *g = marmot_group_new();
    if (!g) return NULL;

    g->mls_group_id = marmot_group_id_new(src->mls_group_id.data, src->mls_group_id.len);
    memcpy(g->nostr_group_id, src->nostr_group_id, 32);
    g->name = xstrdup(src->name);
    g->description = xstrdup(src->description);
    g->image_hash = xmemdup(src->image_hash, 32);
    g->image_key = xmemdup(src->image_key, 32);
    g->image_nonce = xmemdup(src->image_nonce, 12);
    if (src->admin_count > 0 && src->admin_pubkeys) {
        g->admin_pubkeys = malloc(src->admin_count * 32);
        if (g->admin_pubkeys)
            memcpy(g->admin_pubkeys, src->admin_pubkeys, src->admin_count * 32);
    }
    g->admin_count = src->admin_count;
    g->last_message_id = xstrdup(src->last_message_id);
    g->last_message_at = src->last_message_at;
    g->last_message_processed_at = src->last_message_processed_at;
    g->epoch = src->epoch;
    g->state = src->state;
    return g;
}

static MarmotMessage *
msg_deep_copy(const MarmotMessage *src)
{
    MarmotMessage *m = marmot_message_new();
    if (!m) return NULL;

    memcpy(m->id, src->id, 32);
    memcpy(m->pubkey, src->pubkey, 32);
    m->kind = src->kind;
    m->mls_group_id = marmot_group_id_new(src->mls_group_id.data, src->mls_group_id.len);
    m->created_at = src->created_at;
    m->processed_at = src->processed_at;
    m->content = xstrdup(src->content);
    m->tags_json = xstrdup(src->tags_json);
    m->event_json = xstrdup(src->event_json);
    memcpy(m->wrapper_event_id, src->wrapper_event_id, 32);
    m->epoch = src->epoch;
    m->state = src->state;
    return m;
}

/* ── Group operations ──────────────────────────────────────────────────── */

static MarmotError
mem_all_groups(void *ctx, MarmotGroup ***out, size_t *out_count)
{
    MemCtx *mc = ctx;
    *out_count = mc->group_count;
    if (mc->group_count == 0) {
        *out = NULL;
        return MARMOT_OK;
    }
    *out = calloc(mc->group_count, sizeof(MarmotGroup *));
    if (!*out) return MARMOT_ERR_MEMORY;
    for (size_t i = 0; i < mc->group_count; i++) {
        (*out)[i] = group_deep_copy(mc->groups[i]);
        if (!(*out)[i]) {
            for (size_t j = 0; j < i; j++) marmot_group_free((*out)[j]);
            free(*out);
            *out = NULL;
            *out_count = 0;
            return MARMOT_ERR_MEMORY;
        }
    }
    return MARMOT_OK;
}

static MarmotError
mem_find_group_by_mls_id(void *ctx, const MarmotGroupId *gid, MarmotGroup **out)
{
    MemCtx *mc = ctx;
    *out = NULL;
    for (size_t i = 0; i < mc->group_count; i++) {
        if (marmot_group_id_equal(&mc->groups[i]->mls_group_id, gid)) {
            *out = group_deep_copy(mc->groups[i]);
            return *out ? MARMOT_OK : MARMOT_ERR_MEMORY;
        }
    }
    return MARMOT_OK; /* not found is not an error */
}

static MarmotError
mem_find_group_by_nostr_id(void *ctx, const uint8_t nostr_group_id[32],
                            MarmotGroup **out)
{
    MemCtx *mc = ctx;
    *out = NULL;
    for (size_t i = 0; i < mc->group_count; i++) {
        if (memcmp(mc->groups[i]->nostr_group_id, nostr_group_id, 32) == 0) {
            *out = group_deep_copy(mc->groups[i]);
            return *out ? MARMOT_OK : MARMOT_ERR_MEMORY;
        }
    }
    return MARMOT_OK;
}

static MarmotError
mem_save_group(void *ctx, const MarmotGroup *group)
{
    MemCtx *mc = ctx;

    /* Upsert: check if group already exists */
    for (size_t i = 0; i < mc->group_count; i++) {
        if (marmot_group_id_equal(&mc->groups[i]->mls_group_id, &group->mls_group_id)) {
            marmot_group_free(mc->groups[i]);
            mc->groups[i] = group_deep_copy(group);
            return mc->groups[i] ? MARMOT_OK : MARMOT_ERR_MEMORY;
        }
    }

    /* Insert new */
    if (mc->group_count >= mc->group_cap) {
        size_t new_cap = mc->group_cap ? mc->group_cap * 2 : 16;
        MarmotGroup **new_arr = realloc(mc->groups, new_cap * sizeof(MarmotGroup *));
        if (!new_arr) return MARMOT_ERR_MEMORY;
        mc->groups = new_arr;
        mc->group_cap = new_cap;
    }

    mc->groups[mc->group_count] = group_deep_copy(group);
    if (!mc->groups[mc->group_count]) return MARMOT_ERR_MEMORY;
    mc->group_count++;
    return MARMOT_OK;
}

static MarmotError
mem_messages(void *ctx, const MarmotGroupId *gid,
             const MarmotPagination *pg,
             MarmotMessage ***out, size_t *out_count)
{
    MemCtx *mc = ctx;
    *out = NULL;
    *out_count = 0;

    /* Collect matching messages */
    size_t match_count = 0;
    for (size_t i = 0; i < mc->msg_count; i++) {
        if (marmot_group_id_equal(&mc->messages[i]->mls_group_id, gid))
            match_count++;
    }

    if (match_count == 0) return MARMOT_OK;

    /* Apply offset + limit */
    size_t skip = pg ? pg->offset : 0;
    size_t limit = pg ? pg->limit : 1000;
    if (skip >= match_count) return MARMOT_OK;

    size_t result_count = match_count - skip;
    if (result_count > limit) result_count = limit;

    *out = calloc(result_count, sizeof(MarmotMessage *));
    if (!*out) return MARMOT_ERR_MEMORY;

    size_t found = 0, added = 0;
    for (size_t i = 0; i < mc->msg_count && added < result_count; i++) {
        if (marmot_group_id_equal(&mc->messages[i]->mls_group_id, gid)) {
            if (found >= skip) {
                (*out)[added] = msg_deep_copy(mc->messages[i]);
                if (!(*out)[added]) goto oom;
                added++;
            }
            found++;
        }
    }
    *out_count = added;
    return MARMOT_OK;

oom:
    for (size_t j = 0; j < added; j++) marmot_message_free((*out)[j]);
    free(*out);
    *out = NULL;
    *out_count = 0;
    return MARMOT_ERR_MEMORY;
}

static MarmotError
mem_last_message(void *ctx, const MarmotGroupId *gid,
                  MarmotSortOrder order, MarmotMessage **out)
{
    MemCtx *mc = ctx;
    *out = NULL;
    MarmotMessage *best = NULL;

    for (size_t i = 0; i < mc->msg_count; i++) {
        MarmotMessage *m = mc->messages[i];
        if (!marmot_group_id_equal(&m->mls_group_id, gid)) continue;
        if (!best) { best = m; continue; }

        int64_t b_ts = (order == MARMOT_SORT_PROCESSED_AT_FIRST) ? best->processed_at : best->created_at;
        int64_t m_ts = (order == MARMOT_SORT_PROCESSED_AT_FIRST) ? m->processed_at : m->created_at;
        if (m_ts > b_ts) best = m;
    }

    if (best) {
        *out = msg_deep_copy(best);
        if (!*out) return MARMOT_ERR_MEMORY;
    }
    return MARMOT_OK;
}

/* ── Message operations ────────────────────────────────────────────────── */

static MarmotError
mem_save_message(void *ctx, const MarmotMessage *msg)
{
    MemCtx *mc = ctx;
    if (mc->msg_count >= mc->msg_cap) {
        size_t new_cap = mc->msg_cap ? mc->msg_cap * 2 : 64;
        MarmotMessage **arr = realloc(mc->messages, new_cap * sizeof(MarmotMessage *));
        if (!arr) return MARMOT_ERR_MEMORY;
        mc->messages = arr;
        mc->msg_cap = new_cap;
    }
    mc->messages[mc->msg_count] = msg_deep_copy(msg);
    if (!mc->messages[mc->msg_count]) return MARMOT_ERR_MEMORY;
    mc->msg_count++;
    return MARMOT_OK;
}

static MarmotError
mem_find_message_by_id(void *ctx, const uint8_t event_id[32], MarmotMessage **out)
{
    MemCtx *mc = ctx;
    *out = NULL;
    for (size_t i = 0; i < mc->msg_count; i++) {
        if (memcmp(mc->messages[i]->id, event_id, 32) == 0) {
            *out = msg_deep_copy(mc->messages[i]);
            return *out ? MARMOT_OK : MARMOT_ERR_MEMORY;
        }
    }
    return MARMOT_OK;
}

static MarmotError
mem_is_message_processed(void *ctx, const uint8_t wrapper_id[32], bool *out)
{
    MemCtx *mc = ctx;
    *out = false;
    for (size_t i = 0; i < mc->msg_count; i++) {
        if (memcmp(mc->messages[i]->wrapper_event_id, wrapper_id, 32) == 0) {
            *out = true;
            return MARMOT_OK;
        }
    }
    return MARMOT_OK;
}

static MarmotError
mem_save_processed_message(void *ctx, const uint8_t wrapper_id[32],
                            const uint8_t *msg_id, int64_t processed_at,
                            uint64_t epoch, const MarmotGroupId *gid,
                            int state, const char *reason)
{
    /* In-memory: just track via the message itself. For the memory backend,
     * we store a minimal message record. */
    (void)ctx; (void)wrapper_id; (void)msg_id; (void)processed_at;
    (void)epoch; (void)gid; (void)state; (void)reason;
    return MARMOT_OK;
}

/* ── Welcome operations ────────────────────────────────────────────────── */

static MarmotWelcome *
welcome_deep_copy(const MarmotWelcome *src)
{
    MarmotWelcome *w = marmot_welcome_new();
    if (!w) return NULL;
    memcpy(w->id, src->id, 32);
    w->event_json = xstrdup(src->event_json);
    w->mls_group_id = marmot_group_id_new(src->mls_group_id.data, src->mls_group_id.len);
    memcpy(w->nostr_group_id, src->nostr_group_id, 32);
    w->group_name = xstrdup(src->group_name);
    w->group_description = xstrdup(src->group_description);
    w->group_image_hash = xmemdup(src->group_image_hash, 32);
    if (src->group_admin_count > 0 && src->group_admin_pubkeys) {
        w->group_admin_pubkeys = malloc(src->group_admin_count * 32);
        if (w->group_admin_pubkeys)
            memcpy(w->group_admin_pubkeys, src->group_admin_pubkeys, src->group_admin_count * 32);
    }
    w->group_admin_count = src->group_admin_count;
    if (src->group_relay_count > 0 && src->group_relays) {
        w->group_relays = calloc(src->group_relay_count, sizeof(char *));
        if (w->group_relays) {
            for (size_t i = 0; i < src->group_relay_count; i++)
                w->group_relays[i] = xstrdup(src->group_relays[i]);
        }
    }
    w->group_relay_count = src->group_relay_count;
    memcpy(w->welcomer, src->welcomer, 32);
    w->member_count = src->member_count;
    w->state = src->state;
    memcpy(w->wrapper_event_id, src->wrapper_event_id, 32);
    return w;
}

static MarmotError
mem_save_welcome(void *ctx, const MarmotWelcome *welcome)
{
    MemCtx *mc = ctx;
    if (mc->welcome_count >= mc->welcome_cap) {
        size_t new_cap = mc->welcome_cap ? mc->welcome_cap * 2 : 16;
        MarmotWelcome **arr = realloc(mc->welcomes, new_cap * sizeof(MarmotWelcome *));
        if (!arr) return MARMOT_ERR_MEMORY;
        mc->welcomes = arr;
        mc->welcome_cap = new_cap;
    }
    mc->welcomes[mc->welcome_count] = welcome_deep_copy(welcome);
    if (!mc->welcomes[mc->welcome_count]) return MARMOT_ERR_MEMORY;
    mc->welcome_count++;
    return MARMOT_OK;
}

static MarmotError
mem_find_welcome_by_event_id(void *ctx, const uint8_t event_id[32],
                              MarmotWelcome **out)
{
    MemCtx *mc = ctx;
    *out = NULL;
    for (size_t i = 0; i < mc->welcome_count; i++) {
        if (memcmp(mc->welcomes[i]->id, event_id, 32) == 0) {
            *out = welcome_deep_copy(mc->welcomes[i]);
            return *out ? MARMOT_OK : MARMOT_ERR_MEMORY;
        }
    }
    return MARMOT_OK;
}

static MarmotError
mem_pending_welcomes(void *ctx, const MarmotPagination *pg,
                      MarmotWelcome ***out, size_t *out_count)
{
    MemCtx *mc = ctx;
    *out = NULL;
    *out_count = 0;

    size_t pending = 0;
    for (size_t i = 0; i < mc->welcome_count; i++) {
        if (mc->welcomes[i]->state == MARMOT_WELCOME_STATE_PENDING)
            pending++;
    }
    if (pending == 0) return MARMOT_OK;

    size_t skip = pg ? pg->offset : 0;
    size_t limit = pg ? pg->limit : 1000;
    if (skip >= pending) return MARMOT_OK;

    size_t result_count = pending - skip;
    if (result_count > limit) result_count = limit;

    *out = calloc(result_count, sizeof(MarmotWelcome *));
    if (!*out) return MARMOT_ERR_MEMORY;

    size_t found = 0, added = 0;
    for (size_t i = 0; i < mc->welcome_count && added < result_count; i++) {
        if (mc->welcomes[i]->state != MARMOT_WELCOME_STATE_PENDING) continue;
        if (found >= skip) {
            (*out)[added] = welcome_deep_copy(mc->welcomes[i]);
            if (!(*out)[added]) goto oom;
            added++;
        }
        found++;
    }
    *out_count = added;
    return MARMOT_OK;

oom:
    for (size_t j = 0; j < added; j++) marmot_welcome_free((*out)[j]);
    free(*out);
    *out = NULL;
    *out_count = 0;
    return MARMOT_ERR_MEMORY;
}

static MarmotError
mem_find_processed_welcome(void *ctx, const uint8_t wrapper_id[32],
                            bool *out_found, int *out_state, char **out_reason)
{
    (void)ctx; (void)wrapper_id;
    *out_found = false;
    *out_state = 0;
    *out_reason = NULL;
    return MARMOT_OK;
}

static MarmotError
mem_save_processed_welcome(void *ctx, const uint8_t wrapper_id[32],
                            const uint8_t *welcome_event_id, int64_t processed_at,
                            int state, const char *reason)
{
    (void)ctx; (void)wrapper_id; (void)welcome_event_id;
    (void)processed_at; (void)state; (void)reason;
    return MARMOT_OK;
}

/* ── Relay operations (simplified) ─────────────────────────────────────── */

static MarmotError
mem_group_relays(void *ctx, const MarmotGroupId *gid,
                  MarmotGroupRelay **out, size_t *out_count)
{
    (void)ctx; (void)gid;
    *out = NULL;
    *out_count = 0;
    return MARMOT_OK;
}

static MarmotError
mem_replace_group_relays(void *ctx, const MarmotGroupId *gid,
                          const char **urls, size_t count)
{
    (void)ctx; (void)gid; (void)urls; (void)count;
    return MARMOT_OK;
}

/* ── Exporter secret operations ────────────────────────────────────────── */

static MarmotError
mem_get_exporter_secret(void *ctx, const MarmotGroupId *gid,
                         uint64_t epoch, uint8_t out[32])
{
    MemCtx *mc = ctx;
    for (size_t i = 0; i < mc->secret_count; i++) {
        if (mc->secrets[i].epoch == epoch &&
            marmot_group_id_equal(&mc->secrets[i].mls_group_id, gid)) {
            memcpy(out, mc->secrets[i].secret, 32);
            return MARMOT_OK;
        }
    }
    return MARMOT_ERR_STORAGE_NOT_FOUND;
}

static MarmotError
mem_save_exporter_secret(void *ctx, const MarmotGroupId *gid,
                          uint64_t epoch, const uint8_t secret[32])
{
    MemCtx *mc = ctx;
    if (mc->secret_count >= mc->secret_cap) {
        size_t new_cap = mc->secret_cap ? mc->secret_cap * 2 : 16;
        MarmotExporterSecret *arr = realloc(mc->secrets, new_cap * sizeof(MarmotExporterSecret));
        if (!arr) return MARMOT_ERR_MEMORY;
        mc->secrets = arr;
        mc->secret_cap = new_cap;
    }
    MarmotExporterSecret *s = &mc->secrets[mc->secret_count];
    s->mls_group_id = marmot_group_id_new(gid->data, gid->len);
    s->epoch = epoch;
    memcpy(s->secret, secret, 32);
    mc->secret_count++;
    return MARMOT_OK;
}

/* ── Snapshot operations (no-op for memory) ────────────────────────────── */

static MarmotError mem_create_snapshot(void *ctx, const MarmotGroupId *gid, const char *name)
{ (void)ctx; (void)gid; (void)name; return MARMOT_OK; }
static MarmotError mem_rollback_snapshot(void *ctx, const MarmotGroupId *gid, const char *name)
{ (void)ctx; (void)gid; (void)name; return MARMOT_OK; }
static MarmotError mem_release_snapshot(void *ctx, const MarmotGroupId *gid, const char *name)
{ (void)ctx; (void)gid; (void)name; return MARMOT_OK; }
static MarmotError mem_prune_expired(void *ctx, uint64_t ts, size_t *out)
{ (void)ctx; (void)ts; *out = 0; return MARMOT_OK; }

/* ── MLS key store ─────────────────────────────────────────────────────── */

static int
mls_entry_match(const struct mls_entry *e,
                const char *label, const uint8_t *key, size_t key_len)
{
    if (strcmp(e->label, label) != 0) return 0;
    if (e->key_len != key_len) return 0;
    if (key_len > 0 && memcmp(e->key, key, key_len) != 0) return 0;
    return 1;
}

static MarmotError
mem_mls_store(void *ctx, const char *label,
               const uint8_t *key, size_t key_len,
               const uint8_t *value, size_t value_len)
{
    MemCtx *mc = ctx;

    /* Check for existing entry (upsert) */
    for (size_t i = 0; i < mc->mls_count; i++) {
        if (mls_entry_match(&mc->mls_entries[i], label, key, key_len)) {
            free(mc->mls_entries[i].value);
            mc->mls_entries[i].value = xmemdup(value, value_len);
            mc->mls_entries[i].value_len = value_len;
            return MARMOT_OK;
        }
    }

    if (mc->mls_count >= mc->mls_cap) {
        size_t new_cap = mc->mls_cap ? mc->mls_cap * 2 : 64;
        struct mls_entry *arr = realloc(mc->mls_entries, new_cap * sizeof(struct mls_entry));
        if (!arr) return MARMOT_ERR_MEMORY;
        mc->mls_entries = arr;
        mc->mls_cap = new_cap;
    }

    struct mls_entry *e = &mc->mls_entries[mc->mls_count];
    e->label = xstrdup(label);
    e->key = xmemdup(key, key_len);
    e->key_len = key_len;
    e->value = xmemdup(value, value_len);
    e->value_len = value_len;
    mc->mls_count++;
    return MARMOT_OK;
}

static MarmotError
mem_mls_load(void *ctx, const char *label,
              const uint8_t *key, size_t key_len,
              uint8_t **out, size_t *out_len)
{
    MemCtx *mc = ctx;
    *out = NULL;
    *out_len = 0;
    for (size_t i = 0; i < mc->mls_count; i++) {
        if (mls_entry_match(&mc->mls_entries[i], label, key, key_len)) {
            *out = xmemdup(mc->mls_entries[i].value, mc->mls_entries[i].value_len);
            *out_len = mc->mls_entries[i].value_len;
            return *out ? MARMOT_OK : MARMOT_ERR_MEMORY;
        }
    }
    return MARMOT_ERR_STORAGE_NOT_FOUND;
}

static MarmotError
mem_mls_delete(void *ctx, const char *label,
                const uint8_t *key, size_t key_len)
{
    MemCtx *mc = ctx;
    for (size_t i = 0; i < mc->mls_count; i++) {
        if (mls_entry_match(&mc->mls_entries[i], label, key, key_len)) {
            free(mc->mls_entries[i].label);
            free(mc->mls_entries[i].key);
            free(mc->mls_entries[i].value);
            /* Swap with last */
            if (i < mc->mls_count - 1)
                mc->mls_entries[i] = mc->mls_entries[mc->mls_count - 1];
            mc->mls_count--;
            return MARMOT_OK;
        }
    }
    return MARMOT_ERR_STORAGE_NOT_FOUND;
}

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

static bool
mem_is_persistent(void *ctx)
{
    (void)ctx;
    return false;
}

static void
mem_destroy(void *ctx)
{
    MemCtx *mc = ctx;
    for (size_t i = 0; i < mc->group_count; i++) marmot_group_free(mc->groups[i]);
    free(mc->groups);
    for (size_t i = 0; i < mc->msg_count; i++) marmot_message_free(mc->messages[i]);
    free(mc->messages);
    for (size_t i = 0; i < mc->welcome_count; i++) marmot_welcome_free(mc->welcomes[i]);
    free(mc->welcomes);
    for (size_t i = 0; i < mc->mls_count; i++) {
        free(mc->mls_entries[i].label);
        free(mc->mls_entries[i].key);
        free(mc->mls_entries[i].value);
    }
    free(mc->mls_entries);
    for (size_t i = 0; i < mc->secret_count; i++)
        marmot_group_id_free(&mc->secrets[i].mls_group_id);
    free(mc->secrets);
    free(mc);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public constructor
 * ══════════════════════════════════════════════════════════════════════════ */

MarmotStorage *
marmot_storage_memory_new(void)
{
    MemCtx *mc = calloc(1, sizeof(MemCtx));
    if (!mc) return NULL;

    MarmotStorage *s = calloc(1, sizeof(MarmotStorage));
    if (!s) { free(mc); return NULL; }

    s->ctx = mc;

    /* Group ops */
    s->all_groups = mem_all_groups;
    s->find_group_by_mls_id = mem_find_group_by_mls_id;
    s->find_group_by_nostr_id = mem_find_group_by_nostr_id;
    s->save_group = mem_save_group;
    s->messages = mem_messages;
    s->last_message = mem_last_message;

    /* Message ops */
    s->save_message = mem_save_message;
    s->find_message_by_id = mem_find_message_by_id;
    s->is_message_processed = mem_is_message_processed;
    s->save_processed_message = mem_save_processed_message;

    /* Welcome ops */
    s->save_welcome = mem_save_welcome;
    s->find_welcome_by_event_id = mem_find_welcome_by_event_id;
    s->pending_welcomes = mem_pending_welcomes;
    s->find_processed_welcome = mem_find_processed_welcome;
    s->save_processed_welcome = mem_save_processed_welcome;

    /* Relay ops */
    s->group_relays = mem_group_relays;
    s->replace_group_relays = mem_replace_group_relays;

    /* Exporter secret ops */
    s->get_exporter_secret = mem_get_exporter_secret;
    s->save_exporter_secret = mem_save_exporter_secret;

    /* Snapshot ops */
    s->create_snapshot = mem_create_snapshot;
    s->rollback_snapshot = mem_rollback_snapshot;
    s->release_snapshot = mem_release_snapshot;
    s->prune_expired_snapshots = mem_prune_expired;

    /* MLS key store */
    s->mls_store = mem_mls_store;
    s->mls_load = mem_mls_load;
    s->mls_delete = mem_mls_delete;

    /* Lifecycle */
    s->is_persistent = mem_is_persistent;
    s->destroy = mem_destroy;

    return s;
}
