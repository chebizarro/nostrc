#pragma once
#include <adwaita.h>
#include "../home-model.h"
#define GROTTO_TYPE_PAGE_CONNECT (grotto_page_connect_get_type())
G_DECLARE_FINAL_TYPE(GrottoPageConnect, grotto_page_connect, GROTTO, PAGE_CONNECT, AdwPreferencesPage)
void grotto_page_connect_set_model(GrottoPageConnect *, GrottoHomeModel *);
