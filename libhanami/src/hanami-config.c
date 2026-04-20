/*
 * hanami-config.c - Configuration system for libhanami
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-config.h"

#include <git2/config.h>
#include <git2/repository.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * Defaults
 * ========================================================================= */

#define DEFAULT_CACHE_DIR         "~/.cache/hanami"
#define DEFAULT_INDEX_BACKEND     "sqlite"
#define DEFAULT_UPLOAD_THRESHOLD  0
#define DEFAULT_PREFETCH_CONC     4
#define DEFAULT_VERIFY_ON_READ    true

/* =========================================================================
 * Internal struct
 * ========================================================================= */

struct hanami_config {
    char *endpoint;
    char **relays;
    size_t relay_count;
    char *cache_dir;
    char *index_backend;
    size_t upload_threshold;
    int prefetch_concurrency;
    bool verify_on_read;
};

/* =========================================================================
 * Helpers
 * ========================================================================= */

static char *safe_strdup(const char *s)
{
    return s ? strdup(s) : NULL;
}

/* Parse a comma-separated string into a NULL-terminated array */
static int parse_relay_list(const char *csv, char ***out, size_t *count)
{
    *out = NULL;
    *count = 0;

    if (!csv || !*csv)
        return 0;

    /* Count commas */
    size_t n = 1;
    for (const char *p = csv; *p; p++) {
        if (*p == ',') n++;
    }

    char **list = calloc(n + 1, sizeof(char *));
    if (!list)
        return -1;

    char *dup = strdup(csv);
    if (!dup) {
        free(list);
        return -1;
    }

    size_t idx = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(dup, ",", &saveptr);
    while (tok && idx < n) {
        /* Trim leading whitespace */
        while (*tok == ' ' || *tok == '\t') tok++;
        /* Trim trailing whitespace */
        char *end = tok + strlen(tok) - 1;
        while (end > tok && (*end == ' ' || *end == '\t')) *end-- = '\0';

        if (*tok) {
            list[idx] = strdup(tok);
            if (!list[idx]) {
                for (size_t i = 0; i < idx; i++) free(list[i]);
                free(list);
                free(dup);
                return -1;
            }
            idx++;
        }
        tok = strtok_r(NULL, ",", &saveptr);
    }

    free(dup);
    *out = list;
    *count = idx;
    return 0;
}

static void free_relay_list(char **list, size_t count)
{
    if (!list) return;
    for (size_t i = 0; i < count; i++)
        free(list[i]);
    free(list);
}

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

hanami_error_t hanami_config_new(hanami_config_t **out)
{
    if (!out)
        return HANAMI_ERR_INVALID_ARG;

    *out = NULL;

    hanami_config_t *c = calloc(1, sizeof(*c));
    if (!c)
        return HANAMI_ERR_NOMEM;

    /* Apply defaults */
    c->cache_dir = strdup(DEFAULT_CACHE_DIR);
    c->index_backend = strdup(DEFAULT_INDEX_BACKEND);
    c->upload_threshold = DEFAULT_UPLOAD_THRESHOLD;
    c->prefetch_concurrency = DEFAULT_PREFETCH_CONC;
    c->verify_on_read = DEFAULT_VERIFY_ON_READ;

    if (!c->cache_dir || !c->index_backend) {
        free(c->cache_dir);
        free(c->index_backend);
        free(c);
        return HANAMI_ERR_NOMEM;
    }

    *out = c;
    return HANAMI_OK;
}

void hanami_config_free(hanami_config_t *config)
{
    if (!config)
        return;

    free(config->endpoint);
    free_relay_list(config->relays, config->relay_count);
    free(config->cache_dir);
    free(config->index_backend);
    free(config);
}

/* =========================================================================
 * Load from environment
 * ========================================================================= */

hanami_error_t hanami_config_load_env(hanami_config_t *config)
{
    if (!config)
        return HANAMI_ERR_INVALID_ARG;

    const char *val;

    val = getenv("HANAMI_ENDPOINT");
    if (val && *val) {
        free(config->endpoint);
        config->endpoint = strdup(val);
    }

    val = getenv("HANAMI_RELAYS");
    if (val && *val) {
        free_relay_list(config->relays, config->relay_count);
        config->relays = NULL;
        config->relay_count = 0;
        parse_relay_list(val, &config->relays, &config->relay_count);
    }

    val = getenv("HANAMI_CACHE_DIR");
    if (val && *val) {
        free(config->cache_dir);
        config->cache_dir = strdup(val);
    }

    val = getenv("HANAMI_INDEX_BACKEND");
    if (val && *val) {
        free(config->index_backend);
        config->index_backend = strdup(val);
    }

    val = getenv("HANAMI_UPLOAD_THRESHOLD");
    if (val && *val) {
        long v = strtol(val, NULL, 10);
        if (v >= 0)
            config->upload_threshold = (size_t)v;
    }

    val = getenv("HANAMI_PREFETCH_CONCURRENCY");
    if (val && *val) {
        int v = atoi(val);
        if (v > 0)
            config->prefetch_concurrency = v;
    }

    val = getenv("HANAMI_VERIFY_ON_READ");
    if (val && *val) {
        if (strcmp(val, "0") == 0 || strcmp(val, "false") == 0 ||
            strcmp(val, "no") == 0)
            config->verify_on_read = false;
        else
            config->verify_on_read = true;
    }

    return HANAMI_OK;
}

/* =========================================================================
 * Load from .gitconfig
 * ========================================================================= */

