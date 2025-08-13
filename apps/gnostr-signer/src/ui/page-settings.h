#pragma once
#include <adwaita.h>
G_BEGIN_DECLS
#define TYPE_PAGE_SETTINGS (page_settings_get_type())
G_DECLARE_FINAL_TYPE(PageSettings, page_settings, PAGE, SETTINGS, AdwPreferencesPage)
PageSettings *page_settings_new(void);
G_END_DECLS
