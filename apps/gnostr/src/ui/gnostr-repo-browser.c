/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-repo-browser.c - NIP-34 Repository Browser View
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#define G_LOG_DOMAIN "gnostr-repo-browser"

#include "gnostr-repo-browser.h"
#include <nostr-gtk-1.0/nostr-note-card-row.h>
#include <nostr-gobject-1.0/nostr_profile_provider.h>
#include <adwaita.h>

/* Repository data stored in list model */
typedef struct {
  gchar *id;
  gchar *name;
  gchar *description;
  gchar *clone_url;
  gchar *web_url;
  gchar *maintainer_pubkey;
  gint64 updated_at;
} RepoData;

static void
repo_data_free(RepoData *data)
{
  if (!data) return;
  g_free(data->id);
  g_free(data->name);
  g_free(data->description);
  g_free(data->clone_url);
  g_free(data->web_url);
  g_free(data->maintainer_pubkey);
  g_free(data);
}

/* nostrc-35i: Patch proposal data (kind 1617) */
typedef struct {
  gchar *id;
  gchar *pubkey;
  gchar *repo_ref;
  gchar *subject;
  gchar *content;
  gboolean is_root;
  gint64 created_at;
} PatchData;

static void
patch_data_free(PatchData *data)
{
  if (!data) return;
  g_free(data->id);
  g_free(data->pubkey);
  g_free(data->repo_ref);
  g_free(data->subject);
  g_free(data->content);
  g_free(data);
}

/* nostrc-35i: Issue data (kind 1621) */
typedef struct {
  gchar *id;
  gchar *pubkey;
  gchar *repo_ref;
  gchar *subject;
  gchar *content;
  gchar *status;
  gint64 created_at;
} IssueBrowserData;

static void
issue_browser_data_free(IssueBrowserData *data)
{
  if (!data) return;
  g_free(data->id);
  g_free(data->pubkey);
  g_free(data->repo_ref);
  g_free(data->subject);
  g_free(data->content);
  g_free(data->status);
  g_free(data);
}

struct _GnostrRepoBrowser
{
  GtkWidget parent_instance;

  /* Main layout */
  GtkWidget *main_box;
  GtkWidget *header_box;
  GtkWidget *search_entry;
  GtkWidget *refresh_button;
  GtkWidget *stack;

  /* Views */
  GtkWidget *loading_view;
  GtkWidget *empty_view;
  GtkWidget *list_view;
  GtkWidget *scrolled_window;
  GtkListBox *repo_list;

  /* nostrc-35i: Patches & Issues tabs */
  GtkWidget *tab_stack;
  GtkWidget *patch_scrolled;
  GtkListBox *patch_list;
  GtkWidget *issue_scrolled;
  GtkListBox *issue_list;

  /* Data */
  GHashTable *repositories;  /* id -> RepoData */
  GHashTable *patches;       /* id -> PatchData */
  GHashTable *issues_ht;     /* id -> IssueBrowserData */
  gchar *filter_text;
  gchar *selected_id;

  /* State */
  gboolean is_loading;
};

G_DEFINE_TYPE(GnostrRepoBrowser, gnostr_repo_browser, GTK_TYPE_WIDGET)

enum {
  SIGNAL_REPO_SELECTED,
  SIGNAL_CLONE_REQUESTED,
  SIGNAL_REFRESH_REQUESTED,
  SIGNAL_NEED_PROFILE,
  SIGNAL_OPEN_PROFILE,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void rebuild_list(GnostrRepoBrowser *self);
static void rebuild_patch_list(GnostrRepoBrowser *self);
static void rebuild_issue_list(GnostrRepoBrowser *self);
static gboolean repo_matches_filter(GnostrRepoBrowser *self, RepoData *data);

static void
on_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
  GnostrRepoBrowser *self = GNOSTR_REPO_BROWSER(user_data);
  g_free(self->filter_text);
  self->filter_text = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));
  rebuild_list(self);
  rebuild_patch_list(self);
  rebuild_issue_list(self);
}

