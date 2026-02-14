/* secure-delete.h - Secure file and memory deletion for gnostr-signer
 *
 * This module provides secure deletion functionality to prevent recovery of
 * sensitive data such as private keys, passwords, and cryptographic material.
 *
 * Features:
 * - Multi-pass file overwriting (zeros, ones, random data)
 * - File renaming before deletion to obscure original filename
 * - Filesystem sync to ensure data reaches disk
 * - SSD-aware deletion with TRIM support where available
 * - Memory shredding that prevents compiler optimization
 * - Audit logging for security compliance (without sensitive data)
 *
 * Security Notes:
 * - On SSDs, secure deletion is not guaranteed due to wear leveling
 * - Full disk encryption is recommended as a complementary measure
 * - This module provides defense-in-depth, not absolute guarantees
 *
 * Related: secure-memory.h for secure memory allocation
 */
#ifndef APPS_GNOSTR_SIGNER_SECURE_DELETE_H
#define APPS_GNOSTR_SIGNER_SECURE_DELETE_H

#include <glib.h>
#include <stdbool.h>
#include <stddef.h>

G_BEGIN_DECLS

/* ============================================================
 * Result Codes
 * ============================================================ */

typedef enum {
  GN_DELETE_OK = 0,           /* Operation successful */
  GN_DELETE_ERR_NOT_FOUND,    /* File or directory not found */
  GN_DELETE_ERR_PERMISSION,   /* Permission denied */
  GN_DELETE_ERR_IO,           /* I/O error during operation */
  GN_DELETE_ERR_BUSY,         /* File is locked or in use */
  GN_DELETE_ERR_NOT_FILE,     /* Path is not a regular file */
  GN_DELETE_ERR_NOT_DIR,      /* Path is not a directory */
  GN_DELETE_ERR_NOT_EMPTY,    /* Directory not empty (non-recursive) */
  GN_DELETE_ERR_INVALID,      /* Invalid parameter */
  GN_DELETE_ERR_TRIM_FAILED   /* TRIM operation failed (non-fatal) */
} GnDeleteResult;

/* Convert result code to human-readable string */
const char *gn_delete_result_to_string(GnDeleteResult result);

/* ============================================================
 * Deletion Options
 * ============================================================ */

/* Number of overwrite passes for secure deletion */
typedef enum {
  GN_DELETE_PASSES_FAST = 1,      /* Single zero pass (fastest, least secure) */
  GN_DELETE_PASSES_STANDARD = 3,  /* zeros, ones, random (recommended) */
  GN_DELETE_PASSES_PARANOID = 7,  /* Extended passes for maximum security */
  GN_DELETE_PASSES_DOD = 7        /* DoD 5220.22-M compliant (same as paranoid) */
} GnDeletePasses;

/* Options for secure deletion operations */
typedef struct {
  GnDeletePasses passes;          /* Number of overwrite passes */
  gboolean rename_before_delete;  /* Rename to random name before unlinking */
  gboolean sync_after_write;      /* fsync() after each overwrite pass */
  gboolean try_trim;              /* Attempt TRIM on SSDs */
  gboolean recursive;             /* For directories: delete recursively */
  gboolean follow_symlinks;       /* Follow symbolic links (dangerous!) */
  gboolean preserve_timestamps;   /* Don't modify timestamps during wipe */
  gsize buffer_size;              /* Buffer size for I/O (0 = default 64KB) */
} GnDeleteOptions;

/* Default options (recommended for most use cases) */
#define GN_DELETE_OPTIONS_DEFAULT { \
  .passes = GN_DELETE_PASSES_STANDARD, \
  .rename_before_delete = TRUE, \
  .sync_after_write = TRUE, \
  .try_trim = TRUE, \
  .recursive = FALSE, \
  .follow_symlinks = FALSE, \
  .preserve_timestamps = FALSE, \
  .buffer_size = 0 \
}

