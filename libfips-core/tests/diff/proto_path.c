#include "proto_path.h"
#include <fips/proto/path/wire.h>
#include <stdio.h>
#include <string.h>
static bool error_matches(const proto_case *c,const fips_proto_error *e)
{
    const diff_value *v=FIELD(c->record,"empty_input_error");
    return e->variant==FIPS_PROTO_MessageTooShort&&!strcmp(fips_proto_error_name(e->variant),TEXT(v,"variant"))&&
        e->as.message_too_short.expected==UINT(v,"expected")&&e->as.message_too_short.got==UINT(v,"got");
}
static bool boolean(const diff_value *v,const char *field) { const diff_value *b=FIELD(v,field); return b&&b->type==D_BOOL&&b->as.boolean; }
static result check(diff_context *ctx,const proto_case *c,fips_link_message_type type)
{
    const diff_value *in=FIELD(c->record,"input"); diff_bytes bytes=BYTES(c->record,"bytes");
    if(!bytes.len||bytes.len>4096||UINT(c->record,"decode_offset")!=1) return diff_error(ctx,"path size/offset"),FAILED;
    uint8_t encoded[4096]; char debug[512]; fips_writer w; fips_reader r; fips_proto_error e;
    fips_writer_init(&w,encoded,sizeof(encoded)); fips_reader_init(&r,bytes.data+1,bytes.len-1);
    uint32_t path=(uint32_t)UINT(in,"path_id");
    if(type==FIPS_LINK_PathProbe) {
        fips_path_probe p=fips_path_probe_new((uint32_t)UINT(in,"probe_id"),path,boolean(in,"active")?FIPS_PATH_ACTIVE:0,boolean(in,"aggregate")?FIPS_PATH_AGGREGATE:0),d;
        p.len=(size_t)UINT(in,"len");
        if(fips_path_probe_encode(&p,&w,NULL)!=FIPS_OK||fips_path_probe_decode(&r,&d,NULL)!=FIPS_OK) return diff_error(ctx,"path probe codec"),FAILED;
        if(d.path_id!=p.path_id||d.probe_id!=p.probe_id||d.flags!=p.flags||d.caps!=p.caps||d.len!=(p.len<27?27:p.len)) return diff_error(ctx,"path probe fields"),FAILED;
        snprintf(debug,sizeof(debug),"PathProbe { probe_id: %u, flags: PathFlags(%u), path_id: %u, caps: PathCaps(%u), len: %zu }",(unsigned)d.probe_id,(unsigned)d.flags,(unsigned)d.path_id,(unsigned)d.caps,d.len);
        fips_reader_init(&r,NULL,0);
        if(fips_path_probe_decode(&r,&d,&e)!=FIPS_ERR_RANGE||!error_matches(c,&e)) return diff_error(ctx,"path probe error"),FAILED;
    } else if(type==FIPS_LINK_PathAck) {
        fips_path_ack p={.probe_id=(uint32_t)UINT(in,"probe_id"),.flags=boolean(in,"active")?FIPS_PATH_ACTIVE:0,.path_id=path,
            .echo_path_id=(uint32_t)UINT(in,"echo_path_id"),.caps=boolean(in,"aggregate")?FIPS_PATH_AGGREGATE:0,
            .rx_time_us=(uint32_t)UINT(in,"rx_time_us"),.rx_frames=(uint32_t)UINT(in,"rx_frames"),.rx_bytes=(uint32_t)UINT(in,"rx_bytes"),.len=(size_t)UINT(in,"len")},d;
        if(fips_path_ack_encode(&p,&w,NULL)!=FIPS_OK||fips_path_ack_decode(&r,&d,NULL)!=FIPS_OK) return diff_error(ctx,"path ack codec"),FAILED;
        if(d.path_id!=p.path_id||d.probe_id!=p.probe_id||d.flags!=p.flags||d.caps!=p.caps||d.echo_path_id!=p.echo_path_id||
            d.rx_time_us!=p.rx_time_us||d.rx_frames!=p.rx_frames||d.rx_bytes!=p.rx_bytes||d.len!=(p.len<27?27:p.len)) return diff_error(ctx,"path ack fields"),FAILED;
        snprintf(debug,sizeof(debug),"PathAck { probe_id: %u, flags: PathFlags(%u), path_id: %u, echo_path_id: %u, caps: PathCaps(%u), rx_time_us: %u, rx_frames: %u, rx_bytes: %u, len: %zu }",(unsigned)d.probe_id,(unsigned)d.flags,(unsigned)d.path_id,(unsigned)d.echo_path_id,(unsigned)d.caps,(unsigned)d.rx_time_us,(unsigned)d.rx_frames,(unsigned)d.rx_bytes,d.len);
        fips_reader_init(&r,NULL,0);
        if(fips_path_ack_decode(&r,&d,&e)!=FIPS_ERR_RANGE||!error_matches(c,&e)) return diff_error(ctx,"path ack error"),FAILED;
    } else {
        const char *names[]={"Unspecified","InterfaceGone","CarrierLost","Operator"}; size_t reason=0;
        while(reason<4&&strcmp(TEXT(in,"reason"),names[reason])) ++reason;
        if(reason==4) return diff_error(ctx,"path close reason"),FAILED;
        fips_path_close p={path,(fips_path_close_reason)reason},d;
        if(fips_path_close_encode(&p,&w,NULL)!=FIPS_OK||fips_path_close_decode(&r,&d,NULL)!=FIPS_OK) return diff_error(ctx,"path close codec"),FAILED;
        if(d.path_id!=p.path_id||d.reason!=p.reason) return diff_error(ctx,"path close fields"),FAILED;
        snprintf(debug,sizeof(debug),"PathClose { path_id: %u, reason: %s }",(unsigned)d.path_id,names[d.reason]);
        fips_reader_init(&r,NULL,0);
        if(fips_path_close_decode(&r,&d,&e)!=FIPS_ERR_RANGE||!error_matches(c,&e)) return diff_error(ctx,"path close error"),FAILED;
    }
    if(!diff_compare(ctx->id,bytes.data,bytes.len,encoded,w.pos)||strcmp(debug,TEXT(c->record,"decoded_debug"))) return diff_error(ctx,"path wire/Debug mismatch"),FAILED;
    return CHECKED;
}
result check_proto_path_probe(diff_context *ctx,const proto_case *c) { return check(ctx,c,FIPS_LINK_PathProbe); }
result check_proto_path_ack(diff_context *ctx,const proto_case *c) { return check(ctx,c,FIPS_LINK_PathAck); }
result check_proto_path_close(diff_context *ctx,const proto_case *c) { return check(ctx,c,FIPS_LINK_PathClose); }
