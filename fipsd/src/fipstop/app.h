#ifndef FIPSTOP_APP_H
#define FIPSTOP_APP_H
#include "model.h"
#include <wchar.h>
typedef struct ft_sort { unsigned col; bool descending; } ft_sort;
typedef struct ft_app {
    ft_tab tab;
    bool snapshot, quit, connected, gateway_running, help, detail;
    unsigned refresh, adapter_quiet;
    json_t *data[FT_TABS], *listening, *mappings, *stats_peers, *expanded;
    int selected[FT_TABS];
    unsigned focus[FT_TABS];
    size_t scroll[FT_TABS][3], detail_scroll, table_scroll[FT_TABS];
    unsigned graph_mode, graph_window, graph_metric;
    size_t graph_peer, graph_scroll;
    ft_sort mmp_sort[2], graph_sort;
    char *socket, *adapter, *gateway, *error, *confirm_npub, *confirm_name;
    uint64_t fetched_ms, error_ms, now_ms, wall_ms;
} ft_app;
const char *ft_short_version(void);
void ft_version(FILE *out,bool long_form);
void ft_app_init(ft_app *a, unsigned refresh, const char *socket, const char *gateway);
void ft_app_free(ft_app *a);
void ft_fetch(ft_app *a);
bool ft_load_fixtures(ft_app *a, const char *directory, FILE *err);
json_t *ft_rows(const ft_app *a, ft_tab tab);
size_t ft_row_count(const ft_app *a);
json_t *ft_selected(const ft_app *a);
void ft_disconnect(ft_app *a);
uint64_t ft_monotonic_ms(void);
void ft_set_error(ft_app *a, const char *s);
void ft_replace(json_t **slot, json_t *value);
/* Transport tree is rebuilt from the current cross-references; refs are borrowed. */
typedef struct ft_transport_row { json_t *transport, *link; } ft_transport_row;
size_t ft_transport_rows(const ft_app *a, ft_transport_row **out);
bool ft_expanded(const ft_app *a, const json_t *transport);
void ft_expand(ft_app *a, const json_t *transport, bool expanded);
enum { FT_UP=256, FT_DOWN, FT_LEFT, FT_RIGHT, FT_ENTER, FT_ESCAPE, FT_BACKTAB,
    FT_PAGEUP, FT_PAGEDOWN, FT_HOME, FT_END, FT_DELETE };
/* true requests a fetch (tab/window/mode/selector change), not a timed close. */
bool ft_event(ft_app *a, int key);
typedef enum ft_action { ACT_QUIT,ACT_NEXT,ACT_PREV,ACT_GRAPH,ACT_HELP,ACT_ESCAPE,
    ACT_UP,ACT_DOWN,ACT_ENTER,ACT_DELETE,ACT_RIGHT,ACT_LEFT,ACT_EXPAND,ACT_COLLAPSE,
    ACT_FOCUS,ACT_PAGEUP,ACT_PAGEDOWN,ACT_HOME,ACT_END,ACT_SORT,ACT_REVERSE,
    ACT_MODE,ACT_SELECTOR,ACT_PREV_SELECTOR } ft_action;
typedef struct ft_binding { int key; const char *label, *hint; ft_action action; unsigned tabs; } ft_binding;
typedef struct ft_hint { const char *key,*label; } ft_hint;
size_t ft_hints(const ft_app *a,ft_hint out[16]);
extern const ft_binding ft_bindings[];
extern const size_t ft_binding_count;
bool ft_binding_active(const ft_app *a, const ft_binding *b);
#endif
