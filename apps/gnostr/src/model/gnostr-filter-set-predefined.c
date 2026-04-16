/* gnostr-filter-set-predefined.c — list-driven predefined filter sets.
 *
 * SPDX-License-Identifier: MIT
 *
 * This file holds the pure builders and the manager install path. These
 * have no dependency on process-wide singletons, which makes them easy
 * to unit-test (see tests/test_filter_set_predefined.c).
 *
 * The refresh_from_services() entry point that reads the live follow /
 * bookmark / mute-list singletons lives in
 * gnostr-filter-set-predefined-services.c.
 *
 * nostrc-yg8j.3.
 */

#define G_LOG_DOMAIN "gnostr-filter-set-predefined"

#include "gnostr-filter-set-predefined.h"

#include <string.h>

/* ------------------------------------------------------------------------
 * Spec for each list-driven predefined set
 * ------------------------------------------------------------------------ */

typedef struct {
  const gchar *id;
  const gchar *name;
  const gchar *description;
  const gchar *icon;
  const gchar *color;
  const gint   kinds[3];
  gsize        n_kinds;
} PredefSpec;

static const PredefSpec spec_following = {
  .id          = GNOSTR_FILTER_SET_ID_FOLLOWING,
  .name        = "Following",
  .description = "Notes from accounts you follow (NIP-02).",
  .icon        = "people-symbolic",
  .color       = "#33d17a",
  .kinds       = { 1, 6 },
  .n_kinds     = 2,
};

static const PredefSpec spec_bookmarks = {
  .id          = GNOSTR_FILTER_SET_ID_BOOKMARKS,
  .name        = "Bookmarks",
  .description = "Notes you have bookmarked (NIP-51).",
  .icon        = "starred-symbolic",
  .color       = "#f6d32d",
  .kinds       = { 0 },
  .n_kinds     = 0, /* id-based, no kind restriction */
};

static const PredefSpec spec_muted_inverse = {
  .id          = GNOSTR_FILTER_SET_ID_MUTED_INVERSE,
  .name        = "Muted (inverse)",
  .description = "Global feed with muted authors filtered out client-side.",
  .icon        = "action-unavailable-symbolic",
  .color       = "#c01c28",
  .kinds       = { 1 },
  .n_kinds     = 1,
};

static void
apply_spec_metadata(GnostrFilterSet *fs, const PredefSpec *spec)
{
  gnostr_filter_set_set_id         (fs, spec->id);
  gnostr_filter_set_set_name       (fs, spec->name);
  gnostr_filter_set_set_description(fs, spec->description);
  gnostr_filter_set_set_icon       (fs, spec->icon);
  gnostr_filter_set_set_color      (fs, spec->color);
  gnostr_filter_set_set_source     (fs, GNOSTR_FILTER_SET_SOURCE_PREDEFINED);
  if (spec->n_kinds > 0)
    gnostr_filter_set_set_kinds(fs, spec->kinds, spec->n_kinds);
}

/* ------------------------------------------------------------------------
 * Pure builders
 * ------------------------------------------------------------------------ */

GnostrFilterSet *
gnostr_filter_set_predefined_build_following(const gchar * const *follow_pubkeys)
{
  GnostrFilterSet *fs = gnostr_filter_set_new();
  apply_spec_metadata(fs, &spec_following);
  gnostr_filter_set_set_authors(fs, follow_pubkeys);
  return fs;
}

GnostrFilterSet *
gnostr_filter_set_predefined_build_bookmarks(const gchar * const *event_ids)
{
  GnostrFilterSet *fs = gnostr_filter_set_new();
  apply_spec_metadata(fs, &spec_bookmarks);
  gnostr_filter_set_set_ids(fs, event_ids);
  return fs;
}

GnostrFilterSet *
gnostr_filter_set_predefined_build_muted_inverse(const gchar * const *muted_pubkeys)
{
  GnostrFilterSet *fs = gnostr_filter_set_new();
  apply_spec_metadata(fs, &spec_muted_inverse);
  gnostr_filter_set_set_excluded_authors(fs, muted_pubkeys);
  return fs;
}

/* ------------------------------------------------------------------------
 * Manager wiring
 * ------------------------------------------------------------------------ */

/* Install-or-update one predefined set. The add path needs a plain
 * append; the update path uses manager_update (which clones internally)
 * so we still free our local reference afterwards. */
static void
install_or_update(GnostrFilterSetManager *manager, GnostrFilterSet *fs)
{
  const gchar *id = gnostr_filter_set_get_id(fs);
  if (gnostr_filter_set_manager_contains(manager, id)) {
    gnostr_filter_set_manager_update(manager, fs);
  } else {
    gnostr_filter_set_manager_add(manager, fs);
  }
}

void
gnostr_filter_set_predefined_install(GnostrFilterSetManager *manager,
                                     const gchar * const *follow_pubkeys,
                                     const gchar * const *bookmark_ids,
                                     const gchar * const *muted_pubkeys)
{
  g_return_if_fail(GNOSTR_IS_FILTER_SET_MANAGER(manager));

  g_autoptr(GnostrFilterSet) following =
      gnostr_filter_set_predefined_build_following(follow_pubkeys);
  g_autoptr(GnostrFilterSet) bookmarks =
      gnostr_filter_set_predefined_build_bookmarks(bookmark_ids);
  g_autoptr(GnostrFilterSet) muted_inverse =
      gnostr_filter_set_predefined_build_muted_inverse(muted_pubkeys);

  install_or_update(manager, following);
  install_or_update(manager, bookmarks);
  install_or_update(manager, muted_inverse);
}
