/* nostr_profile.h — Nostr kind-0 profile → AccountsService bridge.
 *
 * This subsystem publishes a Nostr account's kind-0 metadata (display
 * name and avatar picture) into org.freedesktop.Accounts so the stock
 * GDM tile, login dialog and lock screen show the user's real face
 * instead of the generic avatar + gecos "Nostr User".
 *
 * SPLIT (security-conscious, per B5-profile design):
 *   1. FETCH kind-0 from the configured relays via libnostr,
 *      verifying signature + pubkey match. Runs in the caller's
 *      privilege domain (broker/CLI, typically root).
 *   2. Extract `name`, `display_name`, `picture` (and `nip05` if
 *      present), sanitise the strings, and cache the sanitised
 *      metadata under /var/lib/nostr-auth/profile/<user>.json.
 *   3. Download the picture UNPRIVILEGED — the parent (root)
 *      fork/execs `nostr-homed-profile-image`, which drops to
 *      an unprivileged uid (default: `nobody`), enforces
 *      https-only + SSRF-safe destination + <=2 MiB body +
 *      image content-type + 10s timeout + <=3 redirects, then
 *      re-encodes via GdkPixbuf into a bounded PNG. The parent
 *      never touches the raw remote bytes.
 *   4. INSTALL into AccountsService via the system D-Bus:
 *      FindUserByName -> SetIconFile(<png>) + SetRealName(<name>).
 *
 * Any failure at step 1, 2 or 3 leaves the existing AccountsService
 * state untouched — a broken picture URL or an offline relay must
 * never block login. */
#ifndef NOSTR_HOMED_PROFILE_H
#define NOSTR_HOMED_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Caps chosen to match GDM/AccountsService practice and RFC bounds:
 *   NAME_MAX matches the AccountsService gecos-ish real name cap in the
 *   greeter; PICTURE_URL_MAX matches typical NIP-05 URLs.
 * These MUST stay wider than the sanitizer's own caps. */
#define NH_PROFILE_NAME_MAX          64
#define NH_PROFILE_DISPLAY_NAME_MAX  64
#define NH_PROFILE_NIP05_MAX         253  /* RFC 1035 max hostname length */
#define NH_PROFILE_PICTURE_URL_MAX   1024

/* Bounded, always-NUL-terminated. Any field may be an empty string when
 * kind-0 omitted it or the sanitizer rejected the raw value. */
typedef struct nh_profile_metadata {
    char name[NH_PROFILE_NAME_MAX + 1];
    char display_name[NH_PROFILE_DISPLAY_NAME_MAX + 1];
    char nip05[NH_PROFILE_NIP05_MAX + 1];
    char picture_url[NH_PROFILE_PICTURE_URL_MAX + 1];
    int64_t created_at;               /* kind-0 event created_at (seconds) */
    char pubkey_hex[65];              /* lc-hex, echoed for logging */
} nh_profile_metadata;

typedef enum nh_profile_rc {
    NH_PROFILE_OK            = 0,
    NH_PROFILE_ERR_ARG       = 1,
    NH_PROFILE_ERR_FETCH     = 2, /* no verified kind-0 within timeout */
    NH_PROFILE_ERR_PARSE     = 3, /* kind-0 content JSON malformed */
    NH_PROFILE_ERR_URL       = 4, /* picture URL blocked by SSRF/scheme */
    NH_PROFILE_ERR_DOWNLOAD  = 5, /* HTTP failure / too big / wrong ct */
    NH_PROFILE_ERR_DECODE    = 6, /* GdkPixbuf rejected the bytes */
    NH_PROFILE_ERR_ACCOUNTS  = 7, /* AccountsService D-Bus refused write */
    NH_PROFILE_ERR_RATE      = 8, /* refresh throttled */
    NH_PROFILE_ERR_IO        = 9, /* filesystem I/O */
} nh_profile_rc;

/* ── Sanitisation ──────────────────────────────────────────── */

