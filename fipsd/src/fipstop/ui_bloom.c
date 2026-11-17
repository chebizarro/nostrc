#include "ui.h"
#include <math.h>
#include <stdlib.h>
void ft_ui_bloom(ft_frame *f,ft_app *a,ft_rect r)
{
    json_t *d=a->data[FT_BLOOM],*state=json_deep_copy(d); ft_rect parts[3]; ft_split3(r,8,15,parts);
    ft_rect in=ft_box(f,parts[0]," Bloom Filter State ",a->focus[FT_BLOOM]==0);
    bool root=json_is_true(ft_get(a->data[FT_NODE],"is_root")); char b[FT_TEXT];
    double fill=ft_num(d,"uptree_fill_ratio"),est=ft_num(d,"uptree_estimated_count");
    if(root) snprintf(b,sizeof(b),"n/a (root)"); else if(isnan(fill)) snprintf(b,sizeof(b),"—"); else snprintf(b,sizeof(b),"%.1f%%",fill*100);
    json_object_set_new(state,"fill",json_string(b));
    if(root) snprintf(b,sizeof(b),"n/a (root)"); else if(isnan(est)) snprintf(b,sizeof(b),"—"); else snprintf(b,sizeof(b),"%.0f",est);
    json_object_set_new(state,"estimate",json_string(b));
    char *addr=ft_truncate_hex(ft_str(d,"own_node_addr"),16); json_object_set_new(state,"short_addr",json_string(addr?addr:"-")); free(addr);
    const char *labels[]={"Node Addr","Leaf Only","Sequence","Leaf Deps","Fill (sent uptree)","Subtree est"};
    const char *paths[]={"short_addr","is_leaf_only","sequence","leaf_dependent_count","fill","estimate"};
    ft_aligned_fields(f,in,state,labels,paths,6,&a->scroll[FT_BLOOM][0]); json_decref(state);
    in=ft_box(f,parts[1]," Bloom Announce Stats ",a->focus[FT_BLOOM]==1);
    const char *sl[]={"  Inbound","Received","Accepted","Decode Error","Invalid","Non-V1","Unknown Peer","Stale","","  Outbound","Sent","Debounce Suppressed","Send Failed"};
    const char *sp[]={NULL,"stats.received","stats.accepted","stats.decode_error","stats.invalid","stats.non_v1","stats.unknown_peer","stats.stale",NULL,NULL,"stats.sent","stats.debounce_suppressed","stats.send_failed"};
    ft_fields(f,in,d,sl,sp,13,&a->scroll[FT_BLOOM][1]);
    json_t *peers=ft_role_peers(a,ft_get(d,"peer_filters"),"peer"); size_t n; json_t **rows=ft_peer_order(peers,&n);
    char title[128]; snprintf(title,sizeof(title)," Peer Filters (%zu) ",n); in=ft_box(f,parts[2],title,a->focus[FT_BLOOM]==2);
    if(!n) ft_put(f,in,0,0,FT_DIM,"  No peers");
    else {
        size_t total=n; unsigned last=3; for(size_t i=0;i<n;++i) if(ft_group(rows[i])!=last) { total+=last==3?1:2; last=ft_group(rows[i]); }
        size_t *scroll=&a->scroll[FT_BLOOM][2]; *scroll=ft_clamp_scroll(*scroll,total,(size_t)in.h); size_t line=0; last=3;
        const char *groups[]={"  ── Parent ──","  ── STP Children ──","  ── Other ──"};
        for(size_t i=0;i<n;++i) {
            unsigned group=ft_group(rows[i]); if(group!=last) { if(last!=3) ++line; if(line>=*scroll) ft_put(f,in,0,(int)(line-*scroll),FT_YELLOW,groups[group]); ++line; last=group; }
            double ratio=ft_num(rows[i],"fill_ratio");
            if(line>=*scroll) ft_printf(f,in,0,(int)(line-*scroll),FT_NORMAL,"  %-20s  filter: %s  fill: %.1f%%  seq: %llu",ft_str(rows[i],"display_name"),json_is_true(ft_get(rows[i],"has_filter"))?"yes":"no",isnan(ratio)?0:ratio*100,(unsigned long long)ft_uint(rows[i],"sequence"));
            ++line;
        }
    }
    free(rows); json_decref(peers);
}
