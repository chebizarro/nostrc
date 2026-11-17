#include "ui.h"
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
void ft_ui_gateway(ft_frame *f,ft_app *a,ft_rect r)
{
    if(!a->gateway_running) { r=ft_box(f,r," Gateway ",false); ft_put(f,r,0,0,FT_DIM,"  Gateway not running"); return; }
    json_t *d=a->data[FT_GATEWAY];
    int summary=r.h<12?r.h:12, top=summary<7?summary:7;
    ft_rect in=ft_box(f,(ft_rect){r.x,r.y,r.w,top}," Gateway ",false);
    ft_printf(f,in,0,0,FT_NORMAL," pool: %s  LAN: %s  DNS: %s",ft_str(d,"pool_cidr"),ft_str(d,"lan_interface"),ft_str(d,"dns_listen"));
    uint64_t total=ft_uint(d,"pool_total"),allocated=ft_uint(d,"pool_allocated");
    ft_printf(f,in,0,1,FT_CYAN," total: %" PRIu64 "  allocated: %" PRIu64 "  active: %" PRIu64 "  draining: %" PRIu64 "  free: %" PRIu64,total,allocated,ft_uint(d,"pool_active"),ft_uint(d,"pool_draining"),ft_uint(d,"pool_free"));
    char uptime[FT_TEXT]; ft_uptime(ft_uint(d,"uptime_secs"),uptime);
    ft_printf(f,in,0,2,FT_NORMAL," NAT mappings: %" PRIu64 "  uptime: %s",ft_uint(d,"nat_mappings"),uptime);
    if(summary>=10) {
        in=ft_box(f,(ft_rect){r.x,r.y+7,r.w,3}," Pool Utilization ",false);
        double ratio=total?fmin(1,(double)allocated/(double)total):0;
        for(int i=0;i<in.w;++i) ft_put(f,in,i,0,i<(int)(ratio*in.w)?FT_GREEN:FT_DIM,"█");
        char label[160]; snprintf(label,sizeof(label),"%" PRIu64 "/%" PRIu64 " allocated (%.1f%%)",allocated,total,ratio*100);
        int x=(in.w-(int)strlen(label))/2; ft_put(f,in,x>0?x:0,0,FT_NORMAL,label);
    }
    r.y+=summary; r.h-=summary;
    ft_rect table=r;
    if(a->detail) { table.w=r.w*40/100; ft_detail(f,(ft_rect){r.x+table.w,r.y,r.w-table.w,r.h},ft_selected(a),&a->detail_scroll," Mapping Detail "); }
    json_t *rows=ft_rows(a,FT_GATEWAY); size_t n=json_array_size(rows);
    char title[128]; snprintf(title,sizeof(title)," Mappings (%zu) ",n); table=ft_box(f,table,title,false);
    ft_column cols[]={{"Virtual IP","virtual_ip",16},{"DNS Name","dns_name",24},{"Mesh Addr","mesh_addr",24},{"State","state",10},{"Sessions","sessions",8},{"Age","age",10},{"Last Ref","last",14}};
    ft_table_header(f,table,cols,7); int selected=a->selected[FT_GATEWAY];
    if(selected>=0 && (size_t)selected>=n) a->selected[FT_GATEWAY]=selected=-1;
    size_t visible=table.h>1?(size_t)table.h-1:0,*start=&a->table_scroll[FT_GATEWAY];
    if(selected>=0 && visible) { if((size_t)selected<*start) *start=(size_t)selected; if((size_t)selected>=*start+visible) *start=(size_t)selected-visible+1; }
    *start=ft_clamp_scroll(*start,n,visible);
    for(size_t i=*start;i<n && i-*start<visible;++i) {
        json_t *v=json_deep_copy(json_array_get(rows,i)); char b[FT_TEXT]; ft_uptime(ft_uint(v,"age_secs"),b); json_object_set_new(v,"age",json_string(b));
        snprintf(b,sizeof(b),"%" PRIu64 "s ago",ft_uint(v,"last_ref_secs")); json_object_set_new(v,"last",json_string(b));
        ft_table_row(f,table,(int)(i-*start)+1,cols,7,v,(int)i==selected); json_decref(v);
    }
}
