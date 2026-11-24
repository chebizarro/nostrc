/* SPDX-License-Identifier: MIT */
#ifndef SIGNET_SESSION_BROKER_INTERNAL_H
#define SIGNET_SESSION_BROKER_INTERNAL_H

#include "signet/credential_access.h"
#include "signet/session_broker.h"

typedef enum {
  SIGNET_SESSION_OK,
  SIGNET_SESSION_ACCESS_FAILED,
  SIGNET_SESSION_EXCHANGE_FAILED,
  SIGNET_SESSION_STORE_FAILED,
} SignetSessionStatus;

SignetSessionStatus signet_session_broker_get_for_transport(
    const SignetCredentialAccessContext *ctx, const SignetSessionRequest *req,
    const char *transport, SignetSessionResult *result,
    SignetCredAccessStatus *access_status);

#endif
