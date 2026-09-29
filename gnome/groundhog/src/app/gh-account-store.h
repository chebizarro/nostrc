#ifndef GH_ACCOUNT_STORE_H
#define GH_ACCOUNT_STORE_H

#include "gh-account-controller.h"
#include "gh-conversation-store.h"
#include "gh-dm-inbox.h"
#include "gh-store-conversations.h"
#include "gh-store-key.h"
#include "gh-store.h"

G_BEGIN_DECLS

/*
 * GhAccountStore: the active account's encrypted message store over its
 * lifetime (privacy charter §3.2-§3.5, §3.8, §5.3 B4, §8.2 G04). GTK-free;
 * main context only.
 *
 * Activation. For each account generation of the controller it:
 *  1. looks the store key up through GhStoreKey without ever prompting
 *     (GH_STORE_KEY_FLAGS_NONE; a lookup when store.db exists, so a lost key
 *     item is KEY_MISSING and never silently replaced, lookup-or-create
 *     otherwise, which stores the item before any file is created);
 *  2. opens (or creates) the SQLCipher store in a worker thread; a damaged
 *     store is reopened read-only (CORRUPT);
 *  3. imports the legacy plaintext NIP-17 seen file and inbox checkpoint of
 *     the account once, then deletes them (ST-12);
 *  4. attaches GhStoreConversations as the conversation model's persistence
 *     delegate, restoring the stored rooms;
 *  5. makes the account's per-store service (the durable outbox, G06) with
 *     create_outbox;
 *  6. grants the inbox storage for the generation (gh_dm_inbox_set_storage),
 *     so the inbox subscribes only now: every admitted message is durable.
 * A locked keyring, a missing Secret Service or any other failure stops
 * after step 1 or 2 with an explicit state and nothing written: there is no
 * plaintext fallback and the inbox stays unsubscribed (the wraps stay on the
 * relays). The user acts through gh_account_store_unlock() (the only call
 * that may show the keyring prompt), gh_account_store_retry(), or, only when
 * no Secret Service exists, gh_account_store_continue_without_saving(): an
 * in-memory store for this generation that writes no file at all (KC-4).
 *
 * Deactivation (switch, sign-out, shutdown) runs, in this order: the inbox
 * grant is withdrawn (checkpoint saved, REQs closed, pending unwraps and
 * signer approvals cancelled), the outbox is disposed (seals and publishes
 * cancelled, rows left resumable), the delegate is detached from the model
 * (the account's rooms are dropped) and closed, and the store is closed.
 * Only then may the next account's store open: operations are serialized,
 * so an open still running for the old generation finishes and its store is
 * closed first (PT-6). "store-opening" is emitted right before a store is
 * opened and "store-closed" right after one is closed, in that real order.
 *
 * Forget (charter §3.8, ST-8): gh_account_store_forget_async() closes the
 * account's store if it is open (as above), deletes its Secret Service key
 * item (crypto-shred), then unlinks its store directory and legacy state
 * files, and finally clears current-npub only if it names this account. The
 * signer's identity and every other setting are untouched.
 */

typedef enum {
  GH_ACCOUNT_STORE_INACTIVE,    /* no active account */
  GH_ACCOUNT_STORE_OPENING,     /* key lookup or open in progress (or queued) */
  GH_ACCOUNT_STORE_OPEN,        /* encrypted store open; inbox and outbox run */
  GH_ACCOUNT_STORE_EPHEMERAL,   /* in memory by the user's choice; nothing saved */
  GH_ACCOUNT_STORE_LOCKED,      /* keyring locked; gh_account_store_unlock() */
  GH_ACCOUNT_STORE_UNAVAILABLE, /* no Secret Service; nothing can be saved */
  GH_ACCOUNT_STORE_KEY_MISSING, /* store.db exists, its key item does not */
  GH_ACCOUNT_STORE_CORRUPT,     /* damaged: open read-only, nothing received or sent */
  GH_ACCOUNT_STORE_ERROR        /* anything else; see gh_account_store_get_error() */
} GhAccountStoreState;

GType gh_account_store_state_get_type(void);
#define GH_TYPE_ACCOUNT_STORE_STATE (gh_account_store_state_get_type())

/* Makes the per-store service of an open, writable store (the account's
 * GhOutbox in the application); it borrows store and is disposed with
 * g_object_run_dispose() and released before the store closes. NULL with
 * error: logged, and the store stays open without it. */
typedef GObject *(*GhAccountStoreOutboxFunc)(GhStore *store, gpointer user_data,
                                             GError **error);

