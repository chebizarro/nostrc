#include "gh-mls-poll.h"

#include <gio/gio.h>
#include <nostr-event.h>
#include <nostr-tag.h>
#include <string.h>
#include <stdlib.h>

/* ---- helpers ------------------------------------------------------------ */

void
gh_mls_poll_option_free(GhMlsPollOption *opt)
{
  if (!opt) return;
  g_free(opt->id);
  g_free(opt->label);
  g_free(opt);
}

void
gh_mls_poll_def_free(GhMlsPollDef *def)
{
  if (!def) return;
  g_free(def->question);
  g_clear_pointer(&def->options, g_ptr_array_unref);
  g_free(def);
}

void
gh_mls_poll_tally_free(GhMlsPollTally *tally)
{
  if (!tally) return;
  g_free(tally->id);
  g_free(tally->label);
  g_free(tally);
}

void
gh_mls_poll_voter_record_free(GhMlsPollVoterRecord *rec)
{
  if (!rec) return;
  g_free(rec->voter_pubkey);
  g_strfreev(rec->option_ids);
  g_free(rec);
}

/* ---- validation --------------------------------------------------------- */

static gboolean
valid_hex64(const gchar *s)
{
  if (!s || strlen(s) != 64) return FALSE;
  for (gsize i = 0; i < 64; i++) {
    gchar c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return FALSE;
  }
  return TRUE;
}

static gboolean
valid_option_id(const gchar *s)
{
  if (!s || !*s) return FALSE;
  gsize len = strlen(s);
  if (len > GH_MLS_POLL_MAX_OPTION_ID) return FALSE;
  for (gsize i = 0; i < len; i++)
    if (!g_ascii_isalnum(s[i])) return FALSE;
  return TRUE;
}

static gboolean
valid_display_text(const gchar *s, gsize max_bytes)
{
  if (!s || !*s) return FALSE;
  gsize len = strlen(s);
  if (len > max_bytes) return FALSE;
  /* No leading/trailing whitespace. */
  if (g_ascii_isspace(s[0]) || g_ascii_isspace(s[len - 1])) return FALSE;
  /* No control characters or bidi overrides. */
  for (const gchar *p = s; *p; p = g_utf8_next_char(p)) {
    gunichar ch = g_utf8_get_char(p);
    if (g_unichar_iscntrl(ch)) return FALSE;
    if (ch == 0x061c || ch == 0x200e || ch == 0x200f ||
        (ch >= 0x202a && ch <= 0x202e) || (ch >= 0x2066 && ch <= 0x2069))
      return FALSE;
  }
  return TRUE;
}

/* ---- GhMlsPoll ---------------------------------------------------------- */

struct _GhMlsPoll {
  GObject parent_instance;
  gchar       *event_id;
  gchar       *creator;
  gchar       *question;
  GPtrArray   *options;      /* GhMlsPollTally */
  GhMlsPollType poll_type;
  gint64       ends_at;
  gint64       created_at;
  /* Voter records: voter pubkey → GhMlsPollVoterRecord. */
  GHashTable  *voters;       /* gchar* → GhMlsPollVoterRecord* */
  gchar       *local_account;
  gchar      **local_selection; /* NULL-terminated, borrowed ids or empty */
};

enum {
  SIG_TALLIES_CHANGED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhMlsPoll, gh_mls_poll, G_TYPE_OBJECT)

static void
gh_mls_poll_finalize(GObject *obj)
{
  GhMlsPoll *self = GH_MLS_POLL(obj);
  g_free(self->event_id);
  g_free(self->creator);
  g_free(self->question);
  g_clear_pointer(&self->options, g_ptr_array_unref);
  g_clear_pointer(&self->voters, g_hash_table_unref);
  g_free(self->local_account);
  g_free(self->local_selection);
  G_OBJECT_CLASS(gh_mls_poll_parent_class)->finalize(obj);
}

