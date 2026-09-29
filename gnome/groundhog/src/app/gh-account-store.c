#include "gh-account-store.h"

#include "gh-identity.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

/* The pre-store GhDmInbox checkpoint file (<state_dir>/<pubkey>.checkpoint). */
#define LEGACY_CHECKPOINT_MAGIC "groundhog-dm-inbox-checkpoint 1"

typedef enum { OP_OPEN, OP_SHRED } OpKind;

/* The one store operation in flight. Operations are strictly serialized so
 * that a store is always closed before the next one opens (PT-6). */
typedef struct {
  OpKind kind;
  GhAccountStore *owner;       /* strong: the store outlives its op */
  guint64 generation;       /* OP_OPEN: the binding it opens for */
  gchar *account;
  GhStoreKeyFlags flags;
  gboolean exists;          /* OP_OPEN: store.db was present */
  GCancellable *cancellable;
  GTask *task;              /* OP_SHRED: the caller's task */
  gboolean clear_current;   /* OP_SHRED: forget (vs. start fresh) */
} Op;

/* A worker-thread open (charter: open in a GTask worker, then hand over). */
typedef struct {
  gchar *data_dir;
  gchar *account;
  GhClock *clock;
  GBytes *key;
  gchar *store_id;
  gboolean create;
  GhStore *store;           /* the result until handed over */
  gboolean corrupt;
} OpenJob;

struct _GhAccountStore {
  GObject parent_instance;
  GhAccountController *accounts; /* NULL once disposed */
  GhStoreKey *store_key;
  GhConversationStore *model;
  GhDmInbox *inbox;
  GSettings *settings;
  gchar *data_dir;
  gchar *legacy_state_dir;
  GhClock *clock;
  GhAccountStoreOutboxFunc create_outbox;
  gpointer outbox_data;

  /* The binding: one account generation. */
  guint64 generation;
  gchar *account;
  gboolean want_open;             /* start an open when idle */
  GhStoreKeyFlags open_flags;     /* for that open */
  GhAccountStoreState state;
  gchar *error;
  GhStore *store;
  GhStoreConversations *conversations;
  GObject *outbox;

  Op *op;                         /* in flight, or NULL */
  GQueue shreds;                  /* Op (OP_SHRED) waiting their turn */
};

enum { SIGNAL_CHANGED, SIGNAL_STORE_OPENING, SIGNAL_STORE_CLOSED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhAccountStore, gh_account_store, G_TYPE_OBJECT)

GType
gh_account_store_state_get_type(void)
{
  static gsize type = 0;
  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { GH_ACCOUNT_STORE_INACTIVE, "GH_ACCOUNT_STORE_INACTIVE", "inactive" },
      { GH_ACCOUNT_STORE_OPENING, "GH_ACCOUNT_STORE_OPENING", "opening" },
      { GH_ACCOUNT_STORE_OPEN, "GH_ACCOUNT_STORE_OPEN", "open" },
      { GH_ACCOUNT_STORE_EPHEMERAL, "GH_ACCOUNT_STORE_EPHEMERAL", "ephemeral" },
      { GH_ACCOUNT_STORE_LOCKED, "GH_ACCOUNT_STORE_LOCKED", "locked" },
      { GH_ACCOUNT_STORE_UNAVAILABLE, "GH_ACCOUNT_STORE_UNAVAILABLE", "unavailable" },
      { GH_ACCOUNT_STORE_KEY_MISSING, "GH_ACCOUNT_STORE_KEY_MISSING", "key-missing" },
      { GH_ACCOUNT_STORE_CORRUPT, "GH_ACCOUNT_STORE_CORRUPT", "corrupt" },
      { GH_ACCOUNT_STORE_ERROR, "GH_ACCOUNT_STORE_ERROR", "error" },
      { 0, NULL, NULL }
    };
    g_once_init_leave(&type, g_enum_register_static(
      g_intern_static_string("GhAccountStoreState"), values));
  }
  return type;
}

