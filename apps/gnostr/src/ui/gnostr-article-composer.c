/*
 * gnostr-article-composer.c - NIP-23 Article Composer (nostrc-zwn4)
 *
 * Widget for creating kind 30023 long-form articles with markdown editor,
 * preview toggle, and NIP-23 metadata fields.
 */

#define G_LOG_DOMAIN "gnostr-article-composer"

#include "gnostr-article-composer.h"
#include "../util/markdown_pango.h"
#include <nostr-gtk-1.0/content_renderer.h>
#include <adwaita.h>
#include <glib/gi18n.h>

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-article-composer.ui"

struct _GnostrArticleComposer {
  GtkWidget parent_instance;

  /* Template children */
  GtkWidget *root_box;
  AdwEntryRow *entry_title;
  AdwEntryRow *entry_summary;
  AdwEntryRow *entry_image;
  AdwEntryRow *entry_hashtags;
  AdwEntryRow *entry_d_tag;

  GtkToggleButton *btn_preview;
  GtkStack *editor_stack;
  GtkTextView *text_editor;
  GtkLabel *lbl_preview;

  GtkWidget *btn_draft;
  GtkWidget *btn_publish;

  /* D-tag auto-generation tracking */
  gboolean d_tag_manually_edited;
};

G_DEFINE_FINAL_TYPE(GnostrArticleComposer, gnostr_article_composer, GTK_TYPE_WIDGET)

enum {
  SIGNAL_PUBLISH_REQUESTED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

/* ---- Helpers ---- */

static char *slugify(const char *text) {
  if (!text || !*text) return g_strdup("");

  GString *slug = g_string_new(NULL);
  gboolean prev_was_dash = TRUE;

  for (const char *p = text; *p; p++) {
    char c = g_ascii_tolower(*p);
    if (g_ascii_isalnum(c)) {
      g_string_append_c(slug, c);
      prev_was_dash = FALSE;
    } else if (!prev_was_dash && (*p == ' ' || *p == '-' || *p == '_')) {
      g_string_append_c(slug, '-');
      prev_was_dash = TRUE;
    }
  }

  /* Trim trailing dash */
  if (slug->len > 0 && slug->str[slug->len - 1] == '-')
    g_string_truncate(slug, slug->len - 1);

  /* Limit length */
  if (slug->len > 80)
    g_string_truncate(slug, 80);

  return g_string_free(slug, FALSE);
}

/* ---- Signal handlers ---- */

static void on_preview_toggled(GtkToggleButton *btn, gpointer user_data) {
  GnostrArticleComposer *self = GNOSTR_ARTICLE_COMPOSER(user_data);

  if (gtk_toggle_button_get_active(btn)) {
    /* Switch to preview: render markdown */
    GtkTextBuffer *buf = gtk_text_view_get_buffer(self->text_editor);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    char *text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);

    if (text && *text) {
      char *pango = markdown_to_pango(text, 0);
      if (pango) {
        /* nostrc-csaf: Use safe markup setter for consistency - users may paste
         * relay content into the composer which could contain malformed markup */
        gnostr_safe_set_markup(GTK_LABEL(self->lbl_preview), pango);
        g_free(pango);
      } else {
        gtk_label_set_text(GTK_LABEL(self->lbl_preview), text);
      }
    } else {
      gtk_label_set_text(GTK_LABEL(self->lbl_preview), "(empty)");
    }
    g_free(text);

    gtk_stack_set_visible_child_name(self->editor_stack, "preview");
  } else {
    gtk_stack_set_visible_child_name(self->editor_stack, "edit");
  }
}

static void on_title_changed(GtkEditable *editable, gpointer user_data) {
  GnostrArticleComposer *self = GNOSTR_ARTICLE_COMPOSER(user_data);

  if (self->d_tag_manually_edited) return;

  const char *title = gtk_editable_get_text(editable);
  char *slug = slugify(title);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_d_tag), slug);
  g_free(slug);
}

static void on_d_tag_changed(GtkEditable *editable, gpointer user_data) {
  GnostrArticleComposer *self = GNOSTR_ARTICLE_COMPOSER(user_data);
  (void)editable;

  /* If the d-tag was changed while the title signal was NOT firing,
   * mark it as manually edited. We detect this by checking focus. */
  if (gtk_widget_has_focus(GTK_WIDGET(self->entry_d_tag)))
    self->d_tag_manually_edited = TRUE;
}

