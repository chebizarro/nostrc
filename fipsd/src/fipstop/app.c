#include "app.h"
#include "fipstop-version.h"
#include "fipsd_build_info.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
uint64_t ft_monotonic_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
void ft_replace(json_t **slot, json_t *value) { json_decref(*slot); *slot = value; }
void ft_set_error(ft_app *a, const char *s)
{ free(a->error); a->error = strdup(s ? s : "unknown error"); a->error_ms = ft_monotonic_ms(); }
static char *default_gateway(void)
{
#if defined(_WIN32)
    /* Upstream default_gateway_path on Windows: TCP port 21211 (there is no
     * gateway there; the tab reports it not running). */
    return strdup("21211");
#else
    char *control = fipsctl_default_socket_path();
    if (!control) return NULL;
    char *slash = strrchr(control, '/');
    char *out;
    if (!strcmp(control, "/tmp/fips-control.sock")) out = strdup("/tmp/fips-gateway.sock");
    else { if (slash) *slash = 0; out = fipsctl_aprintf("%s/gateway.sock", slash ? control : "."); }
    free(control); return out;
#endif
}
void ft_app_init(ft_app *a, unsigned refresh, const char *socket, const char *gateway)
{
    memset(a, 0, sizeof(*a)); a->refresh = refresh; a->graph_window = 1;
    for (size_t i = 0; i < FT_TABS; ++i) a->selected[i] = -1;
    a->socket = socket ? strdup(socket) : fipsctl_default_socket_path();
    a->adapter = a->socket ? fipsctl_ipv6tun_client(a->socket) : NULL;
    a->gateway = gateway ? strdup(gateway) : default_gateway();
    a->expanded = json_object();
}
void ft_app_free(ft_app *a)
{
    for (size_t i = 0; i < FT_TABS; ++i) json_decref(a->data[i]);
    json_decref(a->listening); json_decref(a->mappings); json_decref(a->stats_peers); json_decref(a->expanded);
    free(a->socket); free(a->adapter); free(a->gateway); free(a->error);
    free(a->confirm_npub); free(a->confirm_name);
}
static json_t *ask(const char *socket, const char *command, json_t *params, char **error)
{
    char *request = params ? fipsctl_build_command(command, params) : fipsctl_build_query(command);
    if (!request) return NULL;
    json_t *reply = fipsctl_send_request(socket, request, error); free(request);
    if (!reply) return NULL;
    json_t *out = NULL;
    if (fipsctl_is_ok(reply)) out = json_incref(json_object_get(reply, "data"));
    else if (error && !*error) *error = strdup(ft_str(reply, "message"));
    json_decref(reply); return out;
}
static json_t *adapter_ask(ft_app *a, bool ready, const char *command)
{
    if (!ready || !a->adapter || a->adapter_quiet) return NULL;
    char *error = NULL; json_t *data = ask(a->adapter, command, NULL, &error);
    if (error && !strncmp(error, "cannot connect", 14)) a->adapter_quiet = 10;
    free(error); return data;
}
static void fetch_tab(ft_app *a, ft_tab tab)
{
    char *e = NULL; json_t *v = ask(a->socket, ft_commands[tab], NULL, &e);
    if (v) ft_replace(&a->data[tab], v); else ft_set_error(a, e);
    free(e);
}
void ft_fetch(ft_app *a)
{
    bool ready = a->adapter_quiet == 0;
    if (a->adapter_quiet) --a->adapter_quiet;
    char *e = NULL; json_t *node = ask(a->socket, "show_status", NULL, &e);
    a->connected = node != NULL;
    if (!node) { ft_set_error(a,e); free(e); return; }
    free(e);
    if (fipsctl_composes("show_status",node)) {
        json_t *adapter = adapter_ask(a,ready,"show_status");
        fipsctl_compose("show_status",node,adapter); json_decref(adapter);
    }
    ft_replace(&a->data[FT_NODE],node);
    if (a->tab == FT_NODE) {
        json_t *listeners = adapter_ask(a,ready,"show_listening_sockets");
        if (!listeners) listeners = ask(a->socket,"show_listening_sockets",NULL,NULL);
        ft_replace(&a->listening,listeners);
    } else if (a->tab == FT_GATEWAY) {
        json_t *gateway = ask(a->gateway,"show_gateway",NULL,NULL);
        a->gateway_running = gateway != NULL;
        ft_replace(&a->data[FT_GATEWAY],gateway);
        ft_replace(&a->mappings,gateway ? ask(a->gateway,"show_mappings",NULL,NULL) : NULL);
    } else if (a->tab == FT_GRAPHS) {
        json_t *peers = ask(a->socket,"show_stats_peers",NULL,NULL);
        if (peers) ft_replace(&a->stats_peers,peers);
        size_t n = json_array_size(ft_get(a->stats_peers,"peers"));
        if (a->graph_peer >= n) a->graph_peer = 0;
        json_t *params = json_pack("{s:s,s:s}","window",ft_windows[a->graph_window],
            "granularity",ft_granularities[a->graph_window]);
        const char *command = "show_stats_all_history";
        if (a->graph_mode == 1) {
            command = "show_stats_history_all_peers";
            json_object_set_new(params,"metric",json_string(ft_peer_metrics[a->graph_metric]));
        } else if (a->graph_mode == 2) {
            json_t *peer = json_array_get(ft_get(a->stats_peers,"peers"),a->graph_peer);
            json_object_set_new(params,"peer",json_string(ft_str(peer,"npub")));
        }
        e = NULL;
        json_t *data;
        if (a->graph_mode == 2 && !n) { json_decref(params); data = json_object(); }
        else data = ask(a->socket,command,params,&e);
        if (data) ft_replace(&a->data[FT_GRAPHS],data); else ft_set_error(a,e);
        free(e);
    } else fetch_tab(a,a->tab);
    if (a->tab == FT_PEERS || a->tab == FT_TRANSPORTS) {
        fetch_tab(a,FT_LINKS);
        fetch_tab(a,a->tab == FT_PEERS ? FT_TRANSPORTS : FT_PEERS);
    }
    if (a->tab == FT_TREE || a->tab == FT_BLOOM) fetch_tab(a,FT_PEERS);
    if (a->tab == FT_ROUTING) fetch_tab(a,FT_CACHE);
    a->fetched_ms = ft_monotonic_ms();
}
static json_t *load_fixture(const char *dir, const char *name)
{
    char *path = fipsctl_aprintf("%s/%s.json",dir,name); if (!path) return NULL;
    FILE *f = fopen(path,"rb"); free(path); if (!f) return NULL;
    if (fseek(f,0,SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f); rewind(f);
    if (size < 0 || size > 16*1024*1024) { fclose(f); return NULL; }
    char *text = malloc((size_t)size+1); if (!text) { fclose(f); return NULL; }
    size_t n = fread(text,1,(size_t)size,f); fclose(f); text[n] = 0;
    json_error_t error; json_t *reply = fipsctl_json_load(text,n,&error); free(text);
    json_t *data = json_incref(json_object_get(reply,"data")); json_decref(reply); return data;
}
bool ft_load_fixtures(ft_app *a, const char *dir, FILE *err)
{
    for (size_t i = 0; i < FT_TABS; ++i) ft_replace(&a->data[i],load_fixture(dir,ft_commands[i]));
    ft_replace(&a->stats_peers,load_fixture(dir,"show_stats_peers"));
    ft_replace(&a->listening,load_fixture(dir,"show_listening_sockets"));
    ft_replace(&a->mappings,load_fixture(dir,"show_mappings"));
    if (!a->data[FT_NODE]) { fprintf(err,"error: no show_status fixture in %s\n",dir); return false; }
    fipsctl_compose("show_status",a->data[FT_NODE],NULL);
    a->connected = true; a->gateway_running = a->data[FT_GATEWAY] != NULL; return true;
}
json_t *ft_rows(const ft_app *a, ft_tab tab)
{
    const char *key = tab == FT_PEERS ? "peers" : tab == FT_SESSIONS ? "sessions" :
        tab == FT_TRANSPORTS ? "transports" : tab == FT_LINKS ? "links" : "mappings";
    return ft_get(tab == FT_GATEWAY ? a->mappings : a->data[tab],key);
}
bool ft_expanded(const ft_app *a, const json_t *transport)
{
    char id[FT_TEXT]; ft_value(transport,"transport_id",id,sizeof(id));
    return json_is_true(json_object_get(a->expanded,id));
}
void ft_expand(ft_app *a, const json_t *transport, bool expanded)
{
    char id[FT_TEXT]; ft_value(transport,"transport_id",id,sizeof(id));
    json_object_set_new(a->expanded,id,json_boolean(expanded));
}
size_t ft_transport_rows(const ft_app *a, ft_transport_row **out)
{
    json_t *ts = ft_rows(a,FT_TRANSPORTS), *links = ft_rows(a,FT_LINKS);
    size_t cap = json_array_size(ts) + json_array_size(links), n = 0;
    *out = calloc(cap ? cap : 1,sizeof(**out)); if (!*out) return 0;
    size_t i,j; json_t *t,*l;
    json_array_foreach(ts,i,t) {
        if(n==cap) break;
        (*out)[n++] = (ft_transport_row){t,NULL};
        if (!ft_expanded(a,t)) continue;
        json_array_foreach(links,j,l) if (ft_uint(l,"transport_id") == ft_uint(t,"transport_id")) {
            if (n < cap) (*out)[n++] = (ft_transport_row){t,l};
        }
    }
    return n;
}
size_t ft_row_count(const ft_app *a)
{
    if (a->tab == FT_TRANSPORTS) {
        ft_transport_row *rows = NULL; size_t n = ft_transport_rows(a,&rows); free(rows); return n;
    }
    return json_array_size(ft_rows(a,a->tab));
}
json_t *ft_selected(const ft_app *a)
{
    int selected = a->selected[a->tab]; if (selected < 0) return NULL;
    if (a->tab == FT_PEERS) {
        size_t n; json_t **rows = ft_peer_order(ft_rows(a,FT_PEERS),&n);
        json_t *v = (size_t)selected < n ? rows[selected] : NULL; free(rows); return v;
    }
    if (a->tab == FT_TRANSPORTS) {
        ft_transport_row *rows = NULL; size_t n = ft_transport_rows(a,&rows);
        json_t *v = (size_t)selected < n ? (rows[selected].link ? rows[selected].link : rows[selected].transport) : NULL;
        free(rows); return v;
    }
    return json_array_get(ft_rows(a,a->tab),(size_t)selected);
}
void ft_disconnect(ft_app *a)
{
    if (!a->confirm_npub) return;
    json_t *params = json_pack("{s:s}","npub",a->confirm_npub);
    char *e = NULL; json_t *v = ask(a->socket,"disconnect",params,&e);
    if (!v) ft_set_error(a,e);
    free(e); json_decref(v);
    free(a->confirm_npub); a->confirm_npub = NULL;
    free(a->confirm_name); a->confirm_name = NULL;
}

const char *ft_base_version(void) { return FIPSTOP_VERSION; }
const char *ft_short_version(void)
{
    return FIPSD_GIT_REV[0] ? FIPSTOP_VERSION " (rev " FIPSD_GIT_REV ")" : FIPSTOP_VERSION;
}
void ft_version(FILE *out,bool long_form)
{
    fprintf(out,"fipstop %s\n",ft_short_version());
    if(long_form) fprintf(out,"target: %s\n",FIPSD_TARGET_TRIPLE);
}

uint64_t ft_wall_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_REALTIME,&t);
    return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