static void reconcile(GhAccountStore *self);

static void
op_free(Op *op)
{
  if (!op)
    return;
  g_clear_object(&op->owner);
  g_clear_object(&op->cancellable);
  g_clear_object(&op->task);
  g_free(op->account);
  g_free(op);
}

static void
open_job_free(gpointer data)
{
  OpenJob *job = data;
  /* A store the main context never took over (it stopped first). */
  if (job->store)
    gh_store_close(job->store);
  g_clear_pointer(&job->key, g_bytes_unref);
  g_clear_pointer(&job->clock, gh_clock_unref);
  g_free(job->store_id);
  g_free(job->account);
  g_free(job->data_dir);
  g_free(job);
}

/* ---- state -------------------------------------------------------------- */

static void
set_state(GhAccountStore *self, GhAccountStoreState state, const gchar *error)
{
  if (self->state == state && g_strcmp0(self->error, error) == 0)
    return;
  self->state = state;
  g_free(self->error);
  self->error = g_strdup(error);
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

/* The state an open failure leaves (charter §3.4, §7.15). */
static GhAccountStoreState
failure_state(const GError *error)
{
  if (error->domain == GH_STORE_KEY_ERROR) {
    switch (error->code) {
    case GH_STORE_KEY_ERROR_UNAVAILABLE: return GH_ACCOUNT_STORE_UNAVAILABLE;
    case GH_STORE_KEY_ERROR_LOCKED: return GH_ACCOUNT_STORE_LOCKED;
    case GH_STORE_KEY_ERROR_NOT_FOUND: return GH_ACCOUNT_STORE_KEY_MISSING;
    default: return GH_ACCOUNT_STORE_ERROR;
    }
  }
  if (error->domain == GH_STORE_ERROR) {
    switch (error->code) {
    case GH_STORE_ERROR_UNAVAILABLE: return GH_ACCOUNT_STORE_UNAVAILABLE;
    case GH_STORE_ERROR_LOCKED: return GH_ACCOUNT_STORE_LOCKED;
    case GH_STORE_ERROR_KEY_MISSING: return GH_ACCOUNT_STORE_KEY_MISSING;
    default: return GH_ACCOUNT_STORE_ERROR;
    }
  }
  return GH_ACCOUNT_STORE_ERROR;
}

static gboolean
shred_pending_for(GhAccountStore *self, const gchar *account)
{
  for (GList *l = self->shreds.head; l; l = l->next)
    if (g_strcmp0(((Op *)l->data)->account, account) == 0)
      return TRUE;
  return FALSE;
}

/* ---- inbox storage grant ------------------------------------------------- */

static gint64
inbox_load_checkpoint(gpointer data)
{
  GhAccountStore *self = data;
  g_autoptr(GError) error = NULL;
  gint64 since = 0;
  if (!self->store ||
      !gh_store_get_cursor(self->store, GH_ACCOUNT_STORE_INBOX_CURSOR, "", &since, &error)) {
    if (error)
      g_message("Groundhog could not read the DM inbox checkpoint: %s", error->message);
    return 0;
  }
  return since;
}

static gboolean
inbox_save_checkpoint(gpointer data, gint64 checkpoint, GError **error)
{
  GhAccountStore *self = data;
  if (!self->store) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE, "The store is closed");
    return FALSE;
  }
  return gh_store_set_cursor(self->store, GH_ACCOUNT_STORE_INBOX_CURSOR, "", checkpoint, error);
}

static const GhDmInboxStorage inbox_storage = { inbox_load_checkpoint, inbox_save_checkpoint };

/* ---- close ---------------------------------------------------------------- */

static void
close_store(GhAccountStore *self, GhStore *store)
{
  g_autofree gchar *account = g_strdup(gh_store_get_account_pubkey(store));
  gh_store_close(store);
  g_signal_emit(self, signals[SIGNAL_STORE_CLOSED], 0, account);
}

/* §3.8 steps 1-2 and PT-6: receiving stops, sending stops, the model lets
 * go of the account, and the store closes, in this order. */
