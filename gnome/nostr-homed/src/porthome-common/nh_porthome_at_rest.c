/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "nh_porthome_at_rest.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#endif

#define MAX_DEPTH 32
#define MAX_NODES 256

static int child_path(char out[PATH_MAX], const char *dir, const char *name) {
    int n = snprintf(out, PATH_MAX, "%s/%s", dir, name);
    return n >= 0 && n < PATH_MAX;
}

static nh_at_rest walk(const char *dir, const char **ancestors,
                       unsigned depth, unsigned *nodes) {
    char real[PATH_MAX], path[PATH_MAX];
    if (depth == MAX_DEPTH || ++*nodes > MAX_NODES || !realpath(dir, real))
        return NH_AT_REST_UNKNOWN;
    for (unsigned i = 0; i < depth; i++)
        if (!strcmp(real, ancestors[i])) return NH_AT_REST_UNKNOWN;
    ancestors[depth] = real;

    struct stat st;
    if (!child_path(path, real, "dm")) return NH_AT_REST_UNKNOWN;
    int dm = stat(path, &st) == 0;
    if (!dm && errno != ENOENT) return NH_AT_REST_UNKNOWN;
    if (dm) {
        if (!child_path(path, real, "dm/uuid")) return NH_AT_REST_UNKNOWN;
        FILE *f = fopen(path, "r");
        if (!f) return NH_AT_REST_UNKNOWN;
        char uuid[256];
        int ok = fgets(uuid, sizeof uuid, f) != NULL;
        fclose(f);
        if (!ok) return NH_AT_REST_UNKNOWN;
        /* CRYPT- also names integrity/verity mappings: the namespace alone
         * is NOT encryption evidence. Only known encryption types terminate
         * the walk; integrity/verity still need encrypted backing devices. */
        const char *crypt_types[] = {"CRYPT-LUKS1-", "CRYPT-LUKS2-", "CRYPT-PLAIN-"};
        for (unsigned i = 0; i < sizeof crypt_types / sizeof crypt_types[0]; i++) {
            size_t n = strlen(crypt_types[i]);
            if (!strncmp(uuid, crypt_types[i], n) && uuid[n] && uuid[n] != '\n')
                return NH_AT_REST_ENCRYPTED;
        }
        if (!strncmp(uuid, "CRYPT-", 6) &&
            strncmp(uuid, "CRYPT-INTEGRITY-", 16) &&
            strncmp(uuid, "CRYPT-VERITY-", 13))
            return NH_AT_REST_UNKNOWN;
    }

    /* Partitions inherit the whole device's stack (including dm partitions). */
    if (!child_path(path, real, "partition")) return NH_AT_REST_UNKNOWN;
    if (stat(path, &st) == 0) {
        char *slash = strrchr(real, '/');
        if (!slash || slash == real) return NH_AT_REST_UNKNOWN;
        /* Keep the ancestor string intact while recursing. */
        *slash = 0;
        snprintf(path, sizeof path, "%s", real);
        *slash = '/';
        return walk(path, ancestors, depth + 1, nodes);
    }
    if (errno != ENOENT) return NH_AT_REST_UNKNOWN;

    if (!child_path(path, real, "slaves")) return NH_AT_REST_UNKNOWN;
    DIR *d = opendir(path);
    if (!d) return NH_AT_REST_UNKNOWN;
    unsigned children = 0;
    int unknown = 0, plain = 0;
    struct dirent *de;
    for (;;) {
        errno = 0;
        de = readdir(d);
        if (!de) { if (errno) unknown = 1; break; }
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char child[PATH_MAX];
        nh_at_rest s = child_path(child, path, de->d_name)
            ? walk(child, ancestors, depth + 1, nodes) : NH_AT_REST_UNKNOWN;
        children++;
        unknown |= s == NH_AT_REST_UNKNOWN;
        plain |= s == NH_AT_REST_UNENCRYPTED;
        if (*nodes > MAX_NODES) { unknown = 1; break; }
    }
    closedir(d);
    if (unknown) return NH_AT_REST_UNKNOWN;
    if (children) return plain ? NH_AT_REST_UNENCRYPTED : NH_AT_REST_ENCRYPTED;

    /* Only a positively identified physical leaf is 'unencrypted'. Empty
     * dm/MD, loop, zram, network and other virtual devices are unknown. */
    if (dm || strstr(real, "/virtual/")) return NH_AT_REST_UNKNOWN;
    if (!child_path(path, real, "device")) return NH_AT_REST_UNKNOWN;
    return stat(path, &st) == 0 ? NH_AT_REST_UNENCRYPTED : NH_AT_REST_UNKNOWN;
}

