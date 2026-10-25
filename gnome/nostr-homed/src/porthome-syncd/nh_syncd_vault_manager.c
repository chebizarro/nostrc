/* SPDX-License-Identifier: MIT */
#include "nh_syncd_vault_manager.h"
#include "nh_syncd_vault_net.h"
#include "nh_syncd_cache.h"
#include <openssl/sha.h>
#include <nostr-keys.h>
#include <openssl/crypto.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void nh_syncd_state_bump_generation_(nh_syncd_state *);
typedef struct {
    nh_vault_session *disk;
    nh_vault_engine *engine;
    nh_vault_net *net;
    uint8_t policy_hash[32];
    int listening, needs_install;
    json_t *active, *planned;
} source;
struct nh_vault_manager {
    nh_vault_manager_options options;
    source *sources;
    size_t count, notified;
    int home, state_dir, epoll, timer;
    nh_syncd_pin_ring *pins;
    json_t *owned_pins;
};
static int prefetch(nh_vault_manager *, source *);
static int refresh_pins(nh_vault_manager *);
static int budget_write(nh_vault_manager *, uint64_t, uint64_t);
static int budget_load(nh_vault_manager *, uint64_t *, uint64_t *);
static int publish(void *ctx, NostrEvent *event) { return nh_vault_net_publish(ctx, event); }
static void events_free(NostrEvent **events, size_t count) {
    for (size_t i = 0; i < count; ++i)
        nostr_event_free(events[i]);
    free(events);
}
static int writers(source *s, const uint8_t (**out)[32], size_t *count, uint8_t hash[32]) {
    *out = NULL;
    *count = 0;
    const porthome_vault_info *policy = nh_vault_engine_policy(s->engine);
    const vault_grant *grant = nh_vault_engine_grant(s->engine);
    if (policy) {
        *out = (const uint8_t(*)[32])policy->writers;
        *count = policy->writers_len;
    } else if (grant) {
        *out = (const uint8_t(*)[32]) & grant->owner;
        *count = 1;
    } else {
        *out = (const uint8_t(*)[32])nh_vault_engine_writer(s->engine);
        *count = 1;
    }
    if (*count > VAULT_MAX_HEADS)
        return PORTHOME_ERR_TOO_LARGE;
    uint8_t binding[40] = {0};
    SHA256((const uint8_t *)*out, *count * 32, binding);
    uint64_t epoch = grant ? grant->ring.entries[grant->ring.count - 1].epoch : 0;
    for (size_t i = 0; i < 8; ++i)
        binding[32 + i] = (uint8_t)(epoch >> (8 * i));
    SHA256(binding, sizeof binding, hash);
    return 0;
}
static int refresh_pins(nh_vault_manager *m) {
    json_t *wanted = json_object(), *drop = json_array();
    if (!wanted || !drop) {
        json_decref(wanted);
        json_decref(drop);
        return VAULT_ERR_OOM;
    }
    char **old = NULL;
    size_t count = 0;
    int rc = nh_syncd_pin_ring_effective_pins(m->pins, &old, &count);
    for (size_t i = 0; i < count; ++i) {
        if (!rc && json_object_set_new(wanted, old[i], json_true()))
            rc = VAULT_ERR_OOM;
        free(old[i]);
    }
    free(old);
    for (size_t i = 0; !rc && i < m->count; ++i) {
        const char *key;
        json_t *value;
        json_object_foreach(m->sources[i].active, key,
                            value) if (json_object_set_new(wanted, key, json_true())) rc =
            VAULT_ERR_OOM;
        json_object_foreach(m->sources[i].planned, key,
                            value) if (json_object_set_new(wanted, key, json_true())) rc =
            VAULT_ERR_OOM;
    }
    const char *key;
    json_t *value;
    json_object_foreach(wanted, key, value) {
        if (rc)
            break;
        if (!json_object_get(m->owned_pins, key)) {
            if (nh_syncd_cache_pin(m->options.cache, key))
                rc = VAULT_ERR_IO;
            else if (json_object_set_new(m->owned_pins, key, json_true()))
                rc = VAULT_ERR_OOM;
        }
    }
    json_object_foreach(m->owned_pins, key,
                        value) if (!json_object_get(wanted, key) &&
                                   json_array_append_new(drop, json_string(key))) rc =
        VAULT_ERR_OOM;
    for (size_t i = 0; !rc && i < json_array_size(drop); ++i) {
        key = json_string_value(json_array_get(drop, i));
        if (nh_syncd_cache_unpin(m->options.cache, key))
            rc = VAULT_ERR_IO;
        else
            json_object_del(m->owned_pins, key);
    }
    json_decref(wanted);
    json_decref(drop);
    return rc;
}
typedef struct {
    nh_vault_manager *manager;
    source *source;
    json_t *refs;
    int execute;
    uint64_t start, used;
} prefetch_ctx;
static int ensure_blob(void *ctx, const uint8_t hash[32], uint64_t size, const uint8_t *metadata) {
    prefetch_ctx *p = ctx;
    nh_vault_manager *m = p->manager;
    source *s = p->source;
    if (!size || size > 64u * 1024u * 1024u || size > INT64_MAX)
        return PORTHOME_ERR_TOO_LARGE;
    char hex[65];
    porthome_hex64(hash, hex);
    if (!p->execute) {
        json_t *old = json_object_get(p->refs, hex);
        if (old && (uint64_t)json_integer_value(old) != size)
            return PORTHOME_ERR_HASH_MISMATCH;
        return json_object_set_new(p->refs, hex, json_integer((json_int_t)size)) ? VAULT_ERR_OOM
                                                                                 : 0;
    }
    if (nh_syncd_cache_has(m->options.cache, hex)) {
        char *path = nh_syncd_cache_get_path(m->options.cache, hex);
        uint8_t *bytes = NULL, actual[32];
        size_t n = 0;
        int rc = path ? nh_vault_file_read(AT_FDCWD, path, &bytes, &n) : VAULT_ERR_IO;
        free(path);
        if (!rc) {
            SHA256(bytes, n, actual);
            if (n != size || memcmp(actual, hash, 32))
                rc = PORTHOME_ERR_HASH_MISMATCH;
        }
        free(bytes);
        return rc;
    }
    if (metadata)
        return nh_syncd_cache_put(m->options.cache, hex, metadata, (size_t)size) ? VAULT_ERR_BUDGET
                                                                                 : 0;
    uint8_t *bytes = NULL;
    size_t len = 0;
    int rc = nh_vault_file_read(s->disk->objects, hex, &bytes, &len);
    if (rc == VAULT_ERR_INCOMPLETE) {
        if (p->used > m->options.retention_budget || size > m->options.retention_budget - p->used)
            return VAULT_ERR_BUDGET;
        rc = budget_write(m, p->start, p->used + size);
        if (!rc) {
            p->used += size;
            nh_vault_net_read_limit(s->net, (size_t)size);
            rc = s->disk->io.read(s->disk->io.ctx, hash, &bytes, &len);
            nh_vault_net_read_limit(s->net, 0);
        }
    }
    if (!rc && len != size)
        rc = PORTHOME_ERR_HASH_MISMATCH;
    if (!rc)
        rc = nh_syncd_cache_put(m->options.cache, hex, bytes, len) ? VAULT_ERR_BUDGET : 0;
    free(bytes);
    return rc;
}
static int prefetch(nh_vault_manager *m, source *s) {
    prefetch_ctx ctx = {.manager = m, .source = s, .refs = json_object()};
    if (!ctx.refs)
        return VAULT_ERR_OOM;
    int rc = nh_vault_engine_prefetch(s->engine, ensure_blob, &ctx, 0);
    if (!rc) {
        json_decref(s->planned);
        s->planned = json_deep_copy(ctx.refs);
        if (!s->planned)
            rc = VAULT_ERR_OOM;
    }
    if (!rc)
        rc = refresh_pins(m);
    /* Quota accounting includes current/retained generations and the entire
     * prospective view. A private view is never published partially cached. */
    uint64_t required = 0;
    const char *hex;
    json_t *v;
    json_object_foreach(m->owned_pins, hex, v) {
        if (rc)
            break;
        char *path = nh_syncd_cache_get_path(m->options.cache, hex);
        struct stat st;
        uint64_t size = 0;
        if (path && !lstat(path, &st) && S_ISREG(st.st_mode) && st.st_size >= 0)
            size = (uint64_t)st.st_size;
        free(path);
        json_t *planned = json_object_get(ctx.refs, hex);
        if (planned)
            size = (uint64_t)json_integer_value(planned);
        if (size > UINT64_MAX - required)
            rc = PORTHOME_ERR_TOO_LARGE;
        else
            required += size;
    }
    if (!rc && required > nh_syncd_cache_quota_bytes(m->options.cache))
        rc = VAULT_ERR_BUDGET;
    if (!rc)
        rc = budget_load(m, &ctx.start, &ctx.used);
    ctx.execute = 1;
    if (!rc)
        rc = nh_vault_engine_prefetch(s->engine, ensure_blob, &ctx, 1);
    if (!rc &&
        nh_syncd_cache_used_bytes(m->options.cache) > nh_syncd_cache_quota_bytes(m->options.cache))
        rc = VAULT_ERR_BUDGET;
    if (!rc && json_object_set_new(s->disk->doc, "cache_only",
                                   json_boolean(nh_vault_net_cache_only(s->net))))
        rc = VAULT_ERR_OOM;
    if (!rc)
        rc = nh_vault_session_save(s->disk);
    json_decref(ctx.refs);
    return rc;
}
static int status(nh_vault_manager *m, int result) {
    json_t *all = json_array();
    if (!all)
        return VAULT_ERR_OOM;
    for (size_t i = 0; i < m->count; ++i) {
        const json_t *list = nh_vault_engine_conflicts(m->sources[i].engine);
        size_t k;
        json_t *row;
        json_array_foreach(list, k, row) {
            json_t *copy = json_deep_copy(row);
            char id[65];
            porthome_hex64(m->options.config->vaults[i].vault_id, id);
            if (!copy || json_object_set_new(copy, "vault_id", json_string(id)) ||
                json_array_append_new(all, copy)) {
                json_decref(all);
                return VAULT_ERR_OOM;
            }
        }
    }
    size_t count = json_array_size(all), deleted = 0;
    for (size_t i = 0; i < count; ++i)
        if (json_is_false(json_object_get(json_array_get(all, i), "present")))
            ++deleted;
    char *wire = json_dumps(all, JSON_COMPACT);
    json_decref(all);
    int rc = wire ? 0 : VAULT_ERR_OOM;
    if (!rc && m->options.status) {
        if (nh_syncd_status_set_vault_conflicts(m->options.status, wire))
            rc = VAULT_ERR_IO;
        nh_syncd_status_set_state(m->options.status,
                                  result ? NH_SYNCD_STATE_ERROR : NH_SYNCD_STATE_IDLE);
        nh_syncd_status_set_last_error(m->options.status,
                                       result == VAULT_ERR_AUTH         ? "vault-auth"
                                       : result == VAULT_ERR_EPOCH      ? "vault-epoch"
                                       : result == VAULT_ERR_INCOMPLETE ? "vault-incomplete"
                                       : result == VAULT_ERR_BUDGET     ? "vault-budget"
                                       : result                         ? "vault-sync"
                                                                        : "");
        if (nh_syncd_status_emit(m->options.status, NULL))
            rc = VAULT_ERR_IO;
    }
    free(wire);
    if (!rc && count > m->notified && m->options.notifier) {
        char body[192];
        snprintf(body, sizeof body,
                 "%zu shared-vault conflicts (%zu deletions). Present versions are in "
                 "Portable/.conflicts; deletion details are in porthome status.",
                 count, deleted);
        (void)nh_porthome_notify(m->options.notifier, "nostr-home-sync", "folder-remote",
                                 "Shared vault conflicts", body, NH_NOTIFY_CAT_CONFLICT,
                                 "vault-conflicts");
    }
    if (!rc)
        m->notified = count;
    return result ? result : rc;
}
static int promote_refs(nh_vault_manager *m, source *changed, uint64_t generation) {
    json_t *all = json_object();
    if (!all)
        return VAULT_ERR_OOM;
    int rc = 0;
    for (size_t i = 0; i < m->count; ++i) {
        json_t *refs = &m->sources[i] == changed ? changed->planned : m->sources[i].active;
        const char *key;
        json_t *value;
        json_object_foreach(refs, key, value) if (json_object_set_new(all, key, json_true())) rc =
            VAULT_ERR_OOM;
    }
    size_t count = json_object_size(all), i = 0;
    const char **hashes = calloc(count ? count : 1, sizeof *hashes);
    if (!hashes)
        rc = VAULT_ERR_OOM;
    const char *key;
    json_t *value;
    if (!rc)
        json_object_foreach(all, key, value) hashes[i++] = key;
    if (!rc && nh_syncd_pin_ring_promote(m->pins, generation, hashes, count))
        rc = VAULT_ERR_IO;
    free(hashes);
    json_decref(all);
    return rc;
}
static int commit_view(nh_vault_manager *m, source *s) {
    json_t *view = json_object_get(s->disk->doc, "view");
    if (!json_is_object(view))
        return 0;
    uint64_t generation = nh_syncd_state_get_local_generation(m->options.state);
    if (generation >= INT64_MAX)
        return PORTHOME_ERR_TOO_LARGE;
    json_int_t prior = json_integer_value(json_object_get(s->disk->doc, "generation"));
    json_t *views = json_object_get(s->disk->doc, "views");
    if (!json_is_object(views)) {
        views = json_object();
        if (!views || json_object_set_new(s->disk->doc, "views", views))
            return VAULT_ERR_OOM;
    }
    char key[32];
    snprintf(key, sizeof key, "%lld", (long long)prior);
    json_t *old = json_object_get(views, key);
    const char *before = json_string_value(json_object_get(old, "event"));
    const char *after = json_string_value(json_object_get(view, "event"));
    int changed = !before || !after || strcmp(before, after) || prior > (json_int_t)generation;
    json_t *conflict;
    size_t conflict_index;
    json_array_foreach(json_object_get(s->disk->doc, "conflicts"), conflict_index,
                       conflict) if (!json_object_get(conflict, "generation")) changed = 1;
    if (changed) {
        if (json_object_size(views) >= VAULT_MAX_OBJECTS)
            return PORTHOME_ERR_TOO_LARGE;
        if (prior > (json_int_t)generation) {
            int rc = nh_syncd_vault_materialize(
                m->home, m->options.config->vaults[s - m->sources].path,
                nh_vault_engine_view(s->engine), m->options.state, &s->disk->io);
            if (rc)
                return rc;
        }
        nh_syncd_state_bump_generation_(m->options.state);
        generation = nh_syncd_state_get_local_generation(m->options.state);
        snprintf(key, sizeof key, "%llu", (unsigned long long)generation);
        if (json_object_set_new(views, key, json_deep_copy(view)) ||
            json_object_set_new(s->disk->doc, "generation", json_integer((json_int_t)generation)))
            return VAULT_ERR_OOM;
        json_t *cache_views = json_object_get(s->disk->doc, "cache_views");
        if (!cache_views) {
            cache_views = json_object();
            if (!cache_views || json_object_set_new(s->disk->doc, "cache_views", cache_views))
                return VAULT_ERR_OOM;
        }
        if (json_object_set_new(cache_views, key, json_deep_copy(s->planned)))
            return VAULT_ERR_OOM;
        json_t *conflict_list = json_object_get(s->disk->doc, "conflicts"), *row;
        size_t k;
        json_array_foreach(conflict_list, k,
                           row) if (!json_object_get(row, "generation") &&
                                    json_object_set_new(
                                        row, "generation",
                                        json_integer((json_int_t)generation))) return VAULT_ERR_OOM;
        int rc = nh_vault_session_save(s->disk);
        if (rc)
            return rc;
    }
    int rc = promote_refs(m, s, generation);
    if (!rc)
        rc = nh_syncd_state_save(m->options.state, m->options.state_dir);
    if (!rc) {
        json_t *active = json_deep_copy(s->planned);
        if (!active)
            return VAULT_ERR_OOM;
        json_decref(s->active);
        s->active = active;
        json_decref(s->planned);
        s->planned = NULL;
        rc = refresh_pins(m);
        if (!rc && changed)
            fprintf(stderr, "syncd: vault committed generation=%llu\n",
                    (unsigned long long)generation);
    }
    return rc;
}
static int install(nh_vault_manager *m, source *s) {
    s->needs_install = 1;
    const char *stage = "publish";
    int rc = nh_vault_engine_publish_pending(s->engine, publish, s->net);
    if (!rc) {
        stage = "prefetch";
        rc = prefetch(m, s);
    }
    if (!rc) {
        stage = "fold-apply";
        rc = nh_vault_engine_fold_apply(s->engine);
    }
    if (!rc) {
        stage = "checkpoint";
        rc = nh_vault_engine_checkpoint(s->engine, publish, s->net);
    }
    if (!rc) {
        stage = "checkpoint-apply";
        rc = nh_vault_engine_fold_apply(s->engine);
    }
    if (!rc) {
        stage = "commit";
        rc = commit_view(m, s);
    }
    if (!rc)
        s->needs_install = 0;
    if (rc)
        fprintf(stderr, "syncd: vault %s rc=%d\n", stage, rc);
    return rc;
}
static int bootstrap_source(nh_vault_manager *m, source *s, int once, int capture_first) {
    const nh_syncd_vault_config_entry *config = &m->options.config->vaults[s - m->sources];
    int rc = 0, stable = 0;
    for (unsigned pass = 0; !rc && pass < 3; ++pass) {
        const uint8_t(*authors)[32];
        size_t count;
        uint8_t before[32], after[32];
        rc = writers(s, &authors, &count, before);
        NostrEvent **events = NULL;
        size_t n = 0;
        if (!rc)
            rc = nh_vault_net_catchup(s->net, config->vault_id, authors, count, &events, &n);
        if (!rc)
            rc = nh_vault_engine_observe_sequence(s->engine, events, n);
        if (!rc && capture_first && pass == 0 && nh_vault_engine_view(s->engine) &&
            nh_vault_engine_can_capture(s->engine, events, n))
            rc = nh_vault_engine_capture(s->engine, NULL, publish, s->net);
        if (!rc)
            rc = nh_vault_engine_events(s->engine, events, n);
        if (!rc && !nh_vault_engine_grant(s->engine) && config->is_owner) {
            int existing = 0;
            char own[65];
            porthome_hex64(nh_vault_engine_writer(s->engine), own);
            for (size_t k = 0; k < n; ++k)
                if (nostr_event_get_kind(events[k]) == PORTHOME_HEAD_KIND &&
                    !strcmp(nostr_event_get_pubkey(events[k]), own) &&
                    nostr_event_check_signature(events[k]))
                    existing = 1;
            rc = existing ? VAULT_ERR_INCOMPLETE
                          : nh_vault_engine_owner_init(s->engine, m->options.legacy_known,
                                                       m->options.root_key);
        }
        events_free(events, n);
        if (!rc && nh_vault_engine_grant(s->engine))
            rc = nh_vault_engine_publish_pending(s->engine, publish, s->net);
        if (!rc)
            rc = writers(s, &authors, &count, after);
        if (rc)
            break;
        memcpy(s->policy_hash, after, 32);
        if (nh_vault_engine_policy(s->engine) && !memcmp(before, after, 32)) {
            stable = 1;
            break;
        }
        if (!nh_vault_engine_grant(s->engine))
            break;
    }
    if (!rc && nh_vault_engine_grant(s->engine) && !stable) rc = VAULT_ERR_INCOMPLETE;
    if (!rc && nh_vault_engine_grant(s->engine))
        rc = install(m, s);
    if (!rc && once && !nh_vault_engine_grant(s->engine))
        rc = VAULT_ERR_AGAIN;
    if (!rc && !once) {
        const uint8_t(*authors)[32];
        size_t count;
        uint8_t hash[32];
        rc = writers(s, &authors, &count, hash);
        if (!rc)
            rc = nh_vault_net_listen(s->net, config->vault_id, authors, count);
        if (!rc) {
            memcpy(s->policy_hash, hash, 32);
            s->listening = 1;
        }
    }
    return rc;
}
int nh_vault_manager_open(const nh_vault_manager_options *o, nh_vault_manager **out) {
    *out = NULL;
    if (!o || !o->config || !o->config->len || !o->state || !o->ignore || !o->identity || !o->cache)
        return VAULT_ERR_ARG;
    nh_vault_manager *m = calloc(1, sizeof *m);
    if (!m)
        return VAULT_ERR_OOM;
    m->home = m->state_dir = m->epoll = m->timer = -1;
    m->options = *o;
    m->home = open(o->home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    m->state_dir = open(o->state_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    m->epoll = epoll_create1(EPOLL_CLOEXEC);
    m->timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    m->sources = calloc(o->config->len, sizeof *m->sources);
    if (m->home < 0 || m->state_dir < 0 || m->epoll < 0 || m->timer < 0 || !m->sources)
        goto fail;
    struct itimerspec cadence = {.it_interval = {60, 0}, .it_value = {60, 0}};
    if (timerfd_settime(m->timer, 0, &cadence, NULL))
        goto fail;
    struct epoll_event event = {.events = EPOLLIN, .data.u32 = 0};
    if (epoll_ctl(m->epoll, EPOLL_CTL_ADD, m->timer, &event))
        goto fail;
    m->owned_pins = json_object();
    char *pin_path = malloc(strlen(o->state_dir) + 32);
    if (!m->owned_pins || !pin_path) {
        free(pin_path);
        goto fail;
    }
    sprintf(pin_path, "%s/vault-pinned.json", o->state_dir);
    int pin_result = nh_syncd_pin_ring_open(pin_path, &m->pins);
    free(pin_path);
    if (pin_result)
        goto fail;
    for (size_t i = 0; i < o->config->len; ++i) {
        source *s = &m->sources[i];
        ++m->count;
        int rc = nh_vault_session_open(o->state_dir, o->config->vaults[i].vault_id, o->home_key,
                                       &s->disk);
        if (rc)
            goto fail;
        s->disk->cache = o->cache;
        json_t *by_generation = json_object_get(s->disk->doc, "cache_views"), *refs;
        const char *key;
        uint64_t best = 0, generation = nh_syncd_state_get_local_generation(o->state);
        json_object_foreach(by_generation, key, refs) {
            char *end = NULL;
            errno = 0;
            unsigned long long g = strtoull(key, &end, 10);
            if (errno || !*key || *end || !json_is_object(refs))
                goto fail;
            if (g <= generation && g >= best) {
                best = g;
                json_decref(s->active);
                s->active = json_deep_copy(refs);
            }
        }
        if (!s->active)
            s->active = json_object();
        if (!s->active)
            goto fail;
    }
    if (refresh_pins(m))
        goto fail;
    for (size_t i = 0; i < o->config->len; ++i) {
        source *s = &m->sources[i];
        int rc = nh_vault_net_open(o->identity, o->relays, o->relays_len, o->blossom,
                                   o->blossom_len, o->replication, o->test_mode, &s->net);
        if (rc)
            goto fail;
        s->disk->remote = nh_vault_net_blobs(s->net);
        s->disk->reconcile = nh_vault_net_reconcile;
        char *identity = nostr_key_get_public(o->identity);
        json_t *scope = identity ? json_pack("{s:i,s:[],s:s}", "replication", (int)o->replication,
                                             "servers", "identity", identity)
                                 : NULL;
        free(identity);
        if (!scope)
            goto fail;
        for (size_t k = 0; k < o->blossom_len; ++k)
            json_array_append_new(json_object_get(scope, "servers"), json_string(o->blossom[k]));
        char *wire = json_dumps(scope, JSON_COMPACT);
        json_decref(scope);
        if (!wire)
            goto fail;
        uint8_t hash[32];
        SHA256((uint8_t *)wire, strlen(wire), hash);
        free(wire);
        porthome_hex64(hash, s->disk->remote_scope);
        rc = nh_vault_engine_open(&o->config->vaults[i], s->disk, m->home, o->state, o->ignore,
                                  o->identity, &s->engine);
        if (rc)
            goto fail;
        event = (struct epoll_event){.events = EPOLLIN, .data.u32 = (uint32_t)i + 1};
        if (epoll_ctl(m->epoll, EPOLL_CTL_ADD, nh_vault_net_fd(s->net), &event))
            goto fail;
    }
    *out = m;
    return 0;
fail:
    nh_vault_manager_close(m);
    return VAULT_ERR_IO;
}
int nh_vault_manager_bootstrap(nh_vault_manager *m, int once) {
    int rc = 0;
    for (size_t i = 0; !rc && i < m->count; ++i)
        rc = bootstrap_source(m, &m->sources[i], once, 1);
    if (!rc)
        fprintf(stderr, "syncd: vault catch-up complete sources=%zu\n", m->count);
    return status(m, rc);
}
int nh_vault_manager_fd(const nh_vault_manager *m) { return m ? m->epoll : -1; }
int nh_vault_manager_capture(nh_vault_manager *m, const nh_syncd_batch *batch) {
    int rc = 0;
    for (size_t i = 0; !rc && i < m->count; ++i) {
        source *s = &m->sources[i];
        NostrEvent **received = NULL;
        size_t count = 0;
        rc = nh_vault_net_drain(s->net, &received, &count);
        if (!rc)
            rc = nh_vault_engine_observe_sequence(s->engine, received, count);
        if (!rc && nh_vault_engine_can_capture(s->engine, received, count))
            rc = nh_vault_engine_capture(s->engine, batch, publish, s->net);
        if (!rc)
            rc = nh_vault_engine_events(s->engine, received, count);
        events_free(received, count);
        const uint8_t(*authors)[32];
        size_t author_count;
        uint8_t hash[32];
        if (!rc)
            rc = writers(s, &authors, &author_count, hash);
        if (!rc && memcmp(hash, s->policy_hash, 32))
            rc = bootstrap_source(m, s, 0, 0);
        else if (!rc && nh_vault_engine_policy(s->engine))
            rc = install(m, s);
        if (rc)
            fprintf(stderr, "syncd: vault capture/reconcile rc=%d\n", rc);
    }
    return status(m, rc);
}
int nh_vault_manager_service(nh_vault_manager *m) {
    struct epoll_event events[65];
    int n = epoll_wait(m->epoll, events, 65, 0), rc = n < 0 ? VAULT_ERR_IO : 0;
    for (int k = 0; !rc && k < n; ++k) {
        if (!events[k].data.u32) {
            uint64_t ticks;
            ssize_t got = read(m->timer, &ticks, sizeof ticks);
            if (got == sizeof ticks)
                rc = nh_vault_manager_retention(m);
            continue;
        }
        size_t i = events[k].data.u32 - 1;
        if (i >= m->count) {
            rc = VAULT_ERR_ARG;
            break;
        }
        source *s = &m->sources[i];
        NostrEvent **received = NULL;
        size_t count = 0;
        rc = nh_vault_net_drain(s->net, &received, &count);
        if (!rc)
            rc = nh_vault_engine_observe_sequence(s->engine, received, count);
        if (!rc)
            rc = nh_vault_engine_events(s->engine, received, count);
        events_free(received, count);
        const uint8_t(*authors)[32];
        size_t author_count;
        uint8_t hash[32];
        if (!rc)
            rc = writers(s, &authors, &author_count, hash);
        if (!rc && memcmp(hash, s->policy_hash, 32))
            rc = bootstrap_source(m, s, 0, 0);
        else if (!rc && nh_vault_engine_policy(s->engine))
            rc = install(m, s);
    }
    return status(m, rc);
}
static int budget_write(nh_vault_manager *m, uint64_t start, uint64_t used) {
    json_t *j = json_pack("{s:I,s:I}", "start", (json_int_t)start, "used", (json_int_t)used);
    char *wire = j ? json_dumps(j, JSON_COMPACT) : NULL;
    json_decref(j);
    if (!wire)
        return VAULT_ERR_OOM;
    uint8_t *sealed = NULL;
    size_t n = 0;
    int rc =
        porthome_encrypt_manifest(m->options.home_key, (uint8_t *)wire, strlen(wire), &sealed, &n);
    free(wire);
    if (!rc)
        rc = nh_vault_file_write(m->state_dir, "vault-retention.sealed", sealed, n);
    free(sealed);
    return rc;
}
static int budget_load(nh_vault_manager *m, uint64_t *out_start, uint64_t *out_used) {
    uint64_t now = (uint64_t)time(NULL), start = now, used = 0;
    uint8_t *sealed = NULL, *plain = NULL;
    size_t n = 0, len = 0;
    int rc = nh_vault_file_read(m->state_dir, "vault-retention.sealed", &sealed, &n);
    if (!rc)
        rc = porthome_decrypt_manifest(m->options.home_key, sealed, n, &plain, &len);
    if (!rc) {
        json_t *j = json_loadb((char *)plain, len, JSON_REJECT_DUPLICATES, NULL);
        json_int_t a = json_integer_value(json_object_get(j, "start")),
                   b = json_integer_value(json_object_get(j, "used"));
        if (!json_is_object(j) || json_object_size(j) != 2 ||
            !json_is_integer(json_object_get(j, "start")) ||
            !json_is_integer(json_object_get(j, "used")) || a <= 0 || b < 0)
            rc = VAULT_ERR_ARG;
        else {
            start = (uint64_t)a;
            used = (uint64_t)b;
        }
        json_decref(j);
    }
    if (rc == VAULT_ERR_INCOMPLETE)
        rc = 0;
    free(sealed);
    if (plain) {
        OPENSSL_cleanse(plain, len);
        free(plain);
    }
    if (rc)
        return rc;
    if (now >= start && now - start >= NH_SYNCD_CACHE_SWEEP_PERIOD_SECS) {
        start = now;
        used = 0;
    }
    *out_start = start;
    *out_used = used;
    return 0;
}
int nh_vault_manager_retention(nh_vault_manager *m) {
    if (m->options.retention_budget > INT64_MAX)
        return VAULT_ERR_ARG;
    int rc = 0;
    for (size_t i = 0; !rc && i < m->count; ++i)
        if (m->sources[i].needs_install)
            rc = install(m, &m->sources[i]);
    uint64_t start = 0, used = 0;
    if (!rc)
        rc = budget_load(m, &start, &used);
    for (size_t i = 0; !rc && used < m->options.retention_budget && i < m->count; ++i) {
        source *s = &m->sources[i];
        if (!nh_vault_engine_view(s->engine))
            continue;
        uint64_t allowance = m->options.retention_budget - used, reserved = 0;
        /* Reserve the complete allowance durably before I/O. A crash may spend
         * less, never more, than this weekly class reservation. */
        rc = budget_write(m, start, used + allowance);
        if (!rc)
            rc = nh_vault_engine_retention(s->engine, allowance, publish, s->net, &reserved);
        if (reserved > allowance) {
            rc = VAULT_ERR_BUDGET;
            reserved = allowance;
        }
        used += reserved;
        int saved = budget_write(m, start, used);
        if (!rc)
            rc = saved;
        if (reserved)
            s->needs_install = 1;
        if (!rc && s->needs_install)
            rc = install(m, s);
        if (!rc)
            rc = budget_load(m, &start, &used);
    }
    return status(m, rc);
}
void nh_vault_manager_close(nh_vault_manager *m) {
    if (!m)
        return;
    for (size_t i = 0; i < m->count; ++i) {
        nh_vault_net_close(m->sources[i].net);
        nh_vault_engine_close(m->sources[i].engine);
        nh_vault_session_close(m->sources[i].disk);
        json_decref(m->sources[i].active);
        json_decref(m->sources[i].planned);
    }
    if (m->timer >= 0)
        close(m->timer);
    if (m->epoll >= 0)
        close(m->epoll);
    if (m->state_dir >= 0)
        close(m->state_dir);
    if (m->home >= 0)
        close(m->home);
    nh_syncd_pin_ring_close(m->pins);
    json_decref(m->owned_pins);
    free(m->sources);
    OPENSSL_cleanse(m, sizeof *m);
    free(m);
}