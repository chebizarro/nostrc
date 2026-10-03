#include "gh-create-poll-dialog.h"

#include <glib/gi18n.h>

#define MAX_DYNAMIC_OPTIONS 8  /* options 2..9 (first two are in the template) */

struct _GhCreatePollDialog {
  AdwDialog parent_instance;

  /* Template children. */
  AdwEntryRow *question_row;
  AdwEntryRow *option_row_0;
  AdwEntryRow *option_row_1;
  AdwPreferencesGroup *options_group;
  GtkButton  *add_option_button;
  GtkButton  *create_button;
  AdwComboRow *poll_type_row;
  AdwSwitchRow *has_end_time_row;
  AdwSpinRow *duration_hours_row;

  /* Dynamic option rows (added by "Add Option"). */
  GPtrArray  *extra_rows;  /* AdwEntryRow* */
};

enum {
  SIG_POLL_CREATED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhCreatePollDialog, gh_create_poll_dialog, ADW_TYPE_DIALOG)

/* ---- validation --------------------------------------------------------- */

static gboolean
is_valid(GhCreatePollDialog *self)
{
  const gchar *q = gtk_editable_get_text(GTK_EDITABLE(self->question_row));
  /* F4: the g_strdup was leaked on every keystroke. */
  if (!q || !*q) return FALSE;
  g_autofree gchar *stripped = g_strdup(q);
  g_strstrip(stripped);
  if (!*stripped) return FALSE;

  /* Count non-empty options. */
  guint filled = 0;
  const gchar *t0 = gtk_editable_get_text(GTK_EDITABLE(self->option_row_0));
  const gchar *t1 = gtk_editable_get_text(GTK_EDITABLE(self->option_row_1));
  if (t0 && *t0) filled++;
  if (t1 && *t1) filled++;
  for (guint i = 0; i < self->extra_rows->len; i++) {
    AdwEntryRow *row = g_ptr_array_index(self->extra_rows, i);
    const gchar *t = gtk_editable_get_text(GTK_EDITABLE(row));
    if (t && *t) filled++;
  }
  return filled >= GH_MLS_POLL_MIN_OPTIONS;
}

static void
update_sensitivity(GhCreatePollDialog *self)
{
  gtk_widget_set_sensitive(GTK_WIDGET(self->create_button), is_valid(self));
  guint total = 2 + self->extra_rows->len;
  gtk_widget_set_visible(GTK_WIDGET(self->add_option_button),
                         total < GH_MLS_POLL_MAX_OPTIONS);
}

static void
on_text_changed(GtkEditable *editable, GParamSpec *pspec, GhCreatePollDialog *self)
{
  (void) editable;
  (void) pspec;
  update_sensitivity(self);
}

/* ---- actions ------------------------------------------------------------ */

static void
on_add_option(GtkButton *btn, GhCreatePollDialog *self)
{
  (void) btn;
  guint total = 2 + self->extra_rows->len;
  if (total >= GH_MLS_POLL_MAX_OPTIONS) return;

  g_autofree gchar *title = g_strdup_printf(_("Option %u"), total + 1);
  AdwEntryRow *row = ADW_ENTRY_ROW(adw_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  g_signal_connect(row, "notify::text", G_CALLBACK(on_text_changed), self);
  adw_preferences_group_add(self->options_group, GTK_WIDGET(row));
  g_ptr_array_add(self->extra_rows, row);
  update_sensitivity(self);
}

static void
on_end_time_toggled(GObject *obj, GParamSpec *pspec, GhCreatePollDialog *self)
{
  (void) obj;
  (void) pspec;
  gboolean on = adw_switch_row_get_active(self->has_end_time_row);
  gtk_widget_set_visible(GTK_WIDGET(self->duration_hours_row), on);
}

static void
on_create(GtkButton *btn, GhCreatePollDialog *self)
{
  (void) btn;
  if (!is_valid(self)) return;

  const gchar *question = gtk_editable_get_text(GTK_EDITABLE(self->question_row));

  /* Collect non-empty option labels. */
  g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);
  const gchar *base_opts[] = {
    gtk_editable_get_text(GTK_EDITABLE(self->option_row_0)),
    gtk_editable_get_text(GTK_EDITABLE(self->option_row_1)),
  };
  for (guint i = 0; i < G_N_ELEMENTS(base_opts); i++)
    if (base_opts[i] && *base_opts[i])
      g_ptr_array_add(labels, g_strdup(base_opts[i]));
  for (guint i = 0; i < self->extra_rows->len; i++) {
    AdwEntryRow *row = g_ptr_array_index(self->extra_rows, i);
    const gchar *t = gtk_editable_get_text(GTK_EDITABLE(row));
    if (t && *t)
      g_ptr_array_add(labels, g_strdup(t));
  }

  guint n_options = labels->len;
  g_ptr_array_add(labels, NULL); /* NULL-terminate */
  const gchar **option_labels = (const gchar **) labels->pdata;

  GhMlsPollType poll_type = adw_combo_row_get_selected(self->poll_type_row) == 1
                              ? GH_MLS_POLL_MULTIPLE_CHOICE : GH_MLS_POLL_SINGLE_CHOICE;

  gint64 ends_at = 0;
  if (adw_switch_row_get_active(self->has_end_time_row)) {
    gdouble hours = adw_spin_row_get_value(self->duration_hours_row);
    ends_at = (gint64)(g_get_real_time() / G_USEC_PER_SEC) + (gint64)(hours * 3600);
  }

  g_signal_emit(self, signals[SIG_POLL_CREATED], 0,
                question, option_labels, n_options, poll_type, ends_at);
  adw_dialog_close(ADW_DIALOG(self));
}