static void
on_row_activated(GtkListBox *list_box G_GNUC_UNUSED,
                 GtkListBoxRow *row,
                 gpointer user_data)
{
  GnostrRepoBrowser *self = GNOSTR_REPO_BROWSER(user_data);
  const char *id = g_object_get_data(G_OBJECT(row), "repo-id");

  if (id)
    {
      g_free(self->selected_id);
      self->selected_id = g_strdup(id);
      g_signal_emit(self, signals[SIGNAL_REPO_SELECTED], 0, id);
    }
}

static void
on_clone_clicked(GtkButton *button, gpointer user_data)
{
  GnostrRepoBrowser *self = GNOSTR_REPO_BROWSER(user_data);
  const char *url = g_object_get_data(G_OBJECT(button), "clone-url");

  if (url)
    g_signal_emit(self, signals[SIGNAL_CLONE_REQUESTED], 0, url);
}

static void
on_refresh_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  GnostrRepoBrowser *self = GNOSTR_REPO_BROWSER(user_data);
  g_signal_emit(self, signals[SIGNAL_REFRESH_REQUESTED], 0);
}

/* Handler for open-profile signal from note card rows */
static void
on_note_card_open_profile(NostrGtkNoteCardRow *card G_GNUC_UNUSED,
                          const char        *pubkey_hex,
                          gpointer           user_data)
{
  GnostrRepoBrowser *self = GNOSTR_REPO_BROWSER(user_data);
  if (pubkey_hex && *pubkey_hex)
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, pubkey_hex);
}

static GtkWidget *
create_repo_row(GnostrRepoBrowser *self, RepoData *data)
{
  /* Create note card for consistent display with timeline */
  NostrGtkNoteCardRow *card = nostr_gtk_note_card_row_new();

  /* CRITICAL: Call prepare_for_bind before populating the card.
   * nostrc-NEW: This was missing, causing blank cards because the
   * disposed flag and binding_id weren't properly initialized. */
  nostr_gtk_note_card_row_prepare_for_bind(card);

  /* Fetch maintainer profile for author display */
  const char *display_name = NULL;
  const char *handle = NULL;
  const char *avatar_url = NULL;
  GnostrProfileMeta *profile = NULL;

  if (data->maintainer_pubkey)
    {
      g_debug("[repo-browser] Looking up profile for maintainer: %s", data->maintainer_pubkey);
      profile = gnostr_profile_provider_get(data->maintainer_pubkey);
      if (profile)
        {
          display_name = profile->display_name ? profile->display_name : profile->name;
          handle = profile->name;
          avatar_url = profile->picture;
          g_debug("[repo-browser] Found profile: name=%s, picture=%s",
                  display_name ? display_name : "(null)",
                  avatar_url ? avatar_url : "(null)");
        }
      else
        {
          g_debug("[repo-browser] No profile found for pubkey %s, requesting fetch", data->maintainer_pubkey);
          /* Request profile fetch from relays */
          g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, data->maintainer_pubkey);
        }
    }
  else
    {
      g_debug("[repo-browser] No maintainer_pubkey for repo %s", data->id);
    }

  /* Set author info (maintainer profile) */
  nostr_gtk_note_card_row_set_author(card, display_name, handle, avatar_url);
  nostr_gtk_note_card_row_set_ids(card, data->id, NULL, data->maintainer_pubkey);
  nostr_gtk_note_card_row_set_timestamp(card, data->updated_at, NULL);

  /* Build content: repo name (bold) + description + clone URL */
  GString *content = g_string_new(NULL);

  /* Repo name as title */
  const char *repo_name = data->name ? data->name : data->id;
  g_string_append_printf(content, "📦 %s\n", repo_name);

  /* Description */
  if (data->description && *data->description)
    g_string_append_printf(content, "\n%s", data->description);

  /* Clone URL */
  if (data->clone_url)
    g_string_append_printf(content, "\n\n🔗 %s", data->clone_url);

  nostr_gtk_note_card_row_set_content(card, content->str);
  g_string_free(content, TRUE);

  /* Free profile after we're done with it */
  if (profile)
    gnostr_profile_meta_free(profile);

  /* Connect open-profile signal to relay clicks on author avatar/name */
  g_signal_connect(card, "open-profile", G_CALLBACK(on_note_card_open_profile), self);

  /* Wrap in a container with action buttons */
  GtkWidget *container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(container), GTK_WIDGET(card));

  /* Action button row for clone/web */
  if (data->clone_url || data->web_url)
    {
      GtkWidget *action_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
      gtk_widget_set_margin_start(action_row, 60);  /* Align with content after avatar */
      gtk_widget_set_margin_bottom(action_row, 8);
      gtk_widget_set_halign(action_row, GTK_ALIGN_START);

      if (data->clone_url)
        {
          GtkWidget *clone_btn = gtk_button_new_from_icon_name("folder-download-symbolic");
          gtk_button_set_label(GTK_BUTTON(clone_btn), "Clone");
          gtk_widget_set_tooltip_text(clone_btn, "Clone repository");
          gtk_widget_add_css_class(clone_btn, "flat");
          g_object_set_data_full(G_OBJECT(clone_btn), "clone-url",
                                 g_strdup(data->clone_url), g_free);
          g_signal_connect(clone_btn, "clicked", G_CALLBACK(on_clone_clicked), self);
          gtk_box_append(GTK_BOX(action_row), clone_btn);
        }

      if (data->web_url)
        {
          GtkWidget *web_btn = gtk_button_new_from_icon_name("web-browser-symbolic");
          gtk_button_set_label(GTK_BUTTON(web_btn), "Open");
          gtk_widget_set_tooltip_text(web_btn, "Open in browser");
          gtk_widget_add_css_class(web_btn, "flat");
          gtk_box_append(GTK_BOX(action_row), web_btn);
        }

      gtk_box_append(GTK_BOX(container), action_row);
    }

  /* Store ID on the row widget for selection handling */
  GtkWidget *list_row = gtk_list_box_row_new();
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(list_row), container);
  g_object_set_data_full(G_OBJECT(list_row), "repo-id",
                         g_strdup(data->id), g_free);

  return list_row;
}

