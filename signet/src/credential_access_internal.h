/* SPDX-License-Identifier: MIT */
#ifndef SIGNET_CREDENTIAL_ACCESS_INTERNAL_H
#define SIGNET_CREDENTIAL_ACCESS_INTERNAL_H

#include "signet/credential_access.h"

/* Authorize a credential operation before target lookup or lease issuance.
 * The caller audits the returned decision. */
SignetCredAccessStatus signet_credential_access_authorize(
    const SignetCredentialAccessContext *ctx,
    const SignetCredentialAccessRequest *req, int64_t now);

/* Resolve a unique provisioned endpoint after authorization, then acquire its
 * credential without charging the capability rate bucket a second time. */
SignetCredAccessStatus signet_credential_access_acquire_session(
    const SignetCredentialAccessContext *ctx,
    const SignetCredentialAccessRequest *req, const char *service_url,
    int64_t now, SignetCredentialAccessGrant *out_grant);

#endif
