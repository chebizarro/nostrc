#include "ui.h"
#include <stdarg.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
bool ft_frame_init(ft_frame *f,int w,int h)
{
    if (w<1 || h<1 || w>1000 || h>500) return false;
    f->w=w; f->h=h; f->cells=calloc((size_t)w*(size_t)h,sizeof(*f->cells));
    if (!f->cells) return false;
    for (int i=0;i<w*h;++i) f->cells[i].ch=L' ';
    return true;
}
void ft_frame_free(ft_frame *f) { free(f->cells); f->cells=NULL; }
void ft_frame_print(const ft_frame *f,FILE *out)
{
    for (int y=0;y<f->h;++y) {
        for (int x=0;x<f->w;++x) {
            char text[MB_LEN_MAX]; mbstate_t state; memset(&state,0,sizeof(state));
            wchar_t ch=f->cells[y*f->w+x].ch;
            if (!ch) continue;
            size_t n=wcrtomb(text,ch,&state);
            if (n==(size_t)-1) fputc('?',out); else fwrite(text,1,n,out);
        }
        fputc('\n',out);
    }
}
void ft_put(ft_frame *f,ft_rect r,int x,int y,ft_style style,const char *text)
{
    if (!text || y<0 || y>=r.h || y+r.y<0 || y+r.y>=f->h) return;
    mbstate_t state; memset(&state,0,sizeof(state));
    while (*text && x<r.w) {
        wchar_t ch; size_t n=mbrtowc(&ch,text,strlen(text),&state);
        if (n==(size_t)-1 || n==(size_t)-2) { ch=L'?'; n=1; memset(&state,0,sizeof(state)); }
        if (!n) break;
        text+=n;
        if (ch<L' ' || ch==127) ch=L' ';
        int cw=wcwidth(ch); if (cw<1) cw=1;
        if (x>=0 && x+cw<=r.w && x+r.x>=0 && x+cw+r.x<=f->w) {
            int i=(y+r.y)*f->w+x+r.x; f->cells[i]=(ft_cell){ch,(unsigned char)style};
            for (int j=1;j<cw;++j) f->cells[i+j]=(ft_cell){0,(unsigned char)style};
        }
        x+=cw;
    }
}
void ft_printf(ft_frame *f,ft_rect r,int x,int y,ft_style style,const char *fmt,...)
{
    char b[4096]; va_list ap; va_start(ap,fmt); vsnprintf(b,sizeof(b),fmt,ap); va_end(ap);
    ft_put(f,r,x,y,style,b);
}
ft_rect ft_box(ft_frame *f,ft_rect r,const char *title,bool focused)
{
    ft_style s=focused?FT_CYAN:FT_NORMAL;
    for (int x=0;x<r.w;++x) { ft_put(f,r,x,0,s,"─"); if(r.h>1) ft_put(f,r,x,r.h-1,s,"─"); }
    for (int y=1;y<r.h-1;++y) { ft_put(f,r,0,y,s,"│"); if(r.w>1) ft_put(f,r,r.w-1,y,s,"│"); }
    ft_put(f,r,0,0,s,"┌"); if(r.w>1) ft_put(f,r,r.w-1,0,s,"┐");
    if (r.h>1) { ft_put(f,r,0,r.h-1,s,"└"); if(r.w>1) ft_put(f,r,r.w-1,r.h-1,s,"┘"); }
    ft_rect top=r; top.w=r.w>1?r.w-1:0; ft_put(f,top,1,0,s,title);
    return (ft_rect){r.x+1,r.y+1,r.w>2?r.w-2:0,r.h>2?r.h-2:0};
}
void ft_field(ft_frame *f,ft_rect r,int row,const char *label,const json_t *d,const char *path)
{
    char value[512]; ft_value(d,path,value,sizeof(value));
    ft_printf(f,r,0,row,FT_NORMAL,"    %s: %s",label,value);
}
void ft_fields(ft_frame *f,ft_rect r,const json_t *d,const char *const *labels,const char *const *paths,size_t n,size_t *scroll)
{
    *scroll=ft_clamp_scroll(*scroll,n,(size_t)r.h);
    for (size_t i=*scroll;i<n && i-*scroll<(size_t)r.h;++i) {
        if(paths[i]) ft_field(f,r,(int)(i-*scroll),labels[i],d,paths[i]);
        else ft_put(f,r,0,(int)(i-*scroll),FT_CYAN,labels[i]);
    }
}
static void flatten(ft_frame *f,ft_rect r,const json_t *v,int depth,size_t *line,size_t start)
{
    const char *key; json_t *child;
    json_object_foreach((json_t *)v,key,child) {
        if (json_is_object(child) || json_is_array(child)) {
            if (*line>=start) ft_printf(f,r,depth*2,(int)(*line-start),FT_CYAN,"  %s",key);
            ++*line;
            if (depth<6 && json_is_object(child)) flatten(f,r,child,depth+1,line,start);
            else if (depth<6) {
                size_t i; json_t *item; json_array_foreach(child,i,item) {
                    if (json_is_object(item)) flatten(f,r,item,depth+1,line,start);
                    else { char *text=fipsctl_json_compact(item); if(*line>=start) ft_put(f,r,depth*2+4,(int)(*line-start),FT_NORMAL,text); free(text); ++*line; }
                }
            }
        } else {
            char text[512]; ft_value(v,key,text,sizeof(text));
            if (*line>=start) ft_printf(f,r,depth*2,(int)(*line-start),FT_NORMAL,"    %s: %s",key,text);
            ++*line;
        }
    }
}
void ft_detail(ft_frame *f,ft_rect r,const json_t *d,size_t *scroll,const char *title)
{
    r=ft_box(f,r,title,true); size_t n=0; ft_frame dummy={0}; ft_rect empty={0};
    flatten(&dummy,empty,d,0,&n,SIZE_MAX);
    *scroll=ft_clamp_scroll(*scroll,n,(size_t)r.h); n=0; flatten(f,r,d,0,&n,*scroll);
}
static void help(ft_frame *f,ft_app *a,ft_rect r)
{
    int w=r.w>72?72:r.w, h=r.h; ft_rect modal={r.x+(r.w-w)/2,r.y,w,h};
    for(int y=0;y<modal.h;++y) for(int x=0;x<modal.w;++x) ft_put(f,modal,x,y,FT_NORMAL," ");
    modal=ft_box(f,modal," Keybindings ",true);
    ft_app context=*a; context.help=false;
    int row=0;
    for(size_t i=0;i<ft_binding_count && row<modal.h;++i) {
        const ft_binding *b=&ft_bindings[i];
        if(ft_binding_active(&context,b)) ft_printf(f,modal,1,row++,FT_NORMAL,"%-15s %s",b->label,b->hint);
    }
}
void ft_draw(ft_frame *f,ft_app *a)
{
    for(int i=0;i<f->w*f->h;++i) f->cells[i]=(ft_cell){L' ',FT_NORMAL};
    ft_rect full={0,0,f->w,f->h};
    if(f->h<5 || f->w<8) { ft_put(f,full,0,0,FT_YELLOW,"Resize terminal"); return; }
    char title[128]; snprintf(title,sizeof(title)," fipstop %s ",a->snapshot?"0.1.0":ft_short_version());
    ft_rect head=ft_box(f,(ft_rect){0,0,f->w,3},title,false);
    int x=0;
    for(int i=0;i<10;++i) {
        if(i) { const char *sep=i==1||i==3||i==9?" │ ":" | "; ft_put(f,head,x,0,FT_DIM,sep); x+=3; }
        ft_put(f,head,x,0,i==(int)a->tab?FT_YELLOW:FT_NORMAL,ft_labels[i]); x+=(int)strlen(ft_labels[i]);
    }
    ft_rect r={0,3,f->w,f->h-4};
    void (*draws[10])(ft_frame *,ft_app *,ft_rect)={ft_ui_node,ft_ui_peers,ft_ui_transports,ft_ui_sessions,
        ft_ui_tree,ft_ui_bloom,ft_ui_mmp,ft_ui_routing,ft_ui_graphs,ft_ui_gateway};
    if(a->data[a->tab] || a->tab==FT_GATEWAY) draws[a->tab](f,a,r);
    else ft_put(f,r,0,0,FT_DIM,"  Waiting for data...");
    ft_rect foot={0,f->h-1,f->w,1};
    char conn[256];
    if(a->connected) snprintf(conn,sizeof(conn)," Connected ");
    else { const char *e=a->error?a->error:"Not yet connected"; size_t n=strcspn(e,"\n"); snprintf(conn,sizeof(conn)," Disconnected: %.*s ",(int)(n>200?200:n),e); }
    ft_put(f,foot,0,0,a->connected?FT_GREEN:FT_RED,conn); x=(int)strlen(conn);
    char timing[128]; double elapsed=a->now_ms>a->fetched_ms?(double)(a->now_ms-a->fetched_ms)/1000:0;
    snprintf(timing,sizeof(timing),"| Refresh: %us | Updated: %.1fs ago ",a->refresh,elapsed);
    ft_put(f,foot,x,0,FT_NORMAL,timing); x+=(int)strlen(timing);
    struct hint { const char *key,*label; } candidates[12]; size_t hints=0;
#define H(k,l) do { candidates[hints++]=(struct hint){k,l}; } while(0)
    if(a->detail) {
        if(a->tab==FT_GRAPHS) { H("↑↓","peer"); H("n/N","stat"); H("m","mode"); H("Esc","back"); }
        else { H("Esc","close"); H("↑↓","scroll"); }
    } else if(a->selected[a->tab]>=0) {
        H("Enter","detail"); if(a->tab==FT_PEERS) H("Del","disconnect"); H("Esc","deselect");
    } else if(a->tab==FT_PEERS||a->tab==FT_SESSIONS||a->tab==FT_TRANSPORTS||a->tab==FT_GATEWAY) H("↑↓","select");
    else if(a->tab==FT_GRAPHS) {
        H("Enter","expand"); H("m","mode"); H("n/N","stat"); H("←→","window"); H("s/S","sort");
    } else if(a->tab==FT_TREE||a->tab==FT_BLOOM||a->tab==FT_ROUTING||a->tab==FT_MMP) {
        H("f","focus pane"); H("↑↓","scroll"); if(a->tab==FT_MMP) H("s/S","sort");
    }
    H("Tab","next"); H("g","graphs"); H("q","quit");
#undef H
    for(size_t i=0;i<hints;++i) {
        char hint[96]; snprintf(hint,sizeof(hint),"[%s] %s ",candidates[i].key,candidates[i].label);
        int n=(int)fipsctl_utf8_chars(hint);
        if(x+n+9<=f->w) { ft_put(f,foot,x,0,FT_DIM,hint); x+=n; }
    }
    ft_put(f,foot,x,0,FT_DIM,"[?] Help ");
    if(a->help) help(f,a,r);
    if(a->confirm_npub) {
        ft_rect modal={r.x+2,r.y+r.h/3,r.w-4,r.h>7?7:r.h};
        for(int y=0;y<modal.h;++y) for(int xx=0;xx<modal.w;++xx) ft_put(f,modal,xx,y,FT_NORMAL," ");
        modal=ft_box(f,modal," Disconnect peer? ",true);
        ft_printf(f,modal,1,0,FT_YELLOW,"Disconnect %s?",a->confirm_name);
        ft_put(f,modal,1,1,FT_NORMAL,"It stays disconnected until you manually reconnect it.");
        ft_put(f,modal,1,3,FT_NORMAL,"[Y] Disconnect   [N / Esc] Cancel");
    }
}

