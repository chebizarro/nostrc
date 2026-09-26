/* nss-config.h — backing stores edited by org.nostr.Settings.
 * SPDX-License-Identifier: MIT
 *
 * Settings owns no configuration of its own (GSettings org.nostr.Settings
 * holds window state only); every page edits the owning daemon's file:
 *
 *   Relays / retention  ~/.config/nostr/session-relay.conf      flat key = value
 *   Notifications       ~/.config/nostr-notify/nostr-notify.conf [notify]
 *   Files / seal        ~/.config/nostr/seal.conf                [seal]
 *   Files / share       ~/.config/nostr-share/nostr-share.conf   [nostr-share]
 *
 * All writers preserve comments and keys they do not own, never add a key
 * the page did not change, and replace the file atomically (a new file is
 * created 0600 in a 0700 directory; an existing file keeps its mode). No GTK here: tests/test_config.c round-trips every store.
 */
#ifndef NSS_CONFIG_H
#define NSS_CONFIG_H

#include <glib.h>

G_BEGIN_DECLS

#define NSS_CONFIG_ERROR (nss_config_error_quark())
GQuark nss_config_error_quark(void);
typedef enum {
  NSS_CONFIG_ERROR_INVALID = 1, /* a value is out of range / malformed */
  NSS_CONFIG_ERROR_PARSE,       /* the existing file cannot be parsed */
} NssConfigError;

/* Atomic replace: mkdir -p (0700) + g_file_set_contents_full(CONSISTENT);
 * new files 0600, an existing file keeps its mode. */
gboolean nss_config_write_atomic(const gchar *path, const gchar *contents,
                                 GError **error);

/* ── Session relay retention (session-relay.conf) ───────────────────────
 * Mirrors docs/designs/nostrdb-retention-eviction-policy.md §5 / §7.1 as
 * flat `retention_*` integer keys, because relayd_config_load() rejects
 * `[section]` lines (the relay would refuse to start). The relay does not
 * enforce these yet (RetentionSupported=false, nostrc-prqu.17); the UI keeps
 * the rows insensitive until it does, so nothing is written meanwhile. */
typedef struct {
  gboolean enabled;             /* retention_enabled (0/1)           default 1   */
  gint     cache_max_mb;        /* retention_cache_max_mb, 0 = no cap default 1024 */
  gint     high_watermark_pct;  /* retention_high_watermark_pct       default 90  */
  gint     low_watermark_pct;   /* retention_low_watermark_pct        default 75  */
  gint     min_age_days;        /* retention_min_age_days             default 2   */
  gint     note_ttl_days;       /* retention_note_ttl_days, 0 = never default 90  */
  gint     reaction_ttl_days;   /* retention_reaction_ttl_days        default 14  */
  gint     interval_mins;       /* retention_interval_mins            default 60  */
} NssRetention;

#define NSS_RETENTION_MIN_CACHE_MB 64

void     nss_retention_defaults(NssRetention *r);
/* low < high, 1..99 %, cache 0 or >= 64 MB, TTLs/ages >= 0, interval >= 1. */
gboolean nss_retention_validate(const NssRetention *r, GError **error);
gchar   *nss_relay_conf_path(void);
/* Missing file → defaults. Unknown keys are ignored; a malformed
 * retention_* value is an error. */
gboolean nss_relay_conf_load(const gchar *path, NssRetention *out, GError **error);
/* Line-preserving update: existing retention_* lines are rewritten in place,
 * missing ones appended under a comment; every other line is kept verbatim. */
gboolean nss_relay_conf_save(const gchar *path, const NssRetention *r, GError **error);

/* ── nostr-notify ([notify]) ───────────────────────────────────────────── */
typedef enum {
  NSS_NOTIFY_UPSTREAM_DIRECT = 0,
  NSS_NOTIFY_UPSTREAM_SESSION_RELAY,
} NssNotifyUpstream;

typedef struct {
  NssNotifyUpstream upstream;       /* upstream_mode: direct | session_relay */
  gchar           **home_relays;    /* never NULL */
  /* Reserved presentation keys (nostrc-prqu.16): documented in
   * nostr-notify.conf.example, not read by the daemon yet. */
  gboolean notify_groups;           /* default TRUE */
  gboolean notify_dms;              /* default TRUE */
  gboolean group_preview;           /* default TRUE */
  gboolean sound;                   /* default FALSE */
} NssNotifyConf;

/* FALSE until nostr-notify-daemon honours the reserved keys. */
gboolean nss_notify_presentation_supported(void);
gchar   *nss_notify_conf_path(void);
void     nss_notify_conf_init(NssNotifyConf *c);
void     nss_notify_conf_clear(NssNotifyConf *c);
gboolean nss_notify_conf_load(const gchar *path, NssNotifyConf *out, GError **error);
/* Writes upstream_mode + home_relays; reserved keys only when already in
 * the file or different from their default. */
gboolean nss_notify_conf_save(const gchar *path, const NssNotifyConf *c, GError **error);

/* ── nostr-seal ([seal], see gnome/nostr-seal/src/nseal-config.h) ─────── */
typedef struct {
  gchar  **default_recipients;  /* never NULL; npub1…/64-hex */
  gboolean include_self;
  gint     work_factor;         /* 0 = tool default (16); else 16..20 */
} NssSealConf;

#define NSS_SEAL_WORK_MIN 16
#define NSS_SEAL_WORK_MAX 20

gchar   *nss_seal_conf_path(void);   /* honours $NOSTR_SEAL_CONFIG */
void     nss_seal_conf_init(NssSealConf *c);
void     nss_seal_conf_clear(NssSealConf *c);
gboolean nss_seal_conf_load(const gchar *path, NssSealConf *out, GError **error);
/* Every recipient must be a valid npub or 64-hex key. */
gboolean nss_seal_conf_save(const gchar *path, const NssSealConf *c, GError **error);

/* ── nostr-share ([nostr-share]) ───────────────────────────────────────── */
typedef struct {
  gint     text_kind;      /* default_text_kind: 1 | 30023 */
  gboolean keep_metadata;
} NssShareConf;

gchar   *nss_share_conf_path(void);  /* honours $NOSTR_SHARE_CONFIG */
gboolean nss_share_conf_load(const gchar *path, NssShareConf *out, GError **error);
gboolean nss_share_conf_save(const gchar *path, const NssShareConf *c, GError **error);

/* ── Shared helpers ────────────────────────────────────────────────────── */
/* 32-byte x-only pubkey from npub1… or 64-hex. */
gboolean nss_parse_pubkey(const gchar *text, guint8 out[32], GError **error);
gchar   *nss_pubkey_to_hex(const guint8 pk[32]);
gchar   *nss_pubkey_to_npub(const guint8 pk[32]);

G_END_DECLS

#endif /* NSS_CONFIG_H */
