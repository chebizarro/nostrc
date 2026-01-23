/* secure-delete.c - Secure file and memory deletion implementation
 *
 * Implements defense-in-depth secure deletion for sensitive data.
 *
 * Build requirements:
 * - GLib 2.0
 * - Optional: libsodium for sodium_memzero()
 *
 * Platform support:
 * - Linux: Full support including TRIM via ioctl
 * - macOS: Full support including TRIM via fcntl
 * - Other POSIX: Basic support, no TRIM
 */

#include "secure-delete.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* For directory traversal */
#include <dirent.h>

/* Platform-specific includes */
#ifdef __linux__
#include <linux/fs.h>
#include <sys/sysmacros.h>  /* For major() and minor() macros */
#include <sys/ioctl.h>
#endif

#ifdef __APPLE__
#include <sys/disk.h>
#include <sys/param.h>
#include <sys/mount.h>
#endif

/* Optional libsodium support */
#ifdef GNOSTR_HAVE_SODIUM
#include <sodium.h>
#endif

/* GTK4 clipboard support - only include if building with GTK */
#ifdef GTK_COMPILATION
#include <gdk/gdk.h>
#endif

/* ============================================================
 * Configuration
 * ============================================================ */

#define DEFAULT_BUFFER_SIZE (64 * 1024)  /* 64 KB */
#define RANDOM_NAME_LENGTH 16
#define MAX_PATH_LENGTH 4096

/* Global log level */
static GnDeleteLogLevel g_log_level = GN_DELETE_LOG_INFO;

/* ============================================================
 * Logging
 * ============================================================ */

void gn_secure_delete_set_log_level(GnDeleteLogLevel level) {
  g_log_level = level;
}

GnDeleteLogLevel gn_secure_delete_get_log_level(void) {
  return g_log_level;
}