/* Fast options (for less sensitive data or bulk operations) */
#define GN_DELETE_OPTIONS_FAST { \
  .passes = GN_DELETE_PASSES_FAST, \
  .rename_before_delete = TRUE, \
  .sync_after_write = FALSE, \
  .try_trim = TRUE, \
  .recursive = FALSE, \
  .follow_symlinks = FALSE, \
  .preserve_timestamps = FALSE, \
  .buffer_size = 0 \
}

/* Get a copy of the default options */
GnDeleteOptions gn_delete_options_default(void);

/* ============================================================
 * File Deletion
 * ============================================================ */

/**
 * gn_secure_delete_file:
 * @filepath: Path to the file to delete
 *
 * Securely deletes a file using default options (3-pass overwrite).
 *
 * The file content is overwritten with:
 * 1. All zeros (0x00)
 * 2. All ones (0xFF)
 * 3. Random data
 *
 * After overwriting, the file is renamed to a random name and unlinked.
 * fsync() is called after each pass to ensure data reaches disk.
 *
 * Returns: GN_DELETE_OK on success, error code otherwise
 */
GnDeleteResult gn_secure_delete_file(const char *filepath);

/**
 * gn_secure_delete_file_opts:
 * @filepath: Path to the file to delete
 * @opts: Deletion options (NULL for defaults)
 *
 * Securely deletes a file with custom options.
 *
 * Returns: GN_DELETE_OK on success, error code otherwise
 */
GnDeleteResult gn_secure_delete_file_opts(const char *filepath,
                                           const GnDeleteOptions *opts);

/* ============================================================
 * Directory Deletion
 * ============================================================ */

/**
 * gn_secure_delete_dir:
 * @dirpath: Path to the directory to delete
 *
 * Securely deletes a directory and all its contents recursively.
 *
 * Each file is securely deleted using default options, then the
 * directory structure is removed.
 *
 * Returns: GN_DELETE_OK on success, error code otherwise
 */
GnDeleteResult gn_secure_delete_dir(const char *dirpath);

/**
 * gn_secure_delete_dir_opts:
 * @dirpath: Path to the directory to delete
 * @opts: Deletion options (NULL for defaults, recursive is forced TRUE)
 *
 * Securely deletes a directory with custom options.
 * Note: opts->recursive is forced to TRUE for this function.
 *
 * Returns: GN_DELETE_OK on success, error code otherwise
 */
GnDeleteResult gn_secure_delete_dir_opts(const char *dirpath,
                                          const GnDeleteOptions *opts);

/* ============================================================
 * Memory Shredding
 * ============================================================
 *
 * These functions securely overwrite memory to prevent recovery.
 * They use techniques to prevent compiler optimization from removing
 * the zeroing operation:
 *
 * 1. sodium_memzero() if libsodium is available
 * 2. explicit_bzero() if available (glibc 2.25+, BSD)
 * 3. volatile pointer + memory barrier fallback
 *
 * Unlike secure-memory.h functions, these work on any memory
 * (not just memory from gn_secure_alloc).
 */

/**
 * gn_secure_shred_buffer:
 * @buf: Buffer to shred (can be NULL)
 * @len: Length of buffer in bytes
 *
 * Securely zeros a memory buffer.
 *
 * Uses a secure zeroing technique that won't be optimized away.
 * Safe to call with NULL buffer or zero length.
 */
void gn_secure_shred_buffer(void *buf, size_t len);

/**
 * gn_secure_shred_string:
 * @str: String to shred (can be NULL)
 *
 * Securely zeros a null-terminated string.
 *
 * Overwrites the string content with zeros. The string remains
 * valid (as an empty string) after this call.
 * Safe to call with NULL.
 */
void gn_secure_shred_string(char *str);

/**
 * gn_secure_shred_gstring:
 * @gstr: GString to shred (can be NULL)
 *
 * Securely zeros a GLib GString.
 *
 * Overwrites the string content and resets length to 0.
 * The GString remains valid after this call.
 */
