#include "ui.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
static void peer_display(json_t *v,const json_t *peer)
{
    const char *npub=ft_str(peer,"npub"); char *short_npub=ft_truncate(npub,20,false);
    json_object_set_new(v,"short_npub",json_string(short_npub?short_npub:"-")); free(short_npub);
    const char *dir=ft_str(peer,"direction");
    if(!strcmp(dir,"-")) dir=json_is_true(ft_get(peer,"is_outbound"))?"out":"in";
    json_object_set_new(v,"dir",json_string(dir));
    const char *keys[]={"srtt_ms","smoothed_loss","lqi","effective_depth","goodput_bps"};
    const char *out[]={"srtt","loss","lqi_text","depth","goodput"};
    for(size_t i=0;i<5;++i) {
        char path[96],value[FT_TEXT]; snprintf(path,sizeof(path),"mmp.%s",keys[i]);
        double d=ft_num(peer,i==3?keys[i]:path);
        if(i==1 && isnan(d)) d=ft_num(peer,"mmp.loss_rate");
        if(isnan(d)) snprintf(value,sizeof(value),"%s",i==3?"—":"-");
        else if(i==4) ft_throughput(d,value);
        else snprintf(value,sizeof(value),"%.*f",i==0?1:i==1?3:2,d);
        json_object_set_new(v,out[i],json_string(value));
    }
}
void ft_ui_peers(ft_frame *f,ft_app *a,ft_rect r)
{
    size_t n; json_t **rows=ft_peer_order(ft_rows(a,FT_PEERS),&n);
    if(a->selected[FT_PEERS]>=0 && (size_t)a->selected[FT_PEERS]>=n) a->selected[FT_PEERS]=-1;
    ft_rect table=r;
    if(a->detail) { table.w=r.w*40/100; ft_rect detail={r.x+table.w,r.y,r.w-table.w,r.h};
        json_t *peer=ft_selected(a),*view=peer?json_deep_copy(peer):json_object();
        json_t *links=json_array(),*transports=json_array(); size_t i; json_t *l;
        json_array_foreach(ft_rows(a,FT_LINKS),i,l) {
            const char *npub=ft_str(peer,"npub");
            if(!strcmp(ft_str(l,"npub"),npub)||!strcmp(ft_str(l,"peer_npub"),npub)||!strcmp(ft_str(l,"peer_addr"),ft_str(peer,"node_addr"))) json_array_append(links,l);
        }
        json_t *ts=ft_rows(a,FT_TRANSPORTS); size_t j; json_t *t;
        json_array_foreach(ts,i,t) json_array_foreach(links,j,l) if(ft_uint(t,"transport_id")==ft_uint(l,"transport_id")) { json_array_append(transports,t); break; }
        json_object_set_new(view,"links",links); json_object_set_new(view,"transports",transports);
        char address[FIPSCTL_ADDR_CAPACITY];
        if(fipsctl_address_from_npub(ft_str(peer,"npub"),address,NULL)) json_object_set_new(view,"ipv6_addr",json_string(address));
        ft_detail(f,detail,view,&a->detail_scroll," Peer Detail "); json_decref(view);
    }
    char title[128]; snprintf(title,sizeof(title)," Peers (%zu) ",n); table=ft_box(f,table,title,false);
    ft_column cols[]={{"Name","display_name",20},{"Npub","short_npub",20},{"Transport","transport",20},
        {"Dir","dir",4},{"SRTT","srtt",8},{"Loss","loss",7},{"LQI","lqi_text",6},{"EffD","depth",6},
        {"Goodput","goodput",10},{"Pkts Tx","stats.packets_sent",9},{"Pkts Rx","stats.packets_recv",9}};
    ft_table_header(f,table,cols,11);
    size_t lines=0,selected_line=0; unsigned last=3;
    for(size_t i=0;i<n;++i) {
        unsigned g=ft_group(rows[i]); if(g!=last) { lines+=last==3?1:2; last=g; }
        if((int)i==a->selected[FT_PEERS]) selected_line=lines;
        ++lines; size_t paths=json_array_size(ft_get(rows[i],"paths")); if(paths>1) lines+=paths;
    }
    size_t visible=table.h>1?(size_t)table.h-1:0, *start=&a->table_scroll[FT_PEERS];
    if(a->selected[FT_PEERS]>=0 && visible) { if(selected_line<*start) *start=selected_line; if(selected_line>=*start+visible) *start=selected_line-visible+1; }
    *start=ft_clamp_scroll(*start,lines,visible); lines=0; last=3;
    const char *groups[]={"── Parent ──","── STP Children ──","── Other ──"};
    for(size_t i=0;i<n;++i) {
        unsigned g=ft_group(rows[i]);
        if(g!=last) {
            if(last!=3) ++lines;
            if(lines>=*start) ft_put(f,table,2,(int)(lines-*start)+1,FT_YELLOW,groups[g]);
            ++lines; last=g;
        }
        json_t *view=json_deep_copy(rows[i]); peer_display(view,rows[i]);
        if(lines>=*start) ft_table_row(f,table,(int)(lines-*start)+1,cols,11,view,(int)i==a->selected[FT_PEERS]);
        ++lines; json_decref(view);
        json_t *paths=ft_get(rows[i],"paths"); size_t j; json_t *path;
        if(json_array_size(paths)>1) json_array_foreach(paths,j,path) {
            json_t *pv=json_deep_copy(path); json_object_set_new(pv,"display_name",json_string(j+1==json_array_size(paths)?"  └─":"  ├─"));
            peer_display(pv,path);
            if(lines>=*start) ft_table_row(f,table,(int)(lines-*start)+1,cols,11,pv,false);
            json_decref(pv); ++lines;
        }
    }
    free(rows);
}
