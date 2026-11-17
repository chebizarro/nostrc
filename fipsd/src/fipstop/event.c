#include "app.h"
#include <stdlib.h>
#include <string.h>
#define TAB(t) (1u << (t))
#define ALL ((1u << 10) - 1)
#define TABLES (TAB(FT_PEERS)|TAB(FT_TRANSPORTS)|TAB(FT_SESSIONS)|TAB(FT_GATEWAY))
#define PANES (TAB(FT_TREE)|TAB(FT_BLOOM)|TAB(FT_ROUTING)|TAB(FT_MMP))
const ft_binding ft_bindings[] = {
    {'q',"q","Quit",ACT_QUIT,ALL},{3,"Ctrl-C","Quit",ACT_QUIT,ALL},
    {'\t',"Tab","Next tab",ACT_NEXT,ALL},{FT_BACKTAB,"Shift-Tab","Previous tab",ACT_PREV,ALL},
    {'g',"g","Graphs",ACT_GRAPH,ALL},{'?',"?","Help",ACT_HELP,ALL},
    {FT_ESCAPE,"Esc","Back / deselect",ACT_ESCAPE,ALL},
    {FT_UP,"↑","Up",ACT_UP,TABLES|PANES|TAB(FT_GRAPHS)},
    {FT_DOWN,"↓","Down",ACT_DOWN,TABLES|PANES|TAB(FT_GRAPHS)},
    {FT_ENTER,"Enter","Detail",ACT_ENTER,TABLES|TAB(FT_GRAPHS)},
    {FT_DELETE,"Del","Disconnect",ACT_DELETE,TAB(FT_PEERS)},
    {FT_RIGHT,"→ / Space","Expand / next window",ACT_RIGHT,TAB(FT_TRANSPORTS)|TAB(FT_GRAPHS)},
    {' ',"Space","Expand / next window",ACT_RIGHT,TAB(FT_TRANSPORTS)|TAB(FT_GRAPHS)},
    {FT_LEFT,"←","Collapse / previous window",ACT_LEFT,TAB(FT_TRANSPORTS)|TAB(FT_GRAPHS)},
    {'e',"e","Expand all",ACT_EXPAND,TAB(FT_TRANSPORTS)},
    {'c',"c","Collapse all",ACT_COLLAPSE,TAB(FT_TRANSPORTS)},
    {'f',"f","Focus pane",ACT_FOCUS,PANES},
    {FT_PAGEUP,"PgUp","Scroll up ten",ACT_PAGEUP,PANES},
    {FT_PAGEDOWN,"PgDn","Scroll down ten",ACT_PAGEDOWN,PANES},
    {FT_HOME,"Home","Top",ACT_HOME,PANES},{FT_END,"End","Bottom",ACT_END,PANES},
    {'s',"s","Sort column",ACT_SORT,TAB(FT_MMP)|TAB(FT_GRAPHS)},
    {'S',"S","Sort direction",ACT_REVERSE,TAB(FT_MMP)|TAB(FT_GRAPHS)},
    {'m',"m","View mode",ACT_MODE,TAB(FT_GRAPHS)},
    {'n',"n","Next selector",ACT_SELECTOR,TAB(FT_GRAPHS)},
    {'N',"N","Previous selector",ACT_PREV_SELECTOR,TAB(FT_GRAPHS)}
};
const size_t ft_binding_count = sizeof(ft_bindings)/sizeof(ft_bindings[0]);
bool ft_binding_active(const ft_app *a, const ft_binding *b)
{
    if (!(b->tabs & TAB(a->tab))) return false;
    if (a->help) return b->action == ACT_QUIT || b->action == ACT_HELP || b->action == ACT_ESCAPE;
    if (a->tab == FT_GRAPHS && (b->action == ACT_SORT || b->action == ACT_REVERSE || b->action == ACT_ENTER))
        return a->graph_mode == 1;
    if (b->action == ACT_DELETE) return a->selected[FT_PEERS] >= 0;
    if (b->action == ACT_ENTER && a->tab != FT_GRAPHS) return a->selected[a->tab] >= 0 && !a->detail;
    return true;
}
static void scroll_by(size_t *offset, int delta)
{
    if (delta < 0) *offset = *offset > (size_t)-delta ? *offset - (size_t)-delta : 0;
    else if (*offset < 65535 - (size_t)delta) *offset += (size_t)delta;
    else *offset = 65535;
}
bool ft_event(ft_app *a, int key)
{
    if (a->confirm_npub) {
        if (key == 'y' || key == 'Y') { ft_disconnect(a); return true; }
        if (key == 'n' || key == 'N' || key == FT_ESCAPE) {
            free(a->confirm_npub); free(a->confirm_name); a->confirm_npub = a->confirm_name = NULL;
        }
        if (key == 3 || key == 'q') a->quit = true;
        return false;
    }
    const ft_binding *b = NULL;
    for (size_t i=0;i<ft_binding_count;++i) if (ft_bindings[i].key==key && ft_binding_active(a,&ft_bindings[i])) { b=&ft_bindings[i]; break; }
    if (!b) return false;
    unsigned panes = a->tab == FT_MMP ? 2 : 3;
    size_t *off = &a->scroll[a->tab][a->focus[a->tab]];
    switch (b->action) {
    case ACT_QUIT: a->quit=true; break;
    case ACT_NEXT: case ACT_PREV: case ACT_GRAPH:
        a->tab = b->action == ACT_GRAPH ? FT_GRAPHS : (ft_tab)((a->tab + (b->action == ACT_NEXT ? 1 : 9)) % 10);
        a->detail=false; a->detail_scroll=0; return true;
    case ACT_HELP: a->help=!a->help; break;
    case ACT_ESCAPE:
        if (a->help) a->help=false;
        else if (a->detail) { a->detail=false; a->detail_scroll=0; }
        else a->selected[a->tab]=-1;
        break;
    case ACT_UP: case ACT_DOWN: {
        int delta = b->action == ACT_DOWN ? 1 : -1;
        if (a->tab == FT_GRAPHS) {
            if (a->graph_mode == 1) {
                size_t n=json_array_size(ft_get(a->data[FT_GRAPHS],"peers"));
                if (n) a->graph_peer=(a->graph_peer+n+(size_t)(delta>0?1:n-1))%n;
            } else scroll_by(&a->graph_scroll,delta);
        } else if (b->tabs & PANES && (TAB(a->tab) & PANES)) scroll_by(off,delta);
        else if (a->detail) scroll_by(&a->detail_scroll,delta);
        else {
            size_t n=ft_row_count(a); int *sel=&a->selected[a->tab];
            if (n) { if (*sel<0) *sel=0; else if (delta>0 && (size_t)(*sel+1)<n) ++*sel; else if (delta<0 && *sel>0) --*sel; }
        }
        break;
    }
    case ACT_ENTER:
        if (a->tab==FT_GRAPHS) a->detail=json_array_size(ft_get(a->data[FT_GRAPHS],"peers"))>0;
        else a->detail=ft_selected(a)!=NULL;
        a->detail_scroll=0; break;
    case ACT_DELETE: {
        json_t *p=ft_selected(a); const char *npub=ft_str(p,"npub");
        if (strcmp(npub,"-")) { a->confirm_npub=strdup(npub); a->confirm_name=strdup(ft_str(p,"display_name")); }
        break;
    }
    case ACT_RIGHT: case ACT_LEFT:
        if (a->tab==FT_GRAPHS) { a->graph_window=(a->graph_window+(b->action==ACT_RIGHT?1:3))%4; return true; }
        else {
            ft_transport_row *rows=NULL; size_t n=ft_transport_rows(a,&rows); int sel=a->selected[FT_TRANSPORTS];
            if (sel>=0 && (size_t)sel<n) ft_expand(a,rows[sel].transport,b->action==ACT_RIGHT);
            free(rows);
            n=ft_row_count(a); if (sel>=0 && (size_t)sel>=n) a->selected[FT_TRANSPORTS]=n?(int)n-1:-1;
        } break;
    case ACT_EXPAND: case ACT_COLLAPSE: {
        size_t i; json_t *t; json_array_foreach(ft_rows(a,FT_TRANSPORTS),i,t) ft_expand(a,t,b->action==ACT_EXPAND);
        a->selected[FT_TRANSPORTS]=-1; break;
    }
    case ACT_FOCUS: a->focus[a->tab]=(a->focus[a->tab]+1)%panes; break;
    case ACT_PAGEUP: scroll_by(off,-10); break;
    case ACT_PAGEDOWN: scroll_by(off,10); break;
    case ACT_HOME: *off=0; break;
    case ACT_END: *off=65535; break;
    case ACT_SORT: case ACT_REVERSE: {
        ft_sort *sort=a->tab==FT_MMP?&a->mmp_sort[a->focus[a->tab]]:&a->graph_sort;
        if (b->action==ACT_SORT) sort->col=(sort->col+1)%(a->tab==FT_MMP?6:5);
        else sort->descending=!sort->descending;
        break;
    }
    case ACT_MODE: a->graph_mode=(a->graph_mode+1)%3; a->graph_scroll=0; a->detail=false; return true;
    case ACT_SELECTOR: case ACT_PREV_SELECTOR:
        if (a->graph_mode==1) a->graph_metric=(a->graph_metric+(b->action==ACT_SELECTOR?1:6))%7;
        else if (a->graph_mode==2) {
            size_t n=json_array_size(ft_get(a->stats_peers,"peers"));
            if (n) a->graph_peer=(a->graph_peer+(b->action==ACT_SELECTOR?1:n-1))%n;
        }
        return a->graph_mode!=0;
    }
    return false;
}