void gn_secure_shred_gstring(GString *gstr);

/**
 * gn_secure_shred_bytes:
 * @bytes: GBytes to shred and unref (can be NULL)
 *
 * Securely zeros GBytes data before unreferencing.
 *
 * Note: This only works if the GBytes has a single reference.
 * If there are multiple references, only the internal copy is zeroed.
 * After this call, the GBytes pointer should not be used.
 */
void gn_secure_shred_bytes(GBytes *bytes);

/* ============================================================
 * Convenience Macros
 * ============================================================ */

/**
 * GN_SECURE_CLEAR:
 * @ptr: Pointer to memory
 * @size: Size in bytes
 *
 * Convenience macro that shreds and sets pointer to NULL.
 */
#define GN_SECURE_CLEAR(ptr, size) \
  G_STMT_START { \
    gn_secure_shred_buffer((ptr), (size)); \
    (ptr) = NULL; \
  } G_STMT_END

/**
 * GN_SECURE_FREE_STRING:
 * @str: String pointer (will be set to NULL)
 *
 * Shreds and frees a string, then sets pointer to NULL.
 */
#define GN_SECURE_FREE_STRING(str) \
  G_STMT_START { \
    if ((str) != NULL) { \
      gn_secure_shred_string(str); \
      g_free(str); \
      (str) = NULL; \
    } \
  } G_STMT_END

/**
 * GN_SECURE_FREE_BUFFER:
 * @buf: Buffer pointer (will be set to NULL)
 * @size: Size of buffer
 *
 * Shreds and frees a buffer, then sets pointer to NULL.
 */
#define GN_SECURE_FREE_BUFFER(buf, size) \
  G_STMT_START { \
    if ((buf) != NULL) { \
      gn_secure_shred_buffer((buf), (size)); \
      g_free(buf); \
      (buf) = NULL; \
    } \
  } G_STMT_END

/* ============================================================
 * Clipboard Security
 * ============================================================
 *
 * Functions to securely clear clipboard data after a timeout.
 * Useful for password managers and key export features.
 */

/**
 * gn_clipboard_clear_after:
 * @clipboard: GdkClipboard to clear
 * @timeout_seconds: Seconds before clearing (0 = immediate)
 *
 * Schedules clipboard to be cleared after a timeout.
 *
 * If the clipboard content changes before the timeout,
 * the clear operation is cancelled (user pasted something else).
 *
 * Returns: A source ID that can be used with g_source_remove()
 *          to cancel the clear operation, or 0 on failure.
 */
guint gn_clipboard_clear_after(gpointer clipboard, guint timeout_seconds);

/**
 * gn_clipboard_clear_now:
 * @clipboard: GdkClipboard to clear
 *
 * Immediately clears the clipboard.
 */
void gn_clipboard_clear_now(gpointer clipboard);

/* ============================================================
 * Audit Logging
 * ============================================================
 *
 * Logging functions that record security-relevant events without
 * exposing sensitive data. Useful for compliance and debugging.
 */

/**
 * GnDeleteLogLevel:
 *
 * Log levels for secure deletion operations.
 */
typedef enum {
  GN_DELETE_LOG_NONE = 0,     /* No logging */
  GN_DELETE_LOG_ERROR,        /* Only errors */
  GN_DELETE_LOG_INFO,         /* Errors + success messages */
  GN_DELETE_LOG_DEBUG         /* Full debug output */
} GnDeleteLogLevel;

/**
 * gn_secure_delete_set_log_level:
 * @level: Logging verbosity level
 *
 * Sets the logging level for secure deletion operations.
 * Default is GN_DELETE_LOG_INFO.
 */
void gn_secure_delete_set_log_level(GnDeleteLogLevel level);

/**
 * gn_secure_delete_get_log_level:
 *
 * Gets the current logging level.
 *
 * Returns: Current log level
 */
