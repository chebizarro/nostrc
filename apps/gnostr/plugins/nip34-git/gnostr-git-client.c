/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-git-client.c - Local Git Repository Client
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#define G_LOG_DOMAIN "gnostr-git-client"

#include "gnostr-git-client.h"
#include <gtk/gtk.h>
#include <git2.h>

/* Maximum commits to display in history */
#define MAX_HISTORY_COMMITS 500

/* File status entry */
typedef struct {
  gchar *path;
  git_delta_t status;
  gboolean staged;
} FileStatusEntry;

static void
file_status_entry_free(FileStatusEntry *entry)
{
  if (!entry) return;
  g_free(entry->path);
  g_free(entry);
}

/* Commit entry for history */
typedef struct {
  gchar *id;          /* Short commit ID */
  gchar *id_full;     /* Full commit ID */
  gchar *message;     /* First line of commit message */
  gchar *author;      /* Author name */
  gchar *author_email;
  gint64 time;        /* Commit timestamp */
  GPtrArray *parents; /* Parent commit IDs */
} CommitEntry;

static void
commit_entry_free(CommitEntry *entry)
{
  if (!entry) return;
  g_free(entry->id);
  g_free(entry->id_full);
  g_free(entry->message);
  g_free(entry->author);
  g_free(entry->author_email);
  if (entry->parents) g_ptr_array_unref(entry->parents);
  g_free(entry);
}

/* Branch entry */
typedef struct {
  gchar *name;
  gchar *upstream;
  gboolean is_head;
  gboolean is_remote;
} BranchEntry;

static void
branch_entry_free(BranchEntry *entry)
{
  if (!entry) return;
  g_free(entry->name);
  g_free(entry->upstream);
  g_free(entry);
}

struct _GnostrGitClient
{
  GtkWidget parent_instance;

  /* Repository */
  git_repository *repo;
  gchar *repo_path;

  /* Main layout */
  GtkWidget *main_box;
  GtkWidget *header_bar;
  GtkWidget *repo_label;
  GtkWidget *stack;

  /* Status tab */
  GtkWidget *status_page;
  GtkWidget *status_stack;
  GtkWidget *no_repo_view;
  GtkWidget *status_view;
  GtkWidget *staged_list;
  GtkWidget *unstaged_list;
  GtkWidget *commit_entry;
  GtkWidget *commit_button;

  /* History tab */
  GtkWidget *history_page;
  GtkWidget *history_list;

  /* Branches tab */
  GtkWidget *branches_page;
  GtkWidget *branches_list;

  /* Data */
  GPtrArray *file_statuses;
  GPtrArray *commits;
  GPtrArray *branches;

  /* State */
  gboolean is_cloning;
  GCancellable *clone_cancellable;
};

G_DEFINE_TYPE(GnostrGitClient, gnostr_git_client, GTK_TYPE_WIDGET)