hanami_error_t hanami_config_load_gitconfig(hanami_config_t *config, void *repo)
{
    if (!config)
        return HANAMI_ERR_INVALID_ARG;

    git_config *cfg = NULL;
    int rc;

    if (repo) {
        rc = git_repository_config(&cfg, (git_repository *)repo);
    } else {
        rc = git_config_open_default(&cfg);
    }

    if (rc < 0 || !cfg)
        return HANAMI_OK; /* No config is not an error */

    const char *str_val = NULL;
    int32_t int_val;
    int bool_val;

    if (git_config_get_string(&str_val, cfg, "hanami.endpoint") == 0 && str_val) {
        free(config->endpoint);
        config->endpoint = strdup(str_val);
    }

    if (git_config_get_string(&str_val, cfg, "hanami.relays") == 0 && str_val) {
        free_relay_list(config->relays, config->relay_count);
        config->relays = NULL;
        config->relay_count = 0;
        parse_relay_list(str_val, &config->relays, &config->relay_count);
    }

    if (git_config_get_string(&str_val, cfg, "hanami.cache-dir") == 0 && str_val) {
        free(config->cache_dir);
        config->cache_dir = strdup(str_val);
    }

    if (git_config_get_string(&str_val, cfg, "hanami.index-backend") == 0 && str_val) {
        free(config->index_backend);
        config->index_backend = strdup(str_val);
    }

    if (git_config_get_int32(&int_val, cfg, "hanami.upload-threshold") == 0)
        config->upload_threshold = (size_t)(int_val > 0 ? int_val : 0);

    if (git_config_get_int32(&int_val, cfg, "hanami.prefetch-concurrency") == 0 && int_val > 0)
        config->prefetch_concurrency = (int)int_val;

    if (git_config_get_bool(&bool_val, cfg, "hanami.verify-on-read") == 0)
        config->verify_on_read = (bool)bool_val;

    git_config_free(cfg);
    return HANAMI_OK;
}

/* =========================================================================
 * Getters
 * ========================================================================= */

const char *hanami_config_get_endpoint(const hanami_config_t *config)
{
    return config ? config->endpoint : NULL;
}

const char *const *hanami_config_get_relays(const hanami_config_t *config)
{
    return config ? (const char *const *)config->relays : NULL;
}

size_t hanami_config_get_relay_count(const hanami_config_t *config)
{
    return config ? config->relay_count : 0;
}

const char *hanami_config_get_cache_dir(const hanami_config_t *config)
{
    return config ? config->cache_dir : NULL;
}

const char *hanami_config_get_index_backend(const hanami_config_t *config)
{
    return config ? config->index_backend : NULL;
}

size_t hanami_config_get_upload_threshold(const hanami_config_t *config)
{
    return config ? config->upload_threshold : 0;
}

int hanami_config_get_prefetch_concurrency(const hanami_config_t *config)
{
    return config ? config->prefetch_concurrency : DEFAULT_PREFETCH_CONC;
}

bool hanami_config_get_verify_on_read(const hanami_config_t *config)
{
    return config ? config->verify_on_read : DEFAULT_VERIFY_ON_READ;
}

/* =========================================================================
 * Setters
 * ========================================================================= */

hanami_error_t hanami_config_set_endpoint(hanami_config_t *config,
                                           const char *endpoint)
{
    if (!config)
        return HANAMI_ERR_INVALID_ARG;
    free(config->endpoint);
    config->endpoint = safe_strdup(endpoint);
    return HANAMI_OK;
}

hanami_error_t hanami_config_set_relays(hanami_config_t *config,
                                         const char *const *relay_urls,
                                         size_t count)
{
    if (!config)
        return HANAMI_ERR_INVALID_ARG;

    free_relay_list(config->relays, config->relay_count);
    config->relays = NULL;
    config->relay_count = 0;

    if (!relay_urls || count == 0)
        return HANAMI_OK;

    config->relays = calloc(count + 1, sizeof(char *));
    if (!config->relays)
        return HANAMI_ERR_NOMEM;

    for (size_t i = 0; i < count; i++) {
        config->relays[i] = strdup(relay_urls[i]);
        if (!config->relays[i]) {
            free_relay_list(config->relays, i);
            config->relays = NULL;
            return HANAMI_ERR_NOMEM;
        }
    }
    config->relay_count = count;
    return HANAMI_OK;
}

hanami_error_t hanami_config_set_cache_dir(hanami_config_t *config,
                                            const char *cache_dir)
{
    if (!config)
        return HANAMI_ERR_INVALID_ARG;
    free(config->cache_dir);
    config->cache_dir = safe_strdup(cache_dir);
    return HANAMI_OK;
}

hanami_error_t hanami_config_set_index_backend(hanami_config_t *config,
                                                const char *backend)
{
    if (!config)
        return HANAMI_ERR_INVALID_ARG;
    if (backend && strcmp(backend, "sqlite") != 0 && strcmp(backend, "lmdb") != 0)
        return HANAMI_ERR_INVALID_ARG;
    free(config->index_backend);
    config->index_backend = safe_strdup(backend);
    return HANAMI_OK;
}

void hanami_config_set_upload_threshold(hanami_config_t *config, size_t threshold)
{
    if (config)
        config->upload_threshold = threshold;
}

void hanami_config_set_prefetch_concurrency(hanami_config_t *config, int concurrency)
{
    if (config && concurrency > 0)
        config->prefetch_concurrency = concurrency;
}

void hanami_config_set_verify_on_read(hanami_config_t *config, bool verify)
{
    if (config)
        config->verify_on_read = verify;
}