static void
gh_mls_poll_class_init(GhMlsPollClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = gh_mls_poll_finalize;
  signals[SIG_TALLIES_CHANGED] =
    g_signal_new("tallies-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
gh_mls_poll_init(GhMlsPoll *self)
{
  self->voters = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                       (GDestroyNotify) gh_mls_poll_voter_record_free);
}

/* Recompute tallies from the voter records. */
static void
recompute_tallies(GhMlsPoll *self)
{
  for (guint i = 0; i < self->options->len; i++) {
    GhMlsPollTally *t = g_ptr_array_index(self->options, i);
    t->votes = 0;
  }
  GHashTableIter iter;
  g_hash_table_iter_init(&iter, self->voters);
  GhMlsPollVoterRecord *rec;
  while (g_hash_table_iter_next(&iter, NULL, (gpointer *) &rec)) {
    for (gchar **id = rec->option_ids; id && *id; id++) {
      for (guint i = 0; i < self->options->len; i++) {
        GhMlsPollTally *t = g_ptr_array_index(self->options, i);
        if (g_strcmp0(t->id, *id) == 0) {
          t->votes++;
          break;
        }
      }
    }
  }
}

/* Update local_selection from the voter records. */
static void
update_local_selection(GhMlsPoll *self)
{
  g_free(self->local_selection);
  self->local_selection = NULL;
  if (!self->local_account) {
    self->local_selection = g_new0(gchar *, 1);
    return;
  }
  GhMlsPollVoterRecord *rec = g_hash_table_lookup(self->voters, self->local_account);
  if (!rec || !rec->option_ids) {
    self->local_selection = g_new0(gchar *, 1);
    return;
  }
  guint n = g_strv_length(rec->option_ids);
  self->local_selection = g_new0(gchar *, n + 1);
  for (guint i = 0; i < n; i++)
    self->local_selection[i] = rec->option_ids[i]; /* borrowed */
}

/* ---- parse -------------------------------------------------------------- */

GhMlsPoll *
gh_mls_poll_new_from_event(const gchar *event_id, const gchar *sender,
                           gint64 created_at, const gchar *inner_event_json,
                           GError **error)
{
  if (!valid_hex64(event_id) || !valid_hex64(sender) || !inner_event_json) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Poll event requires valid event id and sender");
    return NULL;
  }

  /* Parse with the libnostr event API. */
  NostrEvent *event = nostr_event_new();
  if (!event) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Out of memory");
    return NULL;
  }
  if (!nostr_event_deserialize_compact(event, inner_event_json, NULL)) {
    nostr_event_free(event);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Malformed inner event");
    return NULL;
  }

  if (nostr_event_get_kind(event) != GH_MLS_POLL_KIND) {
    nostr_event_free(event);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Not a poll event (kind != 1068)");
    return NULL;
  }

  const gchar *question = nostr_event_get_content(event);
  if (!valid_display_text(question, GH_MLS_POLL_MAX_QUESTION)) {
    nostr_event_free(event);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid poll question");
    return NULL;
  }

  NostrTags *tags = (NostrTags *) nostr_event_get_tags(event);
  if (!tags || nostr_tags_size(tags) > GH_MLS_POLL_MAX_TAGS) {
    nostr_event_free(event);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Too many tags or no tags");
    return NULL;
  }

  g_autoptr(GPtrArray) options = g_ptr_array_new_with_free_func(
    (GDestroyNotify) gh_mls_poll_option_free);
  GhMlsPollType poll_type = GH_MLS_POLL_SINGLE_CHOICE;
  gboolean has_poll_type = FALSE;
  gint64 ends_at = 0;
  gboolean has_ends_at = FALSE;
  gboolean malformed = FALSE;

  for (gsize i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (!tag) continue;
    gsize tag_len = nostr_tag_size(tag);
    if (tag_len < 1) continue;
    const gchar *tag_name = nostr_tag_get(tag, 0);
    if (!tag_name) continue;

    if (g_strcmp0(tag_name, GH_MLS_POLL_OPTION_TAG) == 0) {
      if (tag_len != 3) { malformed = TRUE; break; }
      const gchar *opt_id = nostr_tag_get(tag, 1);
      const gchar *opt_label = nostr_tag_get(tag, 2);
      if (!valid_option_id(opt_id) ||
          !valid_display_text(opt_label, GH_MLS_POLL_MAX_OPTION_LABEL)) {
        malformed = TRUE; break;
      }
      /* Check duplicate ids. */
      gboolean dup = FALSE;
      for (guint j = 0; j < options->len; j++) {
        GhMlsPollOption *existing = g_ptr_array_index(options, j);
        if (g_strcmp0(existing->id, opt_id) == 0) { dup = TRUE; break; }
      }
      if (dup) { malformed = TRUE; break; }
      GhMlsPollOption *opt = g_new0(GhMlsPollOption, 1);
      opt->id = g_strdup(opt_id);
      opt->label = g_strdup(opt_label);
      g_ptr_array_add(options, opt);
    } else if (g_strcmp0(tag_name, GH_MLS_POLL_TYPE_TAG) == 0) {
      if (tag_len != 2 || has_poll_type) { malformed = TRUE; break; }
      const gchar *type_str = nostr_tag_get(tag, 1);
      if (g_strcmp0(type_str, "singlechoice") == 0)
        poll_type = GH_MLS_POLL_SINGLE_CHOICE;
      else if (g_strcmp0(type_str, "multiplechoice") == 0)
        poll_type = GH_MLS_POLL_MULTIPLE_CHOICE;
      else { malformed = TRUE; break; }
      has_poll_type = TRUE;
    } else if (g_strcmp0(tag_name, GH_MLS_POLL_ENDS_AT_TAG) == 0) {
      if (tag_len != 2 || has_ends_at) { malformed = TRUE; break; }
      const gchar *val = nostr_tag_get(tag, 1);
      gchar *endp = NULL;
      ends_at = val ? g_ascii_strtoll(val, &endp, 10) : 0;
      if (!val || !*val || (endp && *endp) || ends_at <= 0) {
        malformed = TRUE; break;
      }
      has_ends_at = TRUE;
    }
    /* Unknown tags are ignored (forward compatibility, as MDK does). */
  }

  if (malformed) {
    nostr_event_free(event);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Malformed poll tag");
    return NULL;
  }

  if (options->len < GH_MLS_POLL_MIN_OPTIONS || options->len > GH_MLS_POLL_MAX_OPTIONS) {
    nostr_event_free(event);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "Poll must have %d to %d options, got %u",
                GH_MLS_POLL_MIN_OPTIONS, GH_MLS_POLL_MAX_OPTIONS, options->len);
    return NULL;
  }

  if (has_ends_at) {
    gint64 lifetime = ends_at - created_at;
    if (lifetime <= 0 || lifetime > GH_MLS_POLL_MAX_LIFETIME) {
      nostr_event_free(event);
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                          "Invalid poll deadline");
      return NULL;
    }
  }

  nostr_event_free(event);

  /* Build the GhMlsPoll. */
  GhMlsPoll *self = g_object_new(GH_TYPE_MLS_POLL, NULL);
  self->event_id = g_strdup(event_id);
  self->creator = g_strdup(sender);
  self->question = g_strdup(question);
  self->poll_type = poll_type;
  self->ends_at = ends_at;
  self->created_at = created_at;

  self->options = g_ptr_array_new_with_free_func((GDestroyNotify) gh_mls_poll_tally_free);
  for (guint i = 0; i < options->len; i++) {
    GhMlsPollOption *opt = g_ptr_array_index(options, i);
    GhMlsPollTally *tally = g_new0(GhMlsPollTally, 1);
    tally->id = g_strdup(opt->id);
    tally->label = g_strdup(opt->label);
    tally->votes = 0;
    g_ptr_array_add(self->options, tally);
  }
  self->local_selection = g_new0(gchar *, 1);
  return self;
}

