/* SPDX-License-Identifier: MIT — sealed Git ODB/ref-state composition (§3.5). */
#include "hanami-sealed.h"
#include "hanami/hanami-odb-backend.h"
#include "hanami/hanami-config.h"
#include "nostr-event.h"
#include <git2/sys/odb_backend.h>
#include <git2/sys/transport.h>
#include <jansson.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STATE_MAX_BYTES (16u * 1024u * 1024u)
#define STATE_MAX_ROWS 65536u

static void secret_free(void *p, size_t n)
{
    if (p) OPENSSL_cleanse(p, n);
    free(p);
}

void hanami_sealed_retain(hanami_sealed *s) { if (s) s->refs++; }
void hanami_sealed_release(hanami_sealed *s)
{
    if (!s || --s->refs) return;
    vault_ring_dispose(&s->keys);
    hanami_index_close(s->index);
    blossom_client_free(s->client);
    hanami_nostr_ctx_free(s->nostr);
    free(s->endpoint);
    if (s->relays) {
        for (size_t i = 0; s->relays[i]; i++) free(s->relays[i]);
        free(s->relays);
    }
    nip34_repo_state_free(s->state);
    if (s->head) { porthome_head_dispose(s->head); free(s->head); }
    free(s);
}

static const porthome_ring_entry_t *latest(hanami_sealed *s)
{
    return &s->ring.entries[s->ring.count - 1];
}

static bool hex_valid(const char *s, size_t n)
{
    if (!s || strlen(s) != n) return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    return true;
}

static bool oid_valid(const char *s)
{
    /* This build's libgit2 uses classic SHA-1. Never truncate a SHA-256 OID. */
    return hex_valid(s, GIT_OID_HEXSZ);
}

static bool ref_valid(const char *name, const char *target)
{
    int valid = 0;
    if (!name || !target || strlen(name) > 1024 ||
        (strcmp(name, "HEAD") &&
         (git_reference_name_is_valid(&valid, name) < 0 || !valid))) return false;
    if (strncmp(target, "ref: ", 5) == 0) {
        valid = 0;
        return git_reference_name_is_valid(&valid, target + 5) == 0 && valid;
    }
    return oid_valid(target);
}

/* The shared index's three fields plus Git-only type/size metadata. Exact
 * authenticated lengths are mandatory for PCS v2's zero-padded chunks. */
typedef struct { hanami_sealed *s; json_t *rows; } row_encoder;
static porthome_index_status encode_row(void *ctx, const porthome_index_entry *row)
{
    row_encoder *e = ctx;
    hanami_index_entry_t git;
    if (hanami_index_get_by_oid(e->s->index, row->git_oid, &git) != HANAMI_OK ||
        !row->chunk_key_id || git.size > INT64_MAX) return PORTHOME_INDEX_ERR_STORAGE;
    char address[65];
    porthome_hex64(row->sealed_sha256, address);
    json_t *value = json_pack("{s:s,s:s,s:I,s:i,s:I}",
        "git_oid", row->git_oid, "sealed_sha256", address,
        "chunk_key_id", (json_int_t)row->chunk_key_id,
        "type", (int)git.type, "size", (json_int_t)git.size);
    if (!value || json_array_append_new(e->rows, value) < 0) return PORTHOME_INDEX_ERR_OOM;
    return PORTHOME_INDEX_OK;
}

static hanami_error_t encode_state(hanami_sealed *s, const nip34_ref_t *refs,
    size_t count, const char *head, uint8_t **out, size_t *len)
{
    *out = NULL; *len = 0;
    if (count > STATE_MAX_ROWS || hanami_index_count(s->index) > STATE_MAX_ROWS)
        return HANAMI_ERR_INDEX;
    json_t *root = json_object(), *r = json_array(), *rows = json_array();
    if (!root || !r || !rows) goto oom;
    for (size_t i = 0; i < count + (head ? 1u : 0u); i++) {
        const char *name = i < count ? refs[i].refname : "HEAD";
        const char *target = i < count ? refs[i].target : head;
        if (!ref_valid(name, target)) goto invalid;
        json_t *value = json_pack("{s:s,s:s}", "refname", name, "target_oid", target);
        if (!value || json_array_append_new(r, value) < 0) goto oom;
    }
    row_encoder e = {s, rows};
    if (porthome_index_iterate(hanami_index_porthome(s->index), encode_row, &e) != PORTHOME_INDEX_OK)
        goto invalid;
    char prev[65];
    json_t *previous = json_null();
    if (s->head->has_ref_state) {
        porthome_hex64(s->head->ref_state, prev);
        previous = json_string(prev);
    }
    if (json_object_set(root, "refs", r) < 0 || json_object_set(root, "index", rows) < 0 ||
        json_object_set_new(root, "prev", previous) < 0 ||
        json_object_set_new(root, "public", json_boolean(s->lifted)) < 0) goto oom;
    char *text = json_dumps(root, JSON_COMPACT | JSON_SORT_KEYS);
    if (!text) goto oom;
    *len = strlen(text);
    if (*len > STATE_MAX_BYTES) { secret_free(text, *len); goto invalid; }
    *out = (uint8_t *)text;
    json_decref(root); json_decref(r); json_decref(rows);
    return HANAMI_OK;
invalid:
    json_decref(root); json_decref(r); json_decref(rows);
    return HANAMI_ERR_INDEX;
oom:
    json_decref(root); json_decref(r); json_decref(rows);
    return HANAMI_ERR_NOMEM;
}

