/* Encrypted-group UI (Marmot MLS; privacy charter §7.5, §7.6, §7.7, §7.9,
 * §7.10, §7.15 #13, §1.4, D7, PD-8; nostrc-9xf5, qp24.13 part 2), with
 * GH_FEATURE_ENCRYPTED_GROUPS is on by default; the UI is also attached directly in these tests.
 *
 * Every test runs against the three-account world of the MLS service tests
 * (tests/mls/mls-world.h): real account controllers with the mock
 * org.nostr.Signer on the private test bus, real SQLCipher stores, the real
 * GhMlsService, and local store-and-serve relays through the real gnostr
 * transports (E discovery, W write, X inbox, G group relay with AUTH).
 *
 * Default mode (no display): the view model and its words. Every state's
 * copy; accepted contacts only (never a message request, never the account);
 * the KeyPackage check: ready, ready without the account proof (a legacy
 * KeyPackage; "needs an update" only when proofs are required, nostrc-6ukh),
 * not set up, no relay answered, no discovery
 * relay, cancelled; Owner and Admin from the group's admin order; the
 * composer's reasons (live, offline, left).
 *
 * --gui: on real windows. New Group's chooser and encrypted page end to end
 * (relays, a contact's check, create, Open Group), the lock glyph, the
 * header's "Encrypted group · N members", sending through the composer
 * delegate with an honest status, the invitee's "Group Invitations" entry
 * and dialog (nothing joins before Accept; a stranger shows as one), reading
 * the answer; Group Info (badges, admin Add with a fresh check, Rename,
 * Remove with its confirmation, a member seeing no admin action, Leave whose
 * copy says the others keep counting you); a removed member's composer and
 * Group Info naming who removed them; "Some messages in this group can't
 * be read yet" while a Commit is withheld, never just after joining; the
 * enrollment states with Try Again. It
 * exits 77 without a display.
 *
 * Waits iterate the main context; their deadlines are failure bounds only. */
#include "gh-conversation-list.h"
#include "gh-conversation-row.h"
#include "gh-conversation-view.h"
#include "gh-features.h"
#include "gh-group-ui.h"
#include "gh-mls-copy.h"
#include "gh-mls-group-info-dialog.h"
#include "gh-mls-invitee-picker.h"
#include "gh-mls-invites-page.h"
#include "gh-mls-new-group-page.h"
#include "gh-test-escape.h"
#include "gh-mls-ui.h"
#include "gh-message-row.h"
#include "gh-nip29-service.h"
#include "gh-shell.h"
#include "gh-test-dialog.h"
#include "group-send-stub.h"
#include "gh-recipient.h"
#include "gh-privacy-summary.h"
#include "mls-world.h"
#include "blossom-fixture.h"
#include "gh-attachment-card.h"
#include "nostrc-test-gdk-frame.h"
#include "gh-mls-attachments.h"

#include <glib/gi18n.h>

extern void groundhog_register_resource(void);

/* A running Groundhog account's check row: both KeyPackage formats as
 * shipped, the MDK 0.8 one alone without libmarmot's adopted producer
 * (nostrc-lf62). */
#define GROUNDHOG_READY (GH_MLS_ADOPTED_KEY_PACKAGES ? GH_MLS_INVITEE_READY \
                                                     : GH_MLS_INVITEE_READY_LEGACY)

/* ---- helpers ------------------------------------------------------------------------ */

static void
wait_published(World *w, const guint *keys, guint n)
{
  for (guint i = 0; i < n; i++)
    spin_until(key_package_published, &w->apps[keys[i]], "a KeyPackage published");
}

/* A message request: an incoming rumor from someone the account never
 * accepted. */
static GhConversation *
admit_request(App *app, guint from)
{
  /* An unsigned kind-14 rumor, as a gift wrap carries it. */
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 14);
  nostr_event_set_pubkey(event, hex[from]);
  nostr_event_set_created_at(event, g_get_real_time() / G_USEC_PER_SEC - 60);
  nostr_event_set_content(event, "hello from a stranger");
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("p", hex[app->key], NULL));
  nostr_event_set_tags(event, tags);
  event->id = nostr_event_get_id(event);
  char *serialized = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_autofree gchar *json = g_strdup(serialized);
  free(serialized);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) message = gh_message_new_from_rumor(hex[app->key], json, &error);
  g_assert_no_error(error);
  gh_conversation_store_admit(app->model, message, NULL, &error);
  g_assert_no_error(error);
  GhConversation *room = gh_conversation_store_lookup(app->model,
                                                      gh_message_get_room_id(message));
  g_assert_nonnull(room);
  g_assert_true(gh_conversation_get_is_request(room));
  return room;
}

/* `key` runs an older client: a KeyPackage without the account proof on W.
 * Its event id (transfer full). */
static gchar *
inject_legacy_key_package(World *w, guint key)
{
  MarmotConfig config = marmot_config_default();
  config.allow_unproven_self = true;
  Marmot *legacy = marmot_new_with_config(marmot_storage_memory_new(), &config);
  guint8 pubkey[32];
  g_assert_true(nostr_hex2bin(pubkey, hex[key], sizeof pubkey));
  const char *relays[] = { w->w.url };
  MarmotKeyPackageResult made;
  memset(&made, 0, sizeof made);
  g_assert_cmpint(marmot_create_key_package_unsigned(legacy, pubkey, relays, 1, &made), ==,
                  MARMOT_OK);
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, made.event_json, NULL), ==, 1);
  g_assert_cmpint(nostr_event_sign(event, gh_test_secret[key]), ==, 0);
  char *signed_json = nostr_event_serialize_compact(event);
  gchar *id = event_id_dup(event);
  nostr_event_free(event);
  wire_relay_inject(&w->w, signed_json);
  free(signed_json);
  marmot_key_package_result_free(&made);
  marmot_free(legacy);
  return id;
}

#if GH_MLS_ADOPTED_KEY_PACKAGES
/* `key` runs an app that speaks only the adopted format (White Noise on MDK
 * 0.11): an adopted KeyPackage, its account proof signed with the key, on
 * W. */
static void
inject_adopted_key_package(World *w, guint key)
{
  Marmot *other = marmot_new(marmot_storage_memory_new());
  guint8 pubkey[32], secret[32];
  g_assert_true(nostr_hex2bin(pubkey, hex[key], sizeof pubkey));
  g_assert_true(nostr_hex2bin(secret, gh_test_secret[key], sizeof secret));
  MarmotKeyPackageResult made;
  memset(&made, 0, sizeof made);
  g_assert_cmpint(marmot_create_key_package_for_profile(other, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED,
                                                        pubkey, secret, NULL, NULL, NULL, 0,
                                                        &made), ==, MARMOT_OK);
  memset(secret, 0, sizeof secret);
  wire_relay_inject(&w->w, made.event_json);
  marmot_key_package_result_free(&made);
  marmot_free(other);
}
#endif

typedef struct {
  gboolean done;
  GhMlsInviteeState state;
  GError *error;
} CheckWait;

static gboolean
check_done(gpointer data)
{
  return ((CheckWait *)data)->done;
}

static void
on_checked(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  CheckWait *wait = data;
  wait->state = gh_mls_invitee_check_finish(result, &wait->error);
  wait->done = TRUE;
}

static GhMlsInviteeState
check_with_relays(App *app, GSettings *settings, guint key,
                  const gchar *const *group_relays,
                  GCancellable *cancellable, GError **error)
{
  CheckWait wait = { 0 };
  gh_mls_invitee_check_async(app->accounts, settings, hex[key], 20, group_relays, cancellable,
                             on_checked, &wait);
  spin_until(check_done, &wait, "the KeyPackage check");
  if (wait.error)
    g_propagate_error(error, wait.error);
  return wait.state;
}

static GhMlsInviteeState
check_with(App *app, GSettings *settings, guint key, GCancellable *cancellable, GError **error)
{
  return check_with_relays(app, settings, key, NULL, cancellable, error);
}

static GhMlsInviteeState
check(App *app, guint key)
{
  g_autoptr(GError) error = NULL;
  GhMlsInviteeState state = check_with(app, app->settings, key, NULL, &error);
  g_assert_no_error(error);
  return state;
}

static GSettings *
settings_with_discovery(const gchar *url)
{
  g_autoptr(GSettingsBackend) backend = g_memory_settings_backend_new();
  GSettings *settings = g_settings_new_with_backend("org.nostr.Groundhog", backend);
  const gchar *urls[] = { url, NULL };
  g_settings_set_strv(settings, "discovery-relays", url ? urls : (const gchar *[]){ NULL });
  return settings;
}

/* Whether anyone asked relay for pubkey's KeyPackages (a REQ naming kind
 * 30443 and the person: only a lookup does). */
static gboolean
key_package_asked(WireRelay *relay, const gchar *pubkey)
{
  for (guint i = 0; i < relay->frames->len; i++) {
    WireFrame *frame = g_ptr_array_index(relay->frames, i);
    if (frame->inbound && g_str_has_prefix(frame->text, "[\"REQ\"") &&
        strstr(frame->text, "30443") && strstr(frame->text, pubkey))
      return TRUE;
  }
  return FALSE;
}

static gboolean
strv_has(const gchar *const *strv, const gchar *value)
{
  return strv && g_strv_contains(strv, value);
}

/* ---- default mode: the words ------------------------------------------------------ */

static void
test_copy(void)
{
  /* nostrc-xrya: an ended group says why. */
  g_assert_null(gh_mls_end_copy(GH_MLS_GROUP_END_NONE, NULL));
  g_autofree gchar *left = gh_mls_end_copy(GH_MLS_GROUP_END_LEFT, "Alice");
  g_assert_cmpstr(left, ==, "You left this group. Its messages stay on this device.");
  /* nostrc-2um6: leaving on this device only says the others still count you. */
  g_autofree gchar *left_here = gh_mls_end_copy(GH_MLS_GROUP_END_LEFT_DEVICE, NULL);
  g_assert_nonnull(strstr(left_here, "on this device"));
  g_assert_nonnull(strstr(left_here, "still count you"));
  g_assert_nonnull(strstr(gh_mls_leave_copy(GH_MLS_LEAVE_EVERYONE), "are told that you left"));
  g_assert_nonnull(strstr(gh_mls_leave_copy(GH_MLS_LEAVE_ADMINS), "admins are asked"));
  g_assert_nonnull(strstr(gh_mls_leave_copy(GH_MLS_LEAVE_DEVICE_ADMIN), "step down first"));
  g_assert_nonnull(strstr(gh_mls_leave_copy(GH_MLS_LEAVE_DEVICE_UNSUPPORTED),
                          "can’t process a member leaving"));
  g_assert_nonnull(strstr(gh_mls_leave_copy(GH_MLS_LEAVE_DEVICE_WAITING), "Stop waiting?"));
  for (GhMlsLeave k = GH_MLS_LEAVE_DEVICE_ADMIN; k <= GH_MLS_LEAVE_DEVICE; k++)
    g_assert_nonnull(strstr(gh_mls_leave_copy(k), "keep counting you"));
  g_autofree gchar *gone = gh_mls_member_left_copy("Bob");
  g_assert_cmpstr(gone, ==, "Bob left the group");
  /* nostrc-xrza: a conflict resolved says what it undid. */
  g_autofree gchar *conflict = gh_mls_conflict_copy(0, GH_MLS_UNDONE_NONE);
  g_assert_cmpstr(conflict, ==,
                  "The group resolved a conflict between changes made at the same time");
  g_autofree gchar *undone = gh_mls_conflict_copy(2, GH_MLS_UNDONE_NAME | GH_MLS_UNDONE_ADMINS);
  g_assert_cmpstr(undone, ==,
                  "The group resolved a conflict between changes made at the same time: a "
                  "change of its name was undone; a change of its admins was undone; 2 messages "
                  "were withdrawn");
  g_autofree gchar *one = gh_mls_conflict_copy(1, GH_MLS_UNDONE_MEMBERS);
  g_assert_nonnull(strstr(one, "a change of its members was undone; 1 message was withdrawn"));
  g_autofree gchar *removed_by = gh_mls_end_copy(GH_MLS_GROUP_END_REMOVED, "Alice");
  g_assert_cmpstr(removed_by, ==,
                  "You were removed from this group by Alice. Its messages stay on this device.");
  g_autofree gchar *removed = gh_mls_end_copy(GH_MLS_GROUP_END_REMOVED, NULL);
  g_assert_cmpstr(removed, ==, "You were removed from this group. Its messages stay on this device.");
  /* A damaged record: that it ended, never "You left" (W22 review N3). */
  g_autofree gchar *unknown = gh_mls_end_copy(GH_MLS_GROUP_END_UNKNOWN, "Alice");
  g_assert_cmpstr(unknown, ==,
                  "This group has ended on this device. Its messages stay on this device.");

  g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
  for (gint state = GH_MLS_INVITEE_CHECKING; state <= GH_MLS_INVITEE_FAILED; state++) {
    const gchar *words = gh_mls_invitee_copy(state);
    g_assert_nonnull(words);
    g_assert_cmpuint(strlen(words), >, 0);
    g_assert_false(g_hash_table_contains(seen, words));   /* every state its own */
    g_hash_table_add(seen, (gpointer)words);
    gboolean ready = state == GH_MLS_INVITEE_READY || state == GH_MLS_INVITEE_READY_ADOPTED_ONLY ||
                     state == GH_MLS_INVITEE_READY_LEGACY ||
                     state == GH_MLS_INVITEE_READY_UNPROVEN;
    g_assert_cmpint(gh_mls_invitee_can_invite(state), ==, ready);
    /* Which group format each can join (nostrc-lf62). */
    g_assert_cmpint(gh_mls_invitee_can_join(state, TRUE), ==,
                    state == GH_MLS_INVITEE_READY || state == GH_MLS_INVITEE_READY_ADOPTED_ONLY);
    g_assert_cmpint(gh_mls_invitee_can_join(state, FALSE), ==,
                    state == GH_MLS_INVITEE_READY || state == GH_MLS_INVITEE_READY_LEGACY ||
                    state == GH_MLS_INVITEE_READY_UNPROVEN);
  }
  g_assert_cmpstr(gh_mls_invitee_copy(GH_MLS_INVITEE_READY), ==, "Ready to invite");
  g_assert_nonnull(strstr(gh_mls_invitee_copy(GH_MLS_INVITEE_READY_LEGACY), "older format"));
  g_assert_nonnull(strstr(gh_mls_invitee_copy(GH_MLS_INVITEE_READY_ADOPTED_ONLY),
                          "newer format"));
  g_assert_cmpstr(gh_mls_invitee_copy(GH_MLS_INVITEE_NOT_SET_UP), ==,
                  "Hasn’t set up encrypted groups");
  g_assert_nonnull(strstr(gh_mls_invitee_copy(GH_MLS_INVITEE_NEEDS_UPDATE),
                          "can’t prove their account"));
  g_assert_nonnull(strstr(gh_mls_invitee_copy(GH_MLS_INVITEE_NEEDS_UPDATE),
                          "every member’s app proves their account"));
  g_assert_nonnull(strstr(gh_mls_invitee_copy(GH_MLS_INVITEE_READY_UNPROVEN),
                          "Identity not verified"));

  /* nostrc-6ukh: a member whose identity isn't confirmed, with who added
   * them; a proven or verified one shows nothing. */
  for (gint identity = GH_MLS_MEMBER_PROVEN; identity <= GH_MLS_MEMBER_VERIFIED; identity++) {
    GhMlsMemberCopy plain = gh_mls_member_copy(identity, "Alice", FALSE);
    g_assert_null(plain.badge);
    g_assert_null(plain.explanation);
    gh_mls_member_copy_clear(&plain);
  }
  GhMlsMemberCopy unverified = gh_mls_member_copy(GH_MLS_MEMBER_UNVERIFIED, "Alice", FALSE);
  g_assert_cmpstr(unverified.badge, ==, "Identity not verified");
  g_assert_cmpstr(unverified.explanation, ==,
                  "Added by Alice. Groundhog couldn’t confirm this account owns this device.");
  g_assert_nonnull(strstr(unverified.accessible, "Identity not verified"));
  gh_mls_member_copy_clear(&unverified);
  /* Nobody known (an MDK group's creator, owkh): gentle, no blame. */
  GhMlsMemberCopy before = gh_mls_member_copy(GH_MLS_MEMBER_UNVERIFIED, NULL, FALSE);
  g_assert_nonnull(strstr(before.explanation, "older apps"));
  g_assert_null(strstr(before.explanation, "Added by"));
  gh_mls_member_copy_clear(&before);
  /* Their own device (W24 review M1): never "Added by <themselves>". */
  GhMlsMemberCopy own = gh_mls_member_copy(GH_MLS_MEMBER_UNVERIFIED, "Dave", TRUE);
  g_assert_null(strstr(own.explanation, "Added by"));
  g_assert_nonnull(strstr(own.explanation, "themselves"));
  gh_mls_member_copy_clear(&own);
  /* Checking claims no failure while it runs (W24 review L4). */
  GhMlsMemberCopy checking = gh_mls_member_copy(GH_MLS_MEMBER_CHECKING, "Alice", FALSE);
  g_assert_cmpstr(checking.badge, ==, "Checking identity…");
  g_assert_null(strstr(checking.explanation, "couldn’t"));
  gh_mls_member_copy_clear(&checking);
  /* nostrc-prrl: a refused change is said as such, by its cause, never as
   * a wait; no admin is blamed (W24 review L4). */
  g_assert_null(gh_mls_refused_copy(GH_MLS_REFUSAL_NONE));
  for (gint refusal = GH_MLS_REFUSAL_BROKEN_PROOF; refusal <= GH_MLS_REFUSAL_UNFOLLOWABLE;
       refusal++) {
    const gchar *refused = gh_mls_refused_copy(refusal);
    g_assert_null(strstr(refused, "yet"));
    g_assert_null(strstr(refused, "Waiting"));
    g_assert_null(strstr(refused, "admin"));
    g_assert_nonnull(strstr(refused, "can’t be read"));
  }
  g_assert_nonnull(strstr(gh_mls_refused_copy(GH_MLS_REFUSAL_UNPROVEN),
                          "Turning that preference off"));
  g_assert_null(strstr(gh_mls_refused_copy(GH_MLS_REFUSAL_BROKEN_PROOF), "preference"));
  /* W24b slice H review L2: a change Groundhog can't follow. */
  g_assert_nonnull(strstr(gh_mls_refused_copy(GH_MLS_REFUSAL_UNFOLLOWABLE), "can’t follow"));
  g_assert_null(strstr(gh_mls_refused_copy(GH_MLS_REFUSAL_UNFOLLOWABLE), "preference"));
  /* Re-review R4: when every member refused it, the group never moved:
   * these two say messages are unreadable only if others accepted it. */
  g_assert_nonnull(strstr(gh_mls_refused_copy(GH_MLS_REFUSAL_UNFOLLOWABLE),
                          "If other members accepted it"));
  g_assert_nonnull(strstr(gh_mls_refused_copy(GH_MLS_REFUSAL_BROKEN_PROOF),
                          "If other members accepted it"));
  /* Verify says what it reveals, and what came of it (W24 review H1). */
  g_autofree gchar *prompt = gh_mls_verify_prompt("Dave");
  g_assert_nonnull(strstr(prompt, "Those relays can see whom you look up"));
  g_autofree gchar *yes = gh_mls_verify_result_copy(GH_MLS_MEMBER_VERIFIED, NULL, "Dave");
  g_assert_cmpstr(yes, ==, "Verified: Dave published this device’s key.");
  g_autofree gchar *no = gh_mls_verify_result_copy(GH_MLS_MEMBER_UNVERIFIED, NULL, "Dave");
  g_assert_nonnull(strstr(no, "stays unverified"));
  g_autoptr(GError) silent = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE, "x");
  g_autofree gchar *nobody = gh_mls_verify_result_copy(GH_MLS_MEMBER_UNVERIFIED, silent, "Dave");
  g_assert_cmpstr(nobody, ==, "No relay answered, so nothing was checked.");
  g_autoptr(GError) reveal = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "x");
  g_autofree gchar *group_only = gh_mls_verify_result_copy(GH_MLS_MEMBER_UNVERIFIED, reveal,
                                                           "Dave");
  g_assert_cmpstr(group_only, ==,
                  "Every relay Groundhog could ask is one of this group's relays, "
                  "so checking would reveal this group.");

  GhMlsIdentityCopy ready = gh_mls_identity_copy(GH_MLS_IDENTITY_ENROLLED);
  g_assert_true(ready.ready);
  g_assert_null(ready.title);
  g_assert_true(gh_mls_identity_copy(GH_MLS_IDENTITY_NOT_REQUIRED).ready);
  GhMlsIdentityCopy waiting = gh_mls_identity_copy(GH_MLS_IDENTITY_WAITING);
  g_assert_false(waiting.ready);
  g_assert_true(waiting.busy);
  g_assert_false(waiting.can_retry);
  g_assert_cmpstr(waiting.title, ==, "Waiting for approval in your signer…");
  GhMlsIdentityCopy declined = gh_mls_identity_copy(GH_MLS_IDENTITY_DECLINED);
  g_assert_cmpstr(declined.title, ==, "Declined in your signer");
  g_assert_true(declined.can_retry);
  g_assert_false(declined.busy);
  g_assert_true(gh_mls_identity_copy(GH_MLS_IDENTITY_FAILED).can_retry);
  GhMlsIdentityCopy none = gh_mls_identity_copy(GH_MLS_IDENTITY_NONE);
  g_assert_false(none.ready);
  g_assert_false(none.can_retry);
  g_assert_nonnull(none.title);

  g_autoptr(GHashTable) errors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (gint code = GH_MLS_SERVICE_ERROR_NO_CONSENT;
       code <= GH_MLS_SERVICE_ERROR_INVITEE_UNSUPPORTED; code++) {
    g_autoptr(GError) error = g_error_new(GH_MLS_SERVICE_ERROR, code, "internal detail %d", code);
    gchar *words = gh_mls_error_copy(error);
    g_assert_null(strstr(words, "internal detail"));   /* plain words, not the log's */
    g_assert_false(g_hash_table_contains(errors, words));
    g_hash_table_add(errors, words);
  }
  g_autoptr(GError) update = g_error_new_literal(GH_MLS_SERVICE_ERROR,
                                                 GH_MLS_SERVICE_ERROR_NEEDS_UPDATE, "x");
  g_autofree gchar *update_words = gh_mls_error_copy(update);
  g_assert_nonnull(strstr(update_words, "every member’s app proves their account"));
  g_assert_nonnull(strstr(update_words, "Nothing was changed"));
  g_autoptr(GError) forged = g_error_new_literal(GH_MLS_SERVICE_ERROR,
                                                 GH_MLS_SERVICE_ERROR_FORGED_IDENTITY, "x");
  g_autofree gchar *forged_words = gh_mls_error_copy(forged);
  g_assert_nonnull(strstr(forged_words, "forged"));
  /* Group formats (nostrc-lf62). */
  g_autoptr(GError) mixed = g_error_new_literal(GH_MLS_SERVICE_ERROR,
                                                GH_MLS_SERVICE_ERROR_MIXED_PROFILE, "x");
  g_autofree gchar *mixed_words = gh_mls_error_copy(mixed);
  g_assert_nonnull(strstr(mixed_words, "can’t use both"));
  g_assert_nonnull(strstr(mixed_words, "Nothing was changed"));
  g_autoptr(GError) mismatch = g_error_new_literal(GH_MLS_SERVICE_ERROR,
                                                   GH_MLS_SERVICE_ERROR_PROFILE_MISMATCH, "x");
  g_autofree gchar *mismatch_words = gh_mls_error_copy(mismatch);
  g_assert_nonnull(strstr(mismatch_words, "this group’s format"));
  g_autoptr(GError) taken = g_error_new_literal(GH_MLS_SERVICE_ERROR,
                                                GH_MLS_SERVICE_ERROR_ADDRESS_TAKEN, "x");
  g_autofree gchar *taken_words = gh_mls_error_copy(taken);
  g_assert_nonnull(strstr(taken_words, "same group address as another group"));
  g_autoptr(GError) unsupported = g_error_new_literal(GH_MLS_SERVICE_ERROR,
                                                      GH_MLS_SERVICE_ERROR_INVITEE_UNSUPPORTED,
                                                      "x");
  g_autofree gchar *unsupported_words = gh_mls_error_copy(unsupported);
  g_assert_nonnull(strstr(unsupported_words, "leave on their own"));
  g_assert_nonnull(strstr(taken_words, "refused"));

  g_autofree gchar *from_contact = gh_mls_invite_subtitle("npub1abcd…wxyz", "Bob", TRUE, 3);
  g_assert_cmpstr(from_contact, ==, "From Bob · 3 members");
  g_autofree gchar *from_npub = gh_mls_invite_subtitle("npub1abcd…wxyz", NULL, TRUE, 1);
  g_assert_cmpstr(from_npub, ==, "From npub1abcd…wxyz · 1 member");
  /* PD-8: a stranger's name is never shown, even if one were cached. */
  g_autofree gchar *stranger = gh_mls_invite_subtitle("npub1abcd…wxyz", "Mallory", FALSE, 2);
  g_assert_null(strstr(stranger, "Mallory"));
  g_assert_nonnull(strstr(stranger, "not in your contacts"));

  g_assert_cmpstr(gh_mls_role_copy(GH_MLS_ROLE_OWNER), ==, "Owner");
  g_assert_cmpstr(gh_mls_role_copy(GH_MLS_ROLE_ADMIN), ==, "Admin");
  g_assert_null(gh_mls_role_copy(GH_MLS_ROLE_MEMBER));
  const gchar *admins[] = { "bb", "aa", NULL };
  g_assert_cmpint(gh_mls_role_of(admins, "bb"), ==, GH_MLS_ROLE_OWNER);
  g_assert_cmpint(gh_mls_role_of(admins, "AA"), ==, GH_MLS_ROLE_ADMIN);
  g_assert_cmpint(gh_mls_role_of(admins, "cc"), ==, GH_MLS_ROLE_MEMBER);
  g_assert_cmpint(gh_mls_role_of(NULL, "aa"), ==, GH_MLS_ROLE_MEMBER);

  for (gint read = GH_MLS_READ_IDLE; read <= GH_MLS_READ_DISCONNECTED; read++)
    g_assert_cmpuint(strlen(gh_mls_read_copy(read)), >, 0);

  g_autofree gchar *bare = gh_mls_parse_relay("  relay.example.com ", NULL);
  g_assert_cmpstr(bare, ==, "wss://relay.example.com");
  g_autofree gchar *local = gh_mls_parse_relay("ws://127.0.0.1:7777", NULL);
  g_assert_cmpstr(local, ==, "ws://127.0.0.1:7777");
  const gchar *bad[] = { "", "ws://relay.example.com", "https://relay.example.com",
                         "wss://user@relay.example.com" };
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
    g_autoptr(GError) error = NULL;
    g_assert_null(gh_mls_parse_relay(bad[i], &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  }

  /* The pure classification of a lookup's answer. */
  g_autoptr(GError) not_found = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "x");
  g_autoptr(GError) unreachable = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE,
                                                      "x");
  g_autoptr(GError) no_relays = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "x");
  g_autoptr(GError) other = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "x");
  g_assert_cmpint(gh_mls_invitee_classify(NULL, not_found, FALSE), ==,
                  GH_MLS_INVITEE_NOT_SET_UP);
  g_assert_cmpint(gh_mls_invitee_classify(NULL, unreachable, FALSE), ==,
                  GH_MLS_INVITEE_UNREACHABLE);
  g_assert_cmpint(gh_mls_invitee_classify(NULL, no_relays, FALSE), ==,
                  GH_MLS_INVITEE_NO_RELAYS);
  g_assert_cmpint(gh_mls_invitee_classify(NULL, other, FALSE), ==,
                  GH_MLS_INVITEE_FAILED);
}