const gchar *
gh_mls_poll_get_event_id(GhMlsPoll *self)
{
  g_return_val_if_fail(GH_IS_MLS_POLL(self), NULL);
  return self->event_id;
}

const gchar *
gh_mls_poll_get_creator(GhMlsPoll *self)
{
  g_return_val_if_fail(GH_IS_MLS_POLL(self), NULL);
  return self->creator;
}

const gchar *
gh_mls_poll_get_question(GhMlsPoll *self)
{
  g_return_val_if_fail(GH_IS_MLS_POLL(self), NULL);
  return self->question;
}

GhMlsPollType
gh_mls_poll_get_poll_type(GhMlsPoll *self)
{
  g_return_val_if_fail(GH_IS_MLS_POLL(self), GH_MLS_POLL_SINGLE_CHOICE);
  return self->poll_type;
}

gint64
gh_mls_poll_get_ends_at(GhMlsPoll *self)
{
  g_return_val_if_fail(GH_IS_MLS_POLL(self), 0);
  return self->ends_at;
}

gint64
gh_mls_poll_get_created_at(GhMlsPoll *self)
{
  g_return_val_if_fail(GH_IS_MLS_POLL(self), 0);
  return self->created_at;
}

guint
gh_mls_poll_get_n_options(GhMlsPoll *self)
{
  g_return_val_if_fail(GH_IS_MLS_POLL(self), 0);
  return self->options->len;
}