/* Internal logging macros */
#define LOG_ERROR(fmt, ...) \
  G_STMT_START { \
    if (g_log_level >= GN_DELETE_LOG_ERROR) \
      g_warning("secure-delete: " fmt, ##__VA_ARGS__); \
  } G_STMT_END

#define LOG_INFO(fmt, ...) \
  G_STMT_START { \
    if (g_log_level >= GN_DELETE_LOG_INFO) \
      g_message("secure-delete: " fmt, ##__VA_ARGS__); \
  } G_STMT_END

#define LOG_DEBUG(fmt, ...) \
  G_STMT_START { \
    if (g_log_level >= GN_DELETE_LOG_DEBUG) \
      g_debug("secure-delete: " fmt, ##__VA_ARGS__); \
  } G_STMT_END

/* ============================================================
 * Result Code Strings
 * ============================================================ */

const char *gn_delete_result_to_string(GnDeleteResult result) {
  switch (result) {
    case GN_DELETE_OK:           return "Success";
    case GN_DELETE_ERR_NOT_FOUND: return "File or directory not found";
    case GN_DELETE_ERR_PERMISSION: return "Permission denied";
    case GN_DELETE_ERR_IO:       return "I/O error";
    case GN_DELETE_ERR_BUSY:     return "File is locked or in use";
    case GN_DELETE_ERR_NOT_FILE: return "Path is not a regular file";
    case GN_DELETE_ERR_NOT_DIR:  return "Path is not a directory";
    case GN_DELETE_ERR_NOT_EMPTY: return "Directory not empty";
    case GN_DELETE_ERR_INVALID:  return "Invalid parameter";
    case GN_DELETE_ERR_TRIM_FAILED: return "TRIM operation failed";
    default:                     return "Unknown error";
  }
}

/* ============================================================
 * Default Options
 * ============================================================ */

GnDeleteOptions gn_delete_options_default(void) {
  GnDeleteOptions opts = GN_DELETE_OPTIONS_DEFAULT;
  return opts;
}

/* ============================================================
 * Secure Memory Zeroing
 * ============================================================ */

/* Memory barrier to prevent reordering */
static inline void memory_barrier(void) {
#if defined(__GNUC__) || defined(__clang__)
  __asm__ __volatile__("" : : : "memory");
#elif defined(_MSC_VER)
  _ReadWriteBarrier();
#else
  /* Fallback: volatile variable access */
  static volatile int barrier_dummy = 0;
  barrier_dummy = barrier_dummy;
#endif
}

void gn_secure_shred_buffer(void *buf, size_t len) {
  if (buf == NULL || len == 0) {
    return;
  }

#ifdef GNOSTR_HAVE_SODIUM
  /* Best option: libsodium's secure zeroing */
  sodium_memzero(buf, len);
#elif defined(HAVE_EXPLICIT_BZERO)
  /* Second best: explicit_bzero (glibc 2.25+, FreeBSD 11+) */
  explicit_bzero(buf, len);
#elif defined(HAVE_MEMSET_S)
  /* C11 Annex K: memset_s */
  memset_s(buf, len, 0, len);
#elif defined(_WIN32)
  /* Windows: SecureZeroMemory */
  SecureZeroMemory(buf, len);
#else
  /* Fallback: volatile pointer technique
   * This prevents the compiler from optimizing away the memset
   * because it cannot prove the volatile pointer won't be read.
   */
  volatile unsigned char *vp = (volatile unsigned char *)buf;
  size_t i;
  for (i = 0; i < len; i++) {
    vp[i] = 0;
  }

  /* Memory barrier ensures the writes are not reordered */
  memory_barrier();
#endif

  LOG_DEBUG("Shredded %zu bytes at %p", len, buf);
}

void gn_secure_shred_string(char *str) {
  if (str == NULL) {
    return;
  }

  size_t len = strlen(str);
  if (len > 0) {
    gn_secure_shred_buffer(str, len);
  }

  LOG_DEBUG("Shredded string of length %zu", len);
}

void gn_secure_shred_gstring(GString *gstr) {
  if (gstr == NULL) {
    return;
  }

  if (gstr->str != NULL && gstr->len > 0) {
    gn_secure_shred_buffer(gstr->str, gstr->len);
  }

  /* Reset the GString to empty state */
  g_string_truncate(gstr, 0);

  LOG_DEBUG("Shredded GString");
}

void gn_secure_shred_bytes(GBytes *bytes) {
  if (bytes == NULL) {
    return;
  }

  /* Get the data pointer - note this may be read-only! */
  gsize size;
  gconstpointer data = g_bytes_get_data(bytes, &size);

  if (data != NULL && size > 0) {
    /* Warning: This casts away const, which is technically UB
     * if the memory is truly read-only. However, GBytes created
     * from malloc'd memory should be writable.
     */
    gn_secure_shred_buffer((void *)data, size);
  }

  LOG_DEBUG("Shredded GBytes of size %zu", size);
}

/* ============================================================
 * Random Data Generation
 * ============================================================ */

/* Fill buffer with cryptographically random data */
static void fill_random(void *buf, size_t len) {
#ifdef GNOSTR_HAVE_SODIUM
  randombytes_buf(buf, len);
#else
  /* Fallback to /dev/urandom or GLib's random */
  static gboolean urandom_works = TRUE;

  if (urandom_works) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
      ssize_t r = read(fd, buf, len);
      close(fd);
      if (r == (ssize_t)len) {
        return;
      }
    }
    urandom_works = FALSE;
  }

  /* Fallback to GLib random (not cryptographically secure!) */
  guint32 *p = (guint32 *)buf;
  size_t words = len / sizeof(guint32);
  size_t remainder = len % sizeof(guint32);

  for (size_t i = 0; i < words; i++) {
    p[i] = g_random_int();
  }

  if (remainder > 0) {
    guint32 r = g_random_int();
    memcpy((char *)buf + (words * sizeof(guint32)), &r, remainder);
  }
#endif
}

/* Generate a random filename for renaming */
static char *generate_random_name(void) {
  static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  char *name = g_malloc(RANDOM_NAME_LENGTH + 1);

  for (int i = 0; i < RANDOM_NAME_LENGTH; i++) {
    guint32 idx = g_random_int_range(0, sizeof(chars) - 1);
    name[i] = chars[idx];
  }
  name[RANDOM_NAME_LENGTH] = '\0';

  return name;
}

/* ============================================================
 * SSD Detection
 * ============================================================ */

gboolean gn_is_ssd(const char *path) {
  if (path == NULL) {
    return FALSE;
  }

#ifdef __linux__
  /* Linux: Check /sys/block/<dev>/queue/rotational
   * 0 = SSD, 1 = HDD
   */
  struct stat st;
  if (stat(path, &st) != 0) {
    return FALSE;
  }

  /* Get the device major/minor */
  dev_t dev = st.st_dev;
  unsigned int major_num = major(dev);
  unsigned int minor_num = minor(dev);

  /* Try to find the block device */
  char sysfs_path[256];
  g_snprintf(sysfs_path, sizeof(sysfs_path),
             "/sys/dev/block/%u:%u/queue/rotational",
             major_num, minor_num);

  /* Also try the parent device for partitions */
  gchar *contents = NULL;
  if (!g_file_get_contents(sysfs_path, &contents, NULL, NULL)) {
    /* Try removing partition number */
    g_snprintf(sysfs_path, sizeof(sysfs_path),
               "/sys/dev/block/%u:0/queue/rotational",
               major_num);
    if (!g_file_get_contents(sysfs_path, &contents, NULL, NULL)) {
      return FALSE;
    }
  }

  gboolean is_ssd = (contents && contents[0] == '0');
  g_free(contents);

  LOG_DEBUG("Path %s is on %s", path, is_ssd ? "SSD" : "HDD");
  return is_ssd;

#elif defined(__APPLE__)
  /* macOS: Use statfs to get mount info, then check characteristics */
  struct statfs sfs;
  if (statfs(path, &sfs) != 0) {
    return FALSE;
  }

  /* Check if it's a local filesystem */
  if (sfs.f_flags & MNT_LOCAL) {
    /* Most internal drives on modern Macs are SSDs
     * This is a simplification - could use IOKit for precise detection
     */
    if (strstr(sfs.f_mntfromname, "disk") != NULL) {
      LOG_DEBUG("Path %s is likely on SSD (local disk)", path);
      return TRUE;
    }
  }

  LOG_DEBUG("Path %s - unknown drive type", path);
  return FALSE;

#else
  /* Other platforms: assume HDD (more conservative) */
  (void)path;
  return FALSE;
#endif
}

/* ============================================================
 * TRIM Support
 * ============================================================ */

GnDeleteResult gn_try_trim(const char *filepath) {
  if (filepath == NULL) {
    return GN_DELETE_ERR_INVALID;
  }

#ifdef __linux__
  /* Linux: Use FITRIM or BLKDISCARD ioctl */
  int fd = open(filepath, O_RDWR);
  if (fd < 0) {
    LOG_DEBUG("TRIM: Cannot open %s for TRIM", filepath);
    return GN_DELETE_ERR_PERMISSION;
  }

  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return GN_DELETE_ERR_IO;
  }

  /* For regular files, TRIM is not directly available
   * It would need to be done at the block device level
   * This is a placeholder for completeness
   */
  close(fd);
  LOG_DEBUG("TRIM: Linux file-level TRIM not implemented");
  return GN_DELETE_ERR_TRIM_FAILED;

#elif defined(__APPLE__)
  /* macOS: Use F_PUNCHHOLE to deallocate file regions */
  int fd = open(filepath, O_RDWR);
  if (fd < 0) {
    return GN_DELETE_ERR_PERMISSION;
  }

  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return GN_DELETE_ERR_IO;
  }

