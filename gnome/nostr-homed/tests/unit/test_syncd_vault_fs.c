/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "nh_syncd_vault_fs.h"
#include <nostr-event.h>
#include <nostr-keys.h>
#include <openssl/crypto.h>
#include <assert.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    uint8_t hash[32], *bytes;
    size_t len;
} object;
typedef struct {
    object objects[256];
    size_t count, reads, puts;
    int fail;
    uint8_t fail_hash[32];
} store;
static int get(void *ctx, const uint8_t hash[32], uint8_t **out, size_t *len)
{
    store *s = ctx;
    ++s->reads;
    *out = NULL;
    *len = 0;
    if (s->fail && !memcmp(hash, s->fail_hash, 32))
        return -1;
    for (size_t i = 0; i < s->count; ++i)
        if (!memcmp(hash, s->objects[i].hash, 32)) {
            *len = s->objects[i].len;
            *out = malloc(*len);
            assert(*out);
            memcpy(*out, s->objects[i].bytes, *len);
            return 0;
        }
    return -1;
}
static int put(void *ctx, const uint8_t *bytes, size_t len, const uint8_t hash[32])
{
    store *s = ctx;
    uint8_t actual[32];
    assert(porthome_sha256(bytes, len, actual) == 0 && !memcmp(hash, actual, 32));
    ++s->puts;
    for (size_t i = 0; i < s->count; ++i)
        if (!memcmp(hash, s->objects[i].hash, 32))
            return 0;
    assert(s->count < 256);
    object *o = &s->objects[s->count++];
    memcpy(o->hash, hash, 32);
    o->len = len;
    o->bytes = malloc(len);
    assert(o->bytes);
    memcpy(o->bytes, bytes, len);
    return 0;
}
static void make_file(int dir, const char *name, const uint8_t *data, size_t n)
{
    int fd = openat(dir, name, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    assert(fd >= 0);
    size_t done = 0;
    while (done < n) {
        ssize_t z = write(fd, data + done, n - done);
        assert(z > 0);
        done += z;
    }
    assert(fsync(fd) == 0 && close(fd) == 0);
}
static vault_tree *directory_mode(const vault_tree *base, unsigned mode)
{
    size_t count = vault_tree_count(base);
    vault_tree_node *nodes = calloc(count, sizeof(*nodes));
    assert(nodes);
    uint8_t *encoded = NULL;
    size_t encoded_len = 0;
    const porthome_dir_node *root = vault_tree_node_at(base, 0, NULL);
    assert(porthome_dir_node_encode(root, &encoded, &encoded_len) == 0);
    porthome_dir_node *copy = NULL;
    assert(porthome_dir_node_decode(encoded, encoded_len, &copy) == 0);
    OPENSSL_cleanse(encoded, encoded_len);
    free(encoded);
    for (size_t i = 0; i < copy->entries_len; ++i)
        if (copy->entries[i].kind == PORTHOME_KIND_DIR)
            copy->entries[i].mode = S_IFDIR | mode;
    for (size_t i = 0; i < count; ++i) {
        nodes[i].node = vault_tree_node_at(base, i, &nodes[i].ring);
        if (!i)
            nodes[i].node = copy;
    }
    vault_tree *tree = NULL;
    assert(vault_tree_new(nodes, count, &tree) == 0);
    free(nodes);
    porthome_dir_node_dispose(copy);
    free(copy);
    return tree;
}
static vault_tree *one_entry(porthome_vault_info *policy, const porthome_ring_t *ring,
                             porthome_entry_kind kind, const uint8_t hash[32], size_t length)
{
    porthome_node_chunk chunk = {
        .size_plain = length, .size_sealed = length + PORTHOME_SEAL_OVERHEAD, .chunk_key_id = 1};
    memcpy(chunk.sha256, hash, 32);
    porthome_node_entry entry = {.kind = kind};
    strcpy(entry.name_plain, "item");
    assert(porthome_encrypt_name_v2(ring->entries[0].node_key, "item", entry.name_enc) == 0);
    porthome_dir_node root = {.epoch = 1, .vault = policy, .entries = &entry, .entries_len = 1};
    root.node_id[0] = 99;
    porthome_dir_node child = {.epoch = 1};
    child.node_id[0] = 100;
    porthome_ring_entry_t child_key = {.epoch = 1};
    assert(porthome_child_node_key(ring->entries[0].node_key, child.node_id, NULL,
                                   child_key.node_key) == 0);
    vault_tree_node nodes[] = {{&root, *ring}, {&child, {&child_key, 1}}};
    if (kind == PORTHOME_KIND_FILE) {
        entry.mode = S_IFREG | 0600;
        entry.chunks = &chunk;
        entry.chunks_len = 1;
        entry.size_plain = length;
    } else if (kind == PORTHOME_KIND_DIR) {
        entry.mode = S_IFDIR | 0700;
        entry.child_epoch = 1;
        memcpy(entry.node_id, child.node_id, 32);
    } else {
        entry.mode = S_IFLNK | 0777;
        entry.symlink_target = "target";
    }
    vault_tree *tree = NULL;
    assert(vault_tree_new(nodes, kind == PORTHOME_KIND_DIR ? 2 : 1, &tree) == 0);
    OPENSSL_cleanse(child_key.node_key, 32);
    return tree;
}
static int swap_alpha_fifo(void *ctx, const char *path)
{
    if (strcmp(path, "alpha"))
        return 0;
    int fd = *(int *)ctx;
    assert(unlinkat(fd, "alpha", 0) == 0 && mkfifoat(fd, "alpha", 0600) == 0);
    return 1;
}
static int dirty_beta(void *ctx, const char *path)
{
    (void)ctx;
    return !strcmp(path, "nested/beta");
}
static int remove_owned(const char *path, const struct stat *st, int kind, struct FTW *f)
{
    (void)st;
    (void)kind;
    (void)f;
    return remove(path);
}
int nh_syncd_state_upsert_file_(nh_syncd_state *, const char *, uint32_t, uint32_t, uint32_t,
                                uint64_t, uint64_t, const char *, const char *const *, size_t);
int nh_syncd_state_upsert_dir_(nh_syncd_state *, const char *, uint32_t, uint32_t, uint32_t,
                               uint64_t);
int main(void)
{
    char work[] = "/tmp/nostrc-vault-fs-XXXXXX";
    assert(mkdtemp(work));
    int dir = open(work, O_RDONLY | O_DIRECTORY);
    assert(dir >= 0 && mkdirat(dir, "source", 0700) == 0 && mkdirat(dir, "home", 0700) == 0);
    int source = openat(dir, "source", O_RDONLY | O_DIRECTORY);
    int home = openat(dir, "home", O_RDONLY | O_DIRECTORY);
    assert(source >= 0 && home >= 0 && mkdirat(source, "nested", 0700) == 0);
    size_t bytes_len = 2 * PORTHOME_CHUNK_MAX + 17;
    uint8_t *bytes = malloc(bytes_len);
    assert(bytes);
    uint32_t rng = 0x12345678;
    for (size_t i = 0; i < bytes_len; ++i) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        bytes[i] = (uint8_t)rng;
    }
    memset(bytes + bytes_len - 11, 0, 11); /* Genuine zero suffix, not padding. */
    make_file(source, "alpha", bytes, bytes_len);
    int nested = openat(source, "nested", O_RDONLY | O_DIRECTORY);
    assert(nested >= 0);
    make_file(nested, "beta", (const uint8_t *)"old nested", 10);
    close(nested);
    assert(symlinkat("alpha", source, "link") == 0);
    uint8_t key[32] = {42}, writer[1][32];
    char secret[65];
    memset(secret, '0', 64);
    secret[63] = '1';
    secret[64] = 0;
    char *pub = nostr_key_get_public(secret);
    assert(pub && porthome_from_hex64(pub, writer[0]) == 0);
    free(pub);
    porthome_vault_info policy = {.writers = writer, .writers_len = 1, .grants_rev = 1};
    policy.vault_id[0] = 7;
    memcpy(policy.owner, writer[0], 32);
    porthome_dir_node empty = {.epoch = 1, .vault = &policy};
    empty.node_id[0] = 9;
    porthome_ring_entry_t first = {.epoch = 1};
    memcpy(first.node_key, key, 32);
    porthome_ring_t ring = {&first, 1};
    vault_tree_node initial = {&empty, ring};
    vault_tree *base = NULL, *captured = NULL;
    assert(vault_tree_new(&initial, 1, &base) == 0);
    store storage = {0};
    vault_blob_io io = {&storage, get, put};
    assert(nh_syncd_vault_capture(source, base, NULL, &io, &captured) == 0 &&
           vault_tree_count(captured) == 2);
    const porthome_node_entry *alpha = NULL;
    porthome_ring_t alpha_ring;
    assert(nh_syncd_vault_lookup(captured, "alpha", 0, &alpha, &alpha_ring) == 0 &&
           alpha->chunks_len > 1 && alpha->size_plain == bytes_len);
    for (size_t i = 0; i < alpha->chunks_len; ++i)
        assert(alpha->chunks[i].size_plain <= PORTHOME_CHUNK_MAX &&
               (i + 1 == alpha->chunks_len || alpha->chunks[i].size_plain >= PORTHOME_CHUNK_MIN));
    uint8_t hash[32];
    assert(vault_tree_seal(captured, &io, hash) == 0);
    porthome_head *head = NULL;
    assert(vault_tree_head_new(captured, NULL, writer[0], hash, 1, &head) == 0);
    NostrEvent *event = NULL;
    assert(porthome_head_event_new(head, key, writer[0], &event) == 0 &&
           nostr_event_sign(event, secret) == 0);
    vault_record *record = NULL;
    vault_tree *cold = NULL;
    assert(vault_record_fetch(event, policy.vault_id, empty.node_id, &ring, 1, NULL, NULL, &io,
                              &record, &cold) == 0);
    size_t offset = (size_t)alpha->chunks[0].size_plain - 3;
    uint8_t sample[8192];
    size_t got = 0;
    assert(nh_syncd_vault_read(cold, "alpha", offset, sample, sizeof(sample), &io, &got) == 0 &&
           got == sizeof(sample) && !memcmp(sample, bytes + offset, got));
    char home_abs[4096];
    snprintf(home_abs, sizeof(home_abs), "%s/home", work);
    nh_syncd_state *state = NULL;
    assert(nh_syncd_state_new(home_abs, "test", "", empty.node_id, &state) == 0);
    assert(nh_syncd_vault_materialize(home, "Portable/Team", cold, state, &io) == 0);
    int team = openat(home, "Portable/Team", O_RDONLY | O_DIRECTORY);
    assert(team >= 0);
    int actual = openat(team, "alpha", O_RDONLY | O_NOFOLLOW);
    assert(actual >= 0 && lseek(actual, (off_t)(bytes_len - sizeof(sample)), SEEK_SET) >= 0);
    assert(read(actual, sample, sizeof(sample)) == sizeof(sample) &&
           !memcmp(sample, bytes + bytes_len - sizeof(sample), sizeof(sample)));
    close(actual);
    close(team);
    uint8_t removed[32] = {17};
    vault_tree *rotated = NULL, *unchanged = NULL;
    assert(vault_tree_revoke(captured, head, writer[0], removed, 2, &rotated) == 0);
    size_t puts_before = storage.puts;
    assert(nh_syncd_vault_capture(source, rotated, NULL, &io, &unchanged) == 0 &&
           storage.puts == puts_before);
    const porthome_node_entry *old_chunk_file;
    porthome_ring_t after_ring;
    assert(nh_syncd_vault_lookup(unchanged, "alpha", 0, &old_chunk_file, &after_ring) == 0 &&
           old_chunk_file->chunks[0].chunk_key_id == 1 && after_ring.count == 2);
    porthome_ring_t latest_only = {after_ring.entries + 1, 1};
    uint8_t *refused = (uint8_t *)1;
    size_t refused_len = 99, reads_before = storage.reads;
    assert(vault_chunk_read(&old_chunk_file->chunks[0], &latest_only, &io, &refused,
                            &refused_len) == PORTHOME_ERR_EPOCH &&
           !refused && refused_len == 0 && storage.reads == reads_before);
    vault_tree_reseal_result *task = NULL;
    assert(vault_tree_reseal(unchanged, 0, 0, &io, &task) == VAULT_ERR_BUDGET &&
           task->completed == 0 && task->remaining > 0 && storage.reads == reads_before);
    vault_tree_reseal_result_free(task);
    make_file(source, "alpha", (const uint8_t *)"new epoch", 9);
    vault_tree *modified = NULL;
    assert(nh_syncd_vault_capture(source, unchanged, NULL, &io, &modified) == 0);
    assert(nh_syncd_vault_lookup(modified, "alpha", 0, &alpha, &alpha_ring) == 0 &&
           alpha->chunks[0].chunk_key_id == 2);
    const porthome_node_entry *beta;
    assert(nh_syncd_vault_lookup(modified, "nested/beta", 0, &beta, &alpha_ring) == 0 &&
           beta->chunks[0].chunk_key_id == 1);
    /* A delivered filesystem change forces capture even if a tool restored the
     * previous mtime. Other old-epoch files remain lazy; normal ignores apply. */
    uint64_t saved_mtime = beta->mtime_ns;
    nested = openat(source, "nested", O_RDONLY | O_DIRECTORY);
    assert(nested >= 0);
    make_file(nested, "beta", (const uint8_t *)"CHANGED123", 10);
    struct timespec restore[2] = {
        {.tv_nsec = UTIME_OMIT},
        {.tv_sec = (time_t)(saved_mtime / 1000000000u), .tv_nsec = saved_mtime % 1000000000u}};
    assert(utimensat(nested, "beta", restore, 0) == 0);
    close(nested);
    make_file(source, "ignored", (const uint8_t *)"private", 7);
    nh_syncd_ignore *ignore = NULL;
    assert(nh_syncd_ignore_new(home_abs, &ignore) == 0 &&
           nh_syncd_ignore_add_user_glob(ignore, "Portable/Team/ignored") == 0);
    nh_syncd_vault_scan scan = {
        .ignore = ignore, .home_prefix = "Portable/Team", .changed = dirty_beta};
    vault_tree *forced = NULL;
    assert(nh_syncd_vault_capture(source, modified, &scan, &io, &forced) == 0);
    assert(nh_syncd_vault_read(forced, "nested/beta", 0, sample, 10, &io, &got) == 0 && got == 10 &&
           !memcmp(sample, "CHANGED123", 10));
    assert(nh_syncd_vault_lookup(forced, "ignored", 0, &beta, &alpha_ring) == VAULT_ERR_INCOMPLETE);
    assert(vault_tree_seal(forced, &io, hash) == 0);
    vault_tree *locked = directory_mode(cold, 0000);
    vault_tree *updated_locked = directory_mode(forced, 0000);
    assert(nh_syncd_vault_materialize(home, "Portable/Team", locked, state, &io) == 0);
    assert(nh_syncd_vault_materialize(home, "Portable/Team", updated_locked, state, &io) == 0);
    team = openat(home, "Portable/Team", O_RDONLY | O_DIRECTORY);
    struct stat permissions;
    assert(team >= 0 && fstatat(team, "nested", &permissions, AT_SYMLINK_NOFOLLOW) == 0 &&
           (permissions.st_mode & 0777) == 0);
    assert(nh_syncd_vault_lookup(forced, "nested/beta", 0, &beta, &alpha_ring) == 0);
    memcpy(storage.fail_hash, beta->chunks[0].sha256, 32);
    storage.fail = 1;
    assert(nh_syncd_vault_materialize(home, "Portable/Team", updated_locked, state, &io) ==
           VAULT_ERR_IO);
    assert(fstatat(team, "nested", &permissions, AT_SYMLINK_NOFOLLOW) == 0 &&
           (permissions.st_mode & 0777) == 0);
    storage.fail = 0;
    assert(fchmodat(team, "nested", 0700, 0) == 0);
    close(team);
    vault_tree_free(locked);
    vault_tree_free(updated_locked);
    vault_tree_free(forced);
    nh_syncd_ignore_free(ignore);
    assert(unlinkat(source, "ignored", 0) == 0);
    /* v1 inventory migration: unchanged chunk bytes/address, only new manifests.
     * The old four-MiB geometry provides exact v1 plaintext lengths. */
    uint8_t *sealed_v1 = NULL, v1_hash[32];
    size_t sealed_len = 0;
    assert(porthome_encrypt_chunk(key, bytes, bytes_len, &sealed_v1, &sealed_len, v1_hash) == 0 &&
           put(&storage, sealed_v1, sealed_len, v1_hash) == 0);
    free(sealed_v1);
    nh_syncd_state *legacy = NULL;
    assert(nh_syncd_state_new(home_abs, "legacy", "", empty.node_id, &legacy) == 0);
    char v1_hex[65];
    porthome_hex64(v1_hash, v1_hex);
    const char *v1_chunks[] = {v1_hex};
    assert(nh_syncd_state_upsert_file_(legacy, "Portable/Team/nested/old", S_IFREG | 0600, getuid(),
                                       getgid(), 1000000000, bytes_len, "", v1_chunks, 1) == 0);
    assert(nh_syncd_state_upsert_dir_(legacy, "Portable/Team/nested", S_IFDIR | 0700, getuid(),
                                      getgid(), 1000000000) == 0);
    puts_before = storage.puts;
    vault_tree *migrated = NULL;
    assert(nh_syncd_vault_migrate(legacy, "Portable/Team", &policy, empty.node_id, key,
                                  &migrated) == 0 &&
           storage.puts == puts_before && vault_tree_count(migrated) == 2);
    const porthome_dir_node *migration_root = vault_tree_node_at(migrated, 0, &alpha_ring);
    assert(alpha_ring.count == 1 && alpha_ring.entries[0].epoch == 1 &&
           !memcmp(alpha_ring.entries[0].node_key, key, 32));
    assert(migration_root->entries[0].has_key_override &&
           !memcmp(migration_root->entries[0].key_override, key, 32));
    assert(nh_syncd_vault_lookup(migrated, "nested/old", 0, &beta, &alpha_ring) == 0 &&
           !memcmp(beta->chunks[0].sha256, v1_hash, 32));
    assert(nh_syncd_vault_read(migrated, "nested/old", bytes_len - 64, sample, 64, &io, &got) ==
               0 &&
           got == 64 && !memcmp(sample, bytes + bytes_len - 64, 64));
    assert(vault_tree_seal(migrated, &io, hash) == 0 && storage.puts == puts_before + 2);
    vault_tree_check_result checked;
    assert(vault_tree_check(migrated, &io, &checked) == 0 && checked.chunks == 1);
    /* Tracked type changes are reconciled deepest-first. Untracked children and
     * nonregular bootstrap destinations remain intact instead of being opened. */
    const porthome_entry_kind kinds[] = {PORTHOME_KIND_FILE, PORTHOME_KIND_DIR,
                                         PORTHOME_KIND_SYMLINK, PORTHOME_KIND_DIR,
                                         PORTHOME_KIND_FILE};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        vault_tree *variant = one_entry(&policy, &ring, kinds[i], v1_hash, bytes_len);
        assert(nh_syncd_vault_materialize(home, "Types", variant, state, &io) == 0);
        struct stat st;
        assert(fstatat(home, "Types/item", &st, AT_SYMLINK_NOFOLLOW) == 0);
        assert((kinds[i] == PORTHOME_KIND_FILE && S_ISREG(st.st_mode)) ||
               (kinds[i] == PORTHOME_KIND_DIR && S_ISDIR(st.st_mode)) ||
               (kinds[i] == PORTHOME_KIND_SYMLINK && S_ISLNK(st.st_mode)));
        if (i == 3) {
            int parent = openat(home, "Types/item", O_RDONLY | O_DIRECTORY);
            assert(parent >= 0);
            make_file(parent, "untracked", (const uint8_t *)"keep", 4);
            vault_tree *replacement =
                one_entry(&policy, &ring, PORTHOME_KIND_FILE, v1_hash, bytes_len);
            assert(nh_syncd_vault_materialize(home, "Types", replacement, state, &io) ==
                   VAULT_ERR_IO);
            assert(fstatat(parent, "untracked", &st, AT_SYMLINK_NOFOLLOW) == 0 && st.st_size == 4);
            assert(unlinkat(parent, "untracked", 0) == 0);
            close(parent);
            vault_tree_free(replacement);
        }
        vault_tree_free(variant);
    }
    assert(mkdirat(home, "Fifo", 0700) == 0);
    int fifo = openat(home, "Fifo", O_RDONLY | O_DIRECTORY);
    assert(fifo >= 0 && mkfifoat(fifo, "item", 0600) == 0);
    vault_tree *incoming = one_entry(&policy, &ring, PORTHOME_KIND_FILE, v1_hash, bytes_len);
    assert(nh_syncd_vault_materialize(home, "Fifo", incoming, state, &io) == VAULT_ERR_CONFLICT);
    struct stat fifo_stat;
    assert(fstatat(fifo, "item", &fifo_stat, AT_SYMLINK_NOFOLLOW) == 0 &&
           S_ISFIFO(fifo_stat.st_mode));
    close(fifo);
    vault_tree_free(incoming);
    assert(nh_syncd_vault_lookup(cold, "../alpha", 0, &beta, &alpha_ring) == VAULT_ERR_ARG);
    assert(symlinkat("/tmp", home, "escape") == 0);
    assert(nh_syncd_vault_materialize(home, "escape/Team", cold, state, &io) == VAULT_ERR_IO);
    nh_syncd_vault_scan race_scan = {.changed = swap_alpha_fifo, .changed_ctx = &source};
    vault_tree *raced = NULL;
    assert(nh_syncd_vault_capture(source, modified, &race_scan, &io, &raced) == VAULT_ERR_IO &&
           !raced);
    vault_tree_free(migrated);
    nh_syncd_state_free(legacy);
    vault_tree_free(modified);
    vault_tree_free(unchanged);
    vault_tree_free(rotated);
    vault_tree_free(cold);
    vault_record_free(record);
    nostr_event_free(event);
    porthome_head_dispose(head);
    free(head);
    vault_tree_free(captured);
    vault_tree_free(base);
    nh_syncd_state_free(state);
    for (size_t i = 0; i < storage.count; ++i)
        free(storage.objects[i].bytes);
    OPENSSL_cleanse(bytes, bytes_len);
    free(bytes);
    close(source);
    close(home);
    close(dir);
    assert(nftw(work, remove_owned, 32, FTW_DEPTH | FTW_PHYS) == 0);
    puts("OK vault capture/current epoch, CDC reads, cold apply, no-reseal v1 migration");
    return 0;
}
