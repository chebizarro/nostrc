#include <blossom/capability.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct entry {
    char *url;
    blossom_server_capabilities_t caps;
    struct entry *next;
} entry;
static entry *cache;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void blossom_server_capabilities_init(blossom_server_capabilities_t *c) {
    if (c) memset(c, 0, sizeof(*c));
}
const char *blossom_capability_state_str(blossom_capability_state_t s) {
    return s == BLOSSOM_CAP_YES ? "yes" : s == BLOSSOM_CAP_NO ? "no" : "unknown";
}
void blossom_shim_cache_reset(void) {
    pthread_mutex_lock(&lock);
    while (cache) {
        entry *next = cache->next;
        free(cache->url); free(cache); cache = next;
    }
    pthread_mutex_unlock(&lock);
}
void blossom_capability_cache_set(const char *url, const blossom_server_capabilities_t *caps) {
    if (!url || !caps) return;
    pthread_mutex_lock(&lock);
    entry *e;
    for (e = cache; e && strcmp(e->url, url); e = e->next) {}
    if (!e) {
        e = calloc(1, sizeof(*e));
        if (e) {
            e->url = strdup(url);
            if (!e->url) { free(e); e = NULL; }
            else { e->next = cache; cache = e; }
        }
    }
    if (e) e->caps = *caps;
    pthread_mutex_unlock(&lock);
}
bool blossom_capability_cache_get(const char *url, blossom_server_capabilities_t *caps) {
    if (!url || !caps) return false;
    bool found = false;
    int64_t now = (int64_t)time(NULL);
    pthread_mutex_lock(&lock);
    for (entry *e = cache; e; e = e->next) {
        if (strcmp(e->url, url)) continue;
        int64_t ts = e->caps.last_probe_ts;
        if (ts > 0 && now >= ts && now - ts < BLOSSOM_CACHE_REFRESH_SECONDS) {
            *caps = e->caps; found = true;
        }
        break;
    }
    pthread_mutex_unlock(&lock);
    return found;
}
void blossom_shim_cache_set(const char *url, blossom_capability_state_t raw, blossom_capability_state_t png) {
    blossom_server_capabilities_t c = {0};
    c.raw_random_ok = raw; c.png_shim_ok = png;
    c.last_probe_ts = (int64_t)time(NULL);
    blossom_capability_cache_set(url, &c);
}
