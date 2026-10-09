#ifndef GH_AGENTS_PAGE_H
#define GH_AGENTS_PAGE_H

#include <adwaita.h>

G_BEGIN_DECLS

typedef void (*GhAgentsStartFunc)(const gchar *agent_npub_or_uri,
                                  GtkWidget *page, gpointer user_data);

AdwPreferencesPage *gh_agents_page_new(void);
void gh_agents_page_populate(AdwPreferencesPage *page);
void gh_agents_page_set_account(AdwPreferencesPage *page, const gchar *npub);
void gh_agents_page_set_start_busy(AdwPreferencesPage *page, gboolean busy);
void gh_agents_page_set_start_func(AdwPreferencesPage *page,
                                   GhAgentsStartFunc start, gpointer user_data);
/* The same resource-backed prompt shown and copied by the page. Transfer full. */
gchar *gh_agents_page_dup_prompt(const gchar *connector, const gchar *npub);

G_END_DECLS
#endif