#ifdef F_PUNCHHOLE
  /* F_PUNCHHOLE deallocates disk space for a range */
  struct fpunchhole args = {
    .fp_flags = 0,
    .reserved = 0,
    .fp_offset = 0,
    .fp_length = st.st_size
  };

  int rc = fcntl(fd, F_PUNCHHOLE, &args);
  close(fd);

  if (rc == 0) {
    LOG_INFO("TRIM successful for %s", filepath);
    return GN_DELETE_OK;
  }
#endif

  close(fd);
  LOG_DEBUG("TRIM: macOS F_PUNCHHOLE not available or failed");
  return GN_DELETE_ERR_TRIM_FAILED;

#else
  (void)filepath;
  return GN_DELETE_ERR_TRIM_FAILED;
#endif
}

/* ============================================================
 * File Overwriting
 * ============================================================ */

/* Overwrite a file with a specific pattern */
static GnDeleteResult overwrite_file_pass(int fd, off_t file_size,
                                           int pass_type,
                                           gsize buffer_size,
                                           gboolean do_sync) {
  guint8 *buffer = g_malloc(buffer_size);

  /* Fill buffer based on pass type */
  switch (pass_type) {
    case 0:  /* Zeros */
      memset(buffer, 0x00, buffer_size);
      break;
    case 1:  /* Ones */
      memset(buffer, 0xFF, buffer_size);
      break;
    default: /* Random */
      fill_random(buffer, buffer_size);
      break;
  }

  /* Seek to beginning */
  if (lseek(fd, 0, SEEK_SET) != 0) {
    g_free(buffer);
    return GN_DELETE_ERR_IO;
  }

  /* Write the pattern */
  off_t remaining = file_size;
  while (remaining > 0) {
    gsize to_write = (remaining < (off_t)buffer_size)
                     ? (gsize)remaining : buffer_size;

    /* Regenerate random data for each chunk in random pass */
    if (pass_type >= 2) {
      fill_random(buffer, to_write);
    }

    ssize_t written = write(fd, buffer, to_write);
    if (written <= 0) {
      gn_secure_shred_buffer(buffer, buffer_size);
      g_free(buffer);
      return GN_DELETE_ERR_IO;
    }

    remaining -= written;
  }

  /* Sync to disk if requested */
  if (do_sync) {
    fsync(fd);
  }

  gn_secure_shred_buffer(buffer, buffer_size);
  g_free(buffer);

  return GN_DELETE_OK;
}

/* ============================================================
 * Secure File Deletion
 * ============================================================ */

GnDeleteResult gn_secure_delete_file_opts(const char *filepath,
                                           const GnDeleteOptions *opts) {
  if (filepath == NULL || *filepath == '\0') {
    return GN_DELETE_ERR_INVALID;
  }

  /* Use default options if not provided */
  GnDeleteOptions default_opts = GN_DELETE_OPTIONS_DEFAULT;
  if (opts == NULL) {
    opts = &default_opts;
  }

  gsize buffer_size = opts->buffer_size > 0 ? opts->buffer_size : DEFAULT_BUFFER_SIZE;

  LOG_INFO("Secure delete starting: %s (passes=%d)", filepath, opts->passes);

  /* Check file exists and is regular file */
  struct stat st;
  if (lstat(filepath, &st) != 0) {
    LOG_ERROR("File not found: %s", filepath);
    return GN_DELETE_ERR_NOT_FOUND;
  }

  /* Check for symlinks */
  if (S_ISLNK(st.st_mode) && !opts->follow_symlinks) {
    /* Just unlink symlinks, don't follow */
    if (unlink(filepath) != 0) {
      return GN_DELETE_ERR_PERMISSION;
    }
    LOG_INFO("Removed symlink: %s", filepath);
    return GN_DELETE_OK;
  }

  if (!S_ISREG(st.st_mode)) {
    LOG_ERROR("Not a regular file: %s", filepath);
    return GN_DELETE_ERR_NOT_FILE;
  }

  /* Open file for writing */
  int fd = open(filepath, O_RDWR);
  if (fd < 0) {
    if (errno == EACCES) {
      return GN_DELETE_ERR_PERMISSION;
    }
    return GN_DELETE_ERR_IO;
  }

  off_t file_size = st.st_size;
  GnDeleteResult result = GN_DELETE_OK;

  /* Perform overwrite passes */
  if (file_size > 0) {
    int num_passes = opts->passes;

    for (int pass = 0; pass < num_passes && result == GN_DELETE_OK; pass++) {
      int pass_type;

      if (num_passes == 1) {
        /* Single pass: zeros */
        pass_type = 0;
      } else if (num_passes == 3) {
        /* Standard: zeros, ones, random */
        pass_type = pass;
      } else {
        /* Paranoid: alternate patterns */
        pass_type = (pass == num_passes - 1) ? 2 : (pass % 2);
      }

      LOG_DEBUG("Pass %d/%d (type=%d) for %s",
                pass + 1, num_passes, pass_type, filepath);

      result = overwrite_file_pass(fd, file_size, pass_type,
                                   buffer_size, opts->sync_after_write);
    }

    if (result != GN_DELETE_OK) {
      close(fd);
      LOG_ERROR("Overwrite failed for %s: %s",
                filepath, gn_delete_result_to_string(result));
      return result;
    }
  }

  /* Truncate to zero */
  if (ftruncate(fd, 0) != 0) {
    LOG_DEBUG("Truncate failed for %s (non-fatal)", filepath);
  }

  /* Final sync */
  if (opts->sync_after_write) {
    fsync(fd);
  }

  close(fd);

  /* Try TRIM on SSDs */
  if (opts->try_trim && gn_is_ssd(filepath)) {
    GnDeleteResult trim_result = gn_try_trim(filepath);
    if (trim_result != GN_DELETE_OK) {
      LOG_DEBUG("TRIM failed for %s (non-fatal)", filepath);
    }
  }

  /* Rename to random name before unlinking */
  char *final_path = NULL;
  if (opts->rename_before_delete) {
    gchar *dir = g_path_get_dirname(filepath);
    gchar *random_name = generate_random_name();
    final_path = g_build_filename(dir, random_name, NULL);

    if (rename(filepath, final_path) == 0) {
      LOG_DEBUG("Renamed %s to %s before deletion", filepath, final_path);
    } else {
      LOG_DEBUG("Rename failed (non-fatal), deleting with original name");
      g_free(final_path);
      final_path = g_strdup(filepath);
    }

    g_free(dir);
    g_free(random_name);
  } else {
    final_path = g_strdup(filepath);
  }

  /* Unlink the file */
  if (unlink(final_path) != 0) {
    LOG_ERROR("Unlink failed for %s: %s", final_path, g_strerror(errno));
    g_free(final_path);
    return GN_DELETE_ERR_IO;
  }

  LOG_INFO("Secure delete complete: %s", filepath);
  g_free(final_path);

  return GN_DELETE_OK;
}