GnDeleteLogLevel gn_secure_delete_get_log_level(void);

/* ============================================================
 * SSD Detection
 * ============================================================ */

/**
 * gn_is_ssd:
 * @path: Path to check (file or directory)
 *
 * Attempts to detect if a path resides on an SSD.
 *
 * Detection methods vary by platform:
 * - Linux: Check /sys/block/<device>/queue/rotational
 * - macOS: Check disk characteristics via IOKit
 *
 * Returns: TRUE if likely SSD, FALSE if HDD or unknown
 */
gboolean gn_is_ssd(const char *path);

/**
 * gn_try_trim:
 * @filepath: Path to the file to TRIM
 *
 * Attempts to issue a TRIM command for a file's blocks.
 *
 * This tells the SSD that the blocks are no longer in use and
 * can be garbage collected. This is a hint to the SSD and may
 * improve future write performance.
 *
 * Note: TRIM does not guarantee immediate data erasure.
 *
 * Returns: GN_DELETE_OK on success, error code otherwise
 */
GnDeleteResult gn_try_trim(const char *filepath);

/* ============================================================
 * Verification and OS-Specific Support
 * ============================================================ */

/**
 * gn_secure_delete_verify:
 * @filepath: Path to verify (should not exist after secure delete)
 *
 * Verifies that a file has been successfully deleted.
 *
 * Checks:
 * 1. File no longer exists
 * 2. File is not accessible
 * 3. Parent directory is still valid
 *
 * Returns: TRUE if file is confirmed deleted, FALSE if still accessible
 */
gboolean gn_secure_delete_verify(const char *filepath);

/**
 * gn_secure_delete_with_os_tools:
 * @filepath: Path to the file to delete
 * @opts: Deletion options (NULL for defaults)
 *
 * Attempts to use OS-specific secure deletion tools:
 * - Linux: shred command
 * - macOS: rm -P (secure remove)
 *
 * Falls back to gn_secure_delete_file_opts if tools unavailable.
 *
 * Returns: GN_DELETE_OK on success, error code otherwise
 */
GnDeleteResult gn_secure_delete_with_os_tools(const char *filepath,
                                               const GnDeleteOptions *opts);

/**
 * GnOsSecureDeleteSupport:
 *
 * Indicates what OS-level secure deletion tools are available.
 */
typedef enum {
  GN_OS_DELETE_NONE = 0,          /* No OS tools available */
  GN_OS_DELETE_SHRED = 1 << 0,    /* Linux shred command */
  GN_OS_DELETE_SRM = 1 << 1,      /* macOS srm (deprecated but may exist) */
  GN_OS_DELETE_RM_P = 1 << 2,     /* macOS rm -P */
  GN_OS_DELETE_WIPE = 1 << 3      /* wipe command */
} GnOsSecureDeleteSupport;

/**
 * gn_os_secure_delete_available:
 *
 * Checks what OS-level secure deletion tools are available.
 *
 * Returns: Bitmask of available tools
 */
GnOsSecureDeleteSupport gn_os_secure_delete_available(void);

/* ============================================================
 * Batch Operations
 * ============================================================ */

/**
 * GnSecureDeleteCallback:
 * @filepath: Current file being processed
 * @current: Current file number (1-based)
 * @total: Total number of files
 * @result: Result of current deletion
 * @user_data: User data passed to batch function
 *
 * Callback for batch secure deletion operations.
 *
 * Returns: TRUE to continue processing, FALSE to abort
 */
typedef gboolean (*GnSecureDeleteCallback)(const char *filepath,
                                            guint current,
                                            guint total,
                                            GnDeleteResult result,
                                            gpointer user_data);

/**
 * gn_secure_delete_files:
 * @files: NULL-terminated array of file paths
 * @opts: Deletion options (NULL for defaults)
 * @callback: Optional progress callback
 * @user_data: User data for callback
 *
 * Securely deletes multiple files with progress reporting.
 *
 * Returns: Number of files successfully deleted
 */