nh_at_rest nh_porthome_at_rest_device(const char *device_dir) {
    const char *ancestors[MAX_DEPTH];
    unsigned nodes = 0;
    return device_dir ? walk(device_dir, ancestors, 0, &nodes) : NH_AT_REST_UNKNOWN;
}

nh_at_rest nh_porthome_at_rest_fd(int fd) {
#ifdef __linux__
    struct stat st;
    struct statfs fs;
    char path[128];
    if (fstat(fd, &st) || fstatfs(fd, &fs) || !S_ISDIR(st.st_mode) || major(st.st_dev) == 0)
        return NH_AT_REST_UNKNOWN;
    /* These filesystems can put file data on additional devices which are
     * NOT slaves of st_dev (XFS realtime, F2FS/Btrfs multi-device). Until we
     * inspect their filesystem-specific geometry, even a crypt primary is
     * insufficient evidence. */
    if (fs.f_type == XFS_SUPER_MAGIC || fs.f_type == F2FS_SUPER_MAGIC ||
        fs.f_type == BTRFS_SUPER_MAGIC)
        return NH_AT_REST_UNKNOWN;
    snprintf(path, sizeof path, "/sys/dev/block/%u:%u",
             major(st.st_dev), minor(st.st_dev));
    return nh_porthome_at_rest_device(path);
#else
    (void)fd;
    return NH_AT_REST_UNKNOWN;
#endif
}

nh_at_rest nh_porthome_at_rest_path(const char *path) {
    char buf[PATH_MAX];
    if (!path || !*path || strlen(path) >= sizeof buf) return NH_AT_REST_UNKNOWN;
    strcpy(buf, path);
    for (;;) {
        int fd = open(buf, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd >= 0) {
            nh_at_rest s = nh_porthome_at_rest_fd(fd);
            close(fd);
            return s;
        }
        if (errno != ENOENT) return NH_AT_REST_UNKNOWN;
        struct stat st;
        if (lstat(buf, &st) == 0 || errno != ENOENT) return NH_AT_REST_UNKNOWN;
        size_t len = strlen(buf);
        while (len > 1 && buf[len - 1] == '/') buf[--len] = 0;
        char *slash = strrchr(buf, '/');
        if (!slash) strcpy(buf, ".");
        else if (slash == buf) buf[1] = 0;
        else *slash = 0;
        if (!strcmp(buf, path)) return NH_AT_REST_UNKNOWN;
    }
}

const char *nh_porthome_at_rest_name(nh_at_rest state) {
    switch (state) {
    case NH_AT_REST_ENCRYPTED: return "encrypted";
    case NH_AT_REST_UNENCRYPTED: return "unencrypted";
    default: return "unknown";
    }
}
const char *nh_porthome_at_rest_warning(nh_at_rest state) {
    switch (state) {
    case NH_AT_REST_ENCRYPTED: return NULL;
    case NH_AT_REST_UNENCRYPTED:
        return "WARNING: Portable home storage is not protected by dm-crypt/LUKS. "
               "Local files remain plaintext at rest. Use an encrypted root or home volume.";
    default:
        return "WARNING: Portable home disk encryption is UNKNOWN. "
               "Local files may remain plaintext at rest. Verify an encrypted root or home volume.";
    }
}
int nh_porthome_at_rest_allowed(nh_at_rest state, int require_encrypted) {
    return !require_encrypted || state == NH_AT_REST_ENCRYPTED;
}