/* Decode into fresh state/index; no partial import on malformed snapshots. */
static hanami_error_t decode_state(const uint8_t *plain, size_t len,
    nip34_repo_state_t **out, hanami_index_t **index_out, bool *lifted)
{
    *out = NULL; *index_out = NULL;
    if (len > STATE_MAX_BYTES) return HANAMI_ERR_INDEX;
    json_t *root = json_loadb((const char *)plain, len, JSON_REJECT_DUPLICATES, NULL);
    json_t *refs = json_object_get(root, "refs"), *rows = json_object_get(root, "index");
    json_t *prev = json_object_get(root, "prev"), *pub = json_object_get(root, "public");
    if (!json_is_object(root) || json_object_size(root) != 4 ||
        !json_is_array(refs) || !json_is_array(rows) || !json_is_boolean(pub) ||
        !(json_is_null(prev) || hex_valid(json_string_value(prev), 64)) ||
        json_array_size(refs) > STATE_MAX_ROWS || json_array_size(rows) > STATE_MAX_ROWS) {
        json_decref(root); return HANAMI_ERR_INDEX;
    }
    nip34_repo_state_t *state = calloc(1, sizeof(*state));
    hanami_index_t *index = NULL;
    if (!state || hanami_index_open(&index, ":memory:", NULL) != HANAMI_OK) goto fail;
    state->refs = calloc(json_array_size(refs) + 1, sizeof(*state->refs));
    if (!state->refs) goto fail;
    size_t i; json_t *value;
    json_array_foreach(refs, i, value) {
        json_t *jn = json_object_get(value, "refname"), *jt = json_object_get(value, "target_oid");
        const char *name = json_string_value(jn), *target = json_string_value(jt);
        if (!json_is_object(value) || json_object_size(value) != 2 ||
            !ref_valid(name, target) || json_string_length(jn) != strlen(name) ||
            json_string_length(jt) != strlen(target)) goto fail;
        if (!strcmp(name, "HEAD")) {
            if (state->head) goto fail;
            state->head = strdup(target);
            if (!state->head) goto fail;
        } else {
            for (size_t n = 0; n < state->ref_count; n++)
                if (!strcmp(state->refs[n].refname, name)) goto fail;
            nip34_ref_t *ref = &state->refs[state->ref_count++];
            ref->refname = strdup(name); ref->target = strdup(target);
            if (!ref->refname || !ref->target) goto fail;
        }
    }
    json_array_foreach(rows, i, value) {
        const char *oid = json_string_value(json_object_get(value, "git_oid"));
        const char *sha = json_string_value(json_object_get(value, "sealed_sha256"));
        json_t *key = json_object_get(value, "chunk_key_id");
        json_t *type = json_object_get(value, "type"), *size = json_object_get(value, "size");
        if (!json_is_object(value) || json_object_size(value) != 5 || !oid_valid(oid) ||
            !hex_valid(sha, 64) ||
            json_string_length(json_object_get(value, "git_oid")) != strlen(oid) ||
            json_string_length(json_object_get(value, "sealed_sha256")) != strlen(sha) ||
            !json_is_integer(key) || json_integer_value(key) <= 0 ||
            json_integer_value(key) > UINT32_MAX || !json_is_integer(size) ||
            json_integer_value(size) < 0 || (uint64_t)json_integer_value(size) > SIZE_MAX ||
            !json_is_integer(type) || json_integer_value(type) < GIT_OBJECT_COMMIT ||
            json_integer_value(type) > GIT_OBJECT_TAG || hanami_index_exists(index, oid)) goto fail;
        hanami_index_entry_t entry = {0};
        strcpy(entry.git_oid, oid); strcpy(entry.blossom_hash, sha);
        entry.chunk_key_id = (uint32_t)json_integer_value(key);
        entry.type = (git_object_t)json_integer_value(type); entry.size = (size_t)json_integer_value(size);
        if (hanami_index_put(index, &entry) != HANAMI_OK) goto fail;
    }
    for (size_t n = 0; n < state->ref_count; n++)
        if (oid_valid(state->refs[n].target) && !hanami_index_exists(index, state->refs[n].target)) goto fail;
    if (oid_valid(state->head) && !hanami_index_exists(index, state->head)) goto fail;
    *lifted = json_is_true(pub); *out = state; *index_out = index;
    json_decref(root);
    return HANAMI_OK;
fail:
    nip34_repo_state_free(state); hanami_index_close(index); json_decref(root);
    return HANAMI_ERR_INDEX;
}

static hanami_error_t read_snapshot(hanami_sealed *s, const porthome_head *head,
    nip34_repo_state_t **state, hanami_index_t **index, bool *lifted)
{
    if (!head->has_ref_state) return HANAMI_ERR_INDEX;
    char address[65]; porthome_hex64(head->ref_state, address);
    uint8_t *blob = NULL, *plain = NULL; size_t n = 0, len = 0;
    hanami_error_t err = blossom_client_get_content(s->client, address, &blob, &n);
    if (err != HANAMI_OK) return err;
    int rc = porthome_decrypt_manifest_v2(&s->ring, head->epoch, blob, n, &plain, &len);
    free(blob);
    if (rc != PORTHOME_OK) return HANAMI_ERR_AUTH;
    err = decode_state(plain, len, state, index, lifted);
    secret_free(plain, len);
    return err;
}