enum {
  SIGNAL_REPO_OPENED,
  SIGNAL_REPO_CLOSED,
  SIGNAL_COMMIT_CREATED,
  SIGNAL_ERROR,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void refresh_status(GnostrGitClient *self);
static void refresh_history(GnostrGitClient *self);
static void refresh_branches(GnostrGitClient *self);
static void update_ui_state(GnostrGitClient *self);

/* ============================================================================
 * libgit2 helpers
 * ============================================================================ */

static gint libgit2_ref_count = 0;

static void
libgit2_ref(void)
{
  if (g_atomic_int_add(&libgit2_ref_count, 1) == 0)
    git_libgit2_init();
}

static void
libgit2_unref(void)
{
  if (g_atomic_int_dec_and_test(&libgit2_ref_count))
    git_libgit2_shutdown();
}

static const char *
status_string(git_delta_t status)
{
  switch (status)
    {
    case GIT_DELTA_ADDED:       return "Added";
    case GIT_DELTA_DELETED:     return "Deleted";
    case GIT_DELTA_MODIFIED:    return "Modified";
    case GIT_DELTA_RENAMED:     return "Renamed";
    case GIT_DELTA_COPIED:      return "Copied";
    case GIT_DELTA_TYPECHANGE:  return "Type changed";
    case GIT_DELTA_UNTRACKED:   return "Untracked";
    case GIT_DELTA_IGNORED:     return "Ignored";
    case GIT_DELTA_CONFLICTED:  return "Conflicted";
    default:                    return "Unknown";
    }
}

static const char *
status_icon(git_delta_t status)
{
  switch (status)
    {
    case GIT_DELTA_ADDED:       return "list-add-symbolic";
    case GIT_DELTA_DELETED:     return "list-remove-symbolic";
    case GIT_DELTA_MODIFIED:    return "document-edit-symbolic";
    case GIT_DELTA_RENAMED:     return "edit-find-replace-symbolic";
    case GIT_DELTA_UNTRACKED:   return "document-new-symbolic";
    case GIT_DELTA_CONFLICTED:  return "dialog-warning-symbolic";
    default:                    return "changes-allow-symbolic";
    }
}

/* ============================================================================
 * Status collection callback
 * ============================================================================ */

static int
status_cb(const char *path, unsigned int status_flags, void *payload)
{
  GPtrArray *statuses = (GPtrArray *)payload;

  /* Check for staged changes */
  if (status_flags & (GIT_STATUS_INDEX_NEW | GIT_STATUS_INDEX_MODIFIED |
                      GIT_STATUS_INDEX_DELETED | GIT_STATUS_INDEX_RENAMED |
                      GIT_STATUS_INDEX_TYPECHANGE))
    {
      FileStatusEntry *entry = g_new0(FileStatusEntry, 1);
      entry->path = g_strdup(path);
      entry->staged = TRUE;

      if (status_flags & GIT_STATUS_INDEX_NEW)
        entry->status = GIT_DELTA_ADDED;
      else if (status_flags & GIT_STATUS_INDEX_MODIFIED)
        entry->status = GIT_DELTA_MODIFIED;
      else if (status_flags & GIT_STATUS_INDEX_DELETED)
        entry->status = GIT_DELTA_DELETED;
      else if (status_flags & GIT_STATUS_INDEX_RENAMED)
        entry->status = GIT_DELTA_RENAMED;
      else
        entry->status = GIT_DELTA_TYPECHANGE;

      g_ptr_array_add(statuses, entry);
    }

  /* Check for working tree changes */
  if (status_flags & (GIT_STATUS_WT_NEW | GIT_STATUS_WT_MODIFIED |
                      GIT_STATUS_WT_DELETED | GIT_STATUS_WT_RENAMED |
                      GIT_STATUS_WT_TYPECHANGE))
    {
      FileStatusEntry *entry = g_new0(FileStatusEntry, 1);
      entry->path = g_strdup(path);
      entry->staged = FALSE;

      if (status_flags & GIT_STATUS_WT_NEW)
        entry->status = GIT_DELTA_UNTRACKED;
      else if (status_flags & GIT_STATUS_WT_MODIFIED)
        entry->status = GIT_DELTA_MODIFIED;
      else if (status_flags & GIT_STATUS_WT_DELETED)
        entry->status = GIT_DELTA_DELETED;
      else if (status_flags & GIT_STATUS_WT_RENAMED)
        entry->status = GIT_DELTA_RENAMED;
      else
        entry->status = GIT_DELTA_TYPECHANGE;

      g_ptr_array_add(statuses, entry);
    }

  return 0;
}

/* ============================================================================
 * UI building
 * ============================================================================ */

static void
on_stage_button_clicked(GtkButton *button, gpointer user_data)
{
  GnostrGitClient *self = GNOSTR_GIT_CLIENT(user_data);
  const char *path = g_object_get_data(G_OBJECT(button), "file-path");

  if (path && gnostr_git_client_stage_file(self, path))
    refresh_status(self);
}

static void
on_unstage_button_clicked(GtkButton *button, gpointer user_data)
{
  GnostrGitClient *self = GNOSTR_GIT_CLIENT(user_data);
  const char *path = g_object_get_data(G_OBJECT(button), "file-path");

  if (path && gnostr_git_client_unstage_file(self, path))
    refresh_status(self);
}

static GtkWidget *
create_status_row(GnostrGitClient *self, FileStatusEntry *entry)
{
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(row, 8);
  gtk_widget_set_margin_end(row, 8);
  gtk_widget_set_margin_top(row, 6);
  gtk_widget_set_margin_bottom(row, 6);

  /* Status icon */
  GtkWidget *icon = gtk_image_new_from_icon_name(status_icon(entry->status));
  gtk_widget_set_tooltip_text(icon, status_string(entry->status));
  gtk_box_append(GTK_BOX(row), icon);

  /* File path */
  GtkWidget *label = gtk_label_new(entry->path);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_START);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(row), label);

  /* Stage/unstage button */
  GtkWidget *action_btn;
  if (entry->staged)
    {
      action_btn = gtk_button_new_from_icon_name("list-remove-symbolic");
      gtk_widget_set_tooltip_text(action_btn, "Unstage");
      g_object_set_data_full(G_OBJECT(action_btn), "file-path",
                             g_strdup(entry->path), g_free);
      g_signal_connect(action_btn, "clicked",
                       G_CALLBACK(on_unstage_button_clicked), self);
    }
  else
    {
      action_btn = gtk_button_new_from_icon_name("list-add-symbolic");
      gtk_widget_set_tooltip_text(action_btn, "Stage");
      g_object_set_data_full(G_OBJECT(action_btn), "file-path",
                             g_strdup(entry->path), g_free);
      g_signal_connect(action_btn, "clicked",
                       G_CALLBACK(on_stage_button_clicked), self);
    }
  gtk_widget_add_css_class(action_btn, "flat");
  gtk_box_append(GTK_BOX(row), action_btn);

  GtkWidget *list_row = gtk_list_box_row_new();
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(list_row), row);
  return list_row;
}