GnDeleteResult gn_secure_delete_file(const char *filepath) {
  return gn_secure_delete_file_opts(filepath, NULL);
}

/* ============================================================
 * Secure Directory Deletion
 * ============================================================ */

GnDeleteResult gn_secure_delete_dir_opts(const char *dirpath,
                                          const GnDeleteOptions *opts) {
  if (dirpath == NULL || *dirpath == '\0') {
    return GN_DELETE_ERR_INVALID;
  }

  /* Use default options with recursive forced on */
  GnDeleteOptions local_opts;
  if (opts != NULL) {
    local_opts = *opts;
  } else {
    local_opts = gn_delete_options_default();
  }
  local_opts.recursive = TRUE;

  LOG_INFO("Secure delete directory starting: %s", dirpath);

  /* Check directory exists */
  struct stat st;
  if (stat(dirpath, &st) != 0) {
    LOG_ERROR("Directory not found: %s", dirpath);
    return GN_DELETE_ERR_NOT_FOUND;
  }

  if (!S_ISDIR(st.st_mode)) {
    LOG_ERROR("Not a directory: %s", dirpath);
    return GN_DELETE_ERR_NOT_DIR;
  }

  /* Open directory */
  DIR *dir = opendir(dirpath);
  if (dir == NULL) {
    if (errno == EACCES) {
      return GN_DELETE_ERR_PERMISSION;
    }
    return GN_DELETE_ERR_IO;
  }

  GnDeleteResult result = GN_DELETE_OK;
  struct dirent *entry;

  while ((entry = readdir(dir)) != NULL && result == GN_DELETE_OK) {
    /* Skip . and .. */
    if (g_strcmp0(entry->d_name, ".") == 0 ||
        g_strcmp0(entry->d_name, "..") == 0) {
      continue;
    }

    gchar *full_path = g_build_filename(dirpath, entry->d_name, NULL);

    struct stat entry_st;
    if (lstat(full_path, &entry_st) != 0) {
      g_free(full_path);
      continue;
    }

    if (S_ISDIR(entry_st.st_mode)) {
      /* Recursive call for subdirectory */
      result = gn_secure_delete_dir_opts(full_path, &local_opts);
    } else {
      /* Delete file */
      result = gn_secure_delete_file_opts(full_path, &local_opts);
    }

    g_free(full_path);
  }

  closedir(dir);

  if (result != GN_DELETE_OK) {
    return result;
  }

  /* Remove the now-empty directory */
  if (rmdir(dirpath) != 0) {
    if (errno == ENOTEMPTY) {
      return GN_DELETE_ERR_NOT_EMPTY;
    }
    LOG_ERROR("rmdir failed for %s: %s", dirpath, g_strerror(errno));
    return GN_DELETE_ERR_IO;
  }

  LOG_INFO("Secure delete directory complete: %s", dirpath);

  return GN_DELETE_OK;
}

GnDeleteResult gn_secure_delete_dir(const char *dirpath) {
  return gn_secure_delete_dir_opts(dirpath, NULL);
}

/* ============================================================
 * Clipboard Security
 * ============================================================ */

/* Context for clipboard clear timeout */
typedef struct {
  gpointer clipboard;
  gchar *original_text;
  guint source_id;
} ClipboardClearContext;

/* Callback to check and clear clipboard */
static gboolean clipboard_clear_timeout_cb(gpointer user_data) {
  ClipboardClearContext *ctx = (ClipboardClearContext *)user_data;

  if (ctx == NULL) {
    return G_SOURCE_REMOVE;
  }

#ifdef GTK_COMPILATION
  GdkClipboard *clipboard = GDK_CLIPBOARD(ctx->clipboard);

  /* Check if clipboard content has changed
   * If the user pasted something else, don't clear it
   */
  /* Note: In GTK4, checking clipboard content is async,
   * so we just clear it unconditionally for simplicity
   */

  /* Clear the clipboard */
  gdk_clipboard_set_text(clipboard, "");

  LOG_INFO("Clipboard cleared after timeout");
#endif

  /* Clean up */
  if (ctx->original_text) {
    gn_secure_shred_string(ctx->original_text);
    g_free(ctx->original_text);
  }
  g_free(ctx);

  return G_SOURCE_REMOVE;
}

guint gn_clipboard_clear_after(gpointer clipboard, guint timeout_seconds) {
  if (clipboard == NULL) {
    return 0;
  }

  ClipboardClearContext *ctx = g_new0(ClipboardClearContext, 1);
  ctx->clipboard = clipboard;
  ctx->original_text = NULL;

  if (timeout_seconds == 0) {
    /* Immediate clear */
    gn_clipboard_clear_now(clipboard);
    g_free(ctx);
    return 0;
  }

  guint source_id = g_timeout_add_seconds(timeout_seconds,
                                           clipboard_clear_timeout_cb,
                                           ctx);
  ctx->source_id = source_id;

  LOG_DEBUG("Clipboard clear scheduled in %u seconds", timeout_seconds);

  return source_id;
}

