/* gnostr's GnOgPreviewProvider: link previews through GnostrMediaService
 * (its soup session, per-account memory/disk caches and gdk-pixbuf scaling)
 * for nostr-gtk's GnOgPreviewCard (nostrc-8xfib.3). */
#ifndef GNOSTR_OG_PROVIDER_H
#define GNOSTR_OG_PROVIDER_H

#include <nostr-gtk-1.0/gn-og-preview.h>
#include "gnostr-media-service.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_OG_PROVIDER (gnostr_og_provider_get_type())
G_DECLARE_FINAL_TYPE(GnostrOgProvider, gnostr_og_provider, GNOSTR, OG_PROVIDER, GObject)

/* @service: (nullable): NULL uses gnostr_media_service_get_default() per request. */
GnostrOgProvider *gnostr_og_provider_new(GnostrMediaService *service);
/* (transfer none): process-wide provider over the default media service. */
GnOgPreviewProvider *gnostr_og_provider_get_default(void);

G_END_DECLS

#endif