/* ---- default mode: people, checks, roles, reasons ----------------------------------- */

static void
test_view_model(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_published(&w, keys, G_N_ELEMENTS(keys));

  /* Contacts: accepted conversations' people only; a message request, a
   * note to self and the account never. */
  g_auto(GStrv) nobody = gh_mls_contacts_dup(alice->model);
  g_assert_cmpuint(g_strv_length(nobody), ==, 0);
  admit_request(alice, STRANGER);
  const gchar *self_only[] = { hex[ALICE], NULL };
  g_autoptr(GError) error = NULL;
  gh_conversation_store_open_room(alice->model, self_only, &error);
  g_assert_no_error(error);
  accept_contact(alice, BOB);
  const gchar *pair[] = { hex[BOB], hex[CAROL], NULL };
  g_assert_nonnull(gh_conversation_store_open_room(alice->model, pair, &error));
  g_assert_no_error(error);
  g_auto(GStrv) contacts = gh_mls_contacts_dup(alice->model);
  g_assert_cmpuint(g_strv_length(contacts), ==, 2);
  g_assert_true(strv_has((const gchar *const *)contacts, hex[BOB]));
  g_assert_true(strv_has((const gchar *const *)contacts, hex[CAROL]));
  g_assert_false(strv_has((const gchar *const *)contacts, hex[STRANGER]));
  g_assert_false(strv_has((const gchar *const *)contacts, hex[ALICE]));
  g_autofree gchar *upper = g_ascii_strup(hex[BOB], -1);
  g_assert_true(gh_mls_is_contact(alice->model, upper));
  g_assert_false(gh_mls_is_contact(alice->model, hex[STRANGER]));

  /* The KeyPackage check, each honest state. */
  g_assert_cmpint(check(alice, BOB), ==, GROUNDHOG_READY);
  g_assert_cmpint(check(alice, CAROL), ==, GH_MLS_INVITEE_NOT_SET_UP);
#if GH_MLS_SERVICE_ACCOUNT_PROOF
  g_free(inject_legacy_key_package(&w, CAROL));
  /* nostrc-6ukh: invitable by default; refused when proofs are required. */
  g_assert_cmpint(check(alice, CAROL), ==, GH_MLS_INVITEE_READY_UNPROVEN);
  g_settings_set_boolean(alice->settings, "only-join-verified-mls-groups", TRUE);
  g_assert_cmpint(check(alice, CAROL), ==, GH_MLS_INVITEE_NEEDS_UPDATE);
  g_settings_set_boolean(alice->settings, "only-join-verified-mls-groups", FALSE);
#if GH_MLS_ADOPTED_KEY_PACKAGES
  /* Review L2: Carol also gets an adopted KeyPackage. Her MDK 0.8 one still
   * lacks the proof: by default she is ready for any group; when proofs
   * are required, only for newer-format ones (her older-format key can't be
   * used), never a plain "Ready". */
  inject_adopted_key_package(&w, CAROL);
  g_assert_cmpint(check(alice, CAROL), ==, GH_MLS_INVITEE_READY);
  g_settings_set_boolean(alice->settings, "only-join-verified-mls-groups", TRUE);
  g_assert_cmpint(check(alice, CAROL), ==, GH_MLS_INVITEE_READY_ADOPTED_ONLY);
  g_settings_set_boolean(alice->settings, "only-join-verified-mls-groups", FALSE);
#endif
#endif
  g_autoptr(GSettings) none = settings_with_discovery(NULL);
  g_assert_cmpint(check_with(alice, none, BOB, NULL, NULL), ==, GH_MLS_INVITEE_NO_RELAYS);
  g_autoptr(GSettings) closed = settings_with_discovery("ws://127.0.0.1:1");
  g_assert_cmpint(check_with(alice, closed, BOB, NULL, NULL), ==, GH_MLS_INVITEE_UNREACHABLE);
  g_autoptr(GCancellable) cancelled = g_cancellable_new();
  g_cancellable_cancel(cancelled);
  check_with(alice, alice->settings, BOB, cancelled, &error);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&error);

  /* nostrc-c0yo: when the group relay IS the invitee's write relay, the
   * lookup must exclude it (privacy: the group relay must not learn who is
   * being added).  Without the fix, check() would still say READY because
   * the lookup fetches the KeyPackage from the group relay. */
  const gchar *shared_relays[] = { w.w.url, NULL };
  g_assert_cmpint(
      check_with_relays(alice, alice->settings, BOB, shared_relays, NULL, NULL),
      ==, GH_MLS_INVITEE_NOT_SET_UP);

  /* nostrc-46k7: an adopted group made by a user who has Blossom servers
   * carries those servers as the 0x800b media policy, so admins can set a
   * group picture and members know where to fetch attachments. */
  const gchar *blossom[] = { "https://blossom.example.com/", NULL };
  g_settings_set_strv(alice->settings, "blossom-servers", blossom);
  GhMlsGroup *gm = create_group(alice, "Media", (const guint[]){ BOB }, 1);
  MarmotGroupComponents mc;
  memset(&mc, 0, sizeof mc);
  g_assert_true(gh_mls_service_get_components(alice->service, gm, &mc, NULL));
  g_assert_true(mc.has_media_policy);
  g_assert_cmpuint(mc.media_policy.default_blob_endpoint_count, ==, 1);
  g_assert_cmpstr(mc.media_policy.default_blob_endpoints[0].base_url, ==,
                  "https://blossom.example.com/");
  g_assert_cmpstr(mc.media_policy.default_blob_endpoints[0].locator_kind, ==,
                  MARMOT_MEDIA_LOCATOR_BLOSSOM_V1);
  marmot_group_components_clear(&mc);
  g_settings_reset(alice->settings, "blossom-servers");
  join(bob, ALICE);  /* accept the Media invite so it does not interfere */

  /* Roles: the creator is the Owner; a second admin is an Admin. */
  GhMlsGroup *ga = create_group(alice, "Roles", (const guint[]){ BOB }, 1);
  GhMlsGroup *gb = join(bob, ALICE);
  g_auto(GStrv) admins = gh_mls_group_dup_ordered_admins(alice->service, ga);
  g_assert_cmpuint(g_strv_length(admins), ==, 1);
  g_assert_cmpint(gh_mls_role_of((const gchar *const *)admins, hex[ALICE]), ==,
                  GH_MLS_ROLE_OWNER);
  g_assert_cmpint(gh_mls_role_of((const gchar *const *)admins, hex[BOB]), ==,
                  GH_MLS_ROLE_MEMBER);
  const gchar *both[] = { hex[ALICE], hex[BOB], NULL };
  OpWait promoted = { 0 };
  gh_mls_service_set_admins_async(alice->service, ga, both, NULL, on_changed, &promoted);
  spin_until(op_done, &promoted, "the admin change");
  g_assert_no_error(promoted.error);
  wait_epoch(gb, (gint)gh_mls_group_get_epoch(ga));
  g_auto(GStrv) seen_by_bob = gh_mls_group_dup_ordered_admins(bob->service, gb);
  g_assert_cmpint(gh_mls_role_of((const gchar *const *)seen_by_bob, hex[ALICE]), ==,
                  GH_MLS_ROLE_OWNER);
  g_assert_cmpint(gh_mls_role_of((const gchar *const *)seen_by_bob, hex[BOB]), ==,
                  GH_MLS_ROLE_ADMIN);

  /* The composer's reasons follow the group. */
  wait_live(ga);
  g_autofree gchar *live = gh_mls_send_reason(alice->service, ga, NULL);
  g_assert_null(live);
  g_autofree gchar *no_service = gh_mls_send_reason(NULL, ga, NULL);
  g_assert_nonnull(strstr(no_service, "aren’t running"));
  set_online(alice, FALSE);
  g_autofree gchar *offline = gh_mls_send_reason(alice->service, ga, NULL);
  g_assert_nonnull(strstr(offline, "only while you’re online"));
  set_online(alice, TRUE);
  wait_live(ga);
  g_assert_true(gh_mls_service_leave(bob->service, gb, &error));
  g_assert_no_error(error);
  g_autofree gchar *left = gh_mls_send_reason(bob->service, gb, NULL);
  g_assert_cmpstr(left, ==, "You left this group.");
  world_down(&w);
}

/* ---- default mode: DM shape, request routing, co-members, badges, protocol -- */

/* W26 slice A tests (nostrc-fmbt, nostrc-txnu, nostrc-57o8, nostrc-e92q):
 * Marmot DM shape detection (2-member group with empty name), request
 * routing (stranger DMs are requests, known contacts are auto-accepted),
 * group co-members in the invitee list, protocol badge strings, and the
 * default-dm-protocol setting. */

