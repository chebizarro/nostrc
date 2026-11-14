#include <fips/proto/lookup/mod.h>
#include <stdlib.h>
#include <string.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > FIPS_LOOKUP_RESPONSE_MAX_WIRE_SIZE) return 0;
    fips_lookup_request *req = malloc(sizeof(*req)), *req2 = malloc(sizeof(*req2));
    fips_lookup_response *resp = malloc(sizeof(*resp)), *resp2 = malloc(sizeof(*resp2));
    uint8_t *encoded = malloc(FIPS_LOOKUP_RESPONSE_MAX_WIRE_SIZE);
    if (!req || !req2 || !resp || !resp2 || !encoded) abort();
    fips_reader r;
    fips_writer w;
    fips_proto_error e;
    fips_reader_init(&r, data, size);
    if (fips_lookup_request_decode(&r, req, &e) == FIPS_OK) {
        size_t used = r.pos;
        fips_writer_init(&w, encoded, FIPS_LOOKUP_RESPONSE_MAX_WIRE_SIZE);
        if (fips_lookup_request_encode(req, &w, &e) != FIPS_OK || w.pos != used + 1 ||
            encoded[0] != 0x30 || memcmp(encoded + 1, data, used)) abort();
        fips_reader_init(&r, encoded + 1, w.pos - 1);
        if (fips_lookup_request_decode(&r, req2, &e) != FIPS_OK || r.pos != used ||
            !fips_tree_coordinate_equal(&req->origin_coords, &req2->origin_coords)) abort();
    } else if (r.pos) abort();
    fips_reader_init(&r, data, size);
    if (fips_lookup_response_decode(&r, resp, &e) == FIPS_OK) {
        size_t used = r.pos;
        fips_writer_init(&w, encoded, FIPS_LOOKUP_RESPONSE_MAX_WIRE_SIZE);
        if (fips_lookup_response_encode(resp, &w, &e) != FIPS_OK || w.pos != used + 1 ||
            encoded[0] != 0x31 || memcmp(encoded + 1, data, used)) abort();
        fips_reader_init(&r, encoded + 1, w.pos - 1);
        if (fips_lookup_response_decode(&r, resp2, &e) != FIPS_OK || r.pos != used ||
            memcmp(resp->proof, resp2->proof, 64) || !fips_tree_coordinate_equal(&resp->target_coords, &resp2->target_coords)) abort();
        fips_writer_init(&w, encoded, FIPS_LOOKUP_RESPONSE_MAX_WIRE_SIZE);
        if (fips_lookup_response_proof_bytes(resp->request_id, &resp->target, &resp->target_coords, &w, &e) != FIPS_OK) abort();
    } else if (r.pos) abort();
    free(req); free(req2); free(resp); free(resp2); free(encoded);
    return 0;
}
