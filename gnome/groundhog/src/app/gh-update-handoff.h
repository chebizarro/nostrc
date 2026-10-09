#pragma once
#include <adwaita.h>

G_BEGIN_DECLS

/* Build metadata, never inferred from a URL or a network request. */
const gchar *gh_update_handoff_package_channel(void);
const gchar *gh_update_handoff_instructions(const gchar *channel);
void gh_update_handoff_present(GtkWindow *parent);

G_END_DECLS
