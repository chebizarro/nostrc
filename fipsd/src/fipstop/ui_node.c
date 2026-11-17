#include "ui.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
static void listeners(ft_frame *f,ft_app *a,ft_rect r)
{
    json_t *d=a->listening;
    bool inactive=json_is_false(ft_get(d,"firewall_active"));
    r=ft_box(f,r,inactive?" Listening on fips0  fips-firewall.service inactive — all listeners exposed ":" Listening on fips0 ",false);
    if(!d) { ft_put(f,r,0,0,FT_DIM,"  loading..."); return; }
    json_t *rows=ft_get(d,"sockets");
    if(!json_array_size(rows)) { ft_put(f,r,0,0,FT_DIM,"  no listeners reachable from fips0"); return; }
    ft_put(f,r,0,0,FT_YELLOW,"Proto Port  Process              State");
    size_t i; json_t *s; json_array_foreach(rows,i,s) {
        const char *filter=ft_str(s,"filter");
        const char *state=!strcmp(filter,"accept")?"OPEN":!strcmp(filter,"unknown")?"filt?":!strcmp(filter,"drop")?"filt":"OPEN";
        char process[128];
        const char *comm=ft_str(s,"process");
        if(!strcmp(comm,"-")) comm=ft_str(s,"comm");
        if(!strcmp(comm,"-")) comm="?";
        snprintf(process,sizeof(process),"%s%s",comm,!strcmp(ft_str(s,"local_addr"),"::")?"*":"");
        ft_printf(f,r,0,(int)i+1,!strcmp(state,"OPEN")?FT_NORMAL:FT_DIM,"%-5s %-5" PRIu64 " %-20s %s",ft_str(s,"proto"),ft_uint(s,"port"),process,state);
    }
}
void ft_ui_node(ft_frame *f,ft_app *a,ft_rect r)
{
    json_t *d=a->data[FT_NODE];
    int desired[4]={7,7,8,9}, heights[4], total=31, used=0;
    for(int i=0;i<4;++i) { heights[i]=r.h>=total?desired[i]:(desired[i]*r.h+total/2)/total; used+=heights[i]; }
    heights[3]+=r.h<total?r.h-used:0;
    ft_rect boxes[4]; int y=r.y;
    for(int i=0;i<4;++i) { boxes[i]=(ft_rect){r.x,y,r.w,heights[i]}; y+=heights[i]; }
    ft_rect in=ft_box(f,boxes[0]," Runtime ",false);
    char uptime[FT_TEXT],pid[FT_TEXT]; ft_uptime(ft_uint(d,"uptime_secs"),uptime); ft_value(d,"pid",pid,sizeof(pid));
    ft_printf(f,in,0,0,FT_NORMAL," ver: %s  pid: %s  uptime: %s",ft_str(d,"version"),pid,uptime);
    ft_printf(f,in,0,1,FT_NORMAL," exe: %s",ft_str(d,"exe_path"));
    ft_printf(f,in,0,2,FT_NORMAL," ctl: %s",ft_str(d,"control_socket"));
    ft_printf(f,in,0,3,FT_NORMAL," tun: %s",ft_str(d,"tun_name"));
    in=ft_box(f,boxes[1]," Identity ",false);
    ft_printf(f,in,0,0,FT_NORMAL," npub:      %s",ft_str(d,"npub"));
    ft_printf(f,in,0,1,FT_NORMAL," node_addr: %s",ft_str(d,"node_addr"));
    ft_printf(f,in,0,2,FT_NORMAL," ipv6:      %s",ft_str(d,"ipv6_addr"));
    ft_printf(f,in,0,3,FT_NORMAL," identity:  %s",json_is_true(ft_get(d,"persistent"))?"persistent":"ephemeral");
    in=ft_box(f,boxes[2]," State ",false);
    char mtu[FT_TEXT],leaf[FT_TEXT]; ft_value(d,"effective_ipv6_mtu",mtu,sizeof(mtu)); ft_value(d,"is_leaf_only",leaf,sizeof(leaf));
    ft_printf(f,in,0,0,FT_NORMAL," state: %s  tun: %s  mtu: %s  leaf: %s",ft_str(d,"state"),ft_str(d,"tun_state"),mtu,leaf);
    ft_printf(f,in,0,1,FT_CYAN," peers: %" PRIu64 "  sessions: %" PRIu64 "  links: %" PRIu64 "  transports: %" PRIu64 "  connections: %" PRIu64,
        ft_uint(d,"peer_count"),ft_uint(d,"session_count"),ft_uint(d,"link_count"),ft_uint(d,"transport_count"),ft_uint(d,"connection_count"));
    char mesh[FT_TEXT]; ft_value(d,"estimated_mesh_size",mesh,sizeof(mesh));
    ft_printf(f,in,0,2,FT_NORMAL," approx. mesh estimate: %s%s",strcmp(mesh,"-")?"~":"",mesh);
    char *root=fipsctl_take_chars(ft_str(d,"root"),16);
    ft_printf(f,in,0,3,FT_NORMAL," root: %s%s",json_is_true(ft_get(d,"is_root"))?"I am the one who roots":root,
        json_is_true(ft_get(d,"is_root"))?"":"…"); free(root);
    json_t *counts=ft_get(d,"transport_peer_counts"); char transports[512]=""; const char *key; json_t *v;
    /* Lexical JSON rendering is shared with fipsctl, including u64 sentinels. */
    char *sorted=fipsctl_json_compact(counts); json_error_t e; json_t *map=sorted?json_loads(sorted,0,&e):NULL; free(sorted);
    json_object_foreach(map,key,v) {
        size_t len=strlen(transports); snprintf(transports+len,sizeof(transports)-len,"%s%s (%" JSON_INTEGER_FORMAT ")",len?", ":"",key,json_integer_value(v));
    }
    json_decref(map);
    ft_printf(f,in,0,4,FT_NORMAL," transports: %s",transports[0]?transports:"-");
    char *ps=ft_spark(ft_get(d,"sparklines.peer_count")),*ms=ft_spark(ft_get(d,"sparklines.mesh_size"));
    ft_printf(f,in,0,5,FT_DIM," peers:  %s   mesh: %s",ps?ps:"",ms?ms:""); free(ps); free(ms);
    ft_rect traffic=boxes[3]; traffic.w=r.w/2;
    in=ft_box(f,traffic," Traffic ",false);
    ft_put(f,in,0,0,FT_CYAN,"  TUN (IPv6)");
    const char *labels[]={"Tx","Rx","Packets"};
    const char *packets[]={"delivered_packets","originated_packets","forwarded_packets"};
    const char *bytes[]={"delivered_bytes","originated_bytes","forwarded_bytes"};
    json_t *fw=ft_get(d,"forwarding");
    for(int i=0;i<3;++i) {
        char b[FT_TEXT]; ft_bytes(ft_uint(fw,bytes[i]),b);
        ft_printf(f,in,0,i==2?5:i+1,FT_NORMAL,"    %s: %" PRIu64 " pkts (%s)",labels[i],ft_uint(fw,packets[i]),b);
    }
    ft_put(f,in,0,4,FT_CYAN,"  Forwarded (transit)");
    char *bi=ft_spark(ft_get(d,"sparklines.bytes_in")),*bo=ft_spark(ft_get(d,"sparklines.bytes_out"));
    ft_printf(f,in,0,6,FT_DIM,"    rate in:  %s   rate out: %s",bi?bi:"",bo?bo:""); free(bi); free(bo);
    ft_rect listen={boxes[3].x+traffic.w,boxes[3].y,r.w-traffic.w,boxes[3].h}; listeners(f,a,listen);
}
