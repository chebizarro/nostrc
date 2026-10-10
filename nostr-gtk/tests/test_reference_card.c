/* GnNostrReferenceResolver + GnNostrReferenceCard (nostrc-8xfib.6): local
 * lookup only on bind/map, fetch only on the explicit Find on Relays, and
 * only verified, matching events are shown. */
#include <nostr-gtk-1.0/gn-nostr-reference-card.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-tag.h>
#include <string.h>
#include "nostrc-test-gdk-frame.h"

#define SK "7f7ff03d123792d6ac594bfa67bf6d0c0ab55b6b1fdb6249303fe861f1ccba9a"

static char *signed_event(int kind, const char *content, NostrTags *tags) {
  NostrEvent *ev = nostr_event_new();
  nostr_event_set_kind(ev, kind);
  nostr_event_set_created_at(ev, 1700000000);
  nostr_event_set_content(ev, content);
  nostr_event_set_tags(ev, tags ? tags : nostr_tags_new(0));
  g_assert_cmpint(nostr_event_sign(ev, SK), ==, 0);
  char *json = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);
  return json;
}

static GnNostrReference *reference_for(const char *json) {
  g_autoptr(GnNostrEventInfo) info = gn_nostr_event_parse(json, NULL);
  g_assert_nonnull(info);
  g_autofree gchar *uri = gn_nostr_reference_build_event(info->id, NULL, -1, NULL);
  return gn_nostr_reference_parse(uri);
}

/* ---- fake resolver ---- */
#define FAKE_TYPE_RESOLVER (fake_resolver_get_type())
G_DECLARE_FINAL_TYPE(FakeResolver, fake_resolver, FAKE, RESOLVER, GObject)
struct _FakeResolver {
  GObject parent_instance;
  gchar *local_json;
  gboolean can_fetch;
  guint lookups, fetches;
  GTask *pending;
};
static void fake_iface_init(GnNostrReferenceResolverInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(FakeResolver, fake_resolver, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GN_TYPE_NOSTR_REFERENCE_RESOLVER, fake_iface_init))

static gchar *fake_lookup(GnNostrReferenceResolver *r, const GnNostrReference *ref) {
  (void)ref;
  FAKE_RESOLVER(r)->lookups++;
  return g_strdup(FAKE_RESOLVER(r)->local_json);
}
static gchar *fake_name(GnNostrReferenceResolver *r, const gchar *pubkey) {
  (void)r; (void)pubkey;
  return g_strdup("Alice");
}
static gboolean fake_can_fetch(GnNostrReferenceResolver *r) { return FAKE_RESOLVER(r)->can_fetch; }
static void fake_fetch_async(GnNostrReferenceResolver *r, const GnNostrReference *ref,
                             GCancellable *c, GAsyncReadyCallback cb, gpointer data) {
  (void)ref;
  FakeResolver *self = FAKE_RESOLVER(r);
  self->fetches++;
  g_clear_object(&self->pending);
  self->pending = g_task_new(self, c, cb, data);
}
static gchar *fake_fetch_finish(GnNostrReferenceResolver *r, GAsyncResult *res, GError **error) {
  (void)r;
  return g_task_propagate_pointer(G_TASK(res), error);
}
static void fake_iface_init(GnNostrReferenceResolverInterface *iface) {
  iface->lookup_local = fake_lookup;
  iface->display_name = fake_name;
  iface->can_fetch = fake_can_fetch;
  iface->fetch_async = fake_fetch_async;
  iface->fetch_finish = fake_fetch_finish;
}
static void fake_finalize(GObject *o) {
  g_free(FAKE_RESOLVER(o)->local_json);
  g_clear_object(&FAKE_RESOLVER(o)->pending);
  G_OBJECT_CLASS(fake_resolver_parent_class)->finalize(o);
}
static void fake_resolver_class_init(FakeResolverClass *k) { G_OBJECT_CLASS(k)->finalize = fake_finalize; }
static void fake_resolver_init(FakeResolver *self) { (void)self; }

/* A resolver implementing only the local half. */
#define LOCAL_TYPE_RESOLVER (local_resolver_get_type())
G_DECLARE_FINAL_TYPE(LocalResolver, local_resolver, LOCAL, RESOLVER, GObject)
struct _LocalResolver { GObject parent_instance; };
static void local_iface_init(GnNostrReferenceResolverInterface *iface) { iface->lookup_local = NULL; }
G_DEFINE_FINAL_TYPE_WITH_CODE(LocalResolver, local_resolver, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GN_TYPE_NOSTR_REFERENCE_RESOLVER, local_iface_init))
static void local_resolver_class_init(LocalResolverClass *k) { (void)k; }
static void local_resolver_init(LocalResolver *self) { (void)self; }

