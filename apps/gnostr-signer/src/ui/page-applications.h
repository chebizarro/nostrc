#pragma once
#include <adwaita.h>
G_BEGIN_DECLS
#define TYPE_PAGE_APPLICATIONS (page_applications_get_type())
G_DECLARE_FINAL_TYPE(PageApplications, page_applications, PAGE, APPLICATIONS, AdwPreferencesPage)
PageApplications *page_applications_new(void);
G_END_DECLS