const GhMlsPollTally *
gh_mls_poll_get_option(GhMlsPoll *self, guint index)
{
  g_return_val_if_fail(GH_IS_MLS_POLL(self), NULL);
  if (index >= self->options->len) return NULL;
  return g_ptr_array_index(self->options, index);
}

guint
gh_mls_poll_get_total_voters(GhMlsPoll *self)
{
  g_return_val_if_fail(GH_IS_MLS_POLL(self), 0);
  return g_hash_table_size(self->voters);
}

gboolean
gh_mls_poll_is_open(GhMlsPoll *self, gint64 now_s)
{
  g_return_val_if_fail(GH_IS_MLS_POLL(self), FALSE);
  if (self->ends_at <= 0) return TRUE;
  return now_s <= self->ends_at;
}

const gchar *const *
gh_mls_poll_get_local_selection(GhMlsPoll *self)
{
  g_return_val_if_fail(GH_IS_MLS_POLL(self), NULL);
  return (const gchar *const *) self->local_selection;
}

gboolean
gh_mls_poll_has_voted(GhMlsPoll *self)
{
  g_return_val_if_fail(GH_IS_MLS_POLL(self), FALSE);
  return self->local_selection && self->local_selection[0] != NULL;
}

void
gh_mls_poll_set_local_account(GhMlsPoll *self, const gchar *account_pubkey)
{
  g_return_if_fail(GH_IS_MLS_POLL(self));
  g_free(self->local_account);
  self->local_account = g_strdup(account_pubkey);
  update_local_selection(self);
}

gboolean
gh_mls_poll_apply_vote(GhMlsPoll *self, const gchar *voter_pubkey,
                       const gchar **option_ids, gint64 vote_created_at)
{
  g_return_val_if_fail(GH_IS_MLS_POLL(self), FALSE);
  (void) vote_created_at;
  if (!valid_hex64(voter_pubkey) || !option_ids || !option_ids[0])
    return FALSE;

  /* Validate all option ids exist in the poll. */
  for (const gchar **id = option_ids; *id; id++) {
    gboolean found = FALSE;
    for (guint i = 0; i < self->options->len; i++) {
      GhMlsPollTally *t = g_ptr_array_index(self->options, i);
      if (g_strcmp0(t->id, *id) == 0) { found = TRUE; break; }
    }
    if (!found) return FALSE;
  }

  guint n_selected = g_strv_length((gchar **) option_ids);
  if (self->poll_type == GH_MLS_POLL_SINGLE_CHOICE && n_selected != 1)
    return FALSE;

  GhMlsPollVoterRecord *rec = g_new0(GhMlsPollVoterRecord, 1);
  rec->voter_pubkey = g_strdup(voter_pubkey);
  rec->option_ids = g_strdupv((gchar **) option_ids);
  g_hash_table_replace(self->voters, rec->voter_pubkey, rec);

  recompute_tallies(self);
  update_local_selection(self);
  g_signal_emit(self, signals[SIG_TALLIES_CHANGED], 0);
  return TRUE;
}

/* ---- builders ----------------------------------------------------------- */

