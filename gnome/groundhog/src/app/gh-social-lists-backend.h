#ifndef GH_SOCIAL_LISTS_BACKEND_H
#define GH_SOCIAL_LISTS_BACKEND_H
#include "gh-social-lists.h"
#include "gh-lists-page.h"
/* The page/dialog copies this table and references owner. */
GhListsBackend gh_social_lists_backend(GhSocialLists *self);
#endif
