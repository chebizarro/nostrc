/* SPDX-License-Identifier: MIT */
#include "vc.h"
#include <porthome/crypto.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

typedef struct {
    vc_app *app;
    vault_tree_node *nodes;
    size_t count;
} importer;
static int file_chunks(vc_app *a, int fd, const uint8_t key[32], porthome_node_entry *entry)
{
    uint8_t *buf = malloc(PORTHOME_CHUNK_MAX);
    if (!buf)
        return VAULT_ERR_OOM;
    size_t have = 0;
    int eof = 0, rc = 0;
    while (!eof || have) {
        while (have < PORTHOME_CHUNK_MAX && !eof) {
            ssize_t r = read(fd, buf + have, PORTHOME_CHUNK_MAX - have);
            if (r < 0 && errno == EINTR)
                continue;
            if (r < 0) {
                rc = VAULT_ERR_IO;
                goto done;
            }
            if (!r)
                eof = 1;
            else
                have += (size_t)r;
        }
        if (!have)
            break;
        size_t cut = porthome_chunk_next(buf, have);
        if (entry->chunks_len == PORTHOME_NODE_MAX_CHUNKS) {
            rc = VAULT_ERR_ARG;
            goto done;
        }
        porthome_node_chunk *p = realloc(entry->chunks, (entry->chunks_len + 1) * sizeof *p);
        if (!p) {
            rc = VAULT_ERR_OOM;
            goto done;
        }
        entry->chunks = p;
        porthome_node_chunk *chunk = &p[entry->chunks_len];
        memset(chunk, 0, sizeof *chunk);
        uint8_t *sealed = NULL;
        size_t n = 0;
        rc = porthome_encrypt_chunk_v2(key, buf, cut, &sealed, &n, chunk->sha256);
        if (!rc)
            rc = vc_cache_put(a, chunk->sha256, sealed, n);
        free(sealed);
        if (!rc)
            rc = vc_length_put(a, chunk->sha256, 1, cut);
        if (rc)
            goto done;
        chunk->size_sealed = n;
        chunk->chunk_key_id = 1;
        entry->chunks_len++;
        entry->size_plain += cut;
        memmove(buf, buf + cut, have - cut);
        have -= cut;
    }
done:
    OPENSSL_clear_free(buf, PORTHOME_CHUNK_MAX);
    return rc;
}
static int same_file(const struct stat *a, const struct stat *b)
{
#ifdef __APPLE__
    int nanos = a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
                a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
    int nanos =
        a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
    return nanos && a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
           a->st_mtime == b->st_mtime && a->st_ctime == b->st_ctime;
}
static int walk(importer *im, int fd, const uint8_t id[32], const uint8_t key[32], unsigned depth,
                size_t *index)
{
    struct stat here, state;
    if (fstat(fd, &here) || fstat(im->app->state.dirfd, &state))
        return VAULT_ERR_IO;
    if (here.st_dev == state.st_dev && here.st_ino == state.st_ino) {
        vc_error(im->app, "state directory must not be inside the imported subtree");
        return VAULT_ERR_ARG;
    }
    if (depth > 128 || im->count == VAULT_MAX_OBJECTS)
        return VAULT_ERR_ARG;
    size_t idx = im->count++;
    vault_tree_node *more = realloc(im->nodes, im->count * sizeof *more);
    if (!more) {
        im->count--;
        return VAULT_ERR_OOM;
    }
    im->nodes = more;
    memset(&more[idx], 0, sizeof more[idx]);
    porthome_dir_node *node = calloc(1, sizeof *node);
    porthome_ring_entry_t *r = calloc(1, sizeof *r);
    if (!node || !r) {
        free(node);
        free(r);
        return VAULT_ERR_OOM;
    }
    memcpy(node->node_id, id, 32);
    node->epoch = 1;
    r->epoch = 1;
    memcpy(r->node_key, key, 32);
    more[idx] = (vault_tree_node){node, {r, 1}};
    int copy = dup(fd);
    DIR *dir = copy < 0 ? NULL : fdopendir(copy);
    if (!dir) {
        if (copy >= 0)
            close(copy);
        return VAULT_ERR_IO;
    }
    int rc = 0;
    struct dirent *de;
    struct stat start, finish;
    if (fstat(fd, &start)) {
        closedir(dir);
        return VAULT_ERR_IO;
    }
    errno = 0;
    while ((de = readdir(dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (node->entries_len >= 65536) {
            rc = VAULT_ERR_ARG;
            break;
        }
        porthome_node_entry *entries =
            realloc(node->entries, (node->entries_len + 1) * sizeof *entries);
        if (!entries) {
            rc = VAULT_ERR_OOM;
            break;
        }
        node->entries = entries;
        porthome_node_entry *e = &entries[node->entries_len++];
        memset(e, 0, sizeof *e);
        struct stat st;
        if (fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
            rc = VAULT_ERR_IO;
            break;
        }
        e->mode = st.st_mode & 0777;
        e->mtime_ns = st.st_mtime < 0 ? 0 : (uint64_t)st.st_mtime * 1000000000;
        rc = porthome_encrypt_name_v2(key, de->d_name, e->name_enc);
        if (!rc)
            rc = vc_name_put(im->app, id, e->name_enc, de->d_name);
        if (rc)
            break;
        if (S_ISDIR(st.st_mode)) {
            e->kind = PORTHOME_KIND_DIR;
            e->child_epoch = 1;
            uint8_t child_key[32];
            if (RAND_bytes(e->node_id, 32) != 1) {
                rc = VAULT_ERR_IO;
                break;
            }
            rc = porthome_child_node_key(key, e->node_id, NULL, child_key);
            if (rc)
                break;
            int child = openat(fd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            size_t child_idx;
            struct stat check;
            if (child < 0 || fstat(child, &check) || !same_file(&st, &check))
                rc = VAULT_ERR_IO;
            else
                rc = walk(im, child, e->node_id, child_key, depth + 1, &child_idx);
            if (child >= 0)
                close(child);
            OPENSSL_cleanse(child_key, 32);
            if (rc)
                break;
        } else if (S_ISREG(st.st_mode)) {
            e->kind = PORTHOME_KIND_FILE;
            int f = openat(fd, de->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            struct stat before, after;
            if (f < 0 || fstat(f, &before) || !same_file(&st, &before))
                rc = VAULT_ERR_IO;
            else {
                rc = file_chunks(im->app, f, key, e);
                if (!rc && (fstat(f, &after) || !same_file(&before, &after) ||
                            e->size_plain != (uint64_t)before.st_size))
                    rc = VAULT_ERR_AGAIN;
            }
            if (f >= 0)
                close(f);
            if (rc)
                break;
        } else if (S_ISLNK(st.st_mode)) {
            e->kind = PORTHOME_KIND_SYMLINK;
            char b[4097];
            ssize_t n = readlinkat(fd, de->d_name, b, sizeof b - 1);
            if (n < 1 || n == (ssize_t)sizeof b - 1) {
                rc = VAULT_ERR_ARG;
                break;
            }
            b[n] = 0;
            e->symlink_target = strdup(b);
            if (!e->symlink_target) {
                rc = VAULT_ERR_OOM;
                break;
            }
        } else {
            rc = VAULT_ERR_ARG;
            break;
        }
        errno = 0;
    }
    if (!rc && errno)
        rc = VAULT_ERR_IO;
    if (!rc && (fstat(fd, &finish) || !same_file(&start, &finish)))
        rc = VAULT_ERR_AGAIN;
    closedir(dir);
    *index = idx;
    return rc;
}
int vc_import(vc_app *a, const char *path, vault_tree **out)
{
    *out = NULL;
    uint8_t seed[32], key[32], id[32];
    if (RAND_bytes(seed, 32) != 1 || RAND_bytes(id, 32) != 1)
        return VAULT_ERR_IO;
    int rc = porthome_root_node_key(seed, key);
    OPENSSL_cleanse(seed, 32);
    if (rc)
        return rc;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        OPENSSL_cleanse(key, 32);
        return VAULT_ERR_IO;
    }
    importer im = {.app = a};
    size_t index;
    rc = walk(&im, fd, id, key, 0, &index);
    close(fd);
    OPENSSL_cleanse(key, 32);
    if (!rc) {
        porthome_dir_node *root = (porthome_dir_node *)im.nodes[0].node;
        root->vault = calloc(1, sizeof *root->vault);
        if (!root->vault)
            rc = VAULT_ERR_OOM;
        else {
            memcpy(root->vault->owner, a->pub, 32);
            root->vault->grants_rev = 1;
            root->vault->writers = malloc(32);
            if (!root->vault->writers)
                rc = VAULT_ERR_OOM;
            else {
                memcpy(root->vault->writers[0], a->pub, 32);
                root->vault->writers_len = 1;
            }
            if (RAND_bytes(root->vault->vault_id, 32) != 1)
                rc = VAULT_ERR_IO;
        }
    }
    /* A local seal fills child addresses before closed-graph validation. */
    if (!rc) {
        for (size_t i = im.count; i-- > 0;) {
            porthome_dir_node *n = (porthome_dir_node *)im.nodes[i].node;
            uint8_t *b = NULL, hash[32];
            size_t z = 0;
            rc = porthome_dir_node_seal(n, im.nodes[i].ring.entries[0].node_key, &b, &z, hash);
            if (!rc)
                rc = vc_cache_put(a, hash, b, z);
            free(b);
            if (rc)
                break;
            for (size_t j = 0; j < i; j++) {
                porthome_dir_node *parent = (porthome_dir_node *)im.nodes[j].node;
                for (size_t k = 0; k < parent->entries_len; k++)
                    if (parent->entries[k].kind == PORTHOME_KIND_DIR &&
                        !memcmp(parent->entries[k].node_id, n->node_id, 32))
                        memcpy(parent->entries[k].manifest_sha256, hash, 32);
            }
        }
    }
    if (!rc)
        rc = vault_tree_new(im.nodes, im.count, out);
    for (size_t i = 0; i < im.count; i++) {
        porthome_dir_node *n = (porthome_dir_node *)im.nodes[i].node;
        if (n) {
            porthome_dir_node_dispose(n);
            free(n);
        }
        OPENSSL_clear_free((void *)im.nodes[i].ring.entries,
                           im.nodes[i].ring.count * sizeof(porthome_ring_entry_t));
    }
    free(im.nodes);
    return rc;
}
