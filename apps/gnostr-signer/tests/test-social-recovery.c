/* test-social-recovery.c - Unit tests for social recovery (Shamir's Secret Sharing)
 *
 * Tests the core SSS functionality:
 * - Key splitting into shares
 * - Key reconstruction from threshold shares
 * - Share encoding/decoding
 * - Guardian management
 * - Configuration persistence
 */
#include <glib.h>
#include <string.h>
#include "../src/social-recovery.h"
#include "../src/secure-mem.h"

/* Test fixture */
typedef struct {
  guint8 test_secret[32];
  gchar *test_nsec;
} SSSFixture;

static void fixture_setup(SSSFixture *fix, gconstpointer user_data) {
  (void)user_data;

  /* Generate a deterministic test secret */
  for (int i = 0; i < 32; i++) {
    fix->test_secret[i] = (guint8)(i * 7 + 42);
  }

  /* Create a test nsec (using known test key) */
  fix->test_nsec = g_strdup("nsec1vl029mgpspedva04g90vltkh6fvh240zqtv9k0t9af8935ke9laqsnlfe5");
}

static void fixture_teardown(SSSFixture *fix, gconstpointer user_data) {
  (void)user_data;
  gnostr_secure_clear(fix->test_secret, 32);
  if (fix->test_nsec) {
    gnostr_secure_clear(fix->test_nsec, strlen(fix->test_nsec));
    g_free(fix->test_nsec);
  }
}

/* ============================================================
 * Shamir's Secret Sharing Tests
 * ============================================================ */

static void test_sss_split_basic(SSSFixture *fix, gconstpointer user_data) {
  (void)user_data;
  GError *error = NULL;
  GPtrArray *shares = NULL;

  /* Split 2-of-3 */
  gboolean ok = gn_sss_split(fix->test_secret, 32, 2, 3, &shares, &error);
  g_assert_no_error(error);
  g_assert_true(ok);
  g_assert_nonnull(shares);
  g_assert_cmpuint(shares->len, ==, 3);

  /* Verify each share has correct properties */
  for (guint i = 0; i < shares->len; i++) {
    GnSSSShare *share = g_ptr_array_index(shares, i);
    g_assert_nonnull(share);
    g_assert_cmpuint(share->index, ==, i + 1);  /* 1-indexed */
    g_assert_cmpuint(share->data_len, ==, 32);
    g_assert_nonnull(share->data);
  }

  gn_sss_shares_free(shares);
}

static void test_sss_split_thresholds(SSSFixture *fix, gconstpointer user_data) {
  (void)user_data;
  GError *error = NULL;
  GPtrArray *shares = NULL;

  /* Test various valid thresholds */
  const struct { guint8 k; guint8 n; } valid_configs[] = {
    {2, 2}, {2, 3}, {2, 5}, {3, 5}, {5, 10}, {10, 10}
  };

  for (gsize i = 0; i < G_N_ELEMENTS(valid_configs); i++) {
    gboolean ok = gn_sss_split(fix->test_secret, 32,
                               valid_configs[i].k, valid_configs[i].n,
                               &shares, &error);
    g_assert_no_error(error);
    g_assert_true(ok);
    g_assert_cmpuint(shares->len, ==, valid_configs[i].n);
    gn_sss_shares_free(shares);
    shares = NULL;
  }
}

static void test_sss_split_invalid_params(SSSFixture *fix, gconstpointer user_data) {
  (void)user_data;
  GError *error = NULL;
  GPtrArray *shares = NULL;

  /* Threshold < 2 should fail */
  gboolean ok = gn_sss_split(fix->test_secret, 32, 1, 3, &shares, &error);
  g_assert_false(ok);
  g_assert_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_PARAMS);
  g_clear_error(&error);

  /* Threshold > total should fail */
  ok = gn_sss_split(fix->test_secret, 32, 5, 3, &shares, &error);
  g_assert_false(ok);
  g_assert_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_PARAMS);
  g_clear_error(&error);

  /* Empty secret should fail */
  ok = gn_sss_split(NULL, 0, 2, 3, &shares, &error);
  g_assert_false(ok);
  g_assert_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_KEY);
  g_clear_error(&error);
}