gchar *
gh_mls_poll_build_event(const gchar *account_pubkey, const gchar *nostr_group_hex,
                        gint64 created_at, const gchar *question,
                        const gchar **option_labels, guint n_options,
                        GhMlsPollType poll_type, gint64 ends_at,
                        GError **error)
{
  if (!valid_hex64(account_pubkey) || !nostr_group_hex || !*nostr_group_hex) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid account or group hex");
    return NULL;
  }
  if (!valid_display_text(question, GH_MLS_POLL_MAX_QUESTION)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid poll question");
    return NULL;
  }
  if (n_options < GH_MLS_POLL_MIN_OPTIONS || n_options > GH_MLS_POLL_MAX_OPTIONS) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "Need %d to %d options", GH_MLS_POLL_MIN_OPTIONS, GH_MLS_POLL_MAX_OPTIONS);
    return NULL;
  }
  for (guint i = 0; i < n_options; i++) {
    if (!valid_display_text(option_labels[i], GH_MLS_POLL_MAX_OPTION_LABEL)) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                  "Invalid option label at index %u", i);
      return NULL;
    }
  }
  if (ends_at > 0) {
    gint64 lifetime = ends_at - created_at;
    if (lifetime <= 0 || lifetime > GH_MLS_POLL_MAX_LIFETIME) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                          "Invalid poll deadline");
      return NULL;
    }
  }

  NostrEvent *event = nostr_event_new();
  if (!event) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Out of memory");
    return NULL;
  }
  nostr_event_set_kind(event, GH_MLS_POLL_KIND);
  nostr_event_set_pubkey(event, account_pubkey);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, question);

  NostrTags *tags = nostr_tags_new(1, nostr_tag_new("h", nostr_group_hex, NULL));
  for (guint i = 0; i < n_options; i++) {
    g_autofree gchar *idx = g_strdup_printf("%u", i);
    nostr_tags_append(tags, nostr_tag_new(GH_MLS_POLL_OPTION_TAG, idx,
                                          option_labels[i], NULL));
  }
  const gchar *type_str = poll_type == GH_MLS_POLL_MULTIPLE_CHOICE
                            ? "multiplechoice" : "singlechoice";
  nostr_tags_append(tags, nostr_tag_new(GH_MLS_POLL_TYPE_TAG, type_str, NULL));
  if (ends_at > 0) {
    g_autofree gchar *ea = g_strdup_printf("%" G_GINT64_FORMAT, ends_at);
    nostr_tags_append(tags, nostr_tag_new(GH_MLS_POLL_ENDS_AT_TAG, ea, NULL));
  }
  nostr_event_set_tags(event, tags);

  gchar id[65] = { 0 };
  gchar *json = NULL;
  if (nostr_event_compute_id(event, id) == NOSTR_EVENT_VALIDATION_OK) {
    free(event->id);
    event->id = strdup(id);
    char *serialized = nostr_event_serialize_compact(event);
    json = g_strdup(serialized);
    free(serialized);
  }
  nostr_event_free(event);
  if (!json)
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not serialize poll event");
  return json;
}

gchar *
gh_mls_poll_build_vote_event(const gchar *account_pubkey, const gchar *nostr_group_hex,
                             gint64 created_at, const gchar *poll_event_id,
                             const gchar **option_ids, guint n_options,
                             GError **error)
{
  if (!valid_hex64(account_pubkey) || !nostr_group_hex || !*nostr_group_hex) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid account or group hex");
    return NULL;
  }
  if (!valid_hex64(poll_event_id)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid poll event id");
    return NULL;
  }
  if (n_options == 0 || n_options > GH_MLS_POLL_MAX_OPTIONS) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid number of options");
    return NULL;
  }
  for (guint i = 0; i < n_options; i++) {
    if (!valid_option_id(option_ids[i])) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                  "Invalid option id at index %u", i);
      return NULL;
    }
  }

  NostrEvent *event = nostr_event_new();
  if (!event) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Out of memory");
    return NULL;
  }
  nostr_event_set_kind(event, GH_MLS_POLL_VOTE_KIND);
  nostr_event_set_pubkey(event, account_pubkey);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, "");

  NostrTags *tags = nostr_tags_new(1, nostr_tag_new("h", nostr_group_hex, NULL));
  nostr_tags_append(tags, nostr_tag_new(GH_MLS_POLL_EVENT_REF_TAG, poll_event_id, NULL));
  for (guint i = 0; i < n_options; i++)
    nostr_tags_append(tags, nostr_tag_new(GH_MLS_POLL_RESPONSE_TAG, option_ids[i], NULL));
  nostr_event_set_tags(event, tags);

  gchar id[65] = { 0 };
  gchar *json = NULL;
  if (nostr_event_compute_id(event, id) == NOSTR_EVENT_VALIDATION_OK) {
    free(event->id);
    event->id = strdup(id);
    char *serialized = nostr_event_serialize_compact(event);
    json = g_strdup(serialized);
    free(serialized);
  }
  nostr_event_free(event);
  if (!json)
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not serialize vote event");
  return json;
}