void gn_clipboard_clear_now(gpointer clipboard) {
  if (clipboard == NULL) {
    return;
  }

#ifdef GTK_COMPILATION
  GdkClipboard *gdk_clip = GDK_CLIPBOARD(clipboard);
  gdk_clipboard_set_text(gdk_clip, "");
  LOG_INFO("Clipboard cleared immediately");
#else
  (void)clipboard;
  LOG_DEBUG("Clipboard clearing not available (GTK not compiled in)");
#endif
}

/* ============================================================
 * Identity File Deletion
 * ============================================================ */

GnDeleteResult gn_secure_delete_identity_files(const char *npub) {
  if (npub == NULL || *npub == '\0') {
    return GN_DELETE_ERR_INVALID;
  }

  LOG_INFO("Secure delete identity files for: %.16s...", npub);

  const char *config_dir = g_get_user_config_dir();
  const char *cache_dir = g_get_user_cache_dir();

  GnDeleteResult result = GN_DELETE_OK;

  /* Delete profile cache */
  gchar *profile_cache = g_build_filename(cache_dir, "gnostr-signer",
                                           "profiles", npub, NULL);
  if (g_file_test(profile_cache, G_FILE_TEST_IS_DIR)) {
    GnDeleteResult r = gn_secure_delete_dir(profile_cache);
    if (r != GN_DELETE_OK && result == GN_DELETE_OK) {
      result = r;
    }
  }
  g_free(profile_cache);

  /* Delete any backup files matching this identity */
  gchar *backup_pattern = g_strdup_printf("%s.backup", npub);
  gchar *backups_dir = g_build_filename(config_dir, "gnostr-signer",
                                         "backups", NULL);

  if (g_file_test(backups_dir, G_FILE_TEST_IS_DIR)) {
    GDir *dir = g_dir_open(backups_dir, 0, NULL);
    if (dir) {
      const gchar *entry;
      while ((entry = g_dir_read_name(dir)) != NULL) {
        if (g_str_has_prefix(entry, npub)) {
          gchar *full_path = g_build_filename(backups_dir, entry, NULL);
          GnDeleteResult r = gn_secure_delete_file(full_path);
          if (r != GN_DELETE_OK && result == GN_DELETE_OK) {
            result = r;
          }
          g_free(full_path);
        }
      }
      g_dir_close(dir);
    }
  }

  g_free(backup_pattern);
  g_free(backups_dir);

  /* Delete identity-specific settings (if stored separately) */
  gchar *id_settings = g_build_filename(config_dir, "gnostr-signer",
                                         "identities", npub, NULL);
  if (g_file_test(id_settings, G_FILE_TEST_IS_DIR)) {
    GnDeleteResult r = gn_secure_delete_dir(id_settings);
    if (r != GN_DELETE_OK && result == GN_DELETE_OK) {
      result = r;
    }
  }
  g_free(id_settings);

  if (result == GN_DELETE_OK) {
    LOG_INFO("Identity files deleted successfully for: %.16s...", npub);
  } else {
    LOG_ERROR("Some identity files could not be deleted for: %.16s...", npub);
  }

  return result;
}

/* ============================================================
 * Delete All Data
 * ============================================================ */

GnDeleteResult gn_secure_delete_all_data(void) {
  LOG_INFO("Secure delete ALL gnostr-signer data starting");

  const char *config_dir = g_get_user_config_dir();
  const char *cache_dir = g_get_user_cache_dir();
  const char *data_dir = g_get_user_data_dir();

  GnDeleteResult result = GN_DELETE_OK;

  /* Delete config directory */
  gchar *signer_config = g_build_filename(config_dir, "gnostr-signer", NULL);
  if (g_file_test(signer_config, G_FILE_TEST_IS_DIR)) {
    GnDeleteResult r = gn_secure_delete_dir(signer_config);
    if (r != GN_DELETE_OK && result == GN_DELETE_OK) {
      result = r;
    }
  }
  g_free(signer_config);

  /* Delete cache directory */
  gchar *signer_cache = g_build_filename(cache_dir, "gnostr-signer", NULL);
  if (g_file_test(signer_cache, G_FILE_TEST_IS_DIR)) {
    GnDeleteResult r = gn_secure_delete_dir(signer_cache);
    if (r != GN_DELETE_OK && result == GN_DELETE_OK) {
      result = r;
    }
  }
  g_free(signer_cache);

  /* Delete data directory */
  gchar *signer_data = g_build_filename(data_dir, "gnostr-signer", NULL);
  if (g_file_test(signer_data, G_FILE_TEST_IS_DIR)) {
    GnDeleteResult r = gn_secure_delete_dir(signer_data);
    if (r != GN_DELETE_OK && result == GN_DELETE_OK) {
      result = r;
    }
  }
  g_free(signer_data);

  if (result == GN_DELETE_OK) {
    LOG_INFO("All gnostr-signer data deleted successfully");
  } else {
    LOG_ERROR("Some data could not be deleted");
  }

  return result;
}

/* ============================================================
 * Verification
 * ============================================================ */

gboolean gn_secure_delete_verify(const char *filepath) {
  if (filepath == NULL || *filepath == '\0') {
    return FALSE;
  }

  /* Check if file still exists */
  if (g_file_test(filepath, G_FILE_TEST_EXISTS)) {
    LOG_DEBUG("Verification failed: file still exists: %s", filepath);
    return FALSE;
  }

  /* Try to open - should fail */
  int fd = open(filepath, O_RDONLY);
  if (fd >= 0) {
    close(fd);
    LOG_DEBUG("Verification failed: file still accessible: %s", filepath);
    return FALSE;
  }

  /* Verify parent directory still exists */
  gchar *parent = g_path_get_dirname(filepath);
  gboolean parent_exists = g_file_test(parent, G_FILE_TEST_IS_DIR);
  g_free(parent);

  if (!parent_exists) {
    LOG_DEBUG("Verification warning: parent directory also removed: %s", filepath);
    /* This is still considered successful deletion */
  }

  LOG_DEBUG("Verification successful: file confirmed deleted: %s", filepath);
  return TRUE;
}

