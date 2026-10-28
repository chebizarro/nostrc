/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "nh_syncd_vault_fs.h"
#include "nh_syncd_vault_config.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Existing snapshot mutation seam also used by the legacy pusher/reconciler. */
int nh_syncd_state_upsert_file_(nh_syncd_state *, const char *, uint32_t, uint32_t, uint32_t,
                                uint64_t, uint64_t, const char *, const char *const *, size_t);
int nh_syncd_state_upsert_dir_(nh_syncd_state *, const char *, uint32_t, uint32_t, uint32_t,
                               uint64_t);
int nh_syncd_state_upsert_symlink_(nh_syncd_state *, const char *, uint32_t, uint32_t, uint32_t,
                                   uint64_t, const char *);
int nh_syncd_state_delete_(nh_syncd_state *, const char *);

static int component(const char *p, size_t n)
{
    return n && n <= 255 && !(n == 1 && *p == '.') && !(n == 2 && !memcmp(p, "..", 2)) &&
           !memchr(p, '\\', n);
}
typedef struct {
    int fd;
    dev_t device;
    ino_t inode;
    mode_t original, final;
} permission;
typedef struct {
    permission *items;
    size_t count;
} permissions;

static int chmod_fd(int fd, mode_t mode)
{
    if (!fchmod(fd, mode))
        return 0;
#ifdef __linux__
    /* O_PATH pins an owned directory even when mode 0000 prevents a read-open.
     * This is the kernel's fd link, never a path supplied by a manifest. */
    if (errno == EBADF) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        if (!chmod(path, mode))
            return 0;
    }
