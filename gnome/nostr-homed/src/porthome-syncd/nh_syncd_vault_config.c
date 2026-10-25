/* SPDX-License-Identifier: MIT */
#include "nh_syncd_vault_config.h"
#include <porthome/crypto.h>
#include <jansson.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool beneath(const char *path, const char *prefix)
{
    size_t n = strlen(prefix);
    return !strncmp(path, prefix, n) && (!path[n] || path[n] == '/');
}

static bool safe_path(const char *path)
{
    if (!path || !*path || strlen(path) >= 4096 || *path == '/')
        return false;
    const char *part = path;
    for (const char *p = path;; ++p) {
        if ((unsigned char)*p < 32 && *p)
            return false;
        if (*p == '\\' || (unsigned char)*p == 127)
            return false;
        if (*p && *p != '/')
            continue;
        size_t n = (size_t)(p - part);
        if (!n || n > 255 || (n == 1 && *part == '.') ||
            (n == 2 && part[0] == '.' && part[1] == '.'))
            return false;
        if (!*p)
            return true;
        part = p + 1;
    }
}

void nh_syncd_vault_config_clear(nh_syncd_vault_config *cfg)
{
    if (!cfg)
        return;
    for (size_t i = 0; i < cfg->len; ++i)
        free(cfg->vaults[i].path);
    free(cfg->vaults);
    memset(cfg, 0, sizeof *cfg);
}

int nh_syncd_vault_config_load(const char *file, nh_syncd_vault_config *out)
{
    if (!out)
        return -1;
    memset(out, 0, sizeof *out);
    if (!file)
        return 0;
    int fd = open(file, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 1024 * 1024) {
        close(fd);
        return -1;
    }
    size_t size = (size_t)st.st_size, used = 0;
    char *data = malloc(size + 1);
    if (!data) {
        close(fd);
        return -1;
    }
    while (used < size) {
        ssize_t n = read(fd, data + used, size - used);
        if (n <= 0)
            break;
        used += (size_t)n;
    }
    char extra;
    int complete = used == size && read(fd, &extra, 1) == 0;
    close(fd);
    json_t *root = complete ? json_loadb(data, size, JSON_REJECT_DUPLICATES, NULL) : NULL;
    free(data);
    json_t *array = json_object_get(root, "vaults");
    int rc = -1;
    if (!json_is_object(root) || json_object_size(root) != 1 || !json_is_array(array) ||
        json_array_size(array) > 64)
        goto done;
    out->len = json_array_size(array);
    out->vaults = calloc(out->len ? out->len : 1, sizeof *out->vaults);
    if (!out->vaults) {
        out->len = 0;
        goto done;
    }
    for (size_t i = 0; i < out->len; ++i) {
        json_t *entry = json_array_get(array, i);
        json_t *p = json_object_get(entry, "path"), *v = json_object_get(entry, "vault_id");
        json_t *owner = json_object_get(entry, "is_owner");
        const char *path = json_string_value(p), *id = json_string_value(v);
        if (!json_is_object(entry) || json_object_size(entry) != 3 || !safe_path(path) ||
            json_string_length(p) != strlen(path) || !id || json_string_length(v) != 64 ||
            !json_is_boolean(owner) || porthome_from_hex64(id, out->vaults[i].vault_id))
            goto done;
        if (beneath(path, "Portable/.conflicts"))
            goto done;
        out->vaults[i].path = strdup(path);
        out->vaults[i].is_owner = json_is_true(owner);
        if (!out->vaults[i].path)
            goto done;
        for (size_t j = 0; j < i; ++j) {
            if (beneath(path, out->vaults[j].path) || beneath(out->vaults[j].path, path) ||
                !memcmp(out->vaults[i].vault_id, out->vaults[j].vault_id, 32))
                goto done;
        }
    }
    rc = 0;
done:
    json_decref(root);
    if (rc)
        nh_syncd_vault_config_clear(out);
    return rc;
}

bool nh_syncd_vault_conflict_path(const char *path)
{
    return path && beneath(path, "Portable/.conflicts");
}
bool nh_syncd_vault_config_excludes(void *ud, const char *path)
{
    const nh_syncd_vault_config *cfg = ud;
    if (!cfg || !path)
        return false;
    if (beneath(path, "Portable/.conflicts"))
        return true;
    for (size_t i = 0; i < cfg->len; ++i)
        if (beneath(path, cfg->vaults[i].path))
            return true;
    return false;
}
