/* SPDX-License-Identifier: GPL-3.0-or-later
 * Which rejected kind:445 events may report "group out of sync", and how
 * often (W16b re-review N1).
 */

#include "gn-mls-group-error.h"
#include <marmot/marmot-error.h>

static GError *
client_error(gint code)
{
  return g_error_new_literal(g_quark_from_static_string("marmot-gobject-client-error"),
                             code, "test");
}

/* Only MLS-level failures -- reachable only past the member-keyed NIP-44
 * layer -- count; backfill and outsider junk fail before it. */
static void
test_only_mls_errors_are_divergence(void)
{
  const gint noise[] = {
    MARMOT_ERR_NIP44, MARMOT_ERR_GROUP_NOT_FOUND, MARMOT_ERR_INVALID_ARG,
    MARMOT_ERR_DESERIALIZATION, MARMOT_ERR_STORAGE, MARMOT_ERR_CRYPTO,
    MARMOT_ERR_USE_AFTER_EVICTION,
  };
  for (gsize i = 0; i < G_N_ELEMENTS(noise); i++)
    {
      g_autoptr(GError) e = client_error(noise[i]);
      g_assert_false(gn_mls_group_error_is_divergence(e));
    }
  const gint mls[] = { MARMOT_ERR_MLS, MARMOT_ERR_MLS_PROCESS_MESSAGE,
                       MARMOT_ERR_MLS_FRAMING };
  for (gsize i = 0; i < G_N_ELEMENTS(mls); i++)
    {
      g_autoptr(GError) e = client_error(mls[i]);
      g_assert_true(gn_mls_group_error_is_divergence(e));
    }
  /* Same code from another domain is not a Marmot processing error. */
  g_autoptr(GError) other = g_error_new_literal(G_FILE_ERROR, MARMOT_ERR_MLS, "x");
  g_assert_false(gn_mls_group_error_is_divergence(other));
  g_assert_false(gn_mls_group_error_is_divergence(NULL));
}

/* Commit outcomes that are not divergence: a stale or losing Commit, one
 * deferred behind our pending Commit, an unauthorized one; and the merge
 * result "another member's Commit won" (review B1). */
static void
test_commit_outcomes(void)
{
  const gint not_divergence[] = {
    MARMOT_ERR_WRONG_EPOCH, MARMOT_ERR_OWN_COMMIT_PENDING,
    MARMOT_ERR_COMMIT_FROM_NON_ADMIN, MARMOT_ERR_PROTOCOL_GROUP_MISMATCH,
  };
  for (gsize i = 0; i < G_N_ELEMENTS(not_divergence); i++)
    {
      g_autoptr(GError) e = client_error(not_divergence[i]);
      g_assert_false(gn_mls_group_error_is_divergence(e));
    }
  g_autoptr(GError) superseded = client_error(MARMOT_ERR_WRONG_EPOCH);
  g_assert_true(gn_mls_group_error_is_superseded(superseded));
  g_autoptr(GError) storage = client_error(MARMOT_ERR_STORAGE);
  g_assert_false(gn_mls_group_error_is_superseded(storage));
  g_autoptr(GError) other = g_error_new_literal(G_FILE_ERROR, MARMOT_ERR_WRONG_EPOCH, "x");
  g_assert_false(gn_mls_group_error_is_superseded(other));
  g_assert_false(gn_mls_group_error_is_superseded(NULL));
}

/* A 500-event backfill of MLS failures reports once per group; a group
 * that moves to a new epoch can report again. */
static void
test_gate_reports_once_per_group(void)
{
  g_autoptr(GHashTable) reported =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  guint a = 0, b = 0;
  for (int i = 0; i < 500; i++)
    {
      a += gn_mls_group_error_gate_admit(reported, "aa");
      b += gn_mls_group_error_gate_admit(reported, "bb");
    }
  g_assert_cmpuint(a, ==, 1);
  g_assert_cmpuint(b, ==, 1);
  g_assert_false(gn_mls_group_error_gate_admit(reported, ""));
  g_assert_false(gn_mls_group_error_gate_admit(reported, NULL));

  gn_mls_group_error_gate_reset(reported, "aa");
  g_assert_true(gn_mls_group_error_gate_admit(reported, "aa"));
  g_assert_false(gn_mls_group_error_gate_admit(reported, "aa"));
  g_assert_false(gn_mls_group_error_gate_admit(reported, "bb"));
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/mls-groups/group-error/only-mls-errors", test_only_mls_errors_are_divergence);
  g_test_add_func("/mls-groups/group-error/once-per-group", test_gate_reports_once_per_group);
  g_test_add_func("/mls-groups/group-error/commit-outcomes", test_commit_outcomes);
  return g_test_run();
}