static void settle(GTask *task) {
  (void)task;
  while (g_main_context_iteration(NULL, FALSE));
}

static GtkWidget *child_named(GtkWidget *parent, const char *name) {
  for (GtkWidget *c = gtk_widget_get_first_child(parent); c; c = gtk_widget_get_next_sibling(c))
    if (g_strcmp0(gtk_widget_get_name(c), name) == 0) return c;
  g_assert_not_reached();
}

static void unsupported_done(GObject *src, GAsyncResult *res, gpointer data) {
  g_autoptr(GError) error = NULL;
  g_assert_null(gn_nostr_reference_resolver_fetch_finish(GN_NOSTR_REFERENCE_RESOLVER(src), res, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  *(gboolean *)data = TRUE;
}

static void resolver_contract(void) {
  g_autofree char *note = signed_event(1, "quoted text", NULL);
  g_autofree char *other = signed_event(1, "another note", NULL);
  g_autoptr(GnNostrReference) ref = reference_for(note);
  g_autoptr(FakeResolver) fake = g_object_new(FAKE_TYPE_RESOLVER, NULL);
  GnNostrReferenceResolver *r = GN_NOSTR_REFERENCE_RESOLVER(fake);

  g_assert_null(gn_nostr_reference_resolver_resolve_local(r, ref));
  fake->local_json = g_strdup(note);
  g_autoptr(GnNostrResolvedNote) resolved = gn_nostr_reference_resolver_resolve_local(r, ref);
  g_assert_nonnull(resolved);
  g_assert_cmpstr(resolved->event->content, ==, "quoted text");
  g_assert_cmpstr(resolved->author_name, ==, "Alice");
  /* A different (valid) event is not what the reference points at. */
  g_free(fake->local_json);
  fake->local_json = g_strdup(other);
  g_assert_null(gn_nostr_reference_resolver_resolve_local(r, ref));
  /* A tampered event does not verify. */
  g_autofree gchar *tampered = g_strdup(note);
  char *at = strstr(tampered, "quoted text");
  at[0] = 'Q';
  g_assert_null(gn_nostr_resolved_note_new(ref, tampered, NULL));
  g_assert_cmpuint(fake->fetches, ==, 0);

  /* Verification cache: same answer before and after clearing. */
  g_autoptr(GnNostrEventInfo) a = gn_nostr_event_parse(note, NULL);
  g_autoptr(GnNostrEventInfo) b = gn_nostr_event_parse(note, NULL);
  gn_nostr_event_verify_cache_clear();
  g_autoptr(GnNostrEventInfo) c = gn_nostr_event_parse(note, NULL);
  g_assert_cmpstr(a->id, ==, b->id);
  g_assert_cmpstr(b->content, ==, c->content);
  g_assert_null(gn_nostr_event_parse(tampered, NULL));

  /* Local-only resolvers cannot fetch. */
  g_autoptr(LocalResolver) local = g_object_new(LOCAL_TYPE_RESOLVER, NULL);
  g_assert_false(gn_nostr_reference_resolver_can_fetch(GN_NOSTR_REFERENCE_RESOLVER(local)));
  gboolean done = FALSE;
  gn_nostr_reference_resolver_fetch_async(GN_NOSTR_REFERENCE_RESOLVER(local), ref, NULL,
                                          unsupported_done, &done);
  while (!done) g_main_context_iteration(NULL, TRUE);
}

static void card_inert_and_explicit_fetch(void) {
  if (!gtk_init_check()) { g_test_skip("GTK display unavailable"); return; }
  g_autofree char *note = signed_event(1, "quoted text", NULL);
  g_autoptr(GnNostrReference) ref = reference_for(note);
  g_autoptr(FakeResolver) fake = g_object_new(FAKE_TYPE_RESOLVER, NULL);
  GtkWidget *window = gtk_window_new();
  GtkWidget *card = gn_nostr_reference_card_new();
  GnNostrReferenceCard *rc = GN_NOSTR_REFERENCE_CARD(card);
  gtk_window_set_child(GTK_WINDOW(window), card);
  GtkWidget *find = child_named(card, "reference-find");
  GtkWidget *title = child_named(card, "reference-title");

  g_assert_cmpint(gn_nostr_reference_card_get_state(rc), ==, GN_NOSTR_REFERENCE_CARD_EMPTY);
  g_assert_false(gtk_widget_get_visible(card));

  /* Bound and mapped without fetch ability: inert summary, no Find. */
  gn_nostr_reference_card_set_resolver(rc, GN_NOSTR_REFERENCE_RESOLVER(fake));
  gn_nostr_reference_card_set_reference(rc, ref, GN_NOSTR_REFERENCE_CARD_ROLE_QUOTE, NULL);
  gtk_window_present(GTK_WINDOW(window));
  while (g_main_context_iteration(NULL, FALSE));
  g_assert_cmpint(gn_nostr_reference_card_get_state(rc), ==, GN_NOSTR_REFERENCE_CARD_INERT);
  g_assert_true(g_str_has_prefix(gtk_label_get_text(GTK_LABEL(title)), "Quoted Nostr note: "));
  g_assert_false(gtk_widget_get_visible(find));
  g_assert_false(gn_nostr_reference_card_fetch(rc));
  g_assert_cmpuint(fake->fetches, ==, 0);

  /* Fetchable: Find shows, but binding (and rebinding) still never fetches. */
  fake->can_fetch = TRUE;
  gn_nostr_reference_card_set_reference(rc, ref, GN_NOSTR_REFERENCE_CARD_ROLE_QUOTE, NULL);
  while (g_main_context_iteration(NULL, FALSE));
  g_assert_true(gtk_widget_get_visible(find));
  g_assert_cmpuint(fake->fetches, ==, 0);
  g_assert_cmpuint(fake->lookups, >=, 1);

  /* Declining (cancelled) returns to inert. */
  g_signal_emit_by_name(find, "clicked");
  g_assert_cmpuint(fake->fetches, ==, 1);
  g_assert_cmpint(gn_nostr_reference_card_get_state(rc), ==, GN_NOSTR_REFERENCE_CARD_FETCHING);
  g_assert_false(gtk_widget_get_visible(find));
  /* A rebind of the same reference keeps the explicit fetch. */
  gn_nostr_reference_card_set_reference(rc, ref, GN_NOSTR_REFERENCE_CARD_ROLE_QUOTE, NULL);
  g_assert_cmpint(gn_nostr_reference_card_get_state(rc), ==, GN_NOSTR_REFERENCE_CARD_FETCHING);
  g_task_return_new_error(fake->pending, G_IO_ERROR, G_IO_ERROR_CANCELLED, "declined");
  settle(fake->pending);
  g_assert_cmpint(gn_nostr_reference_card_get_state(rc), ==, GN_NOSTR_REFERENCE_CARD_INERT);

  /* Nothing matching arrived: unavailable, Find offered again. */
  g_assert_true(gn_nostr_reference_card_fetch(rc));
  g_task_return_new_error(fake->pending, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "none");
  settle(fake->pending);
  g_assert_cmpint(gn_nostr_reference_card_get_state(rc), ==, GN_NOSTR_REFERENCE_CARD_UNAVAILABLE);
  g_assert_true(gtk_widget_get_visible(find));

  /* A non-matching result is not shown. */
  g_autofree char *other = signed_event(1, "another", NULL);
  g_assert_true(gn_nostr_reference_card_fetch(rc));
  g_task_return_pointer(fake->pending, g_strdup(other), g_free);
  settle(fake->pending);
  g_assert_cmpint(gn_nostr_reference_card_get_state(rc), ==, GN_NOSTR_REFERENCE_CARD_UNAVAILABLE);

  /* The verified match resolves. */
  g_assert_true(gn_nostr_reference_card_fetch(rc));
  g_assert_cmpuint(fake->fetches, ==, 4);
  g_task_return_pointer(fake->pending, g_strdup(note), g_free);
  settle(fake->pending);
  g_assert_cmpint(gn_nostr_reference_card_get_state(rc), ==, GN_NOSTR_REFERENCE_CARD_RESOLVED);
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(title)), ==, "Quoting Alice");
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(child_named(card, "reference-content"))), ==, "quoted text");
  g_assert_cmpstr(gn_nostr_reference_card_get_note(rc)->event->content, ==, "quoted text");
  g_assert_false(gtk_widget_get_visible(find));

  /* A result for a reference the card no longer shows is dropped. */
  g_autoptr(GnNostrReference) other_ref = reference_for(other);
  gn_nostr_reference_card_set_reference(rc, other_ref, GN_NOSTR_REFERENCE_CARD_ROLE_MENTION, NULL);
  g_assert_true(gn_nostr_reference_card_fetch(rc));
  GTask *stale = g_object_ref(fake->pending);
  gn_nostr_reference_card_set_reference(rc, ref, GN_NOSTR_REFERENCE_CARD_ROLE_MENTION, NULL);
  g_task_return_pointer(stale, g_strdup(other), g_free);
  settle(stale);
  g_object_unref(stale);
  g_assert_cmpint(gn_nostr_reference_card_get_state(rc), ==, GN_NOSTR_REFERENCE_CARD_INERT);

  gn_nostr_reference_card_set_reference(rc, NULL, GN_NOSTR_REFERENCE_CARD_ROLE_MENTION, NULL);
  g_assert_false(gtk_widget_get_visible(card));
  gtk_window_destroy(GTK_WINDOW(window));
}

