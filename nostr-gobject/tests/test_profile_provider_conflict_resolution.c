/**
 * test_profile_provider_conflict_resolution.c — newest-valid-wins behavior
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>
#include <nostr-gobject-1.0/nostr_profile_provider.h>

static void
test_older_profile_does_not_override_newer(void)
{
    const char *pubkey = "1111111111111111111111111111111111111111111111111111111111111111";

    gnostr_profile_provider_init(16);

    g_assert_true(gnostr_profile_provider_update_if_newer(
        pubkey,
        "{\"display_name\":\"Newest\",\"name\":\"newest\"}",
        200));

    g_assert_false(gnostr_profile_provider_update_if_newer(
        pubkey,
        "{\"display_name\":\"Older\",\"name\":\"older\"}",
        100));

    GnostrProfileMeta *meta = gnostr_profile_provider_get(pubkey);
    g_assert_nonnull(meta);
    g_assert_cmpstr(meta->display_name, ==, "Newest");
    g_assert_cmpint(meta->created_at, ==, 200);
    gnostr_profile_meta_free(meta);

    gnostr_profile_provider_shutdown();
}

static void
test_newer_profile_overrides_older(void)
{
    const char *pubkey = "2222222222222222222222222222222222222222222222222222222222222222";

    gnostr_profile_provider_init(16);

    g_assert_true(gnostr_profile_provider_update_if_newer(
        pubkey,
        "{\"display_name\":\"Older\",\"name\":\"older\"}",
        100));

    g_assert_true(gnostr_profile_provider_update_if_newer(
        pubkey,
        "{\"display_name\":\"Newest\",\"name\":\"newest\"}",
        200));

    GnostrProfileMeta *meta = gnostr_profile_provider_get(pubkey);
    g_assert_nonnull(meta);
    g_assert_cmpstr(meta->display_name, ==, "Newest");
    g_assert_cmpint(meta->created_at, ==, 200);
    gnostr_profile_meta_free(meta);

    gnostr_profile_provider_shutdown();
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/nostr-gobject/profile-provider/older-does-not-override-newer",
                    test_older_profile_does_not_override_newer);
    g_test_add_func("/nostr-gobject/profile-provider/newer-overrides-older",
                    test_newer_profile_overrides_older);

    return g_test_run();
}
