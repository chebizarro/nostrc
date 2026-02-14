#ifndef APPS_GNOSTR_SIGNER_UI_PAGE_PERMISSIONS_H
#define APPS_GNOSTR_SIGNER_UI_PAGE_PERMISSIONS_H
#include <adwaita.h>
G_BEGIN_DECLS
#define TYPE_PAGE_PERMISSIONS (page_permissions_get_type())
G_DECLARE_FINAL_TYPE(PagePermissions, page_permissions, PAGE, PERMISSIONS, AdwPreferencesPage)
PagePermissions *page_permissions_new(void);
G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_PAGE_PERMISSIONS_H */
