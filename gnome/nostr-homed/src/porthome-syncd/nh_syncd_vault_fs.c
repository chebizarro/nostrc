/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "nh_syncd_vault_fs.h"
#include "nh_syncd_vault_config.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NV_MAX_NODES 4096u
#define NV_MAX_DEPTH 128u

typedef struct {
    porthome_dir_node *node;
    vault_ring ring;
} nv_node;
typedef struct {
    nv_node *nodes;
    size_t count;
    const vault_tree *base;
    const vault_blob_io *io;
    const nh_syncd_vault_scan *scan;
} nv_build;

static const porthome_dir_node *node_at_id(const vault_tree *tree, const uint8_t id[32],
                                           porthome_ring_t *ring)
{
    for (size_t i = 0; i < vault_tree_count(tree); ++i) {
        const porthome_dir_node *n = vault_tree_node_at(tree, i, ring);
        if (!memcmp(n->node_id, id, 32))
            return n;
    }
    return NULL;
}
static int safe_path(const char *path)
{
    if (!path || !*path || *path == '/' || strlen(path) >= PORTHOME_MAX_PATH_LEN)
        return 0;
    const char *p = path;
    while (*p) {
        size_t n = strcspn(p, "/");
        if (!n || n > 255 || (n == 1 && *p == '.') || (n == 2 && !memcmp(p, "..", 2)) ||
            memchr(p, '\\', n))
            return 0;
        p += n;
        if (*p && !*++p)
            return 0;
    }
    return 1;
}
int nh_syncd_vault_lookup(const vault_tree *tree, const char *path, int encrypted,
                          const porthome_node_entry **out, porthome_ring_t *ring)
{
    if (out)
        *out = NULL;
    if (!tree || !out || !ring || !safe_path(path))
        return VAULT_ERR_ARG;
    const porthome_dir_node *node = vault_tree_node_at(tree, 0, ring);
    const char *p = path;
    for (size_t depth = 0; node && depth < NV_MAX_DEPTH; ++depth) {
        size_t len = strcspn(p, "/");
        const porthome_node_entry *entry = NULL;
        for (size_t i = 0; i < node->entries_len; ++i) {
            const char *name = encrypted ? node->entries[i].name_enc : node->entries[i].name_plain;
            if (strlen(name) == len && !memcmp(name, p, len)) {
                entry = &node->entries[i];
                break;
            }
        }
        if (!entry)
            return VAULT_ERR_INCOMPLETE;
        if (!p[len]) {
            *out = entry;
            return 0;
        }
        if (entry->kind != PORTHOME_KIND_DIR)
            return VAULT_ERR_ARG;
        node = node_at_id(tree, entry->node_id, ring);
        p += len + 1;
    }
    return VAULT_ERR_INCOMPLETE;
}
int nh_syncd_vault_read(const vault_tree *tree, const char *path, uint64_t offset, uint8_t *buf,
                        size_t len, const vault_blob_io *io, size_t *read_len)
{
    if (read_len)
        *read_len = 0;
    if (!read_len || (!buf && len) || !io)
        return VAULT_ERR_ARG;
    const porthome_node_entry *entry = NULL;
    porthome_ring_t ring;
    int rc = nh_syncd_vault_lookup(tree, path, 0, &entry, &ring);
    if (rc)
        return rc;
    if (entry->kind != PORTHOME_KIND_FILE)
        return VAULT_ERR_ARG;
    if (offset >= entry->size_plain || !len)
        return 0;
    uint64_t start = 0;
    for (size_t i = 0; i < entry->chunks_len && *read_len < len; ++i) {
        const porthome_node_chunk *c = &entry->chunks[i];
        if (offset >= start + c->size_plain) {
            start += c->size_plain;
            continue;
        }
        uint8_t *plain = NULL;
        size_t n = 0;
        rc = vault_chunk_read(c, &ring, io, &plain, &n);
        if (rc) {
            if (*read_len)
                OPENSSL_cleanse(buf, *read_len);
            *read_len = 0;
            return rc;
        }
        size_t at = offset > start ? (size_t)(offset - start) : 0;
        size_t take = n - at;
        if (take > len - *read_len)
            take = len - *read_len;
        memcpy(buf + *read_len, plain + at, take);
        OPENSSL_cleanse(plain, n);
        free(plain);
        *read_len += take;
        offset += take;
        start += c->size_plain;
    }
    return 0;
}
static void build_free(nv_build *b)
{
    for (size_t i = 0; i < b->count; ++i) {
        porthome_dir_node_dispose(b->nodes[i].node);
        free(b->nodes[i].node);
        vault_ring_dispose(&b->nodes[i].ring);
    }
    free(b->nodes);
}
static int build_finish(nv_build *b, vault_tree **out)
{
    vault_tree_node *views = calloc(b->count, sizeof(*views));
    if (!views)
        return VAULT_ERR_OOM;
    for (size_t i = 0; i < b->count; ++i) {
        views[i].node = b->nodes[i].node;
        views[i].ring = vault_ring_view(&b->nodes[i].ring);
    }
    int rc = vault_tree_new(views, b->count, out);
    free(views);
    return rc;
}
static int add_node(nv_build *b, porthome_dir_node *node, const porthome_ring_t *ring)
{
    if (b->count == NV_MAX_NODES)
        return PORTHOME_ERR_TOO_LARGE;
    nv_node *nodes = realloc(b->nodes, (b->count + 1) * sizeof(*nodes));
    if (!nodes)
        return VAULT_ERR_OOM;
    b->nodes = nodes;
    nv_node *row = &nodes[b->count];
    memset(row, 0, sizeof(*row));
    int rc = vault_ring_copy(&row->ring, ring);
    if (rc)
        return rc;
    row->node = node;
    ++b->count;
    return 0;
}
static int clone_node(const porthome_dir_node *node, porthome_dir_node **out)
{
    uint8_t *bytes = NULL;
    size_t n = 0;
    int rc = porthome_dir_node_encode(node, &bytes, &n);
    if (!rc)
        rc = porthome_dir_node_decode(bytes, n, out);
    if (bytes)
        OPENSSL_cleanse(bytes, n);
    free(bytes);
    return rc;
}
static porthome_node_entry *append_entry(porthome_dir_node *node, const char *name)
{
    if (node->entries_len == PORTHOME_MAX_ENTRIES || strlen(name) > 255)
        return NULL;
    porthome_node_entry *entries =
        realloc(node->entries, (node->entries_len + 1) * sizeof(*entries));
    if (!entries)
        return NULL;
    node->entries = entries;
    porthome_node_entry *e = &entries[node->entries_len++];
    memset(e, 0, sizeof(*e));
    memcpy(e->name_plain, name, strlen(name) + 1);
    return e;
}
static uint64_t mtime_ns(const struct stat *s)
{
#ifdef __APPLE__
    return (uint64_t)s->st_mtimespec.tv_sec * 1000000000u + s->st_mtimespec.tv_nsec;
#else
    return (uint64_t)s->st_mtim.tv_sec * 1000000000u + s->st_mtim.tv_nsec;
#endif
}
static int stable(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
           a->st_mode == b->st_mode && mtime_ns(a) == mtime_ns(b) && a->st_ctime == b->st_ctime;
}
static int capture_file(int fd, const struct stat *before, porthome_node_entry *e,
                        const porthome_ring_entry_t *epoch, const vault_blob_io *io)
{
    uint8_t *buffer = malloc(PORTHOME_CHUNK_MAX);
    if (!buffer)
        return VAULT_ERR_OOM;
    size_t filled = 0;
    int eof = 0, rc = 0;
    uint64_t total = 0;
    while (!eof || filled) {
        while (!eof && filled < PORTHOME_CHUNK_MAX) {
            ssize_t got = read(fd, buffer + filled, PORTHOME_CHUNK_MAX - filled);
            if (got < 0 && errno == EINTR)
                continue;
            if (got < 0) {
                rc = VAULT_ERR_IO;
                goto done;
            }
            if (!got)
                eof = 1;
            else
                filled += (size_t)got;
        }
        if (!filled)
            break;
        if (e->chunks_len == PORTHOME_NODE_MAX_CHUNKS) {
            rc = PORTHOME_ERR_TOO_LARGE;
            goto done;
        }
        size_t cut = porthome_chunk_next(buffer, filled);
        porthome_node_chunk *chunks = realloc(e->chunks, (e->chunks_len + 1) * sizeof(*chunks));
        if (!chunks) {
            rc = VAULT_ERR_OOM;
            goto done;
        }
        e->chunks = chunks;
        porthome_node_chunk *c = &chunks[e->chunks_len];
        memset(c, 0, sizeof(*c));
        uint8_t *sealed = NULL;
        size_t n = 0;
        rc = porthome_encrypt_chunk_v2(epoch->node_key, buffer, cut, &sealed, &n, c->sha256);
        if (!rc && io->put(io->ctx, sealed, n, c->sha256))
            rc = VAULT_ERR_IO;
        free(sealed);
        if (rc)
            goto done;
        c->size_plain = cut;
        c->size_sealed = n;
        c->chunk_key_id = epoch->epoch;
        ++e->chunks_len;
        total += cut;
        filled -= cut;
        memmove(buffer, buffer + cut, filled);
    }
    struct stat after;
    if (fstat(fd, &after) || !stable(before, &after) || total != (uint64_t)before->st_size)
        rc = VAULT_ERR_AGAIN;
    e->size_plain = total;
done:
    OPENSSL_cleanse(buffer, PORTHOME_CHUNK_MAX);
    free(buffer);
    return rc;
}
static int entry_compare(const void *a, const void *b)
{
    return strcmp(((const porthome_node_entry *)a)->name_plain,
                  ((const porthome_node_entry *)b)->name_plain);
}
static int capture_dir(nv_build *b, int fd, const porthome_dir_node *previous,
                       const porthome_ring_t *ring, const char *prefix, size_t depth)
{
    if (depth >= NV_MAX_DEPTH)
        return PORTHOME_ERR_TOO_LARGE;
    porthome_dir_node *node = NULL;
    int rc = clone_node(previous, &node);
    if (rc)
        return rc;
    porthome_dir_node old = {.entries = node->entries, .entries_len = node->entries_len};
    node->entries = NULL;
    node->entries_len = 0;
    rc = add_node(b, node, ring);
    if (rc) {
        porthome_dir_node_dispose(node);
        free(node);
        porthome_dir_node_dispose(&old);
        return rc;
    }
    int scan_fd = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *dir = scan_fd < 0 ? NULL : fdopendir(scan_fd);
    if (!dir) {
        if (scan_fd >= 0)
            close(scan_fd);
        porthome_dir_node_dispose(&old);
        return VAULT_ERR_IO;
    }
    struct stat before, after;
    if (fstat(fd, &before)) {
        rc = VAULT_ERR_IO;
        goto done;
    }
    const porthome_ring_entry_t *epoch = &ring->entries[ring->count - 1];
    struct dirent *de;
    errno = 0;
    while ((de = readdir(dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        struct stat st;
        if (fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
            rc = VAULT_ERR_IO;
            break;
        }
        char relative[PORTHOME_MAX_PATH_LEN], home_relative[PORTHOME_MAX_PATH_LEN];
        int count =
            snprintf(relative, sizeof(relative), "%s%s%s", prefix, *prefix ? "/" : "", de->d_name);
        if (count < 0 || (size_t)count >= sizeof(relative)) {
            rc = PORTHOME_ERR_TOO_LARGE;
            break;
        }
        if (b->scan) {
            const char *home_prefix = b->scan->home_prefix ? b->scan->home_prefix : "";
            count = snprintf(home_relative, sizeof(home_relative), "%s%s%s", home_prefix,
                             *home_prefix ? "/" : "", relative);
            if (count < 0 || (size_t)count >= sizeof(home_relative)) {
                rc = PORTHOME_ERR_TOO_LARGE;
                break;
            }
            if (nh_syncd_vault_conflict_path(home_relative) ||
                (b->scan->ignore && nh_syncd_ignore_check(b->scan->ignore, home_relative, st.st_dev, st.st_mode) !=
                NH_SYNCD_IGNORE_PASS)) {
                errno = 0;
                continue;
            }
        }
        int changed =
            b->scan && b->scan->changed && b->scan->changed(b->scan->changed_ctx, relative);
        porthome_node_entry *e = append_entry(node, de->d_name);
        if (!e) {
            rc = VAULT_ERR_OOM;
            break;
        }
        porthome_node_entry *prior = NULL;
        for (size_t i = 0; i < old.entries_len; ++i)
            if (!strcmp(old.entries[i].name_plain, de->d_name)) {
                prior = &old.entries[i];
                break;
            }
        if (!changed && S_ISREG(st.st_mode) && prior && prior->kind == PORTHOME_KIND_FILE &&
            prior->size_plain == (uint64_t)st.st_size && prior->mtime_ns == mtime_ns(&st) &&
            prior->mode == (uint32_t)st.st_mode) {
            *e = *prior;
            memset(prior, 0, sizeof(*prior));
        } else if (S_ISREG(st.st_mode)) {
            int file = openat(fd, de->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
            struct stat opened;
            if (file < 0 || fstat(file, &opened) || !stable(&st, &opened))
                rc = VAULT_ERR_IO;
            else {
                e->kind = PORTHOME_KIND_FILE;
                rc = capture_file(file, &opened, e, epoch, b->io);
            }
            if (file >= 0)
                close(file);
        } else if (S_ISDIR(st.st_mode)) {
            e->kind = PORTHOME_KIND_DIR;
            porthome_dir_node empty = {.epoch = 1};
            porthome_ring_entry_t fresh = {.epoch = 1};
            porthome_ring_t child_ring = {&fresh, 1};
            const porthome_dir_node *child = NULL;
            if (prior && prior->kind == PORTHOME_KIND_DIR) {
                *e = *prior;
                memset(prior, 0, sizeof(*prior));
                child = node_at_id(b->base, e->node_id, &child_ring);
            } else {
                if (RAND_bytes(empty.node_id, 32) != 1)
                    rc = PORTHOME_ERR_CRYPTO;
                if (!rc)
                    rc = porthome_child_node_key(epoch->node_key, empty.node_id, NULL,
                                                 fresh.node_key);
                memcpy(e->node_id, empty.node_id, 32);
                e->child_epoch = 1;
                child = &empty;
            }
            int sub = openat(fd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (!rc)
                rc = sub < 0 || !child
                         ? VAULT_ERR_IO
                         : capture_dir(b, sub, child, &child_ring, relative, depth + 1);
            if (sub >= 0)
                close(sub);
            OPENSSL_cleanse(fresh.node_key, 32);
        } else if (S_ISLNK(st.st_mode)) {
            e->kind = PORTHOME_KIND_SYMLINK;
            char target[PORTHOME_MAX_SYMLINK_LEN + 1];
            ssize_t n = readlinkat(fd, de->d_name, target, sizeof(target));
            if (n <= 0 || n >= (ssize_t)sizeof(target))
                rc = VAULT_ERR_ARG;
            else {
                target[n] = 0;
                e->symlink_target = strdup(target);
                if (!e->symlink_target)
                    rc = VAULT_ERR_OOM;
            }
        } else
            rc = VAULT_ERR_ARG;
        if (rc)
            break;
        e->mode = st.st_mode;
        e->mtime_ns = mtime_ns(&st);
        rc = porthome_encrypt_name_v2(epoch->node_key, e->name_plain, e->name_enc);
        if (rc)
            break;
        errno = 0;
    }
    if (!rc && errno)
        rc = VAULT_ERR_IO;
    if (!rc && (fstat(fd, &after) || !stable(&before, &after)))
        rc = VAULT_ERR_AGAIN;
    if (!rc && node->entries_len > 1)
        qsort(node->entries, node->entries_len, sizeof(*node->entries), entry_compare);
done:
    closedir(dir);
    porthome_dir_node_dispose(&old);
    return rc;
}
int nh_syncd_vault_capture(int fd, const vault_tree *base, const nh_syncd_vault_scan *scan,
                           const vault_blob_io *io, vault_tree **out)
{
    if (out)
        *out = NULL;
    if (fd < 0 || !base || !io || !io->put || !out)
        return VAULT_ERR_ARG;
    porthome_ring_t ring;
    const porthome_dir_node *root = vault_tree_node_at(base, 0, &ring);
    if (!root)
        return VAULT_ERR_ARG;
    nv_build b = {.base = base, .io = io, .scan = scan};
    int rc = capture_dir(&b, fd, root, &ring, "", 0);
    if (!rc)
        rc = build_finish(&b, out);
    build_free(&b);
    return rc;
}

static porthome_dir_node *build_node(nv_build *b, const uint8_t id[32])
{
    for (size_t i = 0; i < b->count; ++i)
        if (!memcmp(b->nodes[i].node->node_id, id, 32))
            return b->nodes[i].node;
    return NULL;
}
static int migrate_dirs(nv_build *b, const char *path, const uint8_t key[32],
                        porthome_dir_node **parent, char name[256])
{
    char copy[PORTHOME_MAX_PATH_LEN];
    if (!safe_path(path))
        return VAULT_ERR_ARG;
    memcpy(copy, path, strlen(path) + 1);
    char *last = strrchr(copy, '/');
    memcpy(name, last ? last + 1 : copy, strlen(last ? last + 1 : copy) + 1);
    *parent = b->nodes[0].node;
    if (!last)
        return 0;
    *last = 0;
    char *save = NULL;
    size_t depth = 0;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (++depth >= NV_MAX_DEPTH)
            return PORTHOME_ERR_TOO_LARGE;
        porthome_node_entry *dir = NULL;
        for (size_t i = 0; i < (*parent)->entries_len; ++i)
            if (!strcmp((*parent)->entries[i].name_plain, part)) {
                dir = &(*parent)->entries[i];
                break;
            }
        if (!dir) {
            porthome_dir_node *child = calloc(1, sizeof(*child));
            if (!child)
                return VAULT_ERR_OOM;
            child->epoch = 1;
            int rc = RAND_bytes(child->node_id, 32) == 1 ? 0 : PORTHOME_ERR_CRYPTO;
            porthome_ring_entry_t item = {.epoch = 1};
            memcpy(item.node_key, key, 32);
            porthome_ring_t ring = {&item, 1};
            if (!rc)
                rc = add_node(b, child, &ring);
            OPENSSL_cleanse(item.node_key, 32);
            if (rc) {
                free(child);
                return rc;
            }
            dir = append_entry(*parent, part);
            if (!dir)
                return VAULT_ERR_OOM;
            dir->kind = PORTHOME_KIND_DIR;
            dir->mode = S_IFDIR | 0700;
            memcpy(dir->node_id, child->node_id, 32);
            memcpy(dir->key_override, key, 32);
            dir->has_key_override = 1;
            dir->child_epoch = 1;
            rc = porthome_encrypt_name_v2(key, part, dir->name_enc);
            if (rc)
                return rc;
        }
        if (dir->kind != PORTHOME_KIND_DIR)
            return VAULT_ERR_CONFLICT;
        *parent = build_node(b, dir->node_id);
        if (!*parent)
            return VAULT_ERR_INCOMPLETE;
    }
    return 0;
}
int nh_syncd_vault_migrate(const nh_syncd_state *state, const char *prefix,
                           const porthome_vault_info *policy, const uint8_t root_id[32],
                           const uint8_t key[32], vault_tree **out)
{
    if (out)
        *out = NULL;
    if (!state || !prefix || (*prefix && !safe_path(prefix)) || !policy || !root_id || !key || !out)
        return VAULT_ERR_ARG;
    porthome_dir_node initial = {.epoch = 1, .vault = (porthome_vault_info *)policy};
    memcpy(initial.node_id, root_id, 32);
    porthome_dir_node *root = NULL;
    int rc = clone_node(&initial, &root);
    if (rc)
        return rc;
    nv_build b = {0};
    porthome_ring_entry_t item = {.epoch = 1};
    memcpy(item.node_key, key, 32);
    porthome_ring_t ring = {&item, 1};
    rc = add_node(&b, root, &ring);
    OPENSSL_cleanse(item.node_key, 32);
    if (rc) {
        porthome_dir_node_dispose(root);
        free(root);
        return rc;
    }
    size_t prefix_len = strlen(prefix);
    for (size_t i = 0; !rc && i < nh_syncd_state_file_count(state); ++i) {
        const char *rel = NULL;
        const nh_syncd_entry *old = nh_syncd_state_at(state, i, &rel);
        if (!old || !rel) {
            rc = VAULT_ERR_ARG;
            break;
        }
        if (nh_syncd_vault_conflict_path(rel)) continue;
        if (prefix_len) {
            if (strncmp(rel, prefix, prefix_len) || rel[prefix_len] != '/')
                continue;
            rel += prefix_len + 1;
        }
        porthome_dir_node *parent = NULL;
        char name[256];
        rc = migrate_dirs(&b, rel, key, &parent, name);
        if (rc)
            break;
        porthome_node_entry *entry = NULL;
        for (size_t j = 0; j < parent->entries_len; ++j)
            if (!strcmp(parent->entries[j].name_plain, name)) {
                entry = &parent->entries[j];
                break;
            }
        nh_syncd_kind kind = nh_syncd_entry_kind(old);
        if (kind == NH_SYNCD_KIND_DIR) {
            char child[PORTHOME_MAX_PATH_LEN];
            int n = snprintf(child, sizeof(child), "%s/_", rel);
            if (n < 0 || (size_t)n >= sizeof(child)) {
                rc = PORTHOME_ERR_TOO_LARGE;
                break;
            }
            porthome_dir_node *unused;
            char unused_name[256];
            rc = migrate_dirs(&b, child, key, &unused, unused_name);
            if (rc)
                break;
            for (size_t j = 0; j < parent->entries_len; ++j)
                if (!strcmp(parent->entries[j].name_plain, name)) {
                    entry = &parent->entries[j];
                    break;
                }
        } else {
            if (entry) {
                rc = VAULT_ERR_CONFLICT;
                break;
            }
            entry = append_entry(parent, name);
            if (!entry) {
                rc = VAULT_ERR_OOM;
                break;
            }
            if (kind == NH_SYNCD_KIND_SYMLINK) {
                entry->kind = PORTHOME_KIND_SYMLINK;
                entry->symlink_target = strdup(nh_syncd_entry_symlink_target(old));
                if (!entry->symlink_target) {
                    rc = VAULT_ERR_OOM;
                    break;
                }
            } else if (kind == NH_SYNCD_KIND_FILE) {
                entry->kind = PORTHOME_KIND_FILE;
                entry->size_plain = nh_syncd_entry_size(old);
                size_t n = nh_syncd_entry_chunk_count(old);
                if (n != entry->size_plain / NH_SYNCD_CHUNK_SIZE +
                             (entry->size_plain % NH_SYNCD_CHUNK_SIZE != 0)) {
                    rc = VAULT_ERR_ARG;
                    break;
                }
                entry->chunks = n ? calloc(n, sizeof(*entry->chunks)) : NULL;
                if (n && !entry->chunks) {
                    rc = VAULT_ERR_OOM;
                    break;
                }
                entry->chunks_len = n;
                uint64_t remaining = entry->size_plain;
                for (size_t j = 0; j < n; ++j) {
                    const char *hex = nh_syncd_entry_chunk_at(old, j);
                    porthome_node_chunk *c = &entry->chunks[j];
                    if (porthome_from_hex64(hex, c->sha256)) {
                        rc = VAULT_ERR_ARG;
                        break;
                    }
                    c->size_plain =
                        remaining > NH_SYNCD_CHUNK_SIZE ? NH_SYNCD_CHUNK_SIZE : remaining;
                    c->size_sealed = c->size_plain + PORTHOME_SEAL_OVERHEAD;
                    c->chunk_key_id = 1;
                    remaining -= c->size_plain;
                }
            } else {
                rc = VAULT_ERR_ARG;
                break;
            }
        }
        if (!rc) {
            entry->mode = nh_syncd_entry_mode(old);
            entry->mtime_ns = nh_syncd_entry_mtime_ns(old);
            rc = porthome_encrypt_name_v2(key, entry->name_plain, entry->name_enc);
        }
    }
    if (!rc)
        rc = build_finish(&b, out);
    build_free(&b);
    return rc;
}
