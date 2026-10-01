/* What a modified Marmot client can send and Groundhog must refuse
 * (nostrc-6ukh): an Add Commit whose new leaf claims an account with an
 * account proof another account signed. Built on libmarmot's internal MLS
 * layer, as libmarmot's own tests do (libmarmot/tests/test_commits.c
 * forge_add_commit()). Header-only, for test_mls_service.c alone. */
#ifndef GH_TEST_MLS_FORGE_H
#define GH_TEST_MLS_FORGE_H

#include "mls-world.h"

#include "commits.h"
#include "kp_profile.h"
#include "marmot-internal.h"
#include "mls/mls_group.h"
#include "mls/mls_key_package.h"

#include <sodium.h>

/* `admin`'s modified client: the kind 445 of an Add of a leaf claiming
 * account `claimed`, with a proof `forger` signed over the leaf's key. The
 * admin's own state is not changed. */
static G_GNUC_UNUSED gchar *
forge_add_with_bad_proof(App *admin, GhMlsGroup *group, guint claimed, guint forger)
{
  Marmot *m = gh_mls_service_get_marmot(admin->service);
  const gchar *gid_hex = gh_mls_group_get_group_id(group);
  gsize gid_len = strlen(gid_hex) / 2;
  guint8 *gid_bytes = g_malloc(gid_len);
  g_assert_true(nostr_hex2bin(gid_bytes, gid_hex, gid_len));
  MarmotGroupId gid = marmot_group_id_new(gid_bytes, gid_len);
  g_free(gid_bytes);
  uint8_t *blob = NULL;
  size_t len = 0;
  g_assert_cmpint(m->storage->mls_load(m->storage->ctx, "mls_group", gid.data, gid.len, &blob,
                                       &len), ==, MARMOT_OK);
  MlsGroup g;
  memset(&g, 0, sizeof g);
  g_assert_cmpint(mls_group_deserialize(blob, len, &g), ==, 0);
  sodium_memzero(blob, len);
  free(blob);
  MarmotGroup *info = NULL;
  g_assert_cmpint(marmot_get_group(m, &gid, &info), ==, MARMOT_OK);

  guint8 claimed_pk[32], forger_pk[32], forger_sk[32];
  g_assert_true(nostr_hex2bin(claimed_pk, hex[claimed], 32));
  g_assert_true(nostr_hex2bin(forger_pk, hex[forger], 32));
  g_assert_true(nostr_hex2bin(forger_sk, gh_test_secret[forger], 32));
  MlsKeyPackage kp;
  MlsKeyPackagePrivate priv;
  g_assert_cmpint(mls_key_package_create_unsigned(&kp, &priv, claimed_pk, 32, NULL, 0), ==, 0);
  uint8_t proof[MARMOT_ACCOUNT_PROOF_LEN];
  g_assert_cmpint(marmot_account_proof_create(forger_pk, forger_sk, NULL, NULL,
                                              MARMOT_CIPHERSUITE,
                                              MARMOT_SIGNATURE_SCHEME_ED25519,
                                              kp.leaf_node.signature_key, MLS_SIG_PK_LEN,
                                              (uint64_t)(g_get_real_time() / G_USEC_PER_SEC),
                                              proof), ==, MARMOT_OK);
  g_assert_cmpint(marmot_leaf_set_proof(&kp.leaf_node, proof), ==, MARMOT_OK);
  g_assert_cmpint(mls_key_package_sign(&kp, &priv), ==, 0);
  g_assert_cmpint(marmot_leaf_proof_status(&kp.leaf_node, MARMOT_CIPHERSUITE), ==,
                  MARMOT_LEAF_PROOF_INVALID);

  uint8_t exporter[32];
  memcpy(exporter, g.epoch_secrets.exporter_secret, 32);
  MlsAddResult add;
  memset(&add, 0, sizeof add);
  g_assert_cmpint(mls_group_add_member(&g, &kp, &add), ==, 0);
  char *json = marmot_commit_build_event(add.commit_data, add.commit_len, exporter,
                                         info->nostr_group_id, g_get_real_time() / G_USEC_PER_SEC);
  g_assert_nonnull(json);
  gchar *out = g_strdup(json);
  free(json);
  sodium_memzero(exporter, sizeof exporter);
  sodium_memzero(forger_sk, sizeof forger_sk);
  mls_add_result_clear(&add);
  mls_key_package_clear(&kp);
  mls_key_package_private_clear(&priv);
  mls_group_free(&g);
  marmot_group_free(info);
  marmot_group_id_free(&gid);
  return out;
}

/* `committer`'s modified client, in an adopted group: the kind 445 of a
 * Commit of one AppDataUpdate (component `id` set to `data`), however the
 * group judges it (W24b slice H review L2).  The committer's own state is
 * not changed. */
