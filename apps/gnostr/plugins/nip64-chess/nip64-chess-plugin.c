/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip64-chess-plugin.c - NIP-64 Chess Plugin
 *
 * Implements NIP-64 (Chess Games) for playing and publishing chess games.
 * Provides interactive chess board, AI opponent, and game publishing.
 *
 * Event kind handled:
 * - 64: Chess game in PGN format
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "nip64-chess-plugin.h"
#include <gnostr-plugin-api.h>
#include <libpeas.h>

/* Chess UI components */
#include "../../src/ui/gnostr-chess-game-view.h"
#include "../../src/ui/gnostr-chess-new-game-dialog.h"
#include "../../src/ui/gnostr-chess-publish-dialog.h"
#include "../../src/util/nip64_chess.h"

/* NIP-64 Event Kind */
#define NIP64_KIND_CHESS 64

/* Signal IDs */
enum {
  SIGNAL_GAMES_UPDATED,
  N_SIGNALS
};

static guint plugin_signals[N_SIGNALS];

struct _Nip64ChessPlugin
{
  GObject parent_instance;

  GnostrPluginContext *context;
  gboolean active;

  /* Cached games */
  GHashTable *games;  /* event_id -> GnostrChessGame* */

  /* Event subscription */
  guint64 games_subscription;
};

/* Implement GnostrPlugin interface */
static void gnostr_plugin_iface_init(GnostrPluginInterface *iface);

/* Implement GnostrEventHandler interface */
static void gnostr_event_handler_iface_init(GnostrEventHandlerInterface *iface);

/* Implement GnostrUIExtension interface */
static void gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface);

G_DEFINE_TYPE_WITH_CODE(Nip64ChessPlugin, nip64_chess_plugin, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, gnostr_plugin_iface_init)
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_EVENT_HANDLER, gnostr_event_handler_iface_init)
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_UI_EXTENSION, gnostr_ui_extension_iface_init))

/* ============================================================================
 * GObject lifecycle
 * ============================================================================ */

static void
nip64_chess_plugin_finalize(GObject *object)
{
  Nip64ChessPlugin *self = NIP64_CHESS_PLUGIN(object);

  g_clear_pointer(&self->games, g_hash_table_destroy);

  G_OBJECT_CLASS(nip64_chess_plugin_parent_class)->finalize(object);
}

static void
nip64_chess_plugin_init(Nip64ChessPlugin *self)
{
  self->active = FALSE;
  self->games = g_hash_table_new_full(g_str_hash, g_str_equal,
                                      g_free,
                                      (GDestroyNotify)gnostr_chess_game_free);
}

static void
nip64_chess_plugin_class_init(Nip64ChessPluginClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = nip64_chess_plugin_finalize;

  /**
   * Nip64ChessPlugin::games-updated:
   * @plugin: The plugin
   * @count: Number of games in cache
   *
   * Emitted when new chess games are received from relays.
   */
  plugin_signals[SIGNAL_GAMES_UPDATED] = g_signal_new(
      "games-updated",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL, NULL,
      NULL,
      G_TYPE_NONE, 1,
      G_TYPE_UINT);
}

/* ============================================================================
 * Event Subscription Callback
 * ============================================================================ */

static void
on_chess_game_received(GnostrPluginEvent *event, gpointer user_data)
{
  Nip64ChessPlugin *self = NIP64_CHESS_PLUGIN(user_data);

  if (!self->active || !event)
    return;

  int kind = gnostr_plugin_event_get_kind(event);
  if (kind != NIP64_KIND_CHESS)
    return;

  /* Parse the chess game from event */
  char *json = gnostr_plugin_event_to_json(event);
  if (!json)
    return;

  GnostrChessGame *game = gnostr_chess_parse_from_json(json);
  g_free(json);

  if (game && game->event_id) {
    /* Check if we already have this game */
    if (!g_hash_table_contains(self->games, game->event_id)) {
      g_hash_table_replace(self->games, g_strdup(game->event_id), game);
      g_debug("[NIP-64] Received chess game: %s vs %s (id: %.16s...)",
              game->white_player ? game->white_player : "?",
              game->black_player ? game->black_player : "?",
              game->event_id);

      /* Emit games-updated signal */
      guint count = g_hash_table_size(self->games);
      g_signal_emit(self, plugin_signals[SIGNAL_GAMES_UPDATED], 0, count);
    } else {
      gnostr_chess_game_free(game);
    }
  } else {
    gnostr_chess_game_free(game);
  }
}

/* ============================================================================
 * GnostrPlugin interface implementation
 * ============================================================================ */

