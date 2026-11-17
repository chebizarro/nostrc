#include "ui.h"
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
static void counter(json_t *view,const json_t *fw,const char *name,const char *base)
{
    char pk[128],bk[128],bytes[FT_TEXT],value[256];
    snprintf(pk,sizeof(pk),"%s_packets",base); snprintf(bk,sizeof(bk),"%s_bytes",base);
    ft_bytes(ft_uint(fw,bk),bytes); snprintf(value,sizeof(value),"%" PRIu64 " pkts (%s)",ft_uint(fw,pk),bytes);
    json_object_set_new(view,name,json_string(value));
}
void ft_ui_routing(ft_frame *f,ft_app *a,ft_rect r)
{
    json_t *d=a->data[FT_ROUTING],*view=json_deep_copy(d),*fw=ft_get(d,"forwarding");
    ft_rect parts[3]; ft_split3(r,7,8,parts);
    ft_rect in=ft_box(f,parts[0]," Routing State ",a->focus[FT_ROUTING]==0);
    json_object_set_new(view,"pending_count",json_integer((json_int_t)json_array_size(ft_get(d,"pending_lookups"))));
    counter(view,fw,"warm","warm_malformed");
    const char *labels[]={"Coord Cache","Identity Cache","Pending Lookups","Recent Requests","Warm Malformed"};
    const char *paths[]={"coord_cache_entries","identity_cache_entries","pending_count","recent_requests","warm"};
    ft_aligned_fields(f,in,view,labels,paths,5,&a->scroll[FT_ROUTING][0]);
    in=ft_box(f,parts[1]," Coordinate Cache ",a->focus[FT_ROUTING]==1);
    json_t *cache=a->data[FT_CACHE],*cv=json_object(); char value[256];
    snprintf(value,sizeof(value),"%" PRIu64 " / %" PRIu64,ft_uint(cache,"count"),ft_uint(cache,"max_entries")); json_object_set_new(cv,"entries",json_string(value));
    double ratio=ft_num(cache,"fill_ratio"); if(isnan(ratio)) snprintf(value,sizeof(value),"-"); else snprintf(value,sizeof(value),"%.1f%%",ratio*100);
    json_object_set_new(cv,"fill",json_string(value));
    char b[FT_TEXT]; ft_duration(ft_uint(cache,"default_ttl_ms"),b); json_object_set_new(cv,"ttl",json_string(b));
    ft_duration(ft_uint(cache,"avg_age_ms"),b); json_object_set_new(cv,"age",json_string(b)); json_object_set(cv,"expired",ft_get(cache,"expired"));
    const char *cl[]={"Entries","Fill Ratio","Default TTL","Expired","Avg Age"},*cp[]={"entries","fill","ttl","expired","age"};
    if(cache) ft_aligned_fields(f,in,cv,cl,cp,5,&a->scroll[FT_ROUTING][1]); else ft_put(f,in,0,0,FT_DIM,"  Waiting for data...");
    json_decref(cv);
    in=ft_box(f,parts[2]," Routing Statistics ",a->focus[FT_ROUTING]==2);
    counter(view,fw,"local_tx","originated"); counter(view,fw,"local_rx","delivered"); counter(view,fw,"mesh_rx","received"); counter(view,fw,"transit","forwarded");
    uint64_t tx=ft_uint(fw,"originated_packets")+ft_uint(fw,"forwarded_packets");
    ft_bytes(ft_uint(fw,"originated_bytes")+ft_uint(fw,"forwarded_bytes"),b);
    snprintf(value,sizeof(value),"%" PRIu64 " pkts (%s)",tx,b); json_object_set_new(view,"mesh_tx",json_string(value));
    const char *routes[]={"route_direct_peer","route_tree_down","route_tree_up","route_crosslink_descend","route_crosslink_ascend","route_tree_down_cross"};
    uint64_t total=ft_uint(fw,"forwarded_packets");
    for(size_t i=0;i<6;++i) { uint64_t n=ft_uint(fw,routes[i]); snprintf(value,sizeof(value),"%" PRIu64 " (%.1f%%)",n,total?(double)n/(double)total*100:0); json_object_set_new(view,routes[i],json_string(value)); }
    const char *drops[]={"drop_no_route","ttl_exhausted","decode_error","drop_mtu_exceeded","drop_send_error"};
    for(size_t i=0;i<5;++i) counter(view,fw,drops[i],drops[i]);
    const char *ll[]={"  Local Stack","Transmitted","Received","","  Mesh","Transmitted","Received","","  Lookup Requests","Received","Forwarded","Initiated","Deduplicated","Target Is Us","Duplicate","Own Loopback","Bloom Miss","Backoff Suppressed","Fwd Rate Limited","Sign Rate Limited","Dedup Evicted","TTL Exhausted","Decode Error","","  Lookup Responses","Received","Accepted","Forwarded","Timed Out","Identity Miss","Proof Failed","Unsolicited","Decode Error"};
    const char *lp[]={NULL,"local_tx","local_rx",NULL,NULL,"mesh_tx","mesh_rx",NULL,NULL,"lookup.req_received","lookup.req_forwarded","lookup.req_initiated","lookup.req_deduplicated","lookup.req_target_is_us","lookup.req_duplicate","lookup.req_own_loopback","lookup.req_bloom_miss","lookup.req_backoff_suppressed","lookup.req_forward_rate_limited","lookup.req_sign_rate_limited","lookup.req_dedup_evicted","lookup.req_ttl_exhausted","lookup.req_decode_error",NULL,NULL,"lookup.resp_received","lookup.resp_accepted","lookup.resp_forwarded","lookup.resp_timed_out","lookup.resp_identity_miss","lookup.resp_proof_failed","lookup.resp_unsolicited","lookup.resp_decode_error"};
    const char *rl[]={"  Forwarded","Forwarded","","  Route Class","Direct Peer","Tree-down","Tree-up","Cross-link descend","Cross-link ascend","Tree-down cross","","  Dropped","No Route","TTL Exhausted","Decode Error","MTU Exceeded","Send Error","","  Error Signals","Coords Required","Path Broken","MTU Exceeded","PMTU Notif < Floor","MTU Exceeded < Floor","Lookup PMTU < Floor","Coords Required Refused","Path Broken Refused","MTU Exceeded Refused","Forged Pairing","Emit Over Peer Budget","Emit Over Dest Interval","Emit Limiter At Capacity","MTU Exceeded Uncorroborated","","  Congestion","CE Forwarded","CE Received","Congestion Detected","Kernel Drops"};
    const char *rp[]={NULL,"transit",NULL,NULL,"route_direct_peer","route_tree_down","route_tree_up","route_crosslink_descend","route_crosslink_ascend","route_tree_down_cross",NULL,NULL,"drop_no_route","ttl_exhausted","decode_error","drop_mtu_exceeded","drop_send_error",NULL,NULL,"error_signals.coords_required","error_signals.path_broken","error_signals.mtu_exceeded","error_signals.path_mtu_notif_below_floor","error_signals.mtu_exceeded_below_floor","error_signals.lookup_resp_mtu_below_floor","error_signals.unbound_coords","error_signals.unbound_broken","error_signals.unbound_mtu","error_signals.unbound_forged","error_signals.emit_over_peer_budget","error_signals.emit_over_dest_interval","error_signals.emit_limiter_at_capacity","error_signals.mtu_exceeded_uncorroborated",NULL,NULL,"congestion.ce_forwarded","congestion.ce_received","congestion.congestion_detected","congestion.kernel_drop_events"};
    size_t *off=&a->scroll[FT_ROUTING][2]; *off=ft_clamp_scroll(*off,39,(size_t)in.h);
    ft_rect l=in; l.w=in.w/2; ft_rect rr={in.x+l.w,in.y,in.w-l.w,in.h};
    size_t lo=*off,ro=*off;
    ft_aligned_fields(f,l,view,ll,lp,33,&lo); ft_aligned_fields(f,rr,view,rl,rp,39,&ro);
    json_decref(view);
}