static GtkWidget *
create_commit_row(CommitEntry *entry)
{
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start(row, 12);
  gtk_widget_set_margin_end(row, 12);
  gtk_widget_set_margin_top(row, 8);
  gtk_widget_set_margin_bottom(row, 8);

  /* Header: commit ID and time */
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  GtkWidget *id_label = gtk_label_new(entry->id);
  gtk_widget_add_css_class(id_label, "monospace");
  gtk_widget_add_css_class(id_label, "accent");
  gtk_box_append(GTK_BOX(header), id_label);

  g_autoptr(GDateTime) dt = g_date_time_new_from_unix_local(entry->time);
  g_autofree char *time_str = g_date_time_format(dt, "%Y-%m-%d %H:%M");
  GtkWidget *time_label = gtk_label_new(time_str);
  gtk_widget_add_css_class(time_label, "dim-label");
  gtk_widget_set_hexpand(time_label, TRUE);
  gtk_widget_set_halign(time_label, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(header), time_label);

  gtk_box_append(GTK_BOX(row), header);

  /* Message */
  GtkWidget *msg_label = gtk_label_new(entry->message);
  gtk_label_set_wrap(GTK_LABEL(msg_label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(msg_label), 60);
  gtk_label_set_xalign(GTK_LABEL(msg_label), 0);
  gtk_box_append(GTK_BOX(row), msg_label);

  /* Author */
  g_autofree char *author_str = g_strdup_printf("%s <%s>",
                                                 entry->author,
                                                 entry->author_email);
  GtkWidget *author_label = gtk_label_new(author_str);
  gtk_widget_add_css_class(author_label, "dim-label");
  gtk_widget_set_halign(author_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(row), author_label);

  GtkWidget *list_row = gtk_list_box_row_new();
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(list_row), row);
  return list_row;
}

static GtkWidget *
create_branch_row(BranchEntry *entry)
{
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(row, 12);
  gtk_widget_set_margin_end(row, 12);
  gtk_widget_set_margin_top(row, 8);
  gtk_widget_set_margin_bottom(row, 8);

  /* Branch icon */
  const char *icon_name = entry->is_remote ? "network-server-symbolic"
                                           : "system-software-update-symbolic";
  GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
  gtk_box_append(GTK_BOX(row), icon);

  /* Branch name */
  GtkWidget *name_label = gtk_label_new(entry->name);
  if (entry->is_head)
    gtk_widget_add_css_class(name_label, "accent");
  gtk_widget_set_hexpand(name_label, TRUE);
  gtk_widget_set_halign(name_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(row), name_label);

  /* Head indicator */
  if (entry->is_head)
    {
      GtkWidget *head_badge = gtk_label_new("HEAD");
      gtk_widget_add_css_class(head_badge, "badge");
      gtk_widget_add_css_class(head_badge, "accent");
      gtk_box_append(GTK_BOX(row), head_badge);
    }

  /* Upstream info */
  if (entry->upstream)
    {
      GtkWidget *upstream_label = gtk_label_new(entry->upstream);
      gtk_widget_add_css_class(upstream_label, "dim-label");
      gtk_box_append(GTK_BOX(row), upstream_label);
    }

  GtkWidget *list_row = gtk_list_box_row_new();
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(list_row), row);
  return list_row;
}

static void
on_commit_button_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  GnostrGitClient *self = GNOSTR_GIT_CLIENT(user_data);

  const char *message = gtk_editable_get_text(GTK_EDITABLE(self->commit_entry));
  if (!message || *message == '\0')
    {
      g_signal_emit(self, signals[SIGNAL_ERROR], 0, "Commit message required");
      return;
    }

  g_autofree char *commit_id = gnostr_git_client_commit(self, message);
  if (commit_id)
    {
      gtk_editable_set_text(GTK_EDITABLE(self->commit_entry), "");
      gnostr_git_client_refresh(self);
    }
}

static void
clear_list(GtkListBox *list)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(list))) != NULL)
    gtk_list_box_remove(list, child);
}

