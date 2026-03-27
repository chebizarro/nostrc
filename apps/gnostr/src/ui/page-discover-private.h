#ifndef PAGE_DISCOVER_PRIVATE_H
#define PAGE_DISCOVER_PRIVATE_H

#include "page-discover.h"

#include <stdint.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _GnostrArticlesView GnostrArticlesView;
typedef struct _GnProfileListModel GnProfileListModel;
typedef struct _GnostrDebounce GnostrDebounce;

struct _GnostrPageDiscover {
    GtkWidget parent_instance;

    /* Template widgets - Profile search */
    GtkSearchEntry *search_entry;
    GtkToggleButton *btn_local;
    GtkToggleButton *btn_network;
    GtkDropDown *sort_dropdown;
    GtkLabel *lbl_profile_count;
    GtkStack *content_stack;
    GtkListView *results_list;
    GtkBox *empty_state;
    GtkSpinner *loading_spinner;
    GtkScrolledWindow *scroller;
    GtkButton *btn_communities;

    /* Template widgets - Mode toggle */
    GtkToggleButton *btn_mode_people;
    GtkToggleButton *btn_mode_live;
    GtkToggleButton *btn_mode_articles;
    GtkBox *filter_row;  /* Profile filter row (Local/Network) */

    /* Template widgets - Articles view (NIP-54 Wiki + NIP-23 Long-form) */
    GnostrArticlesView *articles_view;

    /* Template widgets - Trending Hashtags */
    GtkBox *trending_section;
    GtkFlowBox *trending_flow_box;

    /* Template widgets - Live Activities */
    GtkFlowBox *live_flow_box;
    GtkFlowBox *scheduled_flow_box;
    GtkBox *live_now_section;
    GtkBox *scheduled_section;
    GtkSpinner *live_loading_spinner;
    GtkButton *btn_refresh_live;
    GtkButton *btn_refresh_live_empty;

    /* Local profile browser (mode: local) */
    GnProfileListModel *profile_model;
    GtkSingleSelection *local_selection;
    GtkListItemFactory *local_factory;

    /* Network search results (mode: network) */
    GListStore *network_results_model;
    GtkSingleSelection *network_selection;
    GtkListItemFactory *network_factory;

    /* Live activities data */
    GPtrArray *live_activities;      /* GnostrLiveActivity* array */
    GPtrArray *scheduled_activities; /* GnostrLiveActivity* array */
    GCancellable *live_cancellable;
    gboolean live_loaded;
    uint64_t live_sub_id;            /* nostrdb subscription ID for live events */

    /* State */
    GnostrDebounce *search_debounce;
    gboolean profiles_loaded;
    gboolean trending_loaded;
    gboolean is_local_mode;
    gboolean is_live_mode;     /* TRUE for Live mode, FALSE for People mode */
    gboolean is_articles_mode; /* TRUE for Articles mode */
    gboolean articles_loaded;
    GCancellable *search_cancellable;
    GCancellable *trending_cancellable;
};


void gnostr_page_discover_live_present(GnostrPageDiscover *self);
void gnostr_page_discover_live_reload(GnostrPageDiscover *self);
void gnostr_page_discover_live_load_internal(GnostrPageDiscover *self);
void gnostr_page_discover_live_dispose(GnostrPageDiscover *self);

void gnostr_page_discover_people_init(GnostrPageDiscover *self);
void gnostr_page_discover_people_dispose(GnostrPageDiscover *self);
void gnostr_page_discover_people_present(GnostrPageDiscover *self);
void gnostr_page_discover_people_load_profiles_internal(GnostrPageDiscover *self);
void gnostr_page_discover_people_set_loading_internal(GnostrPageDiscover *self, gboolean is_loading);
void gnostr_page_discover_people_clear_results_internal(GnostrPageDiscover *self);
void gnostr_page_discover_people_refresh_internal(GnostrPageDiscover *self);

void gnostr_page_discover_emit_open_profile_internal(GnostrPageDiscover *self, const char *pubkey_hex);
void gnostr_page_discover_emit_watch_live_internal(GnostrPageDiscover *self, const char *event_id_hex);
void gnostr_page_discover_emit_follow_requested_internal(GnostrPageDiscover *self, const char *pubkey_hex);
void gnostr_page_discover_emit_unfollow_requested_internal(GnostrPageDiscover *self, const char *pubkey_hex);
void gnostr_page_discover_emit_mute_requested_internal(GnostrPageDiscover *self, const char *pubkey_hex);
void gnostr_page_discover_emit_copy_npub_requested_internal(GnostrPageDiscover *self, const char *pubkey_hex);

G_END_DECLS

#endif /* PAGE_DISCOVER_PRIVATE_H */