static void
test_dm_shape(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_published(&w, keys, G_N_ELEMENTS(keys));

  /* ---- DM shape detection ------------------------------------------------
   * A 2-member group with empty name is a Marmot DM: is_direct=TRUE. */
  accept_contact(alice, BOB);
  GhMlsGroup *dm_group = create_group(alice, "", (const guint[]){ BOB }, 1);
  g_assert_null(gh_mls_group_get_name(dm_group));
  const gchar *dm_room_id = gh_mls_group_get_room_id(dm_group);
  GhConversation *dm_conv = gh_conversation_store_lookup(alice->model, dm_room_id);
  g_assert_nonnull(dm_conv);
  g_assert_true(gh_conversation_get_is_direct(dm_conv));
  g_assert_cmpint(gh_conversation_get_backend(dm_conv), ==, GH_CONVERSATION_BACKEND_MLS);

  /* Alice created it: never a request. */
  g_assert_false(gh_conversation_get_is_request(dm_conv));

  /* ---- Request routing (nostrc-57o8) ------------------------------------
   * Bob joins the DM. He has no prior accepted DM with Alice, so the
   * incoming Marmot DM is a message request (stranger). */
  GhMlsGroup *bob_dm = join(bob, ALICE);
  GhConversation *bob_dm_conv = gh_conversation_store_lookup(
    bob->model, gh_mls_group_get_room_id(bob_dm));
  g_assert_nonnull(bob_dm_conv);
  g_assert_true(gh_conversation_get_is_direct(bob_dm_conv));
  g_assert_true(gh_conversation_get_is_request(bob_dm_conv));

  /* Accept it: Alice is now a known contact of Bob. */
  gh_conversation_accept(bob_dm_conv);
  g_assert_false(gh_conversation_get_is_request(bob_dm_conv));

  /* A named group with the same members is not a DM, and never a request. */
  GhMlsGroup *named = create_group(alice, "Chat", (const guint[]){ BOB }, 1);
  GhConversation *named_conv = gh_conversation_store_lookup(
    alice->model, gh_mls_group_get_room_id(named));
  g_assert_nonnull(named_conv);
  g_assert_false(gh_conversation_get_is_direct(named_conv));
  g_assert_false(gh_conversation_get_is_request(named_conv));

  GhMlsGroup *bob_named = join(bob, ALICE);
  GhConversation *bob_named_conv = gh_conversation_store_lookup(
    bob->model, gh_mls_group_get_room_id(bob_named));
  g_assert_nonnull(bob_named_conv);
  g_assert_false(gh_conversation_get_is_direct(bob_named_conv));
  g_assert_false(gh_conversation_get_is_request(bob_named_conv));

  /* ---- Auto-accept from known contact -----------------------------------
   * A second DM from Alice: Bob already accepted Alice, so it is
   * auto-accepted (not a request). */
  GhMlsGroup *dm2 = create_group(alice, "", (const guint[]){ BOB }, 1);
  (void)dm2;
  GhMlsGroup *bob_dm2 = join(bob, ALICE);
  GhConversation *bob_dm2_conv = gh_conversation_store_lookup(
    bob->model, gh_mls_group_get_room_id(bob_dm2));
  g_assert_nonnull(bob_dm2_conv);
  g_assert_true(gh_conversation_get_is_direct(bob_dm2_conv));
  g_assert_false(gh_conversation_get_is_request(bob_dm2_conv));

  /* ---- Group co-members as invitable contacts (nostrc-y4wm) ------------- */
  g_auto(GStrv) alice_contacts = gh_mls_contacts_dup(alice->model);
  g_assert_true(strv_has((const gchar *const *)alice_contacts, hex[BOB]));
  g_assert_false(strv_has((const gchar *const *)alice_contacts, hex[ALICE]));

  /* ---- Protocol badge strings ------------------------------------------- */
  const gchar *marmot_badge = gh_privacy_summary_kind(GH_PRIVACY_BACKEND_MLS, TRUE);
  g_assert_cmpstr(marmot_badge, ==, "Marmot private message");
  const gchar *mls_group_badge = gh_privacy_summary_kind(GH_PRIVACY_BACKEND_MLS, FALSE);
  g_assert_cmpstr(mls_group_badge, ==, "Encrypted group");
  const gchar *nip17_badge = gh_privacy_summary_kind(GH_PRIVACY_BACKEND_NIP17, TRUE);
  g_assert_cmpstr(nip17_badge, ==, "Private conversation");
  const gchar *nip29_badge = gh_privacy_summary_kind(GH_PRIVACY_BACKEND_NIP29, FALSE);
  g_assert_cmpstr(nip29_badge, ==, "Relay group, not end-to-end encrypted");

  /* ---- NIP-17 fallback note (L1, review w26-wn-dms) ----------------------
   * When Marmot DM creation fails, the dialog shows honest copy. */
  {
    g_autofree gchar *note_named = gh_mls_fallback_note("Alice");
    g_assert_nonnull(note_named);
    g_assert_nonnull(strstr(note_named, "NIP-17"));
    g_assert_nonnull(strstr(note_named, "Alice"));

    g_autofree gchar *note_anon = gh_mls_fallback_note(NULL);
    g_assert_nonnull(note_anon);
    g_assert_nonnull(strstr(note_anon, "NIP-17"));
  }

  /* ---- Default DM protocol setting -------------------------------------- */
  g_autoptr(GSettingsBackend) settings_backend = g_memory_settings_backend_new();
  g_autoptr(GSettings) dm_settings =
    g_settings_new_with_backend("org.nostr.Groundhog", settings_backend);
  g_autofree gchar *default_val = g_settings_get_string(dm_settings, "default-dm-protocol");
  g_assert_cmpstr(default_val, !=, "nip17");
  g_settings_set_string(dm_settings, "default-dm-protocol", "nip17");
  g_autofree gchar *nip17_val = g_settings_get_string(dm_settings, "default-dm-protocol");
  g_assert_cmpstr(nip17_val, ==, "nip17");
  g_settings_set_string(dm_settings, "default-dm-protocol", "marmot");
  g_autofree gchar *marmot_val = g_settings_get_string(dm_settings, "default-dm-protocol");
  g_assert_cmpstr(marmot_val, ==, "marmot");

  /* ---- Recipient input parsing for the picker npub entry ---------------- */
  g_autoptr(GhRecipientInput) valid = gh_recipient_input_parse(npub[BOB]);
  g_assert_cmpint(valid->kind, ==, GH_RECIPIENT_INPUT_PUBKEY);
  g_assert_cmpstr(valid->pubkey, ==, hex[BOB]);

  g_autofree gchar *nostr_uri = g_strdup_printf("nostr:%s", npub[BOB]);
  g_autoptr(GhRecipientInput) with_prefix = gh_recipient_input_parse(nostr_uri);
  g_assert_cmpint(with_prefix->kind, ==, GH_RECIPIENT_INPUT_PUBKEY);

  /* Raw hex is classified as TEXT by the input parser; the picker's
   * on_add_entry_apply handles it via gh_recipient_is_pubkey() instead. */
  g_assert_true(gh_recipient_is_pubkey(hex[BOB]));
  g_assert_false(gh_recipient_is_pubkey("not-a-key"));
  g_assert_false(gh_recipient_is_pubkey(NULL));

  g_autoptr(GhRecipientInput) secret = gh_recipient_input_parse(
    "nsec1vl029mgpspedva04g90vltkh6fvh240zqtv9k0t9af8935ke9laqsnlfe5");
  g_assert_cmpint(secret->kind, ==, GH_RECIPIENT_INPUT_SECRET);

  g_autoptr(GhRecipientInput) garbage = gh_recipient_input_parse("hello world");
  g_assert_cmpint(garbage->kind, ==, GH_RECIPIENT_INPUT_TEXT);

  g_autoptr(GhRecipientInput) empty_text = gh_recipient_input_parse("");
  g_assert_cmpint(empty_text->kind, ==, GH_RECIPIENT_INPUT_EMPTY);

  g_autoptr(GhRecipientInput) nip05 = gh_recipient_input_parse("bob@example.com");
  g_assert_cmpint(nip05->kind, ==, GH_RECIPIENT_INPUT_NIP05);

  world_down(&w);
}

/* ---- --gui helpers ------------------------------------------------------------------- */

static gboolean
is_mapped(gpointer widget)
{
  return gtk_widget_get_mapped(GTK_WIDGET(widget));
}

static GhWindow *
test_window(void)
{
  GtkWindow *window = GTK_WINDOW(gh_window_new(NULL));
  gtk_window_set_default_size(window, 900, 720);
  gtk_window_present(window);
  spin_until(is_mapped, window, "the window mapped");
  drain();
  return GH_WINDOW(window);
}

static GhNip29Service *
null_nip29(gpointer data)
{
  (void)data;
  return NULL;
}

static GhNip29Service *
the_nip29(gpointer data)
{
  return data;
}

static GhMlsService *
app_service(gpointer data)
{
  return ((App *)data)->service;
}

/* The window glue as gh-app-services.c attaches it. nip29: New Group needs
 * the relay-group service (the dialog's relay part), or NULL. */
/* The next app_window()'s group files (W25), or NULL. */
static GhMlsAttachments *window_files;

static GhWindow *
app_window(App *app, GhNip29Service *nip29)
{
  GhWindow *window = test_window();
  gh_conversation_list_attach(window, app->model, NULL);
  GhGroupUiConfig groups = { .conversations = app->model,
                             .service = nip29 ? the_nip29 : null_nip29,
                             .service_data = nip29 };
  gh_group_ui_attach(window, &groups);
  GhMlsUiConfig mls = {
    .conversations = app->model,
    .accounts = app->accounts,
    .settings = app->settings,
    .service = app_service,
    .service_data = app,
    .account_relays = app->relays,
    .lookup_deadline = 20,
    .files = window_files,
  };
  gh_mls_ui_attach(window, &mls);
  return window;
}

static GhNip29Service *
nip29_up(App *app)
{
  GhNip29ServiceConfig config = {
    .store = app->store,
    .accounts = app->accounts,
    .conversations = app->model,
    .network = G_NETWORK_MONITOR(app->network),
    .settings = app->settings,
  };
  g_autoptr(GError) error = NULL;
  GhNip29Service *service = gh_nip29_service_new(&config, &error);
  g_assert_no_error(error);
  return service;
}

static void
close_window(GhWindow *window)
{
  gtk_window_destroy(GTK_WINDOW(window));
  drain();
}

static AdwDialog *
visible_dialog(GhWindow *window)
{
  return adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
}

/* The window's render node once it has one: a frame under load may still
 * have nothing to snapshot (its first draw after a size change). */
static GskRenderNode *
window_node(GtkWidget *window)
{
  g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new(window);
  GtkSnapshot *snapshot = gtk_snapshot_new();
  gdk_paintable_snapshot(paintable, snapshot, gdk_paintable_get_intrinsic_width(paintable),
                         gdk_paintable_get_intrinsic_height(paintable));
  return gtk_snapshot_free_to_node(snapshot);
}

static gboolean
window_has_node(gpointer window)
{
  g_autoptr(GskRenderNode) node = window_node(window);
  return node != NULL;
}

/* With GROUNDHOG_TEST_SCREENSHOTS=<dir>: the window as
 * <dir>/groundhog-group-contact-panels-<name>.png. */
static void
save_shot(GhWindow *window, const gchar *name)
{
  const gchar *dir = g_getenv("GROUNDHOG_TEST_SCREENSHOTS");
  if (!dir || !*dir)
    return;
  drain();
  gtk_test_widget_wait_for_draw(GTK_WIDGET(window));
  drain();
  spin_until(window_has_node, window, "the window drawn");
  g_autoptr(GskRenderNode) node = window_node(GTK_WIDGET(window));
  g_assert_nonnull(node);
  graphene_rect_t bounds;
  gsk_render_node_get_bounds(node, &bounds);
  GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(window));
  g_autoptr(GdkTexture) texture = gsk_renderer_render_texture(renderer, node, &bounds);
  g_autofree gchar *path = g_strdup_printf("%s/groundhog-group-contact-panels-%s.png", dir, name);
  g_assert_true(gdk_texture_save_to_png(texture, path));
}

/* The sidebar's visible panel (a flow such as New Group), or NULL. */
static AdwNavigationPage *
visible_flow(GhWindow *window)
{
  return gh_sidebar_page_get_panel(gh_window_get_sidebar(window));
}

static gboolean
no_flow(gpointer window)
{
  return visible_flow(window) == NULL;
}

static const gchar *
header_subtitle(GhWindow *window)
{
  return gh_content_page_get_subtitle(gh_window_get_content(window));
}

typedef struct {
  const gchar *(*get)(gpointer);
  gpointer object;
  const gchar *text;
} TextWait;

static gboolean
text_is(gpointer data)
{
  TextWait *wait = data;
  return g_strcmp0(wait->get(wait->object), wait->text) == 0;
}

#define wait_text(get_, object_, text_) \
  G_STMT_START { TextWait tw_ = { (const gchar *(*)(gpointer))(get_), (object_), (text_) }; \
    spin_until(text_is, &tw_, "\"" text_ "\""); } G_STMT_END

typedef struct {
  GhMlsInviteePicker *picker;
  const gchar *pubkey;
  GhMlsInviteeState state;
} PickWait;

static gboolean
pick_is(gpointer data)
{
  PickWait *wait = data;
  return gh_mls_invitee_picker_get_state(wait->picker, wait->pubkey) == wait->state;
}

#define wait_pick(picker_, pubkey_, state_) \
  G_STMT_START { PickWait pw_ = { (picker_), (pubkey_), (state_) }; \
    spin_until(pick_is, &pw_, "the person's KeyPackage check"); } G_STMT_END

static gboolean
nothing_pending(gpointer dialog)
{
  return gh_mls_group_info_dialog_get_pending(dialog) == 0;
}

static const gchar *
last_info_toast(gpointer dialog)
{
  return gh_mls_group_info_dialog_get_last_toast(dialog);
}

/* nostrc-2um6 */
static gboolean
left_for_everyone(gpointer group)
{
  return gh_mls_group_get_end(group) == GH_MLS_GROUP_END_LEFT;
}

static gboolean
toast_says_left(gpointer dialog)
{
  const gchar *t = gh_mls_group_info_dialog_get_last_toast(dialog);
  return t && g_str_has_suffix(t, " left the group");
}

static GtkWidget *
row_for(GtkWidget *widget, GhConversation *conversation)
{
  if (GH_IS_CONVERSATION_ROW(widget) &&
      gh_conversation_row_get_conversation(GH_CONVERSATION_ROW(widget)) == conversation)
    return widget;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = row_for(c, conversation);
    if (found)
      return found;
  }
  return NULL;
}

static GtkWidget *
find_type(GtkWidget *widget, GType type, const gchar *title)
{
  if (G_TYPE_CHECK_INSTANCE_TYPE(widget, type) &&
      (!title || (ADW_IS_PREFERENCES_ROW(widget) &&
                  g_strcmp0(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(widget)),
                            title) == 0)))
    return widget;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = find_type(c, type, title);
    if (found)
      return found;
  }
  return NULL;
}

static gboolean
sent(gpointer data)
{
  MessageWait *wait = data;
  GhMessage *message = find_message(wait->app, wait->room_id, wait->text);
  return message && gh_message_get_status(message) == GH_MESSAGE_STATUS_SENT;
}

static GhConversationView *
view_of(GhWindow *window)
{
  return GH_CONVERSATION_VIEW(gh_content_page_get_view(gh_window_get_content(window)));
}

typedef struct {
  GhConversationView *view;
  gboolean pending;
} PendingWait;

static gboolean
group_removed(gpointer data)
{
  return gh_mls_group_get_end(data) == GH_MLS_GROUP_END_REMOVED;
}

static gboolean
pending_is(gpointer data)
{
  PendingWait *wait = data;
  return gh_conversation_view_get_decrypt_pending(wait->view) == wait->pending;
}

/* ---- --gui: New Group, the invitee, the composer ---------------------------------------- */

static const gchar *
create_reason(gpointer page)
{
  return gh_mls_new_group_page_get_create_reason(page);
}

static gboolean
reason_cleared(gpointer page)
{
  return gh_mls_new_group_page_get_create_reason(page) == NULL;
}

static const gchar *
status_title(gpointer page)
{
  return gh_mls_new_group_page_get_status_title(page);
}

static const gchar *
invites_toast(gpointer page)
{
  return gh_mls_invites_page_get_last_toast(page);
}

typedef struct {
  GhWindow *window;
  guint count;
} InvitationsWait;

static gboolean
invitations_are(gpointer data)
{
  InvitationsWait *wait = data;
  return gh_sidebar_page_get_invitations(gh_window_get_sidebar(wait->window)) == wait->count;
}

typedef struct {
  GtkWidget *widget;
  gint64 from;
} FramePainted;

/* A frame after `from` was drawn (a screenshot needs a render node). */
static gboolean
frame_painted(gpointer data)
{
  FramePainted *wait = data;
  return gdk_frame_clock_get_frame_counter(gtk_widget_get_frame_clock(wait->widget)) >
         wait->from + 1;
}

/* As frame_painted, asking for a frame each time it is polled: a window
 * with nothing changing draws one frame for a queue_draw and stops. */
static gboolean
frame_painted_redraw(gpointer data)
{
  FramePainted *wait = data;
  if (frame_painted(wait))
    return TRUE;
  gtk_widget_queue_draw(wait->widget);
  return FALSE;
}

static gboolean
invitations_entry_hidden(gpointer data)
{
  return !gh_sidebar_page_get_invitations_entry_visible(gh_window_get_sidebar(data));
}

static gboolean
split_collapsed(gpointer split)
{
  return adw_navigation_split_view_get_collapsed(split);
}

/* After win.group-invitations (the sidebar entry's, the main menu's and a
 * new invitation's toast's action): the Group Invitations panel slid over
 * the conversation list, on screen, keyboard in it. */
static GhMlsInvitesPage *
invitations_shown(GhWindow *window)
{
  AdwNavigationPage *panel = gh_sidebar_page_get_panel(gh_window_get_sidebar(window));
  g_assert_true(GH_IS_MLS_INVITES_PAGE(panel));
  g_assert_null(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  AdwNavigationView *navigation = gh_sidebar_page_get_navigation(gh_window_get_sidebar(window));
  AdwNavigationPage *under = adw_navigation_view_get_previous_page(navigation, panel);
  g_assert_cmpstr(adw_navigation_page_get_tag(under), ==, "conversations");
  spin_until(is_mapped, panel, "the Group Invitations panel shown");
  GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(window));
  /* On the list (a row), not on a button nor inside a selectable label. */
  g_assert_true(focus && gtk_widget_is_ancestor(focus, GTK_WIDGET(panel)));
  g_assert_true(GTK_IS_LIST_BOX_ROW(focus));
  return GH_MLS_INVITES_PAGE(panel);
}

static GhMlsInvitesPage *
open_invitations(GhWindow *window)
{
  g_action_group_activate_action(G_ACTION_GROUP(window), "group-invitations", NULL);
  return invitations_shown(window);
}

/* The sidebar's "Group Invitations" entry, pressed. */
static GhMlsInvitesPage *
open_invitations_entry(GhWindow *window)
{
  GtkWidget *entry = GTK_WIDGET(gtk_widget_get_template_child(
    GTK_WIDGET(gh_window_get_sidebar(window)), GH_TYPE_SIDEBAR_PAGE, "invitations_button"));
  g_assert_true(gtk_widget_get_mapped(entry));
  g_signal_emit_by_name(entry, "clicked");
  return invitations_shown(window);
}

/* The mapped button under widget whose action is action, or NULL. */
static GtkWidget *
action_button(GtkWidget *widget, const gchar *action)
{
  if (GTK_IS_BUTTON(widget) && gtk_widget_get_mapped(widget) &&
      g_strcmp0(gtk_actionable_get_action_name(GTK_ACTIONABLE(widget)), action) == 0)
    return widget;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = action_button(c, action);
    if (found)
      return found;
  }
  return NULL;
}

/* Focuses button, as Tab would, and clicks it. */
static void
press_focused(GtkWindow *window, GtkWidget *button)
{
  g_assert_nonnull(button);
  g_assert_true(gtk_widget_grab_focus(button));
  g_assert_true(gtk_window_get_focus(window) == button);
  g_signal_emit_by_name(button, "clicked");
}

static GtkWidget *
invites_empty_row(GhMlsInvitesPage *page)
{
  return GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(page), GH_TYPE_MLS_INVITES_PAGE,
                                                  "empty_row"));
}

/* Whether model (or a section or submenu of it) has an item for action. */
static gboolean
menu_has_action(GMenuModel *model, const gchar *action)
{
  for (gint i = 0; model && i < g_menu_model_get_n_items(model); i++) {
    g_autofree gchar *name = NULL;
    if (g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_ACTION, "s", &name) &&
        g_strcmp0(name, action) == 0)
      return TRUE;
    g_autoptr(GMenuModel) section = g_menu_model_get_item_link(model, i, G_MENU_LINK_SECTION);
    g_autoptr(GMenuModel) submenu = g_menu_model_get_item_link(model, i, G_MENU_LINK_SUBMENU);
    if (menu_has_action(section, action) || menu_has_action(submenu, action))
      return TRUE;
  }
  return FALSE;
}

static gboolean
invitations_entry_shown(gpointer data)
{
  return gh_sidebar_page_get_invitations_entry_visible(gh_window_get_sidebar(data));
}

static gboolean
shows(GhWindow *window, GhConversation *conversation)
{
  return gh_content_page_get_conversation_shown(gh_window_get_content(window)) &&
         gh_conversation_view_get_conversation(view_of(window)) == conversation;
}

typedef struct {
  GhWindow *window;
  GhConversation *conversation;
} ShownWait;

static gboolean
is_shown(gpointer data)
{
  ShownWait *wait = data;
  return shows(wait->window, wait->conversation);
}