static gboolean
repo_matches_filter(GnostrRepoBrowser *self, RepoData *data)
{
  if (!self->filter_text || *self->filter_text == '\0')
    return TRUE;

  gchar *filter_lower = g_utf8_strdown(self->filter_text, -1);
  gboolean matches = FALSE;

  if (data->name)
    {
      gchar *name_lower = g_utf8_strdown(data->name, -1);
      if (g_strstr_len(name_lower, -1, filter_lower))
        matches = TRUE;
      g_free(name_lower);
    }

  if (!matches && data->description)
    {
      gchar *desc_lower = g_utf8_strdown(data->description, -1);
      if (g_strstr_len(desc_lower, -1, filter_lower))
        matches = TRUE;
      g_free(desc_lower);
    }

  if (!matches && data->id)
    {
      gchar *id_lower = g_utf8_strdown(data->id, -1);
      if (g_strstr_len(id_lower, -1, filter_lower))
        matches = TRUE;
      g_free(id_lower);
    }

  g_free(filter_lower);
  return matches;
}

static void
rebuild_list(GnostrRepoBrowser *self)
{
  /* Clear existing rows — call prepare_for_unbind on NoteCardRow children
   * before removal to prevent Pango layout corruption during disposal.
   * nostrc-pgo2: Repo browser manages NoteCardRow lifecycle manually (not
   * via GtkListItemFactory), so we must handle unbind ourselves. */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->repo_list))) != NULL) {
    /* NoteCardRow is inside: ListBoxRow → Box → NoteCardRow */
    GtkWidget *container = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(child));
    if (container && GTK_IS_BOX(container)) {
      GtkWidget *first = gtk_widget_get_first_child(container);
      if (first && NOSTR_GTK_IS_NOTE_CARD_ROW(first))
        nostr_gtk_note_card_row_prepare_for_unbind(NOSTR_GTK_NOTE_CARD_ROW(first));
    }
    gtk_list_box_remove(self->repo_list, child);
  }

  /* Add matching repositories */
  GHashTableIter iter;
  gpointer key, value;
  guint visible_count = 0;

  g_hash_table_iter_init(&iter, self->repositories);
  while (g_hash_table_iter_next(&iter, &key, &value))
    {
      RepoData *data = (RepoData *)value;
      if (repo_matches_filter(self, data))
        {
          GtkWidget *row = create_repo_row(self, data);
          gtk_list_box_append(self->repo_list, row);
          visible_count++;
        }
    }

  /* Update stack visibility */
  if (self->is_loading)
    gtk_stack_set_visible_child(GTK_STACK(self->stack), self->loading_view);
  else if (visible_count == 0 &&
           g_hash_table_size(self->patches) == 0 &&
           g_hash_table_size(self->issues_ht) == 0)
    gtk_stack_set_visible_child(GTK_STACK(self->stack), self->empty_view);
  else
    gtk_stack_set_visible_child(GTK_STACK(self->stack), self->list_view);
}