static void
refresh_status(GnostrGitClient *self)
{
  clear_list(GTK_LIST_BOX(self->staged_list));
  clear_list(GTK_LIST_BOX(self->unstaged_list));

  if (!self->repo)
    return;

  /* Clear old statuses */
  g_clear_pointer(&self->file_statuses, g_ptr_array_unref);

  self->file_statuses = g_ptr_array_new_with_free_func(
      (GDestroyNotify)file_status_entry_free);

  /* Collect status */
  git_status_options opts = GIT_STATUS_OPTIONS_INIT;
  opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
  opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
               GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX |
               GIT_STATUS_OPT_SORT_CASE_SENSITIVELY;

  git_status_foreach_ext(self->repo, &opts, status_cb, self->file_statuses);

  /* Populate lists */
  guint staged_count = 0;
  guint unstaged_count = 0;

  for (guint i = 0; i < self->file_statuses->len; i++)
    {
      FileStatusEntry *entry = g_ptr_array_index(self->file_statuses, i);
      GtkWidget *row = create_status_row(self, entry);

      if (entry->staged)
        {
          gtk_list_box_append(GTK_LIST_BOX(self->staged_list), row);
          staged_count++;
        }
      else
        {
          gtk_list_box_append(GTK_LIST_BOX(self->unstaged_list), row);
          unstaged_count++;
        }
    }

  /* Enable/disable commit button */
  gtk_widget_set_sensitive(self->commit_button, staged_count > 0);

  g_debug("[git-client] Status: %u staged, %u unstaged",
          staged_count, unstaged_count);
}

static void
refresh_history(GnostrGitClient *self)
{
  clear_list(GTK_LIST_BOX(self->history_list));

  if (!self->repo)
    return;

  /* Clear old commits */
  g_clear_pointer(&self->commits, g_ptr_array_unref);

  self->commits = g_ptr_array_new_with_free_func(
      (GDestroyNotify)commit_entry_free);

  /* Get HEAD */
  git_reference *head_ref = NULL;
  if (git_repository_head(&head_ref, self->repo) != 0)
    {
      g_debug("[git-client] No HEAD reference");
      return;
    }

  const git_oid *head_oid = git_reference_target(head_ref);
  if (!head_oid)
    {
      git_reference_free(head_ref);
      return;
    }

  /* Create revwalk */
  git_revwalk *walk = NULL;
  if (git_revwalk_new(&walk, self->repo) != 0)
    {
      git_reference_free(head_ref);
      return;
    }

  git_revwalk_sorting(walk, GIT_SORT_TIME);
  git_revwalk_push_head(walk);

  /* Walk commits */
  git_oid oid;
  guint count = 0;

  while (count < MAX_HISTORY_COMMITS && git_revwalk_next(&oid, walk) == 0)
    {
      git_commit *commit = NULL;
      if (git_commit_lookup(&commit, self->repo, &oid) != 0)
        continue;

      CommitEntry *entry = g_new0(CommitEntry, 1);

      /* Get IDs */
      char id_buf[GIT_OID_MAX_HEXSIZE + 1];
      git_oid_tostr(id_buf, sizeof(id_buf), &oid);
      entry->id_full = g_strdup(id_buf);
      entry->id = g_strndup(id_buf, 8);

      /* Get message (first line only) */
      const char *msg = git_commit_message(commit);
      if (msg)
        {
          const char *newline = strchr(msg, '\n');
          if (newline)
            entry->message = g_strndup(msg, newline - msg);
          else
            entry->message = g_strdup(msg);
        }
      else
        {
          entry->message = g_strdup("");
        }

      /* Get author */
      const git_signature *author = git_commit_author(commit);
      if (author)
        {
          entry->author = g_strdup(author->name ? author->name : "Unknown");
          entry->author_email = g_strdup(author->email ? author->email : "");
          entry->time = author->when.time;
        }
      else
        {
          entry->author = g_strdup("Unknown");
          entry->author_email = g_strdup("");
          entry->time = 0;
        }

      /* Get parent IDs */
      entry->parents = g_ptr_array_new_with_free_func(g_free);
      unsigned int parent_count = git_commit_parentcount(commit);
      for (unsigned int i = 0; i < parent_count; i++)
        {
          const git_oid *parent_oid = git_commit_parent_id(commit, i);
          char parent_buf[GIT_OID_MAX_HEXSIZE + 1];
          git_oid_tostr(parent_buf, sizeof(parent_buf), parent_oid);
          g_ptr_array_add(entry->parents, g_strdup(parent_buf));
        }

      g_ptr_array_add(self->commits, entry);
      git_commit_free(commit);
      count++;
    }

  git_revwalk_free(walk);
  git_reference_free(head_ref);

  /* Populate list */
  for (guint i = 0; i < self->commits->len; i++)
    {
      CommitEntry *entry = g_ptr_array_index(self->commits, i);
      GtkWidget *row = create_commit_row(entry);
      gtk_list_box_append(GTK_LIST_BOX(self->history_list), row);
    }

  g_debug("[git-client] Loaded %u commits", self->commits->len);
}

