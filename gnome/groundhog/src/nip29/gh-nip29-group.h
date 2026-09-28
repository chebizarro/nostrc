#ifndef GH_NIP29_GROUP_H
#define GH_NIP29_GROUP_H

#include <glib.h>
#include <nip29.h>
#include <nostr-event.h>

G_BEGIN_DECLS

/* GTK-free NIP-29 relay-group core. A group is identified by its
 * authoritative relay and group id; its state is only what that relay's
 * NIP-11 `self` key signed. Nothing here performs I/O. Objects are not
 * thread-safe: the owning main context serializes access. */

#define GH_NIP29_ERROR gh_nip29_error_quark()
GQuark gh_nip29_error_quark(void);

typedef enum {
  GH_NIP29_ERROR_INVALID_RELAY_URL,
  GH_NIP29_ERROR_INVALID_GROUP_ID,
  GH_NIP29_ERROR_INVALID_PUBKEY,
  GH_NIP29_ERROR_INVALID_ARGUMENT,
  GH_NIP29_ERROR_FAILED
} GhNip29Error;

/* Canonical relay URL used for group identity: ws/wss only, lowercase
 * scheme and ASCII (punycode) host, default port dropped, RFC 3986
 * percent-encoding normalization, trailing '/' removed from the path.
 * Credentials, query strings and fragments are rejected rather than
 * silently discarded, so two different endpoints never share an identity. */
gchar *gh_nip29_normalize_relay_url(const gchar *url, GError **error);

/* ---- Identity ------------------------------------------------------------ */

/* (normalized relay URL, group id). The same id on two relays is two groups
 * (NIP-29 forks and migrations keep ids). Group ids are limited to the
 * charset nips/nip29 accepts ([a-z0-9-_]); see nostr_group_address_is_valid. */
typedef struct _GhNip29GroupKey GhNip29GroupKey;

GhNip29GroupKey *gh_nip29_group_key_new(const gchar *relay_url,
                                        const gchar *group_id,
                                        GError **error);
GhNip29GroupKey *gh_nip29_group_key_copy(const GhNip29GroupKey *key);
void gh_nip29_group_key_free(GhNip29GroupKey *key);
const gchar *gh_nip29_group_key_get_relay_url(const GhNip29GroupKey *key);
const gchar *gh_nip29_group_key_get_group_id(const GhNip29GroupKey *key);
/* GHashFunc/GEqualFunc over GhNip29GroupKey. */
guint gh_nip29_group_key_hash(gconstpointer key);
gboolean gh_nip29_group_key_equal(gconstpointer a, gconstpointer b);

/* ---- Relay-signed snapshot admission ------------------------------------ */

typedef enum {
  GH_NIP29_ADMISSION_ACCEPTED,
  /* Same event as the current snapshot; state unchanged. */
  GH_NIP29_ADMISSION_DUPLICATE,
  /* Not 39000/39001/39002/39003. */
  GH_NIP29_ADMISSION_UNSUPPORTED_KIND,
  /* No id or no signature. */
  GH_NIP29_ADMISSION_UNSIGNED,
  /* Structurally invalid id/pubkey/signature/fields. */
  GH_NIP29_ADMISSION_MALFORMED,
  /* Declared id is not the canonical hash, or the signature does not verify. */
  GH_NIP29_ADMISSION_FORGED,
  /* Validly signed, but not by this group's relay key. */
  GH_NIP29_ADMISSION_WRONG_AUTHOR,
  /* First `d` tag missing or not this group id. */
  GH_NIP29_ADMISSION_WRONG_GROUP,
  /* Older than the current snapshot of that kind, or the same created_at
   * with a lexically greater id (NIP-01 replaceable tie-break). */
  GH_NIP29_ADMISSION_STALE,
  /* Allocation or merge failure; state unchanged. */
  GH_NIP29_ADMISSION_FAILED
} GhNip29Admission;