static void
unbind_store(GhAccountStore *self)
{
  if (self->inbox)
    gh_dm_inbox_clear_storage(self->inbox);
  if (self->outbox) {
    g_object_run_dispose(self->outbox);
    g_clear_object(&self->outbox);
  }
  if (self->conversations) {
    if (g_strcmp0(gh_conversation_store_get_account(self->model), self->account) == 0)
      gh_conversation_store_set_account(self->model, NULL, NULL, NULL, NULL);
    gh_store_conversations_close(self->conversations);
    g_clear_object(&self->conversations);
  }
  if (self->store)
    close_store(self, g_steal_pointer(&self->store));
}

/* ---- legacy import (ST-12) -------------------------------------------------- */

static gchar *
legacy_path(GhAccountStore *self, const gchar *account, const gchar *suffix)
{
  g_autofree gchar *name = g_strconcat(account, suffix, NULL);
  return self->legacy_state_dir ? g_build_filename(self->legacy_state_dir, name, NULL)
                                : g_build_filename(g_get_user_state_dir(), "groundhog",
                                                   "nip17", name, NULL);
}

/* The old inbox's plaintext checkpoint becomes the store's cursor (unless
 * the store already has one), then the file is deleted. */
static void
import_legacy_checkpoint(GhAccountStore *self)
{
  g_autofree gchar *path = legacy_path(self, self->account, ".checkpoint");
  g_autofree gchar *contents = NULL;
  gsize length = 0;
  if (!g_file_get_contents(path, &contents, &length, NULL))
    return;
  g_autofree gchar *prefix = g_strdup_printf(LEGACY_CHECKPOINT_MAGIC " %s ", self->account);
  gint64 value = 0;
  if (length <= 256 && g_str_has_prefix(contents, prefix) && g_str_has_suffix(contents, "\n")) {
    g_autofree gchar *number = g_strndup(contents + strlen(prefix),
                                         length - strlen(prefix) - 1);
    if (!g_ascii_string_to_signed(number, 10, 1, G_MAXINT64, &value, NULL))
      value = 0;
  }
  g_autoptr(GError) error = NULL;
  gint64 current = 0;
  if (!gh_store_get_cursor(self->store, GH_ACCOUNT_STORE_INBOX_CURSOR, "", &current, &error) ||
      (value > 0 && current == 0 &&
       !gh_store_set_cursor(self->store, GH_ACCOUNT_STORE_INBOX_CURSOR, "", value, &error))) {
    g_message("Groundhog could not import the old DM inbox checkpoint: %s", error->message);
    return;
  }
  if (g_unlink(path) != 0 && errno != ENOENT)
    g_message("Groundhog could not delete the old DM inbox checkpoint: %s", g_strerror(errno));
}

static void
import_legacy(GhAccountStore *self)
{
  g_autofree gchar *seen = gh_store_conversations_legacy_seen_path(self->legacy_state_dir,
                                                                   self->account);
  g_autoptr(GError) error = NULL;
  GhStoreSeenImport stats = { 0 };
  /* A refused file (planted, foreign, malformed) stays where it is; the
   * inbox then only asks the signer again about wraps it held. */
  if (!gh_store_conversations_import_seen_file(self->conversations, seen, &stats, &error))
    g_message("Groundhog did not import the old DM seen file: %s", error->message);
  else if (stats.wraps || stats.rumors || stats.rejected)
    g_debug("Groundhog imported %u wrap, %u rumor and %u rejected ids from the old seen file",
            stats.wraps, stats.rumors, stats.rejected);
  import_legacy_checkpoint(self);
}

/* ---- bind ------------------------------------------------------------------ */