static hanami_error_t open_root(hanami_sealed *s, NostrEvent *event,
    porthome_head **out_head, porthome_dir_node **out_root)
{
    *out_head = NULL; *out_root = NULL;
    porthome_head *head = NULL;
    uint8_t id[32]; porthome_from_hex64(s->repo_id, id);
    if (porthome_head_event_open(event, id, &s->ring, latest(s)->epoch, &head) != PORTHOME_OK)
        return HANAMI_ERR_AUTH;
    char address[65]; porthome_hex64(head->root_manifest, address);
    uint8_t *blob = NULL; size_t len = 0;
    hanami_error_t err = blossom_client_get_content(s->client, address, &blob, &len);
    if (err == HANAMI_OK &&
        porthome_dir_node_open(&s->ring, head->epoch, blob, len, out_root) != PORTHOME_OK)
        err = HANAMI_ERR_AUTH;
    free(blob);
    if (err == HANAMI_OK &&
        (!(*out_root)->vault || memcmp((*out_root)->vault->vault_id, id, 32) ||
         (*out_root)->vault->grants_rev != head->grants_rev)) err = HANAMI_ERR_AUTH;
    if (err == HANAMI_OK) { *out_head = head; return HANAMI_OK; }
    porthome_head_dispose(head); free(head);
    if (*out_root) { porthome_dir_node_dispose(*out_root); free(*out_root); *out_root = NULL; }
    return err;
}

