/* GnNostrReferenceCard (nostrc-8xfib.6): the card row's quote embed
 * (author caption + three-line content, nostr-note-card-row.c
 * set_quote_info) and gnostr's timeline-factory local resolution, behind
 * GnNostrReferenceResolver, with Groundhog's inert, consent-first
 * behaviour. See the header for the contract. */
#include <nostr-gtk-1.0/gn-nostr-reference-card.h>
#include "gn-portable-i18n-private.h"
#include <string.h>

struct _GnNostrReferenceCard {
  GtkBox parent_instance;
  GtkWidget *title, *content, *address, *status, *find;
  GnNostrReferenceResolver *resolver;
  GnNostrReference *reference;
  GnNostrReferenceCardRole role;
  GnNostrReferenceCardState state;
  GnNostrResolvedNote *note;
  GCancellable *cancellable;
  guint64 generation;
};
G_DEFINE_TYPE(GnNostrReferenceCard, gn_nostr_reference_card, GTK_TYPE_BOX)

enum { PROP_0, PROP_STATE, N_PROPS };
static GParamSpec *props[N_PROPS];

static GnNostrReference *reference_copy(const GnNostrReference *r) {
  GnNostrReference *copy = g_new0(GnNostrReference, 1);
  copy->type = r->type;
  copy->uri = g_strdup(r->uri);
  copy->id = g_strdup(r->id);
  copy->author = g_strdup(r->author);
  copy->kind = r->kind;
  copy->relay_hints = g_strdupv(r->relay_hints);
  copy->author_authenticated = r->author_authenticated;
  return copy;
}

static gchar *short_hex(const gchar *value) {
  if (!value) return g_strdup("");
  glong len = g_utf8_strlen(value, -1);
  return g_utf8_substring(value, 0, MIN(12, len));
}

/* What an unresolved reference is, without its id: the card's title. */
static const gchar *unresolved_title(const GnNostrReference *reference,
                                     GnNostrReferenceCardRole role) {
  if (reference->type == GN_NOSTR_REFERENCE_PERSON)
    return _("Nostr profile");
  switch (role) {
  case GN_NOSTR_REFERENCE_CARD_ROLE_QUOTE:
    return _("Quoted Nostr note");
  case GN_NOSTR_REFERENCE_CARD_ROLE_REPOST:
    return _("Reposted Nostr note");
  case GN_NOSTR_REFERENCE_CARD_ROLE_MENTION:
  default:
    return reference->type == GN_NOSTR_REFERENCE_ADDRESS ? _("Nostr address") : _("Nostr note");
  }
}

gchar *gn_nostr_reference_dup_summary(const GnNostrReference *reference,
                                      GnNostrReferenceCardRole role) {
  g_return_val_if_fail(reference != NULL, NULL);
  g_autofree gchar *id = short_hex(reference->type == GN_NOSTR_REFERENCE_PERSON
                                   ? reference->author : reference->id);
  /* TRANSLATORS: an unresolved Nostr reference, e.g. "Quoted Nostr note: 0123456789ab". */
  return g_strdup_printf(_("%s: %s"), unresolved_title(reference, role), id);
}

/* The reference's own address, shortened in the middle as people compare
 * them ("nevent1qqsx3k…h7c2d"); NULL without a URI. */
static gchar *short_address(const GnNostrReference *reference) {
  const gchar *uri = reference->uri;
  if (!uri) return NULL;
  if (g_str_has_prefix(uri, "nostr:")) uri += strlen("nostr:");
  glong len = g_utf8_strlen(uri, -1);
  if (len <= 24) return g_strdup(uri);
  g_autofree gchar *head = g_utf8_substring(uri, 0, 14);
  g_autofree gchar *tail = g_utf8_substring(uri, len - 6, len);
  return g_strconcat(head, "…", tail, NULL);
}