static void
test_gui_new_group(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_published(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  accept_contact(alice, CAROL);         /* a contact without a KeyPackage */
  admit_request(alice, STRANGER);       /* never offered, never looked up */
  GhNip29Service *nip29 = nip29_up(alice);
  group_send_stub_reset();
  GhWindow *window = app_window(alice, nip29);

  /* New Group opens on the chooser, both kinds explained. */
  g_assert_true(g_action_group_get_action_enabled(G_ACTION_GROUP(window), "new-group"));
  g_action_group_activate_action(G_ACTION_GROUP(window), "new-group", NULL);
  AdwNavigationPage *dialog = visible_flow(window);
  g_assert_true(GH_IS_NEW_GROUP_PAGE(dialog));
  spin_until(is_mapped, dialog, "New Group shown");
  AdwNavigationView *navigation = ADW_NAVIGATION_VIEW(
    gtk_widget_get_template_child(GTK_WIDGET(dialog), GH_TYPE_NEW_GROUP_PAGE, "navigation"));
  g_assert_cmpstr(adw_navigation_page_get_tag(adw_navigation_view_get_visible_page(navigation)),
                  ==, "type");
  GtkWidget *encrypted = find_type(GTK_WIDGET(dialog), ADW_TYPE_ACTION_ROW, "Encrypted Group");
  g_assert_nonnull(encrypted);
  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(encrypted)), ==,
                  "Only members can read messages. Ask people to use a compatible Marmot "
                  "format; some app versions cannot join.");
  g_assert_nonnull(find_type(GTK_WIDGET(dialog), ADW_TYPE_ACTION_ROW, "Relay Group"));
  /* A sidebar panel: the chooser's first row has the focus; the form's own
   * Back is hidden (the form is a pushed step here). */
  GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(window));
  g_assert_true(focus && (focus == encrypted || gtk_widget_is_ancestor(focus, encrypted)));
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(gtk_widget_get_template_child(
    GTK_WIDGET(dialog), GH_TYPE_NEW_GROUP_PAGE, "form_back_button"))));
  save_shot(window, "new-group-chooser-wide");
  gtk_widget_activate_action(GTK_WIDGET(dialog), "new-group.choose-encrypted", NULL);
  GhMlsNewGroupPage *page = GH_MLS_NEW_GROUP_PAGE(
    gh_new_group_page_get_encrypted_page(GH_NEW_GROUP_PAGE(dialog)));
  g_assert_true(ADW_NAVIGATION_PAGE(page) == adw_navigation_view_get_visible_page(navigation));
  /* Escape: one step back, to the chooser; then on to the encrypted page. */
  spin_until(is_mapped, page, "the encrypted page shown");
  gh_test_press_escape(GTK_WINDOW(window));
  drain();
  g_assert_cmpstr(adw_navigation_page_get_tag(adw_navigation_view_get_visible_page(navigation)),
                  ==, "type");
  g_assert_true(visible_flow(window) == dialog);
  gtk_widget_activate_action(GTK_WIDGET(dialog), "new-group.choose-encrypted", NULL);
  g_assert_true(ADW_NAVIGATION_PAGE(page) == adw_navigation_view_get_visible_page(navigation));
  spin_until(is_mapped, page, "the encrypted page shown");
  focus = gtk_window_get_focus(GTK_WINDOW(window));
  GtkWidget *name_row = GTK_WIDGET(gtk_widget_get_template_child(
    GTK_WIDGET(page), GH_TYPE_MLS_NEW_GROUP_PAGE, "name_row"));
  g_assert_true(focus && (focus == name_row || gtk_widget_is_ancestor(focus, name_row)));
  save_shot(window, "new-group-encrypted-wide");
  g_assert_nonnull(find_type(GTK_WIDGET(page), ADW_TYPE_ACTION_ROW, "Who can see this group"));
  AdwActionRow *compatibility = ADW_ACTION_ROW(
    find_type(GTK_WIDGET(page), ADW_TYPE_ACTION_ROW, "Marmot compatibility"));
  g_assert_nonnull(compatibility);
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(compatibility),
                          "Some intermediate Marmot app versions cannot join either format."));
  g_assert_nonnull(find_type(GTK_WIDGET(page), ADW_TYPE_ACTION_ROW, "Leaving"));
  g_assert_null(gh_mls_new_group_page_get_identity_title(page));   /* approved */

  /* Relays: the account's own write relay to start with; typed ones are
   * checked; the group relay G replaces W. */
  g_auto(GStrv) start = gh_mls_new_group_page_dup_relays(page);
  g_assert_cmpuint(g_strv_length(start), ==, 1);
  g_assert_cmpstr(start[0], ==, w.w.url);
  g_assert_false(gh_mls_new_group_page_add_relay(page, "https://relay.example.com"));
  g_assert_false(gh_mls_new_group_page_add_relay(page, w.w.url));   /* already listed */
  g_assert_true(gh_mls_new_group_page_add_relay(page, w.g.url));
  GtkWidget *w_row = find_type(GTK_WIDGET(page), GH_TYPE_MLS_RELAY_ROW, w.w.url);
  g_assert_nonnull(w_row);
  gtk_widget_activate_action(w_row, "relay.remove", NULL);
  g_auto(GStrv) relays = gh_mls_new_group_page_dup_relays(page);
  g_assert_cmpuint(g_strv_length(relays), ==, 1);
  g_assert_cmpstr(relays[0], ==, w.g.url);

  /* Create waits, saying why, for a name and a person who can join. */
  g_assert_cmpstr(create_reason(page), ==, "Give the group a name.");
  gh_mls_new_group_page_set_name(page, "Book Club");
  g_assert_cmpstr(create_reason(page), ==, "Choose at least one person.");
  GhMlsInviteePicker *picker = gh_mls_new_group_page_get_picker(page);
  g_assert_cmpuint(gh_mls_invitee_picker_get_n_listed(picker), ==, 2);
  g_assert_null(gh_mls_invitee_picker_get_row(picker, hex[STRANGER]));   /* a request */
  g_assert_false(key_package_asked(&w.w, hex[BOB]));   /* nothing looked up yet */
  g_assert_true(gh_mls_invitee_picker_set_selected(picker, hex[CAROL], TRUE));
  wait_pick(picker, hex[CAROL], GH_MLS_INVITEE_NOT_SET_UP);
  AdwActionRow *carol_row = gh_mls_invitee_picker_get_row(picker, hex[CAROL]);
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(carol_row),
                          "Hasn’t set up encrypted groups"));
  g_assert_cmpstr(create_reason(page), ==,
                  "Someone you chose can’t be invited yet. Remove them to continue.");
  gh_mls_invitee_picker_set_selected(picker, hex[CAROL], FALSE);
  gh_mls_invitee_picker_set_selected(picker, hex[BOB], TRUE);
  wait_pick(picker, hex[BOB], GROUNDHOG_READY);
  /* The KeyPackage on Bob's write relay only, never on the discovery
   * relay (nostrc-0bdg). */
  g_assert_true(key_package_asked(&w.w, hex[BOB]));
  g_assert_false(key_package_asked(&w.e, hex[BOB]));
  g_assert_false(key_package_asked(&w.e, hex[STRANGER]));   /* PD-8 */
  g_assert_false(key_package_asked(&w.w, hex[STRANGER]));
  g_assert_null(create_reason(page));

  /* An account generation change ends the lookups: whoever is chosen is
   * checked again, and Create waits for this device's approval again. */
  g_settings_set_string(alice->settings, "current-npub", npub[STRANGER]);
  gh_account_controller_refresh(alice->accounts);
  g_settings_set_string(alice->settings, "current-npub", npub[ALICE]);
  gh_account_controller_refresh(alice->accounts);
  spin_until(accounts_active, alice->accounts, "Alice active again");
  wait_pick(picker, hex[BOB], GROUNDHOG_READY);
  spin_until(reason_cleared, page, "Create ready again");

  /* Bob's app speaks the newer format: nothing to say about the format
   * (built without the adopted producer, only the older one: said). */
  g_assert_cmpint(gh_mls_new_group_page_get_format_notice(page) != NULL, ==,
                  !GH_MLS_ADOPTED_KEY_PACKAGES);
  g_assert_false(gh_mls_new_group_page_get_format_choice(page));

  /* Review finding 5: the media notice is format-aware.  With no Blossom
   * servers, an adopted group shows "No file servers configured" (not
   * hidden).  With servers it shows the server list.  A legacy group
   * hides the media row entirely. */
#if GH_MLS_ADOPTED_KEY_PACKAGES
  /* No servers: an adopted group shows the no-servers notice. */
  g_assert_cmpstr(gh_mls_new_group_page_get_media_notice(page), ==,
                  "No file servers configured");

  /* With a server: the notice names it. */
  const gchar *srv[] = { "https://blossom.example.com", NULL };
  g_settings_set_strv(alice->settings, "blossom-servers", srv);
  /* Re-sync by toggling selection (sync_create → sync_media_notice). */
  gh_mls_invitee_picker_set_selected(picker, hex[BOB], FALSE);
  gh_mls_invitee_picker_set_selected(picker, hex[BOB], TRUE);
  wait_pick(picker, hex[BOB], GROUNDHOG_READY);
  spin_until(reason_cleared, page, "ready with servers set");
  g_assert_nonnull(gh_mls_new_group_page_get_media_notice(page));
  g_assert_nonnull(strstr(gh_mls_new_group_page_get_media_notice(page), "1 server"));
  g_settings_reset(alice->settings, "blossom-servers");
  gh_mls_invitee_picker_set_selected(picker, hex[BOB], FALSE);
  gh_mls_invitee_picker_set_selected(picker, hex[BOB], TRUE);
  wait_pick(picker, hex[BOB], GROUNDHOG_READY);
  spin_until(reason_cleared, page, "ready after server reset");
#else
  /* A configured account server must not be presented as a group policy
   * when the selected invitees require a legacy group. */
  const gchar *srv[] = { "https://blossom.example.com", NULL };
  g_settings_set_strv(alice->settings, "blossom-servers", srv);
  gh_mls_invitee_picker_set_selected(picker, hex[BOB], FALSE);
  gh_mls_invitee_picker_set_selected(picker, hex[BOB], TRUE);
  wait_pick(picker, hex[BOB], GROUNDHOG_READY);
  spin_until(reason_cleared, page, "ready with legacy server setting");
  g_assert_null(gh_mls_new_group_page_get_media_notice(page));
  g_settings_reset(alice->settings, "blossom-servers");
#endif

  gtk_widget_activate_action(GTK_WIDGET(page), "mls-new.create", NULL);
  wait_text(status_title, page, "Group Created");
  GhMlsGroup *ga = gh_mls_new_group_page_get_group(page);
  g_assert_nonnull(ga);
  g_assert_cmpstr(gh_mls_group_get_name(ga), ==, "Book Club");
  g_assert_cmpint(gh_mls_group_get_adopted(ga), ==, GH_MLS_ADOPTED_KEY_PACKAGES);   /* lf62 */
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  GhConversation *conversation = gh_conversation_store_lookup(alice->model, room);
  g_assert_nonnull(conversation);

  /* Open Group closes the dialog and shows the conversation. */
  gtk_widget_activate_action(GTK_WIDGET(page), "mls-new.open", NULL);
  spin_until(no_flow, window, "New Group closed");
  ShownWait shown = { window, conversation };
  spin_until(is_shown, &shown, "the new group shown");
  g_assert_cmpstr(header_subtitle(window), ==, "Encrypted group · 2 members");

  /* The row: a lock, "Encrypted group". */
  drain();
  GtkWidget *row = row_for(GTK_WIDGET(window), conversation);
  g_assert_nonnull(row);
  GtkWidget *kind = GTK_WIDGET(gtk_widget_get_template_child(row, GH_TYPE_CONVERSATION_ROW,
                                                             "kind_icon"));
  g_assert_true(gtk_widget_get_visible(kind));
  g_assert_cmpstr(gtk_image_get_icon_name(GTK_IMAGE(kind)), ==, "channel-secure-symbolic");
  g_assert_cmpstr(gtk_widget_get_tooltip_text(kind), ==, "Encrypted group");
  g_autoptr(GtkATContext) at = gtk_accessible_get_at_context(GTK_ACCESSIBLE(kind));
  if (at)
    gtk_test_accessible_assert_property(GTK_ACCESSIBLE(kind), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                        "Encrypted group");
  g_assert_nonnull(strstr(gh_conversation_row_get_summary(GH_CONVERSATION_ROW(row)),
                          "Book Club. Encrypted group"));

  /* The composer: the encrypted group's delegate sends; the status is the
   * service's, honest. */
  wait_live(ga);
  gpointer data = NULL;
  const GhSendUiDelegate *delegate = group_send_stub_delegate_for(conversation, &data);
  g_assert_nonnull(delegate);
  g_autofree gchar *reason = delegate->reason(conversation, data);
  g_assert_null(reason);
  g_autoptr(GError) error = NULL;
  g_assert_true(delegate->send(conversation, "Hello, Book Club", data, &error));
  g_assert_no_error(error);
  GhMessage *mine = find_message(alice, room, "Hello, Book Club");
  g_assert_nonnull(mine);
  g_assert_true(gh_message_is_self(mine));
  MessageWait delivered = { alice, room, "Hello, Book Club" };
  spin_until(sent, &delivered, "the message sent");

  /* Bob: the invitation waits in "Group Invitations"; nothing is joined. */
  spin_until(has_invite, bob, "Bob's invitation");
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(bob->service)), ==, 0);
  GhWindow *bob_window = app_window(bob, NULL);
  InvitationsWait one = { bob_window, 1 };
  spin_until(invitations_are, &one, "Bob's invitations entry");
  GhMlsInvitesPage *invites = open_invitations(bob_window);
  g_assert_cmpuint(gh_mls_invites_page_get_n_invites(invites), ==, 1);
  g_autofree gchar *wrapper = the_invite(bob, ALICE);
  const gchar *title = NULL, *subtitle = NULL;
  g_assert_true(gh_mls_invites_page_describe(invites, wrapper, &title, &subtitle));
  g_assert_cmpstr(title, ==, "Book Club");
  g_assert_nonnull(strstr(subtitle, "not in your contacts"));   /* Bob never wrote Alice */
  g_assert_nonnull(strstr(subtitle, "2 members"));
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(bob->service)), ==, 0);
  /* Accept, with the keyboard on the Accept button the row takes with it
   * (the libadwaita 1.5 focus crash's path): the panels close and the group
   * opens, with the window's "You joined". */
  press_focused(GTK_WINDOW(bob_window), action_button(GTK_WIDGET(invites), "mls-invites.accept"));
  spin_until(no_flow, bob_window, "the invitations closed");
  InvitationsWait zero = { bob_window, 0 };
  spin_until(invitations_are, &zero, "Bob's invitations entry gone");
  /* The entry goes when nothing waits. */
  spin_until(invitations_entry_hidden, bob_window, "Bob's invitations entry hidden");
  GhMlsGroup *gb = gh_mls_service_lookup(bob->service, room);
  g_assert_nonnull(gb);
  /* An adopted Welcome (as shipped), listed and accepted in the
   * invitations panel. */
  g_assert_cmpint(gh_mls_group_get_adopted(gb), ==, GH_MLS_ADOPTED_KEY_PACKAGES);
  wait_live(gb);
  wait_message(bob, room, "Hello, Book Club");

  /* Bob is shown the group; Bob answers, Alice reads it. */
  GhConversation *bob_conversation = gh_conversation_store_lookup(bob->model, room);
  ShownWait bob_shown = { bob_window, bob_conversation };
  spin_until(is_shown, &bob_shown, "Bob's group shown");
  g_assert_cmpstr(header_subtitle(bob_window), ==, "Encrypted group · 2 members");
  send_text(bob, gb, "Hi Alice");
  wait_message(alice, room, "Hi Alice");

  close_window(bob_window);
  close_window(window);
  gh_test_release(nip29);
  world_down(&w);
}

/* ---- --gui: an invitation that could not be opened (relogin-signer-groups) ---------------- */

/* Opt-in evidence: GROUNDHOG_SCREENSHOT_PREFIX=/tmp/x saves /tmp/x-<name>.png. */
static void
save_root_png(GtkWidget *widget, const gchar *name)
{
  const gchar *prefix = g_getenv("GROUNDHOG_SCREENSHOT_PREFIX");
  if (!prefix || !*prefix)
    return;
  GtkWidget *root = GTK_WIDGET(gtk_widget_get_root(widget));
  g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new(root);
  GtkSnapshot *snapshot = gtk_snapshot_new();
  gdk_paintable_snapshot(paintable, snapshot, gdk_paintable_get_intrinsic_width(paintable),
                         gdk_paintable_get_intrinsic_height(paintable));
  g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(snapshot);
  g_assert_nonnull(node);
  graphene_rect_t bounds;
  gsk_render_node_get_bounds(node, &bounds);
  GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(root));
  g_autoptr(GdkTexture) texture = gsk_renderer_render_texture(renderer, node, &bounds);
  g_autofree gchar *path = g_strdup_printf("%s-%s.png", prefix, name);
  g_assert_true(gdk_texture_save_to_png(texture, path));
  g_test_message("saved %s", path);
}

/* save_root_png() of window once a frame with its current state is drawn. */
static void
painted_png(GhWindow *window, const gchar *name)
{
  drain();
  FramePainted painted = { GTK_WIDGET(window), 0 };
  painted.from = gdk_frame_clock_get_frame_counter(
    gtk_widget_get_frame_clock(GTK_WIDGET(window)));
  gtk_widget_queue_draw(GTK_WIDGET(window));
  spin_until(frame_painted_redraw, &painted, "the window painted");
  save_root_png(GTK_WIDGET(window), name);
}

static gboolean
inbox_withheld(gpointer data)
{
  WireRelay *relay = data;
  return relay->withheld && g_hash_table_size(relay->withheld) > 0;
}

typedef struct { App *app; const gchar *wrap_id; } WelcomeWait;

static gboolean
welcome_processed(gpointer data)
{
  WelcomeWait *wait = data;
  return wait->app->store && processed_welcome(wait->app, wait->wrap_id, NULL, NULL);
}

