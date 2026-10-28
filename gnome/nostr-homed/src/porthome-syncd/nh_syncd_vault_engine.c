/* SPDX-License-Identifier: MIT */
#include "nh_syncd_vault_engine.h"
#include "nh_syncd_conflicts.h"
#include <vault/nip59.h>
#include <porthome/wrapkey.h>
#include <nostr-keys.h>
#include <json.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

int nh_syncd_state_delete_(nh_syncd_state *, const char *);

typedef struct {
    NostrEvent *event;
    vault_record *record;
    vault_tree *tree;
    json_t *snapshot;
    uint8_t writer[32];
} current_head;
struct nh_vault_engine {
    const nh_syncd_vault_config_entry *config;
    nh_vault_session *session;
    nh_syncd_state *state;
    const nh_syncd_ignore *ignore;
    int home;
    const char *secret;
    uint8_t writer[32];
    vault_grant *grant;
    current_head heads[VAULT_MAX_HEADS];
    size_t count;
    vault_record *base_record;
    vault_tree *base;
};
static int open_relative(int home, const char *path, int directory) {
    if (!path || !*path || *path == '/') {
        errno = EINVAL;
        return -1;
    }
    char *copy = strdup(path);
    if (!copy)
        return -1;
    int current = fcntl(home, F_DUPFD_CLOEXEC, 0);
    char *part = copy;
    while (current >= 0) {
        char *slash = strchr(part, '/');
        if (slash)
            *slash = 0;
        if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) {
            close(current);
            current = -1;
            errno = EINVAL;
            break;
        }
        int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
        if (slash || directory)
            flags |= O_DIRECTORY;
        int next = openat(current, part, flags);
        int saved = errno;
        close(current);
        current = next;
        errno = saved;
        if (!slash || current < 0)
            break;
        part = slash + 1;
    }
    free(copy);
    return current;
}
static void head_clear(current_head *h) {
    nostr_event_free(h->event);
    vault_record_free(h->record);
    vault_tree_free(h->tree);
    nh_vault_secret_json_free(h->snapshot);
    memset(h, 0, sizeof *h);
}
static int set_secret(json_t *object, const char *key, const json_t *value) {
    json_t *copy = json_deep_copy(value);
    if (!copy)
        return VAULT_ERR_OOM;
    json_t *old = json_object_get(object, key);
    json_incref(old);
    int rc = json_object_set_new(object, key, copy) ? VAULT_ERR_OOM : 0;
    nh_vault_secret_json_free(old);
    return rc;
}
static NostrEvent *event_from_snapshot(const json_t *j) {
    const char *wire = json_string_value(json_object_get(j, "event"));
    NostrEvent *event = wire ? nostr_event_new() : NULL;
    if (event && nostr_event_deserialize(event, wire)) {
        nostr_event_free(event);
        event = NULL;
    }
    return event;
}
static int known_head(const nh_vault_engine *e, const char *id) {
    for (size_t i = 0; i < e->count; ++i)
        if (!strcmp(e->heads[i].event->id, id)) return 1;
    size_t i; json_t *snapshot;
    json_array_foreach(json_object_get(e->session->doc, "retained"), i, snapshot) {
        NostrEvent *event = event_from_snapshot(snapshot);
        int match = event && event->id && !strcmp(event->id, id);
        nostr_event_free(event);
        if (match) return 1;
    }
    return 0;
}
static int save_heads(nh_vault_engine *e) {
    json_t *array = json_array();
    if (!array)
        return VAULT_ERR_OOM;
    int rc = 0;
    for (size_t i = 0; !rc && i < e->count; ++i)
        if (json_array_append_new(array, json_deep_copy(e->heads[i].snapshot)))
            rc = VAULT_ERR_OOM;
    if (!rc)
        rc = set_secret(e->session->doc, "heads", array);
    nh_vault_secret_json_free(array);
    return rc ? rc : nh_vault_session_save(e->session);
}
static int retain(nh_vault_engine *e, const current_head *h) {
    json_t *retained = json_object_get(e->session->doc, "retained");
    const char *wire = json_string_value(json_object_get(h->snapshot, "event"));
    size_t i;
    json_t *item;
    json_array_foreach(retained, i, item) {
        const char *old = json_string_value(json_object_get(item, "event"));
        if (old && !strcmp(old, wire))
            return 0;
    }
    if (json_array_size(retained) >= VAULT_MAX_OBJECTS)
        return PORTHOME_ERR_TOO_LARGE;
    return json_array_append_new(retained, json_deep_copy(h->snapshot)) ? VAULT_ERR_OOM : 0;
}
static int insert(nh_vault_engine *e, current_head *h) {
    const porthome_head *body = vault_record_head(h->record);
    for (size_t i = 0; i < e->count;) {
        if (!memcmp(e->heads[i].writer, h->writer, 32)) {
            const porthome_head *old = vault_record_head(e->heads[i].record);
            if (!strcmp(e->heads[i].event->id, h->event->id) || old->seq > body->seq) {
                head_clear(h);
                return 0;
            }
            if (old->seq < body->seq) {
                int rc = retain(e, &e->heads[i]);
                if (rc)
                    return rc;
                head_clear(&e->heads[i]);
                e->heads[i] = e->heads[--e->count];
                memset(&e->heads[e->count], 0, sizeof e->heads[e->count]);
                continue;
            }
        }
        ++i;
    }
    if (e->count == VAULT_MAX_HEADS)
        return PORTHOME_ERR_TOO_LARGE;
    e->heads[e->count++] = *h;
    memset(h, 0, sizeof *h);
    return 0;
}
static size_t owner_index(const nh_vault_engine *e) {
    size_t chosen = SIZE_MAX;
    if (!e->grant)
        return chosen;
    uint64_t epoch = e->grant->ring.entries[e->grant->ring.count - 1].epoch;
    for (size_t i = 0; i < e->count; ++i) {
        const porthome_head *h = vault_record_head(e->heads[i].record);
        const porthome_dir_node *root = vault_record_root(e->heads[i].record);
        if (memcmp(e->heads[i].writer, e->grant->owner, 32) || h->epoch != epoch || !root->vault ||
            memcmp(root->vault->owner, e->grant->owner, 32) ||
            memcmp(root->vault->vault_id, e->grant->vault_id, 32) ||
            h->grants_rev != root->vault->grants_rev)
            continue;
        if (chosen == SIZE_MAX || h->seq > vault_record_head(e->heads[chosen].record)->seq)
            chosen = i;
    }
    return chosen;
}
const porthome_vault_info *nh_vault_engine_policy(const nh_vault_engine *e) {
    size_t i = owner_index(e);
    return i == SIZE_MAX ? NULL : vault_record_root(e->heads[i].record)->vault;
}
static int writer_allowed(const nh_vault_engine *e, const uint8_t writer[32]) {
    if (e->grant && !memcmp(writer, e->grant->owner, 32))
        return 1;
    const porthome_vault_info *policy = nh_vault_engine_policy(e);
    for (size_t i = 0; policy && i < policy->writers_len; ++i)
        if (!memcmp(writer, policy->writers[i], 32))
            return 1;
    return 0;
}
static int open_snapshot(nh_vault_engine *e, const json_t *snapshot, current_head *h) {
    memset(h, 0, sizeof *h);
    h->event = event_from_snapshot(snapshot);
    const char *pub = h->event ? nostr_event_get_pubkey(h->event) : NULL;
    if (!pub || porthome_from_hex64(pub, h->writer)) {
        head_clear(h);
        return VAULT_ERR_ARG;
    }
    int rc = nh_vault_snapshot_open(e->session, e->grant, snapshot, &h->record, &h->tree);
    if (!rc) {
        h->snapshot = json_deep_copy(snapshot);
        if (!h->snapshot)
            rc = VAULT_ERR_OOM;
    }
    if (rc)
        head_clear(h);
    return rc;
}
int nh_vault_engine_open(const nh_syncd_vault_config_entry *config, nh_vault_session *session,
                         int home, nh_syncd_state *state, const nh_syncd_ignore *ignore,
                         const char *secret, nh_vault_engine **out) {
    *out = NULL;
    nh_vault_engine *e = calloc(1, sizeof *e);
    if (!e)
        return VAULT_ERR_OOM;
    e->config = config;
    e->session = session;
    e->home = home;
    e->state = state;
    e->ignore = ignore;
    e->secret = secret;
    char *pub = nostr_key_get_public(secret);
    int rc = pub ? porthome_from_hex64(pub, e->writer) : VAULT_ERR_ARG;
    free(pub);
    const char *body = json_string_value(json_object_get(session->doc, "grant"));
    if (!rc && body)
        rc = vault_grant_decode(body, &e->grant);
    if (!rc && e->grant &&
        (memcmp(e->grant->vault_id, config->vault_id, 32) ||
         (config->is_owner && memcmp(e->grant->owner, e->writer, 32))))
        rc = VAULT_ERR_AUTH;
    json_t *view = json_object_get(session->doc, "view");
    if (!json_is_object(view) && config->is_owner)
        view = json_object_get(session->doc, "own");
    if (!rc && e->grant && json_is_object(view))
        rc = nh_vault_snapshot_open(session, e->grant, view, &e->base_record, &e->base);
    json_t *array = json_object_get(session->doc, "heads"), *snapshot;
    size_t i;
    if (json_array_size(array) > VAULT_MAX_HEADS)
        rc = PORTHOME_ERR_TOO_LARGE;
    json_array_foreach(array, i, snapshot) {
        if (rc || !e->grant) {
            if (!rc)
                rc = VAULT_ERR_ARG;
            break;
        }
        current_head h;
        rc = open_snapshot(e, snapshot, &h);
        if (!rc)
            rc = insert(e, &h);
        head_clear(&h);
    }
    if (rc) {
        nh_vault_engine_close(e);
        return rc;
    }
    *out = e;
    return 0;
}
static int store_grant(nh_vault_engine *e, vault_grant *grant) {
    char *wire = NULL;
    int rc = vault_grant_encode(grant, &wire);
    if (!rc && json_object_set_new(e->session->doc, "grant", json_string(wire)))
        rc = VAULT_ERR_OOM;
    if (wire) {
        OPENSSL_cleanse(wire, strlen(wire));
        free(wire);
    }
    if (!rc)
        rc = nh_vault_session_save(e->session);
    if (!rc) {
        vault_grant_free(e->grant);
        e->grant = grant;
    }
    return rc;
}
static int prepare(nh_vault_engine *e, vault_tree *tree, int checkpoint) {
    porthome_ring_t ring;
    const porthome_dir_node *root = vault_tree_node_at(tree, 0, &ring);
    if (!root || !root->vault || !writer_allowed(e, e->writer))
        return VAULT_ERR_AUTH;
    if (root->epoch != e->grant->ring.entries[e->grant->ring.count - 1].epoch)
        return VAULT_ERR_EPOCH;
    const porthome_vault_info *current_policy = nh_vault_engine_policy(e);
    if (current_policy && root->vault->grants_rev != current_policy->grants_rev)
        return VAULT_ERR_INCOMPLETE;
    uint8_t address[32];
    int rc = vault_tree_seal(tree, &e->session->io, address);
    if (rc)
        return rc;
    if (e->base_record && !checkpoint) {
        const porthome_head *base = vault_record_head(e->base_record);
        if (base->epoch == root->epoch && base->grants_rev == root->vault->grants_rev &&
            !memcmp(base->root_manifest, address, 32))
            return 0;
    }
    porthome_head head = {.epoch = root->epoch,
                          .seq = 1,
                          .grants_rev = root->vault->grants_rev,
                          .written_at = (uint64_t)time(NULL)};
    memcpy(head.vault_id, e->config->vault_id, 32);
    memcpy(head.root_manifest, address, 32);
    current_head previous = {0};
    json_t *own = json_object_get(e->session->doc, "own");
    if (json_is_object(own)) {
        rc = open_snapshot(e, own, &previous);
        if (rc)
            return rc;
        const porthome_head *old = vault_record_head(previous.record);
        if (!checkpoint && old->epoch == root->epoch &&
            old->grants_rev == root->vault->grants_rev &&
            !memcmp(old->root_manifest, address, 32)) {
            head_clear(&previous);
            return 0;
        }
        if (old->seq >= INT64_MAX || old->written_at >= INT64_MAX)
            rc = PORTHOME_ERR_TOO_LARGE;
        else {
            head.seq = old->seq + 1;
            if (head.written_at <= old->written_at)
                head.written_at = old->written_at + 1;
        }
    }
    json_int_t observed = json_integer_value(json_object_get(e->session->doc, "observed_seq"));
    json_int_t observed_at = json_integer_value(json_object_get(e->session->doc, "observed_time"));
    if (observed == INT64_MAX || observed_at == INT64_MAX)
        rc = PORTHOME_ERR_TOO_LARGE;
    if (observed > 0 && (uint64_t)observed >= head.seq)
        head.seq = (uint64_t)observed + 1;
    if (observed_at > 0 && (uint64_t)observed_at >= head.written_at)
        head.written_at = (uint64_t)observed_at + 1;
    const char *saved = json_string_value(json_object_get(e->session->doc, "coverage"));
    porthome_head *coverage = NULL;
    if (!rc && saved)
        rc = porthome_head_decode((const uint8_t *)saved, strlen(saved), &coverage);
    if (!rc && coverage && memcmp(coverage->vault_id, e->config->vault_id, 32))
        rc = VAULT_ERR_AUTH;
    if (!rc && coverage) {
        head.parents = coverage->parents;
        head.parents_len = coverage->parents_len;
        coverage->parents = NULL;
        coverage->parents_len = 0;
    }
    const vault_record *included[1] = {previous.record};
    if (!rc)
        rc = vault_head_cover(&head, included, previous.record ? 1 : 0);
    const uint8_t *key = NULL;
    for (size_t i = 0; i < ring.count; ++i)
        if (ring.entries[i].epoch == root->epoch)
            key = ring.entries[i].node_key;
    NostrEvent *event = NULL;
    if (!rc && !key)
        rc = VAULT_ERR_EPOCH;
    if (!rc)
        rc = porthome_head_event_new(&head, key, e->writer, &event);
    if (!rc)
        rc = nostr_event_sign(event, e->secret);
    json_t *snapshot = NULL;
    if (!rc)
        rc = nh_vault_snapshot_new(event, tree, &snapshot);
    if (!rc)
        rc = set_secret(e->session->doc, "pending", snapshot);
    if (!rc)
        rc = nh_vault_session_save(e->session);
    nh_vault_secret_json_free(snapshot);
    nostr_event_free(event);
    porthome_head_dispose(&head);
    head_clear(&previous);
    if (coverage) {
        porthome_head_dispose(coverage);
        free(coverage);
    }
    return rc;
}
int nh_vault_engine_owner_init(nh_vault_engine *e, int legacy, const uint8_t root_key[32]) {
    if (e->grant)
        return 0;
    if (!e->config->is_owner)
        return VAULT_ERR_AGAIN;
    uint8_t id[32];
    int rc = porthome_wrap_seed_random(id);
    porthome_vault_info policy = {
        .writers = (uint8_t(*)[32]) & e->writer, .writers_len = 1, .grants_rev = 1};
    memcpy(policy.owner, e->writer, 32);
    memcpy(policy.vault_id, e->config->vault_id, 32);
    vault_tree *tree = NULL;
    uint8_t independent[32] = {0};
    if (!rc && !legacy) rc = porthome_child_node_key(root_key, id, NULL, independent);
    const uint8_t *key = legacy ? e->session->home_key : independent;
    if (!rc && legacy)
        rc = nh_syncd_vault_migrate(e->state, e->config->path, &policy, id, key, &tree);
    if (!rc && !legacy) {
        porthome_dir_node node = {.epoch = 1, .vault = &policy};
        memcpy(node.node_id, id, 32);
        porthome_ring_entry_t entry = {.epoch = 1};
        memcpy(entry.node_key, key, 32);
        vault_tree_node n = {&node, {&entry, 1}};
        rc = vault_tree_new(&n, 1, &tree);
        int fd = open_relative(e->home, e->config->path, 1);
        if (!rc && fd >= 0) {
            vault_tree *captured = NULL;
            nh_syncd_vault_scan scan = {.ignore = e->ignore, .home_prefix = e->config->path};
            rc = nh_syncd_vault_capture(fd, tree, &scan, &e->session->io, &captured);
            if (!rc) {
                vault_tree_free(tree);
                tree = captured;
            }
        } else if (!rc && errno != ENOENT)
            rc = VAULT_ERR_IO;
        if (fd >= 0)
            close(fd);
    }
    vault_grant *g = rc ? NULL : calloc(1, sizeof *g);
    if (!rc && !g)
        rc = VAULT_ERR_OOM;
    if (!rc) {
        memcpy(g->vault_id, e->config->vault_id, 32);
        memcpy(g->node_id, id, 32);
        memcpy(g->owner, e->writer, 32);
        g->cap = VAULT_CAP_WRITE;
        g->issued_at = (uint64_t)time(NULL);
        rc = vault_ring_init(&g->ring, 1, key);
    }
    if (!rc)
        rc = vault_tree_seal(tree, &e->session->io, g->root_manifest);
    if (!rc) {
        rc = store_grant(e, g);
        if (!rc)
            g = NULL;
    }
    if (!rc)
        rc = prepare(e, tree, 0);
    vault_grant_free(g);
    vault_tree_free(tree);
    OPENSSL_cleanse(independent, sizeof independent);
    return rc;
}
typedef struct {
    const NostrEvent *event;
    int taken;
} one_wrap;
static int receive_wrap(void *ctx, NostrEvent **out) {
    one_wrap *one = ctx;
    *out = NULL;
    if (one->taken++)
        return VAULT_ERR_AGAIN;
    char *wire = nostr_event_serialize(one->event);
    *out = wire ? nostr_event_new() : NULL;
    int rc = *out ? nostr_event_deserialize(*out, wire) : VAULT_ERR_OOM;
    free(wire);
    return rc;
}
static int decode_grant(const nh_vault_engine *e, const NostrEvent *event, vault_grant **out) {
    *out = NULL;
    uint8_t sk[32];
    if (porthome_from_hex64(e->secret, sk))
        return VAULT_ERR_ARG;
    one_wrap one = {event, 0};
    vault_nip59_transport io = {.ctx = &one, .receive = receive_wrap};
    vault_nip59_carrier *carrier = NULL;
    vault_grant *g = NULL;
    int rc = vault_nip59_carrier_new(sk, &io, &carrier);
    OPENSSL_cleanse(sk, sizeof sk);
    if (!rc)
        rc = vault_grant_receive(vault_nip59_carrier_interface(carrier), &g);
    vault_nip59_carrier_free(carrier);
    if (rc)
        vault_grant_free(g);
    else
        *out = g;
    return rc;
}
static int receive_grant(nh_vault_engine *e, const NostrEvent *event) {
    vault_grant *g = NULL;
    int rc = decode_grant(e, event, &g);
    if (rc)
        return rc;
    if (memcmp(g->vault_id, e->config->vault_id, 32)) {
        vault_grant_free(g);
        return 0;
    }
    if (e->grant) {
        uint64_t old = e->grant->ring.entries[e->grant->ring.count - 1].epoch;
        uint64_t current = g->ring.entries[g->ring.count - 1].epoch;
        if (memcmp(g->owner, e->grant->owner, 32) || memcmp(g->node_id, e->grant->node_id, 32))
            rc = VAULT_ERR_AUTH;
        else if (current < old) {
            vault_grant_free(g);
            return 0;
        } else if (current == old &&
                   CRYPTO_memcmp(g->ring.entries[g->ring.count - 1].node_key,
                                 e->grant->ring.entries[e->grant->ring.count - 1].node_key, 32))
            rc = VAULT_ERR_AUTH;
    }
    if (!rc && e->config->is_owner && memcmp(g->owner, e->writer, 32))
        rc = VAULT_ERR_AUTH;
    if (!rc && e->grant) {
        size_t capacity = e->grant->ring.count + g->ring.count;
        porthome_ring_entry_t *keys = calloc(capacity, sizeof *keys);
        if (!keys)
            rc = VAULT_ERR_OOM;
        size_t a = 0, b = 0, count = 0;
        while (!rc && (a < e->grant->ring.count || b < g->ring.count)) {
            const porthome_ring_entry_t *old =
                a < e->grant->ring.count ? &e->grant->ring.entries[a] : NULL;
            const porthome_ring_entry_t *newer = b < g->ring.count ? &g->ring.entries[b] : NULL;
            if (old && newer && old->epoch == newer->epoch) {
                if (CRYPTO_memcmp(old->node_key, newer->node_key, 32)) {
                    rc = VAULT_ERR_AUTH;
                    break;
                }
                keys[count++] = *old;
                ++a;
                ++b;
            } else if (old && (!newer || old->epoch < newer->epoch)) {
                keys[count++] = *old;
                ++a;
            } else {
                keys[count++] = *newer;
                ++b;
            }
        }
        vault_ring merged = {0};
        porthome_ring_t view = {keys, count};
        if (!rc)
            rc = vault_ring_copy(&merged, &view);
        if (keys) {
            OPENSSL_cleanse(keys, capacity * sizeof *keys);
            free(keys);
        }
        if (!rc) {
            vault_ring_dispose(&g->ring);
            g->ring = merged;
        }
    }
    if (!rc) {
        rc = store_grant(e, g);
        if (!rc)
            g = NULL;
    }
    vault_grant_free(g);
    return rc;
}
int nh_vault_engine_can_capture(const nh_vault_engine *e, NostrEvent *const *events, size_t n) {
    if (!e->grant || !e->base)
        return 0;
    const porthome_dir_node *base = vault_tree_node_at(e->base, 0, NULL);
    porthome_ring_t ring = vault_ring_view(&e->grant->ring);
    uint64_t epoch = ring.entries[ring.count - 1].epoch;
    const porthome_vault_info *policy = nh_vault_engine_policy(e);
    if (base->epoch != epoch ||
        (policy && base->vault && policy->grants_rev != base->vault->grants_rev)) {
        fprintf(stderr, "syncd: vault local capture deferred: base key/policy changed\n");
        return 0;
    }
    for (size_t i = 0; i < n; ++i) {
        if (nostr_event_get_kind(events[i]) == 1059) {
            vault_grant *g = NULL;
            int rc = decode_grant(e, events[i], &g);
            int rotation = !rc && !memcmp(g->vault_id, e->grant->vault_id, 32) &&
                           !memcmp(g->owner, e->grant->owner, 32) &&
                           !memcmp(g->node_id, e->grant->node_id, 32) &&
                           g->ring.entries[g->ring.count - 1].epoch > epoch;
            vault_grant_free(g);
            if (rotation)
                return 0;
        }
        uint8_t writer[32];
        if (nostr_event_get_kind(events[i]) != PORTHOME_HEAD_KIND ||
            porthome_from_hex64(nostr_event_get_pubkey(events[i]), writer) ||
            memcmp(writer, e->grant->owner, 32) || !nostr_event_check_signature(events[i]))
            continue;
        if (known_head(e, events[i]->id)) continue;
        porthome_head *head = NULL;
        int rc = porthome_head_event_open(events[i], e->grant->vault_id, &ring, epoch, &head);
        size_t owner = owner_index(e);
        const porthome_head *current = owner == SIZE_MAX ? NULL : vault_record_head(e->heads[owner].record);
        if (!rc && current && head->seq < current->seq) { porthome_head_dispose(head); free(head); continue; }
        int compatible = !rc && base->vault && head->epoch == base->epoch &&
                         head->grants_rev == base->vault->grants_rev &&
                         (!current || head->seq != current->seq || !memcmp(head->root_manifest, current->root_manifest, 32));
        if (head) {
            porthome_head_dispose(head);
            free(head);
        }
        if (!compatible) {
            fprintf(stderr, "syncd: vault local capture deferred: incoming owner key/policy changed\n");
            return 0;
        }
    }
    return 1;
}
int nh_vault_engine_observe_sequence(nh_vault_engine *e, NostrEvent *const *events, size_t n) {
    if (!e->grant)
        return 0;
    porthome_ring_t ring = vault_ring_view(&e->grant->ring);
    uint64_t epoch = ring.entries[ring.count - 1].epoch;
    json_int_t seq = json_integer_value(json_object_get(e->session->doc, "observed_seq"));
    json_int_t when = json_integer_value(json_object_get(e->session->doc, "observed_time"));
    for (size_t i = 0; i < n; ++i) {
        uint8_t writer[32];
        if (nostr_event_get_kind(events[i]) != PORTHOME_HEAD_KIND ||
            porthome_from_hex64(nostr_event_get_pubkey(events[i]), writer) ||
            memcmp(writer, e->writer, 32))
            continue;
        porthome_head *head = NULL;
        if (porthome_head_event_open(events[i], e->grant->vault_id, &ring, epoch, &head))
            continue;
        if (head->seq > INT64_MAX || head->written_at > INT64_MAX) {
            porthome_head_dispose(head);
            free(head);
            return PORTHOME_ERR_TOO_LARGE;
        }
        if ((json_int_t)head->seq > seq)
            seq = (json_int_t)head->seq;
        if ((json_int_t)head->written_at > when)
            when = (json_int_t)head->written_at;
        porthome_head_dispose(head);
        free(head);
    }
    if (json_object_set_new(e->session->doc, "observed_seq", json_integer(seq)) ||
        json_object_set_new(e->session->doc, "observed_time", json_integer(when)))
        return VAULT_ERR_OOM;
    return nh_vault_session_save(e->session);
}
int nh_vault_engine_events(nh_vault_engine *e, NostrEvent *const *events, size_t n) {
    int rc = 0;
    for (size_t i = 0; !rc && i < n; ++i) {
        if (nostr_event_get_kind(events[i]) != 1059)
            continue;
        rc = receive_grant(e, events[i]);
        /* An unrelated/invalid gift wrap is not an authority or a stream veto. */
        if (rc != VAULT_ERR_OOM && rc != VAULT_ERR_IO)
            rc = 0;
    }
    if (rc || !e->grant)
        return rc;
    /* Owner first: only its authenticated policy admits other authors. */
    for (int pass = 0; pass < 2 && !rc; ++pass)
        for (size_t i = 0; i < n && !rc; ++i) {
            if (nostr_event_get_kind(events[i]) != PORTHOME_HEAD_KIND)
                continue;
            uint8_t writer[32];
            if (porthome_from_hex64(nostr_event_get_pubkey(events[i]), writer))
                continue;
            int owner = !memcmp(writer, e->grant->owner, 32);
            if (owner != (pass == 0) || !writer_allowed(e, writer))
                continue;
            if (known_head(e, events[i]->id)) continue;
            porthome_ring_t ring = vault_ring_view(&e->grant->ring);
            uint64_t epoch = ring.entries[ring.count - 1].epoch;
            current_head h = {0};
            memcpy(h.writer, writer, 32);
            porthome_head *head = NULL;
            rc = porthome_head_event_open(events[i], e->grant->vault_id, &ring, epoch, &head);
            /* Never advertise the old owner view as current when a signed
             * owner update cannot be opened with the explicit grant epoch.
             * Grant processing precedes heads; a later delivery/restart replays
             * current heads without probing other ring entries. */
            if (rc) {
                if (owner && nostr_event_check_signature(events[i])) {
                    rc = VAULT_ERR_INCOMPLETE;
                    break;
                }
                rc = 0;
                continue;
            }
            if (head) {
                const porthome_vault_info *policy = nh_vault_engine_policy(e);
                int stale = !owner && policy && head->grants_rev != policy->grants_rev;
                porthome_head_dispose(head);
                free(head);
                if (stale)
                    continue;
            }
            if (rc)
                break;
            rc = vault_record_fetch(events[i], e->grant->vault_id, e->grant->node_id, &ring, epoch,
                                    nh_vault_session_held, e->session, &e->session->io, &h.record,
                                    &h.tree);
            if (!rc)
                rc = nh_vault_snapshot_new(events[i], h.tree, &h.snapshot);
            if (!rc)
                h.event = event_from_snapshot(h.snapshot);
            if (!rc && !h.event)
                rc = VAULT_ERR_OOM;
            if (!rc)
                rc = insert(e, &h);
            head_clear(&h);
        }
    return rc ? rc : save_heads(e);
}
static int commit_resolutions(nh_vault_engine *e, const char *published_id);
int nh_vault_engine_publish_pending(nh_vault_engine *e, nh_vault_publish_fn publish, void *ctx) {
    json_t *j = json_object_get(e->session->doc, "pending");
    if (!json_is_object(j))
        return 0;
    current_head h;
    int rc = open_snapshot(e, j, &h);
    char published[65] = "";
    if (!rc)
        snprintf(published, sizeof published, "%s", h.event->id);
    int seen = 0;
    for (size_t i = 0; !rc && i < e->count; ++i)
        if (!strcmp(h.event->id, e->heads[i].event->id))
            seen = 1;
    if (!rc && !seen)
        rc = publish(ctx, h.event);
    if (!rc)
        rc = set_secret(e->session->doc, "own", j);
    if (!rc)
        rc = set_secret(e->session->doc, "local_pending", j);
    if (!rc)
        rc = insert(e, &h);
    head_clear(&h);
    if (!rc) {
        json_object_del(e->session->doc, "pending");
        rc = save_heads(e);
    }
    if (!rc && !e->base && e->config->is_owner)
        rc = nh_vault_snapshot_open(e->session, e->grant, json_object_get(e->session->doc, "own"),
                                    &e->base_record, &e->base);
    /* A conflict resolution waiting on this head is now published. */
    if (!rc)
        rc = commit_resolutions(e, published);
    return rc;
}
typedef struct {
    const nh_syncd_batch *batch;
    const char *prefix;
    nh_vault_engine *engine;
} changed_ctx;
static int file_changed_in(nh_vault_engine *e, const nh_syncd_state *state, const char *relative) {
    const nh_syncd_entry *entry = nh_syncd_state_find(state, relative);
    const char *known = entry ? nh_syncd_entry_content_hash_hex(entry) : NULL;
    if (!known || strlen(known) != 64)
        return 1;
    char expected[65];
    memcpy(expected, known, 65);
    int fd = open_relative(e->home, relative, 0);
    if (fd < 0)
        return 1;
    struct stat st;
    EVP_MD_CTX *digest = EVP_MD_CTX_new();
    int ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && digest &&
             EVP_DigestInit_ex(digest, EVP_sha256(), NULL) == 1;
    uint8_t buffer[8192], hash[32];
    unsigned n = 0;
    while (ok) {
        ssize_t count = read(fd, buffer, sizeof buffer);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            ok = 0;
            break;
        }
        if (!count)
            break;
        ok = EVP_DigestUpdate(digest, buffer, (size_t)count) == 1;
    }
    if (ok)
        ok = EVP_DigestFinal_ex(digest, hash, &n) == 1 && n == 32;
    EVP_MD_CTX_free(digest);
    close(fd);
    OPENSSL_cleanse(buffer, sizeof buffer);
    char actual[65];
    if (ok)
        porthome_hex64(hash, actual);
    return !ok || strcmp(expected, actual);
}
static int file_changed(nh_vault_engine *e, const char *relative) {
    return file_changed_in(e, e->state, relative);
}
static int changed(void *ctx, const char *rel) {
    changed_ctx *c = ctx;
    size_t prefix = strlen(c->prefix);
    if (!c->batch) {
        size_t n = prefix + strlen(rel) + 2;
        char *path = malloc(n);
        if (!path)
            return 1;
        snprintf(path, n, "%s/%s", c->prefix, rel);
        int result = file_changed(c->engine, path);
        free(path);
        return result;
    }
    for (size_t i = 0; i < nh_syncd_batch_len(c->batch); ++i) {
        const char *path = NULL;
        nh_syncd_change_kind kind;
        if (nh_syncd_batch_at(c->batch, i, &path, &kind))
            continue;
        if (!strncmp(path, c->prefix, prefix) && path[prefix] == '/' &&
            !strcmp(path + prefix + 1, rel))
            return file_changed(c->engine, path);
    }
    return 0;
}
int nh_vault_engine_capture(nh_vault_engine *e, const nh_syncd_batch *batch,
                            nh_vault_publish_fn publish, void *ctx) {
    if (json_is_object(json_object_get(e->session->doc, "pending"))) {
        int rc = nh_vault_engine_publish_pending(e, publish, ctx);
        if (rc)
            return rc;
    }
    if (!e->base || (batch && !nh_syncd_batch_len(batch)))
        return 0;
    if (!batch && e->grant && e->grant->cap != VAULT_CAP_WRITE)
        return 0;
    size_t prefix = strlen(e->config->path);
    int dirty = !batch;
    for (size_t i = 0; batch && i < nh_syncd_batch_len(batch); ++i) {
        const char *path = NULL;
        nh_syncd_change_kind kind;
        if (!nh_syncd_batch_at(batch, i, &path, &kind) && !strncmp(path, e->config->path, prefix) &&
            (!path[prefix] || path[prefix] == '/'))
            dirty = 1;
    }
    if (!dirty)
        return 0;
    if (!e->grant || e->grant->cap != VAULT_CAP_WRITE || !writer_allowed(e, e->writer))
        return VAULT_ERR_AUTH;
    int fd = open_relative(e->home, e->config->path, 1);
    if (fd < 0)
        return VAULT_ERR_IO;
    changed_ctx c = {batch, e->config->path, e};
    nh_syncd_vault_scan scan = {
        .ignore = e->ignore, .home_prefix = e->config->path, .changed = changed, .changed_ctx = &c};
    vault_tree *tree = NULL;
    int rc = nh_syncd_vault_capture(fd, e->base, &scan, &e->session->io, &tree);
    close(fd);
    if (!rc)
        rc = prepare(e, tree, 0);
    vault_tree_free(tree);
    return rc ? rc : nh_vault_engine_publish_pending(e, publish, ctx);
}
static int conflict_target(const char *path, const uint8_t writer[32], char **out) {
    char hex[65];
    porthome_hex64(writer, hex);
    const char *leaf = strrchr(path, '/');
    leaf = leaf ? leaf + 1 : path;
    size_t n = strlen(path) + 96;
    *out = malloc(n);
    if (!*out)
        return VAULT_ERR_OOM;
    if (strlen(leaf) + 65 <= 255 && strlen(path) + 86 < PORTHOME_MAX_PATH_LEN)
        snprintf(*out, n, "Portable/.conflicts/%s@%s", path, hex);
    else {
        EVP_MD_CTX *digest = EVP_MD_CTX_new();
        uint8_t hash[32];
        unsigned len = 0;
        int ok = digest && EVP_DigestInit_ex(digest, EVP_sha256(), NULL) == 1 &&
                 EVP_DigestUpdate(digest, path, strlen(path)) == 1 &&
                 EVP_DigestUpdate(digest, writer, 32) == 1 &&
                 EVP_DigestFinal_ex(digest, hash, &len) == 1 && len == 32;
        EVP_MD_CTX_free(digest);
        if (!ok) {
            free(*out);
            *out = NULL;
            return PORTHOME_ERR_CRYPTO;
        }
        porthome_hex64(hash, hex);
        snprintf(*out, n, "Portable/.conflicts/.overflow/%s", hex);
    }
    return 0;
}
static int conflicts(nh_vault_engine *e, const vault_fold_result *fold) {
    json_t *metadata = json_object_get(e->session->doc, "conflicts");
    for (size_t i = 0; i < fold->conflicts_len; ++i) {
        const vault_path_conflict *c = &fold->conflicts[i];
        current_head *source = &e->heads[c->other_record];
        const vault_tree *names = c->in_other ? source->tree : e->heads[fold->view].tree;
        char *relative = NULL, *target = NULL, *path = NULL;
        int rc = nh_syncd_vault_path_plain(names, c->path_enc, &relative);
        if (!rc) {
            size_t n = strlen(e->config->path) + strlen(relative) + 2;
            path = malloc(n);
            if (!path)
                rc = VAULT_ERR_OOM;
            else
                snprintf(path, n, "%s/%s", e->config->path, relative);
        }
        if (!rc && c->in_other)
            rc = conflict_target(path, c->writer, &target);
        if (rc) {
            free(relative);
            free(target);
            free(path);
            return rc;
        }
        char writer[65];
        porthome_hex64(c->writer, writer);
        json_t *row = NULL;
        size_t k;
        json_t *old;
        json_array_foreach(metadata, k, old) {
            const char *id = json_string_value(json_object_get(old, "head"));
            const char *p = json_string_value(json_object_get(old, "path"));
            if (id && p && !strcmp(id, source->event->id) && !strcmp(p, path))
                row = old;
        }
        if (!rc && !row) {
            row = json_pack("{s:s,s:s,s:s,s:b,s:b}", "path", path, "writer", writer, "head",
                            source->event->id, "present", c->in_other, "materialized", 0);
            if (!row || json_array_append_new(metadata, row))
                rc = VAULT_ERR_OOM;
            else if (target && (json_object_set_new(row, "destination", json_string(target)) ||
                                json_object_set_new(row, "source_path", json_string(c->path_enc))))
                rc = VAULT_ERR_OOM;
        }
        if (!rc)
            rc = retain(e, source);
        if (!rc)
            rc = nh_vault_session_save(e->session);

        if (!rc)
            rc = nh_vault_session_save(e->session);
        free(relative);
        free(target);
        free(path);
        if (rc)
            return rc;
    }
    return 0;
}
/* A failed copy remains an exact signed-snapshot task even if a newer head
 * replaces that writer's current slot before the next reconciliation. */