const gchar *gh_nip29_admission_to_string(GhNip29Admission admission);

typedef struct _GhNip29Group GhNip29Group;

/* relay_pubkey is the hex key the caller read from the relay's NIP-11
 * `self` (or legacy `pubkey`) field. It is fixed for the group's lifetime:
 * if the relay key changes, build a new group so no snapshot signed by the
 * previous key survives. */
GhNip29Group *gh_nip29_group_new(const GhNip29GroupKey *key,
                                 const gchar *relay_pubkey,
                                 GError **error);
void gh_nip29_group_free(GhNip29Group *group);
const GhNip29GroupKey *gh_nip29_group_get_key(const GhNip29Group *group);
const gchar *gh_nip29_group_get_relay_pubkey(const GhNip29Group *group);

/* Admits a kind 39000-39003 event only if it is fully valid (canonical id
 * and Schnorr signature), authored by the relay key and addressed to this
 * group id, and newer than the current snapshot of the same kind. Rejections
 * leave state untouched. */
GhNip29Admission gh_nip29_group_admit(GhNip29Group *group,
                                      const NostrEvent *event);

/* Canonical id of the admitted snapshot of @kind, or NULL when none. */
const gchar *gh_nip29_group_get_snapshot_id(const GhNip29Group *group,
                                            gint kind,
                                            gint64 *out_created_at);

/* ---- kind:39000 metadata ------------------------------------------------- */

typedef struct {
  gchar *name;
  gchar *about;
  gchar *picture;
  gchar *banner;
  gboolean is_private;    /* only members can read */
  gboolean is_restricted; /* only members can write */
  gboolean is_hidden;     /* metadata hidden from non-members */
  gboolean is_closed;     /* join requests ignored without an invite code */
  gboolean has_livekit;
  /* FALSE: no supported_kinds tag, so every kind is assumed supported.
   * TRUE with n_supported_kinds == 0: no kinds are supported. */
  gboolean has_supported_kinds;
  gint *supported_kinds;
  gsize n_supported_kinds;
  gchar *parent;   /* NULL for a root group */
  GStrv children;  /* display order; NULL or empty when none */
} GhNip29Metadata;

GhNip29Metadata *gh_nip29_metadata_new(void);
GhNip29Metadata *gh_nip29_metadata_copy(const GhNip29Metadata *metadata);
void gh_nip29_metadata_free(GhNip29Metadata *metadata);
gboolean gh_nip29_metadata_supports_kind(const GhNip29Metadata *metadata,
                                         gint kind);

/* NULL until a relay-signed 39000 is admitted. */
GhNip29Metadata *gh_nip29_group_dup_metadata(const GhNip29Group *group);

/* ---- kind:39001 admins and kind:39003 roles ----------------------------- */

typedef struct {
  gchar *pubkey; /* 64-char lowercase hex */
  GStrv roles;   /* never NULL; may be empty */
} GhNip29Admin;

void gh_nip29_admin_free(GhNip29Admin *admin);

typedef struct {
  gchar *name;
  gchar *description; /* nullable */
} GhNip29Role;

void gh_nip29_role_free(GhNip29Role *role);

/* GhNip29Admin elements, or NULL until a 39001 is admitted. Entries whose
 * pubkey is not 64-char lowercase hex are omitted. */
GPtrArray *gh_nip29_group_dup_admins(const GhNip29Group *group);
/* Roles of @pubkey in the admitted 39001, or NULL when not listed. */
GStrv gh_nip29_group_dup_admin_roles(const GhNip29Group *group,
                                     const gchar *pubkey);
/* GhNip29Role elements, or NULL until a 39003 is admitted. */
GPtrArray *gh_nip29_group_dup_roles(const GhNip29Group *group);

/* ---- kind:39002 members -------------------------------------------------- */