static G_GNUC_UNUSED gchar *
forge_adopted_update(App *committer, GhMlsGroup *group, uint16_t id, const uint8_t *data,
                     size_t data_len)
{
  Marmot *m = gh_mls_service_get_marmot(committer->service);
  const gchar *gid_hex = gh_mls_group_get_group_id(group);
  gsize gid_len = strlen(gid_hex) / 2;
  guint8 *gid_bytes = g_malloc(gid_len);
  g_assert_true(nostr_hex2bin(gid_bytes, gid_hex, gid_len));
  MarmotGroupId gid = marmot_group_id_new(gid_bytes, gid_len);
  g_free(gid_bytes);
  uint8_t *blob = NULL;
  size_t len = 0;
  g_assert_cmpint(m->storage->mls_load(m->storage->ctx, "mls_group", gid.data, gid.len, &blob,
                                       &len), ==, MARMOT_OK);
  MlsGroup g;
  memset(&g, 0, sizeof g);
  g_assert_cmpint(mls_group_deserialize(blob, len, &g), ==, 0);
  sodium_memzero(blob, len);
  free(blob);
  MarmotGroup *info = NULL;
  g_assert_cmpint(marmot_get_group(m, &gid, &info), ==, MARMOT_OK);
  uint8_t exporter[32];
  memcpy(exporter, g.epoch_secrets.exporter_secret, 32);
  MlsAppDataUpdate op = { id, MLS_APP_DATA_UPDATE_OP_UPDATE, (uint8_t *)data, data_len };
  MlsAddResult res;
  memset(&res, 0, sizeof res);
  g_assert_cmpint(mls_group_commit_adopted(&g, NULL, 0, NULL, 0, &op, 1, &res), ==, 0);
  char *json = marmot_commit_build_event(res.commit_data, res.commit_len, exporter,
                                         info->nostr_group_id, g_get_real_time() / G_USEC_PER_SEC);
  g_assert_nonnull(json);
  gchar *out = g_strdup(json);
  free(json);
  sodium_memzero(exporter, sizeof exporter);
  mls_add_result_clear(&res);
  mls_group_free(&g);
  marmot_group_free(info);
  marmot_group_id_free(&gid);
  return out;
}

/* `admin`'s modified client: one Commit removing `claimed`'s leaf and adding
 * a leaf that claims `claimed`, made of fresh keys of the admin's (no proof):
 * the new leaf lands in the old slot (W24 review B1). libmarmot judges it a
 * new member the admin added, never `claimed`'s own renewal. The admin's own
 * state is not changed. */
static G_GNUC_UNUSED gchar *
forge_replace(App *admin, GhMlsGroup *group, guint claimed)
{
  Marmot *m = gh_mls_service_get_marmot(admin->service);
  const gchar *gid_hex = gh_mls_group_get_group_id(group);
  gsize gid_len = strlen(gid_hex) / 2;
  guint8 *gid_bytes = g_malloc(gid_len);
  g_assert_true(nostr_hex2bin(gid_bytes, gid_hex, gid_len));
  MarmotGroupId gid = marmot_group_id_new(gid_bytes, gid_len);
  g_free(gid_bytes);
  uint8_t *blob = NULL;
  size_t len = 0;
  g_assert_cmpint(m->storage->mls_load(m->storage->ctx, "mls_group", gid.data, gid.len, &blob,
                                       &len), ==, MARMOT_OK);
  MlsGroup g;
  memset(&g, 0, sizeof g);
  g_assert_cmpint(mls_group_deserialize(blob, len, &g), ==, 0);
  sodium_memzero(blob, len);
  free(blob);
  MarmotGroup *info = NULL;
  g_assert_cmpint(marmot_get_group(m, &gid, &info), ==, MARMOT_OK);
  guint8 claimed_pk[32];
  g_assert_true(nostr_hex2bin(claimed_pk, hex[claimed], 32));
  uint32_t slot = UINT32_MAX;
  for (uint32_t i = 0; i < g.tree.n_leaves; i++) {
    uint8_t id[32];
    if (marmot_mls_sender_identity(&g, i, id) == 0 && memcmp(id, claimed_pk, 32) == 0)
      slot = i;
  }
  g_assert_cmpuint(slot, !=, UINT32_MAX);
  MlsKeyPackage kp;
  MlsKeyPackagePrivate priv;
  g_assert_cmpint(mls_key_package_create_unsigned(&kp, &priv, claimed_pk, 32, NULL, 0), ==, 0);
  g_assert_cmpint(mls_key_package_sign(&kp, &priv), ==, 0);
  uint8_t exporter[32];
  memcpy(exporter, g.epoch_secrets.exporter_secret, 32);
  const MlsKeyPackage *kps[] = { &kp };
  MlsAddResult add;
  memset(&add, 0, sizeof add);
  g_assert_cmpint(mls_group_replace_members(&g, &slot, 1, kps, 1, &add), ==, 0);
  char *json = marmot_commit_build_event(add.commit_data, add.commit_len, exporter,
                                         info->nostr_group_id, g_get_real_time() / G_USEC_PER_SEC);
  g_assert_nonnull(json);
  gchar *out = g_strdup(json);
  free(json);
  sodium_memzero(exporter, sizeof exporter);
  mls_add_result_clear(&add);
  mls_key_package_clear(&kp);
  mls_key_package_private_clear(&priv);
  mls_group_free(&g);
  marmot_group_free(info);
  marmot_group_id_free(&gid);
  return out;
}

#endif