/* nostrc-35i: Rebuild patch list */
static void
rebuild_patch_list(GnostrRepoBrowser *self)
{
  /* Clear existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->patch_list))) != NULL)
    gtk_list_box_remove(self->patch_list, child);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->patches);
  while (g_hash_table_iter_next(&iter, &key, &value))
    {
      PatchData *pd = (PatchData *)value;

      /* Filter */
      if (self->filter_text && *self->filter_text)
        {
          g_autofree gchar *fl = g_utf8_strdown(self->filter_text, -1);
          gboolean match = FALSE;
          if (pd->subject) {
            g_autofree gchar *sl = g_utf8_strdown(pd->subject, -1);
            if (g_strstr_len(sl, -1, fl)) match = TRUE;
          }
          if (!match && pd->content) {
            g_autofree gchar *cl = g_utf8_strdown(pd->content, -1);
            if (g_strstr_len(cl, -1, fl)) match = TRUE;
          }
          if (!match) continue;
        }

      GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
      gtk_widget_set_margin_start(row_box, 12);
      gtk_widget_set_margin_end(row_box, 12);
      gtk_widget_set_margin_top(row_box, 8);
      gtk_widget_set_margin_bottom(row_box, 8);

      /* Title line with root badge */
      GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
      const char *title = pd->subject && *pd->subject ? pd->subject : "(untitled patch)";
      GtkWidget *title_lbl = gtk_label_new(title);
      gtk_widget_add_css_class(title_lbl, "heading");
      gtk_widget_set_halign(title_lbl, GTK_ALIGN_START);
      gtk_widget_set_hexpand(title_lbl, TRUE);
      gtk_label_set_ellipsize(GTK_LABEL(title_lbl), PANGO_ELLIPSIZE_END);
      gtk_box_append(GTK_BOX(title_box), title_lbl);

      if (pd->is_root) {
        GtkWidget *badge = gtk_label_new("root");
        gtk_widget_add_css_class(badge, "accent");
        gtk_widget_add_css_class(badge, "caption");
        gtk_box_append(GTK_BOX(title_box), badge);
      }
      gtk_box_append(GTK_BOX(row_box), title_box);

      /* Timestamp + repo ref */
      GString *meta = g_string_new(NULL);
      if (pd->created_at > 0) {
        g_autoptr(GDateTime) dt = g_date_time_new_from_unix_local(pd->created_at);
        g_autofree gchar *ts = g_date_time_format(dt, "%Y-%m-%d %H:%M");
        g_string_append(meta, ts);
      }
      if (pd->repo_ref && *pd->repo_ref) {
        if (meta->len > 0) g_string_append(meta, " · ");
        /* Show just the d-tag portion: 30617:<pk>:<d-tag> → last component */
        const char *last_colon = strrchr(pd->repo_ref, ':');
        g_string_append(meta, last_colon ? last_colon + 1 : pd->repo_ref);
      }
      GtkWidget *meta_lbl = gtk_label_new(meta->str);
      gtk_widget_add_css_class(meta_lbl, "dim-label");
      gtk_widget_set_halign(meta_lbl, GTK_ALIGN_START);
      gtk_box_append(GTK_BOX(row_box), meta_lbl);
      g_string_free(meta, TRUE);

      /* Content preview (first 120 chars, skip diff headers) */
      if (pd->content && *pd->content) {
        const char *preview = pd->content;
        /* Skip leading diff header lines for preview */
        if (g_str_has_prefix(preview, "From ") || g_str_has_prefix(preview, "diff --"))
          preview = "";
        if (*preview) {
          g_autofree gchar *snip = g_strndup(preview, 120);
          GtkWidget *preview_lbl = gtk_label_new(snip);
          gtk_widget_add_css_class(preview_lbl, "dim-label");
          gtk_label_set_ellipsize(GTK_LABEL(preview_lbl), PANGO_ELLIPSIZE_END);
          gtk_widget_set_halign(preview_lbl, GTK_ALIGN_START);
          gtk_box_append(GTK_BOX(row_box), preview_lbl);
        }
      }

      GtkWidget *list_row = gtk_list_box_row_new();
      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(list_row), row_box);
      gtk_list_box_append(self->patch_list, list_row);
    }
}