static void
bind_store(GhAccountStore *self, GhStore *store, gboolean corrupt)
{
  g_autoptr(GError) error = NULL;
  gboolean ephemeral = gh_store_is_ephemeral(store);
  self->store = store;
  self->conversations = gh_store_conversations_new(store);
  if (!corrupt && !ephemeral)
    import_legacy(self);
  if (!gh_store_conversations_attach(self->conversations, self->model, 0, &error)) {
    /* The rooms restored so far stay listed and the delegate stays bound. */
    g_warning("Groundhog could not restore every stored conversation: %s", error->message);
    g_clear_error(&error);
  }
  if (!corrupt && self->create_outbox) {
    self->outbox = self->create_outbox(store, self->outbox_data, &error);
    if (!self->outbox) {
      g_warning("Groundhog could not start the message outbox: %s",
                error ? error->message : "unknown error");
      g_clear_error(&error);
    }
  }
  if (!corrupt && self->inbox &&
      !gh_dm_inbox_set_storage(self->inbox, self->generation, &inbox_storage, self))
    g_message("Groundhog did not start the DM inbox: the account changed meanwhile");
  if (corrupt)
    set_state(self, GH_ACCOUNT_STORE_CORRUPT,
              "The message storage failed its integrity check and is read-only");
  else
    set_state(self, ephemeral ? GH_ACCOUNT_STORE_EPHEMERAL : GH_ACCOUNT_STORE_OPEN, NULL);
}

/* ---- open ------------------------------------------------------------------ */

/* The next operation may start; the finished one is freed last, since it may
 * hold the last reference to self. */
static void
finish_op(GhAccountStore *self)
{
  Op *op = g_steal_pointer(&self->op);
  reconcile(self);
  op_free(op);
}

/* Whether an open's result still belongs to the binding it was made for. */
static gboolean
open_current(GhAccountStore *self, Op *op)
{
  return self->accounts && op == self->op && op->generation == self->generation &&
         !g_cancellable_is_cancelled(op->cancellable) &&
         !shred_pending_for(self, op->account);
}

static void
open_failed(GhAccountStore *self, const GError *error)
{
  self->want_open = FALSE;
  set_state(self, failure_state(error), error->message);
}

static void
open_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  OpenJob *job = data;
  GError *error = NULL;
  GhStoreConfig config = { job->data_dir, job->account, NULL, NULL, job->clock };
  (void)source;
  (void)cancellable;
  job->store = gh_store_open_with_key(&config, job->key, job->store_id,
                                      job->create ? GH_STORE_OPEN_CREATE : GH_STORE_OPEN_NONE,
                                      &error);
  if (!job->store && !job->create &&
      g_error_matches(error, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT)) {
    /* STORE_CORRUPT: show what is still readable before "Reset Storage". */
    g_clear_error(&error);
    job->store = gh_store_open_with_key(&config, job->key, job->store_id,
                                        GH_STORE_OPEN_ALLOW_CORRUPT, &error);
    job->corrupt = job->store != NULL;
  }
  /* The key leaves memory as soon as the store is keyed. */
  g_clear_pointer(&job->key, g_bytes_unref);
  if (job->store)
    g_task_return_boolean(task, TRUE);
  else
    g_task_return_error(task, error);
}

static void
on_opened(GObject *source, GAsyncResult *result, gpointer data)
{
  GhAccountStore *self = GH_ACCOUNT_STORE(source);
  Op *op = data;
  g_autoptr(GError) error = NULL;
  OpenJob *job = g_task_get_task_data(G_TASK(result));
  gboolean ok = g_task_propagate_boolean(G_TASK(result), &error);
  GhStore *store = g_steal_pointer(&job->store);
  g_assert(op == self->op);
  if (!open_current(self, op)) {
    /* Stale: close it before anything else may open (PT-6). */
    if (store)
      close_store(self, store);
    finish_op(self);
    return;
  }
  if (!ok)
    open_failed(self, error);
  else
    bind_store(self, store, job->corrupt);
  finish_op(self);
}