/* Labels show only with text, always valid UTF-8 (as GnOgPreviewCard). */
static void set_text(GtkWidget *label, const gchar *text) {
  g_autofree gchar *valid = text ? g_utf8_make_valid(text, -1) : NULL;
  gtk_label_set_text(GTK_LABEL(label), valid ? valid : "");
  gtk_widget_set_visible(label, valid && *valid);
}

static gchar *resolved_title(GnNostrReferenceCard *self, const gchar *author) {
  switch (self->role) {
  case GN_NOSTR_REFERENCE_CARD_ROLE_QUOTE:
    return g_strdup_printf(_("Quoting %s"), author);
  case GN_NOSTR_REFERENCE_CARD_ROLE_REPOST:
    return g_strdup_printf(_("Reposted from %s"), author);
  case GN_NOSTR_REFERENCE_CARD_ROLE_MENTION:
  default:
    return g_strdup_printf(_("Note by %s"), author);
  }
}

static gboolean fetchable(GnNostrReferenceCard *self) {
  return self->reference && self->resolver &&
    (self->reference->type == GN_NOSTR_REFERENCE_EVENT ||
     self->reference->type == GN_NOSTR_REFERENCE_ADDRESS) &&
    gn_nostr_reference_resolver_can_fetch(self->resolver);
}

