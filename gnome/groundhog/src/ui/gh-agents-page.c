#include "gh-agents-page.h"

#include <glib/gi18n.h>

#define CONNECTORS_RESOURCE "/org/nostr/Groundhog/agents/connectors.ini"

typedef struct {
  gchar *name;
  GtkWidget *row;        /* borrowed from the page */
  GtkLabel *prompt_label; /* borrowed */
} ConnectorRow;

typedef struct {
  AdwPreferencesPage *page; /* borrowed; owns this state */
  gchar *npub;
  GPtrArray *rows;
  AdwEntryRow *agent_entry;
  GtkButton *start_button;
  GhAgentsStartFunc start;
  gpointer start_data;
  gboolean start_busy;
} PageState;

static GKeyFile *
load_connectors(void)
{
  g_autoptr(GBytes) bytes = g_resources_lookup_data(CONNECTORS_RESOURCE,
                                                     G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
  if (!bytes)
    return NULL;
  gsize size = 0;
  const gchar *data = g_bytes_get_data(bytes, &size);
  GKeyFile *file = g_key_file_new();
  if (!g_key_file_load_from_data(file, data, size, G_KEY_FILE_NONE, NULL)) {
    g_key_file_unref(file);
    return NULL;
  }
  return file;
}

gchar *
gh_agents_page_dup_prompt(const gchar *connector, const gchar *npub)
{
  if (!connector || !npub || !g_str_has_prefix(npub, "npub1"))
    return NULL;
  g_autoptr(GKeyFile) file = load_connectors();
  if (!file)
    return NULL;
  g_autofree gchar *template = g_key_file_get_string(file, connector, "prompt", NULL);
  if (!template || !g_strstr_len(template, -1, "{npub}"))
    return NULL;
  g_auto(GStrv) parts = g_strsplit(template, "{npub}", -1);
  return g_strjoinv(npub, parts);
}

static void
connector_row_free(gpointer data)
{
  ConnectorRow *row = data;
  g_free(row->name);
  g_free(row);
}

static void
page_state_free(gpointer data)
{
  PageState *state = data;
  g_free(state->npub);
  g_ptr_array_unref(state->rows);
  g_free(state);
}

static PageState *
state_of(AdwPreferencesPage *page)
{
  return g_object_get_data(G_OBJECT(page), "gh-agents-page-state");
}

static void
copy_prompt(GtkButton *button, gpointer user_data)
{
  ConnectorRow *row = user_data;
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(gtk_widget_get_ancestor(
    GTK_WIDGET(button), ADW_TYPE_PREFERENCES_PAGE));
  PageState *state = page ? state_of(page) : NULL;
  g_autofree gchar *prompt = state ? gh_agents_page_dup_prompt(row->name, state->npub) : NULL;
  if (prompt)
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(button)), prompt);
}

static void
start_chat(GtkButton *button, gpointer user_data)
{
  PageState *state = user_data;
  const gchar *raw = gtk_editable_get_text(GTK_EDITABLE(state->agent_entry));
  if (state->start && !state->start_busy && raw && *raw) {
    gh_agents_page_set_start_busy(state->page, TRUE);
    state->start(raw, GTK_WIDGET(button), state->start_data);
  }
}

static void
entry_changed(GtkEditable *editable, gpointer user_data)
{
  PageState *state = user_data;
  const gchar *raw = gtk_editable_get_text(editable);
  gtk_widget_set_sensitive(GTK_WIDGET(state->start_button),
                           state->npub && raw && *raw && state->start && !state->start_busy);
}