static void
refresh_branches(GnostrGitClient *self)
{
  clear_list(GTK_LIST_BOX(self->branches_list));

  if (!self->repo)
    return;

  /* Clear old branches */
  g_clear_pointer(&self->branches, g_ptr_array_unref);

  self->branches = g_ptr_array_new_with_free_func(
      (GDestroyNotify)branch_entry_free);

  /* Get current branch name */
  git_reference *head_ref = NULL;
  const char *head_branch = NULL;
  if (git_repository_head(&head_ref, self->repo) == 0)
    {
      if (git_reference_is_branch(head_ref))
        head_branch = git_reference_shorthand(head_ref);
    }

  /* Iterate branches */
  git_branch_iterator *iter = NULL;
  if (git_branch_iterator_new(&iter, self->repo, GIT_BRANCH_ALL) != 0)
    {
      if (head_ref) git_reference_free(head_ref);
      return;
    }

  git_reference *ref = NULL;
  git_branch_t type;

  while (git_branch_next(&ref, &type, iter) == 0)
    {
      BranchEntry *entry = g_new0(BranchEntry, 1);

      const char *name = NULL;
      git_branch_name(&name, ref);
      entry->name = g_strdup(name ? name : "unknown");
      entry->is_remote = (type == GIT_BRANCH_REMOTE);
      entry->is_head = (head_branch && g_strcmp0(name, head_branch) == 0);

      /* Get upstream */
      git_reference *upstream = NULL;
      if (!entry->is_remote && git_branch_upstream(&upstream, ref) == 0)
        {
          const char *upstream_name = git_reference_shorthand(upstream);
          entry->upstream = g_strdup(upstream_name);
          git_reference_free(upstream);
        }

      g_ptr_array_add(self->branches, entry);
      git_reference_free(ref);
    }

  git_branch_iterator_free(iter);
  if (head_ref) git_reference_free(head_ref);

  /* Populate list - local branches first, then remote */
  for (guint i = 0; i < self->branches->len; i++)
    {
      BranchEntry *entry = g_ptr_array_index(self->branches, i);
      if (!entry->is_remote)
        {
          GtkWidget *row = create_branch_row(entry);
          gtk_list_box_append(GTK_LIST_BOX(self->branches_list), row);
        }
    }

  for (guint i = 0; i < self->branches->len; i++)
    {
      BranchEntry *entry = g_ptr_array_index(self->branches, i);
      if (entry->is_remote)
        {
          GtkWidget *row = create_branch_row(entry);
          gtk_list_box_append(GTK_LIST_BOX(self->branches_list), row);
        }
    }

  g_debug("[git-client] Loaded %u branches", self->branches->len);
}

static void
update_ui_state(GnostrGitClient *self)
{
  gboolean has_repo = (self->repo != NULL);

  if (has_repo)
    {
      /* Show repo path in header */
      g_autofree char *basename = g_path_get_basename(self->repo_path);
      gtk_label_set_text(GTK_LABEL(self->repo_label), basename);
      gtk_stack_set_visible_child(GTK_STACK(self->status_stack), self->status_view);
    }
  else
    {
      gtk_label_set_text(GTK_LABEL(self->repo_label), "No repository");
      gtk_stack_set_visible_child(GTK_STACK(self->status_stack), self->no_repo_view);
    }
}

/* ============================================================================
 * Widget lifecycle
 * ============================================================================ */

static void
gnostr_git_client_dispose(GObject *object)
{
  GnostrGitClient *self = GNOSTR_GIT_CLIENT(object);

  gnostr_git_client_close(self);

  g_clear_pointer(&self->file_statuses, g_ptr_array_unref);
  g_clear_pointer(&self->commits, g_ptr_array_unref);
  g_clear_pointer(&self->branches, g_ptr_array_unref);
  g_clear_pointer(&self->repo_path, g_free);

  if (self->clone_cancellable)
    {
      g_cancellable_cancel(self->clone_cancellable);
      g_clear_object(&self->clone_cancellable);
    }

  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
  if (child)
    gtk_widget_unparent(child);

  libgit2_unref();

  G_OBJECT_CLASS(gnostr_git_client_parent_class)->dispose(object);
}