void ft_table_header(ft_frame *f,ft_rect r,const ft_column *cols,size_t n)
{
    int x=0;
    for(size_t i=0;i<n;++i) {
        ft_rect cell={r.x+x,r.y,cols[i].width,r.h};
        if(x+cell.w>r.w) cell.w=r.w-x;
        ft_put(f,cell,0,0,FT_YELLOW,cols[i].label); x+=cols[i].width+1;
    }
}
void ft_table_row(ft_frame *f,ft_rect r,int row,const ft_column *cols,size_t n,const json_t *d,bool selected)
{
    int x=0;
    if(selected) { for(int i=0;i<r.w;++i) ft_put(f,r,i,row,FT_SELECT," "); ft_put(f,r,0,row,FT_SELECT,"▶"); }
    for(size_t i=0;i<n;++i) {
        char value[512]; ft_value(d,cols[i].path,value,sizeof(value));
        char *text=ft_truncate(value,(size_t)cols[i].width,false);
        ft_rect cell={r.x+x,r.y,cols[i].width,r.h}; if(x+cell.w>r.w) cell.w=r.w-x;
        ft_put(f,cell,0,row,selected?FT_SELECT:FT_NORMAL,text); free(text); x+=cols[i].width+1;
    }
}

void ft_split3(ft_rect r,int first,int second,ft_rect out[3])
{
    int third=r.h-first-second;
    if(third<3) {
        third=r.h<3?r.h:3;
        int available=r.h-third;
        if(first>(available+1)/2) first=(available+1)/2;
        second=available-first;
    }
    if(first<0) first=0;
    out[0]=(ft_rect){r.x,r.y,r.w,first};
    out[1]=(ft_rect){r.x,r.y+first,r.w,second};
    out[2]=(ft_rect){r.x,r.y+first+second,r.w,third};
}
json_t *ft_role_peers(const ft_app *a,const json_t *rows,const char *addr_key)
{
    json_t *out=json_deep_copy(rows); if(!out) return json_array();
    size_t i,j; json_t *v,*p;
    json_array_foreach(out,i,v) json_array_foreach(ft_rows(a,FT_PEERS),j,p) {
        if(!strcmp(ft_str(v,addr_key),ft_str(p,"node_addr"))) {
            json_object_set(v,"is_parent",ft_get(p,"is_parent")); json_object_set(v,"is_child",ft_get(p,"is_child"));
            json_object_set(v,"effective_depth",ft_get(p,"effective_depth")); break;
        }
    }
    return out;
}