static void update(GnNostrReferenceCard *self, const gchar *preview_author,
                   const gchar *preview_content) {
  g_autofree gchar *title = NULL, *address = NULL;
  const gchar *content = NULL, *status = NULL;
  gboolean unresolved = self->state == GN_NOSTR_REFERENCE_CARD_INERT ||
                        self->state == GN_NOSTR_REFERENCE_CARD_FETCHING ||
                        self->state == GN_NOSTR_REFERENCE_CARD_UNAVAILABLE;
  gboolean find = FALSE;
  if (unresolved) {
    /* Unresolved: what it is, its shortened address, then one thing to do
     * (Find on Relays…) or, when nothing can be fetched, a muted note. */
    title = g_strdup(unresolved_title(self->reference, self->role));
    address = short_address(self->reference);
    find = self->state != GN_NOSTR_REFERENCE_CARD_FETCHING && fetchable(self);
  }
  switch (self->state) {
  case GN_NOSTR_REFERENCE_CARD_RESOLVED:
    if (self->note) {
      g_autofree gchar *fallback = short_hex(self->note->event->pubkey);
      const gchar *name = self->note->author_name && *self->note->author_name
                          ? self->note->author_name : fallback;
      title = resolved_title(self, name);
      content = self->note->event->content;
    } else {
      title = resolved_title(self, preview_author && *preview_author
                                   ? preview_author : _("Unknown"));
      content = preview_content && *preview_content
                ? preview_content : _("(content unavailable)");
    }
    break;
  case GN_NOSTR_REFERENCE_CARD_FETCHING:
    status = _("Finding on relays…");
    break;
  case GN_NOSTR_REFERENCE_CARD_UNAVAILABLE:
    status = _("No matching signed note was found.");
    break;
  case GN_NOSTR_REFERENCE_CARD_INERT:
    if (!find) status = _("Not available on this device");
    break;
  case GN_NOSTR_REFERENCE_CARD_EMPTY:
  default:
    break;
  }
  set_text(self->title, title);
  set_text(self->content, content);
  set_text(self->address, address);
  gtk_widget_set_tooltip_text(self->address, address && self->reference
                              ? self->reference->uri : NULL);
  set_text(self->status, status);
  gtk_widget_set_visible(self->find, find);
  gtk_widget_set_visible(GTK_WIDGET(self), self->state != GN_NOSTR_REFERENCE_CARD_EMPTY);
  if (unresolved)
    gtk_widget_add_css_class(GTK_WIDGET(self), "unresolved");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(self), "unresolved");
  g_autofree gchar *summary = unresolved ? gn_nostr_reference_dup_summary(self->reference, self->role)
                                         : NULL;
  const gchar *heading = summary ? summary : title;
  const gchar *detail = content && *content ? content : status;
  g_autofree gchar *label = detail && *detail
    ? g_strdup_printf("%s: %s", heading, detail) : g_strdup(heading ? heading : "");
  gtk_accessible_update_property(GTK_ACCESSIBLE(self),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

static void set_state(GnNostrReferenceCard *self, GnNostrReferenceCardState state) {
  gboolean changed = self->state != state;
  self->state = state;
  update(self, NULL, NULL);
  if (changed) g_object_notify_by_pspec(G_OBJECT(self), props[PROP_STATE]);
}

static void cancel_fetch(GnNostrReferenceCard *self) {
  self->generation++;
  if (self->cancellable) g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
}

void gn_nostr_reference_card_set_resolver(GnNostrReferenceCard *self,
                                          GnNostrReferenceResolver *resolver) {
  g_return_if_fail(GN_IS_NOSTR_REFERENCE_CARD(self));
  g_return_if_fail(!resolver || GN_IS_NOSTR_REFERENCE_RESOLVER(resolver));
  if (self->resolver == resolver) return;
  cancel_fetch(self);
  g_set_object(&self->resolver, resolver);
  if (self->state == GN_NOSTR_REFERENCE_CARD_FETCHING)
    self->state = GN_NOSTR_REFERENCE_CARD_INERT;
  update(self, NULL, NULL);
}

void gn_nostr_reference_card_set_reference(GnNostrReferenceCard *self,
                                           const GnNostrReference *target,
                                           GnNostrReferenceCardRole role,
                                           const gchar *verified_json) {
  g_return_if_fail(GN_IS_NOSTR_REFERENCE_CARD(self));
  gboolean same = target && self->reference && self->role == role &&
                  g_strcmp0(target->uri, self->reference->uri) == 0;
  /* Rebinding the same reference keeps an in-flight explicit fetch. */
  if (same && self->state == GN_NOSTR_REFERENCE_CARD_FETCHING) return;
  GnNostrReferenceCardState previous = same ? self->state : GN_NOSTR_REFERENCE_CARD_EMPTY;
  cancel_fetch(self);
  g_clear_pointer(&self->note, gn_nostr_resolved_note_free);
  g_clear_pointer(&self->reference, gn_nostr_reference_free);
  self->role = role;
  if (!target) { set_state(self, GN_NOSTR_REFERENCE_CARD_EMPTY); return; }
  self->reference = reference_copy(target);
  /* Local only: an embedded original, then the resolver's own storage. */
  if (verified_json)
    self->note = gn_nostr_resolved_note_new(self->reference, verified_json, self->resolver);
  if (!self->note && self->resolver)
    self->note = gn_nostr_reference_resolver_resolve_local(self->resolver, self->reference);
  set_state(self, self->note ? GN_NOSTR_REFERENCE_CARD_RESOLVED
                 : previous == GN_NOSTR_REFERENCE_CARD_UNAVAILABLE
                   ? GN_NOSTR_REFERENCE_CARD_UNAVAILABLE : GN_NOSTR_REFERENCE_CARD_INERT);
}

void gn_nostr_reference_card_set_descriptor(GnNostrReferenceCard *self,
                                            const GnNostrRepostDescriptor *descriptor) {
  g_return_if_fail(GN_IS_NOSTR_REFERENCE_CARD(self));
  if (!descriptor || !descriptor->target) {
    gn_nostr_reference_card_set_reference(self, NULL, GN_NOSTR_REFERENCE_CARD_ROLE_MENTION, NULL);
    return;
  }
  gn_nostr_reference_card_set_reference(self, descriptor->target,
    descriptor->quote ? GN_NOSTR_REFERENCE_CARD_ROLE_QUOTE : GN_NOSTR_REFERENCE_CARD_ROLE_REPOST,
    descriptor->original_json);
}

void gn_nostr_reference_card_set_preview(GnNostrReferenceCard *self,
                                         const gchar *author_name,
                                         const gchar *content) {
  g_return_if_fail(GN_IS_NOSTR_REFERENCE_CARD(self));
  cancel_fetch(self);
  g_clear_pointer(&self->note, gn_nostr_resolved_note_free);
  gboolean changed = self->state != GN_NOSTR_REFERENCE_CARD_RESOLVED;
  self->state = GN_NOSTR_REFERENCE_CARD_RESOLVED;
  update(self, author_name, content);
  if (changed) g_object_notify_by_pspec(G_OBJECT(self), props[PROP_STATE]);
}

typedef struct {
  GWeakRef card;
  guint64 generation;
  GnNostrReference *reference;
} FetchData;

static void fetch_data_free(FetchData *data) {
  g_weak_ref_clear(&data->card);
  gn_nostr_reference_free(data->reference);
  g_free(data);
}

static void on_fetched(GObject *source, GAsyncResult *result, gpointer user_data) {
  FetchData *data = user_data;
  GnNostrReferenceResolver *resolver = GN_NOSTR_REFERENCE_RESOLVER(source);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *json = gn_nostr_reference_resolver_fetch_finish(resolver, result, &error);
  g_autoptr(GnNostrReferenceCard) self = g_weak_ref_get(&data->card);
  if (self && self->generation == data->generation &&
      self->state == GN_NOSTR_REFERENCE_CARD_FETCHING) {
    g_clear_object(&self->cancellable);
    self->note = json ? gn_nostr_resolved_note_new(self->reference, json, resolver) : NULL;
    if (self->note)
      set_state(self, GN_NOSTR_REFERENCE_CARD_RESOLVED);
    else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      set_state(self, GN_NOSTR_REFERENCE_CARD_INERT); /* declined: nothing asked */
    else
      set_state(self, GN_NOSTR_REFERENCE_CARD_UNAVAILABLE);
  }
  fetch_data_free(data);
}

gboolean gn_nostr_reference_card_fetch(GnNostrReferenceCard *self) {
  g_return_val_if_fail(GN_IS_NOSTR_REFERENCE_CARD(self), FALSE);
  if ((self->state != GN_NOSTR_REFERENCE_CARD_INERT &&
       self->state != GN_NOSTR_REFERENCE_CARD_UNAVAILABLE) || !fetchable(self))
    return FALSE;
  cancel_fetch(self);
  self->cancellable = g_cancellable_new();
  FetchData *data = g_new0(FetchData, 1);
  g_weak_ref_init(&data->card, self);
  data->generation = self->generation;
  data->reference = reference_copy(self->reference);
  set_state(self, GN_NOSTR_REFERENCE_CARD_FETCHING);
  gn_nostr_reference_resolver_fetch_async(self->resolver, data->reference,
                                          self->cancellable, on_fetched, data);
  return TRUE;
}

GnNostrReferenceCardState gn_nostr_reference_card_get_state(GnNostrReferenceCard *self) {
  g_return_val_if_fail(GN_IS_NOSTR_REFERENCE_CARD(self), GN_NOSTR_REFERENCE_CARD_EMPTY);
  return self->state;
}

const GnNostrReference *gn_nostr_reference_card_get_reference(GnNostrReferenceCard *self) {
  g_return_val_if_fail(GN_IS_NOSTR_REFERENCE_CARD(self), NULL);
  return self->reference;
}

const GnNostrResolvedNote *gn_nostr_reference_card_get_note(GnNostrReferenceCard *self) {
  g_return_val_if_fail(GN_IS_NOSTR_REFERENCE_CARD(self), NULL);
  return self->note;
}

static void on_find_clicked(GtkButton *button, GnNostrReferenceCard *self) {
  (void)button;
  gn_nostr_reference_card_fetch(self);
}

static GtkWidget *new_label(const gchar *name, gint lines) {
  GtkWidget *label = gtk_label_new(NULL);
  gtk_widget_set_name(label, name);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
  if (lines > 0) {
    gtk_label_set_lines(GTK_LABEL(label), lines);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  }
  gtk_widget_set_visible(label, FALSE);
  return label;
}

static void gn_nostr_reference_card_get_property(GObject *object, guint id,
                                                 GValue *value, GParamSpec *pspec) {
  GnNostrReferenceCard *self = GN_NOSTR_REFERENCE_CARD(object);
  if (id == PROP_STATE) g_value_set_int(value, self->state);
  else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void gn_nostr_reference_card_dispose(GObject *object) {
  GnNostrReferenceCard *self = GN_NOSTR_REFERENCE_CARD(object);
  cancel_fetch(self);
  g_clear_object(&self->resolver);
  G_OBJECT_CLASS(gn_nostr_reference_card_parent_class)->dispose(object);
}

static void gn_nostr_reference_card_finalize(GObject *object) {
  GnNostrReferenceCard *self = GN_NOSTR_REFERENCE_CARD(object);
  gn_nostr_resolved_note_free(self->note);
  gn_nostr_reference_free(self->reference);
  G_OBJECT_CLASS(gn_nostr_reference_card_parent_class)->finalize(object);
}

static void gn_nostr_reference_card_class_init(GnNostrReferenceCardClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = gn_nostr_reference_card_get_property;
  object_class->dispose = gn_nostr_reference_card_dispose;
  object_class->finalize = gn_nostr_reference_card_finalize;
  /* A GnNostrReferenceCardState value. */
  props[PROP_STATE] = g_param_spec_int("state", NULL, NULL,
    GN_NOSTR_REFERENCE_CARD_EMPTY, GN_NOSTR_REFERENCE_CARD_UNAVAILABLE,
    GN_NOSTR_REFERENCE_CARD_EMPTY, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  g_object_class_install_properties(object_class, N_PROPS, props);
  gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(klass), "gn-nostr-reference-card");
  gtk_widget_class_set_accessible_role(GTK_WIDGET_CLASS(klass), GTK_ACCESSIBLE_ROLE_GROUP);
}

static void gn_nostr_reference_card_init(GnNostrReferenceCard *self) {
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing(GTK_BOX(self), 4);
  self->title = new_label("reference-title", 0);
  gtk_widget_add_css_class(self->title, "caption");
  gtk_widget_add_css_class(self->title, "dim-label");
  self->content = new_label("reference-content", 3);
  /* The unresolved note's address: selectable, so it can be copied. */
  self->address = new_label("reference-address", 1);
  gtk_label_set_ellipsize(GTK_LABEL(self->address), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_selectable(GTK_LABEL(self->address), TRUE);
  gtk_widget_add_css_class(self->address, "monospace");
  gtk_widget_add_css_class(self->address, "caption");
  gtk_widget_add_css_class(self->address, "dim-label");
  self->status = new_label("reference-status", 0);
  gtk_widget_add_css_class(self->status, "dim-label");
  self->find = gtk_button_new_with_label(_("Find on Relays…"));
  gtk_widget_set_name(self->find, "reference-find");
  gtk_widget_set_halign(self->find, GTK_ALIGN_START);
  gtk_widget_set_margin_top(self->find, 2);

  gtk_accessible_update_property(GTK_ACCESSIBLE(self->find),
    GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
    _("Asks before any relay is contacted"), -1);
  gtk_widget_set_visible(self->find, FALSE);
  g_signal_connect(self->find, "clicked", G_CALLBACK(on_find_clicked), self);
  gtk_box_append(GTK_BOX(self), self->title);
  gtk_box_append(GTK_BOX(self), self->content);
  gtk_box_append(GTK_BOX(self), self->address);
  gtk_box_append(GTK_BOX(self), self->status);
  gtk_box_append(GTK_BOX(self), self->find);
  self->state = GN_NOSTR_REFERENCE_CARD_EMPTY;
  gtk_widget_set_visible(GTK_WIDGET(self), FALSE);
}

GtkWidget *gn_nostr_reference_card_new(void) {
  return g_object_new(GN_TYPE_NOSTR_REFERENCE_CARD, NULL);
}
