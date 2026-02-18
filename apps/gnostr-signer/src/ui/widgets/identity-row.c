#include "identity-row.h"
#include "../app-resources.h"

struct _IdentityRow {
  AdwActionRow parent_instance;
  GtkButton *btn_select;
};

G_DEFINE_TYPE(IdentityRow, identity_row, ADW_TYPE_ACTION_ROW)

static void identity_row_class_init(IdentityRowClass *klass){
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/widgets/identity-row.ui");
  gtk_widget_class_bind_template_child(wc, IdentityRow, btn_select);
}

static void identity_row_init(IdentityRow *self){
  gtk_widget_init_template(GTK_WIDGET(self));

  /* Ensure row is focusable for keyboard navigation (nostrc-qfdg) */
  gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
}

GtkWidget *identity_row_new(void){ return g_object_new(TYPE_IDENTITY_ROW, NULL); }

/**
 * identity_row_set_identity:
 * @self: an #IdentityRow
 * @label: the display label for this identity
 * @npub: the npub identifier
 * @is_active: whether this is the currently active identity
 *
 * Sets the identity information and updates accessibility labels (nostrc-qfdg).
 */
void identity_row_set_identity(IdentityRow *self, const char *label, const char *npub, gboolean is_active) {
  g_return_if_fail(IS_IDENTITY_ROW(self));

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self), label ? label : "Identity");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(self), npub ? npub : "");

  /* Update accessibility properties for screen readers (nostrc-qfdg) */
  g_autofree gchar *accessible_label = g_strdup_printf("Identity: %s", label ? label : "Unnamed");
  g_autofree gchar *accessible_desc = NULL;

  if (npub) {
    if (is_active) {
      accessible_desc = g_strdup_printf("Public key: %s. This is the currently active signing identity.", npub);
    } else {
      accessible_desc = g_strdup_printf("Public key: %s. Press Enter or click Select to switch to this identity.", npub);
    }
  }

  gtk_accessible_update_property(GTK_ACCESSIBLE(self),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, accessible_label,
                                 -1);
  if (accessible_desc) {
    gtk_accessible_update_property(GTK_ACCESSIBLE(self),
                                   GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, accessible_desc,
                                   -1);
  }

  /* Update state for active identity (nostrc-qfdg) */
  gtk_accessible_update_state(GTK_ACCESSIBLE(self),
                              GTK_ACCESSIBLE_STATE_SELECTED, is_active,
                              -1);

  /* Update select button accessibility */
  if (self->btn_select) {
    if (is_active) {
      gtk_widget_set_sensitive(GTK_WIDGET(self->btn_select), FALSE);
      gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_select),
                                     GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, "This identity is already selected",
                                     -1);
    } else {
      gtk_widget_set_sensitive(GTK_WIDGET(self->btn_select), TRUE);
      g_autofree gchar *btn_desc = g_strdup_printf("Select %s as the active signing identity", label ? label : "this identity");
      gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_select),
                                     GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, btn_desc,
                                     -1);
    }
  }
}
