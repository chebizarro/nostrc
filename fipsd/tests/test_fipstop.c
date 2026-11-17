#include "ui.h"
#include <assert.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
static bool has(const ft_frame *f,const char *text)
{
    wchar_t needle[256]; size_t n=mbstowcs(needle,text,255); assert(n!=(size_t)-1);
    for(int y=0;y<f->h;++y) for(int x=0;x<f->w && x+(int)n<=f->w;++x) {
        size_t i=0; while(i<n && f->cells[y*f->w+x+(int)i].ch==needle[i]) ++i;
        if(i==n) return true;
    }
    return false;
}
static json_t *parse(const char *s) { json_error_t e; json_t *d=json_loads(s,0,&e); assert(d); return d; }
int main(int argc,char **argv)
{
    if(argc==4 && !strcmp(argv[1],"--fetch")) {
        ft_app a; ft_app_init(&a,2,argv[2],NULL);
        unsigned count=(unsigned)strtoul(argv[3],NULL,10);
        for(unsigned i=0;i<count;++i) {
            ft_fetch(&a);
            json_t *out=json_pack("{s:i,s:b,s:O,s:O}","quiet",(int)a.adapter_quiet,"connected",a.connected,
                "status",a.data[FT_NODE]?a.data[FT_NODE]:json_null(),"listening",a.listening?a.listening:json_null());
            char *text=fipsctl_json_compact(out); puts(text); free(text); json_decref(out);
        }
        ft_app_free(&a); return 0;
    }
    assert(setlocale(LC_ALL,"")); ft_app a; ft_app_init(&a,2,"/not/a/socket",NULL);
    assert(ft_load_fixtures(&a,FIPSTOP_FIXTURES,stderr));
    const char *titles[]={"Runtime","Peers (0)","Transports (0)","Sessions (0)","Tree Position","Bloom Filter State","Link MMP","Routing State","Graphs — Node","Gateway not running"};
    for(int size=0;size<2;++size) for(int tab=0;tab<10;++tab) {
        a.tab=(ft_tab)tab; ft_frame f; assert(ft_frame_init(&f,size?120:80,size?40:24));
        ft_draw(&f,&a); assert(has(&f,titles[tab])); ft_frame_free(&f);
    }
    a.tab=FT_NODE; assert(ft_event(&a,'\t') && a.tab==FT_PEERS);
    assert(ft_event(&a,FT_BACKTAB) && a.tab==FT_NODE);
    assert(ft_event(&a,'g') && a.tab==FT_GRAPHS && a.graph_window==1);
    assert(ft_event(&a,FT_RIGHT) && a.graph_window==2);
    assert(ft_event(&a,FT_LEFT) && a.graph_window==1);
    assert(ft_event(&a,'m') && a.graph_mode==1);
    assert(ft_event(&a,'N') && a.graph_metric==6);
    assert(ft_event(&a,'n') && a.graph_metric==0);
    ft_event(&a,'s'); assert(a.graph_sort.col==1); ft_event(&a,'S'); assert(a.graph_sort.descending);
    a.tab=FT_TREE; ft_event(&a,'f'); assert(a.focus[FT_TREE]==1);
    ft_event(&a,FT_END); assert(a.scroll[FT_TREE][1]==65535); ft_event(&a,FT_HOME); assert(!a.scroll[FT_TREE][1]);
    ft_event(&a,FT_PAGEDOWN); assert(a.scroll[FT_TREE][1]==10); ft_event(&a,FT_PAGEUP); assert(!a.scroll[FT_TREE][1]);
    a.tab=FT_PEERS;
    ft_replace(&a.data[FT_PEERS],parse("{\"peers\":[{\"npub\":\"other\",\"display_name\":\"other\",\"mmp\":{\"lqi\":1}},{\"npub\":\"parent\",\"display_name\":\"parent\",\"is_parent\":true,\"mmp\":{\"lqi\":9}}]}"));
    ft_event(&a,FT_DOWN); assert(a.selected[FT_PEERS]==0);
    ft_event(&a,FT_ENTER); assert(a.detail);
    ft_event(&a,FT_ESCAPE); assert(!a.detail && a.selected[FT_PEERS]==0);
    ft_event(&a,FT_DELETE); assert(!strcmp(a.confirm_npub,"parent"));
    ft_event(&a,'n'); assert(!a.confirm_npub);
    ft_event(&a,'?'); assert(a.help); ft_event(&a,FT_DELETE); assert(!a.confirm_npub);
    ft_event(&a,FT_ESCAPE); assert(!a.help); ft_event(&a,FT_ESCAPE); assert(a.selected[FT_PEERS]==-1);
    a.tab=FT_TRANSPORTS;
    ft_replace(&a.data[FT_TRANSPORTS],parse("{\"transports\":[{\"transport_id\":1,\"type\":\"ethernet\",\"name\":\"lan\",\"interface\":{\"name\":\"mesh0\",\"presence\":\"absent\",\"policy\":\"optional\"}}]}"));
    ft_replace(&a.data[FT_LINKS],parse("{\"links\":[{\"transport_id\":1,\"remote_addr\":\"00:11:22:33:44:55\",\"direction\":\"Outbound\"}]}"));
    assert(ft_row_count(&a)==1); ft_event(&a,FT_DOWN); ft_event(&a,FT_RIGHT); assert(ft_row_count(&a)==2);
    ft_event(&a,FT_DOWN); assert(a.selected[FT_TRANSPORTS]==1); ft_event(&a,FT_LEFT); assert(ft_row_count(&a)==1 && a.selected[FT_TRANSPORTS]==0);
    ft_event(&a,'e'); assert(ft_row_count(&a)==2); ft_event(&a,'c'); assert(ft_row_count(&a)==1);
    ft_frame f; assert(ft_frame_init(&f,80,24)); ft_draw(&f,&a); assert(has(&f,"mesh0") && has(&f,"absent")); ft_frame_free(&f);
    a.tab=FT_MMP; ft_event(&a,'s'); assert(a.mmp_sort[0].col==1); ft_event(&a,'f'); ft_event(&a,'S'); assert(a.mmp_sort[1].descending && !a.mmp_sort[0].descending);
    ft_replace(&a.data[FT_GRAPHS],parse("{\"series\":[{\"metric\":\"mesh_size\",\"values\":[7,7,7,7]}]}"));
    a.tab=FT_GRAPHS; a.graph_mode=0; assert(ft_frame_init(&f,80,24)); ft_draw(&f,&a); assert(has(&f,"min 7.00") && has(&f,"⣀")); ft_frame_free(&f);
    ft_replace(&a.data[FT_GRAPHS],parse("{\"series\":[{\"metric\":\"mesh_size\",\"values\":[0,0,0,0]}]}"));
    assert(ft_frame_init(&f,80,24)); ft_draw(&f,&a); assert(!has(&f,"⣀") && has(&f,"samples 4")); ft_frame_free(&f);
    ft_replace(&a.data[FT_GRAPHS],parse("{\"series\":[{\"metric\":\"mesh_size\",\"values\":[]}]}"));
    assert(ft_frame_init(&f,80,24)); ft_draw(&f,&a); assert(has(&f,"no samples")); ft_frame_free(&f);
    const double v[]={1,2,3,4,5}; double out[5]; ft_resample(v,5,out,5); for(size_t i=0;i<5;++i) assert(out[i]==v[i]);

    /* Collapsing a child must select its own parent, not the following transport. */
    a.tab=FT_TRANSPORTS;
    json_t *following=parse("{\"transport_id\":2,\"type\":\"udp\"}");
    json_array_append_new(ft_rows(&a,FT_TRANSPORTS),following);
    a.selected[FT_TRANSPORTS]=0; ft_event(&a,FT_RIGHT); ft_event(&a,FT_DOWN);
    assert(a.selected[FT_TRANSPORTS]==1 && ft_row_count(&a)==3);
    ft_event(&a,FT_LEFT); assert(a.selected[FT_TRANSPORTS]==0 && ft_row_count(&a)==2);
    assert(ft_frame_init(&f,8,5));
    ft_put(&f,(ft_rect){0,0,8,5},0,0,FT_NORMAL,"e\u0301x界y");
    assert(f.cells[0].ch==L'e' && f.cells[1].ch==L'?' && f.cells[2].ch==L'x');
    assert(f.cells[3].ch==L'界' && f.cells[4].ch==0 && f.cells[5].ch==L'y');
    ft_frame_free(&f);
    ft_app_free(&a); puts("fipstop: ten panes x two sizes, input/focus/selection/confirmation, transport tree, graph model passed"); return 0;
}