/* nostrc-35i: Rebuild issue list */
static void
rebuild_issue_list(GnostrRepoBrowser *self)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->issue_list))) != NULL)
    gtk_list_box_remove(self->issue_list, child);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->issues_ht);
  while (g_hash_table_iter_next(&iter, &key, &value))
    {
      IssueBrowserData *id = (IssueBrowserData *)value;

      /* Filter */
      if (self->filter_text && *self->filter_text)
        {
          g_autofree gchar *fl = g_utf8_strdown(self->filter_text, -1);
          gboolean match = FALSE;
          if (id->subject) {
            g_autofree gchar *sl = g_utf8_strdown(id->subject, -1);
            if (g_strstr_len(sl, -1, fl)) match = TRUE;
          }
          if (!match && id->content) {
            g_autofree gchar *cl = g_utf8_strdown(id->content, -1);
            if (g_strstr_len(cl, -1, fl)) match = TRUE;
          }
          if (!match) continue;
        }

      GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
      gtk_widget_set_margin_start(row_box, 12);
      gtk_widget_set_margin_end(row_box, 12);
      gtk_widget_set_margin_top(row_box, 8);
      gtk_widget_set_margin_bottom(row_box, 8);

      /* Title line with status badge */
      GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
      const char *title = id->subject && *id->subject ? id->subject : "(untitled issue)";
      GtkWidget *title_lbl = gtk_label_new(title);
      gtk_widget_add_css_class(title_lbl, "heading");
      gtk_widget_set_halign(title_lbl, GTK_ALIGN_START);
      gtk_widget_set_hexpand(title_lbl, TRUE);
      gtk_label_set_ellipsize(GTK_LABEL(title_lbl), PANGO_ELLIPSIZE_END);
      gtk_box_append(GTK_BOX(title_box), title_lbl);

      if (id->status && *id->status) {
        GtkWidget *badge = gtk_label_new(id->status);
        if (g_strcmp0(id->status, "open") == 0)
          gtk_widget_add_css_class(badge, "success");
        else if (g_strcmp0(id->status, "closed") == 0)
          gtk_widget_add_css_class(badge, "error");
        else if (g_strcmp0(id->status, "resolved") == 0)
          gtk_widget_add_css_class(badge, "accent");
        gtk_widget_add_css_class(badge, "caption");
        gtk_box_append(GTK_BOX(title_box), badge);
      }
      gtk_box_append(GTK_BOX(row_box), title_box);

      /* Timestamp + repo ref */
      GString *meta = g_string_new(NULL);
      if (id->created_at > 0) {
        g_autoptr(GDateTime) dt = g_date_time_new_from_unix_local(id->created_at);
        g_autofree gchar *ts = g_date_time_format(dt, "%Y-%m-%d %H:%M");
        g_string_append(meta, ts);
      }
      if (id->repo_ref && *id->repo_ref) {
        if (meta->len > 0) g_string_append(meta, " · ");
        const char *last_colon = strrchr(id->repo_ref, ':');
        g_string_append(meta, last_colon ? last_colon + 1 : id->repo_ref);
      }
      GtkWidget *meta_lbl = gtk_label_new(meta->str);
      gtk_widget_add_css_class(meta_lbl, "dim-label");
      gtk_widget_set_halign(meta_lbl, GTK_ALIGN_START);
      gtk_box_append(GTK_BOX(row_box), meta_lbl);
      g_string_free(meta, TRUE);

      /* Content preview */
      if (id->content && *id->content) {
        g_autofree gchar *snip = g_strndup(id->content, 150);
        GtkWidget *preview_lbl = gtk_label_new(snip);
        gtk_widget_add_css_class(preview_lbl, "dim-label");
        gtk_label_set_ellipsize(GTK_LABEL(preview_lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_wrap(GTK_LABEL(preview_lbl), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(preview_lbl), 80);
        gtk_widget_set_halign(preview_lbl, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(row_box), preview_lbl);
      }

      GtkWidget *list_row = gtk_list_box_row_new();
      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(list_row), row_box);
      gtk_list_box_append(self->issue_list, list_row);
    }
}

