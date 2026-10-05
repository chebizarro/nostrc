#include "gh-contact-directory.h"
#include "gh-auth-policy.h"
#include "gh-identity.h"
#include "gh-store-directory.h"

#include <json.h>
#include <nostr-event.h>
#include <nostr-tag.h>
#include <stdlib.h>
#include <string.h>

#define KIND_PROFILE        0
#define KIND_DM_INBOX       10050   /* NIP-17 */
#define FRESH_S             (24 * 3600)             /* S2 */
#define RUN_MIN_S           (2 * 60)                /* S1: first run, next runs */
#define RUN_MAX_S           (30 * 60)
#define SOON_MIN_S          5                       /* an accepted request, a stale send */
#define SOON_MAX_S          60
#define BATCH_GAP_MIN_S     10                      /* S1 random spacing */
#define BATCH_GAP_MAX_S     120
#define BATCH_MAX           10                      /* S1: <= 10 authors per batch */
#define MAX_SOURCES         16                      /* the relay scope's bound */
#define MAX_INBOX_RELAYS    16                      /* the relay publish's bound */
#define MAX_EVENTS_PER_AUTHOR 16
#define FUTURE_SKEW_S       (15 * 60)
#define DEFAULT_DEADLINE_S  15
#define MAX_DEADLINE_S      120
#define MAX_NAME_CHARS      64
#define MAX_NIP05_BYTES     128

enum { SOURCE_PENDING, SOURCE_EOSE, SOURCE_FAILED };

typedef struct _Fetch Fetch;

/* What the directory knows about one pubkey of the current account. */
typedef struct {
  gchar *pubkey;
  gchar *inbox_id;          /* the admitted 10050, or NULL */
  gchar *inbox_json;
  gint64 inbox_created;
  GStrv inbox_relays;       /* its usable ws(s) relays (may be empty) */
  gboolean inbox_truncated;
  gchar *profile_id;        /* the admitted kind 0, or NULL */
  gchar *profile_json;
  gint64 profile_created;
  gchar *name;              /* cleaned display name, or NULL */
  gchar *nip05;             /* cleaned claimed NIP-05, or NULL */
  gint64 inbox_checked;     /* unix s of the last answered lookup of each; 0 never */
  gint64 profile_checked;
  gboolean accepted;
  Fetch *fetch;             /* the lookup in flight for it (borrowed) */
  guint soon;               /* GhClock id of a single refresh, 0 none */
} Contact;

/* One URL-scoped REQ on a fresh scope for a batch of authors. */
struct _Fetch {
  GhContactDirectory *owner;
  guint64 generation;
  GPtrArray *authors;          /* lowercase hex */
  gboolean profiles;           /* kind 0 too (accepted contacts only) */
  GhRelayScope *scope;
  GHashTable *sources;         /* url -> SOURCE_* */
  guint deadline;              /* GhClock id */
  GSource *completion;
  guint events;
  GPtrArray *waiters;          /* Waiter, resolves waiting for this lookup */
  GHashTable *admitted;        /* "pubkey/kind" admitted by this lookup */
  GHashTable *inbox_changed;   /* pubkeys whose cached relay list changed */
  GHashTable *profile_changed; /* pubkeys whose name or NIP-05 changed */
};

typedef struct {
  Fetch *fetch;
  GTask *task;
  gchar *pubkey;
  GSource *cancel;
} Waiter;

struct _GhContactDirectory {
  GObject parent_instance;
  GhAccountController *accounts; /* NULL once disposed */
  GSettings *settings;
  GhClock *clock;
  GhRelayTransport transport;
  GhRelayAuthTransport auth_transport;
  gpointer transport_data;
  gboolean custom_transport;
  gboolean custom_auth;
  guint deadline_s;
  GhConversationStore *model;
  GhStore *store;                /* borrowed while bound */
  guint64 generation;            /* 0: no active account */
  gchar *account;
  GHashTable *contacts;          /* pubkey -> Contact */
  GPtrArray *fetches;            /* Fetch in flight */
  guint run_timer;
  gint64 run_at;
  guint batch_timer;
  GQueue batches;                /* GPtrArray of pubkeys still to ask in this run */
};

