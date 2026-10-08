#pragma once

#include "gh-account-store.h"
#include "gh-mls-service.h"
#include "gh-nip29-service.h"
#include "gh-window.h"

G_BEGIN_DECLS

#define GH_POLL_UI_DATA "groundhog-poll-ui"

typedef struct {
  GhAccountStore *account_store; /* borrowed; current account's outbox */
  GhMlsService *(*mls_service)(gpointer data); /* nullable */
  GhNip29Service *(*nip29_service)(gpointer data); /* nullable */
  gpointer service_data;
} GhPollUiConfig;

/* One poll projection and composer/dialog handler for every transport. */
void gh_poll_ui_attach(GhWindow *window, const GhPollUiConfig *config);

G_END_DECLS