hanami_error_t hanami_sealed_new(hanami_sealed **out, const char *endpoint,
    const char **relays, const char *repo_id, const char *owner,
    const hanami_signer_t *signer, const porthome_ring_t *ring,
    const hanami_config_t *config)
{
    if (!out || !endpoint || !relays || !hex_valid(repo_id, 64) || !hex_valid(owner, 64))
        return HANAMI_ERR_INVALID_ARG;
    *out = NULL;
    hanami_sealed *s = calloc(1, sizeof(*s));
    if (!s) return HANAMI_ERR_NOMEM;
    s->refs = 1;
    s->endpoint = strdup(endpoint);
    size_t relay_count = 0;
    while (relays[relay_count]) relay_count++;
    s->relays = calloc(relay_count + 1, sizeof(*s->relays));
    if (!s->endpoint || !s->relays) { hanami_sealed_release(s); return HANAMI_ERR_NOMEM; }
    for (size_t i = 0; i < relay_count; i++) {
        s->relays[i] = strdup(relays[i]);
        if (!s->relays[i]) { hanami_sealed_release(s); return HANAMI_ERR_NOMEM; }
    }
    if (signer) s->signer = *signer;
    hanami_error_t err = HANAMI_ERR_INVALID_ARG;
    if (vault_ring_copy(&s->keys, ring) != VAULT_OK) goto fail;
    s->ring = vault_ring_view(&s->keys);
    if (latest(s)->epoch > UINT32_MAX) goto fail;
    strcpy(s->owner, owner); strcpy(s->repo_id, repo_id);
    const char *writer = signer && signer->sign ? signer->pubkey : owner;
    if (!hex_valid(writer, 64)) goto fail;
    strcpy(s->writer, writer); strcpy(s->base_writer, owner);
    s->allow_insecure = hanami_config_get_allow_insecure(config);
    blossom_client_opts_t opts = {.endpoint = endpoint, .timeout_seconds = 30,
        .allow_insecure = s->allow_insecure, .user_agent = "libhanami/0.4"};
    err = blossom_client_new(&opts, signer, &s->client);
    if (err != HANAMI_OK) goto fail;
    err = hanami_nostr_ctx_new((const char *const *)relays, signer, &s->nostr);
    if (err != HANAMI_OK) goto fail;
    uint8_t id[32], owner_bytes[32], writer_bytes[32];
    porthome_from_hex64(repo_id, id); porthome_from_hex64(owner, owner_bytes);
    porthome_from_hex64(writer, writer_bytes);
    NostrEvent *event = NULL;
    err = hanami_nostr_fetch_head(s->nostr, id, owner, &event);
    if (err == HANAMI_ERR_NOT_FOUND) {
        /* An EOSE-confirmed empty slot can only be initialized by its owner. */
        if (!signer || !signer->sign || strcmp(writer, owner)) { err = HANAMI_ERR_AUTH; goto fail; }
        porthome_vault_info policy = {.writers = &owner_bytes, .writers_len = 1};
        memcpy(policy.vault_id, id, 32); memcpy(policy.owner, owner_bytes, 32);
        porthome_dir_node node = {.epoch = latest(s)->epoch, .vault = &policy};
        if (RAND_bytes(node.node_id, 32) != 1) { err = HANAMI_ERR_AUTH; goto fail; }
        uint8_t *blob = NULL, address[32]; size_t len = 0;
        if (porthome_dir_node_seal(&node, latest(s)->node_key, &blob, &len, address) != PORTHOME_OK) {
            err = HANAMI_ERR_AUTH; goto fail;
        }
        char hash[65];
        err = blossom_client_address(s->client, blob, len, hash);
        if (err == HANAMI_OK) err = blossom_client_put(s->client, blob, len, hash, NULL);
        if (err == HANAMI_OK) porthome_from_hex64(hash, address);
        free(blob);
        if (err != HANAMI_OK) goto fail;
        s->head = calloc(1, sizeof(*s->head)); s->state = calloc(1, sizeof(*s->state));
        if (!s->head || !s->state) { err = HANAMI_ERR_NOMEM; goto fail; }
        memcpy(s->head->vault_id, id, 32); memcpy(s->head->root_manifest, address, 32);
        s->head->epoch = latest(s)->epoch;
        s->state->head = strdup("ref: refs/heads/main");
        if (!s->state->head) { err = HANAMI_ERR_NOMEM; goto fail; }
        err = hanami_index_open(&s->index, ":memory:", NULL);
        if (err != HANAMI_OK) goto fail;
        s->writable = true;
    } else if (err == HANAMI_OK) {
        porthome_dir_node *root = NULL;
        porthome_head *owner_head = NULL;
        bool owner_lifted = false;
        /* Git needs root writer policy, not a filesystem fold or a complete
         * subtree inventory: concurrent heads remain independent Git history. */
        err = open_root(s, event, &owner_head, &root);
        bool authorized = false;
        if (err == HANAMI_OK && root->vault && !memcmp(root->vault->owner, owner_bytes, 32)) {
            for (size_t i = 0; i < root->vault->writers_len; i++)
                authorized |= !memcmp(root->vault->writers[i], writer_bytes, 32);
        } else if (err == HANAMI_OK) err = HANAMI_ERR_AUTH;
        s->writable = signer && signer->sign && authorized;
        const porthome_head *head = owner_head;
        uint8_t root_address[32]; uint64_t grants_rev = 0;
        if (head) { memcpy(root_address, head->root_manifest, 32); grants_rev = head->grants_rev; }
        nostr_event_free(event); event = NULL;
        if (err == HANAMI_OK && s->writable && strcmp(writer, owner)) {
            nip34_repo_state_t *owner_state = NULL;
            hanami_index_t *owner_index = NULL;
            err = read_snapshot(s, head, &owner_state, &owner_index, &owner_lifted);
            nip34_repo_state_free(owner_state); hanami_index_close(owner_index);
        }
        if (err == HANAMI_OK && s->writable && strcmp(writer, owner)) {
            err = hanami_nostr_fetch_head(s->nostr, id, writer, &event);
            if (err == HANAMI_ERR_NOT_FOUND) err = HANAMI_OK;
            else if (err == HANAMI_OK) {
                porthome_head *own = NULL;
                if (porthome_head_event_open(event, id, &s->ring, latest(s)->epoch, &own) != PORTHOME_OK ||
                    memcmp(own->root_manifest, root_address, 32) || own->grants_rev != grants_rev)
                    err = HANAMI_ERR_AUTH;
                if (err == HANAMI_OK) {
                    s->head = own; own = NULL; strcpy(s->base_writer, writer);
                }
                if (own) { porthome_head_dispose(own); free(own); }
            }
            nostr_event_free(event); event = NULL;
        }
        if (err == HANAMI_OK && !s->head) {
            uint8_t *plain = NULL; size_t len = 0;
            if (porthome_head_encode(head, &plain, &len) != PORTHOME_OK ||
                porthome_head_decode(plain, len, &s->head) != PORTHOME_OK) err = HANAMI_ERR_INDEX;
            secret_free(plain, len);
        }
        if (root) { porthome_dir_node_dispose(root); free(root); }
        if (owner_head) { porthome_head_dispose(owner_head); free(owner_head); }
        if (err == HANAMI_OK) err = read_snapshot(s, s->head, &s->state, &s->index, &s->lifted);
        /* Public disclosure is owner-authorized, not a member's assertion.
         * An older member branch cannot undo an owner's irreversible lift. */
        if (err == HANAMI_OK && s->writable && strcmp(writer, owner)) s->lifted = owner_lifted;
        if (err != HANAMI_OK) goto fail;
    } else goto fail;
    *out = s; return HANAMI_OK;
fail:
    hanami_sealed_release(s); return err;
}

/* Detect a moved writer slot or owner policy before changing it. This is not
 * compare-and-swap: simultaneous writers still need branches + explicit merge. */
