#include "ui.h"
#include <math.h>
#include <stdlib.h>
void ft_ui_sessions(ft_frame *f,ft_app *a,ft_rect r)
{
    json_t *rows=ft_rows(a,FT_SESSIONS); size_t n=json_array_size(rows);
    if(a->selected[FT_SESSIONS]>=0 && (size_t)a->selected[FT_SESSIONS]>=n) a->selected[FT_SESSIONS]=-1;
    ft_rect table=r;
    if(a->detail) { table.w=r.w*40/100; ft_rect detail={r.x+table.w,r.y,r.w-table.w,r.h};
        ft_detail(f,detail,ft_selected(a),&a->detail_scroll," Session Detail "); }
    char title[128]; snprintf(title,sizeof(title)," Sessions (%zu) ",n); table=ft_box(f,table,title,false);
    ft_column cols[]={{"Name","display_name",12},{"Remote Addr","short_addr",13},{"State","state",13},
        {"Role","role",6},{"SRTT","srtt",8},{"Loss","loss",7},{"SQI","sqi",6},{"Path MTU","mmp.path_mtu",9},{"Last Activity","activity",14}};
    ft_table_header(f,table,cols,9);
    size_t visible=table.h>1?(size_t)table.h-1:0,*start=&a->table_scroll[FT_SESSIONS];
    int selected=a->selected[FT_SESSIONS];
    if(selected>=0 && visible) { if((size_t)selected<*start) *start=(size_t)selected; if((size_t)selected>=*start+visible) *start=(size_t)selected-visible+1; }
    *start=ft_clamp_scroll(*start,n,visible);
    for(size_t i=*start;i<n && i-*start<visible;++i) {
        json_t *s=json_array_get(rows,i),*v=json_deep_copy(s);
        char *addr=ft_truncate(ft_str(s,"remote_addr"),10,false); json_object_set_new(v,"short_addr",json_string(addr?addr:"-")); free(addr);
        json_object_set_new(v,"role",json_string(json_is_true(ft_get(s,"is_initiator"))?"init":"resp"));
        const char *paths[]={"mmp.srtt_ms","mmp.smoothed_loss","mmp.sqi"},*names[]={"srtt","loss","sqi"};
        for(int j=0;j<3;++j) {
            double d=ft_num(s,paths[j]); if(j==1 && isnan(d)) d=ft_num(s,"mmp.loss_rate");
            char b[FT_TEXT]; if(isnan(d)) snprintf(b,sizeof(b),"-"); else snprintf(b,sizeof(b),"%.*f",j==0?1:j==1?3:2,d);
            json_object_set_new(v,names[j],json_string(b));
        }
        char b[FT_TEXT]; ft_elapsed(ft_uint(s,"last_activity_ms"),a->wall_ms,b); json_object_set_new(v,"activity",json_string(b));
        ft_table_row(f,table,(int)(i-*start)+1,cols,9,v,(int)i==selected); json_decref(v);
    }
}