/* ---- GObject ------------------------------------------------------------ */

static void
gh_create_poll_dialog_dispose(GObject *obj)
{
  gtk_widget_dispose_template(GTK_WIDGET(obj), GH_TYPE_CREATE_POLL_DIALOG);
  G_OBJECT_CLASS(gh_create_poll_dialog_parent_class)->dispose(obj);
}

static void
gh_create_poll_dialog_finalize(GObject *obj)
{
  GhCreatePollDialog *self = GH_CREATE_POLL_DIALOG(obj);
  g_clear_pointer(&self->extra_rows, g_ptr_array_unref);
  G_OBJECT_CLASS(gh_create_poll_dialog_parent_class)->finalize(obj);
}

static void
gh_create_poll_dialog_class_init(GhCreatePollDialogClass *klass)
{
  GObjectClass *obj_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  obj_class->dispose = gh_create_poll_dialog_dispose;
  obj_class->finalize = gh_create_poll_dialog_finalize;

  signals[SIG_POLL_CREATED] =
    g_signal_new("poll-created", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL, G_TYPE_NONE, 5,
                 G_TYPE_STRING,    /* question */
                 G_TYPE_POINTER,   /* option_labels (const gchar **) */
                 G_TYPE_UINT,      /* n_options */
                 G_TYPE_INT,       /* poll_type (GhMlsPollType) */
                 G_TYPE_INT64);    /* ends_at */

  gtk_widget_class_set_template_from_resource(widget_class,
    "/org/nostr/Groundhog/ui/gh-create-poll-dialog.ui");
#define BIND(name) gtk_widget_class_bind_template_child(widget_class, GhCreatePollDialog, name)
  BIND(question_row);
  BIND(option_row_0);
  BIND(option_row_1);
  BIND(options_group);
  BIND(add_option_button);
  BIND(create_button);
  BIND(poll_type_row);
  BIND(has_end_time_row);
  BIND(duration_hours_row);
#undef BIND
}

static void
gh_create_poll_dialog_init(GhCreatePollDialog *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  self->extra_rows = g_ptr_array_new();

  g_signal_connect(self->question_row, "notify::text", G_CALLBACK(on_text_changed), self);
  g_signal_connect(self->option_row_0, "notify::text", G_CALLBACK(on_text_changed), self);
  g_signal_connect(self->option_row_1, "notify::text", G_CALLBACK(on_text_changed), self);
  g_signal_connect(self->add_option_button, "clicked", G_CALLBACK(on_add_option), self);
  g_signal_connect(self->create_button, "clicked", G_CALLBACK(on_create), self);
  g_signal_connect(self->has_end_time_row, "notify::active",
                   G_CALLBACK(on_end_time_toggled), self);

  update_sensitivity(self);
}

/* ---- public ------------------------------------------------------------- */

GtkWidget *
gh_create_poll_dialog_new(void)
{
  return g_object_new(GH_TYPE_CREATE_POLL_DIALOG, NULL);
}
