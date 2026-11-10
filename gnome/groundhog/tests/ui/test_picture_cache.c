/* GhPictureCache policy (plan §3.6, owner decision D2; charter PD-2 as
 * amended): one cache per window decides, for every surface, whether a
 * contact's picture may be shown:
 *
 *   master_enabled AND NOT hidden AND (own OR followed OR explicit_consent)
 *
 * Harnesses: a recording HTTP transport whose answers the test releases
 * (H1-style), a fake consent backend (what the account's store keeps), a
 * fake policy (the follow list and the Stage 6 hide seam), and the real
 * GSettings key in the memory backend. No display, no network. Waits
 * iterate the main context; nothing sleeps. */
#include "gh-picture-cache.h"

#include <string.h>

#define URI_A "https://pictures.test.invalid/a.png"
#define URI_B "https://pictures.test.invalid/b.png"

static const gchar *const A = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const gchar *const B = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static const gchar *const C = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
static const gchar *const D = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

static void
drain(void)
{
  while (g_main_context_iteration(NULL, FALSE))
    ;
}

/* ---- recording HTTP transport: nothing answers until the test says so ----------- */

typedef struct {
  GPtrArray *tasks; /* GTask per GET, in order */
} Http;

static void
http_get(gpointer data, const gchar *uri, gsize max_bytes, GCancellable *cancellable,
         GAsyncReadyCallback callback, gpointer user_data)
{
  (void)uri; (void)max_bytes;
  Http *http = data;
  g_ptr_array_add(http->tasks, g_task_new(NULL, cancellable, callback, user_data));
}

static GBytes *
http_finish(gpointer data, GAsyncResult *result, GError **error)
{
  (void)data;
  return g_task_propagate_pointer(G_TASK(result), error);
}

static const GhHttpTransport http_transport = { http_get, http_finish };

/* How many GETs so far (not "gets": libc's). */
static guint
get_count(Http *http)
{
  return http->tasks->len;
}

static gboolean
get_cancelled(Http *http, guint index)
{
  g_assert_cmpuint(index, <, http->tasks->len);
  return g_cancellable_is_cancelled(g_task_get_cancellable(g_ptr_array_index(http->tasks, index)));
}

/* The index-th GET completes with png (a cancelled one as cancelled). */
static void
http_answer(Http *http, guint index, GBytes *png)
{
  g_assert_cmpuint(index, <, http->tasks->len);
  GTask *task = g_ptr_array_index(http->tasks, index);
  g_assert_false(g_task_get_completed(task));
  if (!g_task_return_error_if_cancelled(task))
    g_task_return_pointer(task, g_bytes_ref(png), (GDestroyNotify)g_bytes_unref);
  drain();
}

static GBytes *
one_pixel_png(void)
{
  guint8 pixel[] = { 255, 0, 0, 255 };
  g_autoptr(GBytes) pixels = g_bytes_new(pixel, sizeof pixel);
  g_autoptr(GdkTexture) texture =
    GDK_TEXTURE(gdk_memory_texture_new(1, 1, GDK_MEMORY_R8G8B8A8, pixels, 4));
  return gdk_texture_save_to_png_bytes(texture);
}

/* ---- the account's consent (what the encrypted store keeps) --------------------- */

typedef struct {
  GPtrArray *allowed; /* pubkeys */
  guint clears;
} Consent;

static GStrv
consent_list(gpointer data, GError **error)
{
  (void)error;
  Consent *consent = data;
  GStrv keys = g_new0(gchar *, consent->allowed->len + 1);
  for (guint i = 0; i < consent->allowed->len; i++)
    keys[i] = g_strdup(g_ptr_array_index(consent->allowed, i));
  return keys;
}

static gboolean
consent_set(gpointer data, const gchar *pubkey, gint64 allowed_at, GError **error)
{
  (void)error;
  Consent *consent = data;
  guint index;
  gboolean listed = g_ptr_array_find_with_equal_func(consent->allowed, pubkey, g_str_equal, &index);
  if (allowed_at && !listed)
    g_ptr_array_add(consent->allowed, g_strdup(pubkey));
  else if (!allowed_at && listed)
    g_ptr_array_remove_index(consent->allowed, index);
  return TRUE;
}

