#ifndef GH_DRAFT_SYNC_BACKEND_H
#define GH_DRAFT_SYNC_BACKEND_H
#include "gh-draft-sync.h"
#include "gh-drafts-page.h"
/* The Preferences page/dialog copies this table and references owner. */
GhDraftsBackend gh_draft_sync_backend(GhDraftSync *self);
#endif