static void
nip64_chess_plugin_activate(GnostrPlugin        *plugin,
                            GnostrPluginContext *context)
{
  Nip64ChessPlugin *self = NIP64_CHESS_PLUGIN(plugin);

  g_debug("[NIP-64] Chess plugin activated");

  self->context = context;
  self->active = TRUE;

  /* Subscribe to NIP-64 chess games from relays */
  gchar *filter = g_strdup_printf("{\"kinds\":[%d],\"limit\":50}", NIP64_KIND_CHESS);
  self->games_subscription = gnostr_plugin_context_subscribe_events(
      context, filter, G_CALLBACK(on_chess_game_received), self, NULL);
  g_free(filter);

  if (self->games_subscription > 0) {
    g_debug("[NIP-64] Subscribed to chess games (subscription_id: %lu)",
            (unsigned long)self->games_subscription);
  }
}

static void
nip64_chess_plugin_deactivate(GnostrPlugin        *plugin,
                              GnostrPluginContext *context)
{
  Nip64ChessPlugin *self = NIP64_CHESS_PLUGIN(plugin);

  g_debug("[NIP-64] Chess plugin deactivated");

  /* Unsubscribe from chess game events */
  if (self->games_subscription > 0 && context) {
    gnostr_plugin_context_unsubscribe_events(context, self->games_subscription);
    self->games_subscription = 0;
  }

  self->active = FALSE;
  self->context = NULL;
}

static const char *
nip64_chess_plugin_get_name(GnostrPlugin *plugin)
{
  (void)plugin;
  return "NIP-64 Chess";
}

static const char *
nip64_chess_plugin_get_description(GnostrPlugin *plugin)
{
  (void)plugin;
  return "Play chess games and publish them to Nostr";
}

static const int *
nip64_chess_plugin_get_supported_kinds(GnostrPlugin *plugin,
                                       gsize        *n_kinds)
{
  (void)plugin;
  static const int kinds[] = { NIP64_KIND_CHESS };
  *n_kinds = G_N_ELEMENTS(kinds);
  return kinds;
}

static void
gnostr_plugin_iface_init(GnostrPluginInterface *iface)
{
  iface->activate = nip64_chess_plugin_activate;
  iface->deactivate = nip64_chess_plugin_deactivate;
  iface->get_name = nip64_chess_plugin_get_name;
  iface->get_description = nip64_chess_plugin_get_description;
  iface->get_supported_kinds = nip64_chess_plugin_get_supported_kinds;
}

/* ============================================================================
 * GnostrEventHandler interface implementation
 * ============================================================================ */

static gboolean
nip64_chess_plugin_handle_event(GnostrEventHandler  *handler,
                                GnostrPluginContext *context,
                                GnostrPluginEvent   *event)
{
  Nip64ChessPlugin *self = NIP64_CHESS_PLUGIN(handler);
  (void)context;

  if (!self->active)
    return FALSE;

  int kind = gnostr_plugin_event_get_kind(event);

  if (kind != NIP64_KIND_CHESS)
    return FALSE;

  /* Parse and cache the chess game */
  char *json = gnostr_plugin_event_to_json(event);
  GnostrChessGame *game = gnostr_chess_parse_from_json(json);
  g_free(json);

  if (game && game->event_id)
    {
      g_hash_table_replace(self->games,
                           g_strdup(game->event_id), game);
      g_debug("[NIP-64] Cached chess game: %s vs %s",
              game->white_player ? game->white_player : "?",
              game->black_player ? game->black_player : "?");
    }
  else
    {
      gnostr_chess_game_free(game);
    }

  return TRUE;
}

static gboolean
nip64_chess_plugin_can_handle_kind(GnostrEventHandler *handler, int kind)
{
  (void)handler;
  return kind == NIP64_KIND_CHESS;
}

static void
gnostr_event_handler_iface_init(GnostrEventHandlerInterface *iface)
{
  iface->handle_event = nip64_chess_plugin_handle_event;
  iface->can_handle_kind = nip64_chess_plugin_can_handle_kind;
}

/* ============================================================================
 * GnostrUIExtension interface implementation
 * ============================================================================ */

static GList *
nip64_chess_plugin_get_sidebar_items(GnostrUIExtension   *extension,
                                     GnostrPluginContext *context)
{
  (void)extension;
  (void)context;

  GnostrSidebarItem *item = gnostr_sidebar_item_new(
    "nip64-chess",       /* id */
    "Chess",             /* label */
    "chess-symbolic"     /* icon - may need to use a different icon */
  );

  /* Chess doesn't require auth to view games, but does to play/publish */
  gnostr_sidebar_item_set_requires_auth(item, FALSE);
  gnostr_sidebar_item_set_position(item, 50); /* After repos */

  return g_list_append(NULL, item);
}