static void
gnostr_repo_browser_dispose(GObject *object)
{
  GnostrRepoBrowser *self = GNOSTR_REPO_BROWSER(object);

  /* nostrc-b3b0: Call prepare_for_unbind on all NoteCardRow children BEFORE
   * the widget tree is torn down. Without this, NoteCardRow dispose runs
   * with live PangoLayout refs to a freed PangoContext -> Pango SEGV.
   * The NoteCardRow hierarchy is: ListBoxRow -> Box -> NoteCardRow. */
  if (self->repo_list) {
    GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(self->repo_list));
    while (row != NULL) {
      GtkWidget *next = gtk_widget_get_next_sibling(row);
      if (GTK_IS_LIST_BOX_ROW(row)) {
        GtkWidget *container = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
        if (container && GTK_IS_BOX(container)) {
          GtkWidget *first = gtk_widget_get_first_child(container);
          if (first && NOSTR_GTK_IS_NOTE_CARD_ROW(first))
            nostr_gtk_note_card_row_prepare_for_unbind(NOSTR_GTK_NOTE_CARD_ROW(first));
        }
      }
      row = next;
    }
  }

  g_clear_pointer(&self->repositories, g_hash_table_unref);
  g_clear_pointer(&self->patches, g_hash_table_unref);
  g_clear_pointer(&self->issues_ht, g_hash_table_unref);
  g_clear_pointer(&self->filter_text, g_free);
  g_clear_pointer(&self->selected_id, g_free);

  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
  if (child)
    gtk_widget_unparent(child);

  G_OBJECT_CLASS(gnostr_repo_browser_parent_class)->dispose(object);
}