static void test_sss_combine_basic(SSSFixture *fix, gconstpointer user_data) {
  (void)user_data;
  GError *error = NULL;
  GPtrArray *shares = NULL;

  /* Split 2-of-3 */
  gboolean ok = gn_sss_split(fix->test_secret, 32, 2, 3, &shares, &error);
  g_assert_true(ok);

  /* Combine with exactly threshold shares (first 2) */
  GPtrArray *subset = g_ptr_array_new();
  g_ptr_array_add(subset, g_ptr_array_index(shares, 0));
  g_ptr_array_add(subset, g_ptr_array_index(shares, 1));

  guint8 *recovered = NULL;
  gsize recovered_len = 0;
  ok = gn_sss_combine(subset, 2, &recovered, &recovered_len, &error);
  g_assert_no_error(error);
  g_assert_true(ok);
  g_assert_cmpuint(recovered_len, ==, 32);
  g_assert_cmpmem(recovered, 32, fix->test_secret, 32);

  gnostr_secure_free(recovered, recovered_len);
  g_ptr_array_unref(subset);
  gn_sss_shares_free(shares);
}

static void test_sss_combine_different_subsets(SSSFixture *fix, gconstpointer user_data) {
  (void)user_data;
  GError *error = NULL;
  GPtrArray *shares = NULL;

  /* Split 2-of-3 */
  gboolean ok = gn_sss_split(fix->test_secret, 32, 2, 3, &shares, &error);
  g_assert_true(ok);

  /* Test all possible 2-share combinations */
  guint combinations[3][2] = {{0, 1}, {0, 2}, {1, 2}};

  for (guint c = 0; c < 3; c++) {
    GPtrArray *subset = g_ptr_array_new();
    g_ptr_array_add(subset, g_ptr_array_index(shares, combinations[c][0]));
    g_ptr_array_add(subset, g_ptr_array_index(shares, combinations[c][1]));

    guint8 *recovered = NULL;
    gsize recovered_len = 0;
    ok = gn_sss_combine(subset, 2, &recovered, &recovered_len, &error);
    g_assert_no_error(error);
    g_assert_true(ok);
    g_assert_cmpmem(recovered, 32, fix->test_secret, 32);

    gnostr_secure_free(recovered, recovered_len);
    g_ptr_array_unref(subset);
  }

  gn_sss_shares_free(shares);
}

static void test_sss_combine_insufficient(SSSFixture *fix, gconstpointer user_data) {
  (void)user_data;
  GError *error = NULL;
  GPtrArray *shares = NULL;

  /* Split 3-of-5 */
  gboolean ok = gn_sss_split(fix->test_secret, 32, 3, 5, &shares, &error);
  g_assert_true(ok);

  /* Try to combine with only 2 shares (below threshold) */
  GPtrArray *subset = g_ptr_array_new();
  g_ptr_array_add(subset, g_ptr_array_index(shares, 0));
  g_ptr_array_add(subset, g_ptr_array_index(shares, 1));

  guint8 *recovered = NULL;
  gsize recovered_len = 0;
  ok = gn_sss_combine(subset, 3, &recovered, &recovered_len, &error);
  g_assert_false(ok);
  g_assert_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_THRESHOLD_NOT_MET);

  g_clear_error(&error);
  g_ptr_array_unref(subset);
  gn_sss_shares_free(shares);
}

/* ============================================================
 * Share Encoding/Decoding Tests
 * ============================================================ */

static void test_share_encoding(SSSFixture *fix, gconstpointer user_data) {
  (void)user_data;
  GError *error = NULL;
  GPtrArray *shares = NULL;

  gboolean ok = gn_sss_split(fix->test_secret, 32, 2, 3, &shares, &error);
  g_assert_true(ok);

  GnSSSShare *share = g_ptr_array_index(shares, 0);

  /* Encode */
  gchar *encoded = gn_sss_share_encode(share);
  g_assert_nonnull(encoded);
  g_assert_true(g_str_has_prefix(encoded, "sss1:"));

  /* Validate */
  g_assert_true(gn_sss_share_validate(encoded));

  /* Decode */
  GnSSSShare *decoded = gn_sss_share_decode(encoded, &error);
  g_assert_no_error(error);
  g_assert_nonnull(decoded);
  g_assert_cmpuint(decoded->index, ==, share->index);
  g_assert_cmpuint(decoded->data_len, ==, share->data_len);
  g_assert_cmpmem(decoded->data, decoded->data_len, share->data, share->data_len);

  gn_sss_share_free(decoded);
  g_free(encoded);
  gn_sss_shares_free(shares);
}