static void
start_worker(GhAccountStore *self, Op *op, GBytes *key, const gchar *store_id)
{
  OpenJob *job = g_new0(OpenJob, 1);
  job->data_dir = g_strdup(self->data_dir);
  job->account = g_strdup(op->account);
  job->clock = self->clock ? gh_clock_ref(self->clock) : NULL;
  job->key = g_bytes_ref(key);
  job->store_id = g_strdup(store_id);
  job->create = !op->exists;
  g_signal_emit(self, signals[SIGNAL_STORE_OPENING], 0, op->account);
  GTask *task = g_task_new(self, NULL, on_opened, op);
  g_task_set_source_tag(task, start_worker);
  g_task_set_task_data(task, job, open_job_free);
  g_task_run_in_thread(task, open_worker);
  g_object_unref(task);
}

static void
on_key(GObject *source, GAsyncResult *result, gpointer data)
{
  Op *op = data;
  GhStoreKey *store_key = GH_STORE_KEY(source);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *store_id = NULL;
  g_autoptr(GBytes) key = op->exists
    ? gh_store_key_lookup_finish(store_key, result, &store_id, &error)
    : gh_store_key_lookup_or_create_finish(store_key, result, &store_id, NULL, &error);
  GhAccountStore *self = op->owner;
  g_assert(op == self->op);
  if (!open_current(self, op)) {
    finish_op(self);
    return;
  }
  if (key) {
    start_worker(self, op, key, store_id);
    return;
  }
  open_failed(self, error);
  finish_op(self);
}

static void
start_open(GhAccountStore *self)
{
  g_autoptr(GError) error = NULL;
  Op *op = g_new0(Op, 1);
  op->kind = OP_OPEN;
  op->owner = g_object_ref(self);
  op->generation = self->generation;
  op->account = g_strdup(self->account);
  op->flags = self->open_flags;
  op->cancellable = g_cancellable_new();
  self->op = op;
  self->want_open = FALSE;
  self->open_flags = GH_STORE_KEY_FLAGS_NONE;
  set_state(self, GH_ACCOUNT_STORE_OPENING, NULL);
  /* §3.4: with a store on disk only a lookup (a lost item is KEY_MISSING),
   * otherwise lookup-or-create (the item is stored before any file). */
  if (!gh_store_exists(self->data_dir, op->account, &op->exists, &error)) {
    open_failed(self, error);
    finish_op(self);
    return;
  }
  if (op->exists)
    gh_store_key_lookup_async(self->store_key, op->account, op->flags, op->cancellable,
                              on_key, op);
  else
    gh_store_key_lookup_or_create_async(self->store_key, op->account, op->flags,
                                        op->cancellable, on_key, op);
}

/* ---- crypto-shred (forget, start fresh) --------------------------------------- */

static gboolean
delete_legacy_files(GhAccountStore *self, const gchar *account, GError **error)
{
  static const gchar *const suffixes[] = { ".seen", ".checkpoint" };
  for (guint i = 0; i < G_N_ELEMENTS(suffixes); i++) {
    g_autofree gchar *path = legacy_path(self, account, suffixes[i]);
    if (g_unlink(path) != 0 && errno != ENOENT) {
      int saved = errno;
      g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved),
                  "Could not delete the old DM state file: %s", g_strerror(saved));
      return FALSE;
    }
  }
  return TRUE;
}

