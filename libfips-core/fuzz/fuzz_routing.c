#include <fips/proto/routing/wire.h>
#include <stdlib.h>
#include <string.h>
/* Harness-owned storage: the core allocates nothing. Input starts at sig_flags,
 * matching the post-dispatch body API. Try every decoder, not a guessed type. */
static fips_tree_coordinate coords;
static uint8_t encoded[FIPS_ROUTING_MAX_WIRE_SIZE];
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    for (unsigned kind = 0; kind < 3; ++kind) {
        fips_reader r; fips_reader_init(&r,data,size);
        fips_writer w; fips_writer_init(&w,encoded,sizeof(encoded));
        fips_status status;
        fips_proto_error e = {.variant = FIPS_PROTO_TtlExpired};
        fips_coords_required cr = {{{0}},{{0}}};
        fips_mtu_exceeded me = {{{0}},{{0}},0};
        fips_path_broken pb = {{{0}},{{0}},NULL};
        if (kind == 0) status = fips_coords_required_decode(&r,&cr,&e);
        else if (kind == 1) status = fips_path_broken_decode(&r,&pb,&coords,&e);
        else status = fips_mtu_exceeded_decode(&r,&me,&e);
        if (status != FIPS_OK) {
            if (r.pos != 0 || cr.dest_addr.bytes[0] || me.mtu || pb.last_known_coords) abort();
            continue;
        }
        if (r.pos != size || e.variant != FIPS_PROTO_TtlExpired) abort();
        if (kind == 0) status = fips_coords_required_encode(&cr,&w,NULL);
        else if (kind == 1) status = fips_path_broken_encode(&pb,&w,NULL);
        else status = fips_mtu_exceeded_encode(&me,&w,NULL);
        if (status != FIPS_OK || w.pos != size+5 || memcmp(encoded+5,data,size)) abort();
        /* Every successful parse must also refuse a short writer atomically. */
        uint8_t short_buf[1] = {0xa5}; fips_writer_init(&w,short_buf,1);
        if (kind == 0) status = fips_coords_required_encode(&cr,&w,NULL);
        else if (kind == 1) status = fips_path_broken_encode(&pb,&w,NULL);
        else status = fips_mtu_exceeded_encode(&me,&w,NULL);
        if (status != FIPS_ERR_RANGE || w.pos || short_buf[0] != 0xa5) abort();
    }
    return 0;
}