static void
gnostr_git_client_class_init(GnostrGitClientClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gnostr_git_client_dispose;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "git-client");

  signals[SIGNAL_REPO_OPENED] =
    g_signal_new("repo-opened",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_REPO_CLOSED] =
    g_signal_new("repo-closed",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 0);

  signals[SIGNAL_COMMIT_CREATED] =
    g_signal_new("commit-created",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_ERROR] =
    g_signal_new("error",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gnostr_git_client_init(GnostrGitClient *self)
{
  libgit2_ref();

  /* Main container */
  self->main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent(self->main_box, GTK_WIDGET(self));

  /* Header */
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(header, 12);
  gtk_widget_set_margin_end(header, 12);
  gtk_widget_set_margin_top(header, 12);
  gtk_widget_set_margin_bottom(header, 8);

  GtkWidget *icon = gtk_image_new_from_icon_name("folder-symbolic");
  gtk_box_append(GTK_BOX(header), icon);

  self->repo_label = gtk_label_new("No repository");
  gtk_widget_add_css_class(self->repo_label, "heading");
  gtk_widget_set_hexpand(self->repo_label, TRUE);
  gtk_widget_set_halign(self->repo_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(header), self->repo_label);

  gtk_box_append(GTK_BOX(self->main_box), header);

  /* Tab stack */
  GtkWidget *stack_switcher = gtk_stack_switcher_new();
  gtk_widget_set_halign(stack_switcher, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_bottom(stack_switcher, 8);
  gtk_box_append(GTK_BOX(self->main_box), stack_switcher);

  self->stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(self->stack),
                                 GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
  gtk_widget_set_vexpand(self->stack, TRUE);
  gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(stack_switcher),
                                GTK_STACK(self->stack));
  gtk_box_append(GTK_BOX(self->main_box), self->stack);

  /* ---- Status tab ---- */
  self->status_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  self->status_stack = gtk_stack_new();
  gtk_widget_set_vexpand(self->status_stack, TRUE);
  gtk_box_append(GTK_BOX(self->status_page), self->status_stack);

  /* No repo view - simple GTK placeholder */
  self->no_repo_view = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign(self->no_repo_view, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(self->no_repo_view, GTK_ALIGN_CENTER);

  GtkWidget *no_repo_icon = gtk_image_new_from_icon_name("folder-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(no_repo_icon), 64);
  gtk_widget_add_css_class(no_repo_icon, "dim-label");
  gtk_box_append(GTK_BOX(self->no_repo_view), no_repo_icon);

  GtkWidget *no_repo_title = gtk_label_new("No Repository Open");
  gtk_widget_add_css_class(no_repo_title, "title-2");
  gtk_box_append(GTK_BOX(self->no_repo_view), no_repo_title);

  GtkWidget *no_repo_desc = gtk_label_new("Clone or open a repository to get started");
  gtk_widget_add_css_class(no_repo_desc, "dim-label");
  gtk_box_append(GTK_BOX(self->no_repo_view), no_repo_desc);

  gtk_stack_add_named(GTK_STACK(self->status_stack), self->no_repo_view, "no-repo");

  /* Status view */
  self->status_view = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(self->status_view, 12);
  gtk_widget_set_margin_end(self->status_view, 12);

  /* Staged section */
  GtkWidget *staged_label = gtk_label_new("Staged Changes");
  gtk_widget_add_css_class(staged_label, "title-4");
  gtk_widget_set_halign(staged_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(self->status_view), staged_label);

  GtkWidget *staged_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_min_content_height(
      GTK_SCROLLED_WINDOW(staged_scroll), 120);
  self->staged_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->staged_list),
                                   GTK_SELECTION_NONE);
  gtk_widget_add_css_class(self->staged_list, "boxed-list");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(staged_scroll),
                                 self->staged_list);
  gtk_box_append(GTK_BOX(self->status_view), staged_scroll);

  /* Commit message entry */
  self->commit_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(self->commit_entry),
                                  "Commit message...");
  gtk_box_append(GTK_BOX(self->status_view), self->commit_entry);

  self->commit_button = gtk_button_new_with_label("Commit");
  gtk_widget_add_css_class(self->commit_button, "suggested-action");
  gtk_widget_set_sensitive(self->commit_button, FALSE);
  g_signal_connect(self->commit_button, "clicked",
                   G_CALLBACK(on_commit_button_clicked), self);
  gtk_box_append(GTK_BOX(self->status_view), self->commit_button);

  /* Unstaged section */
  GtkWidget *unstaged_label = gtk_label_new("Unstaged Changes");
  gtk_widget_add_css_class(unstaged_label, "title-4");
  gtk_widget_set_halign(unstaged_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(self->status_view), unstaged_label);

  GtkWidget *unstaged_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_min_content_height(
      GTK_SCROLLED_WINDOW(unstaged_scroll), 150);
  gtk_widget_set_vexpand(unstaged_scroll, TRUE);
  self->unstaged_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->unstaged_list),
                                   GTK_SELECTION_NONE);
  gtk_widget_add_css_class(self->unstaged_list, "boxed-list");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(unstaged_scroll),
                                 self->unstaged_list);
  gtk_box_append(GTK_BOX(self->status_view), unstaged_scroll);

  gtk_stack_add_named(GTK_STACK(self->status_stack), self->status_view, "status");
  gtk_stack_set_visible_child(GTK_STACK(self->status_stack), self->no_repo_view);

  gtk_stack_add_titled(GTK_STACK(self->stack), self->status_page,
                        "status", "Status");

  /* ---- History tab ---- */
  self->history_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *history_scroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(history_scroll, TRUE);
  self->history_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->history_list),
                                   GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class(self->history_list, "boxed-list");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(history_scroll),
                                 self->history_list);
  gtk_box_append(GTK_BOX(self->history_page), history_scroll);

  gtk_stack_add_titled(GTK_STACK(self->stack), self->history_page,
                        "history", "History");

  /* ---- Branches tab ---- */
  self->branches_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *branches_scroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(branches_scroll, TRUE);
  self->branches_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->branches_list),
                                   GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class(self->branches_list, "boxed-list");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(branches_scroll),
                                 self->branches_list);
  gtk_box_append(GTK_BOX(self->branches_page), branches_scroll);

  gtk_stack_add_titled(GTK_STACK(self->stack), self->branches_page,
                        "branches", "Branches");

  update_ui_state(self);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

