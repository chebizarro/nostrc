#define G_LOG_DOMAIN "gnostr-main-window-profile-pane"

#include "gnostr-main-window-private.h"

#include "gnostr-session-view.h"
#include "../ipc/gnostr-signer-service.h"
#include "../model/gn-nostr-event-model.h"
#include "../util/nip02_contacts.h"

#include <nostr-gobject-1.0/gnostr-mute-list.h>
#include <nostr-gtk-1.0/gnostr-profile-pane.h>

#include <string.h>

typedef struct {
  GnostrMainWindow *self;
  NostrGtkProfilePane *pane;
  char *pubkey_hex;
  gboolean was_following;
} FollowContext;

static void
follow_context_free(FollowContext *ctx)
{
  if (!ctx)
    return;
  g_free(ctx->pubkey_hex);
  g_free(ctx);
}

static void
on_follow_save_complete(GnostrContactList *cl,
                        gboolean success,
                        const char *error_msg,
                        gpointer user_data)
{
  FollowContext *ctx = (FollowContext *)user_data;
  if (!ctx)
    return;

  if (success) {
    gnostr_main_window_show_toast(GTK_WIDGET(ctx->self),
                                  ctx->was_following ? "Unfollowed" : "Followed");
  } else {
    if (ctx->was_following)
      gnostr_contact_list_add(cl, ctx->pubkey_hex, NULL);
    else
      gnostr_contact_list_remove(cl, ctx->pubkey_hex);

    if (NOSTR_GTK_IS_PROFILE_PANE(ctx->pane))
      nostr_gtk_profile_pane_set_following(ctx->pane, ctx->was_following);

    gnostr_main_window_show_toast(GTK_WIDGET(ctx->self),
                                  error_msg ? error_msg : "Follow action failed");
  }

  follow_context_free(ctx);
}

void
gnostr_main_window_on_profile_pane_mute_user_requested_internal(NostrGtkProfilePane *pane,
                                                                const char *pubkey_hex,
                                                                gpointer user_data)
{
  (void)pane;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  if (!pubkey_hex || strlen(pubkey_hex) != 64) {
    g_warning("[MUTE] Invalid pubkey hex from profile pane");
    return;
  }

  g_debug("[MUTE] Mute user from profile pane for pubkey=%.16s...", pubkey_hex);

  GNostrMuteList *mute_list = gnostr_mute_list_get_default();
  gnostr_mute_list_add_pubkey(mute_list, pubkey_hex, FALSE);

  if (self->event_model)
    gn_nostr_event_model_refresh_async(GN_NOSTR_EVENT_MODEL(self->event_model));

  gnostr_main_window_show_toast(GTK_WIDGET(self), "User muted");
}

void
gnostr_main_window_on_profile_pane_follow_requested_internal(NostrGtkProfilePane *pane,
                                                             const char *pubkey_hex,
                                                             gpointer user_data)
{
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;
  if (!pubkey_hex || strlen(pubkey_hex) != 64)
    return;

  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    gnostr_main_window_show_toast(GTK_WIDGET(self), "Please log in to follow users");
    return;
  }

  GnostrContactList *cl = gnostr_contact_list_get_default();
  if (!gnostr_contact_list_get_user_pubkey(cl)) {
    const char *my_pk = gnostr_signer_service_get_pubkey(signer);
    if (my_pk)
      gnostr_contact_list_load_from_ndb(cl, my_pk);
  }

  gboolean was_following = gnostr_contact_list_is_following(cl, pubkey_hex);
  if (was_following)
    gnostr_contact_list_remove(cl, pubkey_hex);
  else
    gnostr_contact_list_add(cl, pubkey_hex, NULL);

  nostr_gtk_profile_pane_set_following(pane, !was_following);

  FollowContext *ctx = g_new0(FollowContext, 1);
  ctx->self = self;
  ctx->pane = pane;
  ctx->pubkey_hex = g_strdup(pubkey_hex);
  ctx->was_following = was_following;

  gnostr_contact_list_save_async(cl, on_follow_save_complete, ctx);
}

void
gnostr_main_window_on_profile_pane_message_requested_internal(NostrGtkProfilePane *pane,
                                                              const char *pubkey_hex,
                                                              gpointer user_data)
{
  (void)pane;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;
  if (!pubkey_hex || strlen(pubkey_hex) != 64)
    return;

  if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
    gnostr_session_view_show_page(self->session_view, "messages");
    gnostr_main_window_navigate_to_dm_conversation_internal(self, pubkey_hex);
  }
}