enum { SIGNAL_PROFILE_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void gh_contact_directory_resolver_init(GhInboxResolverInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GhContactDirectory, gh_contact_directory, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(GH_TYPE_INBOX_RESOLVER,
                                                    gh_contact_directory_resolver_init))

static void fetch_abort(Fetch *fetch);
static Fetch *fetch_new(GhContactDirectory *self, GPtrArray *authors, gboolean profiles);
static void rescan(GhContactDirectory *self, gboolean initial, gboolean accept_transition);

/* ---- small helpers ----------------------------------------------------------- */

static gboolean
hex64(const gchar *value)
{
  if (!value || strlen(value) != 64)
    return FALSE;
  for (const gchar *p = value; *p; p++)
    if (!g_ascii_isxdigit(*p))
      return FALSE;
  return TRUE;
}

static gint64
now_s(GhContactDirectory *self)
{
  return gh_clock_get_unix(self->clock);
}

static gboolean
stale(GhContactDirectory *self, gint64 checked)
{
  return checked <= 0 || now_s(self) - checked >= FRESH_S;
}

static gboolean
strv_equal0(const gchar *const *a, const gchar *const *b)
{
  if (!a || !b)
    return a == b;
  return g_strv_equal(a, b);
}

/* Display text from untrusted kind-0 content: valid UTF-8, no control or
 * formatting characters (bidi overrides included), whitespace collapsed,
 * at most max_chars characters; NULL when nothing is left. */
static gchar *
clean_text(const gchar *raw, guint max_chars)
{
  if (!raw)
    return NULL;
  g_autofree gchar *valid = g_utf8_make_valid(raw, -1);
  GString *out = g_string_new(NULL);
  gboolean space = FALSE;
  guint chars = 0;
  for (const gchar *p = valid; *p && chars < max_chars; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    GUnicodeType type = g_unichar_type(c);
    if (g_unichar_isspace(c) || type == G_UNICODE_CONTROL || type == G_UNICODE_LINE_SEPARATOR ||
        type == G_UNICODE_PARAGRAPH_SEPARATOR) {
      space = out->len > 0;
      continue;
    }
    if (type == G_UNICODE_FORMAT || type == G_UNICODE_SURROGATE ||
        type == G_UNICODE_UNASSIGNED || type == G_UNICODE_PRIVATE_USE)
      continue;
    if (space) {
      g_string_append_c(out, ' ');
      chars++;
      space = FALSE;
      if (chars >= max_chars)
        break;
    }
    g_string_append_unichar(out, c);
    chars++;
  }
  if (!out->len) {
    g_string_free(out, TRUE);
    return NULL;
  }
  return g_string_free(out, FALSE);
}

/* A claimed NIP-05 address for display: one '@' between two non-empty parts,
 * no whitespace, bounded. Never verified here. */
static gchar *
clean_nip05(const gchar *raw)
{
  g_autofree gchar *text = clean_text(raw, MAX_NIP05_BYTES);
  if (!text || strlen(text) > MAX_NIP05_BYTES || strchr(text, ' '))
    return NULL;
  const gchar *at = strchr(text, '@');
  if (!at || at == text || !at[1] || strchr(at + 1, '@'))
    return NULL;
  return g_utf8_strdown(text, -1);
}

static gchar *
json_string(const gchar *json, const gchar *key)
{
  char *value = NULL;
  if (nostr_json_get_string(json, key, &value) != 0 || !value) {
    free(value);
    return NULL;
  }
  gchar *copy = g_strdup(value);
  free(value);
  return copy;
}

/* Usable relay tags of a 10050: ws(s), deduplicated, first 16. */
static GStrv
inbox_relays(NostrEvent *event, gboolean *truncated)
{
  GPtrArray *urls = g_ptr_array_new_with_free_func(g_free);
  NostrTags *tags = nostr_event_get_tags(event);
  gsize count = tags ? nostr_tags_size(tags) : 0;
  *truncated = FALSE;
  for (gsize i = 0; i < count; i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    const gchar *url = tag && nostr_tag_size(tag) >= 2 ? nostr_tag_get(tag, 1) : NULL;
    if (!tag || g_strcmp0(nostr_tag_get_key(tag), "relay") != 0 ||
        !gh_relay_url_validate(url, NULL) ||
        g_ptr_array_find_with_equal_func(urls, url, g_str_equal, NULL))
      continue;
    if (urls->len >= MAX_INBOX_RELAYS) {
      *truncated = TRUE;
      break;
    }
    g_ptr_array_add(urls, g_strdup(url));
  }
  g_ptr_array_add(urls, NULL);
  return (GStrv)g_ptr_array_free(urls, FALSE);
}

/* A signed event of pubkey with an admissible kind, verified (id and
 * signature); NULL otherwise. id gets its canonical id. */
static NostrEvent *
verified_event(const gchar *json, gchar id[65])
{
  if (!json || strlen(json) > GH_STORE_DIRECTORY_MAX_EVENT)
    return NULL;
  NostrEvent *event = nostr_event_new();
  if (!event)
    return NULL;
  if (nostr_event_deserialize_signed(event, json, NULL) != NOSTR_EVENT_VALIDATION_OK ||
      nostr_event_validate(event, id) != NOSTR_EVENT_VALIDATION_OK) {
    nostr_event_free(event);
    return NULL;
  }
  return event;
}

/* ---- contacts ------------------------------------------------------------------ */

static void
contact_clear_events(Contact *contact)
{
  g_clear_pointer(&contact->inbox_id, g_free);
  g_clear_pointer(&contact->inbox_json, g_free);
  g_clear_pointer(&contact->inbox_relays, g_strfreev);
  g_clear_pointer(&contact->profile_id, g_free);
  g_clear_pointer(&contact->profile_json, g_free);
  g_clear_pointer(&contact->name, g_free);
  g_clear_pointer(&contact->nip05, g_free);
  contact->inbox_created = contact->profile_created = 0;
  contact->inbox_checked = contact->profile_checked = 0;
  contact->inbox_truncated = FALSE;
}

static void
contact_free(gpointer data)
{
  Contact *contact = data;
  contact_clear_events(contact);
  g_free(contact->pubkey);
  g_free(contact);
}

static Contact *
contact_ensure(GhContactDirectory *self, const gchar *pubkey)
{
  Contact *contact = g_hash_table_lookup(self->contacts, pubkey);
  if (!contact) {
    contact = g_new0(Contact, 1);
    contact->pubkey = g_strdup(pubkey);
    g_hash_table_insert(self->contacts, contact->pubkey, contact);
  }
  return contact;
}

static void
contact_cancel_soon(GhContactDirectory *self, Contact *contact)
{
  if (!contact->soon)
    return;
  guint id = contact->soon;
  contact->soon = 0;
  gh_clock_source_remove(self->clock, id);
}

/* Drops what nobody needs: not accepted, nothing cached, nothing pending. */
static void
prune(GhContactDirectory *self)
{
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->contacts);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Contact *contact = value;
    if (!contact->accepted && !contact->inbox_id && !contact->profile_id && !contact->fetch &&
        !contact->soon)
      g_hash_table_iter_remove(&iter);
  }
}

/* Offers an admitted-kind event of contact; TRUE when it replaced the cached
 * one (newest wins). fetch (nullable: a restore) records what changed. */
static gboolean
contact_offer(GhContactDirectory *self, Contact *contact, NostrEvent *event, const gchar *json,
              const gchar *id, Fetch *fetch)
{
  gint64 created = nostr_event_get_created_at(event);
  if (created > now_s(self) + FUTURE_SKEW_S)
    return FALSE; /* a pinned future date would win forever */
  int kind = nostr_event_get_kind(event);
  if (kind == KIND_DM_INBOX) {
    if (contact->inbox_id &&
        (created < contact->inbox_created ||
         (created == contact->inbox_created && g_strcmp0(id, contact->inbox_id) >= 0)))
      return FALSE;
    gboolean truncated = FALSE;
    GStrv relays = inbox_relays(event, &truncated);
    if (fetch && contact->inbox_id &&
        !strv_equal0((const gchar *const *)relays, (const gchar *const *)contact->inbox_relays))
      g_hash_table_add(fetch->inbox_changed, g_strdup(contact->pubkey));
    g_free(contact->inbox_id);
    g_free(contact->inbox_json);
    g_strfreev(contact->inbox_relays);
    contact->inbox_id = g_strdup(id);
    contact->inbox_json = g_strdup(json);
    contact->inbox_created = created;
    contact->inbox_relays = relays;
    contact->inbox_truncated = truncated;
    return TRUE;
  }
  if (kind != KIND_PROFILE)
    return FALSE;
  if (contact->profile_id &&
      (created < contact->profile_created ||
       (created == contact->profile_created && g_strcmp0(id, contact->profile_id) >= 0)))
    return FALSE;
  const gchar *content = nostr_event_get_content(event);
  g_autofree gchar *display = NULL, *name = NULL, *nip05 = NULL;
  if (content && nostr_json_is_object_str(content)) {
    g_autofree gchar *raw_display = json_string(content, "display_name");
    g_autofree gchar *raw_name = json_string(content, "name");
    g_autofree gchar *raw_nip05 = json_string(content, "nip05");
    display = clean_text(raw_display, MAX_NAME_CHARS);
    name = display ? g_steal_pointer(&display) : clean_text(raw_name, MAX_NAME_CHARS);
    nip05 = clean_nip05(raw_nip05);
  }
  if (fetch && (g_strcmp0(name, contact->name) != 0 || g_strcmp0(nip05, contact->nip05) != 0))
    g_hash_table_add(fetch->profile_changed, g_strdup(contact->pubkey));
  g_free(contact->profile_id);
  g_free(contact->profile_json);
  g_free(contact->name);
  g_free(contact->nip05);
  contact->profile_id = g_strdup(id);
  contact->profile_json = g_strdup(json);
  contact->profile_created = created;
  contact->name = g_steal_pointer(&name);
  contact->nip05 = g_steal_pointer(&nip05);
  return TRUE;
}

