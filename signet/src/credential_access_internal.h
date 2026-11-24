/* SPDX-License-Identifier: MIT */
#ifndef SIGNET_CREDENTIAL_ACCESS_INTERNAL_H
#define SIGNET_CREDENTIAL_ACCESS_INTERNAL_H

#include "signet/credential_access.h"

/* Authorize a credential operation before target lookup or lease issuance.
 * The caller audits the returned decision. */
SignetCredAccessStatus signet_credential_access_authorize(
    const SignetCredentialAccessContext *ctx,
    const SignetCredentialAccessRequest *req, int64_t now);

#endif
