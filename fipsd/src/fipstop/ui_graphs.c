#include "ui.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
static const wchar_t braille[5][5] = {
    {L' ',L'⢀',L'⢠',L'⢰',L'⢸'}, {L'⡀',L'⣀',L'⣠',L'⣰',L'⣸'},
    {L'⡄',L'⣄',L'⣤',L'⣴',L'⣼'}, {L'⡆',L'⣆',L'⣦',L'⣶',L'⣾'}, {L'⡇',L'⣇',L'⣧',L'⣷',L'⣿'} };
static void value(double n,char out[FT_TEXT])
{
    if(isnan(n)) snprintf(out,FT_TEXT,"-");
    else if(fabs(n)<10) snprintf(out,FT_TEXT,"%.2f",n);
    else if(fabs(n)<1000) snprintf(out,FT_TEXT,"%.1f",n);
    else if(fabs(n)<1e6) snprintf(out,FT_TEXT,"%.1fK",n/1000);
    else snprintf(out,FT_TEXT,"%.1fM",n/1e6);
}
static const char *unit(const char *metric)
{
    const char *units[]={"nodes","hops","peers","events/s","bytes/s","bytes/s","packets/s","packets/s","fraction","sessions"};
    for(size_t i=0;i<10;++i) if(!strcmp(metric,ft_metrics[i])) return units[i];
    return !strcmp(metric,"srtt_ms")?"ms":!strcmp(metric,"ecn_ce")?"events/s":"";
}
static unsigned level(double v,int low,int high)
{
    if(round(v)>=high) return 4;
    if(round(v)<=low) return 0;
    double scaled=round((v-low)*4.0/(high>low?high-low:1)+0.1);
    return scaled<0?0:scaled>4?4:(unsigned)scaled;
}
static void plot(ft_frame *f,ft_rect r,int y,int height,const double *v,size_t n,ft_summary sum)
{
    int cols=r.w-2; if(cols<=0 || height<=0) return;
    if(!n) { for(int row=0;row<height;++row) ft_put(f,r,0,y+row,FT_DIM,"  (no samples)"); return; }
    size_t count=(size_t)cols*2; double *samples=calloc(count,sizeof(*samples)); if(!samples) return;
    ft_resample(v,n,samples,count); double range=sum.max-sum.min; bool flat=!isfinite(range)||range<=0;
    for(size_t i=0;i<count;++i) {
        if(isnan(samples[i]) || (flat && sum.max==0)) samples[i]=NAN;
        else if(flat) samples[i]=8;
        else samples[i]=fmax(0,fmin(100,(samples[i]-sum.min)/range*100));
    }
    for(int row=0;row<height;++row) {
        int high=(int)round((double)(height-row)*100/height),low=(int)round((double)(height-row-1)*100/height);
        ft_style style=row*3<height?FT_RED:row*3<height*2?FT_YELLOW:FT_GREEN;
        for(int col=0;col<cols;++col) {
            double l=samples[(size_t)col*2],rr=samples[(size_t)col*2+1];
            wchar_t ch=isnan(l)||isnan(rr)?L' ':braille[level(l,low,high)][level(rr,low,high)];
            char text[16]; mbstate_t state; memset(&state,0,sizeof(state)); size_t bytes=wcrtomb(text,ch,&state);
            if(bytes!=(size_t)-1) { text[bytes]=0; ft_put(f,r,col+2,y+row,style,text); }
        }
    }
    free(samples);
}
static void block(ft_frame *f,ft_rect r,int y,const char *metric,const char *name,const json_t *values,int plot_height)
{
    double *v=NULL; size_t n=ft_series(values,&v); ft_summary s=ft_summarize(v,n);
    char min[FT_TEXT],max[FT_TEXT],last[FT_TEXT]; value(s.min,min); value(s.max,max); value(s.last,last);
    ft_printf(f,r,0,y,FT_NORMAL,"  %s  [%s]    min %s   max %s   last %s   samples %zu",name?name:metric,unit(metric),min,max,last,s.n);
    plot(f,r,y+1,plot_height,v,n,s); free(v);
}
static json_t *find_series(const json_t *d,const char *metric)
{
    size_t i; json_t *s; json_array_foreach(ft_get(d,"series"),i,s) if(!strcmp(ft_str(s,"metric"),metric)) return ft_get(s,"values");
    return NULL;
}
typedef struct peer_summary { size_t idx; json_t *peer; ft_summary summary; } peer_summary;
static int peer_compare(const peer_summary *a,const peer_summary *b,ft_sort sort)
{
    int cmp;
    if(!sort.col) cmp=strcmp(ft_str(a->peer,"display_name"),ft_str(b->peer,"display_name"));
    else {
        double x=sort.col==1?a->summary.min:sort.col==2?a->summary.max:sort.col==3?a->summary.last:(double)a->summary.n;
        double y=sort.col==1?b->summary.min:sort.col==2?b->summary.max:sort.col==3?b->summary.last:(double)b->summary.n;
        cmp=isnan(x)?(isnan(y)?0:1):isnan(y)?-1:x<y?-1:x>y?1:0;
    }
    return sort.descending?-cmp:cmp;
}
void ft_ui_graphs(ft_frame *f,ft_app *a,ft_rect r)
{
    const char *metric=ft_peer_metrics[a->graph_metric]; char mode[256],title[320];
    json_t *peer=json_array_get(ft_get(a->stats_peers,"peers"),a->graph_peer);
    const char *name=peer?ft_str(peer,"display_name"):"(no peers)";
    if(!a->graph_mode) { snprintf(mode,sizeof(mode),"node"); snprintf(title,sizeof(title)," Graphs — Node "); }
    else if(a->graph_mode==1) { snprintf(mode,sizeof(mode),"metric-by-peer [%s]",metric); snprintf(title,sizeof(title)," Graphs — %s by peer ",metric); }
    else { snprintf(mode,sizeof(mode),"peer-by-metric [%s]",name); snprintf(title,sizeof(title)," Graphs — peer %s ",name); }
    ft_printf(f,r,0,0,FT_NORMAL,"  mode: %s   window: %s   granularity: %s   scroll: %zu",mode,ft_windows[a->graph_window],ft_granularities[a->graph_window],a->graph_scroll);
    ft_put(f,r,0,1,FT_DIM,a->graph_mode==1?(a->detail?"  [↑/↓] peer   [n/N] stat   [m] mode   [Esc] back":"  [↑/↓] select   [Enter] expand   [n/N] stat   [m] mode"):"  [↑/↓] scroll   [←/→] window   [m] mode   [n/N] cycle");
    ft_rect outer={r.x,r.y+2,r.w,r.h>2?r.h-2:0};
    r=ft_box(f,outer,title,false);
    ft_put(f,outer,0,0,FT_DIM,"╭"); ft_put(f,outer,outer.w-1,0,FT_DIM,"╮");
    ft_put(f,outer,0,outer.h-1,FT_DIM,"╰"); ft_put(f,outer,outer.w-1,outer.h-1,FT_DIM,"╯");
    json_t *d=a->data[FT_GRAPHS];
    if(a->graph_mode!=1) {
        if(!json_array_size(ft_get(d,"series"))) { ft_put(f,r,0,0,FT_DIM,"  Waiting for data..."); return; }
        size_t metrics=a->graph_mode==0?10:7,total=metrics*6;
        a->graph_scroll=ft_clamp_scroll(a->graph_scroll,total,(size_t)r.h);
        for(size_t i=0;i<metrics;++i) {
            const char *m=a->graph_mode==0?ft_metrics[i]:ft_peer_metrics[i];
            block(f,r,(int)(i*6)-(int)a->graph_scroll,m,NULL,find_series(d,m),4);
        }
        return;
    }
    json_t *peers=ft_get(d,"peers"); size_t n=json_array_size(peers);
    if(!n) { ft_put(f,r,0,0,FT_DIM,"  No peers tracked yet in stats history."); return; }
    if(a->graph_peer>=n) a->graph_peer=0;
    if(a->detail) {
        peer=json_array_get(peers,a->graph_peer);
        block(f,r,0,metric,ft_str(peer,"display_name"),ft_get(peer,"values"),r.h>3?r.h-3:1); return;
    }
    peer_summary *order=calloc(n,sizeof(*order)); if(!order) return;
    for(size_t i=0;i<n;++i) {
        json_t *p=json_array_get(peers,i); double *v=NULL; size_t vn=ft_series(ft_get(p,"values"),&v);
        peer_summary item={i,p,ft_summarize(v,vn)}; free(v); size_t j=i;
        while(j && peer_compare(&order[j-1],&item,a->graph_sort)>0) { order[j]=order[j-1]; --j; } order[j]=item;
    }
    size_t selected=0;
    for(size_t i=0;i<n;++i) if(order[i].idx==a->graph_peer) selected=i+1;
    size_t *off=&a->scroll[FT_GRAPHS][0];
    if(selected<*off) *off=selected;
    if(r.h>0 && selected>=*off+(size_t)r.h) *off=selected-(size_t)r.h+1;
    *off=ft_clamp_scroll(*off,n+1,(size_t)r.h);
    const char *labels[]={"name","min","max","last","n"}; char sort[160]="  sort: ";
    for(unsigned i=0;i<5;++i) { size_t len=strlen(sort); snprintf(sort+len,sizeof(sort)-len,"%s%s%s",i?" ":"",labels[i],i==a->graph_sort.col?(a->graph_sort.descending?"▼":"▲"):""); }
    if(!*off) ft_put(f,r,0,0,FT_CYAN,sort);
    for(size_t i=0;i<n;++i) if(i+1>=*off) {
        char min[FT_TEXT],max[FT_TEXT],last[FT_TEXT]; value(order[i].summary.min,min); value(order[i].summary.max,max); value(order[i].summary.last,last);
        ft_printf(f,r,0,(int)(i+1-*off),order[i].idx==a->graph_peer?FT_SELECT:FT_NORMAL,"%s%-18s [%s]  min %8s  max %8s  last %8s  n=%5zu",order[i].idx==a->graph_peer?"▶ ":"  ",ft_str(order[i].peer,"display_name"),unit(metric),min,max,last,order[i].summary.n);
    }
    free(order);
}