static void test_share_validation(SSSFixture *fix, gconstpointer user_data) {
  (void)fix;
  (void)user_data;

  /* Valid formats */
  g_assert_true(gn_sss_share_validate("sss1:1:SGVsbG8gV29ybGQ="));
  g_assert_true(gn_sss_share_validate("sss1:255:dGVzdA=="));

  /* Invalid formats */
  g_assert_false(gn_sss_share_validate(NULL));
  g_assert_false(gn_sss_share_validate(""));
  g_assert_false(gn_sss_share_validate("invalid"));
  g_assert_false(gn_sss_share_validate("sss1:"));
  g_assert_false(gn_sss_share_validate("sss1:abc:data"));
  g_assert_false(gn_sss_share_validate("sss2:1:data"));
}

/* ============================================================
 * Guardian Management Tests
 * ============================================================ */

static void test_guardian_new(SSSFixture *fix, gconstpointer user_data) {
  (void)fix;
  (void)user_data;

  GnGuardian *g = gn_guardian_new("npub1test", "Alice");
  g_assert_nonnull(g);
  g_assert_cmpstr(g->npub, ==, "npub1test");
  g_assert_cmpstr(g->label, ==, "Alice");
  g_assert_cmpuint(g->share_index, ==, 0);
  g_assert_false(g->confirmed);

  gn_guardian_free(g);
}

static void test_guardian_dup(SSSFixture *fix, gconstpointer user_data) {
  (void)fix;
  (void)user_data;

  GnGuardian *g = gn_guardian_new("npub1test", "Bob");
  g->share_index = 5;
  g->confirmed = TRUE;

  GnGuardian *dup = gn_guardian_dup(g);
  g_assert_nonnull(dup);
  g_assert_cmpstr(dup->npub, ==, g->npub);
  g_assert_cmpstr(dup->label, ==, g->label);
  g_assert_cmpuint(dup->share_index, ==, g->share_index);
  g_assert_true(dup->confirmed);

  gn_guardian_free(g);
  gn_guardian_free(dup);
}

/* ============================================================
 * Recovery Configuration Tests
 * ============================================================ */

static void test_config_create(SSSFixture *fix, gconstpointer user_data) {
  (void)fix;
  (void)user_data;

  GnRecoveryConfig *config = gn_recovery_config_new("npub1owner");
  g_assert_nonnull(config);
  g_assert_cmpstr(config->owner_npub, ==, "npub1owner");
  g_assert_cmpuint(config->guardians->len, ==, 0);

  gn_recovery_config_free(config);
}

static void test_config_add_guardians(SSSFixture *fix, gconstpointer user_data) {
  (void)fix;
  (void)user_data;

  GnRecoveryConfig *config = gn_recovery_config_new("npub1owner");

  GnGuardian *g1 = gn_guardian_new("npub1alice", "Alice");
  GnGuardian *g2 = gn_guardian_new("npub1bob", "Bob");

  gboolean ok = gn_recovery_config_add_guardian(config, g1);
  g_assert_true(ok);
  g_assert_cmpuint(config->guardians->len, ==, 1);

  ok = gn_recovery_config_add_guardian(config, g2);
  g_assert_true(ok);
  g_assert_cmpuint(config->guardians->len, ==, 2);

  /* Adding duplicate should fail */
  GnGuardian *g3 = gn_guardian_new("npub1alice", "Alice Copy");
  ok = gn_recovery_config_add_guardian(config, g3);
  g_assert_false(ok);
  g_assert_cmpuint(config->guardians->len, ==, 2);

  gn_recovery_config_free(config);
}

