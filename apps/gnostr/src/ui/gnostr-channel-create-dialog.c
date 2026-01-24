/**
 * GnostrChannelCreateDialog - Dialog for creating a new NIP-28 channel
 */

#include "gnostr-channel-create-dialog.h"

struct _GnostrChannelCreateDialog {
    AdwDialog parent_instance;

    /* Template widgets */
    AdwEntryRow *entry_name;
    AdwEntryRow *entry_about;
    AdwEntryRow *entry_picture;
    GtkButton *btn_cancel;
    GtkButton *btn_create;
    GtkLabel *lbl_title;

    /* Data */
    char *channel_id;  /* Non-NULL when editing existing channel */
    gboolean is_editing;
};

G_DEFINE_TYPE(GnostrChannelCreateDialog, gnostr_channel_create_dialog, ADW_TYPE_DIALOG)

enum {
    SIGNAL_CREATE_CHANNEL,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
gnostr_channel_create_dialog_finalize(GObject *object)
{
    GnostrChannelCreateDialog *self = GNOSTR_CHANNEL_CREATE_DIALOG(object);

    g_free(self->channel_id);

    G_OBJECT_CLASS(gnostr_channel_create_dialog_parent_class)->finalize(object);
}

static void
on_cancel_clicked(GtkButton *button, GnostrChannelCreateDialog *self)
{
    (void)button;
    adw_dialog_close(ADW_DIALOG(self));
}

static void
on_create_clicked(GtkButton *button, GnostrChannelCreateDialog *self)
{
    (void)button;

    const char *name = gtk_editable_get_text(GTK_EDITABLE(self->entry_name));
    if (!name || !*name) {
        /* Name is required */
        gtk_widget_add_css_class(GTK_WIDGET(self->entry_name), "error");
        return;
    }

    GnostrChannel *channel = gnostr_channel_create_dialog_get_channel(self);
    g_signal_emit(self, signals[SIGNAL_CREATE_CHANNEL], 0, channel);
    gnostr_channel_free(channel);

    adw_dialog_close(ADW_DIALOG(self));
}

static void
on_name_changed(GtkEditable *editable, GnostrChannelCreateDialog *self)
{
    (void)editable;
    gtk_widget_remove_css_class(GTK_WIDGET(self->entry_name), "error");

    /* Enable/disable create button based on name */
    const char *name = gtk_editable_get_text(GTK_EDITABLE(self->entry_name));
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_create), name && *name);
}

static void
gnostr_channel_create_dialog_class_init(GnostrChannelCreateDialogClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->finalize = gnostr_channel_create_dialog_finalize;

    /* Load template */
    gtk_widget_class_set_template_from_resource(widget_class,
        "/org/gnostr/ui/ui/dialogs/gnostr-channel-create-dialog.ui");

    /* Bind template children */
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelCreateDialog, entry_name);
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelCreateDialog, entry_about);
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelCreateDialog, entry_picture);
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelCreateDialog, btn_cancel);
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelCreateDialog, btn_create);
    gtk_widget_class_bind_template_child(widget_class, GnostrChannelCreateDialog, lbl_title);

    /* Signals */
    signals[SIGNAL_CREATE_CHANNEL] = g_signal_new(
        "create-channel",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_POINTER);
}

static void
gnostr_channel_create_dialog_init(GnostrChannelCreateDialog *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    self->channel_id = NULL;
    self->is_editing = FALSE;

    /* Connect signals */
    g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel_clicked), self);
    g_signal_connect(self->btn_create, "clicked", G_CALLBACK(on_create_clicked), self);
    g_signal_connect(self->entry_name, "changed", G_CALLBACK(on_name_changed), self);

    /* Initially disable create button */
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_create), FALSE);
}

GnostrChannelCreateDialog *
gnostr_channel_create_dialog_new(void)
{
    return g_object_new(GNOSTR_TYPE_CHANNEL_CREATE_DIALOG, NULL);
}

void
gnostr_channel_create_dialog_present(GnostrChannelCreateDialog *self,
                                      GtkWidget *parent)
{
    g_return_if_fail(GNOSTR_IS_CHANNEL_CREATE_DIALOG(self));

    self->is_editing = FALSE;
    g_free(self->channel_id);
    self->channel_id = NULL;

    gtk_label_set_text(self->lbl_title, "Create Channel");
    gtk_button_set_label(self->btn_create, "Create");

    /* Clear form */
    gtk_editable_set_text(GTK_EDITABLE(self->entry_name), "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_about), "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_picture), "");

    adw_dialog_present(ADW_DIALOG(self), parent);
}

void
gnostr_channel_create_dialog_present_edit(GnostrChannelCreateDialog *self,
                                           GtkWidget *parent,
                                           const GnostrChannel *channel)
{
    g_return_if_fail(GNOSTR_IS_CHANNEL_CREATE_DIALOG(self));
    g_return_if_fail(channel != NULL);

    self->is_editing = TRUE;
    g_free(self->channel_id);
    self->channel_id = g_strdup(channel->channel_id);

    gtk_label_set_text(self->lbl_title, "Edit Channel");
    gtk_button_set_label(self->btn_create, "Save");

    /* Populate form */
    gtk_editable_set_text(GTK_EDITABLE(self->entry_name),
                          channel->name ? channel->name : "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_about),
                          channel->about ? channel->about : "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_picture),
                          channel->picture ? channel->picture : "");

    adw_dialog_present(ADW_DIALOG(self), parent);
}

GnostrChannel *
gnostr_channel_create_dialog_get_channel(GnostrChannelCreateDialog *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHANNEL_CREATE_DIALOG(self), NULL);

    GnostrChannel *channel = gnostr_channel_new();

    channel->channel_id = g_strdup(self->channel_id);

    const char *name = gtk_editable_get_text(GTK_EDITABLE(self->entry_name));
    if (name && *name)
        channel->name = g_strdup(name);

    const char *about = gtk_editable_get_text(GTK_EDITABLE(self->entry_about));
    if (about && *about)
        channel->about = g_strdup(about);

    const char *picture = gtk_editable_get_text(GTK_EDITABLE(self->entry_picture));
    if (picture && *picture)
        channel->picture = g_strdup(picture);

    return channel;
}