static gboolean
consent_clear(gpointer data, GError **error)
{
  (void)error;
  Consent *consent = data;
  g_ptr_array_set_size(consent->allowed, 0);
  consent->clears++;
  return TRUE;
}

static const GhPictureConsentBackend consent_backend = { consent_list, consent_set, consent_clear };

/* ---- the account's policy: followed (NIP-02) and hidden (Stage 6) ------------------- */

typedef struct {
  GHashTable *followed;
  GHashTable *hidden;
} Policy;

static gboolean
policy_followed(gpointer data, const gchar *pubkey)
{
  return g_hash_table_contains(((Policy *)data)->followed, pubkey);
}

static gboolean
policy_hidden(gpointer data, const gchar *pubkey)
{
  return g_hash_table_contains(((Policy *)data)->hidden, pubkey);
}

static const GhPicturePolicy policy_backend = { policy_followed, policy_hidden };

/* ---- fixture ---------------------------------------------------------------------- */

typedef struct {
  GSettings *settings;
  Http http;
  Consent consent;
  Policy policy;
  GhWebContent *web;
  GPtrArray *changed;  /* "picture-changed" pubkeys, every cache */
  guint policy_changes; /* "policy-changed", every cache */
} Fixture;

static void
on_picture_changed(GhPictureCache *cache, const gchar *pubkey, gpointer data)
{
  (void)cache;
  g_ptr_array_add(((Fixture *)data)->changed, g_strdup(pubkey));
}

static void
on_policy_changed(GhPictureCache *cache, gpointer data)
{
  (void)cache;
  ((Fixture *)data)->policy_changes++;
}

static guint
changed_count(Fixture *f, const gchar *pubkey)
{
  guint n = 0;
  for (guint i = 0; i < f->changed->len; i++)
    n += g_str_equal(g_ptr_array_index(f->changed, i), pubkey);
  return n;
}

static void
fixture_up(Fixture *f, gboolean master)
{
  memset(f, 0, sizeof *f);
  f->settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_boolean(f->settings, "load-profile-pictures", master);
  f->http.tasks = g_ptr_array_new_with_free_func(g_object_unref);
  f->consent.allowed = g_ptr_array_new_with_free_func(g_free);
  f->policy.followed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  f->policy.hidden = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  f->web = gh_web_content_new(f->settings, &http_transport, &f->http);
  f->changed = g_ptr_array_new_with_free_func(g_free);
}

/* A window's cache, as gh_conversation_list_attach() and the app bind it. */
static GhPictureCache *
cache_new(Fixture *f)
{
  GhPictureCache *cache = gh_picture_cache_new(f->web);
  g_signal_connect(cache, "picture-changed", G_CALLBACK(on_picture_changed), f);
  g_signal_connect(cache, "policy-changed", G_CALLBACK(on_policy_changed), f);
  gh_picture_cache_bind_settings(cache, f->settings);
  gh_picture_cache_set_consent(cache, &consent_backend, &f->consent);
  gh_picture_cache_set_policy(cache, &policy_backend, &f->policy);
  return cache;
}

/* The caches of a test are still alive here (g_autoptr in the test body):
 * the key is reset first, while what their handlers touch still exists. */
static void
fixture_down(Fixture *f)
{
  g_settings_reset(f->settings, "load-profile-pictures");
  drain();
  /* A GET nobody answered ends as cancelled: the cache ignores it (its
   * entry is gone), and GTask insists on an answer. */
  for (guint i = 0; i < f->http.tasks->len; i++) {
    GTask *task = g_ptr_array_index(f->http.tasks, i);
    if (!g_task_get_completed(task))
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "test over");
  }
  drain();
  g_clear_object(&f->web);
  g_ptr_array_unref(f->http.tasks);
  g_ptr_array_unref(f->consent.allowed);
  g_hash_table_unref(f->policy.followed);
  g_hash_table_unref(f->policy.hidden);
  g_ptr_array_unref(f->changed);
  g_object_unref(f->settings);
}

static void
set_master(Fixture *f, gboolean on)
{
  g_settings_set_boolean(f->settings, "load-profile-pictures", on);
  drain();
}

