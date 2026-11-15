#include <fips/proto/path/wire.h>
#include <stdlib.h>
#include <string.h>
/* Both arbitrary bodies and canonicalization: unknown flags are retained on
 * ingress but cleared on egress, and unknown optional TLVs are not echoed. */
int LLVMFuzzerTestOneInput(const uint8_t *data,size_t size)
{
    if(size>65536) return 0;
    uint8_t bytes[65564],again[65564]; fips_reader r; fips_writer w,q;
    fips_proto_error e; fips_path_probe p; fips_path_ack a; fips_path_close c;
    for(unsigned kind=0;kind<3;++kind) {
        fips_reader_init(&r,data,size); fips_writer_init(&w,bytes,sizeof(bytes));
        fips_status s=kind==0?fips_path_probe_decode(&r,&p,&e):kind==1?fips_path_ack_decode(&r,&a,&e):fips_path_close_decode(&r,&c,&e);
        if(s!=FIPS_OK) { if(r.pos) abort(); continue; }
        s=kind==0?fips_path_probe_encode(&p,&w,&e):kind==1?fips_path_ack_encode(&a,&w,&e):fips_path_close_encode(&c,&w,&e);
        if(s!=FIPS_OK) abort();
        fips_reader_init(&r,bytes+1,w.pos-1); fips_writer_init(&q,again,sizeof(again));
        s=kind==0?fips_path_probe_decode(&r,&p,&e):kind==1?fips_path_ack_decode(&r,&a,&e):fips_path_close_decode(&r,&c,&e);
        if(s!=FIPS_OK) abort();
        s=kind==0?fips_path_probe_encode(&p,&q,&e):kind==1?fips_path_ack_encode(&a,&q,&e):fips_path_close_encode(&c,&q,&e);
        if(s!=FIPS_OK||q.pos!=w.pos||memcmp(bytes,again,w.pos)) abort();
        fips_writer_init(&q,again,w.pos-1); memset(again,0xa5,w.pos);
        s=kind==0?fips_path_probe_encode(&p,&q,&e):kind==1?fips_path_ack_encode(&a,&q,&e):fips_path_close_encode(&c,&q,&e);
        if(s!=FIPS_ERR_RANGE||q.pos) abort();
        for(size_t i=0;i<w.pos;++i) if(again[i]!=0xa5) abort();
    }
    return 0;
}
