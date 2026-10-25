/* SPDX-License-Identifier: MIT */
#include "test_helpers.h"

int main(void) {
    const size_t len = 4u * PORTHOME_CHUNK_MAX;
    uint8_t *data = malloc(len), *edited = malloc(len + 38);
    assert(data && edited);
    uint32_t state = UINT32_C(0x12345678);
    for (size_t i = 0; i < len; ++i) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        data[i] = (uint8_t)state;
    }
    /* Independent Python buzhash reference over the fixed xorshift32 bytes. */
    const size_t ends[] = {165895,292695,1059416,1296655,1382158,1668025,
        2021376,2424373,2854952,2984429,3085032,3389663,3482846,4145571,4194304};
    size_t offset = 0;
    for (size_t i = 0; i < sizeof(ends)/sizeof(ends[0]); ++i) {
        size_t cut = porthome_chunk_next(data+offset, len-offset);
        assert(cut <= PORTHOME_CHUNK_MAX);
        assert(cut >= PORTHOME_CHUNK_MIN || offset+cut == len);
        assert(offset+cut == ends[i]);
        assert(porthome_chunk_next(data+offset, len-offset) == cut);
        offset += cut;
    }
    assert(offset == len);
    const char inserted[] = "inserted content for locality test!!!!";
    assert(sizeof(inserted)-1 == 38);
    memcpy(edited, data, 1024);
    memcpy(edited+1024, inserted, 38);
    memcpy(edited+1062, data+1024, len-1024);
    offset = 0;
    for (size_t i = 0; i < sizeof(ends)/sizeof(ends[0]); ++i) {
        size_t cut = porthome_chunk_next(edited+offset, len+38-offset);
        assert(offset+cut == ends[i]+38);
        offset += cut;
    }
    /* Periodic fixture has no boundary hit: exercise the forced maximum. */
    for (size_t i = 0; i < len; ++i) data[i] = (uint8_t)(i % 64);
    assert(porthome_chunk_next(data, len) == PORTHOME_CHUNK_MAX);
    memset(data, 0, len);
    assert(porthome_chunk_next(data, len) == PORTHOME_CHUNK_MIN);
    assert(porthome_chunk_next(data, 1) == 1);
    assert(porthome_chunk_next(data, PORTHOME_CHUNK_MIN-1) == PORTHOME_CHUNK_MIN-1);
    assert(porthome_chunk_next(data, PORTHOME_CHUNK_MIN) == PORTHOME_CHUNK_MIN);
    assert(porthome_chunk_next(data, 0) == 0);
    assert(porthome_chunk_next(NULL, 0) == 0);
    assert(porthome_chunk_next(NULL, 100) == 0);
    free(data); free(edited);
    return 0;
}
