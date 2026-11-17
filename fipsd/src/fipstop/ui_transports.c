#include "ui.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
void ft_ui_transports(ft_frame *f,ft_app *a,ft_rect r)
{
    ft_transport_row *rows=NULL; size_t n=ft_transport_rows(a,&rows);
    if(a->selected[FT_TRANSPORTS]>=0 && (size_t)a->selected[FT_TRANSPORTS]>=n) a->selected[FT_TRANSPORTS]=-1;
    ft_rect table=r;
    if(a->detail) {
        ft_rect detail;
        if(r.w<110) { table.h=(r.h+1)/2; detail=(ft_rect){r.x,r.y+table.h,r.w,r.h-table.h}; }
        else { table.w=r.w*40/100; detail=(ft_rect){r.x+table.w,r.y,r.w-table.w,r.h}; }
        ft_detail(f,detail,ft_selected(a),&a->detail_scroll," Transport / Link Detail ");
    }
    char title[128]; size_t tn=json_array_size(ft_rows(a,FT_TRANSPORTS));
    snprintf(title,sizeof(title)," Transports (%zu) ",tn); ft_rect outer=table; table=ft_box(f,table,title,false);
    bool narrow=table.w<100;
    ft_column cols[]={{"Transport / Link","label",narrow?12:18},{"Instance","instance",narrow?18:20},
        {"Bound to","bound",narrow?17:18},{"State","state_text",narrow?9:10},{"Peer","peer",narrow?6:10},
        {"Tx","tx",7},{"Rx","rx",7}};
    size_t col_count=narrow?5:7;
    int fixed=0; for(size_t i=0;i<col_count;++i) if(i!=4) fixed+=cols[i].width;
    int slack=table.w-fixed-(int)col_count+1;
    if(slack>cols[4].width) cols[4].width=slack;
    ft_table_header(f,table,cols,col_count);
    char header_state[FT_TEXT]; snprintf(header_state,sizeof(header_state),"%*s",cols[3].width,"State");
    int state_x=cols[0].width+cols[1].width+cols[2].width+3; ft_put(f,table,state_x,0,FT_YELLOW,header_state);
    size_t visible=table.h>1?(size_t)table.h-1:0,*start=&a->table_scroll[FT_TRANSPORTS];
    int selected=a->selected[FT_TRANSPORTS];
    if(selected>=0 && visible) { if((size_t)selected<*start) *start=(size_t)selected; if((size_t)selected>=*start+visible) *start=(size_t)selected-visible+1; }
    *start=ft_clamp_scroll(*start,n,visible);
    for(size_t i=*start;i<n && i-*start<visible;++i) {
        json_t *t=rows[i].transport,*link=rows[i].link,*v=json_deep_copy(link?link:t); char label[512];
        if(link) {
            const char *dir=ft_str(link,"direction");
            bool last=i+1==n || rows[i+1].transport!=t || !rows[i+1].link;
            snprintf(label,sizeof(label),"  %s %s",last?"└─":"├─",!strcmp(dir,"Outbound")?"Out":!strcmp(dir,"Inbound")?"In":dir);
            json_object_set_new(v,"instance",json_string("")); json_object_set_new(v,"bound",json_string(ft_str(link,"remote_addr")));
            const char *peer_name=""; size_t pi; json_t *peer;
            json_array_foreach(ft_rows(a,FT_PEERS),pi,peer) {
                if(!strcmp(ft_str(link,"peer_node_addr"),ft_str(peer,"node_addr")) || !strcmp(ft_str(link,"peer_addr"),ft_str(peer,"node_addr"))) { peer_name=ft_str(peer,"display_name"); break; }
            }
            json_object_set_new(v,"peer",json_string(peer_name));
        }
        else {
            size_t count=0,j; json_t *l; json_array_foreach(ft_rows(a,FT_LINKS),j,l) if(ft_uint(l,"transport_id")==ft_uint(t,"transport_id")) ++count;
            const char *name=ft_str(t,"name"),*addr=ft_str(t,"local_addr");
            json_t *iface=ft_get(t,"interface");
            const char *type=ft_str(t,"type");
            if(!strcmp(type,"tor")) snprintf(label,sizeof(label),"%stor(%s)",count?(ft_expanded(a,t)?"▼ ":"▶ "):"  ",strcmp(ft_str(t,"tor_mode"),"-")?ft_str(t,"tor_mode"):"socks5");
            else snprintf(label,sizeof(label),"%s%s",count?(ft_expanded(a,t)?"▼ ":"▶ "):"  ",type);
            char instance[256],bound[256];
            snprintf(instance,sizeof(instance),"%s%s",strcmp(name,"-")?name:"",iface && !strcmp(ft_str(iface,"policy"),"optional")?" (optional)":"");
            if(iface) snprintf(bound,sizeof(bound),"%s",ft_str(iface,"name"));
            else if(strcmp(addr,"-")) snprintf(bound,sizeof(bound),"%s",addr);
            else if(!strcmp(type,"tor") && strcmp(ft_str(t,"onion_address"),"-")) snprintf(bound,sizeof(bound),"%.16s..",ft_str(t,"onion_address"));
            else snprintf(bound,sizeof(bound),"#%llu",(unsigned long long)ft_uint(t,"transport_id"));
            json_object_set_new(v,"instance",json_string(instance)); json_object_set_new(v,"bound",json_string(bound));
            char peers[FT_TEXT]; snprintf(peers,sizeof(peers),"%s",""); json_object_set_new(v,"peer",json_string(peers));
            if(iface && strcmp(ft_str(iface,"presence"),"present")) json_object_set_new(v,"state",json_string(ft_str(iface,"presence")));

        }
        json_object_set_new(v,"label",json_string(label));
        char state[FT_TEXT],aligned[FT_TEXT]; ft_value(v,"state",state,sizeof(state));
        snprintf(aligned,sizeof(aligned),"%*s",cols[3].width,state); json_object_set_new(v,"state_text",json_string(aligned));
        json_object_set(v,"tx",ft_get(v,"stats.packets_sent")?ft_get(v,"stats.packets_sent"):ft_get(v,"stats.frames_sent"));
        json_object_set(v,"rx",ft_get(v,"stats.packets_recv")?ft_get(v,"stats.packets_recv"):ft_get(v,"stats.frames_recv"));
        ft_table_row(f,table,(int)(i-*start)+1,cols,col_count,v,(int)i==selected); json_decref(v);
    }
    ft_scrollbar(f,outer,n,selected<0?0:(size_t)selected);
    free(rows);
}