static GhInboxResult *
contact_result(Contact *contact)
{
  GhInboxResult *result = g_new0(GhInboxResult, 1);
  result->recipient = g_strdup(contact->pubkey);
  result->event_id = g_strdup(contact->inbox_id);
  result->created_at = contact->inbox_created;
  result->truncated = contact->inbox_truncated;
  if (contact->inbox_relays && contact->inbox_relays[0]) {
    result->status = GH_INBOX_FOUND;
    result->relays = g_strdupv(contact->inbox_relays);
  } else {
    result->status = GH_INBOX_EMPTY;
  }
  return result;
}

/* ---- store ------------------------------------------------------------------- */

/* Loads and re-verifies the store's rows; a row that fails is deleted. */
static void
restore(GhContactDirectory *self)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) entries = gh_store_directory_load(self->store, &error);
  if (!entries) {
    g_message("Groundhog could not read its contact directory: %s", error->message);
    return;
  }
  for (guint i = 0; i < entries->len; i++) {
    GhStoreDirectoryEntry *entry = g_ptr_array_index(entries, i);
    gchar id[65] = { 0 };
    NostrEvent *event = verified_event(entry->event_json, id);
    gboolean ok = event && hex64(entry->pubkey) &&
                  g_ascii_strcasecmp(nostr_event_get_pubkey(event), entry->pubkey) == 0 &&
                  nostr_event_get_kind(event) == entry->kind &&
                  (entry->kind == KIND_DM_INBOX || entry->kind == KIND_PROFILE) &&
                  g_strcmp0(id, entry->event_id) == 0;
    if (ok) {
      Contact *contact = contact_ensure(self, entry->pubkey);
      ok = contact_offer(self, contact, event, entry->event_json, id, NULL) ||
           g_strcmp0(entry->kind == KIND_DM_INBOX ? contact->inbox_id : contact->profile_id,
                     id) == 0;
      if (ok && entry->kind == KIND_DM_INBOX)
        contact->inbox_checked = entry->fetched_at;
      else if (ok)
        contact->profile_checked = entry->fetched_at;
    }
    if (event)
      nostr_event_free(event);
    if (!ok) {
      g_autoptr(GError) delete_error = NULL;
      if (!gh_store_directory_delete(self->store, entry->pubkey, entry->kind, &delete_error))
        g_message("Groundhog could not drop an invalid contact directory entry: %s",
                  delete_error->message);
    }
  }
}

static gboolean
persist(GhContactDirectory *self, Fetch *fetch, gboolean answered, gint64 now)
{
  GhStore *store = self->store;
  g_autoptr(GError) error = NULL;
  if (!gh_store_begin(store, &error))
    goto fail;
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init(&iter, fetch->admitted);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    g_auto(GStrv) parts = g_strsplit(key, "/", 2);
    Contact *contact = g_hash_table_lookup(self->contacts, parts[0]);
    if (!contact)
      continue;
    gboolean inbox = g_str_equal(parts[1], "10050");
    GhStoreDirectoryEntry entry = {
      .pubkey = contact->pubkey,
      .kind = inbox ? KIND_DM_INBOX : KIND_PROFILE,
      .event_id = inbox ? contact->inbox_id : contact->profile_id,
      .created_at = inbox ? contact->inbox_created : contact->profile_created,
      .event_json = inbox ? contact->inbox_json : contact->profile_json,
      .fetched_at = inbox ? contact->inbox_checked : contact->profile_checked,
    };
    if (entry.event_id && !gh_store_directory_put(store, &entry, &error))
      goto rollback;
  }
  for (guint i = 0; answered && i < fetch->authors->len; i++) {
    const gchar *pubkey = g_ptr_array_index(fetch->authors, i);
    if (!gh_store_directory_touch(store, pubkey, KIND_DM_INBOX, now, &error) ||
        (fetch->profiles && !gh_store_directory_touch(store, pubkey, KIND_PROFILE, now, &error)))
      goto rollback;
  }
  if (gh_store_commit(store, &error))
    return TRUE;
  goto fail;
rollback:
  gh_store_rollback(store);
fail:
  g_message("Groundhog could not store its contact directory: %s", error->message);
  return FALSE;
}

/* ---- scheduling (S1, S2) --------------------------------------------------------- */

static gboolean on_run(gpointer data);

/* A refresh run at unix time at, unless one is due sooner. */
static void
schedule_run(GhContactDirectory *self, gint64 at)
{
  if (!self->store || !self->generation)
    return;
  if (self->run_timer && self->run_at <= at)
    return;
  if (self->run_timer)
    gh_clock_source_remove(self->clock, self->run_timer);
  gint64 delay = MAX(at - now_s(self), 0);
  self->run_at = at;
  self->run_timer = gh_clock_timeout_add(self->clock, (guint64)delay * 1000, on_run, self, NULL);
}

static void
schedule_run_within(GhContactDirectory *self, gint64 min_s, gint64 max_s)
{
  schedule_run(self, now_s(self) + gh_clock_random_range(self->clock, min_s, max_s));
}

static gboolean
contact_due(GhContactDirectory *self, Contact *contact)
{
  return contact->accepted && !contact->fetch &&
         (stale(self, contact->inbox_checked) || stale(self, contact->profile_checked));
}

/* The next run: when the next accepted contact turns stale, plus U(2, 30) min. */
static void
plan_next_run(GhContactDirectory *self)
{
  gint64 now = now_s(self), earliest = G_MAXINT64;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->contacts);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Contact *contact = value;
    if (!contact->accepted)
      continue;
    gint64 due = contact->fetch ? now + FRESH_S
                                : MIN(contact->inbox_checked, contact->profile_checked) + FRESH_S;
    earliest = MIN(earliest, due);
  }
  if (earliest == G_MAXINT64)
    return; /* nobody to refresh; an accepted contact schedules one */
  schedule_run(self, MAX(earliest, now) + gh_clock_random_range(self->clock, RUN_MIN_S,
                                                                 RUN_MAX_S));
}

static gboolean on_batch(gpointer data);

/* Starts the run's next batch; the one after waits U(10, 120) s. */
static void
next_batch(GhContactDirectory *self)
{
  GPtrArray *batch;
  while ((batch = g_queue_pop_head(&self->batches))) {
    g_autoptr(GPtrArray) authors = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < batch->len; i++) {
      Contact *contact = g_hash_table_lookup(self->contacts, g_ptr_array_index(batch, i));
      if (contact && contact_due(self, contact))
        g_ptr_array_add(authors, g_strdup(contact->pubkey));
    }
    g_ptr_array_unref(batch);
    if (authors->len && fetch_new(self, authors, TRUE))
      break;
  }
  if (!g_queue_is_empty(&self->batches)) {
    gint64 gap = gh_clock_random_range(self->clock, BATCH_GAP_MIN_S, BATCH_GAP_MAX_S);
    self->batch_timer = gh_clock_timeout_add(self->clock, (guint64)gap * 1000, on_batch, self,
                                             NULL);
    return;
  }
  plan_next_run(self);
}

