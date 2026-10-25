#pragma once
#include <blossom/pool.h>
typedef blossom_pool_t nh_porthome_blossom_t;
typedef blossom_pool_opts_t nh_porthome_blossom_opts_t;
typedef blossom_pool_status nh_porthome_blossom_status;
typedef blossom_pool_batch_blob_t nh_porthome_blossom_batch_blob_t;
typedef blossom_pool_batch_per_server_t nh_porthome_blossom_batch_per_server_t;
#define NH_PORTHOME_BLOSSOM_DEFAULT_MAX_BLOB_BYTES BLOSSOM_POOL_DEFAULT_MAX_BLOB_BYTES
#define NH_PORTHOME_BLOSSOM_DEFAULT_MAX_RETRIES BLOSSOM_POOL_DEFAULT_MAX_RETRIES
#define NH_PORTHOME_BLOSSOM_DEFAULT_TIMEOUT_S BLOSSOM_POOL_DEFAULT_TIMEOUT_S
#define NH_PORTHOME_BLOSSOM_ERR_ARG BLOSSOM_POOL_ERR_ARG
#define NH_PORTHOME_BLOSSOM_ERR_AUTH BLOSSOM_POOL_ERR_AUTH
#define NH_PORTHOME_BLOSSOM_ERR_HASH_MISMATCH BLOSSOM_POOL_ERR_HASH_MISMATCH
#define NH_PORTHOME_BLOSSOM_ERR_HTTPS_ONLY BLOSSOM_POOL_ERR_HTTPS_ONLY
#define NH_PORTHOME_BLOSSOM_ERR_NETWORK BLOSSOM_POOL_ERR_NETWORK
#define NH_PORTHOME_BLOSSOM_ERR_NOT_FOUND BLOSSOM_POOL_ERR_NOT_FOUND
#define NH_PORTHOME_BLOSSOM_ERR_OOM BLOSSOM_POOL_ERR_OOM
#define NH_PORTHOME_BLOSSOM_ERR_TOO_LARGE BLOSSOM_POOL_ERR_TOO_LARGE
#define NH_PORTHOME_BLOSSOM_OK BLOSSOM_POOL_OK

#ifdef __cplusplus
extern "C" {
#endif
int nh_porthome_blossom_url_ok(const char *url);

int nh_porthome_blossom_new(const nh_porthome_blossom_opts_t *opts,
                            const blossom_signer_t *signer,
                            nh_porthome_blossom_t **out);
void nh_porthome_blossom_free(nh_porthome_blossom_t *c);


int nh_porthome_blossom_upload(nh_porthome_blossom_t *c,
                               const uint8_t *data, size_t len,
                               const char *expected_sha256_hex,
                               char out_sha256_hex[65]);


int nh_porthome_blossom_fetch(nh_porthome_blossom_t *c,
                              const char *sha256_hex,
                              uint8_t **out_data, size_t *out_len);


int nh_porthome_blossom_has(nh_porthome_blossom_t *c,
                            const char *sha256_hex,
                            bool *out_exists);


int nh_porthome_blossom_client_delete(nh_porthome_blossom_t *c,
                               const char *sha256_hex);










int nh_porthome_blossom_upload_batch(nh_porthome_blossom_t *c,
                                     nh_porthome_blossom_batch_blob_t *blobs,
                                     size_t n,
                                     nh_porthome_blossom_batch_per_server_t *out_per_server);


#ifdef __cplusplus
}
#endif