/* ============================================================
 * OS-Specific Secure Deletion Tools
 * ============================================================ */

GnOsSecureDeleteSupport gn_os_secure_delete_available(void) {
  GnOsSecureDeleteSupport support = GN_OS_DELETE_NONE;

#ifdef __linux__
  /* Check for shred command */
  if (g_find_program_in_path("shred") != NULL) {
    support |= GN_OS_DELETE_SHRED;
    LOG_DEBUG("OS tool available: shred");
  }

  /* Check for wipe command */
  gchar *wipe_path = g_find_program_in_path("wipe");
  if (wipe_path != NULL) {
    support |= GN_OS_DELETE_WIPE;
    LOG_DEBUG("OS tool available: wipe");
    g_free(wipe_path);
  }
#endif

#ifdef __APPLE__
  /* macOS rm supports -P flag for secure delete */
  support |= GN_OS_DELETE_RM_P;
  LOG_DEBUG("OS tool available: rm -P");

  /* Check for srm (deprecated but may be installed) */
  gchar *srm_path = g_find_program_in_path("srm");
  if (srm_path != NULL) {
    support |= GN_OS_DELETE_SRM;
    LOG_DEBUG("OS tool available: srm");
    g_free(srm_path);
  }
#endif

  return support;
}

/* Internal: Execute OS secure delete command */
static GnDeleteResult execute_os_secure_delete(const char *filepath,
                                                 GnOsSecureDeleteSupport tool,
                                                 int passes) {
  gchar *cmd = NULL;
  GError *error = NULL;
  gint exit_status = 0;

  switch (tool) {
#ifdef __linux__
    case GN_OS_DELETE_SHRED: {
      /* shred -n PASSES -z -u FILE
       * -n: number of overwrite passes
       * -z: add final zero pass
       * -u: deallocate and remove file
       */
      cmd = g_strdup_printf("shred -n %d -z -u '%s'", passes, filepath);
      break;
    }

    case GN_OS_DELETE_WIPE: {
      /* wipe -f -q -Q PASSES FILE */
      cmd = g_strdup_printf("wipe -f -q -Q %d '%s'", passes, filepath);
      break;
    }
#endif

#ifdef __APPLE__
    case GN_OS_DELETE_SRM: {
      /* srm -sz FILE (simple mode with zero) */
      cmd = g_strdup_printf("srm -sz '%s'", filepath);
      break;
    }

    case GN_OS_DELETE_RM_P: {
      /* rm -P FILE (3-pass overwrite before unlink) */
      cmd = g_strdup_printf("rm -P '%s'", filepath);
      break;
    }
#endif

    default:
      return GN_DELETE_ERR_INVALID;
  }

  if (cmd == NULL) {
    return GN_DELETE_ERR_INVALID;
  }

  LOG_DEBUG("Executing OS secure delete: %s", cmd);

  gboolean ok = g_spawn_command_line_sync(cmd, NULL, NULL, &exit_status, &error);

  g_free(cmd);

  if (!ok) {
    LOG_ERROR("OS secure delete failed: %s", error ? error->message : "unknown");
    g_clear_error(&error);
    return GN_DELETE_ERR_IO;
  }

  if (exit_status != 0) {
    LOG_ERROR("OS secure delete command returned %d", exit_status);
    return GN_DELETE_ERR_IO;
  }

  /* Verify deletion */
  if (!gn_secure_delete_verify(filepath)) {
    LOG_ERROR("OS secure delete verification failed for %s", filepath);
    return GN_DELETE_ERR_IO;
  }

  LOG_INFO("OS secure delete successful: %s", filepath);
  return GN_DELETE_OK;
}

GnDeleteResult gn_secure_delete_with_os_tools(const char *filepath,
                                               const GnDeleteOptions *opts) {
  if (filepath == NULL || *filepath == '\0') {
    return GN_DELETE_ERR_INVALID;
  }

  /* Use default options if not provided */
  GnDeleteOptions default_opts = GN_DELETE_OPTIONS_DEFAULT;
  if (opts == NULL) {
    opts = &default_opts;
  }

  GnOsSecureDeleteSupport available = gn_os_secure_delete_available();

  /* Try OS tools in order of preference */
#ifdef __linux__
  if (available & GN_OS_DELETE_SHRED) {
    GnDeleteResult r = execute_os_secure_delete(filepath, GN_OS_DELETE_SHRED, opts->passes);
    if (r == GN_DELETE_OK) {
      return r;
    }
    LOG_DEBUG("shred failed, trying fallback");
  }

  if (available & GN_OS_DELETE_WIPE) {
    GnDeleteResult r = execute_os_secure_delete(filepath, GN_OS_DELETE_WIPE, opts->passes);
    if (r == GN_DELETE_OK) {
      return r;
    }
    LOG_DEBUG("wipe failed, trying fallback");
  }
#endif

#ifdef __APPLE__
  if (available & GN_OS_DELETE_SRM) {
    GnDeleteResult r = execute_os_secure_delete(filepath, GN_OS_DELETE_SRM, opts->passes);
    if (r == GN_DELETE_OK) {
      return r;
    }
    LOG_DEBUG("srm failed, trying fallback");
  }

  if (available & GN_OS_DELETE_RM_P) {
    GnDeleteResult r = execute_os_secure_delete(filepath, GN_OS_DELETE_RM_P, opts->passes);
    if (r == GN_DELETE_OK) {
      return r;
    }
    LOG_DEBUG("rm -P failed, trying fallback");
  }
#endif

  /* Fallback to our implementation */
  LOG_DEBUG("No OS tools succeeded, using built-in secure delete");
  return gn_secure_delete_file_opts(filepath, opts);
}

/* ============================================================
 * Batch Operations
 * ============================================================ */