static void test_config_serialization(SSSFixture *fix, gconstpointer user_data) {
  (void)fix;
  (void)user_data;
  GError *error = NULL;

  GnRecoveryConfig *config = gn_recovery_config_new("npub1owner123");
  config->threshold = 2;
  config->total_shares = 3;
  config->created_at = 1706000000;

  gn_recovery_config_add_guardian(config, gn_guardian_new("npub1alice", "Alice"));
  gn_recovery_config_add_guardian(config, gn_guardian_new("npub1bob", "Bob"));
  gn_recovery_config_add_guardian(config, gn_guardian_new("npub1charlie", "Charlie"));

  /* Serialize */
  gchar *json = gn_recovery_config_to_json(config);
  g_assert_nonnull(json);

  /* Deserialize */
  GnRecoveryConfig *loaded = gn_recovery_config_from_json(json, &error);
  g_assert_no_error(error);
  g_assert_nonnull(loaded);
  g_assert_cmpstr(loaded->owner_npub, ==, config->owner_npub);
  g_assert_cmpuint(loaded->threshold, ==, config->threshold);
  g_assert_cmpuint(loaded->total_shares, ==, config->total_shares);
  g_assert_cmpuint(loaded->guardians->len, ==, config->guardians->len);

  g_free(json);
  gn_recovery_config_free(config);
  gn_recovery_config_free(loaded);
}

/* ============================================================
 * Utility Tests
 * ============================================================ */

static void test_validate_threshold(SSSFixture *fix, gconstpointer user_data) {
  (void)fix;
  (void)user_data;
  GError *error = NULL;

  /* Valid thresholds */
  g_assert_true(gn_social_recovery_validate_threshold(2, 3, &error));
  g_assert_no_error(error);
  g_assert_true(gn_social_recovery_validate_threshold(3, 5, &error));
  g_assert_no_error(error);

  /* Invalid: threshold < 2 */
  g_assert_false(gn_social_recovery_validate_threshold(1, 3, &error));
  g_assert_nonnull(error);
  g_clear_error(&error);

  /* Invalid: threshold > total */
  g_assert_false(gn_social_recovery_validate_threshold(5, 3, &error));
  g_assert_nonnull(error);
  g_clear_error(&error);

  /* Invalid: total = 0 */
  g_assert_false(gn_social_recovery_validate_threshold(2, 0, &error));
  g_assert_nonnull(error);
  g_clear_error(&error);
}

/* ============================================================
 * Main
 * ============================================================ */

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);

  /* SSS core tests */
  g_test_add("/social-recovery/sss/split-basic", SSSFixture, NULL,
             fixture_setup, test_sss_split_basic, fixture_teardown);
  g_test_add("/social-recovery/sss/split-thresholds", SSSFixture, NULL,
             fixture_setup, test_sss_split_thresholds, fixture_teardown);
  g_test_add("/social-recovery/sss/split-invalid", SSSFixture, NULL,
             fixture_setup, test_sss_split_invalid_params, fixture_teardown);
  g_test_add("/social-recovery/sss/combine-basic", SSSFixture, NULL,
             fixture_setup, test_sss_combine_basic, fixture_teardown);
  g_test_add("/social-recovery/sss/combine-subsets", SSSFixture, NULL,
             fixture_setup, test_sss_combine_different_subsets, fixture_teardown);
  g_test_add("/social-recovery/sss/combine-insufficient", SSSFixture, NULL,
             fixture_setup, test_sss_combine_insufficient, fixture_teardown);

  /* Share encoding tests */
  g_test_add("/social-recovery/share/encoding", SSSFixture, NULL,
             fixture_setup, test_share_encoding, fixture_teardown);
  g_test_add("/social-recovery/share/validation", SSSFixture, NULL,
             fixture_setup, test_share_validation, fixture_teardown);

  /* Guardian tests */
  g_test_add("/social-recovery/guardian/new", SSSFixture, NULL,
             fixture_setup, test_guardian_new, fixture_teardown);
  g_test_add("/social-recovery/guardian/dup", SSSFixture, NULL,
             fixture_setup, test_guardian_dup, fixture_teardown);

  /* Config tests */
  g_test_add("/social-recovery/config/create", SSSFixture, NULL,
             fixture_setup, test_config_create, fixture_teardown);
  g_test_add("/social-recovery/config/add-guardians", SSSFixture, NULL,
             fixture_setup, test_config_add_guardians, fixture_teardown);
  g_test_add("/social-recovery/config/serialization", SSSFixture, NULL,
             fixture_setup, test_config_serialization, fixture_teardown);

  /* Utility tests */
  g_test_add("/social-recovery/util/validate-threshold", SSSFixture, NULL,
             fixture_setup, test_validate_threshold, fixture_teardown);

  return g_test_run();
}