/* Copy @src into @dst (cap = @dst_cap chars incl. NUL). Rejects control
 * characters (< 0x20 except space; DEL; C1 controls 0x80..0x9f in the
 * *decoded* UTF-8 sense — see impl). Trims leading/trailing ASCII space.
 * Truncates on UTF-8 boundaries to at most @dst_cap-1 bytes. Returns 0 on
 * success (dst NUL-terminated, may be empty), -1 if src was NULL or dst
 * would be empty after sanitisation. */
int nh_profile_sanitize_text(const char *src, char *dst, size_t dst_cap);

/* Validate a picture URL literal. Rules:
 *   - starts with "https://" (case-sensitive) — plain http is refused
 *   - <= NH_PROFILE_PICTURE_URL_MAX
 *   - contains no whitespace or control chars, no user-info (no `@` in
 *     the authority), fragment stripped by callers if desired
 *   - host label parses as either an IPv4/IPv6 literal or a DNS name
 * Returns 0 on OK, -1 on rejection.
 * This is a syntactic pre-check; final SSRF enforcement happens in the
 * image-downloader child on the resolved peer address. */
int nh_profile_validate_picture_url(const char *url);

/* SSRF check on a resolved sockaddr. Returns 0 if the address is safe to
 * connect to (i.e. a public unicast destination), -1 if it must be
 * refused. Refuses: loopback (127/8, ::1), link-local (169.254/16,
 * fe80::/10), private RFC1918 (10/8, 172.16/12, 192.168/16), CGNAT
 * (100.64/10), IPv6 ULA (fc00::/7), IPv4-mapped IPv6 that resolves to a
 * refused v4 space, multicast, reserved and unspecified. */
struct sockaddr;
int nh_profile_ssrf_check_sockaddr(const struct sockaddr *sa);

/* ── kind-0 content parse ──────────────────────────────────── */

/* Parse a kind-0 event's `content` (a JSON object) into @out.
 * Fields are sanitised via nh_profile_sanitize_text /
 * nh_profile_validate_picture_url. Missing/rejected fields become
 * empty strings. Never fails on a well-formed JSON object; returns
 * NH_PROFILE_ERR_PARSE if @content is not a JSON object. */
nh_profile_rc nh_profile_parse_content_json(const char *content,
                                            nh_profile_metadata *out);

/* Parse a full compact-serialised kind-0 event JSON (id/pubkey/kind/
 * created_at/tags/content/sig). Populates out->pubkey_hex, out->created_at,
 * then delegates content parsing. Assumes the event has already been
 * signature-verified by the caller (nh_fetch_latest_kind0_verified
 * does this). Returns NH_PROFILE_ERR_PARSE on structural failure. */
nh_profile_rc nh_profile_parse_event_json(const char *event_json,
                                          nh_profile_metadata *out);

/* ── Cache (profile/<user>.json) ───────────────────────────── */

/* Write the sanitised metadata to <cache_dir>/<user>.json (mode 0644,
 * root-owned; parent must exist). Adds a fetched_at unix timestamp.
 * @user must be a valid nostr-homed username (already validated by the
 * store); a slash or NUL byte returns NH_PROFILE_ERR_ARG. */
nh_profile_rc nh_profile_cache_write(const char *cache_dir, const char *user,
                                     const nh_profile_metadata *meta);

/* Return the unix timestamp of the last successful refresh for @user
 * (mtime of <cache_dir>/<user>.json), or 0 when the file is missing /
 * unreadable. Used for the ≤ once-per-hour throttle. */
int64_t nh_profile_cache_last_refresh(const char *cache_dir, const char *user);

/* ── Image download child ──────────────────────────────────── */

/* Fork/exec the image-downloader helper with @url on argv, dropping
 * privileges to @drop_user (uname, e.g. "nobody"). The helper writes
 * the re-encoded PNG to @out_png_path (mode 0644). If @drop_user is
 * NULL, defaults to "nobody"; if the target user does not exist the
 * download fails with NH_PROFILE_ERR_ARG rather than running as root.
 *
 * Enforced INSIDE the helper (never trust the parent):
 *   - https:// only, <=3 redirects, 10s total timeout
 *   - <=2 MiB body, image/... content type
 *   - SSRF: refuses every non-public-unicast peer address
 *   - GdkPixbuf decode + re-encode to <=512x512 PNG
 *
 * @helper_path may be NULL to look up "nostr-homed-profile-image" on
 * PATH; tests set it explicitly to the build tree binary. */