typedef enum {
  /* No relay-signed 39002: membership is not available, not "zero". */
  GH_NIP29_MEMBERS_UNAVAILABLE,
  /* A 39002 was admitted. NIP-29 lets relays publish only a subset, so the
   * list is never proof of completeness. */
  GH_NIP29_MEMBERS_PARTIAL
} GhNip29MemberList;

typedef enum {
  GH_NIP29_MEMBERSHIP_LISTED,
  /* Not in the (possibly partial or absent) 39002. Never "not a member". */
  GH_NIP29_MEMBERSHIP_UNKNOWN
} GhNip29Membership;

/* out_pubkeys (nullable) receives the listed well-formed pubkeys, or NULL
 * when unavailable. */
GhNip29MemberList gh_nip29_group_dup_members(const GhNip29Group *group,
                                             GStrv *out_pubkeys);
GhNip29Membership gh_nip29_group_lookup_member(const GhNip29Group *group,
                                               const gchar *pubkey);

/* ---- Authorization ------------------------------------------------------- */

/* NIP-29 leaves what each role may do to relay policy, so role capabilities
 * are only known when the caller supplies them (for example from knowledge
 * of the relay implementation). A role absent from the policy is unknown. */
typedef struct _GhNip29RolePolicy GhNip29RolePolicy;

GhNip29RolePolicy *gh_nip29_role_policy_new(void);
void gh_nip29_role_policy_free(GhNip29RolePolicy *policy);
/* Declares the complete permission set the relay grants @role (possibly
 * empty), replacing any earlier declaration. */
gboolean gh_nip29_role_policy_set_role(GhNip29RolePolicy *policy,
                                       const gchar *role,
                                       const nostr_permission_t *permissions,
                                       gsize n_permissions,
                                       GError **error);

typedef enum {
  /* A listed role is granted the permission by the supplied policy. */
  GH_NIP29_AUTHZ_ALLOWED,
  /* Malformed pubkey or permission. */
  GH_NIP29_AUTHZ_DENIED_INVALID,
  /* A 39001 was admitted and does not list the pubkey. */
  GH_NIP29_AUTHZ_DENIED_NOT_ADMIN,
  /* A 39003 was admitted and none of the pubkey's roles is advertised there,
   * so none carries a relay privilege. */
  GH_NIP29_AUTHZ_DENIED_UNADVERTISED_ROLES,
  /* The policy knows every candidate role and none grants the permission. */
  GH_NIP29_AUTHZ_DENIED_BY_POLICY,
  /* No relay-signed 39001 is available. */
  GH_NIP29_AUTHZ_UNKNOWN_NO_ADMINS,
  /* Listed as admin, but the policy does not cover the roles held (or the
   * 39001 entry names no role; the relay still lists the user as
   * privileged). The UI may let the user attempt the action and surface
   * the relay's OK. */
  GH_NIP29_AUTHZ_UNKNOWN_POLICY
} GhNip29Authz;

const gchar *gh_nip29_authz_to_string(GhNip29Authz authz);

/* policy may be NULL (nothing known). */
GhNip29Authz gh_nip29_group_check_permission(const GhNip29Group *group,
                                             const GhNip29RolePolicy *policy,
                                             const gchar *pubkey,
                                             nostr_permission_t permission);
/* Conservative boolean: TRUE only for GH_NIP29_AUTHZ_ALLOWED. */
gboolean gh_nip29_group_can(const GhNip29Group *group,
                            const GhNip29RolePolicy *policy,
                            const gchar *pubkey,
                            nostr_permission_t permission);

/* 64-char lowercase hex (event ids and x-only pubkeys). */
gboolean gh_nip29_is_hex64(const gchar *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhNip29GroupKey, gh_nip29_group_key_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhNip29Group, gh_nip29_group_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhNip29Metadata, gh_nip29_metadata_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhNip29RolePolicy, gh_nip29_role_policy_free)

G_END_DECLS

#endif /* GH_NIP29_GROUP_H */