guint gn_secure_delete_files(const char **files,
                              const GnDeleteOptions *opts,
                              GnSecureDeleteCallback callback,
                              gpointer user_data);

/**
 * gn_secure_delete_pattern:
 * @dirpath: Directory to search in
 * @pattern: Glob pattern to match (e.g., "*.backup", "*.log")
 * @opts: Deletion options (NULL for defaults)
 * @callback: Optional progress callback
 * @user_data: User data for callback
 *
 * Securely deletes all files matching a pattern in a directory.
 *
 * Returns: Number of files successfully deleted
 */
guint gn_secure_delete_pattern(const char *dirpath,
                                const char *pattern,
                                const GnDeleteOptions *opts,
                                GnSecureDeleteCallback callback,
                                gpointer user_data);

/* ============================================================
 * Sensitive Data Categories
 * ============================================================
 *
 * High-level functions for securely deleting specific types
 * of sensitive data in the gnostr-signer application.
 */

/**
 * gn_secure_delete_key_files:
 * @npub: The npub identifier for the key
 *
 * Securely deletes all local key-related files:
 * - Key backup files
 * - Encrypted key exports
 * - Temporary key files
 *
 * Note: Does NOT remove from secure storage (Keychain/libsecret).
 *
 * Returns: GN_DELETE_OK on success, first error on failure
 */
GnDeleteResult gn_secure_delete_key_files(const char *npub);

/**
 * gn_secure_delete_backup_files:
 * @max_age_days: Delete backups older than this (0 = all backups)
 *
 * Securely deletes backup files:
 * - NIP-49 encrypted backups
 * - JSON backup metadata files
 * - Old/expired backups
 *
 * Returns: Number of files deleted
 */
guint gn_secure_delete_backup_files(guint max_age_days);

/**
 * gn_secure_delete_session_data:
 *
 * Securely deletes all session-related data:
 * - Session cache files
 * - Client session data
 * - Temporary authentication tokens
 *
 * Returns: GN_DELETE_OK on success, error code otherwise
 */
GnDeleteResult gn_secure_delete_session_data(void);

/**
 * gn_secure_delete_log_files:
 * @sensitive_only: TRUE to only delete logs with sensitive patterns
 *
 * Securely deletes log files:
 * - Application logs
 * - Debug logs
 * - Audit logs containing sensitive data
 *
 * If @sensitive_only is TRUE, scans logs for sensitive patterns
 * (nsec, ncryptsec, passwords) before deletion.
 *
 * Returns: Number of log files deleted
 */
guint gn_secure_delete_log_files(gboolean sensitive_only);

/* ============================================================
 * Integration Helpers
 * ============================================================
 *
 * High-level functions for common secure deletion scenarios
 * in the gnostr-signer application.
 */

/**
 * gn_secure_delete_identity_files:
 * @npub: The npub identifier for the identity
 *
 * Securely deletes all local files associated with an identity.
 *
 * This includes:
 * - Cached profile data
 * - Local key backups (if any)
 * - Identity-specific settings
 *
 * Note: This does NOT remove the key from secure storage
 * (use secret_store_remove for that).
 *
 * Returns: GN_DELETE_OK on success, first error code on failure
 */
GnDeleteResult gn_secure_delete_identity_files(const char *npub);

/**
 * gn_secure_delete_all_data:
 *
 * Securely deletes all gnostr-signer data.
 *
 * This is a "nuclear option" that removes:
 * - All configuration files
 * - All cached data
 * - Policy store
 * - Request logs
 *
 * Note: Keys in secure storage (Keychain/libsecret) are NOT deleted.
 *
 * Returns: GN_DELETE_OK on success, first error code on failure
 */
GnDeleteResult gn_secure_delete_all_data(void);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_SECURE_DELETE_H */