static void
on_key_destroyed(GObject *source, GAsyncResult *result, gpointer data)
{
  Op *op = data;
  GhAccountStore *self = op->owner;
  g_assert(op == self->op);
  g_autoptr(GError) key_error = NULL;
  g_autoptr(GError) file_error = NULL;
  g_assert(op && op->kind == OP_SHRED);
  gboolean key_ok = gh_store_key_destroy_finish(GH_STORE_KEY(source), result, &key_error);
  /* §3.8 step 3: the key first, then the files, which are unlinked even if
   * the key item could not be deleted (it then no longer opens anything). */
  gboolean files_ok = gh_store_delete_files(self->data_dir, op->account, &file_error) &&
                      delete_legacy_files(self, op->account, &file_error);
  GTask *task = g_steal_pointer(&op->task);
  gboolean current = self->accounts && g_strcmp0(op->account, self->account) == 0;
  if (op->clear_current) {
    /* Step 4: only if current-npub names this account. */
    g_autofree gchar *npub = g_settings_get_string(self->settings, "current-npub");
    g_autofree gchar *hex = npub && *npub ? gh_identity_pubkey_hex(npub) : NULL;
    if (hex && g_ascii_strcasecmp(hex, op->account) == 0)
      g_settings_set_string(self->settings, "current-npub", "");
  } else if (current) {
    /* Start fresh: a new store for the same, still active account. */
    self->want_open = TRUE;
  }
  /* Idle again before the caller hears back. */
  finish_op(self);
  if (!key_ok)
    g_task_return_error(task, g_steal_pointer(&key_error));
  else if (!files_ok)
    g_task_return_error(task, g_steal_pointer(&file_error));
  else
    g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

static void
start_shred(GhAccountStore *self, Op *op)
{
  op->owner = g_object_ref(self);
  self->op = op;
  if (g_strcmp0(op->account, self->account) == 0) {
    unbind_store(self);
    self->want_open = FALSE;
    set_state(self, op->clear_current ? GH_ACCOUNT_STORE_INACTIVE : GH_ACCOUNT_STORE_OPENING,
              NULL);
  }
  /* User-initiated: the keyring may ask to be unlocked. */
  gh_store_key_destroy_async(self->store_key, op->account, GH_STORE_KEY_FLAGS_INTERACTIVE,
                             g_task_get_cancellable(op->task), on_key_destroyed, op);
}

static void
queue_shred(GhAccountStore *self, const gchar *account, gboolean clear_current,
            GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data,
            gpointer source_tag)
{
  Op *op = g_new0(Op, 1);
  op->kind = OP_SHRED;
  op->account = g_ascii_strdown(account, -1);
  op->clear_current = clear_current;
  op->task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(op->task, source_tag);
  /* An open of this account in flight is abandoned (its store, if the
   * worker already opened one, is closed when it returns). */
  if (self->op && self->op->kind == OP_OPEN && g_strcmp0(self->op->account, op->account) == 0)
    g_cancellable_cancel(self->op->cancellable);
  g_queue_push_tail(&self->shreds, op);
  reconcile(self);
}

/* ---- reconcile --------------------------------------------------------------- */

static void
reconcile(GhAccountStore *self)
{
  if (!self->accounts)
    return;
  guint64 generation = 0;
  g_autofree gchar *account = NULL;
  if (gh_account_controller_get_state(self->accounts) == GH_ACCOUNT_STATE_ACTIVE) {
    account = gh_identity_pubkey_hex(gh_account_controller_get_active_npub(self->accounts));
    if (account)
      generation = gh_account_controller_get_generation(self->accounts);
  }
  if (generation != self->generation) {
    if (self->op && self->op->kind == OP_OPEN)
      g_cancellable_cancel(self->op->cancellable);
    unbind_store(self);
    self->generation = generation;
    g_free(self->account);
    self->account = generation ? g_ascii_strdown(account, -1) : NULL;
    self->want_open = generation != 0;
    self->open_flags = GH_STORE_KEY_FLAGS_NONE;
    set_state(self, generation ? GH_ACCOUNT_STORE_OPENING : GH_ACCOUNT_STORE_INACTIVE, NULL);
  }
  if (self->op)
    return;
  if (!g_queue_is_empty(&self->shreds)) {
    start_shred(self, g_queue_pop_head(&self->shreds));
    return;
  }
  if (self->generation && self->want_open && !self->store)
    start_open(self);
}

/* ---- public ------------------------------------------------------------------ */

GhAccountStore *
gh_account_store_new(const GhAccountStoreConfig *config)
{
  g_return_val_if_fail(config != NULL, NULL);
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(config->accounts), NULL);
  g_return_val_if_fail(GH_IS_STORE_KEY(config->store_key), NULL);
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(config->conversations), NULL);
  g_return_val_if_fail(!config->inbox || GH_IS_DM_INBOX(config->inbox), NULL);
  g_return_val_if_fail(G_IS_SETTINGS(config->settings), NULL);
  g_return_val_if_fail(!config->data_dir || g_path_is_absolute(config->data_dir), NULL);
  GhAccountStore *self = g_object_new(GH_TYPE_ACCOUNT_STORE, NULL);
  self->accounts = g_object_ref(config->accounts);
  self->store_key = g_object_ref(config->store_key);
  self->model = g_object_ref(config->conversations);
  self->inbox = config->inbox ? g_object_ref(config->inbox) : NULL;
  self->settings = g_object_ref(config->settings);
  self->data_dir = g_strdup(config->data_dir);
  self->legacy_state_dir = g_strdup(config->legacy_state_dir);
  self->clock = config->clock ? gh_clock_ref(config->clock) : NULL;
  self->create_outbox = config->create_outbox;
  self->outbox_data = config->outbox_data;
  g_signal_connect_object(self->accounts, "changed", G_CALLBACK(reconcile), self,
                          G_CONNECT_SWAPPED);
  reconcile(self);
  return self;
}