gboolean
gh_mls_poll_parse_vote(const gchar *inner_event_json,
                       gchar **out_target_poll_id, gchar ***out_option_ids,
                       GError **error)
{
  g_return_val_if_fail(inner_event_json && out_target_poll_id && out_option_ids, FALSE);
  *out_target_poll_id = NULL;
  *out_option_ids = NULL;

  NostrEvent *event = nostr_event_new();
  if (!event) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Out of memory");
    return FALSE;
  }
  if (!nostr_event_deserialize_compact(event, inner_event_json, NULL)) {
    nostr_event_free(event);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Malformed inner event");
    return FALSE;
  }

  if (nostr_event_get_kind(event) != GH_MLS_POLL_VOTE_KIND) {
    nostr_event_free(event);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Not a vote event");
    return FALSE;
  }

  const gchar *content = nostr_event_get_content(event);
  if (content && *content) {
    nostr_event_free(event);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Vote content must be empty");
    return FALSE;
  }

  NostrTags *tags = (NostrTags *) nostr_event_get_tags(event);
  if (!tags || nostr_tags_size(tags) > GH_MLS_POLL_MAX_TAGS) {
    nostr_event_free(event);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Too many tags or no tags");
    return FALSE;
  }

  gchar *target = NULL;
  g_autoptr(GPtrArray) selections = g_ptr_array_new_with_free_func(g_free);
  gboolean parse_error = FALSE;

  for (gsize i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (!tag) continue;
    gsize tag_len = nostr_tag_size(tag);
    if (tag_len < 1) continue;
    const gchar *tag_name = nostr_tag_get(tag, 0);
    if (!tag_name) continue;

    if (g_strcmp0(tag_name, GH_MLS_POLL_EVENT_REF_TAG) == 0) {
      if (tag_len != 2 || target) { parse_error = TRUE; break; }
      const gchar *val = nostr_tag_get(tag, 1);
      if (!valid_hex64(val)) { parse_error = TRUE; break; }
      target = g_strdup(val);
    } else if (g_strcmp0(tag_name, GH_MLS_POLL_RESPONSE_TAG) == 0) {
      if (tag_len != 2) { parse_error = TRUE; break; }
      const gchar *val = nostr_tag_get(tag, 1);
      if (!valid_option_id(val)) { parse_error = TRUE; break; }
      g_ptr_array_add(selections, g_strdup(val));
    }
  }

  nostr_event_free(event);

  if (parse_error || !target || selections->len == 0) {
    g_free(target);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Malformed or missing vote tags");
    return FALSE;
  }

  /* Check for duplicate selections. */
  for (guint i = 0; i < selections->len; i++) {
    for (guint j = i + 1; j < selections->len; j++) {
      if (g_strcmp0(g_ptr_array_index(selections, i),
                    g_ptr_array_index(selections, j)) == 0) {
        g_free(target);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Duplicate selection");
        return FALSE;
      }
    }
  }

  gchar **ids = g_new0(gchar *, selections->len + 1);
  for (guint i = 0; i < selections->len; i++)
    ids[i] = g_strdup(g_ptr_array_index(selections, i));
  ids[selections->len] = NULL;

  *out_target_poll_id = target;
  *out_option_ids = ids;
  return TRUE;
}
