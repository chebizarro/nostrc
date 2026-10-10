#include "gnostr-og-activate.h"
#include "gnostr-youtube-embed.h"
#include "../services/gnostr-og-provider.h"
#include "../util/utils.h"
#include "../util/youtube_url.h"
#include <nostr-gtk-1.0/nostr-note-card-row.h>

#define YOUTUBE_EMBED_KEY "gnostr-youtube-embed"

static void
on_activate(GnOgPreviewCard *card, const char *url, gpointer user_data)
{
  (void)user_data;
  if (!url || !*url)
    return;
#ifdef HAVE_WEBKITGTK
  if (gnostr_youtube_url_is_youtube(url)) {
    g_autofree char *video_id = gnostr_youtube_url_extract_video_id(url);
    if (video_id && !g_object_get_data(G_OBJECT(card), YOUTUBE_EMBED_KEY)) {
      /* The embed replaces the card's content for this binding; the row
       * builds a new card on its next bind. */
      GtkWidget *embed = gnostr_youtube_embed_new(video_id);
      for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(card)); child;
           child = gtk_widget_get_next_sibling(child))
        gtk_widget_set_visible(child, FALSE);
      gtk_box_append(GTK_BOX(card), embed);
      g_object_set_data(G_OBJECT(card), YOUTUBE_EMBED_KEY, embed);
      return;
    }
    if (video_id)
      return;
  }
#endif
  GtkWidget *root = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(card)));
  g_autoptr(GtkUriLauncher) launcher = gtk_uri_launcher_new(url);
  gtk_uri_launcher_launch(launcher, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL,
                          NULL, NULL, NULL);
}

void
gnostr_og_preview_setup(GnOgPreviewCard *card, const char *url, gpointer user_data)
{
  (void)user_data;
  g_return_if_fail(GN_IS_OG_PREVIEW_CARD(card));
  gn_og_preview_card_set_layout(card, GN_OG_PREVIEW_LAYOUT_COMPACT);
  gn_og_preview_card_set_provider(card, gnostr_og_provider_get_default());
  /* gnostr's existing default: with "load-remote-media" on, previews and
   * their artwork load on bind; off, the card waits for "Load preview". */
  gn_og_preview_card_set_auto_load(card, gnostr_is_remote_media_allowed());
  gn_og_preview_card_set_auto_load_image(card, TRUE);
  if (url && gnostr_youtube_url_is_youtube(url))
    gn_og_preview_card_set_media_badge(card, "media-playback-start-symbolic");
  g_signal_connect(card, "activate", G_CALLBACK(on_activate), NULL);
}

void
gnostr_og_preview_install(void)
{
  nostr_gtk_note_card_row_set_link_preview_setup(gnostr_og_preview_setup, NULL);
}