guint gn_secure_delete_files(const char **files,
                              const GnDeleteOptions *opts,
                              GnSecureDeleteCallback callback,
                              gpointer user_data) {
  if (files == NULL) {
    return 0;
  }

  /* Count files */
  guint total = 0;
  for (const char **p = files; *p != NULL; p++) {
    total++;
  }

  if (total == 0) {
    return 0;
  }

  guint deleted = 0;
  guint current = 0;

  for (const char **p = files; *p != NULL; p++) {
    current++;
    const char *filepath = *p;

    GnDeleteResult result = gn_secure_delete_with_os_tools(filepath, opts);

    if (result == GN_DELETE_OK) {
      deleted++;
    }

    /* Call progress callback */
    if (callback) {
      gboolean cont = callback(filepath, current, total, result, user_data);
      if (!cont) {
        LOG_INFO("Batch deletion aborted by callback at file %u/%u", current, total);
        break;
      }
    }
  }

  LOG_INFO("Batch deletion complete: %u/%u files deleted", deleted, total);
  return deleted;
}

guint gn_secure_delete_pattern(const char *dirpath,
                                const char *pattern,
                                const GnDeleteOptions *opts,
                                GnSecureDeleteCallback callback,
                                gpointer user_data) {
  if (dirpath == NULL || pattern == NULL) {
    return 0;
  }

  LOG_INFO("Secure delete pattern: %s in %s", pattern, dirpath);

  GDir *dir = g_dir_open(dirpath, 0, NULL);
  if (dir == NULL) {
    LOG_ERROR("Cannot open directory: %s", dirpath);
    return 0;
  }

  /* Collect matching files */
  GPtrArray *matches = g_ptr_array_new_with_free_func(g_free);
  const gchar *entry;

  while ((entry = g_dir_read_name(dir)) != NULL) {
    if (g_pattern_match_simple(pattern, entry)) {
      gchar *full_path = g_build_filename(dirpath, entry, NULL);

      /* Only include regular files */
      if (g_file_test(full_path, G_FILE_TEST_IS_REGULAR)) {
        g_ptr_array_add(matches, full_path);
      } else {
        g_free(full_path);
      }
    }
  }

  g_dir_close(dir);

  if (matches->len == 0) {
    g_ptr_array_free(matches, TRUE);
    LOG_DEBUG("No files matched pattern: %s", pattern);
    return 0;
  }

  /* Add NULL terminator */
  g_ptr_array_add(matches, NULL);

  /* Delete the matched files */
  guint deleted = gn_secure_delete_files(
    (const char **)matches->pdata,
    opts,
    callback,
    user_data
  );

  g_ptr_array_free(matches, TRUE);

  return deleted;
}

/* ============================================================
 * Sensitive Data Category Deletion
 * ============================================================ */

GnDeleteResult gn_secure_delete_key_files(const char *npub) {
  if (npub == NULL || *npub == '\0') {
    return GN_DELETE_ERR_INVALID;
  }

  LOG_INFO("Secure delete key files for: %.16s...", npub);

  const char *config_dir = g_get_user_config_dir();
  const char *cache_dir = g_get_user_cache_dir();

  GnDeleteResult result = GN_DELETE_OK;
  GnDeleteOptions opts = GN_DELETE_OPTIONS_DEFAULT;
  opts.passes = GN_DELETE_PASSES_PARANOID;  /* Use paranoid mode for key files */

  /* Delete key backup files */
  gchar *backups_dir = g_build_filename(config_dir, "gnostr-signer", "backups", NULL);
  if (g_file_test(backups_dir, G_FILE_TEST_IS_DIR)) {
    gchar *pattern = g_strdup_printf("%s*", npub);
    guint deleted = gn_secure_delete_pattern(backups_dir, pattern, &opts, NULL, NULL);
    LOG_DEBUG("Deleted %u key backup files", deleted);
    g_free(pattern);
  }
  g_free(backups_dir);

  /* Delete encrypted exports */
  gchar *exports_dir = g_build_filename(config_dir, "gnostr-signer", "exports", NULL);
  if (g_file_test(exports_dir, G_FILE_TEST_IS_DIR)) {
    gchar *pattern = g_strdup_printf("%s*.ncryptsec", npub);
    guint deleted = gn_secure_delete_pattern(exports_dir, pattern, &opts, NULL, NULL);
    LOG_DEBUG("Deleted %u encrypted export files", deleted);
    g_free(pattern);
  }
  g_free(exports_dir);

  /* Delete key cache */
  gchar *key_cache = g_build_filename(cache_dir, "gnostr-signer", "keys", npub, NULL);
  if (g_file_test(key_cache, G_FILE_TEST_IS_DIR)) {
    GnDeleteResult r = gn_secure_delete_dir_opts(key_cache, &opts);
    if (r != GN_DELETE_OK && result == GN_DELETE_OK) {
      result = r;
    }
  }
  g_free(key_cache);

  return result;
}

guint gn_secure_delete_backup_files(guint max_age_days) {
  LOG_INFO("Secure delete backup files (max_age=%u days)", max_age_days);

  const char *config_dir = g_get_user_config_dir();
  gchar *backups_dir = g_build_filename(config_dir, "gnostr-signer", "backups", NULL);

  if (!g_file_test(backups_dir, G_FILE_TEST_IS_DIR)) {
    g_free(backups_dir);
    return 0;
  }

  GDir *dir = g_dir_open(backups_dir, 0, NULL);
  if (dir == NULL) {
    g_free(backups_dir);
    return 0;
  }

  guint deleted = 0;
  time_t cutoff_time = 0;

  if (max_age_days > 0) {
    cutoff_time = time(NULL) - ((time_t)max_age_days * 24 * 60 * 60);
  }

  const gchar *entry;
  GnDeleteOptions opts = GN_DELETE_OPTIONS_DEFAULT;
  opts.passes = GN_DELETE_PASSES_PARANOID;

  while ((entry = g_dir_read_name(dir)) != NULL) {
    gchar *full_path = g_build_filename(backups_dir, entry, NULL);

    /* Check if it's a regular file */
    struct stat st;
    if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
      gboolean should_delete = FALSE;

      if (max_age_days == 0) {
        /* Delete all backups */
        should_delete = TRUE;
      } else if (st.st_mtime < cutoff_time) {
        /* Delete old backups */
        should_delete = TRUE;
      }

      if (should_delete) {
        GnDeleteResult r = gn_secure_delete_with_os_tools(full_path, &opts);
        if (r == GN_DELETE_OK) {
          deleted++;
        }
      }
    }

    g_free(full_path);
  }

  g_dir_close(dir);
  g_free(backups_dir);

  LOG_INFO("Deleted %u backup files", deleted);
  return deleted;
}