static gboolean
on_batch(gpointer data)
{
  GhContactDirectory *self = data;
  self->batch_timer = 0;
  next_batch(self);
  return G_SOURCE_REMOVE;
}

/* A run: every due accepted contact, in random order, in batches of <= 10. */
static gboolean
on_run(gpointer data)
{
  GhContactDirectory *self = data;
  self->run_timer = 0;
  if (!self->store || !self->generation || self->batch_timer)
    return G_SOURCE_REMOVE;
  g_autoptr(GPtrArray) due = g_ptr_array_new();
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->contacts);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    if (contact_due(self, value))
      g_ptr_array_add(due, ((Contact *)value)->pubkey);
  for (guint i = due->len; i > 1; i--) { /* Fisher-Yates from the GhClock */
    guint j = gh_clock_random_uniform(self->clock, i);
    gpointer tmp = due->pdata[i - 1];
    due->pdata[i - 1] = due->pdata[j];
    due->pdata[j] = tmp;
  }
  for (guint i = 0; i < due->len; i += BATCH_MAX) {
    GPtrArray *batch = g_ptr_array_new_with_free_func(g_free);
    for (guint j = i; j < MIN(due->len, i + BATCH_MAX); j++)
      g_ptr_array_add(batch, g_strdup(g_ptr_array_index(due, j)));
    g_queue_push_tail(&self->batches, batch);
  }
  next_batch(self);
  return G_SOURCE_REMOVE;
}

typedef struct {
  GhContactDirectory *self;
  gchar *pubkey;
} Soon;

static void
soon_free(gpointer data)
{
  Soon *soon = data;
  g_free(soon->pubkey);
  g_free(soon);
}

static gboolean
on_soon(gpointer data)
{
  Soon *soon = data;
  GhContactDirectory *self = soon->self;
  Contact *contact = g_hash_table_lookup(self->contacts, soon->pubkey);
  if (!contact)
    return G_SOURCE_REMOVE;
  contact->soon = 0;
  if (!contact->fetch && self->generation) {
    g_autoptr(GPtrArray) authors = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(authors, g_strdup(contact->pubkey));
    fetch_new(self, authors, contact->accepted);
  }
  return G_SOURCE_REMOVE;
}

/* One contact on its own after U(min, max) s: an accepted request (its
 * name), or a stale inbox list just used for a send (S2). */
static void
schedule_soon(GhContactDirectory *self, Contact *contact)
{
  if (contact->soon || contact->fetch || !self->generation)
    return;
  gint64 delay = gh_clock_random_range(self->clock, SOON_MIN_S, SOON_MAX_S);
  Soon *soon = g_new0(Soon, 1);
  soon->self = self;
  soon->pubkey = g_strdup(contact->pubkey);
  contact->soon = gh_clock_timeout_add(self->clock, (guint64)delay * 1000, on_soon, soon,
                                       soon_free);
}

static void
stop_schedule(GhContactDirectory *self)
{
  if (self->run_timer) {
    gh_clock_source_remove(self->clock, self->run_timer);
    self->run_timer = 0;
  }
  if (self->batch_timer) {
    gh_clock_source_remove(self->clock, self->batch_timer);
    self->batch_timer = 0;
  }
  GPtrArray *batch;
  while ((batch = g_queue_pop_head(&self->batches)))
    g_ptr_array_unref(batch);
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->contacts);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    contact_cancel_soon(self, value);
}

/* ---- lookups ------------------------------------------------------------------ */

static void
destroy_source(GSource **source)
{
  if (!*source)
    return;
  g_source_destroy(*source);
  g_clear_pointer(source, g_source_unref);
}

static gboolean
fetch_current(Fetch *fetch)
{
  GhContactDirectory *self = fetch->owner;
  return self->accounts && self->generation == fetch->generation &&
         gh_account_controller_is_current(self->accounts, fetch->generation);
}

static void
waiter_finish(Waiter *waiter, GhInboxResult *result)
{
  destroy_source(&waiter->cancel);
  if (result) {
    g_task_return_pointer(waiter->task, result, (GDestroyNotify)gh_inbox_result_free);
  } else {
    g_task_return_new_error(waiter->task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "Inbox lookup cancelled or the account changed");
  }
  g_object_unref(waiter->task);
  g_free(waiter->pubkey);
  g_free(waiter);
}

static gboolean
on_waiter_cancelled(GCancellable *cancellable, gpointer data)
{
  (void)cancellable;
  Waiter *waiter = data;
  g_ptr_array_remove(waiter->fetch->waiters, waiter);
  waiter_finish(waiter, NULL);
  return G_SOURCE_REMOVE;
}

static void
fetch_free(Fetch *fetch)
{
  GhContactDirectory *self = fetch->owner;
  destroy_source(&fetch->completion);
  if (fetch->deadline)
    gh_clock_source_remove(self->clock, fetch->deadline);
  if (fetch->scope) {
    gh_relay_scope_cancel(fetch->scope);
    g_clear_pointer(&fetch->scope, gh_relay_scope_unref);
  }
  for (guint i = 0; i < fetch->authors->len; i++) {
    Contact *contact = g_hash_table_lookup(self->contacts, g_ptr_array_index(fetch->authors, i));
    if (contact && contact->fetch == fetch)
      contact->fetch = NULL;
  }
  g_ptr_array_remove(self->fetches, fetch);
  g_ptr_array_unref(fetch->authors);
  g_hash_table_unref(fetch->sources);
  g_ptr_array_unref(fetch->waiters);
  g_hash_table_unref(fetch->admitted);
  g_hash_table_unref(fetch->inbox_changed);
  g_hash_table_unref(fetch->profile_changed);
  g_free(fetch);
}

/* Cancelled (account switch, dispose): every waiter finishes CANCELLED. */
static void
fetch_abort(Fetch *fetch)
{
  while (fetch->waiters->len) {
    Waiter *waiter = g_ptr_array_steal_index(fetch->waiters, 0);
    waiter_finish(waiter, NULL);
  }
  fetch_free(fetch);
}

static void
fetch_counts(Fetch *fetch, guint *sources, guint *answered, guint *failed)
{
  *sources = *answered = *failed = 0;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, fetch->sources);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    (*sources)++;
    if (GPOINTER_TO_UINT(value) == SOURCE_EOSE)
      (*answered)++;
    else
      (*failed)++;
  }
}

static GhInboxResult *
waiter_result(Fetch *fetch, const gchar *pubkey)
{
  GhContactDirectory *self = fetch->owner;
  Contact *contact = g_hash_table_lookup(self->contacts, pubkey);
  GhInboxResult *result = contact && contact->inbox_id ? contact_result(contact)
                                                       : g_new0(GhInboxResult, 1);
  if (!result->recipient)
    result->recipient = g_strdup(pubkey);
  fetch_counts(fetch, &result->sources, &result->answered, &result->failed);
  if (!(contact && contact->inbox_id))
    result->status = result->sources == 0 ? GH_INBOX_NO_SOURCES
                   : result->answered ? GH_INBOX_NOT_FOUND : GH_INBOX_UNREACHABLE;
  return result;
}