static void
gnostr_repo_browser_class_init(GnostrRepoBrowserClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gnostr_repo_browser_dispose;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "repo-browser");

  signals[SIGNAL_REPO_SELECTED] =
    g_signal_new("repo-selected",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_CLONE_REQUESTED] =
    g_signal_new("clone-requested",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_REFRESH_REQUESTED] =
    g_signal_new("refresh-requested",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 0);

  signals[SIGNAL_NEED_PROFILE] =
    g_signal_new("need-profile",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_OPEN_PROFILE] =
    g_signal_new("open-profile",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gnostr_repo_browser_init(GnostrRepoBrowser *self)
{
  self->repositories = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, (GDestroyNotify)repo_data_free);
  self->patches = g_hash_table_new_full(g_str_hash, g_str_equal,
                                         g_free, (GDestroyNotify)patch_data_free);
  self->issues_ht = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free, (GDestroyNotify)issue_browser_data_free);
  self->is_loading = FALSE;

  /* Main container */
  self->main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent(self->main_box, GTK_WIDGET(self));

  /* Header with search and refresh */
  self->header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(self->header_box, 12);
  gtk_widget_set_margin_end(self->header_box, 12);
  gtk_widget_set_margin_top(self->header_box, 12);
  gtk_widget_set_margin_bottom(self->header_box, 8);

  self->search_entry = gtk_search_entry_new();
  gtk_widget_set_hexpand(self->search_entry, TRUE);
  /* GtkSearchEntry uses placeholder-text property, not GTK_ENTRY cast */
  g_object_set(self->search_entry, "placeholder-text", "Search repositories...", NULL);
  g_signal_connect(self->search_entry, "search-changed", G_CALLBACK(on_search_changed), self);
  gtk_box_append(GTK_BOX(self->header_box), self->search_entry);

  self->refresh_button = gtk_button_new_from_icon_name("view-refresh-symbolic");
  gtk_widget_set_tooltip_text(self->refresh_button, "Refresh repositories");
  g_signal_connect(self->refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), self);
  gtk_box_append(GTK_BOX(self->header_box), self->refresh_button);

  gtk_box_append(GTK_BOX(self->main_box), self->header_box);

  /* Stack for different states */
  self->stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(self->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_widget_set_vexpand(self->stack, TRUE);
  gtk_box_append(GTK_BOX(self->main_box), self->stack);

  /* Loading view */
  self->loading_view = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign(self->loading_view, GTK_ALIGN_CENTER);
  GtkWidget *spinner = gtk_spinner_new();
  gtk_spinner_set_spinning(GTK_SPINNER(spinner), TRUE);
  gtk_widget_set_size_request(spinner, 32, 32);
  gtk_box_append(GTK_BOX(self->loading_view), spinner);
  GtkWidget *loading_label = gtk_label_new("Loading repositories...");
  gtk_widget_add_css_class(loading_label, "dim-label");
  gtk_box_append(GTK_BOX(self->loading_view), loading_label);
  gtk_stack_add_named(GTK_STACK(self->stack), self->loading_view, "loading");

  /* Empty view */
  self->empty_view = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(self->empty_view), "folder-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(self->empty_view), "No Repositories");
  adw_status_page_set_description(ADW_STATUS_PAGE(self->empty_view),
    "No git repositories found. Repositories are published via kind 30617 events.");
  gtk_stack_add_named(GTK_STACK(self->stack), self->empty_view, "empty");

  /* List view — nostrc-35i: tabbed Repos / Patches / Issues */
  self->list_view = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  /* Tab switcher */
  self->tab_stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(self->tab_stack),
                                 GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
  gtk_widget_set_vexpand(self->tab_stack, TRUE);

  GtkWidget *tab_switcher = gtk_stack_switcher_new();
  gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(tab_switcher),
                                GTK_STACK(self->tab_stack));
  gtk_widget_set_halign(tab_switcher, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_bottom(tab_switcher, 8);
  gtk_box_append(GTK_BOX(self->list_view), tab_switcher);
  gtk_box_append(GTK_BOX(self->list_view), self->tab_stack);

  /* --- Repos tab --- */
  self->scrolled_window = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scrolled_window),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(self->scrolled_window, TRUE);

  self->repo_list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(self->repo_list, GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class(GTK_WIDGET(self->repo_list), "boxed-list");
  g_signal_connect(self->repo_list, "row-activated", G_CALLBACK(on_row_activated), self);

  GtkWidget *repo_placeholder = gtk_label_new("No repositories found.\nClick refresh to search relays.");
  gtk_widget_add_css_class(repo_placeholder, "dim-label");
  gtk_list_box_set_placeholder(self->repo_list, repo_placeholder);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scrolled_window),
                                 GTK_WIDGET(self->repo_list));
  gtk_stack_add_titled(GTK_STACK(self->tab_stack), self->scrolled_window,
                        "repos", "Repositories");

  /* --- Patches tab --- */
  self->patch_scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->patch_scrolled),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(self->patch_scrolled, TRUE);

  self->patch_list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(self->patch_list, GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class(GTK_WIDGET(self->patch_list), "boxed-list");

  GtkWidget *patch_placeholder = gtk_label_new("No patches found.\nPatches are published via kind 1617 events.");
  gtk_widget_add_css_class(patch_placeholder, "dim-label");
  gtk_list_box_set_placeholder(self->patch_list, patch_placeholder);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->patch_scrolled),
                                 GTK_WIDGET(self->patch_list));
  gtk_stack_add_titled(GTK_STACK(self->tab_stack), self->patch_scrolled,
                        "patches", "Patches");

  /* --- Issues tab --- */
  self->issue_scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->issue_scrolled),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(self->issue_scrolled, TRUE);

  self->issue_list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(self->issue_list, GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class(GTK_WIDGET(self->issue_list), "boxed-list");

  GtkWidget *issue_placeholder = gtk_label_new("No issues found.\nIssues are published via kind 1621 events.");
  gtk_widget_add_css_class(issue_placeholder, "dim-label");
  gtk_list_box_set_placeholder(self->issue_list, issue_placeholder);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->issue_scrolled),
                                 GTK_WIDGET(self->issue_list));
  gtk_stack_add_titled(GTK_STACK(self->tab_stack), self->issue_scrolled,
                        "issues", "Issues");

  gtk_stack_add_named(GTK_STACK(self->stack), self->list_view, "list");

  /* Start with empty view */
  gtk_stack_set_visible_child(GTK_STACK(self->stack), self->empty_view);
}

GnostrRepoBrowser *
gnostr_repo_browser_new(void)
{
  return g_object_new(GNOSTR_TYPE_REPO_BROWSER, NULL);
}

