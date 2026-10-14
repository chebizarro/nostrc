#pragma once
#include <adwaita.h>
#include "../home-model.h"
#define GROTTO_TYPE_PAGE_HOME (grotto_page_home_get_type())
G_DECLARE_FINAL_TYPE(GrottoPageHome, grotto_page_home, GROTTO, PAGE_HOME, AdwPreferencesPage)
void grotto_page_home_set_model(GrottoPageHome *, GrottoHomeModel *);
void grotto_page_home_set_pending_group(GrottoPageHome *, GtkWidget *);
