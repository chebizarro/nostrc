#pragma once
#include <goa/goa.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define TYPE_PROVIDER_GNOSTR            (provider_gnostr_get_type())
G_DECLARE_FINAL_TYPE(ProviderGnostr, provider_gnostr, PROVIDER, GNOSTR, GoaProvider)

G_END_DECLS