static void
wipe_dir(const gchar *path)
{
  GDir *dir = g_dir_open(path, 0, NULL);
  if (dir) {
    const gchar *name;
    while ((name = g_dir_read_name(dir))) {
      g_autofree gchar *child = g_build_filename(path, name, NULL);
      if (g_file_test(child, G_FILE_TEST_IS_DIR) && !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
        wipe_dir(child);
      g_remove(child);
    }
    g_dir_close(dir);
  }
}

/* The owner's case: an invitation made for a KeyPackage whose private key
 * the account's store on this device does not have (here: the store was
 * started afresh after the KeyPackage was published; in the field also a
 * KeyPackage from another device or app). libmarmot refuses the Welcome
 * ("matching key not found"); it used to be only a debug line and the
 * invitations stayed "No Invitations". Now it is listed as one that could
 * not be opened, with its cause and Dismiss, and counted in the sidebar. */
static void
test_gui_refused_invitation(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_published(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);

  /* Alice invites Bob with his current KeyPackage; the Welcome waits on
   * Bob's inbox relay. */
  w.x.withhold_new = TRUE;
  GhMlsGroup *group = create_group(alice, "Book Club", (const guint[]){ BOB }, 1);
  g_assert_nonnull(group);
  spin_until(inbox_withheld, &w.x, "Alice's Welcome on Bob's inbox relay");
  w.x.withhold_new = FALSE;
  g_autoptr(GPtrArray) wraps = g_ptr_array_new_with_free_func(g_free);
  GHashTableIter iter;
  gpointer id;
  g_hash_table_iter_init(&iter, w.x.withheld);
  while (g_hash_table_iter_next(&iter, &id, NULL))
    g_ptr_array_add(wraps, g_strdup(id));
  g_assert_cmpuint(wraps->len, ==, 1);

  /* Bob's store on this device starts empty: the KeyPackage's key is gone. */
  app_store_down(bob);
  wipe_dir(bob->data_dir);
  app_store_up(bob);
  spin_until(key_package_published, bob, "Bob's new KeyPackage");

  wire_relay_release(&w.x, g_ptr_array_index(wraps, 0));
  WelcomeWait processed = { bob, g_ptr_array_index(wraps, 0) };
  spin_until(welcome_processed, &processed, "Bob's verdict on the Welcome");

  GhWindow *first_window = app_window(bob, NULL);
  GhMlsInvitesPage *page = open_invitations(first_window);
  painted_png(first_window, "refused-invitation");
  g_assert_cmpuint(gh_mls_invites_page_get_n_invites(page), ==, 0);
  g_assert_cmpuint(gh_mls_invites_page_get_n_refused(page), ==, 1);
  const gchar *title = NULL, *subtitle = NULL;
  g_assert_true(gh_mls_invites_page_describe(page, g_ptr_array_index(wraps, 0),
                                             &title, &subtitle));
  g_assert_nonnull(strstr(title, "Invitation from "));
  g_assert_cmpstr(subtitle, ==,
                  "Made for a key this device doesn\u2019t have. Ask them to invite you again.");

  /* This device's own invitation key is published: a new invitation works. */
  g_assert_cmpstr(gh_mls_invites_page_get_key_status(page), ==,
                  "Published. Invitations made for this device can be opened here.");

  /* Dismiss, from the keyboard: the list updates in place. */
  press_focused(GTK_WINDOW(first_window), action_button(GTK_WIDGET(page), "mls-invites.dismiss"));
  g_assert_cmpuint(gh_mls_invites_page_get_n_refused(page), ==, 0);
  g_assert_true(visible_flow(first_window) == ADW_NAVIGATION_PAGE(page));
  close_window(first_window);

  /* mls-groups-bunker, the owner's store: the refusal came before refusals
   * were listed (only libmarmot's failed record is left, its wrap seen).
   * The next start lists it, sender unknown, and the window counts it in
   * the Group Invitations entry. */
  forget_refusal_list(bob);
  app_restart(bob);
  spin_until(key_package_published, bob, "Bob's KeyPackage after the restart");
  GhWindow *bob_window = app_window(bob, NULL);
  spin_until(invitations_entry_shown, bob_window, "Bob's Group Invitations entry");
  InvitationsWait one = { bob_window, 1 };
  spin_until(invitations_are, &one, "the earlier refusal counted");
  FramePainted painted = { GTK_WIDGET(bob_window), 0 };
  painted.from = gdk_frame_clock_get_frame_counter(
    gtk_widget_get_frame_clock(GTK_WIDGET(bob_window)));
  gtk_widget_queue_draw(GTK_WIDGET(bob_window));
  spin_until(frame_painted, &painted, "the window painted with the entry");
  save_root_png(GTK_WIDGET(bob_window), "sidebar-entry");
  /* The search entry is always part of the sidebar. */
  g_assert_true(gtk_widget_get_mapped(GTK_WIDGET(gtk_widget_get_template_child(
    GTK_WIDGET(gh_window_get_sidebar(bob_window)), GH_TYPE_SIDEBAR_PAGE, "search_entry"))));
  GhMlsInvitesPage *again = open_invitations_entry(bob_window);
  painted_png(bob_window, "earlier-refusal");
  g_assert_cmpuint(gh_mls_invites_page_get_n_invites(again), ==, 0);
  g_assert_cmpuint(gh_mls_invites_page_get_n_refused(again), ==, 1);
  g_assert_true(gh_mls_invites_page_describe(again, g_ptr_array_index(wraps, 0), &title,
                                             &subtitle));
  g_assert_cmpstr(title, ==, "An earlier invitation (sender not recorded)");
  g_assert_cmpstr(subtitle, ==,
                  "Made for a key this device doesn\u2019t have. Ask them to invite you again.");
  g_assert_cmpstr(gh_mls_invites_page_get_key_status(again), ==,
                  "Published. Invitations made for this device can be opened here.");
  gtk_widget_activate_action(GTK_WIDGET(again), "mls-invites.dismiss", "s",
                             (const gchar *)g_ptr_array_index(wraps, 0));
  /* The last one dismissed: the entry goes at once, while the panel stays,
   * with its empty state (no group on this device changes nothing). */
  g_assert_cmpuint(gh_sidebar_page_get_invitations(gh_window_get_sidebar(bob_window)), ==, 0);
  g_assert_false(
    gh_sidebar_page_get_invitations_entry_visible(gh_window_get_sidebar(bob_window)));
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(bob->service)), ==, 0);
  g_assert_true(visible_flow(bob_window) == ADW_NAVIGATION_PAGE(again));
  g_assert_true(gtk_widget_get_visible(invites_empty_row(again)));
  painted_png(bob_window, "panel-empty");
  /* Escape returns to the list, without the entry. */
  gh_test_press_escape(GTK_WINDOW(bob_window));
  spin_until(no_flow, bob_window, "the list again");
  g_assert_false(
    gh_sidebar_page_get_invitations_entry_visible(gh_window_get_sidebar(bob_window)));
  painted.from = gdk_frame_clock_get_frame_counter(
    gtk_widget_get_frame_clock(GTK_WIDGET(bob_window)));
  gtk_widget_queue_draw(GTK_WIDGET(bob_window));
  spin_until(frame_painted_redraw, &painted, "the window painted without the entry");
  save_root_png(GTK_WIDGET(bob_window), "sidebar-no-entry");
  /* No main-menu item: the panel opens from the sidebar entry (only while
   * invitations wait) and from a new invitation's toast, through the same
   * window action. */
  g_assert_true(g_action_group_get_action_enabled(G_ACTION_GROUP(bob_window),
                                                  "group-invitations"));
  g_assert_false(menu_has_action(gtk_menu_button_get_menu_model(GTK_MENU_BUTTON(
    gtk_widget_get_template_child(GTK_WIDGET(gh_window_get_sidebar(bob_window)),
                                  GH_TYPE_SIDEBAR_PAGE, "primary_button"))),
    "win.group-invitations"));
  GhMlsInvitesPage *from_action = open_invitations(bob_window);
  g_assert_cmpstr(gh_mls_invites_page_get_key_status(from_action), ==,
                  "Published. Invitations made for this device can be opened here.");
  g_assert_true(gtk_widget_get_visible(invites_empty_row(from_action)));
  /* The header's back button returns too. */
  gh_sidebar_panel_leave(ADW_NAVIGATION_PAGE(from_action));
  spin_until(no_flow, bob_window, "the list again");
  close_window(bob_window);
  world_down(&w);
}

/* ---- --gui: Remove old invitation keys (nostrc-ab9kn) ----------------------------------- */

/* Bob's KeyPackage events (kind 30443 or 443) stored on relay; with
 * `current`, only those this device's service last published. */
static guint
bob_key_packages_on(WireRelay *relay, App *bob, gboolean current)
{
  guint n = 0;
  for (guint i = 0; i < relay->stored->len; i++) {
    WireStored *stored = g_ptr_array_index(relay->stored, i);
    gint kind = nostr_event_get_kind(stored->event);
    if ((kind != 30443 && kind != 443) ||
        g_strcmp0(nostr_event_get_pubkey(stored->event), hex[bob->key]) != 0)
      continue;
    gboolean mine = FALSE;
    for (guint f = 0; f < GH_MLS_KEY_PACKAGE_N_FORMATS; f++)
      mine |= g_strcmp0(gh_mls_service_get_key_package_id_for_format(bob->service, f),
                        stored->id) == 0;
    n += !current || mine;
  }
  return n;
}

typedef struct { WireRelay *relay; App *bob; } OnlyCurrentWait;

static gboolean
only_current_left(gpointer data)
{
  OnlyCurrentWait *wait = data;
  return bob_key_packages_on(wait->relay, wait->bob, FALSE) ==
         bob_key_packages_on(wait->relay, wait->bob, TRUE);
}

static gboolean
old_keys_idle(gpointer page)
{
  return !gh_mls_invites_page_get_old_keys_busy(page);
}

static gboolean
alert_shown(gpointer window)
{
  AdwDialog *dialog = visible_dialog(window);
  return ADW_IS_ALERT_DIALOG(dialog) && gh_test_dialog_shown(dialog);
}

static const gchar *
old_keys_status(gpointer page)
{
  return gh_mls_invites_page_get_old_keys_status(page);
}

/* The owner's case: KeyPackages the account published from a store this
 * device no longer has (the 10-08 Groundhog keys), from another app (White
 * Noise) and in the older kind 443 stay on the write relay, and inviters
 * may pick one whose private key is gone here. Group Invitations' "Old
 * Invitation Keys" lists them through the account's relays, the dialog
 * marks this device's as kept and says other apps must publish anew, and
 * Remove publishes ONE kind-5 signed by the account that the relay applies
 * to every other one, never to this device's current KeyPackages. */
static void
test_gui_remove_old_keys(void)
{
  World w;
  const guint keys[] = { BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *bob = &w.apps[BOB];
  w.w.nip09 = TRUE;
  wait_published(&w, keys, G_N_ELEMENTS(keys));
  /* A fresh store here: new slots, the first store's KeyPackages orphaned. */
  app_store_down(bob);
  wipe_dir(bob->data_dir);
  app_store_up(bob);
  spin_until(key_package_published, bob, "Bob's new KeyPackage");
#if GH_MLS_ADOPTED_KEY_PACKAGES
  inject_adopted_key_package(&w, BOB);   /* White Noise's */
#endif
  g_free(inject_legacy_key_package(&w, BOB));   /* an older app's */
  NostrTags *tags = nostr_tags_new(0);
  g_autofree gchar *kind_443 = sign_event(BOB, 443, real_now() - 86400 * 30, "00", tags);
  wire_relay_inject(&w.w, kind_443);
  guint current = bob_key_packages_on(&w.w, bob, TRUE);
  guint others = bob_key_packages_on(&w.w, bob, FALSE) - current;
  g_assert_cmpuint(current, ==, GH_MLS_ADOPTED_KEY_PACKAGES ? 2 : 1);
  g_assert_cmpuint(others, >=, current + 2 + GH_MLS_ADOPTED_KEY_PACKAGES);

  GhWindow *window = app_window(bob, NULL);
  GhMlsInvitesPage *page = open_invitations(window);
  GtkWidget *review = action_button(GTK_WIDGET(page), "mls-invites.review-keys");
  g_assert_nonnull(review);
  g_assert_true(gtk_widget_get_sensitive(review));
  painted_png(window, "old-keys-row");

  /* Review: the account's relays are asked, then the confirmation. */
  guint deletions_before = w.w.deleted;
  press_focused(GTK_WINDOW(window), review);
  g_assert_true(gh_mls_invites_page_get_old_keys_busy(page));
  g_assert_false(gtk_widget_get_sensitive(review));
  spin_until(alert_shown, window, "the old keys' confirmation");
  AdwAlertDialog *alert = ADW_ALERT_DIALOG(visible_dialog(window));
  g_autofree gchar *heading = g_strdup_printf("Remove %u Old Invitation Keys?", others);
  g_assert_cmpstr(adw_alert_dialog_get_heading(alert), ==, heading);
  const gchar *body = adw_alert_dialog_get_body(alert);
  g_assert_nonnull(strstr(body, "such as White Noise, stop working for new invitations until "
                                "those apps publish new ones"));
  g_assert_cmpstr(adw_alert_dialog_get_default_response(alert), ==, "cancel");
  g_assert_cmpint(adw_alert_dialog_get_response_appearance(alert, "remove"), ==,
                  ADW_RESPONSE_DESTRUCTIVE);
  /* Every key listed, this device's first and marked kept. */
  GtkWidget *extra = adw_alert_dialog_get_extra_child(alert);
  GtkWidget *list = find_type(extra, GTK_TYPE_LIST_BOX, NULL);
  guint rows = 0, kept = 0;
  for (GtkWidget *c = gtk_widget_get_first_child(list); c; c = gtk_widget_get_next_sibling(c)) {
    if (!ADW_IS_ACTION_ROW(c))
      continue;
    const gchar *title = adw_preferences_row_get_title(ADW_PREFERENCES_ROW(c));
    if (g_str_equal(title, "This device (kept)")) {
      g_assert_cmpuint(rows, ==, kept);   /* listed first */
      kept++;
    }
    rows++;
  }
  g_assert_cmpuint(kept, ==, current);
  g_assert_cmpuint(rows, ==, current + others);
  g_assert_nonnull(find_type(extra, ADW_TYPE_ACTION_ROW, "Older-format key"));
  g_assert_nonnull(find_type(extra, ADW_TYPE_ACTION_ROW, "Another device, app or install"));
  g_assert_cmpuint(w.w.deleted, ==, deletions_before);   /* nothing before Remove */
  painted_png(window, "old-keys-confirm");

  /* Cancel changes nothing. */
  g_signal_emit_by_name(alert, "response", "cancel");
  adw_dialog_force_close(ADW_DIALOG(alert));
  drain();
  g_assert_cmpuint(bob_key_packages_on(&w.w, bob, FALSE), ==, current + others);
  g_autofree gchar *found = g_strdup_printf("%u other keys are on your relays.", others);
  g_assert_cmpstr(gh_mls_invites_page_get_old_keys_status(page), ==, found);

  /* Remove: one kind 5 by the account, every other key named. */
  press_focused(GTK_WINDOW(window), review);
  spin_until(alert_shown, window, "the confirmation again");
  alert = ADW_ALERT_DIALOG(visible_dialog(window));
  g_signal_emit_by_name(alert, "response", "remove");
  adw_dialog_force_close(ADW_DIALOG(alert));
  OnlyCurrentWait gone = { &w.w, bob };
  spin_until(only_current_left, &gone, "the relay applied the deletion");
  g_assert_cmpuint(w.w.deleted - deletions_before, ==, others);
  g_autoptr(GPtrArray) requests = published(&w.w, 5);
  g_assert_cmpuint(requests->len, ==, 1);
  NostrEvent *request = g_ptr_array_index(requests, 0);
  g_assert_cmpstr(nostr_event_get_pubkey(request), ==, hex[BOB]);
  g_assert_cmpint(nostr_event_validate(request, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  guint e_tags = 0;
  NostrTags *request_tags = nostr_event_get_tags(request);
  for (size_t t = 0; request_tags && t < nostr_tags_size(request_tags); t++)
    e_tags += g_strcmp0(nostr_tag_get(nostr_tags_get(request_tags, t), 0), "e") == 0;
  g_assert_cmpuint(e_tags, ==, others);
  g_assert_cmpuint(bob_key_packages_on(&w.w, bob, TRUE), ==, current);
  g_assert_true(key_package_published(bob));
  spin_until(old_keys_idle, page, "the panel looked again");
  wait_text(old_keys_status, page, "No other keys are on your relays.");
  g_autofree gchar *toast = g_strdup_printf("Asked your relays to remove %u old invitation keys",
                                            others);
  g_assert_cmpstr(gh_mls_invites_page_get_last_toast(page), ==, toast);
  painted_png(window, "old-keys-removed");

  /* Looking again finds nothing to remove: a toast, no dialog. */
  press_focused(GTK_WINDOW(window), review);
  spin_until(old_keys_idle, page, "the second look");
  g_assert_null(visible_dialog(window));
  g_assert_cmpstr(gh_mls_invites_page_get_last_toast(page), ==,
                  "No old invitation keys on your relays");
  close_window(window);
  world_down(&w);
}

/* ---- --gui: the Group Invitations panel ------------------------------------------------- */

/* The panel slides over the conversation list from the sidebar entry and
 * from win.group-invitations (the main menu's item and a new invitation's
 * toast), leaves the open conversation as it is, returns on Escape, works
 * collapsed, and a Decline updates it in place: the entry goes, the panel
 * stays with its empty state. PNGs with GROUNDHOG_SCREENSHOT_PREFIX. */
static void
test_gui_invites_panel(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_published(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  const gchar *peers[] = { hex[ALICE], NULL };
  g_autoptr(GError) error = NULL;
  GhConversation *dm = gh_conversation_store_open_room(bob->model, peers, &error);
  g_assert_no_error(error);
  GhWindow *window = app_window(bob, NULL);
  AdwNavigationSplitView *split = gh_window_get_split(window);
  g_assert_true(gh_window_open_item(window, dm));
  ShownWait shown = { window, dm };
  spin_until(is_shown, &shown, "Bob's conversation with Alice shown");

  create_group(alice, "Garden", (const guint[]){ BOB }, 1);
  spin_until(has_invite, bob, "Bob's invitation");
  InvitationsWait one = { window, 1 };
  spin_until(invitations_are, &one, "Bob's invitations entry");
  g_autofree gchar *wrapper = the_invite(bob, ALICE);

  /* The sidebar entry: the panel, the conversation still shown beside it. */
  GhMlsInvitesPage *page = open_invitations_entry(window);
  g_assert_true(shows(window, dm));
  g_assert_cmpuint(gh_mls_invites_page_get_n_invites(page), ==, 1);
  const gchar *title = NULL, *subtitle = NULL;
  g_assert_true(gh_mls_invites_page_describe(page, wrapper, &title, &subtitle));
  g_assert_cmpstr(title, ==, "Garden");
  g_assert_false(gtk_widget_get_visible(invites_empty_row(page)));
  painted_png(window, "panel-wide");
  /* Opening it again keeps the one panel. */
  g_assert_true(open_invitations(window) == page);
  gh_test_press_escape(GTK_WINDOW(window));
  spin_until(no_flow, window, "the list again");
  g_assert_true(shows(window, dm));

  /* Collapsed, from the open conversation: win.group-invitations (the main
   * menu's item, a new invitation's toast) brings the sidebar back. */
  gtk_window_set_default_size(GTK_WINDOW(window), 360, 600);
  spin_until(split_collapsed, split, "the collapsed layout");
  /* The breakpoint's own frame (it moves a lost focus) before acting. */
  FramePainted resized = { GTK_WIDGET(window), 0 };
  resized.from = gdk_frame_clock_get_frame_counter(
    gtk_widget_get_frame_clock(GTK_WIDGET(window)));
  spin_until(frame_painted_redraw, &resized, "the collapsed layout drawn");
  g_assert_true(gh_window_open_item(window, dm));
  drain();
  g_assert_true(adw_navigation_split_view_get_show_content(split));
  page = open_invitations(window);
  g_assert_false(adw_navigation_split_view_get_show_content(split));
  painted_png(window, "panel-narrow");

  /* Decline, from the keyboard: nothing joined, the list updates in place,
   * the entry goes, the panel stays with its empty state. */
  press_focused(GTK_WINDOW(window), action_button(GTK_WIDGET(page), "mls-invites.decline"));
  wait_text(invites_toast, page, "Invitation declined");
  g_assert_cmpuint(gh_mls_invites_page_get_n_invites(page), ==, 0);
  g_assert_true(gtk_widget_get_visible(invites_empty_row(page)));
  g_assert_true(visible_flow(window) == ADW_NAVIGATION_PAGE(page));
  spin_until(invitations_entry_hidden, window, "Bob's invitations entry hidden");
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(bob->service)), ==, 0);
  painted_png(window, "panel-declined-narrow");
  gh_test_press_escape(GTK_WINDOW(window));
  spin_until(no_flow, window, "the list again");

  close_window(window);
  world_down(&w);
}

/* ---- --gui: the build's own flag --------------------------------------------------------- */

/* A widget of type titled title that is on screen, or NULL. */
static GtkWidget *
find_mapped(GtkWidget *widget, GType type, const gchar *title)
{
  if (!gtk_widget_get_mapped(widget))
    return NULL;
  if (G_TYPE_CHECK_INSTANCE_TYPE(widget, type) && ADW_IS_PREFERENCES_ROW(widget) &&
      g_strcmp0(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(widget)), title) == 0)
    return widget;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = find_mapped(c, type, title);
    if (found)
      return found;
  }
  return NULL;
}

/* Release path: the window is glued as gh-app-services.c glues it, with
 * gh_mls_ui_attach_if_enabled() and the default-on feature flag. */
static void
test_gui_flag_new_group(void)
{
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  GhNip29Service *nip29 = nip29_up(alice);
  GhWindow *window = test_window();
  gh_conversation_list_attach(window, alice->model, NULL);
  GhGroupUiConfig groups = { .conversations = alice->model, .service = the_nip29,
                             .service_data = nip29 };
  gh_group_ui_attach(window, &groups);
  GhMlsUiConfig mls = {
    .conversations = alice->model,
    .accounts = alice->accounts,
    .settings = alice->settings,
    .service = app_service,
    .service_data = alice,
    .account_relays = alice->relays,
  };
  gboolean attached = gh_mls_ui_attach_if_enabled(window, &mls);
  g_assert_true(attached);
  g_assert_true(gh_mls_ui_enabled());
  g_assert_true(g_action_group_has_action(G_ACTION_GROUP(window), "group-invitations"));

  g_action_group_activate_action(G_ACTION_GROUP(window), "new-group", NULL);
  AdwNavigationPage *dialog = visible_flow(window);
  g_assert_true(GH_IS_NEW_GROUP_PAGE(dialog));
  spin_until(is_mapped, dialog, "New Group shown");
  AdwNavigationView *navigation = ADW_NAVIGATION_VIEW(
    gtk_widget_get_template_child(GTK_WIDGET(dialog), GH_TYPE_NEW_GROUP_PAGE, "navigation"));
  const gchar *first = adw_navigation_page_get_tag(adw_navigation_view_get_visible_page(navigation));
  g_autoptr(GListModel) stack = adw_navigation_view_get_navigation_stack(navigation);
  g_assert_cmpuint(g_list_model_get_n_items(stack), ==, 1);   /* nothing to go back to */
  g_assert_cmpstr(first, ==, "type");
  g_assert_nonnull(gh_new_group_page_get_encrypted_page(GH_NEW_GROUP_PAGE(dialog)));
  g_assert_nonnull(find_mapped(GTK_WIDGET(dialog), ADW_TYPE_ACTION_ROW, "Encrypted Group"));
  gh_window_close_panels(window);
  drain();
  close_window(window);
  gh_test_release(nip29);
  world_down(&w);
}