GnostrGitClient *
gnostr_git_client_new(void)
{
  return g_object_new(GNOSTR_TYPE_GIT_CLIENT, NULL);
}

void
gnostr_git_client_clone(GnostrGitClient *self,
                         const char      *url,
                         const char      *path)
{
  g_return_if_fail(GNOSTR_IS_GIT_CLIENT(self));
  g_return_if_fail(url != NULL);
  g_return_if_fail(path != NULL);

  if (self->is_cloning)
    {
      g_signal_emit(self, signals[SIGNAL_ERROR], 0, "Clone already in progress");
      return;
    }

  self->is_cloning = TRUE;
  self->clone_cancellable = g_cancellable_new();

  git_repository *repo = NULL;
  git_clone_options opts = GIT_CLONE_OPTIONS_INIT;

  int error = git_clone(&repo, url, path, &opts);

  self->is_cloning = FALSE;
  g_clear_object(&self->clone_cancellable);

  if (error != 0)
    {
      const git_error *e = git_error_last();
      g_autofree char *msg = g_strdup_printf("Clone failed: %s",
                                              e ? e->message : "unknown error");
      g_signal_emit(self, signals[SIGNAL_ERROR], 0, msg);
      return;
    }

  /* Store repo */
  if (self->repo)
    git_repository_free(self->repo);

  self->repo = repo;
  g_free(self->repo_path);
  self->repo_path = g_strdup(path);

  update_ui_state(self);
  gnostr_git_client_refresh(self);

  g_signal_emit(self, signals[SIGNAL_REPO_OPENED], 0, path);
}

gboolean
gnostr_git_client_open(GnostrGitClient *self, const char *path)
{
  g_return_val_if_fail(GNOSTR_IS_GIT_CLIENT(self), FALSE);
  g_return_val_if_fail(path != NULL, FALSE);

  git_repository *repo = NULL;
  int error = git_repository_open(&repo, path);

  if (error != 0)
    {
      const git_error *e = git_error_last();
      g_autofree char *msg = g_strdup_printf("Open failed: %s",
                                              e ? e->message : "unknown error");
      g_signal_emit(self, signals[SIGNAL_ERROR], 0, msg);
      return FALSE;
    }

  /* Close existing repo */
  if (self->repo)
    git_repository_free(self->repo);

  self->repo = repo;
  g_free(self->repo_path);
  self->repo_path = g_strdup(path);

  update_ui_state(self);
  gnostr_git_client_refresh(self);

  g_signal_emit(self, signals[SIGNAL_REPO_OPENED], 0, path);
  return TRUE;
}

void
gnostr_git_client_close(GnostrGitClient *self)
{
  g_return_if_fail(GNOSTR_IS_GIT_CLIENT(self));

  if (!self->repo)
    return;

  git_repository_free(self->repo);
  self->repo = NULL;

  g_clear_pointer(&self->repo_path, g_free);

  /* Clear UI */
  clear_list(GTK_LIST_BOX(self->staged_list));
  clear_list(GTK_LIST_BOX(self->unstaged_list));
  clear_list(GTK_LIST_BOX(self->history_list));
  clear_list(GTK_LIST_BOX(self->branches_list));

  update_ui_state(self);

  g_signal_emit(self, signals[SIGNAL_REPO_CLOSED], 0);
}

const char *
gnostr_git_client_get_path(GnostrGitClient *self)
{
  g_return_val_if_fail(GNOSTR_IS_GIT_CLIENT(self), NULL);
  return self->repo_path;
}

gboolean
gnostr_git_client_is_open(GnostrGitClient *self)
{
  g_return_val_if_fail(GNOSTR_IS_GIT_CLIENT(self), FALSE);
  return self->repo != NULL;
}

void
gnostr_git_client_refresh(GnostrGitClient *self)
{
  g_return_if_fail(GNOSTR_IS_GIT_CLIENT(self));

  if (!self->repo)
    return;

  refresh_status(self);
  refresh_history(self);
  refresh_branches(self);
}

