#include "model/gn-profile-list-model.h"

#include <gio/gio.h>
#include <glib.h>

static void
test_clear_removes_model_owned_identity_state(void)
{
    static const char pubkey[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char *muted[] = { pubkey, NULL };

    GnProfileListModel *model = gn_profile_list_model_new();
    g_assert_nonnull(model);

    gn_profile_list_model_set_muted_set(model, muted);
    gn_profile_list_model_filter(model, "private-contact@example.com");
    g_assert_true(gn_profile_list_model_is_pubkey_muted(model, pubkey));

    gn_profile_list_model_clear_cache(model);

    g_assert_false(gn_profile_list_model_is_pubkey_muted(model, pubkey));
    g_assert_cmpuint(gn_profile_list_model_get_total_count(model), ==, 0);
    g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(model)), ==, 0);
    g_assert_false(gn_profile_list_model_get_all_loaded(model));

    /* Clearing an already-empty model must remain safe and idempotent. */
    gn_profile_list_model_clear_cache(model);
    g_object_unref(model);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/profile/cache/clear-owned-identity-state",
                    test_clear_removes_model_owned_identity_state);
    return g_test_run();
}