/* ---- --gui: Group Info ------------------------------------------------------------------ */

static void
confirm(AdwAlertDialog *alert, GtkWidget *parent, const gchar *response)
{
  spin_until(gh_test_dialog_shown, alert, "the confirmation shown");
  (void)parent;
  g_signal_emit_by_name(alert, "response", response);
  adw_dialog_force_close(ADW_DIALOG(alert));
  drain();
}

static gboolean
relay_ok_is_held(gpointer data)
{
  WireRelay *relay = data;
  return relay->held_oks->len > 0;
}

static GhMlsGroupInfoDialog *
show_info(GhWindow *window, GhConversation *conversation)
{
  g_assert_true(gh_mls_ui_show_info(window, conversation, NULL));
  AdwDialog *dialog = visible_dialog(window);
  g_assert_true(GH_IS_MLS_GROUP_INFO_DIALOG(dialog));
  spin_until(gh_test_dialog_shown, dialog, "Group Info shown");
  return GH_MLS_GROUP_INFO_DIALOG(dialog);
}

typedef struct {
  GhWindow *window;
  const gchar *text;
} SubtitleWait;

static gboolean
subtitle_is(gpointer data)
{
  SubtitleWait *wait = data;
  return g_strcmp0(header_subtitle(wait->window), wait->text) == 0;
}

typedef struct {
  App *bob;
  GhMlsGroup *ga, *gb;
} UpgradeWait;

/* nostrc-8ndz: the Add that brought Bob required SelfRemove (both apps
 * support it); settled when Bob leaves by it and nothing of Alice's is
 * pending. */
static gboolean
self_remove_settled(gpointer data)
{
  UpgradeWait *wait = data;
  return gh_mls_service_leave_kind(wait->bob->service, wait->gb) == GH_MLS_LEAVE_EVERYONE &&
         !gh_mls_group_get_pending_commit(wait->ga) &&
         gh_mls_group_get_epoch(wait->ga) == gh_mls_group_get_epoch(wait->gb);
}

static void
test_gui_group_info(void)
{
  World w;
  const guint keys[] = { ALICE, BOB, CAROL };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB], *carol = &w.apps[CAROL];
  wait_published(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  accept_contact(alice, CAROL);
  GhMlsGroup *ga = create_group(alice, "Info", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  GhMlsGroup *gb = join(bob, ALICE);
  UpgradeWait upgraded = { bob, ga, gb };
  spin_until(self_remove_settled, &upgraded, "the group requiring SelfRemove");
  group_send_stub_reset();
  GhWindow *window = app_window(alice, NULL);
  GhConversation *conversation = gh_conversation_store_lookup(alice->model, room);
  g_assert_true(gh_window_open_item(window, conversation));
  g_assert_cmpstr(header_subtitle(window), ==, "Encrypted group · 2 members");

  /* The owner: badges, Remove on everyone else, Add. */
  GhMlsGroupInfoDialog *info = show_info(window, conversation);
  gboolean removable = TRUE;
  g_assert_cmpstr(gh_mls_group_info_dialog_get_member(info, hex[ALICE], &removable), ==,
                  "Owner");
  g_assert_false(removable);
  g_assert_cmpstr(gh_mls_group_info_dialog_get_member(info, hex[BOB], &removable), ==, "");
  g_assert_true(removable);
  GtkWidget *device = find_type(GTK_WIDGET(info), ADW_TYPE_ACTION_ROW, "On this device only");
  g_assert_nonnull(device);
  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(device)), ==,
                  "Group history stays on this device. It can’t be exported or restored if this device’s data is lost.");

  /* Add Carol: accepted contacts not in the group, a fresh check. */
  gtk_widget_activate_action(GTK_WIDGET(info), "mls-group.add-members", NULL);
  GhMlsInviteePicker *picker = gh_mls_group_info_dialog_get_add_picker(info);
  g_assert_cmpuint(gh_mls_invitee_picker_get_n_listed(picker), ==, 1);
  g_assert_null(gh_mls_invitee_picker_get_row(picker, hex[BOB]));   /* a member already */
  gh_mls_invitee_picker_set_selected(picker, hex[CAROL], TRUE);
  wait_pick(picker, hex[CAROL], GROUNDHOG_READY);
  gtk_widget_activate_action(GTK_WIDGET(info), "mls-group.save-add", NULL);
  spin_until(nothing_pending, info, "the Add merged");
  wait_text(last_info_toast, info, "Invited 1 person. They join once they accept.");
  wait_members(ga, 3);
  SubtitleWait three = { window, "Encrypted group · 3 members" };
  spin_until(subtitle_is, &three, "the header's member count");
  g_assert_cmpstr(gh_mls_group_info_dialog_get_member(info, hex[CAROL], &removable), ==, "");
  GhMlsGroup *gc = join(carol, ALICE);

  /* Rename. */
  gtk_widget_activate_action(GTK_WIDGET(info), "mls-group.rename", NULL);
  gh_mls_group_info_dialog_set_rename(info, "Renamed Club", "Books, mostly");
  gtk_widget_activate_action(GTK_WIDGET(info), "mls-group.save-rename", NULL);
  spin_until(nothing_pending, info, "the rename merged");
  wait_text(last_info_toast, info, "Name and description saved");
  wait_text(gh_mls_group_get_name, gb, "Renamed Club");
  g_assert_cmpstr(gh_mls_group_get_description(gb), ==, "Books, mostly");

  /* Closing Group Info while its media-policy Commit awaits an OK must
   * release the dialog; the later completion must not touch its widgets. */
  const gchar *servers[] = { "https://files.example.invalid", NULL };
  g_settings_set_strv(alice->settings, "blossom-servers", servers);
  w.g.hold_oks = TRUE;
  gtk_widget_activate_action(GTK_WIDGET(info), "mls-group.update-media", NULL);
  g_assert_cmpuint(gh_mls_group_info_dialog_get_pending(info), ==, 1);
  spin_until(relay_ok_is_held, &w.g, "media-policy OK held");
  gpointer closed_info = info;
  g_object_add_weak_pointer(G_OBJECT(info), &closed_info);
  adw_dialog_force_close(ADW_DIALOG(info));
  drain();
  g_assert_null(closed_info);
  wire_relay_release_oks(&w.g);
  drain();
  g_settings_reset(alice->settings, "blossom-servers");
  info = show_info(window, conversation);

  /* Remove Carol, after the confirmation naming what it does. */
  gtk_widget_activate_action(GTK_WIDGET(info), "mls-group.remove", "s", hex[CAROL]);
  AdwAlertDialog *remove = gh_mls_group_info_dialog_get_remove_dialog(info);
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(remove),
                          "can’t read anything sent to the group after this"));
  confirm(remove, GTK_WIDGET(info), "remove-confirm");
  spin_until(nothing_pending, info, "the removal merged");
  wait_text(last_info_toast, info, "Removed from the group");
  wait_members(ga, 2);
  g_assert_null(gh_mls_group_info_dialog_get_member(info, hex[CAROL], NULL));
  adw_dialog_force_close(ADW_DIALOG(info));
  drain();

  /* Carol, removed (nostrc-xrya): the composer says so and by whom, Group
   * Info too, and nothing is "unable to decrypt". */
  spin_until(group_removed, gc, "Carol's group ending");
  GhWindow *carol_window = app_window(carol, NULL);
  GhConversation *carol_conversation = gh_conversation_store_lookup(carol->model, room);
  g_assert_true(gh_window_open_item(carol_window, carol_conversation));
  g_autofree gchar *alice_npub = gh_recipient_npub_short(hex[ALICE]);
  g_autofree gchar *removed_reason =
    g_strdup_printf("You were removed from this group by %s.", alice_npub);
  gpointer carol_data = NULL;
  const GhSendUiDelegate *carol_delegate =
    group_send_stub_delegate_for(carol_conversation, &carol_data);
  g_autofree gchar *carol_reason = carol_delegate->reason(carol_conversation, carol_data);
  g_assert_cmpstr(carol_reason, ==, removed_reason);
  g_autoptr(GError) carol_error = NULL;
  g_assert_false(carol_delegate->send(carol_conversation, "still here", carol_data,
                                      &carol_error));
  g_assert_cmpstr(carol_error->message, ==,
                  "You were removed from this group, so the message was not sent. It is kept "
                  "here.");
  GhMlsGroupInfoDialog *carol_info = show_info(carol_window, carol_conversation);
  AdwActionRow *carol_status = ADW_ACTION_ROW(gtk_widget_get_template_child(
    GTK_WIDGET(carol_info), GH_TYPE_MLS_GROUP_INFO_DIALOG, "messages_row"));
  g_autofree gchar *removed_status = g_strdup_printf(
    "You were removed from this group by %s. Its messages stay on this device.", alice_npub);
  g_assert_cmpstr(adw_action_row_get_subtitle(carol_status), ==, removed_status);
  adw_dialog_force_close(ADW_DIALOG(carol_info));
  send_text(alice, ga, "after carol");
  wait_message(bob, room, "after carol");
  drain();
  g_assert_cmpuint(gh_mls_group_get_unreadable(gc), ==, 0);
  g_assert_false(gh_mls_group_get_decrypt_pending(gc));
  g_assert_false(gh_conversation_view_get_decrypt_pending(view_of(carol_window)));
  close_window(carol_window);

  /* Bob, a member: the Owner badge, no admin action anywhere. */
  GhWindow *bob_window = app_window(bob, NULL);
  GhConversation *bob_conversation = gh_conversation_store_lookup(bob->model, room);
  g_assert_true(gh_window_open_item(bob_window, bob_conversation));
  GhMlsGroupInfoDialog *bob_info = show_info(bob_window, bob_conversation);
  g_assert_cmpstr(gh_mls_group_info_dialog_get_member(bob_info, hex[ALICE], &removable), ==,
                  "Owner");
  g_assert_false(removable);
  g_assert_cmpstr(gh_mls_group_info_dialog_get_member(bob_info, hex[BOB], &removable), ==, "");
  g_assert_false(removable);
  GtkWidget *add = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(bob_info),
                                                            GH_TYPE_MLS_GROUP_INFO_DIALOG,
                                                            "add_member_button"));
  g_assert_false(gtk_widget_get_visible(add));
  gtk_widget_activate_action(GTK_WIDGET(bob_info), "mls-group.add-members", NULL);
  g_assert_cmpuint(gh_mls_group_info_dialog_get_pending(bob_info), ==, 0);
  adw_dialog_force_close(ADW_DIALOG(bob_info));
  drain();

  /* "Some messages … can't be read yet" (nostrc-oya4): not after the join,
   * although Bob may hold his own Add Commit; then a message whose Commit
   * Bob hasn't got (served dated a minute later, outside the join's own
   * second), then the Commit. No number: held events may be group changes. */
  PendingWait view_wait = { view_of(bob_window), FALSE };
  spin_until(pending_is, &view_wait, "no banner after the join");
  g_assert_false(gh_mls_group_get_decrypt_pending(gb));
  set_online(bob, FALSE);
  OpWait renamed = { 0 };
  gh_mls_service_update_metadata_async(alice->service, ga, "Next", NULL, NULL, on_changed,
                                       &renamed);
  spin_until(op_done, &renamed, "the rename");
  g_assert_no_error(renamed.error);
  g_autofree gchar *commit = g_strdup(last_stored_445(&w.g)->id);
  wire_relay_withhold(&w.g, commit);
  send_text(alice, ga, "next epoch");
  MessageWait next = { alice, room, "next epoch" };
  spin_until(sent, &next, "the next epoch's message sent");
  WireStored *original = last_stored_445(&w.g);
  g_autofree gchar *later = resigned(original->json, real_now() + 60);
  wire_relay_withhold(&w.g, original->id);
  wire_relay_inject(&w.g, later);
  set_online(bob, TRUE);
  view_wait.pending = TRUE;
  spin_until(pending_is, &view_wait, "the view's banner");
  g_assert_true(gh_mls_group_get_decrypt_pending(gb));
  GtkLabel *label = GTK_LABEL(gtk_widget_get_template_child(GTK_WIDGET(view_of(bob_window)),
                                                            GH_TYPE_CONVERSATION_VIEW,
                                                            "undecryptable_label"));
  g_assert_cmpstr(gtk_label_get_text(label), ==,
                  "Some messages in this group can't be read yet.");
  GtkWidget *undecryptable = GTK_WIDGET(gtk_widget_get_template_child(
    GTK_WIDGET(view_of(bob_window)), GH_TYPE_CONVERSATION_VIEW, "undecryptable_row"));
  g_assert_true(gtk_widget_get_visible(undecryptable));
  g_assert_null(find_message(bob, room, "next epoch"));
  wire_relay_release(&w.g, commit);
  wait_message(bob, room, "next epoch");
  view_wait.pending = FALSE;
  spin_until(pending_is, &view_wait, "the banner gone");
  g_assert_false(gtk_widget_get_visible(undecryptable));

  /* Bob (not an admin) leaves for everyone (nostrc-2um6): the copy says how;
   * he is "leaving" until a member's service commits his leave, and Alice's
   * Group Info toasts that he left. */
  GhMlsGroupInfoDialog *alice_info = show_info(window, conversation);
  bob_info = show_info(bob_window, bob_conversation);
  gtk_widget_activate_action(GTK_WIDGET(bob_info), "mls-group.leave", NULL);
  AdwAlertDialog *leave = gh_mls_group_info_dialog_get_leave_dialog(bob_info);
  g_assert_cmpstr(adw_alert_dialog_get_heading(leave), ==, "Leave Group?");
  /* Between Groundhog accounts the group is adopted (nostrc-lf62); in a build
   * without the adopted producer it is an MDK 0.8 group whose first Add
   * required SelfRemove (nostrc-8ndz), as every member supports it. Either
   * way: Bob's own leave, the others told. */
  g_assert_cmpint(gh_mls_group_get_adopted(gb), ==, GH_MLS_ADOPTED_KEY_PACKAGES);
  g_assert_cmpstr(adw_alert_dialog_get_body(leave), ==,
                  gh_mls_leave_copy(GH_MLS_LEAVE_EVERYONE));
  confirm(leave, GTK_WIDGET(bob_info), "leave-confirm");
  g_assert_cmpstr(gh_mls_group_info_dialog_get_last_toast(bob_info), ==, "Leaving the group…");
  gpointer data = NULL;
  const GhSendUiDelegate *delegate = group_send_stub_delegate_for(bob_conversation, &data);
  if (gh_mls_group_get_active(gb)) {
    g_autofree gchar *leaving = delegate->reason(bob_conversation, data);
    g_assert_cmpstr(leaving, ==, "You’re leaving this group, so nothing more can be sent to it.");
  }
  spin_until(left_for_everyone, gb, "Bob's leave confirmed");
  g_autofree gchar *reason = delegate->reason(bob_conversation, data);
  g_assert_cmpstr(reason, ==, "You left this group.");
  wait_members(ga, 1);
  g_auto(GStrv) members = gh_mls_group_dup_members(ga);
  g_assert_false(strv_has((const gchar *const *)members, hex[BOB]));   /* gone for everyone */
  spin_until(toast_says_left, alice_info, "Alice's toast that Bob left");
  adw_dialog_force_close(ADW_DIALOG(alice_info));
  adw_dialog_force_close(ADW_DIALOG(bob_info));
  drain();

  close_window(bob_window);
  close_window(window);
  world_down(&w);
}

/* ---- --gui: Delete for Everyone in a group left (nostrc-knbkr) ------------------------ */

/* The application's delete callback in a test window: the group's service
 * deletes it (gh_app_outbox_delete_group_message() in the app). */
static gboolean
service_delete(GhConversation *conversation, GhMessage *message, gboolean for_everyone,
               gpointer data, GError **error)
{
  App *app = data;
  GhMlsGroup *group = gh_mls_service_lookup(app->service,
                                            gh_conversation_get_room_id(conversation));
  return group && gh_mls_service_delete_message(app->service, group,
                                                gh_message_get_rumor_id(message),
                                                for_everyone, error);
}

static GhMessageRow *
message_row(GtkWidget *widget, const gchar *text)
{
  if (GH_IS_MESSAGE_ROW(widget)) {
    GhMessage *message = gh_message_row_get_message(GH_MESSAGE_ROW(widget));
    if (message && g_strcmp0(gh_message_get_content(message), text) == 0)
      return GH_MESSAGE_ROW(widget);
  }
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GhMessageRow *found = message_row(c, text);
    if (found)
      return found;
  }
  return NULL;
}

typedef struct {
  GhWindow *window;
  const gchar *text;
} RowWait;

static gboolean
row_shown(gpointer data)
{
  RowWait *wait = data;
  return message_row(GTK_WIDGET(view_of(wait->window)), wait->text) != NULL;
}

static gboolean
menu_up(gpointer data)
{
  return gtk_widget_get_mapped(data);
}

static gboolean
menu_down(gpointer data)
{
  return !gtk_widget_get_mapped(data);
}

static void
menu_flatten(GMenuModel *model, GString *out)
{
  for (int i = 0; i < g_menu_model_get_n_items(model); i++) {
    g_autoptr(GMenuModel) section = g_menu_model_get_item_link(model, i, G_MENU_LINK_SECTION);
    if (section) {
      menu_flatten(section, out);
      continue;
    }
    g_autofree gchar *label = NULL;
    g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_LABEL, "s", &label);
    g_string_append_printf(out, "%s%s", out->len ? "|" : "", label);
  }
}

/* save_root_png() with the open @popover drawn where it sits (its own
 * surface is not part of the window's), as a screenshot of the screen. */
static void
save_with_popover(GhWindow *window, GtkWidget *popover, const gchar *name)
{
  const gchar *prefix = g_getenv("GROUNDHOG_SCREENSHOT_PREFIX");
  if (!prefix || !*prefix)
    return;
  GtkWidget *root = GTK_WIDGET(window);
  GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(root));
  double wx = 0, wy = 0, px = 0, py = 0;
  gtk_native_get_surface_transform(GTK_NATIVE(root), &wx, &wy);
  GtkSnapshot *snapshot = gtk_snapshot_new();
  g_autoptr(GdkPaintable) window_paintable = gtk_widget_paintable_new(root);
  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(wx, wy));
  gdk_paintable_snapshot(window_paintable, snapshot, gtk_widget_get_width(root),
                         gtk_widget_get_height(root));
  gtk_snapshot_restore(snapshot);
  GdkSurface *popup = gtk_native_get_surface(GTK_NATIVE(popover));
  gtk_native_get_surface_transform(GTK_NATIVE(popover), &px, &py);
  g_autoptr(GdkPaintable) popover_paintable = gtk_widget_paintable_new(popover);
  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot,
                         &GRAPHENE_POINT_INIT(gdk_popup_get_position_x(GDK_POPUP(popup)) + px,
                                              gdk_popup_get_position_y(GDK_POPUP(popup)) + py));
  gdk_paintable_snapshot(popover_paintable, snapshot, gtk_widget_get_width(popover),
                         gtk_widget_get_height(popover));
  gtk_snapshot_restore(snapshot);
  g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(snapshot);
  g_assert_nonnull(node);
  GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(root));
  g_autoptr(GdkTexture) texture = gsk_renderer_render_texture(
    renderer, node, &GRAPHENE_RECT_INIT(0, 0, gdk_surface_get_width(surface),
                                        gdk_surface_get_height(surface)));
  g_autofree gchar *path = g_strdup_printf("%s-%s.png", prefix, name);
  g_assert_true(gdk_texture_save_to_png(texture, path));
  g_test_message("saved %s", path);
}

/* The bubble menu of the row showing @text, opened as right-click does, its
 * labels joined with "|"; saved as <prefix>-<shot>.png while open. */