static gboolean
fetch_complete(gpointer data)
{
  Fetch *fetch = data;
  GhContactDirectory *self = fetch->owner;
  g_clear_pointer(&fetch->completion, g_source_unref);
  if (!fetch_current(fetch)) {
    fetch_abort(fetch);
    return G_SOURCE_REMOVE;
  }
  guint sources, answered, failed;
  fetch_counts(fetch, &sources, &answered, &failed);
  gint64 now = now_s(self);
  for (guint i = 0; answered && i < fetch->authors->len; i++) {
    Contact *contact = g_hash_table_lookup(self->contacts, g_ptr_array_index(fetch->authors, i));
    if (!contact)
      continue;
    contact->inbox_checked = now;
    if (fetch->profiles)
      contact->profile_checked = now;
  }
  if (self->store && (answered || g_hash_table_size(fetch->admitted)))
    persist(self, fetch, answered > 0, now);
  /* Anything this lookup found is visible before anyone is told. */
  g_autoptr(GPtrArray) waiters = g_ptr_array_new();
  while (fetch->waiters->len)
    g_ptr_array_add(waiters, g_ptr_array_steal_index(fetch->waiters, 0));
  g_autoptr(GPtrArray) inbox_changed = g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GPtrArray) profile_changed = g_ptr_array_new_with_free_func(g_free);
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init(&iter, fetch->inbox_changed);
  while (g_hash_table_iter_next(&iter, &key, NULL))
    g_ptr_array_add(inbox_changed, g_strdup(key));
  g_hash_table_iter_init(&iter, fetch->profile_changed);
  while (g_hash_table_iter_next(&iter, &key, NULL))
    g_ptr_array_add(profile_changed, g_strdup(key));
  for (guint i = 0; i < waiters->len; i++) {
    Waiter *waiter = g_ptr_array_index(waiters, i);
    waiter_finish(waiter, waiter_result(fetch, waiter->pubkey));
  }
  gboolean background_failed = answered == 0 && waiters->len == 0;
  fetch_free(fetch);
  g_object_ref(self);
  for (guint i = 0; i < inbox_changed->len; i++)
    gh_inbox_resolver_emit_changed(GH_INBOX_RESOLVER(self), g_ptr_array_index(inbox_changed, i));
  for (guint i = 0; i < profile_changed->len; i++) {
    Contact *contact = g_hash_table_lookup(self->contacts, g_ptr_array_index(profile_changed, i));
    if (contact && contact->accepted)
      g_signal_emit(self, signals[SIGNAL_PROFILE_CHANGED], 0, contact->pubkey);
  }
  /* No source answered a refresh: try again at the next run, not before
   * U(2, 30) min. */
  if (background_failed && self->generation)
    schedule_run_within(self, RUN_MIN_S, RUN_MAX_S);
  prune(self);
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

/* Deferred to an idle so the scope is never torn down from inside its own
 * callback. The transport delivers each source's stored events before its
 * EOSE (nostrc-qp24.10.6), so none is still pending at completion. */
static void
schedule_completion(Fetch *fetch)
{
  if (fetch->completion)
    return;
  fetch->completion = g_idle_source_new();
  g_source_set_callback(fetch->completion, fetch_complete, fetch, NULL);
  g_source_attach(fetch->completion, g_main_context_get_thread_default());
}

static gboolean
all_settled(Fetch *fetch)
{
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, fetch->sources);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    if (GPOINTER_TO_UINT(value) == SOURCE_PENDING)
      return FALSE;
  return TRUE;
}

static gboolean
fetch_has_author(Fetch *fetch, const gchar *pubkey)
{
  return g_ptr_array_find_with_equal_func(fetch->authors, pubkey, g_str_equal, NULL);
}

static void
fetch_admit(Fetch *fetch, const GhRelayUpdate *update)
{
  if (++fetch->events > MAX_EVENTS_PER_AUTHOR * fetch->authors->len)
    return;
  gchar id[65] = { 0 };
  NostrEvent *event = verified_event(update->event_json, id);
  if (!event)
    return;
  g_autofree gchar *pubkey = g_ascii_strdown(nostr_event_get_pubkey(event) ?
                                             nostr_event_get_pubkey(event) : "", -1);
  int kind = nostr_event_get_kind(event);
  Contact *contact = fetch_has_author(fetch, pubkey)
    ? g_hash_table_lookup(fetch->owner->contacts, pubkey) : NULL;
  /* Someone else's event, another kind, or a kind 0 nobody may ask for. */
  if (contact && (kind == KIND_DM_INBOX ||
                  (kind == KIND_PROFILE && fetch->profiles && contact->accepted)) &&
      contact_offer(fetch->owner, contact, event, update->event_json, id, fetch))
    g_hash_table_add(fetch->admitted, g_strdup_printf("%s/%d", pubkey, kind));
  nostr_event_free(event);
}

static void
on_scope_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  Fetch *fetch = data;
  if (scope != fetch->scope || fetch->completion || !fetch_current(fetch) ||
      !g_hash_table_contains(fetch->sources, update->url))
    return;
  guint status = GPOINTER_TO_UINT(g_hash_table_lookup(fetch->sources, update->url));
  switch (update->notice) {
  case GH_RELAY_NOTICE_EVENT:
    fetch_admit(fetch, update);
    return;
  case GH_RELAY_NOTICE_EOSE:
    if (status == SOURCE_PENDING)
      status = SOURCE_EOSE;
    break;
  case GH_RELAY_NOTICE_ERROR:
  case GH_RELAY_NOTICE_CLOSED:
    if (status == SOURCE_PENDING)
      status = SOURCE_FAILED;
    break;
  default:
    return;
  }
  g_hash_table_insert(fetch->sources, g_strdup(update->url), GUINT_TO_POINTER(status));
  if (all_settled(fetch))
    schedule_completion(fetch);
}

static gboolean
on_deadline(gpointer data)
{
  Fetch *fetch = data;
  fetch->deadline = 0;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, fetch->sources);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    if (GPOINTER_TO_UINT(value) == SOURCE_PENDING)
      g_hash_table_iter_replace(&iter, GUINT_TO_POINTER(SOURCE_FAILED));
  schedule_completion(fetch);
  return G_SOURCE_REMOVE;
}

static NostrFilters *
lookup_filters(GPtrArray *authors, gboolean profiles)
{
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  if (!filters || !filter) {
    if (filters)
      nostr_filters_free(filters);
    if (filter)
      nostr_filter_free(filter);
    return NULL;
  }
  const int both[] = { KIND_PROFILE, KIND_DM_INBOX };
  const int inbox_only[] = { KIND_DM_INBOX };
  if (profiles)
    nostr_filter_set_kinds(filter, both, G_N_ELEMENTS(both));
  else
    nostr_filter_set_kinds(filter, inbox_only, G_N_ELEMENTS(inbox_only));
  nostr_filter_set_authors(filter, (const char *const *)authors->pdata, authors->len);
  gboolean added = nostr_filters_add(filters, filter);
  nostr_filter_free(filter); /* contents moved into the vector */
  if (!added) {
    nostr_filters_free(filters);
    return NULL;
  }
  return filters;
}