static hanami_error_t check_publish_base(hanami_sealed *s)
{
    uint8_t id[32]; porthome_from_hex64(s->repo_id, id);
    const char *authors[] = {s->owner, s->writer};
    size_t count = strcmp(s->owner, s->writer) ? 2 : 1;
    for (size_t i = 0; i < count; i++) {
        NostrEvent *event = NULL; porthome_head *head = NULL;
        hanami_error_t err = hanami_nostr_fetch_head(s->nostr, id, authors[i], &event);
        bool own = !strcmp(authors[i], s->writer);
        if (err == HANAMI_ERR_NOT_FOUND) {
            if ((own && strcmp(s->base_writer, s->writer)) || !s->head->has_ref_state) continue;
            return HANAMI_ERR_INDEX;
        }
        if (err != HANAMI_OK) return err;
        if (porthome_head_event_open(event, id, &s->ring, latest(s)->epoch, &head) != PORTHOME_OK)
            err = HANAMI_ERR_AUTH;
        else if (memcmp(head->root_manifest, s->head->root_manifest, 32) ||
                 head->grants_rev != s->head->grants_rev ||
                 (own && (strcmp(s->base_writer, s->writer) || head->seq != s->head->seq ||
                          head->has_ref_state != s->head->has_ref_state ||
                          memcmp(head->ref_state, s->head->ref_state, 32))))
            err = HANAMI_ERR_INDEX;
        nostr_event_free(event);
        if (head) { porthome_head_dispose(head); free(head); }
        if (err != HANAMI_OK) return err;
    }
    return HANAMI_OK;
}

hanami_error_t hanami_sealed_publish(hanami_sealed *s, const nip34_ref_t *refs,
    size_t count, const char *head)
{
    if (!s->writable) return HANAMI_ERR_AUTH;
    hanami_error_t base = check_publish_base(s);
    if (base != HANAMI_OK) return base;
    if (s->head->seq == UINT64_MAX || s->head->written_at >= INT64_MAX) return HANAMI_ERR_INDEX;
    uint8_t *plain = NULL, *blob = NULL; size_t len = 0, n = 0;
    hanami_error_t err = encode_state(s, refs, count, head, &plain, &len);
    if (err != HANAMI_OK) return err;
    /* Decode before uploading, to commit only a snapshot our reader accepts. */
    nip34_repo_state_t *next_state = NULL; hanami_index_t *check = NULL; bool lifted;
    err = decode_state(plain, len, &next_state, &check, &lifted);
    hanami_index_close(check);
    if (err != HANAMI_OK) { secret_free(plain, len); return err; }
    uint8_t address[32];
    int rc = porthome_encrypt_manifest_v2(latest(s)->node_key, plain, len, &blob, &n, address);
    secret_free(plain, len);
    if (rc != PORTHOME_OK) { nip34_repo_state_free(next_state); return HANAMI_ERR_AUTH; }
    char hash[65];
    err = blossom_client_address(s->client, blob, n, hash);
    if (err == HANAMI_OK) err = blossom_client_put(s->client, blob, n, hash, NULL);
    if (err == HANAMI_OK) porthome_from_hex64(hash, address);
    free(blob);
    if (err != HANAMI_OK) { nip34_repo_state_free(next_state); return err; }
    porthome_head next = *s->head;
    porthome_head_parent parent = {.seq = s->head->seq};
    memcpy(parent.root_manifest, s->head->root_manifest, 32);
    porthome_from_hex64(s->base_writer, parent.writer);
    next.parents = s->head->has_ref_state ? &parent : NULL;
    next.parents_len = s->head->has_ref_state ? 1 : 0;
    next.has_ref_state = 1; memcpy(next.ref_state, address, 32);
    next.seq++; next.epoch = latest(s)->epoch;
    uint64_t now = (uint64_t)time(NULL);
    next.written_at = now > s->head->written_at ? now : s->head->written_at + 1;
    uint8_t writer[32]; porthome_from_hex64(s->writer, writer);
    NostrEvent *event = NULL;
    if (porthome_head_event_new(&next, latest(s)->node_key, writer, &event) != PORTHOME_OK)
        err = HANAMI_ERR_AUTH;
    else {
        err = hanami_nostr_sign_head(s->nostr, event);
        if (err == HANAMI_OK) err = hanami_nostr_publish_event(s->nostr, event);
    }
    nostr_event_free(event);
    if (err == HANAMI_OK) {
        /* Keep the acknowledged tuple; parents are only needed on the wire. */
        porthome_head_dispose(s->head);
        next.parents = NULL; next.parents_len = 0;
        *s->head = next;
        nip34_repo_state_free(s->state); s->state = next_state; next_state = NULL;
        strcpy(s->base_writer, s->writer);
    }
    nip34_repo_state_free(next_state);
    return err;
}

bool hanami_repo_private(git_repository *repo)
{
    if (!repo) return false;
    hanami_sealed *s = hanami_sealed_for_repo(repo);
    if (s) return !s->lifted;
    git_config *config = NULL;
    if (git_repository_config(&config, repo) < 0) return true;
    git_buf id = GIT_BUF_INIT;
    int rc = git_config_get_string_buf(&id, config, "hanami.vault-id");
    int public = 0;
    if (rc < 0 && rc != GIT_ENOTFOUND) {
        git_buf_dispose(&id); git_config_free(config); return true;
    }
    bool sealed = rc != GIT_ENOTFOUND;
    if (sealed) git_config_get_bool(&public, config, "hanami.public");
    git_buf_dispose(&id); git_config_free(config);
    return sealed && !public;
}

