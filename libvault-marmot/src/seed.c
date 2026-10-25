/* SPDX-License-Identifier: MIT */
#include <vault-marmot/vault-marmot.h>
#include <openssl/crypto.h>

MarmotError vault_marmot_seed(Marmot *m, const MarmotGroupId *group,
                               const uint8_t vault_id[32], uint8_t seed[32])
{
    static const uint8_t label[] = VAULT_MARMOT_EXPORTER_LABEL;
    if (seed)
        OPENSSL_cleanse(seed, 32);
    if (!vault_id || !seed)
        return MARMOT_ERR_INVALID_ARG;
    return marmot_export_secret(m, group, label, sizeof label - 1, vault_id, 32, seed, 32);
}
