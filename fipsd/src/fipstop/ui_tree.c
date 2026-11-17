#include "ui.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>
void ft_ui_tree(ft_frame *f,ft_app *a,ft_rect r)
{
    json_t *d=a->data[FT_TREE]; ft_rect parts[3]; ft_split3(r,10,22,parts);
    ft_rect in=ft_box(f,parts[0]," Tree Position ",a->focus[FT_TREE]==0);
    json_t *pos=json_deep_copy(d); bool root=json_is_true(ft_get(d,"is_root"));
    char *root_text=fipsctl_aprintf("%s%s",ft_str(d,"root"),root?" (self)":"");
    json_object_set_new(pos,"root_text",json_string(root_text?root_text:"-")); free(root_text);
    json_object_set_new(pos,"parent_text",json_string(root?"self (root)":ft_str(d,"parent_display_name")));
    if(!strcmp(ft_str(d,"root_npub"),"-")) json_object_set_new(pos,"root_npub",json_string("<unknown>"));
    char signed_text[FT_TEXT],seq[FT_TEXT],decl[256]; ft_value(d,"declaration_signed",signed_text,sizeof(signed_text)); ft_value(d,"declaration_sequence",seq,sizeof(seq));
    snprintf(decl,sizeof(decl),"seq %s, %s",seq,signed_text); json_object_set_new(pos,"declaration",json_string(decl));
    char path[2048]=""; json_t *coords=ft_get(d,"my_coords"); size_t cn=json_array_size(coords);
    if(!cn) snprintf(path,sizeof(path),"[root]");
    for(size_t i=cn;i>0;--i) { const char *s=json_string_value(json_array_get(coords,i-1)); char *short_s=ft_truncate_hex(s?s:"-",8);
        size_t n=strlen(path); snprintf(path+n,sizeof(path)-n,"%s%s",n?" > ":"",short_s?short_s:"-"); free(short_s); }
    if(cn) { size_t n=strlen(path); snprintf(path+n,sizeof(path)-n," > [self]"); }
    json_object_set_new(pos,"path_text",json_string(path));
    const char *labels[]={"Root","Npub","Depth","Parent","Declaration","","Path"};
    const char *paths[]={"root_text","root_npub","depth","parent_text","declaration",NULL,"path_text"};
    ft_fields(f,in,pos,labels,paths,7,&a->scroll[FT_TREE][0]); json_decref(pos);
    in=ft_box(f,parts[1]," Tree Announce Stats ",a->focus[FT_TREE]==1);
    const char *sl[]={"  Inbound","Received","Accepted","Decode Error","Unknown Peer","Addr Mismatch","Sig Failed","Stale","Loop Detected","Ancestry Changed","","  Outbound","Sent","Rate Limited","Send Failed","","  Cumulative","Parent Switches","Parent Losses","Flap Dampened"};
    const char *sp[]={NULL,"stats.received","stats.accepted","stats.decode_error","stats.unknown_peer","stats.addr_mismatch","stats.sig_failed","stats.stale","stats.loop_detected","stats.ancestry_changed",NULL,NULL,"stats.sent","stats.rate_limited","stats.send_failed",NULL,NULL,"stats.parent_switches","stats.parent_losses","stats.flap_dampened"};
    ft_fields(f,in,d,sl,sp,20,&a->scroll[FT_TREE][1]);
    json_t *peers=ft_role_peers(a,ft_get(d,"peers"),"node_addr"); size_t n; json_t **rows=ft_peer_order(peers,&n);
    char title[128]; snprintf(title,sizeof(title)," Tree Peers (%zu) ",n); in=ft_box(f,parts[2],title,a->focus[FT_TREE]==2);
    if(!n) ft_put(f,in,0,0,FT_DIM,"  No peers");
    else {
        size_t total=n; unsigned last=3; for(size_t i=0;i<n;++i) if(ft_group(rows[i])!=last) { total+=last==3?1:2; last=ft_group(rows[i]); }
        size_t *scroll=&a->scroll[FT_TREE][2]; *scroll=ft_clamp_scroll(*scroll,total,(size_t)in.h); size_t line=0; last=3;
        const char *groups[]={"  ── Parent ──","  ── STP Children ──","  ── Other ──"};
        for(size_t i=0;i<n;++i) {
            unsigned group=ft_group(rows[i]); if(group!=last) { if(last!=3) ++line; if(line>=*scroll) ft_put(f,in,0,(int)(line-*scroll),FT_YELLOW,groups[group]); ++line; last=group; }
            char depth[FT_TEXT],tree_depth[FT_TEXT],distance[FT_TEXT];
            double effective=ft_num(rows[i],"effective_depth");
            if(isnan(effective)) snprintf(depth,sizeof(depth),"—"); else snprintf(depth,sizeof(depth),"%.2f",effective);
            ft_value(rows[i],"depth",tree_depth,sizeof(tree_depth)); ft_value(rows[i],"distance_to_us",distance,sizeof(distance));
            char *name=ft_truncate(ft_str(rows[i],"display_name"),16,true);
            if(line>=*scroll) {
                if(!ft_get(rows[i],"depth")) ft_printf(f,in,0,(int)(line-*scroll),FT_DIM,"    %s (no position)",name?name:"");
                else ft_printf(f,in,0,(int)(line-*scroll),FT_NORMAL,"    %s depth: %-4s dist: %-4s eff: %-7s%s",name?name:"",tree_depth,
                    distance,depth,!strcmp(ft_str(rows[i],"root"),ft_str(d,"root"))?"same root":"diff root");
            }
            free(name);
            ++line;
        }
    }
    free(rows); json_decref(peers);
}