static void on_publish_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrArticleComposer *self = GNOSTR_ARTICLE_COMPOSER(user_data);
  g_signal_emit(self, signals[SIGNAL_PUBLISH_REQUESTED], 0, FALSE);
}

static void on_draft_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrArticleComposer *self = GNOSTR_ARTICLE_COMPOSER(user_data);
  g_signal_emit(self, signals[SIGNAL_PUBLISH_REQUESTED], 0, TRUE);
}

/* ---- GObject boilerplate ---- */

static void gnostr_article_composer_dispose(GObject *object) {
  GnostrArticleComposer *self = GNOSTR_ARTICLE_COMPOSER(object);
  gtk_widget_set_layout_manager(GTK_WIDGET(self), NULL);
  g_clear_pointer(&self->root_box, gtk_widget_unparent);
  G_OBJECT_CLASS(gnostr_article_composer_parent_class)->dispose(object);
}

static void gnostr_article_composer_class_init(GnostrArticleComposerClass *klass) {
  GObjectClass *obj_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  obj_class->dispose = gnostr_article_composer_dispose;

  signals[SIGNAL_PUBLISH_REQUESTED] = g_signal_new(
      "publish-requested", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
      0, NULL, NULL, g_cclosure_marshal_VOID__BOOLEAN, G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "article-composer");
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);

  gtk_widget_class_bind_template_child(widget_class, GnostrArticleComposer, root_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleComposer, entry_title);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleComposer, entry_summary);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleComposer, entry_image);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleComposer, entry_hashtags);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleComposer, entry_d_tag);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleComposer, btn_preview);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleComposer, editor_stack);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleComposer, text_editor);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleComposer, lbl_preview);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleComposer, btn_draft);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticleComposer, btn_publish);
}

static void gnostr_article_composer_init(GnostrArticleComposer *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  self->d_tag_manually_edited = FALSE;

  g_signal_connect(self->btn_preview, "toggled", G_CALLBACK(on_preview_toggled), self);
  g_signal_connect(self->entry_title, "changed", G_CALLBACK(on_title_changed), self);
  g_signal_connect(self->entry_d_tag, "changed", G_CALLBACK(on_d_tag_changed), self);
  g_signal_connect(self->btn_publish, "clicked", G_CALLBACK(on_publish_clicked), self);
  g_signal_connect(self->btn_draft, "clicked", G_CALLBACK(on_draft_clicked), self);
}

/* ---- Public API ---- */

GtkWidget *gnostr_article_composer_new(void) {
  return g_object_new(GNOSTR_TYPE_ARTICLE_COMPOSER, NULL);
}

const char *gnostr_article_composer_get_title(GnostrArticleComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_ARTICLE_COMPOSER(self), NULL);
  return gtk_editable_get_text(GTK_EDITABLE(self->entry_title));
}

const char *gnostr_article_composer_get_summary(GnostrArticleComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_ARTICLE_COMPOSER(self), NULL);
  return gtk_editable_get_text(GTK_EDITABLE(self->entry_summary));
}

const char *gnostr_article_composer_get_image_url(GnostrArticleComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_ARTICLE_COMPOSER(self), NULL);
  return gtk_editable_get_text(GTK_EDITABLE(self->entry_image));
}

char *gnostr_article_composer_get_content(GnostrArticleComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_ARTICLE_COMPOSER(self), NULL);
  GtkTextBuffer *buf = gtk_text_view_get_buffer(self->text_editor);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buf, &start, &end);
  return gtk_text_buffer_get_text(buf, &start, &end, FALSE);
}

const char *gnostr_article_composer_get_d_tag(GnostrArticleComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_ARTICLE_COMPOSER(self), NULL);
  return gtk_editable_get_text(GTK_EDITABLE(self->entry_d_tag));
}

char **gnostr_article_composer_get_hashtags(GnostrArticleComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_ARTICLE_COMPOSER(self), NULL);
  const char *text = gtk_editable_get_text(GTK_EDITABLE(self->entry_hashtags));
  if (!text || !*text) return NULL;

  char **parts = g_strsplit(text, ",", -1);
  GPtrArray *tags = g_ptr_array_new();

  for (int i = 0; parts[i]; i++) {
    char *trimmed = g_strstrip(g_strdup(parts[i]));
    if (*trimmed)
      g_ptr_array_add(tags, trimmed);
    else
      g_free(trimmed);
  }
  g_strfreev(parts);

  g_ptr_array_add(tags, NULL);
  return (char **)g_ptr_array_free(tags, FALSE);
}