static GtkWidget *
nip64_chess_plugin_create_panel_widget(GnostrUIExtension   *extension,
                                       GnostrPluginContext *context,
                                       const char          *panel_id)
{
  Nip64ChessPlugin *self = NIP64_CHESS_PLUGIN(extension);
  (void)context;

  if (g_strcmp0(panel_id, "nip64-chess") != 0)
    return NULL;

  g_debug("[NIP-64] Creating chess panel widget");

  /* Create the game view with New Game button */
  GnostrChessGameView *game_view = gnostr_chess_game_view_new();

  /* Connect the plugin to the game view for games browsing */
  gnostr_chess_game_view_set_plugin(game_view, self);
  gnostr_chess_game_view_set_plugin_callbacks(game_view,
      (GnostrChessGetGamesFunc)nip64_chess_plugin_get_games,
      (GnostrChessRequestGamesFunc)nip64_chess_plugin_request_games);

  return GTK_WIDGET(game_view);
}

static GtkWidget *
nip64_chess_plugin_create_settings_page(GnostrUIExtension   *extension,
                                        GnostrPluginContext *context)
{
  (void)extension;
  (void)context;

  /* Simple settings page */
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(box, 24);
  gtk_widget_set_margin_end(box, 24);
  gtk_widget_set_margin_top(box, 24);
  gtk_widget_set_margin_bottom(box, 24);

  GtkWidget *title = gtk_label_new("Chess Settings");
  gtk_widget_add_css_class(title, "title-2");
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box), title);

  GtkWidget *desc = gtk_label_new(
    "NIP-64 Chess allows you to play chess games and publish them to Nostr.\n\n"
    "Games are stored as PGN (Portable Game Notation) in kind 64 events."
  );
  gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
  gtk_label_set_xalign(GTK_LABEL(desc), 0);
  gtk_box_append(GTK_BOX(box), desc);

  return box;
}

static GList *
nip64_chess_plugin_create_menu_items(GnostrUIExtension     *extension,
                                     GnostrPluginContext   *context,
                                     GnostrUIExtensionPoint point,
                                     gpointer               extra)
{
  (void)extension;
  (void)context;
  (void)extra;

  /* No menu items for now */
  return NULL;
}

static GtkWidget *
nip64_chess_plugin_create_note_decoration(GnostrUIExtension   *extension,
                                          GnostrPluginContext *context,
                                          GnostrPluginEvent   *event)
{
  (void)extension;
  (void)context;

  int kind = gnostr_plugin_event_get_kind(event);

  if (kind != NIP64_KIND_CHESS)
    return NULL;

  /* Return existing chess card widget for viewing games in timeline */
  /* This would use gnostr_chess_card_new() if we wanted inline viewing */
  return NULL;
}

static void
gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface)
{
  iface->get_sidebar_items = nip64_chess_plugin_get_sidebar_items;
  iface->create_panel_widget = nip64_chess_plugin_create_panel_widget;
  iface->create_settings_page = nip64_chess_plugin_create_settings_page;
  iface->create_menu_items = nip64_chess_plugin_create_menu_items;
  iface->create_note_decoration = nip64_chess_plugin_create_note_decoration;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

GHashTable *
nip64_chess_plugin_get_games(Nip64ChessPlugin *self)
{
  g_return_val_if_fail(NIP64_IS_CHESS_PLUGIN(self), NULL);
  return self->games;
}

void
nip64_chess_plugin_request_games(Nip64ChessPlugin *self)
{
  g_return_if_fail(NIP64_IS_CHESS_PLUGIN(self));

  if (!self->context || !self->active)
    return;

  /* Request fresh chess games from relays */
  static const int kinds[] = { NIP64_KIND_CHESS };
  gnostr_plugin_context_request_relay_events_async(
      self->context,
      kinds, 1,
      50,  /* limit */
      NULL,
      NULL, NULL);

  g_debug("[NIP-64] Requested fresh chess games from relays");
}

/* ============================================================================
 * Plugin entry point for libpeas
 * ============================================================================ */

G_MODULE_EXPORT void
peas_register_types(PeasObjectModule *module)
{
  peas_object_module_register_extension_type(module,
                                             GNOSTR_TYPE_PLUGIN,
                                             NIP64_TYPE_CHESS_PLUGIN);
  peas_object_module_register_extension_type(module,
                                             GNOSTR_TYPE_EVENT_HANDLER,
                                             NIP64_TYPE_CHESS_PLUGIN);
  peas_object_module_register_extension_type(module,
                                             GNOSTR_TYPE_UI_EXTENSION,
                                             NIP64_TYPE_CHESS_PLUGIN);
}