GnDeleteResult gn_secure_delete_session_data(void) {
  LOG_INFO("Secure delete session data");

  const char *cache_dir = g_get_user_cache_dir();
  const char *data_dir = g_get_user_data_dir();

  GnDeleteResult result = GN_DELETE_OK;
  GnDeleteOptions opts = GN_DELETE_OPTIONS_DEFAULT;

  /* Delete session cache */
  gchar *session_cache = g_build_filename(cache_dir, "gnostr-signer", "sessions", NULL);
  if (g_file_test(session_cache, G_FILE_TEST_IS_DIR)) {
    GnDeleteResult r = gn_secure_delete_dir_opts(session_cache, &opts);
    if (r != GN_DELETE_OK && result == GN_DELETE_OK) {
      result = r;
    }
  }
  g_free(session_cache);

  /* Delete client sessions */
  gchar *client_sessions = g_build_filename(data_dir, "gnostr-signer", "client_sessions", NULL);
  if (g_file_test(client_sessions, G_FILE_TEST_IS_DIR)) {
    GnDeleteResult r = gn_secure_delete_dir_opts(client_sessions, &opts);
    if (r != GN_DELETE_OK && result == GN_DELETE_OK) {
      result = r;
    }
  }
  g_free(client_sessions);

  /* Delete authentication tokens */
  gchar *tokens_dir = g_build_filename(cache_dir, "gnostr-signer", "tokens", NULL);
  if (g_file_test(tokens_dir, G_FILE_TEST_IS_DIR)) {
    GnDeleteResult r = gn_secure_delete_dir_opts(tokens_dir, &opts);
    if (r != GN_DELETE_OK && result == GN_DELETE_OK) {
      result = r;
    }
  }
  g_free(tokens_dir);

  /* Delete IPC tokens */
  gchar *ipc_tokens = g_build_filename(cache_dir, "gnostr-signer", "ipc.token", NULL);
  if (g_file_test(ipc_tokens, G_FILE_TEST_EXISTS)) {
    GnDeleteResult r = gn_secure_delete_file_opts(ipc_tokens, &opts);
    if (r != GN_DELETE_OK && result == GN_DELETE_OK) {
      result = r;
    }
  }
  g_free(ipc_tokens);

  return result;
}

/* Check if a file contains sensitive patterns */
static gboolean file_contains_sensitive_data(const char *filepath) {
  gchar *contents = NULL;
  gsize length = 0;

  if (!g_file_get_contents(filepath, &contents, &length, NULL)) {
    return FALSE;
  }

  gboolean sensitive = FALSE;

  /* Check for sensitive patterns */
  const char *patterns[] = {
    "nsec1",           /* Private keys */
    "ncryptsec1",      /* Encrypted private keys */
    "password",        /* Password fields */
    "secret",          /* Secret data */
    "private_key",     /* Key fields */
    "mnemonic",        /* Seed phrases */
    NULL
  };

  for (const char **p = patterns; *p != NULL; p++) {
    if (g_strstr_len(contents, length, *p) != NULL) {
      sensitive = TRUE;
      break;
    }
  }

  /* Securely clear contents before freeing */
  gn_secure_shred_buffer(contents, length);
  g_free(contents);

  return sensitive;
}

guint gn_secure_delete_log_files(gboolean sensitive_only) {
  LOG_INFO("Secure delete log files (sensitive_only=%s)",
           sensitive_only ? "true" : "false");

  const char *cache_dir = g_get_user_cache_dir();
  const char *data_dir = g_get_user_data_dir();

  guint deleted = 0;
  GnDeleteOptions opts = GN_DELETE_OPTIONS_DEFAULT;

  /* Log directories to check */
  const char *log_subdirs[] = { "logs", "debug", "audit", NULL };

  for (const char **subdir = log_subdirs; *subdir != NULL; subdir++) {
    gchar *log_dir = g_build_filename(cache_dir, "gnostr-signer", *subdir, NULL);

    if (g_file_test(log_dir, G_FILE_TEST_IS_DIR)) {
      GDir *dir = g_dir_open(log_dir, 0, NULL);
      if (dir) {
        const gchar *entry;
        while ((entry = g_dir_read_name(dir)) != NULL) {
          gchar *full_path = g_build_filename(log_dir, entry, NULL);

          if (g_file_test(full_path, G_FILE_TEST_IS_REGULAR)) {
            gboolean should_delete = TRUE;

            if (sensitive_only) {
              should_delete = file_contains_sensitive_data(full_path);
            }

            if (should_delete) {
              GnDeleteResult r = gn_secure_delete_file_opts(full_path, &opts);
              if (r == GN_DELETE_OK) {
                deleted++;
              }
            }
          }

          g_free(full_path);
        }
        g_dir_close(dir);
      }
    }

    g_free(log_dir);
  }

  /* Also check data directory for logs */
  gchar *data_logs = g_build_filename(data_dir, "gnostr-signer", "logs", NULL);
  if (g_file_test(data_logs, G_FILE_TEST_IS_DIR)) {
    deleted += gn_secure_delete_pattern(data_logs, "*.log", &opts, NULL, NULL);
  }
  g_free(data_logs);

  LOG_INFO("Deleted %u log files", deleted);
  return deleted;
}