gboolean
gnostr_git_client_stage_file(GnostrGitClient *self, const char *path)
{
  g_return_val_if_fail(GNOSTR_IS_GIT_CLIENT(self), FALSE);
  g_return_val_if_fail(path != NULL, FALSE);

  if (!self->repo)
    return FALSE;

  git_index *index = NULL;
  if (git_repository_index(&index, self->repo) != 0)
    return FALSE;

  int error = git_index_add_bypath(index, path);
  if (error != 0)
    {
      git_index_free(index);
      return FALSE;
    }

  error = git_index_write(index);
  git_index_free(index);

  return error == 0;
}

gboolean
gnostr_git_client_unstage_file(GnostrGitClient *self, const char *path)
{
  g_return_val_if_fail(GNOSTR_IS_GIT_CLIENT(self), FALSE);
  g_return_val_if_fail(path != NULL, FALSE);

  if (!self->repo)
    return FALSE;

  git_reference *head_ref = NULL;
  git_object *head_obj = NULL;

  /* Get HEAD commit */
  if (git_repository_head(&head_ref, self->repo) != 0)
    return FALSE;

  if (git_reference_peel(&head_obj, head_ref, GIT_OBJECT_COMMIT) != 0)
    {
      git_reference_free(head_ref);
      return FALSE;
    }

  /* Reset file in index to HEAD state */
  const char *paths[] = { path };
  git_strarray pathspec = { (char **)paths, 1 };

  int error = git_reset_default(self->repo, head_obj, &pathspec);

  git_object_free(head_obj);
  git_reference_free(head_ref);

  return error == 0;
}

char *
gnostr_git_client_commit(GnostrGitClient *self, const char *message)
{
  g_return_val_if_fail(GNOSTR_IS_GIT_CLIENT(self), NULL);
  g_return_val_if_fail(message != NULL, NULL);

  if (!self->repo)
    {
      g_signal_emit(self, signals[SIGNAL_ERROR], 0, "No repository open");
      return NULL;
    }

  /* Get index */
  git_index *index = NULL;
  if (git_repository_index(&index, self->repo) != 0)
    {
      g_signal_emit(self, signals[SIGNAL_ERROR], 0, "Failed to get index");
      return NULL;
    }

  /* Write index to tree */
  git_oid tree_oid;
  if (git_index_write_tree(&tree_oid, index) != 0)
    {
      git_index_free(index);
      g_signal_emit(self, signals[SIGNAL_ERROR], 0, "Failed to write tree");
      return NULL;
    }
  git_index_free(index);

  /* Get tree */
  git_tree *tree = NULL;
  if (git_tree_lookup(&tree, self->repo, &tree_oid) != 0)
    {
      g_signal_emit(self, signals[SIGNAL_ERROR], 0, "Failed to lookup tree");
      return NULL;
    }

  /* Get signature */
  git_signature *sig = NULL;
  if (git_signature_default(&sig, self->repo) != 0)
    {
      /* Try to create a signature from config */
      if (git_signature_now(&sig, "Gnostr User", "user@gnostr.local") != 0)
        {
          git_tree_free(tree);
          g_signal_emit(self, signals[SIGNAL_ERROR], 0, "Failed to get signature");
          return NULL;
        }
    }

  /* Get parent commit (HEAD) */
  git_reference *head_ref = NULL;
  git_commit *parent = NULL;
  const git_commit *parents[1] = { NULL };
  size_t parent_count = 0;

  if (git_repository_head(&head_ref, self->repo) == 0)
    {
      git_oid parent_oid;
      if (git_reference_name_to_id(&parent_oid, self->repo, "HEAD") == 0)
        {
          git_commit_lookup(&parent, self->repo, &parent_oid);
          parents[0] = parent;
          parent_count = 1;
        }
    }

  /* Create commit */
  git_oid commit_oid;
  int error = git_commit_create(
      &commit_oid,
      self->repo,
      "HEAD",
      sig,
      sig,
      NULL,
      message,
      tree,
      parent_count,
      parents);

  /* Cleanup */
  if (parent) git_commit_free(parent);
  if (head_ref) git_reference_free(head_ref);
  git_signature_free(sig);
  git_tree_free(tree);

  if (error != 0)
    {
      const git_error *e = git_error_last();
      g_autofree char *msg = g_strdup_printf("Commit failed: %s",
                                              e ? e->message : "unknown error");
      g_signal_emit(self, signals[SIGNAL_ERROR], 0, msg);
      return NULL;
    }

  /* Return commit ID */
  char id_buf[GIT_OID_MAX_HEXSIZE + 1];
  git_oid_tostr(id_buf, sizeof(id_buf), &commit_oid);

  g_signal_emit(self, signals[SIGNAL_COMMIT_CREATED], 0, id_buf);

  return g_strdup(id_buf);
}
