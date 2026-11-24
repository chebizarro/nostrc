/* SPDX-License-Identifier: MIT */
#ifndef SIGNET_SESSION_BROKER_INTERNAL_H
#define SIGNET_SESSION_BROKER_INTERNAL_H

#include <gio/gio.h>
#include "signet/credential_access.h"
#include "signet/session_broker.h"

typedef enum {
  SIGNET_SESSION_OK,
  SIGNET_SESSION_ACCESS_FAILED,
  SIGNET_SESSION_EXCHANGE_FAILED,
  SIGNET_SESSION_STORE_FAILED,
} SignetSessionStatus;

SignetCredAccessStatus signet_session_broker_prepare(
    const SignetCredentialAccessContext *ctx, const SignetSessionRequest *req,
    const char *transport, SignetCredentialAccessGrant *grant);

/* Consumes the prepared grant, even on failure or cancellation. */
SignetSessionStatus signet_session_broker_exchange(
    struct SignetStore *store, const char *agent_id, const char *transport,
    SignetCredentialAccessGrant *grant, GCancellable *cancel,
    SignetSessionResult *result);

#endif