void
gh_agents_page_populate(AdwPreferencesPage *page)
{
  g_return_if_fail(ADW_IS_PREFERENCES_PAGE(page));
  g_return_if_fail(state_of(page) == NULL);
  PageState *state = g_new0(PageState, 1);
  state->page = page;
  state->rows = g_ptr_array_new_with_free_func(connector_row_free);
  g_object_set_data_full(G_OBJECT(page), "gh-agents-page-state", state, page_state_free);

  AdwPreferencesGroup *intro = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(intro, _("About AI Agents"));
  adw_preferences_group_set_description(intro,
    _("An agent is a separate Marmot account. Its messages are end-to-end encrypted like any other group chat."));
  adw_preferences_page_add(page, intro);
  AdwActionRow *warning = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(warning), _("Only connect an agent you run and trust"));
  adw_action_row_set_subtitle(warning, _("It can read every chat you add it to. Your private key is never included in a setup prompt."));
  adw_preferences_group_add(intro, GTK_WIDGET(warning));

  AdwPreferencesGroup *connectors = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(connectors, _("Connectors"));
  adw_preferences_group_set_description(connectors,
    _("Copy a setup prompt into the agent. It includes your public npub and asks for approval before installation."));
  adw_preferences_page_add(page, connectors);
  g_autoptr(GKeyFile) file = load_connectors();
  static const gchar *names[] = { "Hermes", "OpenClaw", "OpenCode", "Codex", "Claude Code", "Pi" };
  for (guint i = 0; file && i < G_N_ELEMENTS(names); i++) {
    g_autofree gchar *subtitle = g_key_file_get_string(file, names[i], "subtitle", NULL);
    g_autofree gchar *docs = g_key_file_get_string(file, names[i], "docs", NULL);
    ConnectorRow *row = g_new0(ConnectorRow, 1);
    row->name = g_strdup(names[i]);
    AdwExpanderRow *expander = ADW_EXPANDER_ROW(adw_expander_row_new());
    row->row = GTK_WIDGET(expander);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(expander), names[i]);
    adw_expander_row_set_subtitle(expander, subtitle ? subtitle : "");
    GtkButton *copy = GTK_BUTTON(gtk_button_new_with_label(_("Copy Prompt")));
    gtk_widget_set_valign(GTK_WIDGET(copy), GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(GTK_WIDGET(copy), _("Copy setup prompt for this agent"));
    adw_expander_row_add_suffix(expander, GTK_WIDGET(copy));
    g_signal_connect(copy, "clicked", G_CALLBACK(copy_prompt), row);
    GtkLabel *prompt = GTK_LABEL(gtk_label_new(NULL));
    row->prompt_label = prompt;
    gtk_label_set_wrap(prompt, TRUE);
    gtk_label_set_selectable(prompt, TRUE);
    gtk_label_set_xalign(prompt, 0.0f);
    gtk_widget_set_margin_start(GTK_WIDGET(prompt), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(prompt), 12);
    gtk_widget_set_margin_bottom(GTK_WIDGET(prompt), 12);
    adw_expander_row_add_row(expander, GTK_WIDGET(prompt));
    if (docs) {
      GtkWidget *link = gtk_link_button_new_with_label(docs, _("Connector documentation"));
      gtk_widget_set_halign(link, GTK_ALIGN_START);
      gtk_widget_set_margin_start(link, 12);
      gtk_widget_set_margin_bottom(link, 12);
      adw_expander_row_add_row(expander, link);
    }
    adw_preferences_group_add(connectors, GTK_WIDGET(expander));
    g_ptr_array_add(state->rows, row);
  }

  AdwPreferencesGroup *chat = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(chat, _("Start chat with agent"));
  adw_preferences_group_set_description(chat,
    _("Paste the agent's npub, nprofile, or nostr: QR text. Creating the chat uses Marmot and your configured write relays."));
  adw_preferences_page_add(page, chat);
  state->agent_entry = ADW_ENTRY_ROW(adw_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(state->agent_entry), _("Agent npub or QR text"));
  adw_preferences_group_add(chat, GTK_WIDGET(state->agent_entry));
  state->start_button = GTK_BUTTON(gtk_button_new_with_label(_("Start Marmot Chat")));
  gtk_widget_set_sensitive(GTK_WIDGET(state->start_button), FALSE);
  gtk_widget_set_halign(GTK_WIDGET(state->start_button), GTK_ALIGN_END);
  gtk_widget_set_margin_top(GTK_WIDGET(state->start_button), 12);
  adw_preferences_group_add(chat, GTK_WIDGET(state->start_button));
  g_signal_connect(state->start_button, "clicked", G_CALLBACK(start_chat), state);
  g_signal_connect(state->agent_entry, "changed", G_CALLBACK(entry_changed), state);
}

AdwPreferencesPage *
gh_agents_page_new(void)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
  adw_preferences_page_set_name(page, "agents");
  adw_preferences_page_set_title(page, _("AI Agents"));
  adw_preferences_page_set_icon_name(page, "computer-symbolic");
  gh_agents_page_populate(page);
  return page;
}

void
gh_agents_page_set_account(AdwPreferencesPage *page, const gchar *npub)
{
  g_return_if_fail(ADW_IS_PREFERENCES_PAGE(page));
  PageState *state = state_of(page);
  if (!state)
    return;
  gboolean changed = g_strcmp0(state->npub, npub) != 0;
  g_free(state->npub);
  state->npub = npub && *npub ? g_strdup(npub) : NULL;
  if (changed) {
    state->start_busy = FALSE;
    gtk_editable_set_text(GTK_EDITABLE(state->agent_entry), "");
  }
  for (guint i = 0; i < state->rows->len; i++) {
    ConnectorRow *row = g_ptr_array_index(state->rows, i);
    g_autofree gchar *prompt = gh_agents_page_dup_prompt(row->name, state->npub);
    gtk_widget_set_sensitive(row->row, prompt != NULL);
    gtk_label_set_text(row->prompt_label, prompt ? prompt : _("Select an account to copy a prompt."));
  }
  entry_changed(GTK_EDITABLE(state->agent_entry), state);
}

void
gh_agents_page_set_start_busy(AdwPreferencesPage *page, gboolean busy)
{
  g_return_if_fail(ADW_IS_PREFERENCES_PAGE(page));
  PageState *state = state_of(page);
  if (!state)
    return;
  state->start_busy = busy;
  entry_changed(GTK_EDITABLE(state->agent_entry), state);
}

void
gh_agents_page_set_start_func(AdwPreferencesPage *page,
                                   GhAgentsStartFunc start, gpointer user_data)
{
  g_return_if_fail(ADW_IS_PREFERENCES_PAGE(page));
  PageState *state = state_of(page);
  if (!state)
    return;
  state->start = start;
  state->start_data = user_data;
  entry_changed(GTK_EDITABLE(state->agent_entry), state);
}
