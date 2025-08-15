#pragma once
#ifndef GOA_API_IS_SUBJECT_TO_CHANGE
#define GOA_API_IS_SUBJECT_TO_CHANGE 1
#endif
#include <goa/goa.h>
#if defined(__has_include)
#  if __has_include(<goa/goa-backend.h>)
#    include <goa/goa-backend.h>
#  elif __has_include(<goa/goabackend.h>)
#    include <goa/goabackend.h>
#  else
     /* Fallback: some distros expose backend symbols via goa/goa.h when using goa-backend-1.0 pkg */
#  endif
#else
#  include <goa/goa-backend.h>
#endif
#include <glib-object.h>

G_BEGIN_DECLS

#define TYPE_PROVIDER_GNOSTR            (provider_gnostr_get_type())
G_DECLARE_FINAL_TYPE(ProviderGnostr, provider_gnostr, PROVIDER, GNOSTR, GoaProvider)

G_END_DECLS