/* One REQ for authors on a fresh scope to the discovery relays only, not the
 * account's own relay lists (they would learn whom it talks to, §4.3) unless
 * the user adopted them as discovery relays in onboarding, with that
 * disclosed. NULL when there is no usable source. */
static Fetch *
fetch_new(GhContactDirectory *self, GPtrArray *authors, gboolean profiles)
{
  g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
  g_auto(GStrv) discovery = g_settings_get_strv(self->settings, "discovery-relays");
  for (guint i = 0; discovery && discovery[i] && urls->len < MAX_SOURCES; i++)
    if (gh_relay_url_validate(discovery[i], NULL) &&
        !g_ptr_array_find_with_equal_func(urls, discovery[i], g_str_equal, NULL))
      g_ptr_array_add(urls, g_strdup(discovery[i]));
  if (!urls->len || !authors->len || !self->generation)
    return NULL;
  NostrFilters *filters = lookup_filters(authors, profiles);
  if (!filters)
    return NULL;
  Fetch *fetch = g_new0(Fetch, 1);
  fetch->owner = self;
  fetch->generation = self->generation;
  fetch->profiles = profiles;
  fetch->authors = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < authors->len; i++)
    g_ptr_array_add(fetch->authors, g_strdup(g_ptr_array_index(authors, i)));
  fetch->sources = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  fetch->waiters = g_ptr_array_new();
  fetch->admitted = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  fetch->inbox_changed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  fetch->profile_changed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < fetch->authors->len; i++) {
    Contact *contact = contact_ensure(self, g_ptr_array_index(fetch->authors, i));
    contact_cancel_soon(self, contact);
    contact->fetch = fetch;
  }
  g_ptr_array_add(self->fetches, fetch);
  fetch->scope = self->custom_transport
    ? gh_relay_scope_new_with_transport(fetch->generation, filters, &self->transport,
                                        self->transport_data, on_scope_update, fetch)
    : gh_relay_scope_new(fetch->generation, filters, on_scope_update, fetch);
  if (self->custom_transport && self->custom_auth)
    gh_relay_scope_set_auth_transport(fetch->scope, &self->auth_transport);
  GhAuthPolicy *policy = gh_auth_policy_get_for_accounts(self->accounts);
  for (guint i = 0; i < urls->len; i++) {
    const gchar *url = g_ptr_array_index(urls, i);
    if (!gh_relay_scope_add_url(fetch->scope, url, NULL))
      continue;
    /* An ephemeral key if the relay demands AUTH, never the account (PD-12). */
    gh_auth_policy_apply_scope(policy, fetch->scope, GH_AUTH_PURPOSE_CONTACT_DIRECTORY, url,
                               NULL);
    g_hash_table_insert(fetch->sources, g_strdup(url), GUINT_TO_POINTER(SOURCE_PENDING));
  }
  fetch->deadline = gh_clock_timeout_add(self->clock, (guint64)self->deadline_s * 1000,
                                         on_deadline, fetch, NULL);
  gh_relay_scope_start(fetch->scope);
  if (all_settled(fetch))
    schedule_completion(fetch);
  return fetch;
}

/* ---- account, model and store binding ---------------------------------------------- */

static void
abort_fetches(GhContactDirectory *self)
{
  while (self->fetches->len)
    fetch_abort(g_ptr_array_index(self->fetches, 0));
}

/* Forgets what was cached (a store unbound or a switch); acceptance stays. */
static void
clear_cache(GhContactDirectory *self)
{
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->contacts);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    contact_clear_events(value);
}

static void
sync_account(GhContactDirectory *self)
{
  guint64 generation = 0;
  g_autofree gchar *account = NULL;
  if (self->accounts &&
      gh_account_controller_get_state(self->accounts) == GH_ACCOUNT_STATE_ACTIVE) {
    const gchar *npub = gh_account_controller_get_active_npub(self->accounts);
    account = npub ? gh_identity_pubkey_hex(npub) : NULL;
    generation = account ? gh_account_controller_get_generation(self->accounts) : 0;
  }
  if (generation == self->generation)
    return;
  /* Nothing of the old generation survives into the next one. */
  stop_schedule(self);
  abort_fetches(self);
  self->store = NULL;
  g_hash_table_remove_all(self->contacts);
  g_clear_pointer(&self->account, g_free);
  self->generation = generation;
  self->account = g_steal_pointer(&account);
  rescan(self, TRUE, FALSE);
}

static void
on_room_request_changed(GObject *room, GParamSpec *pspec, gpointer data)
{
  (void)room;
  (void)pspec;
  rescan(data, FALSE, TRUE);
}

static void
on_model_changed(GListModel *model, guint position, guint removed, guint added, gpointer data)
{
  (void)model;
  (void)position;
  (void)removed;
  (void)added;
  rescan(data, FALSE, FALSE);
}

/* The accepted set: peers of the account's NIP-17 rooms that are not
 * requests. A contact accepted by an explicit accept (or reply) is refreshed
 * alone soon; one that appears accepted otherwise joins a run no sooner than
 * U(2, 30) min away; the initial set waits for the first run (S1). */
static void
rescan(GhContactDirectory *self, gboolean initial, gboolean accept_transition)
{
  if (!self->generation)
    return;
  g_autoptr(GHashTable) before = g_hash_table_new(g_str_hash, g_str_equal);
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->contacts);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Contact *contact = value;
    if (contact->accepted)
      g_hash_table_add(before, contact->pubkey);
    contact->accepted = FALSE;
  }
  if (self->model && g_strcmp0(gh_conversation_store_get_account(self->model), self->account) == 0) {
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->model));
    for (guint i = 0; i < n; i++) {
      g_autoptr(GhConversation) room = g_list_model_get_item(G_LIST_MODEL(self->model), i);
      GhConversationBackend backend = gh_conversation_get_backend(room);
      /* NIP-17 conversations and Marmot DMs both contribute accepted contacts.
       * Marmot DMs (is_direct) are the WN DM shape; their peers are contacts
       * the same way NIP-17 peers are. Non-DM MLS groups don't: the person
       * you share a group with is not necessarily a contact (PT-8). */
      gboolean is_dm = backend == GH_CONVERSATION_BACKEND_NIP17 ||
                       (backend == GH_CONVERSATION_BACKEND_MLS &&
                        gh_conversation_get_is_direct(room));
      if (!is_dm)
        continue;
      if (!g_signal_handler_find(room, G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL,
                                 on_room_request_changed, self)) {
        g_signal_connect_object(room, "notify::is-request", G_CALLBACK(on_room_request_changed),
                                self, 0);
        /* MLS conversations may transition to DM when their members arrive;
         * rescan so their peers become accepted contacts. */
        g_signal_connect_object(room, "notify::is-direct", G_CALLBACK(on_room_request_changed),
                                self, 0);
      }
      if (gh_conversation_get_is_request(room))
        continue;
      const gchar *const *peers = gh_conversation_get_peers(room);
      for (guint p = 0; peers && peers[p]; p++)
        if (hex64(peers[p]))
          contact_ensure(self, peers[p])->accepted = TRUE;
    }
  }
  g_autoptr(GPtrArray) shown = g_ptr_array_new_with_free_func(g_free);
  gboolean join_run = FALSE;
  g_hash_table_iter_init(&iter, self->contacts);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Contact *contact = value;
    gboolean was = g_hash_table_contains(before, contact->pubkey);
    if (was == contact->accepted)
      continue;
    if (contact->name || contact->nip05)
      g_ptr_array_add(shown, g_strdup(contact->pubkey));
    if (!contact->accepted || initial)
      continue;
    if (accept_transition)
      schedule_soon(self, contact);
    else
      join_run = TRUE;
  }
  if (join_run)
    schedule_run_within(self, RUN_MIN_S, RUN_MAX_S);
  prune(self);
  for (guint i = 0; i < shown->len; i++)
    g_signal_emit(self, signals[SIGNAL_PROFILE_CHANGED], 0, g_ptr_array_index(shown, i));
}