static gchar *
bubble_menu(GhWindow *window, const gchar *text, const gchar *shot)
{
  RowWait wait = { window, text };
  spin_until(row_shown, &wait, "the message's row");
  GhMessageRow *row = message_row(GTK_WIDGET(view_of(window)), text);
  g_assert_true(gh_message_row_popup_menu(row, -1, -1));
  GtkPopover *menu = gh_message_row_get_menu(row);
  g_assert_true(GTK_IS_POPOVER_MENU(menu));
  spin_until(menu_up, menu, "the bubble menu shown");
  gtk_test_widget_wait_for_draw(GTK_WIDGET(menu));
  save_with_popover(window, GTK_WIDGET(menu), shot);
  GString *labels = g_string_new(NULL);
  menu_flatten(gtk_popover_menu_get_menu_model(GTK_POPOVER_MENU(menu)), labels);
  gtk_popover_popdown(menu);
  spin_until(menu_down, menu, "the bubble menu closed");
  return g_string_free(labels, FALSE);
}

/* Bob's own sent message offers Delete for Everyone while he is in the
 * group; once he has left it, only Delete for Me (nothing can reach the
 * group any more, so the request could only fail). */
static void
test_gui_left_group_delete(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_published(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Left behind", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  GhMlsGroup *gb = join(bob, ALICE);
  UpgradeWait upgraded = { bob, ga, gb };
  spin_until(self_remove_settled, &upgraded, "the group requiring SelfRemove");
  send_text(bob, gb, "Bob's own words");
  MessageWait bob_sent = { bob, room, "Bob's own words" };
  spin_until(sent, &bob_sent, "Bob's message sent");
  wait_message(alice, room, "Bob's own words");

  GhWindow *window = app_window(bob, NULL);
  gh_conversation_view_set_delete_func(view_of(window), service_delete, bob, NULL);
  GhConversation *conversation = gh_conversation_store_lookup(bob->model, room);
  g_assert_true(gh_window_open_item(window, conversation));
  g_autofree gchar *member = bubble_menu(window, "Bob's own words", "member-menu");
  g_assert_nonnull(strstr(member, "_Delete for Me…"));
  g_assert_nonnull(strstr(member, "Delete for _Everyone…"));
  g_assert_nonnull(strstr(member, "Add _Reaction…"));

  g_autoptr(GError) error = NULL;
  g_assert_true(gh_mls_service_leave(bob->service, gb, &error));
  g_assert_no_error(error);
  spin_until(left_for_everyone, gb, "Bob's leave confirmed");
  g_autofree gchar *left = bubble_menu(window, "Bob's own words", "left-menu");
  g_assert_nonnull(strstr(left, "_Delete for Me…"));
  g_assert_null(strstr(left, "Delete for _Everyone…"));
  g_assert_null(strstr(left, "Add _Reaction…"));

  /* Delete for Me still works there. */
  GhMessage *own = find_message(bob, room, "Bob's own words");
  g_assert_nonnull(own);
  g_assert_true(service_delete(conversation, own, FALSE, bob, &error));
  g_assert_no_error(error);
  g_assert_null(find_message(bob, room, "Bob's own words"));
  g_assert_nonnull(find_message(alice, room, "Bob's own words"));
  close_window(window);
  world_down(&w);
}

#if GH_MLS_ADOPTED_KEY_PACKAGES
static const gchar *
format_notice(gpointer page)
{
  return gh_mls_new_group_page_get_format_notice(page);
}

/* nostrc-lf62: the group's format on New Group. Bob's app speaks only the
 * newer (adopted) format, Carol's only the older one. Carol alone: the
 * group will use the older format, and the page says so. Both: one group
 * can't use both formats; the page explains it and offers the choice, Keep
 * Newer-Format People or Keep Older-Format People, each un-choosing the
 * others. The older-format group is made; its Group Info refuses to add
 * Bob, saying why. */
static void
test_gui_new_group_formats(void)
{
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  wait_published(&w, keys, G_N_ELEMENTS(keys));
  inject_adopted_key_package(&w, BOB);
  g_free(inject_legacy_key_package(&w, CAROL));
  accept_contact(alice, BOB);
  accept_contact(alice, CAROL);
  GhNip29Service *nip29 = nip29_up(alice);
  group_send_stub_reset();
  GhWindow *window = app_window(alice, nip29);
  g_action_group_activate_action(G_ACTION_GROUP(window), "new-group", NULL);
  AdwNavigationPage *dialog = visible_flow(window);
  g_assert_true(GH_IS_NEW_GROUP_PAGE(dialog));
  spin_until(is_mapped, dialog, "New Group shown");
  gtk_widget_activate_action(GTK_WIDGET(dialog), "new-group.choose-encrypted", NULL);
  GhMlsNewGroupPage *page = GH_MLS_NEW_GROUP_PAGE(
    gh_new_group_page_get_encrypted_page(GH_NEW_GROUP_PAGE(dialog)));
  gh_mls_new_group_page_set_name(page, "Formats");
  /* The group relay G, not W: the invitees' KeyPackages are on W, and a
   * lookup never asks the new group's relays (nostrc-0bdg). */
  g_assert_true(gh_mls_new_group_page_add_relay(page, w.g.url));
  gtk_widget_activate_action(find_type(GTK_WIDGET(page), GH_TYPE_MLS_RELAY_ROW, w.w.url),
                             "relay.remove", NULL);
  GhMlsInviteePicker *picker = gh_mls_new_group_page_get_picker(page);
  const gchar *servers[] = { "https://blossom.example.com", NULL };
  g_settings_set_strv(alice->settings, "blossom-servers", servers);

  /* Carol alone: the older format, said, and Create runs. */
  gh_mls_invitee_picker_set_selected(picker, hex[CAROL], TRUE);
  wait_pick(picker, hex[CAROL], GH_MLS_INVITEE_READY_UNPROVEN);
  g_assert_null(create_reason(page));
  g_assert_nonnull(format_notice(page));
  g_assert_nonnull(strstr(format_notice(page),
                          "Some people use an older app version; this group will use the "
                          "older format."));
  g_assert_false(gh_mls_new_group_page_get_format_choice(page));
  g_assert_null(gh_mls_new_group_page_get_media_notice(page));

  /* Bob too: no format in common. */
  gh_mls_invitee_picker_set_selected(picker, hex[BOB], TRUE);
  wait_pick(picker, hex[BOB], GH_MLS_INVITEE_READY_ADOPTED_ONLY);
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(gh_mls_invitee_picker_get_row(picker,
                                                                                    hex[BOB])),
                          "joins only groups in the newer format"));
  g_assert_nonnull(create_reason(page));
  g_assert_nonnull(strstr(create_reason(page), "One group can’t use both formats"));
  g_assert_null(format_notice(page));
  g_assert_true(gh_mls_new_group_page_get_format_choice(page));

  /* Keep the newer format's people: Carol is un-chosen; an adopted group
   * could be made. */
  gtk_widget_activate_action(GTK_WIDGET(page), "mls-new.keep-adopted", NULL);
  g_assert_cmpuint(gh_mls_invitee_picker_get_n_selected(picker), ==, 1);
  g_assert_cmpint(gh_mls_invitee_picker_get_state(picker, hex[BOB]), ==,
                  GH_MLS_INVITEE_READY_ADOPTED_ONLY);
  g_assert_null(create_reason(page));
  g_assert_null(format_notice(page));
  g_assert_false(gh_mls_new_group_page_get_format_choice(page));
  g_assert_nonnull(strstr(gh_mls_new_group_page_get_media_notice(page), "1 server"));

  /* Carol again, then keep the older format's people: Bob is un-chosen. */
  gh_mls_invitee_picker_set_selected(picker, hex[CAROL], TRUE);
  wait_pick(picker, hex[CAROL], GH_MLS_INVITEE_READY_UNPROVEN);
  g_assert_true(gh_mls_new_group_page_get_format_choice(page));
  gtk_widget_activate_action(GTK_WIDGET(page), "mls-new.keep-legacy", NULL);
  g_assert_cmpuint(gh_mls_invitee_picker_get_n_selected(picker), ==, 1);
  g_assert_cmpint(gh_mls_invitee_picker_get_state(picker, hex[CAROL]), ==,
                  GH_MLS_INVITEE_READY_UNPROVEN);
  g_assert_null(create_reason(page));
  g_assert_nonnull(format_notice(page));
  g_assert_false(gh_mls_new_group_page_get_format_choice(page));
  g_assert_null(gh_mls_new_group_page_get_media_notice(page));

  /* Created: an older-format group. */
  gtk_widget_activate_action(GTK_WIDGET(page), "mls-new.create", NULL);
  wait_text(status_title, page, "Group Created");
  GhMlsGroup *ga = gh_mls_new_group_page_get_group(page);
  g_assert_nonnull(ga);
  g_assert_false(gh_mls_group_get_adopted(ga));
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  GhConversation *conversation = gh_conversation_store_lookup(alice->model, room);
  gtk_widget_activate_action(GTK_WIDGET(page), "mls-new.open", NULL);
  spin_until(no_flow, window, "New Group closed");

  /* Group Info: Bob can't join this group's format, and it says why. */
  GhMlsGroupInfoDialog *info = show_info(window, conversation);
  gtk_widget_activate_action(GTK_WIDGET(info), "mls-group.add-members", NULL);
  GhMlsInviteePicker *add = gh_mls_group_info_dialog_get_add_picker(info);
  gh_mls_invitee_picker_set_selected(add, hex[BOB], TRUE);
  wait_pick(add, hex[BOB], GH_MLS_INVITEE_READY_ADOPTED_ONLY);
  GtkLabel *add_reason = GTK_LABEL(gtk_widget_get_template_child(
    GTK_WIDGET(info), GH_TYPE_MLS_GROUP_INFO_DIALOG, "add_reason"));
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(add_reason)));
  g_assert_nonnull(strstr(gtk_label_get_text(add_reason),
                          "joins only newer-format groups, and this group uses the older "
                          "format"));
  adw_dialog_force_close(ADW_DIALOG(info));
  drain();
  close_window(window);
  gh_test_release(nip29);
  world_down(&w);
}
#endif

#if GH_MLS_ADOPTED_KEY_PACKAGES
/* Review M2: New Group creates the format it showed, or nothing. Bob is
 * checked Ready (both formats: the newer-format group, no notice); then his
 * write relay keeps his adopted KeyPackage back. Create is refused with
 * "Someone's invitation key changed; review and try again" -- no
 * older-format group made unseen -- and Bob is checked again: now the page
 * says the group would use the older format. */
static void
test_gui_new_group_format_changed(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  wait_published(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhNip29Service *nip29 = nip29_up(alice);
  group_send_stub_reset();
  GhWindow *window = app_window(alice, nip29);
  g_action_group_activate_action(G_ACTION_GROUP(window), "new-group", NULL);
  AdwNavigationPage *dialog = visible_flow(window);
  g_assert_true(GH_IS_NEW_GROUP_PAGE(dialog));
  spin_until(is_mapped, dialog, "New Group shown");
  gtk_widget_activate_action(GTK_WIDGET(dialog), "new-group.choose-encrypted", NULL);
  GhMlsNewGroupPage *page = GH_MLS_NEW_GROUP_PAGE(
    gh_new_group_page_get_encrypted_page(GH_NEW_GROUP_PAGE(dialog)));
  gh_mls_new_group_page_set_name(page, "As Shown");
  g_assert_true(gh_mls_new_group_page_add_relay(page, w.g.url));
  gtk_widget_activate_action(find_type(GTK_WIDGET(page), GH_TYPE_MLS_RELAY_ROW, w.w.url),
                             "relay.remove", NULL);
  GhMlsInviteePicker *picker = gh_mls_new_group_page_get_picker(page);
  gh_mls_invitee_picker_set_selected(picker, hex[BOB], TRUE);
  wait_pick(picker, hex[BOB], GH_MLS_INVITEE_READY);
  g_assert_null(create_reason(page));
  g_assert_null(gh_mls_new_group_page_get_format_notice(page));
  g_assert_cmpint(gh_mls_new_group_page_get_format(page), ==, GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED);

  g_assert_cmpuint(withhold_key_packages(&w.w, BOB, GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED), ==, 1);
  guint g_events = w.g.events;
  gtk_widget_activate_action(GTK_WIDGET(page), "mls-new.create", NULL);
  wait_text(status_title, page, "Group Not Created");
  GtkLabel *description = GTK_LABEL(gtk_widget_get_template_child(
    GTK_WIDGET(page), GH_TYPE_MLS_NEW_GROUP_PAGE, "status_description"));
  g_assert_nonnull(strstr(gtk_label_get_text(description),
                          "Someone’s invitation key changed; review and try again"));
  g_assert_null(gh_mls_new_group_page_get_group(page));
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(alice->service)), ==, 0);
  g_assert_cmpuint(w.g.events, ==, g_events);
  /* Checked again: what Create would make now, said. */
  wait_pick(picker, hex[BOB], GH_MLS_INVITEE_READY_LEGACY);
  g_assert_nonnull(gh_mls_new_group_page_get_format_notice(page));
  g_assert_cmpint(gh_mls_new_group_page_get_format(page), ==, GH_MLS_KEY_PACKAGE_FORMAT_LEGACY);
  gh_window_close_panels(window);
  drain();
  close_window(window);
  gh_test_release(nip29);
  world_down(&w);
}
#endif

/* ---- --gui: files and pictures in encrypted groups (W25) ----------------------------- */

/* The cards' provider of the application's group delegate
 * (gh-mls-attachment-ui.c), on the test window. */
static GhAttachmentTransfer *
files_lookup(GhMessage *message, gpointer data)
{
  return gh_mls_attachments_lookup(data, message, 0);
}

static GhAttachmentTransfer *
files_lookup_at(GhMessage *message, guint index, gpointer data)
{
  return gh_mls_attachments_lookup(data, message, index);
}

static void
files_download(GhAttachmentTransfer *transfer, gpointer data)
{
  gh_mls_attachments_download(data, transfer);
}

static void
files_cancel(GhAttachmentTransfer *transfer, gpointer data)
{
  gh_mls_attachments_cancel(data, transfer);
}

static void
files_save(GhAttachmentTransfer *transfer, GtkWidget *card, gpointer data)
{
  (void)transfer;
  (void)card;
  (void)data;
}

static const GhAttachmentCardProvider files_provider = {
  .lookup = files_lookup,
  .download = files_download,
  .cancel = files_cancel,
  .save = files_save,
  .lookup_at = files_lookup_at,
};

/* The button bound to action (its action-name), or NULL. */
static GtkWidget *
button_for(GtkWidget *widget, const gchar *action)
{
  if (GTK_IS_ACTIONABLE(widget) &&
      g_strcmp0(gtk_actionable_get_action_name(GTK_ACTIONABLE(widget)), action) == 0)
    return widget;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = button_for(c, action);
    if (found)
      return found;
  }
  return NULL;
}

static gboolean
card_shown(gpointer view)
{
  return find_type(view, GH_TYPE_ATTACHMENT_CARD, NULL) != NULL;
}

static gboolean
card_has_transfer(gpointer card)
{
  return gh_attachment_card_get_transfer(card) != NULL;
}

static gboolean
card_ready(gpointer card)
{
  GhAttachmentTransfer *t = gh_attachment_card_get_transfer(card);
  return t && gh_attachment_transfer_get_state(t) == GH_ATTACHMENT_STATE_READY;
}

static GBytes *
tiny_png(void)
{
  /* A 1x1 PNG GTK decodes (the decode guard passes). */
  static const guint8 png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f,
    0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8,
    0xcf, 0xc0, 0xf0, 0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x89, 0x99, 0x3d, 0x1d, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
  };
  return g_bytes_new_static(png, sizeof png);
}

typedef struct {
  gboolean done;
  GhMessage *message;
  GError *error;
} FileSent;

static gboolean
file_sent(gpointer data)
{
  return ((FileSent *)data)->done;
}

static void
on_file_sent(GObject *source, GAsyncResult *result, gpointer data)
{
  FileSent *sent = data;
  sent->message = gh_mls_attachments_send_finish(GH_MLS_ATTACHMENTS(source), result, NULL,
                                                 &sent->error);
  sent->done = TRUE;
}

/* Group Info of a legacy group says it can't have a picture and offers no
 * change; a received photo's card fetches nothing until Download, then
 * shows the photo inline. */
static void
test_gui_group_files(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_legacy_only = TRUE;   /* MDK 0.8 KeyPackages only: a legacy group */
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_published(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Files", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  join(bob, ALICE);
  BlossomFixture *blossom = blossom_fixture_new();
  g_autoptr(GSettingsBackend) backend = g_memory_settings_backend_new();
  g_autoptr(GSettings) settings = g_settings_new_with_backend("org.nostr.Groundhog", backend);
  const gchar *servers[] = { blossom_fixture_url(blossom), NULL };
  g_settings_set_strv(settings, "blossom-servers", servers);
  g_settings_set_string(settings, "network-mode", "none");
  g_autoptr(GhNetHttp) http = gh_net_http_new(settings);
  GhAttachmentsConfig config = { .settings = settings, .http = http };
  GhAttachments *attachments_a = gh_attachments_new(&config);
  GhAttachments *attachments_b = gh_attachments_new(&config);
  gh_attachments_set_allow_private_hosts(attachments_a, TRUE);
  gh_attachments_set_allow_private_hosts(attachments_b, TRUE);
  gh_attachments_set_store(attachments_a, alice->store);
  gh_attachments_set_store(attachments_b, bob->store);
  GhMlsAttachments *files_a = gh_mls_attachments_new(attachments_a);
  GhMlsAttachments *files_b = gh_mls_attachments_new(attachments_b);
  gh_mls_attachments_set_service(files_a, alice->service);
  gh_mls_attachments_set_service(files_b, bob->service);

  /* The picture section of a legacy group: honest, and nothing to do. */
  window_files = files_a;
  GhWindow *window = app_window(alice, NULL);
  window_files = NULL;
  GhConversation *conversation = gh_conversation_store_lookup(alice->model, room);
  g_assert_true(gh_window_open_item(window, conversation));
  GhMlsGroupInfoDialog *info = show_info(window, conversation);
  g_assert_cmpstr(gh_mls_group_info_dialog_get_picture_status(info), ==,
                  "Groundhog can’t show or change the picture of this older kind of group.");
  g_assert_false(gh_mls_group_info_dialog_get_picture_shown(info));
  const gchar *picture_actions[] = { "mls-group.set-picture", "mls-group.show-picture",
                                      "mls-group.remove-picture" };
  for (guint i = 0; i < G_N_ELEMENTS(picture_actions); i++) {
    GtkWidget *button = button_for(GTK_WIDGET(info), picture_actions[i]);
    g_assert_nonnull(button);
    g_assert_false(gtk_widget_get_visible(button));
    /* Activated anyway (a shortcut, a stale button): nothing starts. */
    gtk_widget_activate_action(GTK_WIDGET(info), picture_actions[i], NULL);
    g_assert_cmpuint(gh_mls_group_info_dialog_get_pending(info), ==, 0);
  }
  drain();
  g_assert_cmpuint(blossom_fixture_requests(blossom)->len, ==, 0);
  adw_dialog_force_close(ADW_DIALOG(info));
  drain();

  /* A photo from Alice: Bob's card, no fetch until Download. */
  g_autoptr(GBytes) photo = tiny_png();
  FileSent sent = { 0 };
  gh_mls_attachments_send_async(files_a, ga, photo, "IMG_0001.png", "image/png", NULL, NULL,
                                on_file_sent, &sent);
  spin_until(file_sent, &sent, "the photo sent");
  g_assert_no_error(sent.error);
  GhWindow *bob_window = app_window(bob, NULL);
  gh_attachment_card_set_provider(GTK_WIDGET(bob_window), &files_provider, files_b, NULL);
  MessageWait listed = { bob, room, "" };
  spin_until(message_listed, &listed, "Bob's photo message");
  GhConversation *bob_conversation = gh_conversation_store_lookup(bob->model, room);
  g_assert_true(gh_window_open_item(bob_window, bob_conversation));
  spin_until(card_shown, view_of(bob_window), "the photo's card");
  GhAttachmentCard *card =
    GH_ATTACHMENT_CARD(find_type(GTK_WIDGET(view_of(bob_window)), GH_TYPE_ATTACHMENT_CARD, NULL));
  spin_until(card_has_transfer, card, "the card bound to its transfer");
  g_assert_true(g_str_has_prefix(gh_attachment_card_get_summary(card), "Photo"));
  g_assert_nonnull(strstr(gh_attachment_card_get_summary(card), "Not downloaded"));
  drain();
  g_assert_cmpuint(blossom_fixture_count(blossom, "GET"), ==, 0);
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(card), "attachment.download", NULL));
  spin_until(card_ready, card, "the photo downloaded");
  g_assert_cmpuint(blossom_fixture_count(blossom, "GET"), ==, 1);
  /* Shown inline: the card decoded it (GTK's own loaders) after the guard. */
  g_assert_true(GDK_IS_TEXTURE(gh_attachment_transfer_get_preview(
    gh_attachment_card_get_transfer(card))));

  g_clear_object(&sent.message);
  close_window(bob_window);
  close_window(window);
  g_object_unref(files_a);
  g_object_unref(files_b);
  gh_attachments_set_store(attachments_a, NULL);
  gh_attachments_set_store(attachments_b, NULL);
  g_object_unref(attachments_a);
  g_object_unref(attachments_b);
  drain();
  blossom_fixture_free(blossom);
  world_down(&w);
}