static int row_resolved(const json_t *row) {
    const char *st = json_string_value(json_object_get(json_object_get(row, "resolution"), "state"));
    return st && !strcmp(st, "resolved");
}
static int finish_conflicts(nh_vault_engine *e) {
    json_t *rows = json_object_get(e->session->doc, "conflicts"), *row;
    size_t i;
    json_array_foreach(rows, i, row) {
        /* A resolved row's saved copy was deliberately removed; never
         * rematerialize it. Its signed source stays retained. */
        if (!json_is_true(json_object_get(row, "present")) ||
            json_is_true(json_object_get(row, "superseded")) || row_resolved(row))
            continue;
        const char *target = json_string_value(json_object_get(row, "destination"));
        const char *id = json_string_value(json_object_get(row, "head"));
        const char *path = json_string_value(json_object_get(row, "source_path"));
        if (!target || !id)
            return VAULT_ERR_ARG;
        json_t *snapshot = NULL, *at;
        size_t k;
        json_array_foreach(json_object_get(e->session->doc, "retained"), k, at) {
            NostrEvent *event = event_from_snapshot(at);
            int match = event && event->id && !strcmp(event->id, id);
            nostr_event_free(event);
            if (match) {
                snapshot = at;
                break;
            }
        }
        if (!snapshot)
            return VAULT_ERR_INCOMPLETE;
        current_head h;
        int rc = open_snapshot(e, snapshot, &h);
        if (!rc && json_is_true(json_object_get(row, "materialized"))) {
            /* Copy completion is not namespace publication. Rebuild rows from
             * the retained authenticated tree, including after a crash before
             * snapshot.json's atomic rename; never trust or rewrite HOME here. */
            rc = json_is_true(json_object_get(row, "root_snapshot"))
                     ? nh_syncd_vault_inventory(target, h.tree, e->state, &e->session->io)
                 : path
                     ? nh_syncd_vault_inventory_one(target, h.tree, path, e->state, &e->session->io)
                     : VAULT_ERR_ARG;
            head_clear(&h);
            if (rc)
                return rc;
            continue;
        }
        if (!rc && json_is_true(json_object_get(row, "root_snapshot")))
            rc = nh_syncd_vault_materialize(e->home, target, h.tree, e->state, &e->session->io);
        else if (!rc)
            rc = path ? nh_syncd_vault_materialize_one(e->home, target, h.tree, path, e->state,
                                                       &e->session->io)
                      : VAULT_ERR_ARG;
        head_clear(&h);
        if (rc)
            return rc;
        /* One physical suffix per path/writer; older signed versions remain
         * retained and generation-bound, not falsely reported as current copies. */
        json_t *prior;
        size_t j;
        json_array_foreach(rows, j, prior) {
            const char *dest = json_string_value(json_object_get(prior, "destination"));
            if (j != i && dest && !strcmp(dest, target) &&
                json_is_true(json_object_get(prior, "materialized"))) {
                json_object_set_new(prior, "superseded", json_true());
                json_object_set_new(prior, "materialized", json_false());
            }
        }
        if (json_object_set_new(row, "materialized", json_true()))
            return VAULT_ERR_OOM;
        rc = nh_vault_session_save(e->session);
        if (rc)
            return rc;
    }
    return 0;
}
static int preserve_stale_local(nh_vault_engine *e, const current_head *local) {
    char *target = NULL, writer[65];
    int rc = conflict_target(e->config->path, e->writer, &target);
    porthome_hex64(e->writer, writer);
    json_t *array = json_object_get(e->session->doc, "conflicts"), *row = NULL, *at;
    size_t i;
    json_array_foreach(array, i, at) {
        const char *head = json_string_value(json_object_get(at, "head"));
        if (head && !strcmp(head, local->event->id) &&
            json_is_true(json_object_get(at, "root_snapshot")))
            row = at;
    }
    if (!rc && !row) {
        row = json_pack("{s:s,s:s,s:s,s:s,s:s,s:b,s:b,s:b}", "path", e->config->path, "writer",
                        writer, "head", local->event->id, "destination", target, "reason",
                        "stale-local", "root_snapshot", 1, "present", 1, "materialized", 0);
        if (!row || json_array_append_new(array, row))
            rc = VAULT_ERR_OOM;
    }
    if (!rc)
        rc = retain(e, local);
    if (!rc)
        rc = nh_vault_session_save(e->session);
    if (!rc && !json_is_true(json_object_get(row, "materialized")))
        rc = nh_syncd_vault_materialize(e->home, target, local->tree, e->state, &e->session->io);
    if (!rc)
        rc = nh_syncd_vault_inventory(target, local->tree, e->state, &e->session->io);
    if (!rc)
        json_object_set_new(row, "materialized", json_true());
    free(target);
    return rc;
}
static int forget_cold_prefix(nh_vault_engine *e) {
    size_t n = strlen(e->config->path);
    for (size_t i = 0; i < nh_syncd_state_file_count(e->state);) {
        const char *path = NULL;
        if (!nh_syncd_state_at(e->state, i, &path) || !path)
            return VAULT_ERR_ARG;
        if (nh_syncd_vault_conflict_path(path)) {
            ++i;
            continue;
        }
        if (!strncmp(path, e->config->path, n) && (!path[n] || path[n] == '/')) {
            char *copy = strdup(path);
            if (!copy)
                return VAULT_ERR_OOM;
            int rc = nh_syncd_state_delete_(e->state, copy);
            free(copy);
            if (rc)
                return rc;
        } else
            ++i;
    }
    return 0;
}
int nh_vault_engine_fold_apply(nh_vault_engine *e) {
    if (!e->grant)
        return VAULT_ERR_AGAIN;
    size_t owner = owner_index(e);
    if (owner == SIZE_MAX)
        return VAULT_ERR_INCOMPLETE;
    const vault_record *records[VAULT_MAX_HEADS];
    for (size_t i = 0; i < e->count; ++i)
        records[i] = e->heads[i].record;
    vault_fold_result *fold = NULL;
    int rc = vault_fold_checkpoint(e->grant, records, e->count, owner, &fold);
    current_head working = {0};
    json_t *local = json_object_get(e->session->doc, "local_pending");
    if (!rc && json_is_object(local))
        rc = open_snapshot(e, local, &working);
    const vault_tree *working_tree = working.tree ? working.tree : e->base;
    int stale_working = 0;
    if (!rc && working_tree) {
        const porthome_dir_node *old_root = vault_tree_node_at(working_tree, 0, NULL);
        const porthome_dir_node *next_root = vault_tree_node_at(e->heads[fold->view].tree, 0, NULL);
        stale_working = old_root->epoch != next_root->epoch ||
                        old_root->vault->grants_rev != next_root->vault->grants_rev;
    }
    /* Preflight must not publish candidate rows into the shared namespace:
     * another configured vault may commit while this one defers dirty HOME. */
    nh_syncd_state *inventory = NULL;
    if (!rc && working_tree) {
        const porthome_dir_node *root = vault_tree_node_at(working_tree, 0, NULL);
        if (nh_syncd_state_new("", "", "", root->node_id, &inventory))
            rc = VAULT_ERR_OOM;
        if (!rc)
            rc =
                nh_syncd_vault_inventory(e->config->path, working_tree, inventory, &e->session->io);
    }
    for (size_t i = 0; !rc && inventory && i < nh_syncd_state_file_count(inventory); ++i) {
        const char *path = NULL;
        const nh_syncd_entry *entry = nh_syncd_state_at(inventory, i, &path);
        if (entry && nh_syncd_entry_kind(entry) == NH_SYNCD_KIND_FILE && path &&
            file_changed_in(e, inventory, path)) {
            fprintf(stderr, "syncd: vault dirty preflight (pending=%d stale-policy=%d)\n", working.record != NULL, stale_working);
            rc = stale_working ? VAULT_ERR_INCOMPLETE : VAULT_ERR_AGAIN;
        }
    }
    nh_syncd_state_free(inventory);
    if (!rc && working_tree)
        rc = nh_syncd_vault_inventory(e->config->path, working_tree, e->state, &e->session->io);
    if (!rc)
        rc = conflicts(e, fold);
    if (!rc)
        rc = finish_conflicts(e);
    if (!rc && working.record &&
        vault_record_head(working.record)->grants_rev <
            vault_record_head(e->heads[fold->view].record)->grants_rev)
        rc = preserve_stale_local(e, &working);
    if (!rc && !e->base && !e->config->is_owner)
        rc = forget_cold_prefix(e);
    const porthome_head *next = !rc ? vault_record_head(e->heads[fold->view].record) : NULL;
    const porthome_head *prior = e->base_record ? vault_record_head(e->base_record) : NULL;
    const porthome_head *candidate = working.record ? vault_record_head(working.record) : NULL;
    int changed_view =
        next && (!prior || prior->epoch != next->epoch ||
                 memcmp(prior->root_manifest, next->root_manifest, 32) ||
                 (candidate && (candidate->epoch != next->epoch ||
                                memcmp(candidate->root_manifest, next->root_manifest, 32))));
    if (!rc && changed_view)
        rc = nh_syncd_vault_materialize(e->home, e->config->path, e->heads[fold->view].tree,
                                        e->state, &e->session->io);
    if (!rc)
        rc = nh_syncd_vault_inventory(e->config->path, e->heads[fold->view].tree, e->state,
                                      &e->session->io);
    if (!rc)
        rc = set_secret(e->session->doc, "view", e->heads[fold->view].snapshot);
    if (!rc) {
        porthome_head coverage = *vault_record_head(e->heads[fold->view].record);
        coverage.parents = NULL;
        coverage.parents_len = 0;
        const vault_record *incorporated[VAULT_MAX_HEADS];
        size_t count = 0;
        for (size_t i = 0; i < e->count; ++i)
            if (fold->dispositions[i] == VAULT_HEAD_ACCEPTED)
                incorporated[count++] = e->heads[i].record;
        rc = vault_head_cover(&coverage, incorporated, count);
        uint8_t *wire = NULL;
        size_t n = 0;
        if (!rc)
            rc = porthome_head_encode(&coverage, &wire, &n);
        if (!rc && json_object_set_new(e->session->doc, "coverage", json_stringn((char *)wire, n)))
            rc = VAULT_ERR_OOM;
        free(wire);
        porthome_head_dispose(&coverage);
        if (!rc)
            json_object_del(e->session->doc, "local_pending");
    }
    if (!rc)
        rc = nh_vault_session_save(e->session);
    if (!rc) {
        vault_record *record = NULL;
        vault_tree *tree = NULL;
        rc = nh_vault_snapshot_open(e->session, e->grant, e->heads[fold->view].snapshot, &record,
                                    &tree);
        if (!rc) {
            vault_record_free(e->base_record);
            vault_tree_free(e->base);
            e->base_record = record;
            e->base = tree;
        }
    }
    head_clear(&working);
    vault_fold_result_free(fold);
    return rc;
}
int nh_vault_engine_checkpoint(nh_vault_engine *e, nh_vault_publish_fn publish, void *ctx) {
    if (!e->config->is_owner || !e->base)
        return 0;
    const vault_record *records[VAULT_MAX_HEADS];
    for (size_t i = 0; i < e->count; ++i)
        records[i] = e->heads[i].record;
    size_t owner = owner_index(e);
    if (owner == SIZE_MAX)
        return VAULT_ERR_INCOMPLETE;
    vault_fold_result *fold = NULL;
    int rc = vault_fold_checkpoint(e->grant, records, e->count, owner, &fold);
    int needed = !rc && (fold->frontier_len != 1 ||
                         memcmp(e->heads[fold->frontier[0]].writer, e->writer, 32));
    if (!rc && needed)
        rc = prepare(e, e->base, 1);
    if (!rc && needed)
        rc = nh_vault_engine_publish_pending(e, publish, ctx);
    if (!rc && !needed)
        rc = set_secret(e->session->doc, "own", e->heads[owner].snapshot);
    if (!rc)
        rc = nh_vault_session_save(e->session);
    vault_fold_result_free(fold);
    return rc;
}
int nh_vault_engine_retention(nh_vault_engine *e, uint64_t budget, nh_vault_publish_fn publish,
                              void *ctx, uint64_t *reserved) {
    *reserved = 0;
    if (!e->base || !budget || !e->grant || e->grant->cap != VAULT_CAP_WRITE ||
        !writer_allowed(e, e->writer))
        return 0;
    int rc = nh_vault_engine_publish_pending(e, publish, ctx);
    vault_tree *tree = NULL;
    vault_record *record = NULL;
    if (!rc)
        rc = nh_vault_snapshot_open(e->session, e->grant, json_object_get(e->session->doc, "view"),
                                    &record, &tree);
    vault_tree_reseal_result *result = NULL;
    if (!rc)
        rc = vault_tree_reseal(tree, 0, budget, &e->session->io, &result);
    int progress = result && result->completed;
    if (result)
        *reserved = result->reserved;
    if (rc == VAULT_ERR_BUDGET)
        rc = 0;
    if (progress) {
        int saved = prepare(e, tree, 0);
        if (!saved)
            saved = nh_vault_engine_publish_pending(e, publish, ctx);
        if (!rc)
            rc = saved;
    }
    vault_tree_reseal_result_free(result);
    vault_record_free(record);
    vault_tree_free(tree);
    return rc;
}
static int prefetch_head(nh_vault_engine *e, const current_head *h, nh_vault_prefetch_fn ensure,
                         void *ctx, int check) {
    for (size_t i = 0; i < vault_tree_count(h->tree); ++i) {
        porthome_ring_t ring;
        const porthome_dir_node *node = vault_tree_node_at(h->tree, i, &ring);
        const uint8_t *address = NULL, *key = NULL;
        for (size_t k = 0; k < vault_record_manifest_count(h->record); ++k) {
            const uint8_t *at = NULL;
            const porthome_dir_node *n = vault_record_manifest_at(h->record, k, &at);
            if (!memcmp(n->node_id, node->node_id, 32)) {
                address = at;
                break;
            }
        }
        for (size_t k = 0; k < ring.count; ++k)
            if (ring.entries[k].epoch == node->epoch)
                key = ring.entries[k].node_key;
        if (!address || !key)
            return VAULT_ERR_EPOCH;
        uint8_t *wire = NULL, hash[32];
        size_t len = 0;
        int rc = porthome_dir_node_seal(node, key, &wire, &len, hash);
        if (!rc && memcmp(hash, address, 32))
            rc = PORTHOME_ERR_HASH_MISMATCH;
        if (!rc)
            rc = ensure(ctx, address, len, wire);
        free(wire);
        if (rc)
            return rc;
        for (size_t k = 0; k < node->entries_len; ++k)
            if (node->entries[k].kind == PORTHOME_KIND_FILE) {
                const porthome_node_entry *entry = &node->entries[k];
                for (size_t c = 0; c < entry->chunks_len; ++c) {
                    rc = ensure(ctx, entry->chunks[c].sha256, entry->chunks[c].size_sealed, NULL);
                    if (rc)
                        return rc;
                }
            }
    }
    if (check) {
        vault_tree_check_result result;
        return vault_tree_check(h->tree, &e->session->io, &result);
    }
    return 0;
}
int nh_vault_engine_prefetch(nh_vault_engine *e, nh_vault_prefetch_fn ensure, void *ctx,
                             int check) {
    if (!e->grant)
        return VAULT_ERR_AGAIN;
    size_t owner = owner_index(e);
    if (owner == SIZE_MAX)
        return VAULT_ERR_INCOMPLETE;
    const vault_record *records[VAULT_MAX_HEADS];
    for (size_t i = 0; i < e->count; ++i)
        records[i] = e->heads[i].record;
    vault_fold_result *fold = NULL;
    int rc = vault_fold_checkpoint(e->grant, records, e->count, owner, &fold);
    if (!rc)
        rc = prefetch_head(e, &e->heads[fold->view], ensure, ctx, check);
    for (size_t i = 0; !rc && i < fold->conflicts_len; ++i)
        rc = prefetch_head(e, &e->heads[fold->conflicts[i].other_record], ensure, ctx, check);
    const char *extra_fields[] = {"view", "local_pending"};
    for (size_t k = 0; !rc && k < 2; ++k) {
        json_t *snapshot = json_object_get(e->session->doc, extra_fields[k]);
        if (json_is_object(snapshot)) {
            current_head held;
            rc = open_snapshot(e, snapshot, &held);
            if (!rc)
                rc = prefetch_head(e, &held, ensure, ctx, check);
            head_clear(&held);
        }
    }
    json_t *retained = json_object_get(e->session->doc, "retained");
    json_t *metadata = json_object_get(e->session->doc, "conflicts"), *snapshot;
    size_t i;
    json_array_foreach(retained, i, snapshot) {
        if (rc)
            break;
        NostrEvent *event = event_from_snapshot(snapshot);
        if (!event) {
            rc = VAULT_ERR_ARG;
            break;
        }
        const char *id = event->id;
        int needed = 0;
        size_t k;
        json_t *row;
        json_array_foreach(metadata, k, row) {
            const char *head = json_string_value(json_object_get(row, "head"));
            if (head && !strcmp(head, id))
                needed = 1;
        }
        nostr_event_free(event);
        if (needed) {
            current_head h;
            rc = open_snapshot(e, snapshot, &h);
            if (!rc)
                rc = prefetch_head(e, &h, ensure, ctx, check);
            head_clear(&h);
        }
    }
    vault_fold_result_free(fold);
    return rc;
}
/* ── Conflict resolution (nostrc-h10m.6) ──
 *
 * Identity: sha256("nh-vault-conflict-v1" 0 vault-hex 0 head-id 0 path),
 * NUL-separated so no path can alias another field. A resolution is an
 * engine transaction: persist intent on the row, apply the chosen version to
 * HOME from the AUTHENTICATED retained snapshot (never from the editable
 * Portable/.conflicts copy), capture, sign a forced successor head (even for
 * keep-current, so the conflicting head is covered network-wide) and publish
 * it through the normal pending journal. Only after a relay accepts it does
 * the row become resolved and its saved copy go away (commit_resolutions,
 * also reached from a later publish_pending when publication was deferred). */