/* ---- /privacy/avatars/master-off-all-surfaces ------------------------------------- */

/* Master off at startup, with a persisted consent, an own identity and a
 * followed account: nothing is eligible on any surface and nothing is
 * fetched. On: all three are; a load starts. Off again: the load is
 * cancelled, the consent cleared, everyone told. On once more: own and
 * followed are eligible again, the formerly consented account is not. */
static void
test_master_off_all_surfaces(void)
{
  Fixture f;
  fixture_up(&f, FALSE);
  g_ptr_array_add(f.consent.allowed, g_strdup(A)); /* persisted from an earlier session */
  g_hash_table_add(f.policy.followed, g_strdup(C));
  g_autoptr(GhPictureCache) cache = cache_new(&f);
  const gchar *own[] = { B, NULL };
  gh_picture_cache_set_own(cache, own);
  g_assert_false(gh_picture_cache_get_master(cache));
  g_assert_true(gh_picture_cache_has_consent(cache, A)); /* kept, unused */
  g_assert_false(gh_picture_cache_is_allowed(cache, A));
  g_assert_false(gh_picture_cache_is_allowed(cache, B));
  g_assert_false(gh_picture_cache_is_allowed(cache, C));
  g_assert_false(gh_picture_cache_is_followed(cache, C));
  g_assert_null(gh_picture_cache_get(cache, A, URI_A));
  g_assert_null(gh_picture_cache_get(cache, B, URI_B));
  g_assert_null(gh_picture_cache_get(cache, C, URI_A));
  drain();
  g_assert_cmpuint(get_count(&f.http), ==, 0);
  g_assert_cmpuint(f.consent.clears, ==, 0);

  set_master(&f, TRUE);
  g_assert_true(gh_picture_cache_is_allowed(cache, A));
  g_assert_true(gh_picture_cache_is_allowed(cache, B));
  g_assert_true(gh_picture_cache_is_allowed(cache, C));
  g_assert_true(gh_picture_cache_is_followed(cache, C));
  g_assert_false(gh_picture_cache_is_allowed(cache, D));
  g_assert_null(gh_picture_cache_get(cache, A, URI_A)); /* a load starts */
  g_assert_cmpuint(get_count(&f.http), ==, 1);
  g_assert_true(gh_picture_cache_is_loading(cache, URI_A));

  g_ptr_array_set_size(f.changed, 0);
  set_master(&f, FALSE);
  g_assert_true(get_cancelled(&f.http, 0));
  g_assert_cmpuint(f.consent.clears, ==, 1);
  g_assert_false(gh_picture_cache_has_consent(cache, A));
  g_assert_false(gh_picture_cache_is_allowed(cache, A));
  g_assert_false(gh_picture_cache_is_allowed(cache, B));
  g_assert_false(gh_picture_cache_is_allowed(cache, C));
  g_assert_false(gh_picture_cache_is_loading(cache, URI_A));
  g_assert_cmpuint(changed_count(&f, A), ==, 1);
  g_assert_cmpuint(changed_count(&f, B), ==, 1);
  /* The cancelled answer, arriving late, restores nothing. */
  g_autoptr(GBytes) png = one_pixel_png();
  http_answer(&f.http, 0, png);
  g_assert_null(gh_picture_cache_get(cache, A, URI_A));
  g_assert_cmpuint(get_count(&f.http), ==, 1);

  set_master(&f, TRUE);
  g_assert_false(gh_picture_cache_is_allowed(cache, A)); /* fresh consent needed */
  g_assert_true(gh_picture_cache_is_allowed(cache, B));
  g_assert_true(gh_picture_cache_is_allowed(cache, C));
  fixture_down(&f);
}

/* ---- follow removal and explicit consent ------------------------------------------- */

/* Unfollowing revokes follow-derived eligibility at once: the load in flight
 * for that account is dropped and its late answer ignored; an explicit
 * consent still permits loading while the master stays on; a hidden account
 * is ineligible whatever else applies. Nothing here writes consent for a
 * followed or own account. */