GhAccountStoreState
gh_account_store_get_state(GhAccountStore *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_STORE(self), GH_ACCOUNT_STORE_INACTIVE);
  return self->state;
}

const gchar *
gh_account_store_get_error(GhAccountStore *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_STORE(self), NULL);
  return self->error;
}

guint64
gh_account_store_get_generation(GhAccountStore *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_STORE(self), 0);
  return self->generation;
}

const gchar *
gh_account_store_get_account(GhAccountStore *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_STORE(self), NULL);
  return self->account;
}

GhStore *
gh_account_store_get_store(GhAccountStore *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_STORE(self), NULL);
  return self->store;
}

GhStoreConversations *
gh_account_store_get_conversations(GhAccountStore *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_STORE(self), NULL);
  return self->conversations;
}

GObject *
gh_account_store_get_outbox(GhAccountStore *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_STORE(self), NULL);
  return self->outbox;
}

static gboolean
reopen(GhAccountStore *self, GhStoreKeyFlags flags)
{
  self->want_open = TRUE;
  self->open_flags = flags;
  set_state(self, GH_ACCOUNT_STORE_OPENING, NULL);
  reconcile(self);
  return TRUE;
}

gboolean
gh_account_store_unlock(GhAccountStore *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_STORE(self), FALSE);
  if (!self->accounts || self->op || self->state != GH_ACCOUNT_STORE_LOCKED)
    return FALSE;
  return reopen(self, GH_STORE_KEY_FLAGS_INTERACTIVE);
}

gboolean
gh_account_store_retry(GhAccountStore *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_STORE(self), FALSE);
  if (!self->accounts || self->op)
    return FALSE;
  switch (self->state) {
  case GH_ACCOUNT_STORE_LOCKED:
  case GH_ACCOUNT_STORE_UNAVAILABLE:
  case GH_ACCOUNT_STORE_KEY_MISSING:
  case GH_ACCOUNT_STORE_ERROR:
    return reopen(self, GH_STORE_KEY_FLAGS_NONE);
  default:
    return FALSE;
  }
}

gboolean
gh_account_store_continue_without_saving(GhAccountStore *self, GError **error)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_STORE(self), FALSE);
  if (!self->accounts || self->op || self->state != GH_ACCOUNT_STORE_UNAVAILABLE) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        "Messages can be kept in memory only when no keyring is available");
    return FALSE;
  }
  g_signal_emit(self, signals[SIGNAL_STORE_OPENING], 0, self->account);
  GhStore *store = gh_store_open_ephemeral(self->account, self->clock, error);
  if (!store)
    return FALSE;
  bind_store(self, store, FALSE);
  return TRUE;
}