static void vault_conflict_id(const nh_vault_engine *e, const char *head, const char *path,
                              char out[65]) {
    char vault[65];
    porthome_hex64(e->config->vault_id, vault);
    uint8_t hash[32];
    unsigned len = 0;
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    static const char tag[] = "nh-vault-conflict-v1";
    int ok = md && EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1 &&
             EVP_DigestUpdate(md, tag, sizeof tag) == 1 &&
             EVP_DigestUpdate(md, vault, strlen(vault) + 1) == 1 &&
             EVP_DigestUpdate(md, head, strlen(head) + 1) == 1 &&
             EVP_DigestUpdate(md, path, strlen(path) + 1) == 1 &&
             EVP_DigestFinal_ex(md, hash, &len) == 1 && len == 32;
    EVP_MD_CTX_free(md);
    if (ok)
        porthome_hex64(hash, out);
    else
        out[0] = '\0';
}
static json_t *find_conflict(const nh_vault_engine *e, const char *id) {
    size_t i;
    json_t *row;
    json_array_foreach(json_object_get(e->session->doc, "conflicts"), i, row) {
        const char *head = json_string_value(json_object_get(row, "head"));
        const char *path = json_string_value(json_object_get(row, "path"));
        char rid[65];
        if (!head || !path)
            continue;
        vault_conflict_id(e, head, path, rid);
        if (rid[0] && !strcmp(rid, id))
            return row;
    }
    return NULL;
}
int nh_vault_engine_has_conflict(const nh_vault_engine *e, const char *id) {
    return e && id && find_conflict(e, id) != NULL;
}
static int can_write(const nh_vault_engine *e) {
    return e->grant && e->grant->cap == VAULT_CAP_WRITE && writer_allowed(e, e->writer);
}
/* Bitmask of (1 << nh_conflict_choice) the row supports here. */
static unsigned row_choices(const nh_vault_engine *e, const json_t *row) {
    unsigned keep = 1u << NH_CONFLICT_KEEP_CURRENT;
    if (json_is_true(json_object_get(row, "root_snapshot")) || !can_write(e))
        return keep; /* whole-folder stale copy, or read-only: dismiss only */
    const char *path = json_string_value(json_object_get(row, "path"));
    int current = path && nh_syncd_state_find(e->state, path) != NULL;
    int saved = json_is_true(json_object_get(row, "present"));
    unsigned mask = keep;
    if (saved || current)
        mask |= 1u << NH_CONFLICT_USE_SAVED;
    if (saved && current)
        mask |= 1u << NH_CONFLICT_KEEP_BOTH;
    return mask;
}
/* Snapshot of the signed head named by `id`, from retained or current heads. */
static int open_head(nh_vault_engine *e, const char *id, current_head *h) {
    size_t k;
    json_t *at;
    json_array_foreach(json_object_get(e->session->doc, "retained"), k, at) {
        NostrEvent *event = event_from_snapshot(at);
        int match = event && event->id && !strcmp(event->id, id);
        nostr_event_free(event);
        if (match)
            return open_snapshot(e, at, h);
    }
    for (size_t i = 0; i < e->count; ++i)
        if (!strcmp(e->heads[i].event->id, id))
            return open_snapshot(e, e->heads[i].snapshot, h);
    return VAULT_ERR_INCOMPLETE;
}
/* Drop snapshot rows at and under `path` so capture re-reads those files
 * instead of reusing a descriptor whose size and mtime happen to match. */