static void
test_follow_removal(void)
{
  Fixture f;
  fixture_up(&f, TRUE);
  g_hash_table_add(f.policy.followed, g_strdup(C));
  g_hash_table_add(f.policy.followed, g_strdup(D));
  g_autoptr(GhPictureCache) cache = cache_new(&f);
  gh_picture_cache_allow(cache, D); /* explicit, besides being followed */
  g_assert_cmpuint(f.consent.allowed->len, ==, 1);
  g_assert_null(gh_picture_cache_get(cache, C, URI_A));
  g_assert_null(gh_picture_cache_get(cache, D, URI_B));
  g_assert_cmpuint(get_count(&f.http), ==, 2);

  g_hash_table_remove_all(f.policy.followed);
  g_ptr_array_set_size(f.changed, 0);
  gh_picture_cache_policy_changed(cache, NULL);
  g_assert_false(gh_picture_cache_is_allowed(cache, C));
  g_assert_true(gh_picture_cache_is_allowed(cache, D)); /* consent remains */
  g_assert_true(get_cancelled(&f.http, 0));
  g_assert_false(get_cancelled(&f.http, 1));
  g_assert_cmpuint(changed_count(&f, C), ==, 1);
  g_assert_cmpuint(changed_count(&f, D), ==, 0);
  g_autoptr(GBytes) png = one_pixel_png();
  http_answer(&f.http, 0, png);
  g_assert_null(gh_picture_cache_get(cache, C, URI_A)); /* not eligible: no texture, no load */
  g_assert_cmpuint(get_count(&f.http), ==, 2);
  http_answer(&f.http, 1, png);
  g_assert_nonnull(gh_picture_cache_get(cache, D, URI_B));
  g_assert_cmpuint(changed_count(&f, D), ==, 1);
  /* Following never wrote a consent: only D's explicit allow is stored. */
  g_assert_cmpuint(f.consent.allowed->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(f.consent.allowed, 0), ==, D);

  /* Hidden (the Stage 6 seam) beats following, consent and own. */
  g_hash_table_add(f.policy.followed, g_strdup(C));
  g_hash_table_add(f.policy.hidden, g_strdup(C));
  g_hash_table_add(f.policy.hidden, g_strdup(D));
  gh_picture_cache_policy_changed(cache, NULL);
  g_assert_false(gh_picture_cache_is_allowed(cache, C));
  g_assert_false(gh_picture_cache_is_followed(cache, C));
  g_assert_false(gh_picture_cache_is_allowed(cache, D));
  g_assert_null(gh_picture_cache_get(cache, D, URI_B)); /* dropped with its texture */
  g_assert_cmpuint(get_count(&f.http), ==, 2);
  fixture_down(&f);
}

/* ---- same-URI stale completion ----------------------------------------------------- */

/* A load that started before a rebinding (another account's consent, or a
 * revocation) never completes the entry created for the same URI
 * afterwards: only the exact entry that started a load takes its answer. */
static void
test_same_uri_stale_completion(void)
{
  Fixture f;
  fixture_up(&f, TRUE);
  g_autoptr(GhPictureCache) cache = cache_new(&f);
  gh_picture_cache_allow(cache, A);
  g_assert_null(gh_picture_cache_get(cache, A, URI_A));
  g_assert_cmpuint(get_count(&f.http), ==, 1);
  /* The account changes and comes back (the store rebinds its consent). */
  Consent other = { g_ptr_array_new_with_free_func(g_free), 0 };
  gh_picture_cache_set_consent(cache, &consent_backend, &other);
  g_assert_true(get_cancelled(&f.http, 0));
  gh_picture_cache_set_consent(cache, &consent_backend, &f.consent);
  g_assert_true(gh_picture_cache_is_allowed(cache, A)); /* the stored consent, reloaded */
  g_assert_null(gh_picture_cache_get(cache, A, URI_A)); /* a new entry, a new load */
  g_assert_cmpuint(get_count(&f.http), ==, 2);
  g_ptr_array_set_size(f.changed, 0);
  g_autoptr(GBytes) png = one_pixel_png();
  http_answer(&f.http, 0, png); /* the old load's answer: not this entry's */
  g_assert_true(gh_picture_cache_is_loading(cache, URI_A));
  g_assert_null(gh_picture_cache_get(cache, A, URI_A));
  g_assert_cmpuint(changed_count(&f, A), ==, 0);
  http_answer(&f.http, 1, png);
  g_assert_false(gh_picture_cache_is_loading(cache, URI_A));
  g_assert_nonnull(gh_picture_cache_get(cache, A, URI_A));
  g_assert_cmpuint(changed_count(&f, A), ==, 1);
  g_assert_cmpuint(get_count(&f.http), ==, 2);
  g_ptr_array_unref(other.allowed);
  fixture_down(&f);
}