#endif
    return VAULT_ERR_IO;
}
static int permit(permissions *guard, int fd, const char *rel, nh_syncd_state *state)
{
    struct stat st;
    if (fstat(fd, &st) || !S_ISDIR(st.st_mode))
        return VAULT_ERR_IO;
    if ((st.st_mode & 0700) == 0700)
        return 0;
    const nh_syncd_entry *entry = nh_syncd_state_find(state, rel);
    if (!entry || nh_syncd_entry_kind(entry) != NH_SYNCD_KIND_DIR || st.st_uid != geteuid())
        return VAULT_ERR_CONFLICT;
    int owned = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (owned < 0)
        return VAULT_ERR_IO;
    permission *items = realloc(guard->items, (guard->count + 1) * sizeof(*items));
    if (!items) {
        close(owned);
        return VAULT_ERR_OOM;
    }
    guard->items = items;
    permission *p = &items[guard->count++];
    *p = (permission){owned, st.st_dev, st.st_ino, st.st_mode & 0777, st.st_mode & 0777};
    return chmod_fd(fd, (st.st_mode & 0777) | 0700);
}
static void requested_mode(permissions *guard, int fd, mode_t mode)
{
    struct stat st;
    if (fstat(fd, &st))
        return;
    for (size_t i = 0; i < guard->count; ++i)
        if (guard->items[i].device == st.st_dev && guard->items[i].inode == st.st_ino)
            guard->items[i].final = mode;
}
static int restore_permissions(permissions *guard, int success)
{
    int rc = 0;
    for (size_t i = guard->count; i > 0; --i) {
        permission *p = &guard->items[i - 1];
        if (chmod_fd(p->fd, success ? p->final : p->original))
            rc = VAULT_ERR_IO;
        close(p->fd);
    }
    free(guard->items);
    return rc;
}
static int directory(int fd, const char *path, int make, const char *base, nh_syncd_state *state,
                     permissions *guard)
{
    if (!path || *path == '/' || strlen(path) >= PORTHOME_MAX_PATH_LEN)
        return -1;
    int current = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    const char *p = path;
    char relative[PORTHOME_MAX_PATH_LEN];
    if (strlen(base) >= sizeof(relative)) {
        if (current >= 0)
            close(current);
        return -1;
    }
    memcpy(relative, base, strlen(base) + 1);
    while (current >= 0 && *p) {
        size_t n = strcspn(p, "/");
        if (!component(p, n)) {
            close(current);
            return -1;
        }
        char name[256];
        memcpy(name, p, n);
        name[n] = 0;
        size_t used = strlen(relative);
        int z = snprintf(relative + used, sizeof(relative) - used, "%s%s", used ? "/" : "", name);
        if (z < 0 || (size_t)z >= sizeof(relative) - used) {
            close(current);
            return -1;
        }
        int next = openat(current, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
#ifdef __linux__
        if (next < 0 && errno == EACCES) {
            int pinned = openat(current, name, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (pinned >= 0) {
                if (!permit(guard, pinned, relative, state))
                    next = openat(pinned, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                close(pinned);
            }
            if (next < 0) {
                close(current);
                return -1;
            }
        }
#endif
        if (next < 0 && errno == ENOENT && make) {
            if (mkdirat(current, name, 0700) && errno != EEXIST) {
                close(current);
                return -1;
            }
            next = openat(current, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (next >= 0 && permit(guard, next, relative, state)) {
            close(next);
            next = -1;
        }
        close(current);
        current = next;
        p += n;
        if (*p) {
            ++p;
            if (!*p) {
                if (current >= 0)
                    close(current);
                return -1;
            }
        }
    }
    return current;
}
static int join(const char *a, const char *b, char out[PORTHOME_MAX_PATH_LEN])
{
    int n = snprintf(out, PORTHOME_MAX_PATH_LEN, "%s%s%s", a, *a ? "/" : "", b);
    return n < 0 || n >= (int)PORTHOME_MAX_PATH_LEN ? PORTHOME_ERR_TOO_LARGE : 0;
}
static const porthome_dir_node *find_node(const vault_tree *tree, const uint8_t id[32],
                                          porthome_ring_t *ring)
{
    for (size_t i = 0; i < vault_tree_count(tree); ++i) {
        const porthome_dir_node *node = vault_tree_node_at(tree, i, ring);
        if (!memcmp(id, node->node_id, 32))
            return node;
    }
    return NULL;
}
static int write_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t written = write(fd, p, n);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return VAULT_ERR_IO;
        p += written;
        n -= (size_t)written;
    }
    return 0;
}
static int times(int fd, uint64_t ns)
{
    struct timespec t[2] = {{.tv_nsec = UTIME_OMIT},
                            {.tv_sec = (time_t)(ns / 1000000000u), .tv_nsec = ns % 1000000000u}};
    return futimens(fd, t) ? VAULT_ERR_IO : 0;
}
static int hash_file(int fd, uint8_t hash[32])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    uint8_t *buffer = malloc(65536);
    if (!ctx || !buffer) {
        EVP_MD_CTX_free(ctx);
        free(buffer);
        return VAULT_ERR_OOM;
    }
    int rc = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 ? 0 : PORTHOME_ERR_CRYPTO;
    while (!rc) {
        ssize_t n = read(fd, buffer, 65536);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            rc = VAULT_ERR_IO;
            break;
        }
        if (!n)
            break;
        if (EVP_DigestUpdate(ctx, buffer, (size_t)n) != 1)
            rc = PORTHOME_ERR_CRYPTO;
    }
    unsigned n = 0;
    if (!rc && (EVP_DigestFinal_ex(ctx, hash, &n) != 1 || n != 32))
        rc = PORTHOME_ERR_CRYPTO;
    OPENSSL_cleanse(buffer, 65536);
    free(buffer);
    EVP_MD_CTX_free(ctx);
    return rc;
}
static int apply_file(int fd, const char *rel, const porthome_node_entry *entry,
                      const porthome_ring_t *ring, nh_syncd_state *state, const vault_blob_io *io)
{
    uint8_t random[16];
    char tmp[48] = ".vault-";
    if (RAND_bytes(random, sizeof(random)) != 1)
        return PORTHOME_ERR_CRYPTO;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(random); ++i) {
        tmp[7 + i * 2] = hex[random[i] >> 4];
        tmp[8 + i * 2] = hex[random[i] & 15];
    }
    tmp[39] = 0;
    int file = openat(fd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (file < 0)
        return VAULT_ERR_IO;
    EVP_MD_CTX *digest = EVP_MD_CTX_new();
    int rc = digest ? 0 : VAULT_ERR_OOM;
    if (!rc && EVP_DigestInit_ex(digest, EVP_sha256(), NULL) != 1)
        rc = PORTHOME_ERR_CRYPTO;
    uint64_t total = 0;
    for (size_t i = 0; !rc && i < entry->chunks_len; ++i) {
        uint8_t *plain = NULL;
        size_t n = 0;
        rc = vault_chunk_read(&entry->chunks[i], ring, io, &plain, &n);
        if (!rc)
            rc = write_all(file, plain, n);
        if (!rc && EVP_DigestUpdate(digest, plain, n) != 1)
            rc = PORTHOME_ERR_CRYPTO;
        if (plain)
            OPENSSL_cleanse(plain, n);
        free(plain);
        total += n;
    }
    uint8_t hash[32];
    unsigned hash_len = 0;
    if (!rc && (total != entry->size_plain || EVP_DigestFinal_ex(digest, hash, &hash_len) != 1 ||
                hash_len != 32))
        rc = VAULT_ERR_ARG;
    EVP_MD_CTX_free(digest);
    if (!rc && (fchmod(file, entry->mode & 0777) || times(file, entry->mtime_ns) || fsync(file)))
        rc = VAULT_ERR_IO;
    close(file);
    /* Do not destroy an untracked local file during initial member bootstrap.
     * An identical prior partial materialisation may be adopted safely. */
    if (!rc && !nh_syncd_state_find(state, rel)) {
        struct stat existing;
        if (fstatat(fd, entry->name_plain, &existing, AT_SYMLINK_NOFOLLOW) == 0) {
            uint8_t local[32];
            if (!S_ISREG(existing.st_mode))
                rc = VAULT_ERR_CONFLICT;
            else {
                int old =
                    openat(fd, entry->name_plain, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
                struct stat opened;
                if (old < 0 || fstat(old, &opened) || !S_ISREG(opened.st_mode) ||
                    opened.st_dev != existing.st_dev || opened.st_ino != existing.st_ino)
                    rc = VAULT_ERR_CONFLICT;
                else
                    rc = hash_file(old, local);
                if (old >= 0)
                    close(old);
                if (!rc && CRYPTO_memcmp(local, hash, 32))
                    rc = VAULT_ERR_CONFLICT;
            }
        } else if (errno != ENOENT)
            rc = VAULT_ERR_IO;
    }
    if (!rc && (renameat(fd, tmp, fd, entry->name_plain) || fsync(fd)))
        rc = VAULT_ERR_IO;
    if (rc) {
        unlinkat(fd, tmp, 0);
        return rc;
    }
    char hash_hex[65];
    porthome_hex64(hash, hash_hex);
    char **chunks = entry->chunks_len ? calloc(entry->chunks_len, sizeof(*chunks)) : NULL;
    if (entry->chunks_len && !chunks)
        return VAULT_ERR_OOM;
    for (size_t i = 0; i < entry->chunks_len; ++i) {
        chunks[i] = malloc(65);
        if (!chunks[i]) {
            rc = VAULT_ERR_OOM;
            break;
        }
        porthome_hex64(entry->chunks[i].sha256, chunks[i]);
    }
    if (!rc)
        rc = nh_syncd_state_upsert_file_(state, rel, S_IFREG | (entry->mode & 0777), getuid(),
                                         getgid(), entry->mtime_ns, entry->size_plain, hash_hex,
                                         (const char *const *)chunks, entry->chunks_len);
    for (size_t i = 0; i < entry->chunks_len; ++i)
        free(chunks[i]);
    free(chunks);
    return rc;
}
static int apply_node(int fd, const char *prefix, const vault_tree *tree,
                      const porthome_dir_node *node, const porthome_ring_t *ring,
                      nh_syncd_state *state, const vault_blob_io *io, permissions *guard,
                      size_t depth)
{
    if (depth >= 128)
        return PORTHOME_ERR_TOO_LARGE;
    for (size_t i = 0; i < node->entries_len; ++i) {
        const porthome_node_entry *entry = &node->entries[i];
        char rel[PORTHOME_MAX_PATH_LEN];
        int rc = join(prefix, entry->name_plain, rel);
        if (rc)
            return rc;
        if (entry->kind == PORTHOME_KIND_FILE)
            rc = apply_file(fd, rel, entry, ring, state, io);
        else if (entry->kind == PORTHOME_KIND_DIR) {
            int child_fd = directory(fd, entry->name_plain, 1, prefix, state, guard);
            porthome_ring_t child_ring;
            const porthome_dir_node *child = find_node(tree, entry->node_id, &child_ring);
            rc = child_fd < 0 || !child ? VAULT_ERR_IO
                                        : apply_node(child_fd, rel, tree, child, &child_ring, state,
                                                     io, guard, depth + 1);
            if (!rc && (fchmod(child_fd, entry->mode & 0777) || times(child_fd, entry->mtime_ns) ||
                        fsync(child_fd)))
                rc = VAULT_ERR_IO;
            if (!rc)
                requested_mode(guard, child_fd, entry->mode & 0777);
            if (child_fd >= 0)
                close(child_fd);
            if (!rc)
                rc = nh_syncd_state_upsert_dir_(state, rel, S_IFDIR | (entry->mode & 0777),
                                                getuid(), getgid(), entry->mtime_ns);
        } else if (entry->kind == PORTHOME_KIND_SYMLINK) {
            struct stat st;
            if (fstatat(fd, entry->name_plain, &st, AT_SYMLINK_NOFOLLOW) == 0) {
                char target[PORTHOME_MAX_SYMLINK_LEN + 1];
                ssize_t n = readlinkat(fd, entry->name_plain, target, sizeof(target) - 1);
                if (n < 0)
                    return VAULT_ERR_CONFLICT;
                target[n] = 0;
                if (strcmp(target, entry->symlink_target)) {
                    if (!nh_syncd_state_find(state, rel))
                        return VAULT_ERR_CONFLICT;
                    if (unlinkat(fd, entry->name_plain, 0))
                        return VAULT_ERR_IO;
                } else
                    goto symlink_row;
            } else if (errno != ENOENT)
                return VAULT_ERR_IO;
            if (symlinkat(entry->symlink_target, fd, entry->name_plain) || fsync(fd))
                return VAULT_ERR_IO;
        symlink_row:
            rc =
                nh_syncd_state_upsert_symlink_(state, rel, S_IFLNK | (entry->mode & 0777), getuid(),
                                               getgid(), entry->mtime_ns, entry->symlink_target);
        } else
            rc = VAULT_ERR_ARG;
        if (rc)
            return rc;
    }
    return 0;
}
static int reverse_path(const void *a, const void *b)
{
    return -strcmp(*(const char *const *)a, *(const char *const *)b);
}
static int mapped_lookup(const vault_tree *tree, const char *source_prefix, const char *relative,
                         const porthome_node_entry **entry, porthome_ring_t *ring)
{
    if (!tree) return VAULT_ERR_INCOMPLETE;
    char path[PORTHOME_MAX_PATH_LEN];
    if (source_prefix) {
        int rc = join(source_prefix, relative, path); if (rc) return rc;
        relative = path;
    }
    return nh_syncd_vault_lookup(tree, relative, 0, entry, ring);
}
static int remove_absent(int fd, const char *prefix, const vault_tree *tree, const char *source_prefix,
                         nh_syncd_state *state, permissions *guard)
{
    size_t n = 0;
    char **paths = NULL;
    size_t prefix_len = strlen(prefix);
    int rc = 0;
    for (size_t i = 0; i < nh_syncd_state_file_count(state); ++i) {
        const char *path = NULL;
        const nh_syncd_entry *old = nh_syncd_state_at(state, i, &path);
        if (!old || !path) {
            rc = VAULT_ERR_ARG;
            break;
        }
        if (!nh_syncd_vault_conflict_path(prefix) && nh_syncd_vault_conflict_path(path)) continue;
        const char *relative = path;
        if (prefix_len) {
            if (strncmp(path, prefix, prefix_len) || path[prefix_len] != '/')
                continue;
            relative += prefix_len + 1;
        }
        nh_syncd_kind old_kind = nh_syncd_entry_kind(old);
        const porthome_node_entry *entry;
        porthome_ring_t ring;
        int found = mapped_lookup(tree, source_prefix, relative, &entry, &ring);
        if (!found && (int)entry->kind == (int)old_kind)
            continue;
        if (found && found != VAULT_ERR_INCOMPLETE && found != VAULT_ERR_ARG) {
            rc = found;
            break;
        }
        char **next = realloc(paths, (n + 1) * sizeof(*paths));
        if (!next) {
            rc = VAULT_ERR_OOM;
            break;
        }
        paths = next;
        paths[n] = strdup(path);
        if (!paths[n]) {
            rc = VAULT_ERR_OOM;
            break;
        }
        ++n;
    }
    if (!rc && n > 1)
        qsort(paths, n, sizeof(*paths), reverse_path);
    for (size_t i = 0; !rc && i < n; ++i) {
        char parent[PORTHOME_MAX_PATH_LEN];
        if (strlen(paths[i]) >= sizeof(parent)) {
            rc = VAULT_ERR_ARG;
            break;
        }
        memcpy(parent, paths[i], strlen(paths[i]) + 1);
        char *name = strrchr(parent, '/');
        if (name)
            *name++ = 0;
        else {
            name = parent;
        }
        int dir = name == parent ? fcntl(fd, F_DUPFD_CLOEXEC, 0)
                                 : directory(fd, parent, 0, "", state, guard);
        struct stat st;
        if (dir < 0) {
            rc = VAULT_ERR_IO;
            break;
        }
        if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            if (unlinkat(dir, name, S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0))
                rc = VAULT_ERR_IO;
            if (!rc && fsync(dir))
                rc = VAULT_ERR_IO;
        } else if (errno != ENOENT)
            rc = VAULT_ERR_IO;
        close(dir);
        if (!rc)
            rc = nh_syncd_state_delete_(state, paths[i]);
    }
    for (size_t i = 0; i < n; ++i)
        free(paths[i]);
    free(paths);
    return rc;
}
static int inventory_node(const char *prefix, const vault_tree *tree, const porthome_dir_node *node,
                           const porthome_ring_t *ring, nh_syncd_state *state, const vault_blob_io *io, size_t depth)
{
    if (depth >= 128) return PORTHOME_ERR_TOO_LARGE;
    for (size_t i = 0; i < node->entries_len; ++i) {
        const porthome_node_entry *entry = &node->entries[i];
        char path[PORTHOME_MAX_PATH_LEN];
        int rc = join(prefix, entry->name_plain, path);
        if (rc) return rc;
        if (entry->kind == PORTHOME_KIND_DIR) {
            rc = nh_syncd_state_upsert_dir_(state, path, S_IFDIR | (entry->mode & 0777), getuid(), getgid(), entry->mtime_ns);
            porthome_ring_t child_ring;
            const porthome_dir_node *child = find_node(tree, entry->node_id, &child_ring);
            if (!rc) rc = child ? inventory_node(path, tree, child, &child_ring, state, io, depth + 1) : VAULT_ERR_INCOMPLETE;
        } else if (entry->kind == PORTHOME_KIND_SYMLINK) {
            rc = nh_syncd_state_upsert_symlink_(state, path, S_IFLNK | (entry->mode & 0777), getuid(), getgid(),
                entry->mtime_ns, entry->symlink_target);
        } else {
            char **chunks = calloc(entry->chunks_len ? entry->chunks_len : 1, sizeof *chunks);
            if (!chunks) return VAULT_ERR_OOM;
            const nh_syncd_entry *old = nh_syncd_state_find(state, path);
            const char *known = old ? nh_syncd_entry_content_hash_hex(old) : NULL;
            int same = known && strlen(known) == 64 && nh_syncd_entry_size(old) == entry->size_plain &&
                       nh_syncd_entry_chunk_count(old) == entry->chunks_len;
            char hash[65] = {0};
            if (same) memcpy(hash, known, 65);
            for (size_t k = 0; !rc && k < entry->chunks_len; ++k) {
                chunks[k] = malloc(65);
                if (!chunks[k]) { rc = VAULT_ERR_OOM; break; }
                porthome_hex64(entry->chunks[k].sha256, chunks[k]);
                const char *before = same ? nh_syncd_entry_chunk_at(old, k) : NULL;
                if (!before || strcmp(before, chunks[k])) same = 0;
            }
            if (!rc && !same) {
                EVP_MD_CTX *digest = EVP_MD_CTX_new(); uint8_t bytes[32]; unsigned n = 0;
                if (!digest) rc = VAULT_ERR_OOM;
                else if (EVP_DigestInit_ex(digest, EVP_sha256(), NULL) != 1) rc = PORTHOME_ERR_CRYPTO;
                for (size_t k = 0; !rc && k < entry->chunks_len; ++k) {
                    uint8_t *plain = NULL; size_t len = 0;
                    rc = vault_chunk_read(&entry->chunks[k], ring, io, &plain, &len);
                    if (!rc && EVP_DigestUpdate(digest, plain, len) != 1) rc = PORTHOME_ERR_CRYPTO;
                    if (plain) { OPENSSL_cleanse(plain, len); free(plain); }
                }
                if (!rc && (EVP_DigestFinal_ex(digest, bytes, &n) != 1 || n != 32)) rc = PORTHOME_ERR_CRYPTO;
                if (!rc) porthome_hex64(bytes, hash);
                EVP_MD_CTX_free(digest);
            }
            if (!rc) rc = nh_syncd_state_upsert_file_(state, path, S_IFREG | (entry->mode & 0777), getuid(), getgid(),
                entry->mtime_ns, entry->size_plain, hash, (const char *const *)chunks, entry->chunks_len);
            for (size_t k = 0; k < entry->chunks_len; ++k) free(chunks[k]);
            free(chunks);
        }
        if (rc) return rc;
    }
    return 0;
}
static int prune_inventory(const char *prefix, const vault_tree *tree, const char *source_prefix, nh_syncd_state *state)
{
    size_t prefix_len = strlen(prefix);
    for (size_t i = 0; i < nh_syncd_state_file_count(state);) {
        const char *path = NULL;
        const nh_syncd_entry *old = nh_syncd_state_at(state, i, &path);
        if (!old || !path) return VAULT_ERR_ARG;
        if (!nh_syncd_vault_conflict_path(prefix) && nh_syncd_vault_conflict_path(path)) { ++i; continue; }
        if (!strncmp(path, prefix, prefix_len) && path[prefix_len] == '/') {
            const porthome_node_entry *entry = NULL; porthome_ring_t ring;
            int found = mapped_lookup(tree, source_prefix, path + prefix_len + 1, &entry, &ring);
            if (found == VAULT_ERR_INCOMPLETE || found == VAULT_ERR_ARG) {
                char *copy = strdup(path);
                if (!copy) return VAULT_ERR_OOM;
                int rc = nh_syncd_state_delete_(state, copy); free(copy);
                if (rc) return rc;
                continue;
            }
            if (found) return found;
        }
        ++i;
    }
    return 0;
}
int nh_syncd_vault_inventory(const char *prefix, const vault_tree *tree, nh_syncd_state *state, const vault_blob_io *io)
{
    if (!prefix || !*prefix || !tree || !state || !io) return VAULT_ERR_ARG;
    int result = prune_inventory(prefix, tree, NULL, state);
    if (result) return result;
    if (!nh_syncd_state_find(state, prefix)) {
        int rc = nh_syncd_state_upsert_dir_(state, prefix, S_IFDIR | 0700, getuid(), getgid(), 0);
        if (rc) return rc;
    }
    porthome_ring_t ring;
    const porthome_dir_node *root = vault_tree_node_at(tree, 0, &ring);
    return root ? inventory_node(prefix, tree, root, &ring, state, io, 0) : VAULT_ERR_ARG;
}
int nh_syncd_vault_path_plain(const vault_tree *tree, const char *encoded, char **out)
{
    *out = NULL;
    if (!tree || !encoded || !*encoded || strlen(encoded) >= PORTHOME_MAX_PATH_LEN)
        return VAULT_ERR_ARG;
    char *prefix = strdup(encoded);
    if (!prefix) return VAULT_ERR_OOM;
    char result[PORTHOME_MAX_PATH_LEN] = {0};
    size_t used = 0;
    int rc = 0;
    for (char *p = prefix;; ++p) {
        if (*p && *p != '/') continue;
        char saved = *p;
        *p = 0;
        const porthome_node_entry *entry = NULL;
        porthome_ring_t ring;
        rc = nh_syncd_vault_lookup(tree, prefix, 1, &entry, &ring);
        *p = saved;
        if (rc) break;
        size_t n = strlen(entry->name_plain);
        if (used + !!used + n >= sizeof result) { rc = PORTHOME_ERR_TOO_LARGE; break; }
        if (used) result[used++] = '/';
        memcpy(result + used, entry->name_plain, n + 1);
        used += n;
        if (!saved) break;
    }
    free(prefix);
    if (!rc) { *out = strdup(result); if (!*out) rc = VAULT_ERR_OOM; }
    return rc;
}
int nh_syncd_vault_inventory_one(const char *destination, const vault_tree *tree, const char *encoded,
                                 nh_syncd_state *state, const vault_blob_io *io)
{
    if (!destination || strlen(destination) >= PORTHOME_MAX_PATH_LEN || !tree || !state || !io) return VAULT_ERR_ARG;
    const porthome_node_entry *source = NULL; porthome_ring_t ring;
    int rc = nh_syncd_vault_lookup(tree, encoded, 1, &source, &ring);
    char parent[PORTHOME_MAX_PATH_LEN];
    memcpy(parent, destination, strlen(destination) + 1);
    char *leaf = strrchr(parent, '/');
    if (!leaf || !leaf[1] || strlen(leaf + 1) > 255) return VAULT_ERR_ARG;
    *leaf++ = 0;
    char *source_path = NULL;
    if (!rc) rc = nh_syncd_vault_path_plain(tree, encoded, &source_path);
    if (!rc) rc = prune_inventory(destination, source->kind == PORTHOME_KIND_DIR ? tree : NULL, source_path, state);
    free(source_path);
    if (rc) return rc;
    porthome_node_entry copy = *source;
    memcpy(copy.name_plain, leaf, strlen(leaf) + 1);
    porthome_dir_node wrapper = {.entries = &copy, .entries_len = 1};
    return inventory_node(parent, tree, &wrapper, &ring, state, io, 0);
}
int nh_syncd_vault_materialize_one(int home_fd, const char *destination, const vault_tree *tree,
                                   const char *encoded, nh_syncd_state *state, const vault_blob_io *io)
{
    if (!destination || strlen(destination) >= PORTHOME_MAX_PATH_LEN || !tree || !state || !io)
        return VAULT_ERR_ARG;
    const porthome_node_entry *source = NULL;
    porthome_ring_t ring;
    int rc = nh_syncd_vault_lookup(tree, encoded, 1, &source, &ring);
    if (rc) return rc;
    char parent[PORTHOME_MAX_PATH_LEN];
    memcpy(parent, destination, strlen(destination) + 1);
    char *leaf = strrchr(parent, '/');
    if (!leaf || !leaf[1] || strlen(leaf + 1) > 255) return VAULT_ERR_ARG;
    *leaf++ = 0;
    permissions guard = {0};
    int fd = directory(home_fd, parent, 1, "", state, &guard);
    if (fd < 0) { restore_permissions(&guard, 0); return VAULT_ERR_IO; }
    const nh_syncd_entry *old = nh_syncd_state_find(state, destination);
    if (old && (int)nh_syncd_entry_kind(old) != (int)source->kind) {
        rc = remove_absent(home_fd, destination, NULL, NULL, state, &guard);
        struct stat st;
        if (!rc && fstatat(fd, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            if (unlinkat(fd, leaf, S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0) || fsync(fd)) rc = VAULT_ERR_IO;
        } else if (!rc && errno != ENOENT) rc = VAULT_ERR_IO;
        if (!rc) rc = nh_syncd_state_delete_(state, destination);
    }
    if (!rc && source->kind == PORTHOME_KIND_DIR) {
        char *source_path = NULL;
        rc = nh_syncd_vault_path_plain(tree, encoded, &source_path);
        if (!rc) rc = remove_absent(home_fd, destination, tree, source_path, state, &guard);
        free(source_path);
    }
    porthome_node_entry copy = *source;
    memcpy(copy.name_plain, leaf, strlen(leaf) + 1);
    porthome_dir_node wrapper = {.entries = &copy, .entries_len = 1};
    if (!rc) rc = apply_node(fd, parent, tree, &wrapper, &ring, state, io, &guard, 0);
    close(fd);
    int restored = restore_permissions(&guard, rc == 0);
    return rc ? rc : restored;
}
int nh_syncd_vault_remove_one(int home_fd, const char *path, nh_syncd_state *state)
{
    if (home_fd < 0 || !path || !*path || *path == '/' || !state ||
        strlen(path) >= PORTHOME_MAX_PATH_LEN)
        return VAULT_ERR_ARG;
    permissions guard = {0};
    /* Tracked descendants first (deepest first); untracked files survive. */
    int rc = remove_absent(home_fd, path, NULL, NULL, state, &guard);
    char parent[PORTHOME_MAX_PATH_LEN];
    memcpy(parent, path, strlen(path) + 1);
    char *leaf = strrchr(parent, '/');
    if (leaf)
        *leaf++ = 0;
    else
        leaf = parent;
    if (!rc) {
        int dir = leaf == parent ? fcntl(home_fd, F_DUPFD_CLOEXEC, 0)
                                 : directory(home_fd, parent, 0, "", state, &guard);
        struct stat st;
        if (dir < 0)
            rc = errno == ENOENT ? 0 : VAULT_ERR_IO;
        else if (fstatat(dir, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            if (unlinkat(dir, leaf, S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0) &&
                !(S_ISDIR(st.st_mode) && (errno == ENOTEMPTY || errno == EEXIST)))
                rc = VAULT_ERR_IO;
            if (!rc && fsync(dir))
                rc = VAULT_ERR_IO;
        } else if (errno != ENOENT)
            rc = VAULT_ERR_IO;
        if (dir >= 0)
            close(dir);
    }
    if (!rc)
        rc = nh_syncd_state_delete_(state, path);
    int restored = restore_permissions(&guard, rc == 0);
    return rc ? rc : restored;
}
static int reserved_entries(const char *prefix, const vault_tree *tree, const porthome_dir_node *node, unsigned depth)
{
    if (depth >= 128) return PORTHOME_ERR_TOO_LARGE;
    for (size_t i = 0; i < node->entries_len; ++i) {
        char path[PORTHOME_MAX_PATH_LEN];
        int rc = join(prefix, node->entries[i].name_plain, path);
        if (rc) return rc;
        if (nh_syncd_vault_conflict_path(path)) return VAULT_ERR_CONFLICT;
        if (node->entries[i].kind == PORTHOME_KIND_DIR) {
            porthome_ring_t ring;
            const porthome_dir_node *child = find_node(tree, node->entries[i].node_id, &ring);
            rc = child ? reserved_entries(path, tree, child, depth + 1) : VAULT_ERR_INCOMPLETE;
            if (rc) return rc;
        }
    }
    return 0;
}
int nh_syncd_vault_materialize(int home_fd, const char *prefix, const vault_tree *tree,
                               nh_syncd_state *state, const vault_blob_io *io)
{
    if (home_fd < 0 || !prefix || !tree || !state || !io || !io->read)
        return VAULT_ERR_ARG;
    if (!nh_syncd_vault_conflict_path(prefix)) {
        const porthome_dir_node *root_node = vault_tree_node_at(tree, 0, NULL);
        int rc = root_node ? reserved_entries(prefix, tree, root_node, 0) : VAULT_ERR_ARG;
        if (rc) return rc;
    }
    permissions guard = {0};
    int root = directory(home_fd, prefix, 1, "", state, &guard);
    if (root < 0) {
        restore_permissions(&guard, 0);
        return VAULT_ERR_IO;
    }
    porthome_ring_t ring;
    const porthome_dir_node *node = vault_tree_node_at(tree, 0, &ring);
    int rc = node ? remove_absent(home_fd, prefix, tree, NULL, state, &guard) : VAULT_ERR_ARG;
    if (!rc)
        rc = apply_node(root, prefix, tree, node, &ring, state, io, &guard, 0);
    close(root);
    int restored = restore_permissions(&guard, rc == 0);
    return rc ? rc : restored;
}