hanami_error_t hanami_sealed_attach_odb(git_repository *repo, hanami_sealed *s)
{
    git_config *config = NULL;
    if (git_repository_config(&config, repo) < 0) return HANAMI_ERR_LIBGIT2;
    int rc = git_config_set_string(config, "hanami.vault-id", s->repo_id);
    if (rc == 0) rc = git_config_set_bool(config, "hanami.public", s->lifted);
    git_config_free(config);
    if (rc < 0) return HANAMI_ERR_LIBGIT2;
    git_odb *odb = NULL; git_odb_backend *backend = NULL;
    hanami_error_t err = hanami_sealed_odb_new(&backend, s);
    if (err != HANAMI_OK) return err;
    if (git_repository_odb(&odb, repo) < 0 || git_odb_add_backend(odb, backend, 999) < 0) {
        backend->free(backend); git_odb_free(odb); return HANAMI_ERR_LIBGIT2;
    }
    git_odb_free(odb); return HANAMI_OK;
}

/* Clone's local refs remain local until explicit repo_sync(). The transport
 * advertises the private snapshot; objects are read through its attached ODB. */
typedef struct { git_transport parent; hanami_sealed *s; git_remote_head **heads; size_t count; bool connected; } sealed_transport;
static int sealed_connect(git_transport *t, const char *url, int direction, const git_remote_connect_options *opts)
{
    (void)url; (void)opts;
    if (direction != GIT_DIRECTION_FETCH) return -1;
    ((sealed_transport *)t)->connected = true; return 0;
}
static int sealed_options(git_transport *t, const git_remote_connect_options *opts)
{ (void)t; (void)opts; return 0; }
static int sealed_push(git_transport *t, git_push *push)
{ (void)t; (void)push; return -1; }
static int sealed_caps(unsigned int *caps, git_transport *t) { (void)t; *caps = 0; return 0; }
static int sealed_ls(const git_remote_head ***heads, size_t *count, git_transport *t)
{
    sealed_transport *st = (sealed_transport *)t;
    *heads = (const git_remote_head **)st->heads; *count = st->count; return 0;
}
static int sealed_negotiate(git_transport *t, git_repository *r, const git_fetch_negotiation *n)
{ (void)t; (void)r; (void)n; return 0; }
static int sealed_roots(git_oidarray *out, git_transport *t) { (void)t; out->ids = NULL; out->count = 0; return 0; }
static int sealed_download(git_transport *t, git_repository *r, git_indexer_progress *stats)
{ (void)t; (void)r; if (stats) memset(stats, 0, sizeof(*stats)); return 0; }
static int sealed_connected(git_transport *t) { return ((sealed_transport *)t)->connected; }
static void sealed_cancel(git_transport *t) { (void)t; }
static int sealed_close(git_transport *t) { ((sealed_transport *)t)->connected = false; return 0; }
static void sealed_transport_free(git_transport *t)
{
    sealed_transport *st = (sealed_transport *)t;
    for (size_t i = 0; i < st->count; i++) {
        free(st->heads[i]->name); free(st->heads[i]->symref_target); free(st->heads[i]);
    }
    free(st->heads); free(st);
}
static int sealed_transport_new(git_transport **out, git_remote *remote, void *payload)
{
    (void)remote;
    hanami_sealed *s = payload;
    sealed_transport *st = calloc(1, sizeof(*st));
    if (!st) return -1;
    st->s = s;
    if (git_transport_init(&st->parent, GIT_TRANSPORT_VERSION) < 0) { free(st); return -1; }
    st->parent.connect = sealed_connect; st->parent.capabilities = sealed_caps;
    st->parent.set_connect_opts = sealed_options; st->parent.push = sealed_push;
    st->parent.ls = sealed_ls; st->parent.negotiate_fetch = sealed_negotiate;
    st->parent.shallow_roots = sealed_roots; st->parent.download_pack = sealed_download;
    st->parent.is_connected = sealed_connected; st->parent.cancel = sealed_cancel;
    st->parent.close = sealed_close; st->parent.free = sealed_transport_free;
    st->heads = calloc(s->state->ref_count + 1, sizeof(*st->heads));
    if (!st->heads) { free(st); return -1; }
    for (size_t i = 0; i < s->state->ref_count + 1; i++) {
        const char *name = i == 0 ? "HEAD" : s->state->refs[i - 1].refname;
        const char *target = i == 0 ? s->state->head : s->state->refs[i - 1].target;
        if (!target) continue;
        const char *sym = NULL;
        if (!strncmp(target, "ref: ", 5)) {
            sym = target + 5; target = NULL;
            for (size_t j = 0; j < s->state->ref_count; j++)
                if (!strcmp(s->state->refs[j].refname, sym)) { target = s->state->refs[j].target; break; }
        }
        if (!oid_valid(target)) continue; /* unborn HEAD or chained symbolic ref */
        git_remote_head *h = calloc(1, sizeof(*h));
        if (!h) { sealed_transport_free(&st->parent); return -1; }
        st->heads[st->count++] = h;
        h->name = strdup(name); h->symref_target = sym ? strdup(sym) : NULL;
        if (!h->name || (sym && !h->symref_target) || git_oid_fromstr(&h->oid, target) < 0) {
            sealed_transport_free(&st->parent); return -1;
        }
    }
    *out = &st->parent; return 0;
}
static int clone_repo(git_repository **out, const char *path, int bare, void *payload)
{
    if (git_repository_init(out, path, bare) < 0) return -1;
    if (hanami_sealed_attach_odb(*out, payload) != HANAMI_OK) {
        git_repository_free(*out); *out = NULL; return -1;
    }
    return 0;
}
hanami_error_t hanami_clone_sealed(git_repository **out, const char *path,
    const char *endpoint, const char **relays, const char *id, const char *owner,
    const hanami_signer_t *signer, const porthome_ring_t *ring,
    const hanami_config_t *config)
{
    if (!out || !path || !ring) return HANAMI_ERR_INVALID_ARG;
    *out = NULL;
    hanami_sealed *s = NULL;
    hanami_error_t err = hanami_sealed_new(&s, endpoint, relays, id, owner, signer, ring, config);
    if (err != HANAMI_OK) return err;
    git_clone_options opts;
    git_clone_options_init(&opts, GIT_CLONE_OPTIONS_VERSION);
    opts.local = GIT_CLONE_NO_LOCAL;
    opts.repository_cb = clone_repo; opts.repository_cb_payload = s;
    opts.fetch_opts.callbacks.transport = sealed_transport_new;
    opts.fetch_opts.callbacks.payload = s;
    int rc = git_clone(out, "hanami-sealed://vault", path, &opts);
    hanami_sealed_release(s);
    return rc == 0 ? HANAMI_OK : HANAMI_ERR_LIBGIT2;
}

