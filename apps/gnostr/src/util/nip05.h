#ifndef GNOSTR_NIP05_H
#define GNOSTR_NIP05_H

#include <glib.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

/**
 * NIP-05 Verification Status
 */
typedef enum {
  GNOSTR_NIP05_STATUS_UNKNOWN = 0,   /* Not yet verified */
  GNOSTR_NIP05_STATUS_VERIFYING,     /* Verification in progress */
  GNOSTR_NIP05_STATUS_VERIFIED,      /* Successfully verified */
  GNOSTR_NIP05_STATUS_FAILED,        /* Verification failed */
  GNOSTR_NIP05_STATUS_INVALID        /* Invalid NIP-05 format */
} GnostrNip05Status;

/**
 * NIP-05 Verification Result
 */
typedef struct {
  GnostrNip05Status status;
  char *identifier;      /* The original NIP-05 identifier (e.g., "user@example.com") */
  char *pubkey_hex;      /* The verified pubkey (hex) */
  char **relays;         /* NULL-terminated array of relay URLs (optional) */
  gint64 verified_at;    /* Unix timestamp when verified */
  gint64 expires_at;     /* Unix timestamp when cache expires */
} GnostrNip05Result;

/**
 * Callback for async NIP-05 verification
 *
 * @param result The verification result (owned by caller after callback)
 * @param user_data User-provided data
 */
typedef void (*GnostrNip05VerifyCallback)(GnostrNip05Result *result, gpointer user_data);

/**
 * Parse NIP-05 identifier into local-part and domain.
 *
 * @param identifier The NIP-05 identifier (e.g., "user@example.com" or "_@example.com")
 * @param out_local Output for local-part (caller frees)
 * @param out_domain Output for domain (caller frees)
 * @return TRUE if valid format, FALSE otherwise
 */
gboolean gnostr_nip05_parse(const char *identifier, char **out_local, char **out_domain);

/**
 * Verify NIP-05 identifier asynchronously.
 *
 * @param identifier The NIP-05 identifier to verify
 * @param expected_pubkey The expected pubkey in hex (64 chars)
 * @param callback Callback to invoke when verification completes
 * @param user_data User data for callback
 * @param cancellable Optional GCancellable
 */
void gnostr_nip05_verify_async(const char *identifier,
                               const char *expected_pubkey,
                               GnostrNip05VerifyCallback callback,
                               gpointer user_data,
                               GCancellable *cancellable);

/**
 * Get cached NIP-05 verification result.
 *
 * @param identifier The NIP-05 identifier
 * @return Cached result or NULL if not cached/expired. Caller must free with gnostr_nip05_result_free().
 */
GnostrNip05Result *gnostr_nip05_cache_get(const char *identifier);

/**
 * Store NIP-05 verification result in cache.
 *
 * @param result The result to cache (takes ownership)
 */
void gnostr_nip05_cache_put(GnostrNip05Result *result);

/**
 * Free a NIP-05 verification result.
 *
 * @param result The result to free
 */
void gnostr_nip05_result_free(GnostrNip05Result *result);

/**
 * Create a verified NIP-05 badge widget (checkmark icon).
 *
 * @return A new GtkImage with verification badge styling
 */
GtkWidget *gnostr_nip05_create_badge(void);

/**
 * Get display string for NIP-05 (e.g., "@user" for "_@domain.com" or "user@domain.com")
 *
 * @param identifier The NIP-05 identifier
 * @return Display string (caller frees)
 */
char *gnostr_nip05_get_display(const char *identifier);

/**
 * Clear expired entries from the NIP-05 cache.
 */
void gnostr_nip05_cache_cleanup(void);

/**
 * Get verification status string for debugging.
 *
 * @param status The status enum value
 * @return Static string describing the status
 */
const char *gnostr_nip05_status_to_string(GnostrNip05Status status);

G_END_DECLS

#endif /* GNOSTR_NIP05_H */