void
gh_account_store_forget_async(GhAccountStore *self, const gchar *account_pubkey_hex,
                              GCancellable *cancellable, GAsyncReadyCallback callback,
                              gpointer user_data)
{
  g_return_if_fail(GH_IS_ACCOUNT_STORE(self));
  gboolean valid = account_pubkey_hex && strlen(account_pubkey_hex) == 64;
  for (guint i = 0; valid && account_pubkey_hex[i]; i++)
    valid = g_ascii_isxdigit(account_pubkey_hex[i]);
  if (!valid || !self->accounts) {
    g_task_report_new_error(self, callback, user_data, gh_account_store_forget_async,
                            G_IO_ERROR, valid ? G_IO_ERROR_CLOSED : G_IO_ERROR_INVALID_ARGUMENT,
                            valid ? "Groundhog is shutting down" : "Not an account public key");
    return;
  }
  queue_shred(self, account_pubkey_hex, TRUE, cancellable, callback, user_data,
              gh_account_store_forget_async);
}

gboolean
gh_account_store_forget_finish(GhAccountStore *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void
gh_account_store_start_fresh_async(GhAccountStore *self, GCancellable *cancellable,
                                   GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_ACCOUNT_STORE(self));
  if (!self->accounts || self->op ||
      (self->state != GH_ACCOUNT_STORE_KEY_MISSING && self->state != GH_ACCOUNT_STORE_CORRUPT)) {
    g_task_report_new_error(self, callback, user_data, gh_account_store_start_fresh_async,
                            G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Only missing-key or damaged message storage can be started fresh");
    return;
  }
  queue_shred(self, self->account, FALSE, cancellable, callback, user_data,
              gh_account_store_start_fresh_async);
}

gboolean
gh_account_store_start_fresh_finish(GhAccountStore *self, GAsyncResult *result,
                                    GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* ---- GObject ------------------------------------------------------------------ */

static void
gh_account_store_dispose(GObject *object)
{
  GhAccountStore *self = GH_ACCOUNT_STORE(object);
  if (self->accounts) {
    g_signal_handlers_disconnect_by_data(self->accounts, self);
    /* Shutdown runs the same ordered teardown as a switch. */
    if (self->op && self->op->kind == OP_OPEN)
      g_cancellable_cancel(self->op->cancellable);
    unbind_store(self);
    Op *op;
    while ((op = g_queue_pop_head(&self->shreds))) {
      g_task_return_new_error(op->task, G_IO_ERROR, G_IO_ERROR_CLOSED,
                              "Groundhog is shutting down");
      op_free(op);
    }
    self->generation = 0;
    g_clear_pointer(&self->account, g_free);
    self->state = GH_ACCOUNT_STORE_INACTIVE;
    g_clear_object(&self->accounts);
  }
  G_OBJECT_CLASS(gh_account_store_parent_class)->dispose(object);
}

static void
gh_account_store_finalize(GObject *object)
{
  GhAccountStore *self = GH_ACCOUNT_STORE(object);
  /* An op still in flight holds a reference, so none is left here. */
  g_assert(self->op == NULL);
  g_clear_object(&self->store_key);
  g_clear_object(&self->model);
  g_clear_object(&self->inbox);
  g_clear_object(&self->settings);
  g_clear_pointer(&self->clock, gh_clock_unref);
  g_free(self->data_dir);
  g_free(self->legacy_state_dir);
  g_free(self->error);
  G_OBJECT_CLASS(gh_account_store_parent_class)->finalize(object);
}

static void
gh_account_store_class_init(GhAccountStoreClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_account_store_dispose;
  object_class->finalize = gh_account_store_finalize;
  signals[SIGNAL_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass),
                                         G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 0);
  signals[SIGNAL_STORE_OPENING] = g_signal_new("store-opening", G_TYPE_FROM_CLASS(klass),
                                               G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                               G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_STORE_CLOSED] = g_signal_new("store-closed", G_TYPE_FROM_CLASS(klass),
                                              G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                              G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gh_account_store_init(GhAccountStore *self)
{
  self->state = GH_ACCOUNT_STORE_INACTIVE;
  g_queue_init(&self->shreds);
}