static void card_local_and_embedded(void) {
  if (!gtk_init_check()) { g_test_skip("GTK display unavailable"); return; }
  g_autofree char *note = signed_event(1, "original", NULL);
  g_autoptr(GnNostrReference) ref = reference_for(note);
  /* A kind-6 repost embedding the signed original resolves with no resolver. */
  ref->kind = 1;
  g_autofree gchar *repost = gn_nostr_build_repost_template(ref, note);
  g_autoptr(GnNostrRepostDescriptor) d = gn_nostr_repost_descriptor_parse(repost, FALSE);
  g_assert_nonnull(d);
  g_assert_nonnull(d->original_json);
  GnNostrReferenceCard *rc = GN_NOSTR_REFERENCE_CARD(g_object_ref_sink(gn_nostr_reference_card_new()));
  gn_nostr_reference_card_set_descriptor(rc, d);
  g_assert_cmpint(gn_nostr_reference_card_get_state(rc), ==, GN_NOSTR_REFERENCE_CARD_RESOLVED);
  g_assert_true(g_str_has_prefix(gtk_label_get_text(GTK_LABEL(child_named(GTK_WIDGET(rc), "reference-title"))),
                                 "Reposted from "));
  /* A tampered embedded copy is ignored: inert. */
  gn_nostr_reference_card_set_reference(rc, ref, GN_NOSTR_REFERENCE_CARD_ROLE_REPOST, "{\"id\":\"x\"}");
  g_assert_cmpint(gn_nostr_reference_card_get_state(rc), ==, GN_NOSTR_REFERENCE_CARD_INERT);
  /* Local lookup resolves without any fetch. */
  g_autoptr(FakeResolver) fake = g_object_new(FAKE_TYPE_RESOLVER, NULL);
  fake->local_json = g_strdup(note);
  fake->can_fetch = TRUE;
  gn_nostr_reference_card_set_resolver(rc, GN_NOSTR_REFERENCE_RESOLVER(fake));
  gn_nostr_reference_card_set_reference(rc, ref, GN_NOSTR_REFERENCE_CARD_ROLE_MENTION, NULL);
  g_assert_cmpint(gn_nostr_reference_card_get_state(rc), ==, GN_NOSTR_REFERENCE_CARD_RESOLVED);
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(child_named(GTK_WIDGET(rc), "reference-title"))), ==, "Note by Alice");
  g_assert_false(gn_nostr_reference_card_fetch(rc));
  g_assert_cmpuint(fake->fetches, ==, 0);
  /* A precomputed preview (gnostr view model). */
  gn_nostr_reference_card_set_preview(rc, NULL, NULL);
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(child_named(GTK_WIDGET(rc), "reference-content"))), ==,
                  "(content unavailable)");
  /* Summaries. */
  g_autofree gchar *s = gn_nostr_reference_dup_summary(ref, GN_NOSTR_REFERENCE_CARD_ROLE_REPOST);
  g_assert_true(g_str_has_prefix(s, "Reposted Nostr note: "));
  g_assert_cmpuint(strlen(s), ==, strlen("Reposted Nostr note: ") + 12);
  g_object_unref(rc);
}

int main(int argc, char **argv) {
  g_setenv("LANGUAGE", "C", TRUE);
  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  g_test_add_func("/reference/resolver-contract", resolver_contract);
  g_test_add_func("/reference/card-inert-and-explicit-fetch", card_inert_and_explicit_fetch);
  g_test_add_func("/reference/card-local-and-embedded", card_local_and_embedded);
  return g_test_run();
}