/* Upload through the remote backend explicitly: git_odb_write could skip an
 * object already present in the local loose backend after clone/edit. */
typedef struct { git_odb *odb; git_odb_backend *remote; } sync_objects;
static int sync_object(const git_oid *oid, void *ctx)
{
    sync_objects *s = ctx; git_odb_object *obj = NULL;
    if (git_odb_read(&obj, s->odb, oid) < 0) return -1;
    int rc = s->remote->write(s->remote, oid, git_odb_object_data(obj),
        git_odb_object_size(obj), git_odb_object_type(obj));
    git_odb_object_free(obj); return rc;
}
hanami_error_t hanami_repo_sync(git_repository *repo)
{
    hanami_sealed *s = hanami_sealed_for_repo(repo);
    if (!s || !s->writable) return HANAMI_ERR_AUTH;
    git_odb *odb = NULL; git_odb_backend *remote = NULL;
    if (git_repository_odb(&odb, repo) < 0) return HANAMI_ERR_LIBGIT2;
    hanami_error_t err = hanami_sealed_odb_new(&remote, s);
    if (err == HANAMI_OK) {
        sync_objects ctx = {odb, remote};
        if (git_odb_foreach(odb, sync_object, &ctx) != 0) err = HANAMI_ERR_UPLOAD;
        remote->free(remote);
    }
    git_odb_free(odb);
    if (err != HANAMI_OK) return err;
    git_reference_iterator *iter = NULL; git_reference *ref = NULL;
    if (git_reference_iterator_new(&iter, repo) < 0) return HANAMI_ERR_LIBGIT2;
    size_t count = 0, cap = 16; nip34_ref_t *refs = calloc(cap, sizeof(*refs));
    if (!refs) { git_reference_iterator_free(iter); return HANAMI_ERR_NOMEM; }
    char *head = NULL;
    int rc;
    while ((rc = git_reference_next(&ref, iter)) == 0) {
        const char *name = git_reference_name(ref);
        if (!strncmp(name, "refs/remotes/", 13) || !strcmp(name, "HEAD")) { git_reference_free(ref); continue; }
        if (count == cap) {
            cap *= 2; nip34_ref_t *grown = realloc(refs, cap * sizeof(*refs));
            if (!grown) { git_reference_free(ref); err = HANAMI_ERR_NOMEM; break; }
            refs = grown;
        }
        char target[1100];
        if (git_reference_type(ref) == GIT_REFERENCE_SYMBOLIC)
            snprintf(target, sizeof(target), "ref: %s", git_reference_symbolic_target(ref));
        else git_oid_tostr(target, sizeof(target), git_reference_target(ref));
        refs[count].refname = strdup(name); refs[count].target = strdup(target);
        git_reference_free(ref);
        if (!refs[count].refname || !refs[count].target) { count++; err = HANAMI_ERR_NOMEM; break; }
        count++;
    }
    git_reference_iterator_free(iter);
    if (rc != GIT_ITEROVER && err == HANAMI_OK) err = HANAMI_ERR_LIBGIT2;
    if (git_reference_lookup(&ref, repo, "HEAD") == 0) {
        char target[1100];
        if (git_reference_type(ref) == GIT_REFERENCE_SYMBOLIC)
            snprintf(target, sizeof(target), "ref: %s", git_reference_symbolic_target(ref));
        else git_oid_tostr(target, sizeof(target), git_reference_target(ref));
        head = strdup(target); git_reference_free(ref);
        if (!head) err = HANAMI_ERR_NOMEM;
    }
    if (err == HANAMI_OK) err = hanami_sealed_publish(s, refs, count, head);
    for (size_t i = 0; i < count; i++) { free(refs[i].refname); free(refs[i].target); }
    free(refs); free(head); return err;
}

