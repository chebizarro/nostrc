#include "ui.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
void ft_ui_transports(ft_frame *f,ft_app *a,ft_rect r)
{
    ft_transport_row *rows=NULL; size_t n=ft_transport_rows(a,&rows);
    if(a->selected[FT_TRANSPORTS]>=0 && (size_t)a->selected[FT_TRANSPORTS]>=n) a->selected[FT_TRANSPORTS]=-1;
    ft_rect table=r;
    if(a->detail) { table.w=r.w*40/100; ft_rect detail={r.x+table.w,r.y,r.w-table.w,r.h};
        ft_detail(f,detail,ft_selected(a),&a->detail_scroll," Transport / Link Detail "); }
    char title[128]; size_t tn=json_array_size(ft_rows(a,FT_TRANSPORTS));
    snprintf(title,sizeof(title)," Transports (%zu) ",tn); table=ft_box(f,table,title,false);
    bool narrow=table.w<100;
    ft_column cols[]={{"Transport / Link","label",narrow?28:40},{"State","state",12},{"MTU","mtu",6},
        {"Frames Tx","stats.frames_sent",12},{"Frames Rx","stats.frames_recv",12},
        {"Bytes Tx","bytes_tx",12},{"Bytes Rx","bytes_rx",12}};
    size_t col_count=narrow?5:7; ft_table_header(f,table,cols,col_count);
    size_t visible=table.h>1?(size_t)table.h-1:0,*start=&a->table_scroll[FT_TRANSPORTS];
    int selected=a->selected[FT_TRANSPORTS];
    if(selected>=0 && visible) { if((size_t)selected<*start) *start=(size_t)selected; if((size_t)selected>=*start+visible) *start=(size_t)selected-visible+1; }
    *start=ft_clamp_scroll(*start,n,visible);
    for(size_t i=*start;i<n && i-*start<visible;++i) {
        json_t *t=rows[i].transport,*link=rows[i].link,*v=json_deep_copy(link?link:t); char label[512];
        if(link) snprintf(label,sizeof(label),"  └─ %s %s",ft_str(link,"display_name"),ft_str(link,"remote_addr"));
        else {
            size_t count=0,j; json_t *l; json_array_foreach(ft_rows(a,FT_LINKS),j,l) if(ft_uint(l,"transport_id")==ft_uint(t,"transport_id")) ++count;
            const char *name=ft_str(t,"name"),*addr=ft_str(t,"local_addr");
            json_t *iface=ft_get(t,"interface");
            snprintf(label,sizeof(label),"%s%s %s%s%s%s%s",count?(ft_expanded(a,t)?"▼ ":"▶ "):"  ",ft_str(t,"type"),
                strcmp(name,"-")?name:"",iface?" ":"",iface?ft_str(iface,"name"):"",iface?" ":"",iface?ft_str(iface,"presence"):strcmp(addr,"-")?addr:"");
            if(iface && strcmp(ft_str(iface,"presence"),"present")) json_object_set_new(v,"state",json_string(ft_str(iface,"presence")));
            else if(iface && json_is_false(ft_get(iface,"carrier"))) json_object_set_new(v,"state",json_string("no carrier"));
        }
        json_object_set_new(v,"label",json_string(label)); char b[FT_TEXT];
        ft_bytes(ft_uint(v,"stats.bytes_sent"),b); json_object_set_new(v,"bytes_tx",json_string(b));
        ft_bytes(ft_uint(v,"stats.bytes_recv"),b); json_object_set_new(v,"bytes_rx",json_string(b));
        ft_table_row(f,table,(int)(i-*start)+1,cols,col_count,v,(int)i==selected); json_decref(v);
    }
    free(rows);
}
