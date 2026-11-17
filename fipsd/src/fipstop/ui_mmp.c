#include "ui.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
static double metric(const json_t *p,bool link,unsigned col)
{
    const char *keys[]={"", "srtt_ms", "smoothed_loss", "smoothed_etx", link?"lqi":"sqi", link?"goodput_bps":"path_mtu"};
    json_t *layer=ft_get(p,link?"link_layer":"session_layer"); double n=ft_num(layer,keys[col]);
    if(isnan(n) && col==2) n=ft_num(layer,"loss_rate");
    if(isnan(n) && col==3) n=ft_num(layer,"etx");
    return isnan(n)?INFINITY:n;
}
static int compare(const json_t *a,const json_t *b,bool link,ft_sort sort)
{
    int cmp;
    if(!sort.col) cmp=strcmp(ft_str(a,"display_name"),ft_str(b,"display_name"));
    else { double x=metric(a,link,sort.col),y=metric(b,link,sort.col); cmp=x<y?-1:x>y?1:0; }
    return sort.descending?-cmp:cmp;
}
static const char *arrow(const json_t *d,const char *key)
{
    const char *s=ft_str(d,key); return !strcmp(s,"rising")?"↑":!strcmp(s,"falling")?"↓":" ";
}
static void pane(ft_frame *f,ft_app *a,ft_rect r,bool link)
{
    unsigned pane=link?0:1; ft_sort sort=a->mmp_sort[pane];
    json_t *source=ft_get(a->data[FT_MMP],link?"peers":"sessions"); size_t n=json_array_size(source);
    char title[128]; snprintf(title,sizeof(title),link?" Link MMP (%zu peers) ":" Session MMP (%zu sessions) ",n);
    r=ft_box(f,r,title,a->focus[FT_MMP]==pane);
    if(!n) { ft_put(f,r,0,0,FT_DIM,link?"  No peers":"  No sessions"); return; }
    json_t **rows=calloc(n,sizeof(*rows)); if(!rows) return;
    for(size_t i=0;i<n;++i) {
        json_t *v=json_array_get(source,i); size_t j=i;
        while(j && compare(rows[j-1],v,link,sort)>0) { rows[j]=rows[j-1]; --j; } rows[j]=v;
    }
    const char *labels[]={"name","srtt","loss","etx",link?"lqi":"sqi",link?"gp":"mtu"};
    char header[256]="  sort: ";
    for(unsigned i=0;i<6;++i) { size_t len=strlen(header); snprintf(header+len,sizeof(header)-len,"%s%s%s",i?" ":"",labels[i],i==sort.col?(sort.descending?"▼":"▲"):""); }
    size_t *off=&a->scroll[FT_MMP][pane]; *off=ft_clamp_scroll(*off,n+1,(size_t)r.h);
    if(!*off) ft_put(f,r,0,0,FT_CYAN,header);
    for(size_t i=0;i<n;++i) {
        size_t line=i+1; if(line<*off) continue;
        json_t *layer=ft_get(rows[i],link?"link_layer":"session_layer");
        char values[6][FT_TEXT];
        for(unsigned col=1;col<6;++col) {
            double v=metric(rows[i],link,col);
            if(!isfinite(v)) snprintf(values[col],FT_TEXT,"-");
            else if(col==5 && link) ft_throughput(v,values[col]);
            else if(col==1) snprintf(values[col],FT_TEXT,"%.1fms",v);
            else snprintf(values[col],FT_TEXT,"%.*f",col==2?4:col==5?0:2,v);
        }
        char *name=ft_truncate(ft_str(rows[i],"display_name"),16,true);
        if(link) ft_printf(f,r,0,(int)(line-*off),FT_NORMAL,"  %s srtt: %-10s%s loss: %-8s%s etx: %-6slqi: %-8sgp: %s%s",name?name:"",values[1],arrow(layer,"rtt_trend"),values[2],arrow(layer,"loss_trend"),values[3],values[4],values[5],arrow(layer,"goodput_trend"));
        else ft_printf(f,r,0,(int)(line-*off),FT_NORMAL,"  %s srtt: %-10s%s loss: %-8s%s etx: %-6s%s sqi: %-8smtu: %s",name?name:"",values[1],arrow(layer,"rtt_trend"),values[2],arrow(layer,"loss_trend"),values[3],arrow(layer,"etx_trend"),values[4],values[5]);
        free(name);
    }
    free(rows);
}
void ft_ui_mmp(ft_frame *f,ft_app *a,ft_rect r)
{
    int top=(r.h*60+50)/100; pane(f,a,(ft_rect){r.x,r.y,r.w,top},true);
    pane(f,a,(ft_rect){r.x,r.y+top,r.w,r.h-top},false);
}