/* ---- --gui: members without the account proof (nostrc-6ukh, nostrc-prrl) ---------------- */

typedef struct {
  GhMlsGroup *group;
  guint key;
  GhMlsMemberIdentity want;
} MemberIs;

static gboolean
member_is(gpointer data)
{
  MemberIs *wait = data;
  g_auto(GStrv) members = gh_mls_group_dup_members(wait->group);
  return strv_has((const gchar *const *)members, hex[wait->key]) &&
         gh_mls_group_get_member_identity(wait->group, hex[wait->key], NULL) == wait->want;
}

static gboolean
reason_is(gpointer data)
{
  SubtitleWait *wait = data;
  return g_strcmp0(gh_conversation_view_get_unreadable_reason(view_of(wait->window)),
                   wait->text) == 0;
}

/* Bob requires proofs: Alice's Add of the stranger (an older app) is
 * refused, and Bob's conversation and Group Info say so, by its cause, not
 * that something is still on its way; off, it applies. Bob's Group Info
 * marks the stranger "Identity not verified" with who added him, and offers
 * Verify: confirmed first (it says what relays learn), it finds his
 * KeyPackage and the mark goes. Carol's KeyPackage is gone by the time Bob
 * verifies her: she stays marked, and the toast says so. Alice, who added
 * both from their KeyPackages, sees no mark and is offered no Verify. */
static void
test_gui_unverified_member(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  /* Members without the proof exist only in MDK 0.8-format groups: Alice
   * and Bob publish that format only here (nostrc-lf62). */
  world_legacy_only = TRUE;
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_published(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Older Apps", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  GhMlsGroup *gb = join(bob, ALICE);
  group_send_stub_reset();
  GhWindow *bob_window = app_window(bob, NULL);
  GhConversation *bob_conversation = gh_conversation_store_lookup(bob->model, room);
  g_assert_true(gh_window_open_item(bob_window, bob_conversation));

  /* Refused, honestly (nostrc-prrl). */
  g_settings_set_boolean(bob->settings, "only-join-verified-mls-groups", TRUE);
  g_autofree gchar *stranger_kp = inject_legacy_key_package(&w, STRANGER);
  accept_contact(alice, STRANGER);
  const gchar *stranger[] = { hex[STRANGER], NULL };
  OpWait added = { 0 };
  gh_mls_service_add_members_async(alice->service, ga, stranger, NULL, on_changed, &added);
  spin_until(op_done, &added, "Alice's Add of the stranger");
  g_assert_no_error(added.error);
  SubtitleWait refused = { bob_window, gh_mls_refused_copy(GH_MLS_REFUSAL_UNPROVEN) };
  spin_until(reason_is, &refused, "Bob's view saying the change was refused");
  g_assert_false(gh_conversation_view_get_decrypt_pending(view_of(bob_window)));
  GhMlsGroupInfoDialog *bob_info = show_info(bob_window, bob_conversation);
  g_assert_cmpstr(gh_mls_group_info_dialog_get_messages_status(bob_info), ==,
                  gh_mls_refused_copy(GH_MLS_REFUSAL_UNPROVEN));
  adw_dialog_force_close(ADW_DIALOG(bob_info));
  drain();
  g_settings_set_boolean(bob->settings, "only-join-verified-mls-groups", FALSE);
  SubtitleWait cleared = { bob_window, NULL };
  spin_until(reason_is, &cleared, "the refused change applying");
  MemberIs stranger_unverified = { gb, STRANGER, GH_MLS_MEMBER_UNVERIFIED };
  spin_until(member_is, &stranger_unverified, "the stranger listed, nothing looked up");

  /* Verify the stranger, from Group Info. */
  bob_info = show_info(bob_window, bob_conversation);
  g_assert_true(gh_mls_group_info_dialog_get_member_verifiable(bob_info, hex[STRANGER]));
  g_assert_false(gh_mls_group_info_dialog_get_member_verifiable(bob_info, hex[ALICE]));
  gtk_widget_activate_action(GTK_WIDGET(bob_info), "mls-group.verify", "s", hex[STRANGER]);
  AdwAlertDialog *ask = gh_mls_group_info_dialog_get_verify_dialog(bob_info);
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(ask), "Those relays can see whom you"));
  confirm(ask, GTK_WIDGET(bob_info), "verify-confirm");
  spin_until(nothing_pending, bob_info, "the Verify");
  g_assert_true(g_str_has_prefix(gh_mls_group_info_dialog_get_last_toast(bob_info),
                                 "Verified: "));
  g_assert_cmpstr(gh_mls_group_info_dialog_get_member_identity(bob_info, hex[STRANGER], NULL),
                  ==, "");
  g_assert_false(gh_mls_group_info_dialog_get_member_verifiable(bob_info, hex[STRANGER]));
  adw_dialog_force_close(ADW_DIALOG(bob_info));
  drain();

  /* Unverified: Carol's KeyPackage is gone before Bob reads the Add. */
  g_autofree gchar *carol_kp = inject_legacy_key_package(&w, CAROL);
  accept_contact(alice, CAROL);
  set_online(bob, FALSE);
  const gchar *carol[] = { hex[CAROL], NULL };
  OpWait added_carol = { 0 };
  gh_mls_service_add_members_async(alice->service, ga, carol, NULL, on_changed, &added_carol);
  spin_until(op_done, &added_carol, "Alice's Add of Carol");
  g_assert_no_error(added_carol.error);
  wire_relay_withhold(&w.w, carol_kp);
  set_online(bob, TRUE);
  MemberIs carol_unverified = { gb, CAROL, GH_MLS_MEMBER_UNVERIFIED };
  spin_until(member_is, &carol_unverified, "Carol unverified for Bob");
  bob_info = show_info(bob_window, bob_conversation);
  const gchar *explanation = NULL;
  g_assert_cmpstr(gh_mls_group_info_dialog_get_member_identity(bob_info, hex[CAROL],
                                                               &explanation), ==,
                  "Identity not verified");
  g_assert_true(g_str_has_prefix(explanation, "Added by "));
  g_assert_nonnull(strstr(explanation, "Groundhog couldn’t confirm this account owns this "
                                       "device."));
  gtk_widget_activate_action(GTK_WIDGET(bob_info), "mls-group.verify", "s", hex[CAROL]);
  confirm(gh_mls_group_info_dialog_get_verify_dialog(bob_info), GTK_WIDGET(bob_info),
          "verify-confirm");
  spin_until(nothing_pending, bob_info, "the Verify");
  g_assert_nonnull(strstr(gh_mls_group_info_dialog_get_last_toast(bob_info),
                          "stays unverified"));
  g_assert_cmpstr(gh_mls_group_info_dialog_get_member_identity(bob_info, hex[CAROL], NULL),
                  ==, "Identity not verified");
  g_assert_cmpstr(gh_mls_group_info_dialog_get_member_identity(bob_info, hex[STRANGER], NULL),
                  ==, "");
  g_assert_cmpstr(gh_mls_group_info_dialog_get_member_identity(bob_info, hex[ALICE], NULL),
                  ==, "");
  adw_dialog_force_close(ADW_DIALOG(bob_info));
  drain();

  GhWindow *window = app_window(alice, NULL);
  GhConversation *conversation = gh_conversation_store_lookup(alice->model, room);
  g_assert_true(gh_window_open_item(window, conversation));
  MemberIs carol_verified = { ga, CAROL, GH_MLS_MEMBER_VERIFIED };
  spin_until(member_is, &carol_verified, "Carol confirmed for Alice");
  GhMlsGroupInfoDialog *info = show_info(window, conversation);
  g_assert_cmpstr(gh_mls_group_info_dialog_get_member_identity(info, hex[CAROL], NULL), ==, "");
  g_assert_false(gh_mls_group_info_dialog_get_member_verifiable(info, hex[CAROL]));
  adw_dialog_force_close(ADW_DIALOG(info));
  drain();

  close_window(bob_window);
  close_window(window);
  world_down(&w);
}

/* ---- --gui: enrollment ------------------------------------------------------------------ */

#if GH_MLS_SERVICE_ACCOUNT_PROOF
static guint
held_proofs(GhTestSigner *signer)
{
  guint n = 0;
  for (guint i = 0; i < signer->held->len; i++) {
    GDBusMethodInvocation *call = g_ptr_array_index(signer->held, i);
    const gchar *input = NULL, *account = NULL, *app = NULL;
    if (!g_str_equal(g_dbus_method_invocation_get_method_name(call), "SignEvent"))
      continue;
    g_variant_get(g_dbus_method_invocation_get_parameters(call), "(&s&s&s)", &input, &account,
                  &app);
    NostrEvent *event = nostr_event_new();
    if (nostr_event_deserialize_compact(event, input, NULL) == 1 &&
        nostr_event_get_kind(event) == 450)
      n++;
    nostr_event_free(event);
  }
  return n;
}

static gboolean
a_proof_held(gpointer data)
{
  return held_proofs(data) > 0;
}

static const gchar *
identity_title(gpointer page)
{
  return gh_mls_new_group_page_get_identity_title(page);
}

static gboolean
identity_hidden(gpointer page)
{
  return gh_mls_new_group_page_get_identity_title(page) == NULL;
}

static void
test_gui_enrollment(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  wait_published(&w, keys, G_N_ELEMENTS(keys));

  /* A start whose approval the signer holds. */
  w.signer.hold = TRUE;
  app_restart(alice);
  accept_contact(alice, BOB);
  spin_until(a_proof_held, &w.signer, "the proof request");
  GhWindow *window = app_window(alice, NULL);
  GhMlsUiContext context = {
    .service = alice->service,
    .accounts = alice->accounts,
    .model = alice->model,
    .settings = alice->settings,
    .lookup_deadline = 20,
  };
  GhMlsNewGroupPage *page = gh_mls_new_group_page_new(&context);
  AdwNavigationView *navigation = ADW_NAVIGATION_VIEW(adw_navigation_view_new());
  adw_navigation_view_add(navigation, ADW_NAVIGATION_PAGE(page));
  AdwDialog *dialog = adw_dialog_new();
  adw_dialog_set_child(dialog, GTK_WIDGET(navigation));
  adw_dialog_present(dialog, GTK_WIDGET(window));
  spin_until(gh_test_dialog_shown, dialog, "the page shown");
  wait_text(identity_title, page, "Waiting for approval in your signer…");
  GtkWidget *retry = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(page),
                                                              GH_TYPE_MLS_NEW_GROUP_PAGE,
                                                              "retry_identity_button"));
  g_assert_false(gtk_widget_get_visible(retry));
  gh_mls_new_group_page_set_name(page, "Waiting");
  g_assert_cmpstr(create_reason(page), ==, "Approve this device in your signer first.");

  /* Declined: said so, with Try Again. */
  w.signer.deny = TRUE;
  gh_test_signer_release_all(&w.signer);
  wait_text(identity_title, page, "Declined in your signer");
  g_assert_true(gtk_widget_get_visible(retry));
  g_assert_cmpstr(create_reason(page), ==, "Approve this device in your signer first.");

  /* Try Again asks once more; approved, the row goes and Create only needs
   * people. */
  gtk_widget_activate_action(GTK_WIDGET(page), "mls-new.retry-identity", NULL);
  wait_text(identity_title, page, "Waiting for approval in your signer…");
  spin_until(a_proof_held, &w.signer, "the new proof request");
  w.signer.deny = FALSE;
  w.signer.hold = FALSE;
  gh_test_signer_release_all(&w.signer);
  spin_until(identity_hidden, page, "the approval");
  g_assert_cmpstr(create_reason(page), ==, "Choose at least one person.");

  adw_dialog_force_close(dialog);
  drain();
  close_window(window);
  world_down(&w);
}
#endif

static void
test_release_flag(void)
{
  /* The release build must expose the same MLS UI exercised by --gui. */
  g_assert_cmpint(GH_FEATURE_ENCRYPTED_GROUPS, ==, 1);
  g_assert_true(gh_mls_ui_enabled());
}

static gboolean
weak_row_gone(gpointer data)
{
  return *(gpointer *)data == NULL;
}

/* The release picker accepts the format it promises, caps even pasted or
 * programmatic selection, and keeps a cancelled check's row alive until its
 * completion callback has finished. */
static void
test_gui_picker_boundaries(void)
{
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  accept_contact(alice, BOB);
  accept_contact(alice, CAROL);
  GhMlsUiContext context = { .service = alice->service, .accounts = alice->accounts,
                             .model = alice->model, .settings = alice->settings,
                             .lookup_deadline = 1 };
  GhMlsInviteePicker *picker = g_object_ref_sink(g_object_new(GH_TYPE_MLS_INVITEE_PICKER, NULL));
  gh_mls_invitee_picker_setup(picker, &context, NULL);
  AdwEntryRow *entry = ADW_ENTRY_ROW(gtk_widget_get_template_child(
    GTK_WIDGET(picker), GH_TYPE_MLS_INVITEE_PICKER, "add_entry"));
  gtk_editable_set_text(GTK_EDITABLE(entry), hex[BOB]);
  g_signal_emit_by_name(entry, "apply");
  g_assert_cmpuint(gh_mls_invitee_picker_get_n_selected(picker), ==, 1);
  g_assert_nonnull(gh_mls_invitee_picker_get_row(picker, hex[BOB]));

  for (guint i = 0; i < GH_MLS_SERVICE_MAX_INVITEES - 1; i++) {
    g_autofree gchar *pubkey = g_strdup_printf("%064x", i + 100);
    gtk_editable_set_text(GTK_EDITABLE(entry), pubkey);
    g_signal_emit_by_name(entry, "apply");
  }
  g_assert_cmpuint(gh_mls_invitee_picker_get_n_selected(picker), ==,
                   GH_MLS_SERVICE_MAX_INVITEES);
  g_assert_false(gh_mls_invitee_picker_set_selected(picker, hex[CAROL], TRUE));
  g_assert_cmpuint(gh_mls_invitee_picker_get_n_selected(picker), ==,
                   GH_MLS_SERVICE_MAX_INVITEES);
  gtk_editable_set_text(GTK_EDITABLE(entry), hex[STRANGER]);
  g_signal_emit_by_name(entry, "apply");
  g_assert_null(gh_mls_invitee_picker_get_row(picker, hex[STRANGER]));
  g_assert_cmpuint(gh_mls_invitee_picker_get_n_selected(picker), ==,
                   GH_MLS_SERVICE_MAX_INVITEES);

  /* Force a new check, then replace rows before its callback is dispatched. */
  g_assert_true(gh_mls_invitee_picker_set_selected(picker, hex[BOB], FALSE));
  g_assert_true(gh_mls_invitee_picker_set_selected(picker, hex[BOB], TRUE));
  gpointer weak_row = gh_mls_invitee_picker_get_row(picker, hex[BOB]);
  g_object_add_weak_pointer(G_OBJECT(weak_row), &weak_row);
  gh_mls_invitee_picker_setup(picker, &context, NULL);
  g_assert_nonnull(weak_row);
  spin_until(weak_row_gone, &weak_row, "cancelled picker row finalized");
  g_object_unref(picker);
  world_down(&w);
}

/* ---- main ------------------------------------------------------------------------------- */

int
main(int argc, char **argv)
{
  gboolean gui_mode = argc > 1 && g_str_equal(argv[1], "--gui");
  if (gui_mode) {
    argv[1] = argv[0];
    argv++;
    argc--;
  }
#ifdef __APPLE__
  /* As test_group_ui.c: GTK's macOS accessibility backend has no announce. */
  g_setenv("GTK_A11Y", "none", FALSE);
#endif
  int status;
  if (gui_mode) {
    /* Before g_test_init(), as the other GUI tests. */
    if (!gtk_init_check()) {
      g_printerr("groundhog-mls-ui GUI test skipped: no graphical display\n");
      return 77;
    }
    adw_init();
    groundhog_register_resource();
    /* The app's stylesheet, as main.c loads it, so screenshots show the app. */
    g_autoptr(GtkCssProvider) css = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(css, "/org/nostr/Groundhog/style.css");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
    g_test_init(&argc, &argv, NULL);
    nostrc_test_tolerate_gdk_frame_warning();
    /* mls_world_init() beside GTK: GTK keeps the session bus it was given. */
    g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
    g_log_set_fatal_mask(NULL, G_LOG_FATAL_MASK | G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL);
    for (guint key = 1; key < GH_TEST_KEYS; key++) {
      hex[key] = gh_test_pub(key);
      npub[key] = gh_test_npub(key);
    }
    gh_test_bus_up_beside_gtk(&test_bus);
    g_test_add_func("/groundhog/mls-ui-gui/flag-new-group", test_gui_flag_new_group);
    g_test_add_func("/groundhog/mls-ui-gui/picker-boundaries", test_gui_picker_boundaries);
    g_test_add_func("/groundhog/mls-ui-gui/new-group", test_gui_new_group);
#if GH_MLS_ADOPTED_KEY_PACKAGES
    g_test_add_func("/groundhog/mls-ui-gui/new-group-formats", test_gui_new_group_formats);
    g_test_add_func("/groundhog/mls-ui-gui/new-group-format-changed",
                    test_gui_new_group_format_changed);
#endif
    g_test_add_func("/groundhog/mls-ui-gui/invites-panel", test_gui_invites_panel);
    g_test_add_func("/groundhog/mls-ui-gui/refused-invitation", test_gui_refused_invitation);
    g_test_add_func("/groundhog/mls-ui-gui/remove-old-keys", test_gui_remove_old_keys);
    g_test_add_func("/groundhog/mls-ui-gui/group-info", test_gui_group_info);
    g_test_add_func("/groundhog/mls-ui-gui/left-group-delete", test_gui_left_group_delete);
    g_test_add_func("/groundhog/mls-ui-gui/group-files", test_gui_group_files);
#if GH_MLS_SERVICE_ACCOUNT_PROOF
    g_test_add_func("/groundhog/mls-ui-gui/unverified-member", test_gui_unverified_member);
#endif
#if GH_MLS_SERVICE_ACCOUNT_PROOF
    g_test_add_func("/groundhog/mls-ui-gui/enrollment", test_gui_enrollment);
#endif
  } else {
    g_test_init(&argc, &argv, NULL);
    mls_world_init();
    g_test_add_func("/groundhog/mls-ui/release-flag", test_release_flag);
    g_test_add_func("/groundhog/mls-ui/copy", test_copy);
    g_test_add_func("/groundhog/mls-ui/view-model", test_view_model);
    g_test_add_func("/groundhog/mls-ui/dm-shape", test_dm_shape);
  }
  status = g_test_run();
  mls_world_finish();
  return status;
}
