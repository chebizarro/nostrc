#define _XOPEN_SOURCE_EXTENDED 1
#include "ui.h"
#include <curses.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t stopped;
static void on_signal(int sig) { (void)sig; stopped=1; }
static void usage(FILE *out)
{
    fputs("Monitor a running FIPS daemon\n\nUsage: fipstop [-s SOCKET] [--gateway-socket PATH] [-r SECONDS]\n\n"
        "  -s, --socket PATH          Daemon control socket (same resolution as fipsctl)\n"
        "      --gateway-socket PATH  Gateway control socket\n"
        "  -r, --refresh SECONDS      Refresh interval [default: 2]\n"
        "      --no-color             Disable colours (also NO_COLOR)\n"
        "  -V, --version             Short / long version\n"
        "  -h, --help                Print help\n",out);
}
static int keycode(int c)
{
    switch(c) {
    case KEY_UP:return FT_UP; case KEY_DOWN:return FT_DOWN;
    case KEY_LEFT:return FT_LEFT; case KEY_RIGHT:return FT_RIGHT;
    case KEY_ENTER:case '\r':case '\n':return FT_ENTER;
    case 27:return FT_ESCAPE; case KEY_BTAB:return FT_BACKTAB;
    case KEY_PPAGE:return FT_PAGEUP; case KEY_NPAGE:return FT_PAGEDOWN;
    case KEY_HOME:return FT_HOME; case KEY_END:return FT_END; case KEY_DC:return FT_DELETE;
    default:return c;
    }
}
static int scripted_key(const char *s)
{
    const char *names[]={"Up","Down","Left","Right","Enter","Esc","BackTab","PageUp","PageDown","Home","End","Del"};
    for(size_t i=0;i<sizeof(names)/sizeof(names[0]);++i) if(!strcmp(s,names[i])) return FT_UP+(int)i;
    return !strcmp(s,"Tab")?'\t':!strcmp(s,"Space")?' ':s[0];
}
static void fixture_graph(ft_app *a,const char *dir)
{
    if(a->tab!=FT_GRAPHS || a->graph_mode!=1) return;
    char *path=fipsctl_aprintf("%s/show_stats_history_all_peers.json",dir);
    json_error_t e; json_t *reply=path?json_load_file(path,0,&e):NULL; free(path);
    if(reply) { ft_replace(&a->data[FT_GRAPHS],json_incref(ft_get(reply,"data"))); json_decref(reply); }
}
int main(int argc,char **argv)
{
    setlocale(LC_ALL,"");
    const char *socket=NULL,*gateway=NULL,*fixtures=NULL,*keys=NULL;
    unsigned interval=2; int w=0,h=0; ft_tab tab=FT_NODE; bool color=getenv("NO_COLOR")==NULL;
    for(int i=1;i<argc;++i) {
        const char *arg=argv[i];
        if(!strcmp(arg,"-h")||!strcmp(arg,"--help")) { usage(stdout); return 0; }
        if(!strcmp(arg,"-V")||!strcmp(arg,"--version")) {
            puts("fipstop 0.1.0");
            if(!strcmp(arg,"--version")) puts("target: " FIPSTOP_TARGET);
            return 0;
        }
        if(!strcmp(arg,"--no-color")) { color=false; continue; }
        if(i+1>=argc) { fprintf(stderr,"error: missing value for %s\n",arg); usage(stderr); return 1; }
        const char *value=argv[++i];
        if(!strcmp(arg,"-s")||!strcmp(arg,"--socket")) socket=value;
        else if(!strcmp(arg,"--gateway-socket")) gateway=value;
        else if(!strcmp(arg,"-r")||!strcmp(arg,"--refresh")) {
            char *end; errno=0; unsigned long n=strtoul(value,&end,10);
            if(errno||!value[0]||*end||value[0]=='-'||n>INT_MAX/1000) { fputs("error: invalid refresh interval\n",stderr); return 1; }
            interval=(unsigned)n;
        } else if(!strcmp(arg,"--render-once")) {
            char tail; if(sscanf(value,"%dx%d%c",&w,&h,&tail)!=2||w<1||h<1||w>1000||h>500) { fputs("error: invalid frame size (1x1 through 1000x500)\n",stderr); return 1; }
        } else if(!strcmp(arg,"--fixture-dir")) fixtures=value;
        else if(!strcmp(arg,"--keys")) keys=value;
        else if(!strcmp(arg,"--tab")) {
            bool found=false; for(int j=0;j<10;++j) if(!strcmp(value,ft_labels[j])) { tab=(ft_tab)j; found=true; break; }
            if(!found) { fputs("error: unknown tab\n",stderr); return 1; }
        } else { fprintf(stderr,"error: unknown option %s\n",arg); usage(stderr); return 1; }
    }
    ft_app app; ft_app_init(&app,interval,socket,gateway); app.tab=tab;
    if(!app.socket || !app.gateway || !app.expanded) { fputs("error: out of memory\n",stderr); ft_app_free(&app); return 1; }
    if(fixtures) {
        if(!ft_load_fixtures(&app,fixtures,stderr)) { ft_app_free(&app); return 1; }
    } else ft_fetch(&app);
    if(keys) {
        char *copy=strdup(keys); if(!copy) { ft_app_free(&app); return 1; }
        char *save=NULL;
        for(char *key=strtok_r(copy,",",&save);key;key=strtok_r(NULL,",",&save)) {
            if(ft_event(&app,scripted_key(key))) { if(fixtures) fixture_graph(&app,fixtures); else ft_fetch(&app); }
        }
        free(copy);
    }
    if(w && h) {
        ft_frame frame;
        if(!ft_frame_init(&frame,w,h)) { ft_app_free(&app); return 1; }
        app.now_ms=app.fetched_ms; app.wall_ms=fixtures?1700000000000ULL:(uint64_t)time(NULL)*1000;
        fixture_graph(&app,fixtures?fixtures:""); ft_draw(&frame,&app); ft_frame_print(&frame,stdout);
        ft_frame_free(&frame); ft_app_free(&app); return ferror(stdout)?1:0;
    }
    if(!isatty(STDIN_FILENO)||!isatty(STDOUT_FILENO)) { fputs("error: terminal required\n",stderr); ft_app_free(&app); return 1; }
    SCREEN *screen=newterm(NULL,stdout,stdin);
    if(!screen) { fputs("error: failed to initialise terminal\n",stderr); ft_app_free(&app); return 1; }
    cbreak(); noecho(); keypad(stdscr,TRUE); curs_set(0);
    color=color && has_colors();
    if(color) {
        start_color(); use_default_colors();
        init_pair(FT_DIM,COLOR_WHITE,-1); init_pair(FT_CYAN,COLOR_CYAN,-1); init_pair(FT_YELLOW,COLOR_YELLOW,-1);
        init_pair(FT_GREEN,COLOR_GREEN,-1); init_pair(FT_RED,COLOR_RED,-1); init_pair(FT_SELECT,COLOR_BLACK,COLOR_CYAN);
    }
    struct sigaction action; memset(&action,0,sizeof(action)); action.sa_handler=on_signal; sigemptyset(&action.sa_mask);
    sigaction(SIGINT,&action,NULL); sigaction(SIGTERM,&action,NULL); sigaction(SIGHUP,&action,NULL);
    int rc=0; uint64_t next=ft_monotonic_ms()+(uint64_t)interval*1000;
    while(!app.quit && !stopped) {
        int rows,cols; getmaxyx(stdscr,rows,cols); ft_frame frame;
        if(!ft_frame_init(&frame,cols,rows)) { rc=1; break; }
        app.now_ms=ft_monotonic_ms(); app.wall_ms=(uint64_t)time(NULL)*1000; ft_draw(&frame,&app);
        erase();
        for(int y=0;y<rows;++y) for(int x=0;x<cols;++x) {
            ft_cell cell=frame.cells[y*cols+x]; if(!cell.ch) continue;
            attr_t attr=color?COLOR_PAIR(cell.style):A_NORMAL;
            if(cell.style==FT_CYAN||cell.style==FT_YELLOW||cell.style==FT_SELECT) attr|=A_BOLD;
            if(cell.style==FT_DIM) attr|=A_DIM;
            if(!color && cell.style==FT_SELECT) attr|=A_REVERSE;
            attrset(attr);
#if NCURSES_WIDECHAR
            wchar_t text[2]={cell.ch,0}; mvaddnwstr(y,x,text,1);
#else
            mvaddch(y,x,cell.ch<128?(chtype)cell.ch:(chtype)'?');
#endif
        }
        ft_frame_free(&frame); refresh();
        uint64_t now=ft_monotonic_ms(); int wait=next>now?(int)(next-now):0;
        wtimeout(stdscr,wait); int c=getch();
        bool fetch=c!=ERR && c!=KEY_RESIZE && ft_event(&app,keycode(c));
        now=ft_monotonic_ms();
        if(now>=next || fetch) {
            if(!fixtures) ft_fetch(&app); else fixture_graph(&app,fixtures);
            next=ft_monotonic_ms()+(app.refresh?(uint64_t)app.refresh*1000:1);
        }
    }
    endwin(); delscreen(screen); ft_app_free(&app); return rc;
}