void
gh_contact_directory_set_conversations(GhContactDirectory *self,
                                       GhConversationStore *conversations)
{
  g_return_if_fail(GH_IS_CONTACT_DIRECTORY(self));
  g_return_if_fail(!conversations || GH_IS_CONVERSATION_STORE(conversations));
  if (conversations == self->model)
    return;
  if (self->model)
    g_signal_handlers_disconnect_by_data(self->model, self);
  g_set_object(&self->model, conversations);
  if (self->model)
    g_signal_connect_object(self->model, "items-changed", G_CALLBACK(on_model_changed), self, 0);
  rescan(self, FALSE, FALSE);
}

/* Accepted contacts whose name or NIP-05 is shown, into shown. */
static void
collect_shown(GhContactDirectory *self, GHashTable *shown)
{
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->contacts);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Contact *contact = value;
    if (contact->accepted && (contact->name || contact->nip05))
      g_hash_table_add(shown, g_strdup(contact->pubkey));
  }
}

gboolean
gh_contact_directory_set_store(GhContactDirectory *self, GhStore *store, GError **error)
{
  g_return_val_if_fail(GH_IS_CONTACT_DIRECTORY(self), FALSE);
  sync_account(self);
  if (store == self->store)
    return TRUE;
  if (store && (!self->generation ||
                g_strcmp0(gh_store_get_account_pubkey(store), self->account) != 0)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "The store does not belong to the active account");
    return FALSE;
  }
  /* Names that the cache drops or restores change what is shown: say so
   * (conversation titles follow "profile-changed", nostrc-qp24.66). */
  g_autoptr(GHashTable) shown = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  collect_shown(self, shown);
  stop_schedule(self);
  clear_cache(self);
  self->store = store;
  if (store) {
    restore(self);
    /* S1: nothing is refreshed right away. */
    schedule_run_within(self, RUN_MIN_S, RUN_MAX_S);
    collect_shown(self, shown);
  }
  GHashTableIter iter;
  gpointer pubkey;
  g_hash_table_iter_init(&iter, shown);
  while (g_hash_table_iter_next(&iter, &pubkey, NULL))
    g_signal_emit(self, signals[SIGNAL_PROFILE_CHANGED], 0, pubkey);
  return TRUE;
}

void
gh_contact_directory_set_deadline(GhContactDirectory *self, guint seconds)
{
  g_return_if_fail(GH_IS_CONTACT_DIRECTORY(self));
  self->deadline_s = CLAMP(seconds, 1, MAX_DEADLINE_S);
}

/* ---- display --------------------------------------------------------------------- */

static Contact *
accepted_contact(GhContactDirectory *self, const gchar *pubkey)
{
  if (!pubkey)
    return NULL;
  g_autofree gchar *key = g_ascii_strdown(pubkey, -1);
  Contact *contact = g_hash_table_lookup(self->contacts, key);
  return contact && contact->accepted ? contact : NULL;
}

gboolean
gh_contact_directory_is_accepted(GhContactDirectory *self, const gchar *pubkey)
{
  g_return_val_if_fail(GH_IS_CONTACT_DIRECTORY(self), FALSE);
  return accepted_contact(self, pubkey) != NULL;
}

const gchar *
gh_contact_directory_get_display_name(GhContactDirectory *self, const gchar *pubkey)
{
  g_return_val_if_fail(GH_IS_CONTACT_DIRECTORY(self), NULL);
  Contact *contact = accepted_contact(self, pubkey);
  return contact ? contact->name : NULL;
}

const gchar *
gh_contact_directory_get_nip05(GhContactDirectory *self, const gchar *pubkey)
{
  g_return_val_if_fail(GH_IS_CONTACT_DIRECTORY(self), NULL);
  Contact *contact = accepted_contact(self, pubkey);
  return contact ? contact->nip05 : NULL;
}

gchar *
gh_contact_directory_dup_picture_uri(GhContactDirectory *self, const gchar *pubkey)
{
  Contact *contact = accepted_contact(self, pubkey);
  if (!contact || !contact->profile_json) return NULL;
  g_autofree gchar *content = json_string(contact->profile_json, "content");
  if (!content) return NULL;
  gchar *uri = json_string(content, "picture");
  if (uri && (strlen(uri) > 4096 || !g_utf8_validate(uri, -1, NULL)))
    g_clear_pointer(&uri, g_free);
  return uri;
}

gchar *
gh_contact_directory_dup_conversation_title(GhContactDirectory *self,
                                            GhConversation *conversation)
{
  g_return_val_if_fail(GH_IS_CONTACT_DIRECTORY(self), NULL);
  g_return_val_if_fail(GH_IS_CONVERSATION(conversation), NULL);
  if (gh_conversation_get_is_request(conversation) ||
      g_strcmp0(gh_conversation_get_account(conversation), self->account) != 0)
    return NULL;
  const gchar *const *peers = gh_conversation_get_peers(conversation);
  if (!peers || !peers[0])
    return NULL;
  GString *title = g_string_new(NULL);
  for (guint i = 0; peers[i]; i++) {
    const gchar *name = gh_contact_directory_get_display_name(self, peers[i]);
    if (!name) {
      g_string_free(title, TRUE);
      return NULL;
    }
    if (i)
      g_string_append(title, ", ");
    g_string_append(title, name);
  }
  return g_string_free(title, FALSE);
}

/* ---- GhInboxResolver --------------------------------------------------------------- */

