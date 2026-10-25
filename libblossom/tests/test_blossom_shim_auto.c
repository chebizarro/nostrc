/* Probe policy, environment independence and 24h cache discipline. */
#include <blossom/client.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
int main(void) {
    const char *urls[] = { "https://raw.example", "https://png.example" };
    blossom_shim_cache_reset();
    blossom_shim_cache_set(urls[0], BLOSSOM_CAP_YES, BLOSSOM_CAP_YES);
    blossom_shim_cache_set(urls[1], BLOSSOM_CAP_NO, BLOSSOM_CAP_YES);
    blossom_shim_reason_t reason;
    char who[80];
    assert(!blossom_shim_active_for_ex(urls, 1, &reason, who, sizeof who));
    assert(reason == BLOSSOM_SHIM_REASON_AUTO_RAW_OK);
    assert(blossom_shim_active_for_ex(urls, 2, &reason, who, sizeof who));
    assert(reason == BLOSSOM_SHIM_REASON_AUTO_REQUIRED && !strcmp(who, urls[1]));
    /* Neither direction of the retired override may change the wire address. */
    setenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM", "0", 1);
    assert(blossom_shim_active_for(urls, 2));
    setenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM", "1", 1);
    assert(!blossom_shim_active_for(urls, 1));
    assert(!blossom_shim_active_for(NULL, 0));
    assert(!blossom_shim_active());
    unsetenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM");
    /* UNKNOWN is conservative and does not override positive observations. */
    blossom_shim_cache_set(urls[1], BLOSSOM_CAP_UNKNOWN, BLOSSOM_CAP_YES);
    assert(!blossom_shim_active_for_ex(urls, 2, &reason, NULL, 0));
    assert(reason == BLOSSOM_SHIM_REASON_AUTO_FALLBACK);
    blossom_server_capabilities_t caps;
    assert(blossom_capability_cache_get(urls[0], &caps));
    int64_t now = (int64_t)time(NULL);
    assert(caps.last_probe_ts >= now - 2 && caps.last_probe_ts <= now);
    caps.last_probe_ts = now - BLOSSOM_CACHE_REFRESH_SECONDS - 1;
    blossom_capability_cache_set(urls[0], &caps);
    assert(!blossom_capability_cache_get(urls[0], &caps));
    caps.last_probe_ts = now + 3600; /* Clock rollback cannot immortalize an entry. */
    blossom_capability_cache_set(urls[0], &caps);
    assert(!blossom_capability_cache_get(urls[0], &caps));
    caps.last_probe_ts = 1; /* Old sentinel is expired, not a freshness exemption. */
    blossom_capability_cache_set(urls[0], &caps);
    assert(!blossom_capability_cache_get(urls[0], &caps));
    blossom_shim_cache_reset();
    assert(!blossom_capability_cache_get(urls[1], &caps));
    puts("OK probe-selected shim and 24h expiry");
    return 0;
}