void ft_aligned_fields(ft_frame *f,ft_rect r,const json_t *d,const char *const *labels,
    const char *const *paths,size_t n,size_t *scroll)
{
    *scroll=ft_clamp_scroll(*scroll,n,(size_t)r.h);
    for(size_t i=*scroll;i<n && i-*scroll<(size_t)r.h;++i) {
        if(!paths[i]) { ft_put(f,r,0,(int)(i-*scroll),FT_CYAN,labels[i]); continue; }
        size_t begin=i,end=i+1,width=0;
        while(begin && paths[begin-1]) --begin;
        while(end<n && paths[end]) ++end;
        for(size_t j=begin;j<end;++j) if(strlen(labels[j])>width) width=strlen(labels[j]);
        char value[512]; ft_value(d,paths[i],value,sizeof(value));
        ft_printf(f,r,0,(int)(i-*scroll),FT_NORMAL,"    %-*s: %s",(int)width,labels[i],value);
    }
}
void ft_scrollbar(ft_frame *f,ft_rect r,size_t count,size_t selected)
{
    if(!count || r.h<=0) return;
    size_t visible=(size_t)r.h;
    size_t thumb=count<=visible?visible:visible*visible/count;
    if(!thumb) thumb=1;
    size_t position=count>1?(visible-thumb)*selected/(count-1):0;
    for(size_t i=0;i<visible;++i)
        ft_put(f,r,r.w-1,(int)i,FT_DIM,i>=position && i<position+thumb?"█":"║");
}