/* ---- multi-window revocation ------------------------------------------------------- */

/* Two windows, two caches on the same preference and consent: turning the
 * switch off revokes both at once, from the preference itself, with no
 * view in between; each says so to its surfaces. */
static void
test_multi_window_revocation(void)
{
  Fixture f;
  fixture_up(&f, TRUE);
  g_hash_table_add(f.policy.followed, g_strdup(C));
  g_autoptr(GhPictureCache) one = cache_new(&f);
  g_autoptr(GhPictureCache) two = cache_new(&f);
  gh_picture_cache_allow(one, A);
  gh_picture_cache_allow(two, A);
  g_assert_null(gh_picture_cache_get(one, A, URI_A));
  g_assert_null(gh_picture_cache_get(two, C, URI_B));
  g_assert_cmpuint(get_count(&f.http), ==, 2);
  g_ptr_array_set_size(f.changed, 0);
  f.policy_changes = 0;
  set_master(&f, FALSE);
  g_assert_false(gh_picture_cache_is_allowed(one, A));
  g_assert_false(gh_picture_cache_is_allowed(two, A));
  g_assert_false(gh_picture_cache_is_allowed(two, C));
  g_assert_true(get_cancelled(&f.http, 0));
  g_assert_true(get_cancelled(&f.http, 1));
  g_assert_cmpuint(f.consent.allowed->len, ==, 0);
  g_assert_cmpuint(changed_count(&f, A), ==, 2); /* once per window */
  g_assert_cmpuint(changed_count(&f, C), ==, 1);
  g_assert_cmpuint(f.policy_changes, ==, 2);
  /* The view's own revocation (gh_conversation_view) is harmless on top. */
  gh_picture_cache_revoke_all(one);
  g_assert_cmpuint(get_count(&f.http), ==, 2);
  fixture_down(&f);
}

/* ---- own identities --------------------------------------------------------------- */

/* The account switcher feeds the own identities: eligible under the master
 * switch, never through consent; replacing the set drops the old ones. */
static void
test_own_identities(void)
{
  Fixture f;
  fixture_up(&f, TRUE);
  g_autoptr(GhPictureCache) cache = cache_new(&f);
  const gchar *own[] = { A, B, NULL };
  gh_picture_cache_set_own(cache, own);
  g_assert_true(gh_picture_cache_is_allowed(cache, A));
  g_assert_true(gh_picture_cache_is_allowed(cache, B));
  g_assert_false(gh_picture_cache_has_consent(cache, A));
  g_assert_cmpuint(f.consent.allowed->len, ==, 0);
  g_assert_null(gh_picture_cache_get(cache, A, URI_A));
  g_assert_cmpuint(get_count(&f.http), ==, 1);
  f.policy_changes = 0;
  gh_picture_cache_set_own(cache, own); /* the same set again: nothing happens */
  g_assert_cmpuint(f.policy_changes, ==, 0);
  const gchar *fewer[] = { B, NULL };
  gh_picture_cache_set_own(cache, fewer);
  g_assert_false(gh_picture_cache_is_allowed(cache, A));
  g_assert_true(get_cancelled(&f.http, 0));
  g_assert_cmpuint(f.policy_changes, ==, 1);
  fixture_down(&f);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/privacy/avatars/master-off-all-surfaces", test_master_off_all_surfaces);
  g_test_add_func("/groundhog/picture-cache/follow-removal", test_follow_removal);
  g_test_add_func("/groundhog/picture-cache/same-uri-stale-completion",
                  test_same_uri_stale_completion);
  g_test_add_func("/groundhog/picture-cache/multi-window-revocation",
                  test_multi_window_revocation);
  g_test_add_func("/groundhog/picture-cache/own-identities", test_own_identities);
  return g_test_run();
}