void
gnostr_repo_browser_add_repository(GnostrRepoBrowser *self,
                                    const char        *id,
                                    const char        *name,
                                    const char        *description,
                                    const char        *clone_url,
                                    const char        *web_url,
                                    const char        *maintainer_pubkey,
                                    gint64             updated_at)
{
  g_return_if_fail(GNOSTR_IS_REPO_BROWSER(self));
  g_return_if_fail(id != NULL);

  RepoData *data = g_new0(RepoData, 1);
  data->id = g_strdup(id);
  data->name = g_strdup(name);
  data->description = g_strdup(description);
  data->clone_url = g_strdup(clone_url);
  data->web_url = g_strdup(web_url);
  data->maintainer_pubkey = g_strdup(maintainer_pubkey);
  data->updated_at = updated_at;

  g_hash_table_replace(self->repositories, g_strdup(id), data);
  rebuild_list(self);
}

void
gnostr_repo_browser_clear(GnostrRepoBrowser *self)
{
  g_return_if_fail(GNOSTR_IS_REPO_BROWSER(self));
  g_hash_table_remove_all(self->repositories);
  rebuild_list(self);
}

void
gnostr_repo_browser_set_loading(GnostrRepoBrowser *self, gboolean loading)
{
  g_return_if_fail(GNOSTR_IS_REPO_BROWSER(self));
  self->is_loading = loading;
  rebuild_list(self);
}

void
gnostr_repo_browser_set_filter(GnostrRepoBrowser *self, const char *filter_text)
{
  g_return_if_fail(GNOSTR_IS_REPO_BROWSER(self));
  g_free(self->filter_text);
  self->filter_text = g_strdup(filter_text);
  rebuild_list(self);
}

const char *
gnostr_repo_browser_get_selected_id(GnostrRepoBrowser *self)
{
  g_return_val_if_fail(GNOSTR_IS_REPO_BROWSER(self), NULL);
  return self->selected_id;
}

guint
gnostr_repo_browser_get_count(GnostrRepoBrowser *self)
{
  g_return_val_if_fail(GNOSTR_IS_REPO_BROWSER(self), 0);
  return g_hash_table_size(self->repositories);
}

/* nostrc-35i: Patch & Issue public API */

void
gnostr_repo_browser_add_patch(GnostrRepoBrowser *self,
                               const char        *id,
                               const char        *pubkey,
                               const char        *repo_ref,
                               const char        *subject,
                               const char        *content,
                               gboolean           is_root,
                               gint64             created_at)
{
  g_return_if_fail(GNOSTR_IS_REPO_BROWSER(self));
  g_return_if_fail(id != NULL);

  PatchData *data = g_new0(PatchData, 1);
  data->id         = g_strdup(id);
  data->pubkey     = g_strdup(pubkey);
  data->repo_ref   = g_strdup(repo_ref);
  data->subject    = g_strdup(subject);
  data->content    = g_strdup(content);
  data->is_root    = is_root;
  data->created_at = created_at;

  g_hash_table_replace(self->patches, g_strdup(id), data);
  rebuild_patch_list(self);

  /* Ensure list view is visible */
  if (!self->is_loading)
    gtk_stack_set_visible_child(GTK_STACK(self->stack), self->list_view);
}

void
gnostr_repo_browser_add_issue(GnostrRepoBrowser *self,
                               const char        *id,
                               const char        *pubkey,
                               const char        *repo_ref,
                               const char        *subject,
                               const char        *content,
                               const char        *status,
                               gint64             created_at)
{
  g_return_if_fail(GNOSTR_IS_REPO_BROWSER(self));
  g_return_if_fail(id != NULL);

  IssueBrowserData *data = g_new0(IssueBrowserData, 1);
  data->id         = g_strdup(id);
  data->pubkey     = g_strdup(pubkey);
  data->repo_ref   = g_strdup(repo_ref);
  data->subject    = g_strdup(subject);
  data->content    = g_strdup(content);
  data->status     = g_strdup(status ? status : "open");
  data->created_at = created_at;

  g_hash_table_replace(self->issues_ht, g_strdup(id), data);
  rebuild_issue_list(self);

  if (!self->is_loading)
    gtk_stack_set_visible_child(GTK_STACK(self->stack), self->list_view);
}
