/* SPDX-License-Identifier: MIT — durable plaintext clone/fetch (nostrc-v2zyh). */
#ifndef HANAMI_FETCH_H
#define HANAMI_FETCH_H
#include "hanami/hanami.h"
#include "hanami/hanami-config.h"

/* nostr://<hex owner>/<repo_id>; caller frees both outputs. */
hanami_error_t hanami_parse_nostr_uri(const char *uri, char **owner, char **repo_id);

/* git_clone into local_path from the owner's 30618 + manifest chain on
 * relays, reading verified objects from endpoint. Writes objects, refs,
 * remote "origin" = uri, hanami.endpoint/hanami.relays and the
 * .git/hanami/index.sqlite cache. Checkout failure is fatal. */
hanami_error_t hanami_clone_materialize(git_repository **out, const char *uri,
    const char *local_path, const char *owner, const char *repo_id,
    const char *endpoint, const char *const *relays,
    const hanami_signer_t *signer, const hanami_config_t *config);
#endif