typedef struct {
  GhAccountController *accounts;       /* required */
  GhStoreKey *store_key;               /* required: the process's one instance */
  GhConversationStore *conversations;  /* required: the model the store backs */
  GhDmInbox *inbox;                    /* nullable; from gh_dm_inbox_new_with_storage() */
  GSettings *settings;                 /* required: current-npub (forget) */
  const gchar *data_dir;               /* absolute; NULL = g_get_user_data_dir() */
  const gchar *legacy_state_dir;       /* pre-store GhDmInbox state; NULL = its default */
  GhClock *clock;                      /* NULL = system clock */
  GhAccountStoreOutboxFunc create_outbox; /* nullable */
  gpointer outbox_data;
} GhAccountStoreConfig;

/* The inbox checkpoint's cursor in the store (gh_store_get_cursor). */
#define GH_ACCOUNT_STORE_INBOX_CURSOR "nip17-inbox"

#define GH_TYPE_ACCOUNT_STORE (gh_account_store_get_type())
G_DECLARE_FINAL_TYPE(GhAccountStore, gh_account_store, GH, ACCOUNT_STORE, GObject)

/* Signals: "changed" whenever the state, the error or the open store
 * changes; "store-opening" (account pubkey hex) right before a store is
 * opened; "store-closed" (account pubkey hex) right after one closed. */
GhAccountStore *gh_account_store_new(const GhAccountStoreConfig *config);

GhAccountStoreState gh_account_store_get_state(GhAccountStore *self);
/* The reason for LOCKED, UNAVAILABLE, KEY_MISSING, CORRUPT and ERROR (never
 * containing the account pubkey), else NULL. */
const gchar *gh_account_store_get_error(GhAccountStore *self);
/* The generation and account (lowercase hex) the state belongs to; 0/NULL
 * when inactive. */
guint64 gh_account_store_get_generation(GhAccountStore *self);
const gchar *gh_account_store_get_account(GhAccountStore *self);
/* Borrowed; non-NULL only in OPEN, EPHEMERAL and CORRUPT. */
GhStore *gh_account_store_get_store(GhAccountStore *self);
GhStoreConversations *gh_account_store_get_conversations(GhAccountStore *self);
/* The create_outbox object, only in OPEN and EPHEMERAL. */
GObject *gh_account_store_get_outbox(GhAccountStore *self);

/* LOCKED: open again, allowing the keyring's unlock prompt. FALSE when not
 * LOCKED. */
gboolean gh_account_store_unlock(GhAccountStore *self);
/* LOCKED, UNAVAILABLE, KEY_MISSING or ERROR: open again without prompting.
 * FALSE otherwise. */
gboolean gh_account_store_retry(GhAccountStore *self);
/* UNAVAILABLE only ("Continue Without Saving Messages", charter §3.4): an
 * in-memory store for the current generation; nothing is written, and it is
 * gone when the account is switched or the app quits. A later activation
 * tries the Secret Service again. G_IO_ERROR_NOT_SUPPORTED otherwise. */
gboolean gh_account_store_continue_without_saving(GhAccountStore *self, GError **error);

/* Forget account (above). Errors: G_IO_ERROR_INVALID_ARGUMENT (not a 64-hex
 * pubkey); the key item's deletion error (e.g. GH_STORE_KEY_ERROR_LOCKED if
 * the keyring stayed locked: the files are unlinked anyway) or the unlink's
 * GH_STORE_ERROR. The keyring's unlock prompt may be shown. */
void gh_account_store_forget_async(GhAccountStore *self, const gchar *account_pubkey_hex,
                                   GCancellable *cancellable, GAsyncReadyCallback callback,
                                   gpointer user_data);
gboolean gh_account_store_forget_finish(GhAccountStore *self, GAsyncResult *result,
                                        GError **error);

/* "Start Fresh on This Device" (KEY_MISSING) and "Reset Storage" (CORRUPT),
 * after the user confirmed: crypto-shreds the active account's store as
 * forget does (current-npub is kept), then creates a new one. Fails with
 * G_IO_ERROR_NOT_SUPPORTED in any other state. */
void gh_account_store_start_fresh_async(GhAccountStore *self, GCancellable *cancellable,
                                        GAsyncReadyCallback callback, gpointer user_data);
gboolean gh_account_store_start_fresh_finish(GhAccountStore *self, GAsyncResult *result,
                                             GError **error);

G_END_DECLS
#endif
