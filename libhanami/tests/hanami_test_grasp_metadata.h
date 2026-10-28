#ifndef HANAMI_TEST_GRASP_METADATA_H
#define HANAMI_TEST_GRASP_METADATA_H

#include "hanami/hanami-grasp.h"
#include <assert.h>
#include <string.h>

static hanami_error_t nip11_expected(const char *scenario)
{
    if (strncmp(scenario, "nip11-", 6) || !strcmp(scenario, "nip11-no-curation"))
        return HANAMI_OK;
    if (!strcmp(scenario, "nip11-missing") || !strcmp(scenario, "nip11-unsupported"))
        return HANAMI_ERR_CONFIG;
    if (!strcmp(scenario, "nip11-http-error") || !strcmp(scenario, "nip11-disconnect") ||
        !strcmp(scenario, "nip11-http-oversize"))
        return HANAMI_ERR_NETWORK;
    return HANAMI_ERR_NOSTR;
}

static void check_metadata(const char *scenario, const hanami_grasp_metadata_t *server)
{
    hanami_error_t expected = nip11_expected(scenario);
    if (expected == HANAMI_ERR_NETWORK || (expected == HANAMI_ERR_NOSTR &&
        strcmp(scenario, "nip11-no-criteria"))) {
        assert(!server);
        return;
    }
    assert(server);
    if (strcmp(scenario, "nip11-no-criteria"))
        assert(server->repo_acceptance_criteria && !strcmp(server->repo_acceptance_criteria,
            "Local repositories only; ask \"why\"."));
    else
        assert(!server->repo_acceptance_criteria);
    if (!strcmp(scenario, "nip11-no-curation")) {
        assert(!server->curation && !server->supports_grasp05);
        assert(server->supported_grasps_count == 1);
    } else {
        assert(server->curation && !strcmp(server->curation, "Fixture whitelist"));
        assert(server->supports_grasp05 == (strcmp(scenario, "nip11-missing") != 0));
        assert(server->supported_grasps_count ==
            (!strcmp(scenario, "nip11-missing") ? 0u :
             !strcmp(scenario, "nip11-unsupported") ? 1u : 2u));
    }
    if (expected == HANAMI_OK)
        assert(!strcmp(server->supported_grasps[0], "GRASP-01"));
}
#endif
