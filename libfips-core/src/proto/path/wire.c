#include <fips/proto/path/wire.h>
#include <fips/proto/fmp/wire.h>
#include <string.h>
static fips_status malformed(fips_proto_error *e, const char *why)
{
    if (e) *e=(fips_proto_error){.variant=FIPS_PROTO_Malformed,.as.malformed=why};
    return FIPS_ERR_RANGE;
}
static fips_status room(fips_writer *w, size_t n, fips_proto_error *e)
{
    if (n<=fips_writer_remaining(w)) return FIPS_OK;
    if (e) *e=(fips_proto_error){.variant=FIPS_PROTO_MessageTooLong,
        .as.message_too_long={w->capacity,n>SIZE_MAX-w->pos?SIZE_MAX:w->pos+n}};
    return FIPS_ERR_RANGE;
}
static fips_status extension(fips_reader r, fips_proto_error *e)
{
    uint16_t len;
    if (fips_reader_remaining(&r)<2) return malformed(e,"path extension block overruns the message");
    fips_reader_read_u16_le(&r,&len,NULL);
    if (len>fips_reader_remaining(&r)) return malformed(e,"path extension block overruns the message");
    fips_reader block; fips_reader_init(&block,fips_reader_rest(&r),len);
    while (fips_reader_remaining(&block)) {
        uint16_t type,n;
        if (fips_reader_remaining(&block)<4) return malformed(e,"path extension TLV overruns the block");
        fips_reader_read_u16_le(&block,&type,NULL);
        fips_reader_read_u16_le(&block,&n,NULL);
        if (n>fips_reader_remaining(&block)) return malformed(e,"path extension TLV overruns the block");
        if (type & FIPS_PATH_TLV_CRITICAL) return malformed(e,"unknown critical path extension TLV");
        fips_reader_advance(&block,n,NULL);
    }
    return FIPS_OK;
}
fips_path_probe fips_path_probe_new(uint32_t id,uint32_t path,fips_path_flags flags,fips_path_caps caps)
{ return (fips_path_probe){id,flags,path,caps,FIPS_PATH_PROBE_MIN_SIZE}; }
bool fips_path_probe_answerable(const fips_path_probe *p) { return p->len>=FIPS_PATH_PROBE_MIN_SIZE; }
size_t fips_path_ack_len(size_t probe,size_t mtu)
{
    size_t body=mtu>FIPS_FRAMING_FMP_FRAME_OVERHEAD?mtu-FIPS_FRAMING_FMP_FRAME_OVERHEAD:0;
    size_t n=probe<body?probe:body;
    return n<FIPS_PATH_ACK_FIXED_SIZE?FIPS_PATH_ACK_FIXED_SIZE:n;
}
fips_path_close_reason fips_path_close_reason_from_byte(uint8_t b)
{ return b<=3?(fips_path_close_reason)b:FIPS_PATH_Unspecified; }
fips_status fips_path_probe_encode(const fips_path_probe *p,fips_writer *w,fips_proto_error *e)
{
    size_t n=p->len<27?27:p->len;
    if (room(w,n,e)!=FIPS_OK) return FIPS_ERR_RANGE;
    size_t end=w->pos+n;
    fips_writer_write_u8(w,FIPS_LINK_PathProbe,NULL);
    fips_writer_write_u32_le(w,p->probe_id,NULL);
    fips_writer_write_u8(w,p->flags & FIPS_PATH_ACTIVE,NULL);
    fips_writer_write_u32_le(w,p->path_id,NULL);
    fips_writer_write_u8(w,p->caps & FIPS_PATH_AGGREGATE,NULL);
    memset(w->buf+w->pos,0,end-w->pos); w->pos=end;
    return FIPS_OK;
}
fips_status fips_path_probe_decode(fips_reader *r,fips_path_probe *p,fips_proto_error *e)
{
    if (fips_reader_require(r,10,e)!=FIPS_OK) return FIPS_ERR_RANGE;
    fips_reader q=*r; fips_path_probe v={.len=fips_reader_remaining(r)+1};
    fips_reader_read_u32_le(&q,&v.probe_id,NULL);
    fips_reader_read_u8(&q,&v.flags,NULL);
    fips_reader_read_u32_le(&q,&v.path_id,NULL);
    fips_reader_read_u8(&q,&v.caps,NULL);
    if (!v.path_id) return malformed(e,"path_id is zero");
    if ((v.flags & FIPS_PATH_EXT) && extension(q,e)!=FIPS_OK) return FIPS_ERR_RANGE;
    q.pos=q.len; *r=q; *p=v; return FIPS_OK;
}
fips_status fips_path_ack_encode(const fips_path_ack *p,fips_writer *w,fips_proto_error *e)
{
    size_t n=p->len<27?27:p->len;
    if (room(w,n,e)!=FIPS_OK) return FIPS_ERR_RANGE;
    size_t end=w->pos+n;
    fips_writer_write_u8(w,FIPS_LINK_PathAck,NULL);
    fips_writer_write_u32_le(w,p->probe_id,NULL);
    fips_writer_write_u8(w,p->flags & FIPS_PATH_ACTIVE,NULL);
    fips_writer_write_u32_le(w,p->path_id,NULL);
    fips_writer_write_u32_le(w,p->echo_path_id,NULL);
    fips_writer_write_u8(w,p->caps & FIPS_PATH_AGGREGATE,NULL);
    fips_writer_write_u32_le(w,p->rx_time_us,NULL);
    fips_writer_write_u32_le(w,p->rx_frames,NULL);
    fips_writer_write_u32_le(w,p->rx_bytes,NULL);
    memset(w->buf+w->pos,0,end-w->pos); w->pos=end;
    return FIPS_OK;
}
fips_status fips_path_ack_decode(fips_reader *r,fips_path_ack *p,fips_proto_error *e)
{
    if (fips_reader_require(r,26,e)!=FIPS_OK) return FIPS_ERR_RANGE;
    fips_reader q=*r; fips_path_ack v={.len=fips_reader_remaining(r)+1};
    fips_reader_read_u32_le(&q,&v.probe_id,NULL);
    fips_reader_read_u8(&q,&v.flags,NULL);
    fips_reader_read_u32_le(&q,&v.path_id,NULL);
    fips_reader_read_u32_le(&q,&v.echo_path_id,NULL);
    fips_reader_read_u8(&q,&v.caps,NULL);
    fips_reader_read_u32_le(&q,&v.rx_time_us,NULL);
    fips_reader_read_u32_le(&q,&v.rx_frames,NULL);
    fips_reader_read_u32_le(&q,&v.rx_bytes,NULL);
    if (!v.path_id) return malformed(e,"path_id is zero");
    if ((v.flags & FIPS_PATH_EXT) && extension(q,e)!=FIPS_OK) return FIPS_ERR_RANGE;
    q.pos=q.len; *r=q; *p=v; return FIPS_OK;
}
fips_status fips_path_close_encode(const fips_path_close *p,fips_writer *w,fips_proto_error *e)
{
    if (room(w,7,e)!=FIPS_OK) return FIPS_ERR_RANGE;
    fips_writer_write_u8(w,FIPS_LINK_PathClose,NULL);
    fips_writer_write_u32_le(w,p->path_id,NULL);
    fips_writer_write_u8(w,(uint8_t)p->reason,NULL);
    fips_writer_write_u8(w,0,NULL); return FIPS_OK;
}
fips_status fips_path_close_decode(fips_reader *r,fips_path_close *p,fips_proto_error *e)
{
    if (fips_reader_require(r,6,e)!=FIPS_OK) return FIPS_ERR_RANGE;
    fips_reader q=*r; fips_path_close v; uint8_t reason;
    fips_reader_read_u32_le(&q,&v.path_id,NULL);
    fips_reader_read_u8(&q,&reason,NULL);
    v.reason=fips_path_close_reason_from_byte(reason);
    q.pos=q.len; *r=q; *p=v; return FIPS_OK;
}
