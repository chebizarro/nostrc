#pragma once
#include <adwaita.h>
#include "../home-model.h"
G_BEGIN_DECLS
#define GN_TYPE_PAGE_HISTORY (gn_page_history_get_type())
G_DECLARE_FINAL_TYPE(GnPageHistory, gn_page_history, GN, PAGE_HISTORY, AdwPreferencesPage)
GnPageHistory *gn_page_history_new(void);
void gn_page_history_refresh(GnPageHistory *self);
void gn_page_history_clear_filters(GnPageHistory *self);
void gn_page_history_set_model(GnPageHistory *, GrottoHomeModel *);
const gchar *gn_page_history_operation_label(const gchar *operation);
const gchar *gn_page_history_outcome_label(const gchar *outcome);
const gchar *gn_page_history_remote_state_label(const gchar *state);
G_END_DECLS