typedef struct { hanami_index_t *source, *destination; } import_index;
static porthome_index_status import_row(void *payload, const porthome_index_entry *row)
{
    import_index *ctx = payload;
    /* An imported writer may add history, but cannot replace a mapping this
     * reader already trusts for the same immutable Git object. */
    if (hanami_index_exists(ctx->destination, row->git_oid)) return PORTHOME_INDEX_OK;
    hanami_index_entry_t entry;
    if (hanami_index_get_by_oid(ctx->source, row->git_oid, &entry) != HANAMI_OK ||
        hanami_index_put(ctx->destination, &entry) != HANAMI_OK) return PORTHOME_INDEX_ERR_STORAGE;
    return PORTHOME_INDEX_OK;
}

hanami_error_t hanami_repo_fetch_writer(git_repository *repo, const char *writer)
{
    hanami_sealed *s = hanami_sealed_for_repo(repo);
    if (!s || !hex_valid(writer, 64)) return HANAMI_ERR_INVALID_ARG;
    uint8_t id[32], owner[32], member[32];
    porthome_from_hex64(s->repo_id, id); porthome_from_hex64(s->owner, owner);
    porthome_from_hex64(writer, member);
    NostrEvent *event = NULL;
    porthome_head *anchor = NULL, *head = NULL;
    porthome_dir_node *root = NULL;
    nip34_repo_state_t *state = NULL;
    hanami_index_t *index = NULL;
    hanami_error_t err = hanami_nostr_fetch_head(s->nostr, id, s->owner, &event);
    if (err != HANAMI_OK) goto done;
    err = open_root(s, event, &anchor, &root);
    nostr_event_free(event); event = NULL;
    if (err != HANAMI_OK) goto done;
    bool allowed = false;
    if (memcmp(root->vault->owner, owner, 32)) { err = HANAMI_ERR_AUTH; goto done; }
    for (size_t i = 0; i < root->vault->writers_len; i++)
        allowed |= !memcmp(root->vault->writers[i], member, 32);
    if (!allowed) { err = HANAMI_ERR_AUTH; goto done; }
    err = hanami_nostr_fetch_head(s->nostr, id, writer, &event);
    if (err != HANAMI_OK) goto done;
    if (porthome_head_event_open(event, id, &s->ring, latest(s)->epoch, &head) != PORTHOME_OK ||
        memcmp(head->root_manifest, anchor->root_manifest, 32) || head->grants_rev != anchor->grants_rev) {
        err = HANAMI_ERR_AUTH; goto done;
    }
    bool public;
    err = read_snapshot(s, head, &state, &index, &public);
    if (err != HANAMI_OK) goto done;
    import_index ctx = {index, s->index};
    if (porthome_index_iterate(hanami_index_porthome(index), import_row, &ctx) != PORTHOME_INDEX_OK) {
        err = HANAMI_ERR_INDEX; goto done;
    }
    for (size_t i = 0; i < state->ref_count; i++) {
        const nip34_ref_t *r = &state->refs[i];
        /* Only direct branch tips become remote tracking refs. Tags/HEAD and
         * existing remote tracking names cannot escape this namespace. */
        if (strncmp(r->refname, "refs/heads/", 11) || !oid_valid(r->target)) continue;
        size_t n = strlen(writer) + strlen(r->refname) + 20;
        char *name = malloc(n);
        if (!name) { err = HANAMI_ERR_NOMEM; break; }
        snprintf(name, n, "refs/remotes/%s/%s", writer, r->refname + 11);
        git_reference *ref = NULL; git_oid oid;
        git_oid_fromstr(&oid, r->target);
        int rc = git_reference_create(&ref, repo, name, &oid, 1, "fetch private writer");
        free(name); git_reference_free(ref);
        if (rc < 0) { err = HANAMI_ERR_LIBGIT2; break; }
    }
done:
    nostr_event_free(event);
    if (head) { porthome_head_dispose(head); free(head); }
    if (anchor) { porthome_head_dispose(anchor); free(anchor); }
    if (root) { porthome_dir_node_dispose(root); free(root); }
    nip34_repo_state_free(state); hanami_index_close(index);
    return err;
}

hanami_error_t hanami_repo_lift_privacy(git_repository *repo)
{
    hanami_sealed *s = hanami_sealed_for_repo(repo);
    if (!s || !s->writable || strcmp(s->writer, s->owner)) return HANAMI_ERR_AUTH;
    if (s->lifted) return HANAMI_OK;
    /* First checkpoint any local objects/refs while still private. */
    hanami_error_t err = hanami_repo_sync(repo);
    if (err != HANAMI_OK) return err;
    /* Use the existing public uploader/projection, not a second HTTP client. */
    s->lifted = true;
    err = hanami_push_to_blossom(repo, s->endpoint, &s->signer,
                                 (const char **)s->relays, s->repo_id);
    if (err == HANAMI_OK)
        err = hanami_sealed_publish(s, s->state->refs, s->state->ref_count, s->state->head);
    if (err != HANAMI_OK) s->lifted = false;
    else {
        git_config *config = NULL;
        if (git_repository_config(&config, repo) < 0) return HANAMI_ERR_LIBGIT2;
        int rc = git_config_set_bool(config, "hanami.public", 1);
        git_config_free(config);
        if (rc < 0) return HANAMI_ERR_LIBGIT2;
    }
    return err;
}