static void
directory_resolve_async(GhInboxResolver *resolver, const gchar *pubkey_hex,
                        GCancellable *cancellable, GAsyncReadyCallback callback,
                        gpointer user_data)
{
  GhContactDirectory *self = GH_CONTACT_DIRECTORY(resolver);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, directory_resolve_async);
  if (g_task_return_error_if_cancelled(task)) {
    g_object_unref(task);
    return;
  }
  if (!hex64(pubkey_hex)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "A 64-character hex pubkey is required");
    g_object_unref(task);
    return;
  }
  sync_account(self);
  if (!self->generation) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                            "No active Groundhog account");
    g_object_unref(task);
    return;
  }
  g_task_set_task_data(task, g_memdup2(&self->generation, sizeof self->generation), g_free);
  g_autofree gchar *key = g_ascii_strdown(pubkey_hex, -1);
  Contact *contact = g_hash_table_lookup(self->contacts, key);
  if (contact && contact->inbox_id) {
    /* S2: the cached list is the answer; a stale one is refreshed off the
     * send path, and a changed result reaches the outbox as "changed". */
    GhInboxResult *result = contact_result(contact);
    result->cached = TRUE;
    if (stale(self, contact->inbox_checked))
      schedule_soon(self, contact);
    g_task_return_pointer(task, result, (GDestroyNotify)gh_inbox_result_free);
    g_object_unref(task);
    return;
  }
  /* The first contact with this recipient, whom the user chose: one lookup
   * the send waits for (its name only if already accepted, PT-8). */
  Fetch *fetch = contact ? contact->fetch : NULL;
  if (!fetch) {
    g_autoptr(GPtrArray) authors = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(authors, g_strdup(key));
    fetch = fetch_new(self, authors, contact && contact->accepted);
  }
  if (!fetch) {
    GhInboxResult *result = g_new0(GhInboxResult, 1);
    result->status = GH_INBOX_NO_SOURCES;
    result->recipient = g_strdup(key);
    g_task_return_pointer(task, result, (GDestroyNotify)gh_inbox_result_free);
    g_object_unref(task);
    prune(self);
    return;
  }
  Waiter *waiter = g_new0(Waiter, 1);
  waiter->fetch = fetch;
  waiter->task = task;
  waiter->pubkey = g_steal_pointer(&key);
  if (cancellable) {
    waiter->cancel = g_cancellable_source_new(cancellable);
    g_source_set_callback(waiter->cancel, G_SOURCE_FUNC(on_waiter_cancelled), waiter, NULL);
    g_source_attach(waiter->cancel, g_main_context_get_thread_default());
  }
  g_ptr_array_add(fetch->waiters, waiter);
}

static GhInboxResult *
directory_resolve_finish(GhInboxResolver *resolver, GAsyncResult *result, GError **error)
{
  GhContactDirectory *self = GH_CONTACT_DIRECTORY(resolver);
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == directory_resolve_async, NULL);
  GhInboxResult *value = g_task_propagate_pointer(G_TASK(result), error);
  const guint64 *generation = g_task_get_task_data(G_TASK(result));
  if (value && (!generation || !self->accounts ||
                !gh_account_controller_is_current(self->accounts, *generation))) {
    gh_inbox_result_free(value);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                        "Inbox lookup cancelled or the account changed");
    return NULL;
  }
  return value;
}

static void
directory_forget(GhInboxResolver *resolver, const gchar *pubkey_hex)
{
  GhContactDirectory *self = GH_CONTACT_DIRECTORY(resolver);
  if (!hex64(pubkey_hex))
    return;
  g_autofree gchar *key = g_ascii_strdown(pubkey_hex, -1);
  Contact *contact = g_hash_table_lookup(self->contacts, key);
  if (contact)
    contact_clear_events(contact);
  g_autoptr(GError) error = NULL;
  if (self->store && !gh_store_directory_delete(self->store, key, -1, &error))
    g_message("Groundhog could not forget a contact directory entry: %s", error->message);
  prune(self);
}

static void
gh_contact_directory_resolver_init(GhInboxResolverInterface *iface)
{
  iface->resolve_async = directory_resolve_async;
  iface->resolve_finish = directory_resolve_finish;
  iface->forget = directory_forget;
}

/* ---- GObject -------------------------------------------------------------------- */

GhContactDirectory *
gh_contact_directory_new(const GhContactDirectoryConfig *config)
{
  g_return_val_if_fail(config != NULL, NULL);
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(config->accounts), NULL);
  g_return_val_if_fail(G_IS_SETTINGS(config->settings), NULL);
  g_return_val_if_fail(!config->transport || (config->transport->open && config->transport->close),
                       NULL);
  g_return_val_if_fail(!config->auth_transport ||
                       (config->transport && config->auth_transport->send_auth &&
                        config->auth_transport->resubscribe), NULL);
  GhContactDirectory *self = g_object_new(GH_TYPE_CONTACT_DIRECTORY, NULL);
  self->accounts = g_object_ref(config->accounts);
  self->settings = g_object_ref(config->settings);
  self->clock = config->clock ? gh_clock_ref(config->clock) : gh_clock_new_system();
  if (config->transport) {
    self->transport = *config->transport;
    self->transport_data = config->transport_data;
    self->custom_transport = TRUE;
    if (config->auth_transport) {
      self->auth_transport = *config->auth_transport;
      self->custom_auth = TRUE;
    }
  }
  g_signal_connect_object(self->accounts, "changed", G_CALLBACK(sync_account), self,
                          G_CONNECT_SWAPPED);
  sync_account(self);
  return self;
}

static void
gh_contact_directory_dispose(GObject *object)
{
  GhContactDirectory *self = GH_CONTACT_DIRECTORY(object);
  stop_schedule(self);
  abort_fetches(self);
  self->store = NULL;
  self->generation = 0;
  if (self->model) {
    g_signal_handlers_disconnect_by_data(self->model, self);
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->model));
    for (guint i = 0; i < n; i++) {
      g_autoptr(GObject) room = g_list_model_get_item(G_LIST_MODEL(self->model), i);
      g_signal_handlers_disconnect_by_data(room, self);
    }
    g_clear_object(&self->model);
  }
  if (self->accounts)
    g_signal_handlers_disconnect_by_data(self->accounts, self);
  g_clear_object(&self->accounts);
  g_clear_object(&self->settings);
  g_hash_table_remove_all(self->contacts);
  G_OBJECT_CLASS(gh_contact_directory_parent_class)->dispose(object);
}

static void
gh_contact_directory_finalize(GObject *object)
{
  GhContactDirectory *self = GH_CONTACT_DIRECTORY(object);
  g_hash_table_unref(self->contacts);
  g_ptr_array_unref(self->fetches);
  g_clear_pointer(&self->clock, gh_clock_unref);
  g_free(self->account);
  G_OBJECT_CLASS(gh_contact_directory_parent_class)->finalize(object);
}

static void
gh_contact_directory_class_init(GhContactDirectoryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_contact_directory_dispose;
  object_class->finalize = gh_contact_directory_finalize;
  signals[SIGNAL_PROFILE_CHANGED] =
    g_signal_new("profile-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                 NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gh_contact_directory_init(GhContactDirectory *self)
{
  self->deadline_s = DEFAULT_DEADLINE_S;
  self->contacts = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, contact_free);
  self->fetches = g_ptr_array_new();
  g_queue_init(&self->batches);
}