static int forget_rows(nh_syncd_state *state, const char *path) {
    size_t n = strlen(path);
    for (size_t i = 0; i < nh_syncd_state_file_count(state);) {
        const char *p = NULL;
        if (!nh_syncd_state_at(state, i, &p) || !p)
            return VAULT_ERR_ARG;
        if (!strncmp(p, path, n) && (!p[n] || p[n] == '/')) {
            char *copy = strdup(p);
            int rc = copy ? nh_syncd_state_delete_(state, copy) : VAULT_ERR_OOM;
            free(copy);
            if (rc)
                return rc;
        } else
            ++i;
    }
    return 0;
}
static int commit_row(nh_vault_engine *e, json_t *row) {
    json_t *res = json_object_get(row, "resolution");
    const char *dest = json_string_value(json_object_get(row, "destination"));
    int rc = 0;
    if (dest)
        rc = nh_syncd_vault_remove_one(e->home, dest, e->state);
    /* FUSE stops serving the saved copy from the next local generation on. */
    if (!rc && (json_object_set_new(res, "state", json_string("resolved")) ||
                json_object_set_new(res, "at", json_integer((json_int_t)time(NULL))) ||
                json_object_set_new(row, "resolved_generation",
                                    json_integer((json_int_t)nh_syncd_state_get_local_generation(e->state) + 1)) ||
                json_object_set_new(row, "materialized", json_false())))
        rc = VAULT_ERR_OOM;
    return rc;
}
static int commit_resolutions(nh_vault_engine *e, const char *published_id) {
    if (!published_id || !*published_id)
        return 0;
    size_t i;
    json_t *row;
    int rc = 0, changed = 0;
    json_array_foreach(json_object_get(e->session->doc, "conflicts"), i, row) {
        json_t *res = json_object_get(row, "resolution");
        const char *st = json_string_value(json_object_get(res, "state"));
        const char *head = json_string_value(json_object_get(res, "head"));
        if (rc || !st || strcmp(st, "pending") || !head || strcmp(head, published_id))
            continue;
        rc = commit_row(e, row);
        changed = 1;
    }
    return !rc && changed ? nh_vault_session_save(e->session) : rc;
}
static int apply_choice(nh_vault_engine *e, const json_t *row, nh_conflict_choice choice,
                        const char *sibling) {
    const char *path = json_string_value(json_object_get(row, "path"));
    const char *head = json_string_value(json_object_get(row, "head"));
    const char *source = json_string_value(json_object_get(row, "source_path"));
    int present = json_is_true(json_object_get(row, "present"));
    if (choice == NH_CONFLICT_KEEP_CURRENT)
        return 0;
    if (choice == NH_CONFLICT_USE_SAVED && !present)
        return nh_syncd_vault_remove_one(e->home, path, e->state);
    const char *target = choice == NH_CONFLICT_KEEP_BOTH ? sibling : path;
    if (!target || !head || !source)
        return VAULT_ERR_ARG;
    current_head h;
    int rc = open_head(e, head, &h);
    if (!rc)
        rc = nh_syncd_vault_materialize_one(e->home, target, h.tree, source, e->state,
                                            &e->session->io);
    head_clear(&h);
    return rc ? rc : forget_rows(e->state, target);
}
static const char *pending_id(const nh_vault_engine *e, char out[65]) {
    NostrEvent *event = event_from_snapshot(json_object_get(e->session->doc, "pending"));
    out[0] = '\0';
    if (event && event->id)
        snprintf(out, 65, "%s", event->id);
    nostr_event_free(event);
    return out;
}
int nh_vault_engine_resolve(nh_vault_engine *e, const char *id, nh_conflict_choice choice,
                            const char *expect, const char *request, nh_vault_publish_fn publish,
                            void *ctx, const char **why) {
    static const char *unused;
    if (!why)
        why = &unused;
    *why = "invalid-request";
    if (!e || !id || !request || !nh_conflict_choice_slug(choice)[0])
        return VAULT_ERR_ARG;
    if (!e->grant || !e->base) {
        *why = "busy";
        return VAULT_ERR_AGAIN;
    }
    json_t *row = find_conflict(e, id);
    if (!row || json_is_true(json_object_get(row, "superseded"))) {
        *why = "not-found";
        return VAULT_ERR_ARG;
    }
    json_t *res = json_object_get(row, "resolution");
    const char *sibling = NULL;
    int fresh = 0, rc = 0;
    if (res) {
        const char *req = json_string_value(json_object_get(res, "request"));
        if (!req || strcmp(req, request)) {
            *why = row_resolved(row) ? "already-resolved" : "busy";
            return VAULT_ERR_ARG;
        }
        if (row_resolved(row))
            return 0; /* duplicate delivery of a finished request */
        if (nh_conflict_choice_parse(json_string_value(json_object_get(res, "choice")), &choice)) {
            *why = "io";
            return VAULT_ERR_ARG;
        }
        sibling = json_string_value(json_object_get(res, "sibling"));
    } else {
        if (!(row_choices(e, row) & (1u << choice))) {
            *why = "unsupported-choice";
            return VAULT_ERR_ARG;
        }
        const char *path = json_string_value(json_object_get(row, "path"));
        char token[65];
        nh_syncd_state_version_token(e->state, path, token);
        if (expect && *expect && strcmp(expect, token)) {
            *why = "stale";
            return VAULT_ERR_ARG;
        }
        /* An unrelated signed head must go out first; never stack ours on it. */
        if (can_write(e) && json_is_object(json_object_get(e->session->doc, "pending")) &&
            (rc = nh_vault_engine_publish_pending(e, publish, ctx))) {
            *why = "offline";
            return rc;
        }
        char *sib = NULL;
        if (choice == NH_CONFLICT_KEEP_BOTH) {
            const char *writer = json_string_value(json_object_get(row, "writer"));
            char label[24];
            snprintf(label, sizeof label, "from %.8s", writer ? writer : "other");
            rc = nh_conflict_sibling_path(e->home, e->state, path, label, &sib);
            if (rc) {
                *why = "io";
                return rc;
            }
        }
        res = json_pack("{s:s,s:s,s:s,s:I}", "request", request, "choice",
                        nh_conflict_choice_slug(choice), "state", "pending", "at",
                        (json_int_t)time(NULL));
        int bad = !res || (sib && json_object_set_new(res, "sibling", json_string(sib)));
        free(sib);
        if (bad || json_object_set_new(row, "resolution", res)) {
            if (bad)
                json_decref(res);
            *why = "io";
            return VAULT_ERR_OOM;
        }
        sibling = json_string_value(json_object_get(res, "sibling"));
        fresh = 1;
        if ((rc = nh_vault_session_save(e->session))) {
            json_object_del(row, "resolution");
            *why = "io";
            return rc;
        }
    }

    if (!can_write(e)) {
        /* Read-only member: a local dismissal of keep-current. There is
         * nothing this account may publish. */
        rc = commit_row(e, row);
        if (!rc)
            rc = nh_vault_session_save(e->session);
        if (rc)
            *why = "io";
        return rc;
    }

    char published[65] = "", queued[65];
    const char *planned = json_string_value(json_object_get(res, "head"));
    const char *own = NULL;
    NostrEvent *own_event = event_from_snapshot(json_object_get(e->session->doc, "own"));
    if (own_event && own_event->id) {
        snprintf(published, sizeof published, "%s", own_event->id);
        own = published;
    }
    nostr_event_free(own_event);
    if (planned && own && !strcmp(planned, own)) {
        /* Published before a crash, not yet committed. */
        rc = commit_resolutions(e, planned);
        if (rc)
            *why = "io";
        return rc;
    }
    if (!planned || strcmp(planned, pending_id(e, queued))) {
        rc = apply_choice(e, row, choice, sibling);
        int fd = rc ? -1 : open_relative(e->home, e->config->path, 1);
        if (!rc && fd < 0)
            rc = VAULT_ERR_IO;
        vault_tree *tree = NULL;
        if (!rc) {
            changed_ctx c = {NULL, e->config->path, e};
            nh_syncd_vault_scan scan = {.ignore = e->ignore, .home_prefix = e->config->path,
                                        .changed = changed, .changed_ctx = &c};
            rc = nh_syncd_vault_capture(fd, e->base, &scan, &e->session->io, &tree);
        }
        if (fd >= 0)
            close(fd);
        if (!rc)
            rc = prepare(e, tree, 1); /* forced: keep-current still needs a head */
        vault_tree_free(tree);
        if (!rc && (json_object_set_new(res, "head", json_string(pending_id(e, queued))) ||
                    (rc = nh_vault_session_save(e->session))))
            rc = rc ? rc : VAULT_ERR_OOM;
        if (rc) {
            if (fresh) {
                json_object_del(row, "resolution");
                (void)nh_vault_session_save(e->session);
            }
            *why = rc == VAULT_ERR_EPOCH || rc == VAULT_ERR_INCOMPLETE ? "busy" : "io";
            return rc;
        }
    }
    rc = nh_vault_engine_publish_pending(e, publish, ctx);
    if (rc) {
        *why = "offline";
        return NH_CONFLICT_QUEUED; /* signed + journalled; commits on a later publish */
    }
    return 0;
}
int nh_vault_engine_project_conflicts(const nh_vault_engine *e, json_t *out) {
    char vault[65];
    porthome_hex64(e->config->vault_id, vault);
    char me[65];
    porthome_hex64(e->writer, me);
    size_t i;
    json_t *row;
    json_array_foreach(json_object_get(e->session->doc, "conflicts"), i, row) {
        if (json_is_true(json_object_get(row, "superseded")) || row_resolved(row))
            continue;
        const char *path = json_string_value(json_object_get(row, "path"));
        const char *head = json_string_value(json_object_get(row, "head"));
        const char *writer = json_string_value(json_object_get(row, "writer"));
        if (!path || !head || !writer)
            continue;
        char id[65], token[65];
        vault_conflict_id(e, head, path, id);
        nh_syncd_state_version_token(e->state, path, token);
        const char *reason = json_string_value(json_object_get(row, "reason"));
        const char *mine = !strcmp(writer, me) || (reason && !strcmp(reason, "stale-local"))
                               ? "saved"
                           : e->config->is_owner ? "current"
                                                 : NULL;
        json_t *choices = json_array();
        unsigned mask = row_choices(e, row);
        for (int c = NH_CONFLICT_KEEP_CURRENT; choices && c <= NH_CONFLICT_KEEP_BOTH; ++c)
            if ((mask & (1u << c)) &&
                json_array_append_new(choices, json_string(nh_conflict_choice_slug(c)))) {
                json_decref(choices);
                choices = NULL;
            }
        const char *dest = json_string_value(json_object_get(row, "destination"));
        json_t *p = json_pack("{s:s,s:s,s:s,s:s,s:s,s:s,s:b,s:b,s:b,s:s,s:o,s:s,s:b}", "source",
                              "vault", "id", id, "vault_id", vault, "path", path, "writer", writer,
                              "head", head, "present", json_is_true(json_object_get(row, "present")),
                              "current_present", strcmp(token, "absent") != 0, "materialized",
                              json_is_true(json_object_get(row, "materialized")), "state",
                              json_object_get(row, "resolution") ? "pending" : "active", "choices",
                              choices, "expect", token, "overflow",
                              dest && !strncmp(dest, "Portable/.conflicts/.overflow/", 30));
        int bad = !p;
        static const char *const copy[] = {"destination", "generation", "reason", "root_snapshot"};
        for (size_t k = 0; !bad && k < sizeof copy / sizeof copy[0]; ++k) {
            json_t *v = json_object_get(row, copy[k]);
            if (v)
                bad = json_object_set(p, copy[k], v);
        }
        if (!bad)
            bad = json_object_set_new(p, "mine", mine ? json_string(mine) : json_null());
        if (bad || json_array_append_new(out, p)) {
            if (bad)
                json_decref(p);
            return VAULT_ERR_OOM;
        }
    }
    return 0;
}
const uint8_t *nh_vault_engine_writer(const nh_vault_engine *e) { return e->writer; }
const vault_grant *nh_vault_engine_grant(const nh_vault_engine *e) { return e->grant; }
const vault_tree *nh_vault_engine_view(const nh_vault_engine *e) { return e->base; }
const json_t *nh_vault_engine_conflicts(const nh_vault_engine *e) {
    return json_object_get(e->session->doc, "conflicts");
}
void nh_vault_engine_close(nh_vault_engine *e) {
    if (!e)
        return;
    for (size_t i = 0; i < e->count; ++i)
        head_clear(&e->heads[i]);
    vault_record_free(e->base_record);
    vault_tree_free(e->base);
    vault_grant_free(e->grant);
    OPENSSL_cleanse(e, sizeof *e);
    free(e);
}
