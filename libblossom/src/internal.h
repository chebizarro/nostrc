#ifndef BLOSSOM_INTERNAL_H
#define BLOSSOM_INTERNAL_H
#include <blossom/client.h>
/* Pre-addressed transport used after the pool prepares one wire representation. */
blossom_error_t blossom_put_wire(blossom_client_t *, const uint8_t *, size_t, const char *, blossom_blob_descriptor_t **);
blossom_error_t blossom_put_batch_wire(blossom_client_t *, const blossom_blob_t *, size_t, blossom_batch_result_t *);
#endif