nh_profile_rc nh_profile_download_image(const char *helper_path,
                                        const char *url,
                                        const char *drop_user,
                                        const char *out_png_path);

/* ── AccountsService installer ─────────────────────────────── */

/* Install @png_path (may be NULL/empty to skip icon update) and
 * @real_name (may be NULL/empty to skip name update) into AccountsService
 * for @user. Writes /var/lib/AccountsService/users/<user> as a keyfile
 * (Icon=, SystemAccount=false) and copies @png_path into
 * /var/lib/AccountsService/icons/<user>. Also updates the NSS
 * projection's gecos to @real_name (see nh_profile_projection_update_gecos)
 * because AccountsService's RealName property is sourced from
 * pw_gecos and does NOT read RealName= from the keyfile — a subtle
 * detail specific to accounts-daemon's implementation.
 * Requires the caller be root. Returns NH_PROFILE_OK when at least
 * one artefact reached its target. */
nh_profile_rc nh_profile_accounts_install(const char *user,
                                          const char *png_path,
                                          const char *real_name);

/* Update the passwd.gecos column for @user in the NSS projection
 * SQLite file at @projection_path (default:
 * /var/lib/nostr-auth/nss.db). Returns NH_PROFILE_OK when the row was
 * updated OR was already up-to-date, NH_PROFILE_ERR_IO on any SQL
 * failure, NH_PROFILE_ERR_ARG on validation failure. This is a
 * best-effort augmentation of the projection: the next full
 * publish_projection (during enrolment) rewrites the whole table with
 * the hardcoded default gecos, so the profile-refresh hook re-applies
 * it after that event. */
nh_profile_rc nh_profile_projection_update_gecos(const char *projection_path,
                                                 const char *user,
                                                 const char *gecos);

/* ── Orchestration ─────────────────────────────────────────── */

typedef struct nh_profile_refresh_opts {
    /* Relay list (each "ws(s)://..."), <= 8 relays. If num_relays == 0
     * the caller has no site config; refresh returns NH_PROFILE_ERR_ARG. */
    const char *const *relays;
    size_t num_relays;
    /* Directory for the JSON cache. NULL => /var/lib/nostr-auth/profile. */
    const char *cache_dir;
    /* Uname the image helper drops to. NULL => "nobody". */
    const char *image_drop_user;
    /* Absolute path to the image helper. NULL => search PATH. */
    const char *image_helper_path;
    /* Throttle: if < throttle_seconds have elapsed since the last
     * successful refresh, return NH_PROFILE_ERR_RATE without any
     * relay / download / D-Bus traffic. 0 disables throttling. */
    int64_t throttle_seconds;
    /* When true, skip the AccountsService step (useful for pre-seed
     * dry-runs and for tests that only want to populate the cache). */
    bool skip_accounts_install;
    /* Optional out — receives the resolved metadata on success. May be
     * NULL if the caller only needs the return code. */
    nh_profile_metadata *out_metadata;
    /* Optional out — receives the absolute path of the installed PNG
     * (buffer of >= PATH_MAX bytes owned by caller). May be NULL. */
    char *out_png_path;
    size_t out_png_path_cap;
} nh_profile_refresh_opts;

/* Full "fetch + download + install" for @user with account pubkey
 * @pubkey_hex. Returns NH_PROFILE_OK when either the icon or the real
 * name (or both) were successfully installed. A picture-download
 * failure with a successful name install still returns OK (icon
 * remains at whatever AccountsService had before). */
nh_profile_rc nh_profile_refresh(const char *user, const char *pubkey_hex,
                                 const nh_profile_refresh_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_HOMED_PROFILE_H */
