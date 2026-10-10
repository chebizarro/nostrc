/* gnostr's link-preview policy for nostr-gtk's GnOgPreviewCard
 * (nostrc-8xfib.3): media-card skin, media-service provider, auto-load
 * from the "load-remote-media" setting, YouTube badge/embed and opening
 * the link on click. */
#ifndef GNOSTR_OG_ACTIVATE_H
#define GNOSTR_OG_ACTIVATE_H

#include <nostr-gtk-1.0/gn-og-preview-card.h>

G_BEGIN_DECLS

/* A NostrGtkLinkPreviewSetupFunc. */
void gnostr_og_preview_setup(GnOgPreviewCard *card, const char *url, gpointer user_data);
/* Installs gnostr_og_preview_setup() for every note card row. */
void gnostr_og_preview_install(void);

G_END_DECLS

#endif
