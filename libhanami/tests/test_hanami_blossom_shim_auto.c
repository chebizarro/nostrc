/*
 * test_hanami_blossom_shim_auto.c
 *
 * Unit tests for the probe-driven auto-shim decision helper
 * (nostrc-si30): hanami_blossom_shim_active_for + the URL-keyed
 * capability cache seeded via hanami_blossom_shim_cache_set.
 *
 * Doesn't touch the network — every test seeds the cache directly.
 * Env-var precedence is exercised via setenv/unsetenv toggles.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-blossom-shim.h"
#include "hanami/hanami-server-capability.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_passed = 0;

#define TEST(name) \
    do { \
        printf("  %-56s ", #name); \
        fflush(stdout); \
        test_##name(); \
        printf("OK\n"); \
        tests_passed++; \
    } while (0)

/* Discipline: clear the cache + unset the env vars before every test
 * so results are hermetic. */
static void reset_state(void)
{
    hanami_blossom_shim_cache_reset();
    unsetenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM");
    /* Skip probes — we seed the cache directly. */
    setenv("NOSTR_HOMED_HANAMI_SKIP_CAPABILITY_PROBE", "1", 1);
}

/* --- 1. All servers raw_random_ok=YES -> shim OFF. */
static void test_all_raw_ok_returns_false(void)
{
    reset_state();
    hanami_blossom_shim_cache_set("https://sharegap.example",
                                  HANAMI_CAP_YES, HANAMI_CAP_UNKNOWN);
    hanami_blossom_shim_cache_set("https://alt.example",
                                  HANAMI_CAP_YES, HANAMI_CAP_YES);
    const char *urls[] = { "https://sharegap.example",
                           "https://alt.example" };
    hanami_blossom_shim_reason_t r;
    bool on = hanami_blossom_shim_active_for_ex(urls, 2, &r, NULL, 0);
    assert(!on);
    assert(r == HANAMI_SHIM_REASON_AUTO_RAW_OK);
}

/* --- 2. One server raw=NO && shim=YES -> shim ON. */
static void test_one_shim_required_returns_true(void)
{
    reset_state();
    hanami_blossom_shim_cache_set("https://sharegap.example",
                                  HANAMI_CAP_YES, HANAMI_CAP_YES);
    hanami_blossom_shim_cache_set("https://primal.example",
                                  HANAMI_CAP_NO,  HANAMI_CAP_YES);
    const char *urls[] = { "https://sharegap.example",
                           "https://primal.example" };
    hanami_blossom_shim_reason_t r;
    char who[128] = {0};
    bool on = hanami_blossom_shim_active_for_ex(urls, 2, &r, who, sizeof who);
    assert(on);
    assert(r == HANAMI_SHIM_REASON_AUTO_REQUIRED);
    assert(strcmp(who, "https://primal.example") == 0);
}

/* --- 3. UNKNOWN capabilities (probe disabled) -> shim OFF (fallback). */
static void test_unknown_falls_back_to_off(void)
{
    reset_state();
    /* No cache entries: they get created lazily but probe is disabled,
     * so they stay UNKNOWN. */
    const char *urls[] = { "https://never-probed.example" };
    hanami_blossom_shim_reason_t r;
    bool on = hanami_blossom_shim_active_for_ex(urls, 1, &r, NULL, 0);
    assert(!on);
    assert(r == HANAMI_SHIM_REASON_AUTO_FALLBACK);
}

/* --- 4. Env-var "1" force-ON wins over any capability state. */
static void test_env_force_on_overrides_caps(void)
{
    reset_state();
    /* Cache says every server accepts raw — auto-decide would say OFF. */
    hanami_blossom_shim_cache_set("https://sharegap.example",
                                  HANAMI_CAP_YES, HANAMI_CAP_YES);
    setenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM", "1", 1);
    const char *urls[] = { "https://sharegap.example" };
    hanami_blossom_shim_reason_t r;
    bool on = hanami_blossom_shim_active_for_ex(urls, 1, &r, NULL, 0);
    assert(on);
    assert(r == HANAMI_SHIM_REASON_ENV_FORCE_ON);
    unsetenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM");
}

/* --- 5. Env-var "0" force-OFF wins over any capability state. */
static void test_env_force_off_overrides_caps(void)
{
    reset_state();
    /* Cache says primal requires shim — auto-decide would say ON. */
    hanami_blossom_shim_cache_set("https://primal.example",
                                  HANAMI_CAP_NO, HANAMI_CAP_YES);
    setenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM", "0", 1);
    const char *urls[] = { "https://primal.example" };
    hanami_blossom_shim_reason_t r;
    bool on = hanami_blossom_shim_active_for_ex(urls, 1, &r, NULL, 0);
    assert(!on);
    assert(r == HANAMI_SHIM_REASON_ENV_FORCE_OFF);
    unsetenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM");
}

/* --- 6. Env-var unset + garbage string -> auto-decide. */
static void test_env_garbage_falls_through(void)
{
    reset_state();
    hanami_blossom_shim_cache_set("https://a.example",
                                  HANAMI_CAP_YES, HANAMI_CAP_NO);
    setenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM", "true", 1);
    const char *urls[] = { "https://a.example" };
    hanami_blossom_shim_reason_t r;
    bool on = hanami_blossom_shim_active_for_ex(urls, 1, &r, NULL, 0);
    assert(!on);
    assert(r == HANAMI_SHIM_REASON_AUTO_RAW_OK);
    unsetenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM");
}

/* --- 7. Zero servers falls back to hanami_blossom_shim_active(). */
static void test_zero_servers_falls_back_to_env(void)
{
    reset_state();
    hanami_blossom_shim_reason_t r;
    /* Env unset: legacy shim_active() returns false. */
    bool on = hanami_blossom_shim_active_for_ex(NULL, 0, &r, NULL, 0);
    assert(!on);
    assert(r == HANAMI_SHIM_REASON_AUTO_FALLBACK);

    /* Env set to 1: overrides even the zero-servers path. */
    setenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM", "1", 1);
    on = hanami_blossom_shim_active_for_ex(NULL, 0, &r, NULL, 0);
    assert(on);
    assert(r == HANAMI_SHIM_REASON_ENV_FORCE_ON);
    unsetenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM");
}

/* --- 8. Reset clears prior seed. */
static void test_cache_reset(void)
{
    reset_state();
    hanami_blossom_shim_cache_set("https://a.example",
                                  HANAMI_CAP_NO, HANAMI_CAP_YES);
    /* Confirm it's there. */
    const char *urls[] = { "https://a.example" };
    hanami_blossom_shim_reason_t r;
    bool on = hanami_blossom_shim_active_for_ex(urls, 1, &r, NULL, 0);
    assert(on);

    /* Reset -> UNKNOWN -> fallback OFF. */
    hanami_blossom_shim_cache_reset();
    on = hanami_blossom_shim_active_for_ex(urls, 1, &r, NULL, 0);
    assert(!on);
    assert(r == HANAMI_SHIM_REASON_AUTO_FALLBACK);
}

int main(void)
{
    printf("libhanami PNG-shim auto-decide tests (nostrc-si30)\n");
    printf("==================================================\n");

    TEST(all_raw_ok_returns_false);
    TEST(one_shim_required_returns_true);
    TEST(unknown_falls_back_to_off);
    TEST(env_force_on_overrides_caps);
    TEST(env_force_off_overrides_caps);
    TEST(env_garbage_falls_through);
    TEST(zero_servers_falls_back_to_env);
    TEST(cache_reset);

    printf("\n%d passed, 0 failed\n", tests_passed);
    return 0;
}
