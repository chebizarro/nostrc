/* The requests `fipsctl path show`, `pin` and `unpin` send (upstream
 * fipsctl.rs path_request). The address goes only when given: a pin without
 * one names the whole transport. */
#include "fipsctl.h"
#include <stdlib.h>

char *fipsctl_path_request(const fipsctl_args *a, const char *hosts_path, FILE *err)
{
    char *npub = fipsctl_resolve_peer_in(hosts_path, a->peer, err);
    if (!npub) return NULL;
    json_t *params = json_pack("{s:s}", "npub", npub);
    free(npub);
    if (!params) return NULL;
    const char *command;
    switch (a->command) {
    case FIPSCTL_CMD_PATH_SHOW: command = "path_show"; break;
    case FIPSCTL_CMD_PATH_UNPIN: command = "path_unpin"; break;
    case FIPSCTL_CMD_PATH_PIN:
        command = "path_pin";
        json_object_set_new(params, "transport", json_string(a->transport));
        if (a->address) json_object_set_new(params, "address", json_string(a->address));
        break;
    default:
        json_decref(params);
        return NULL;
    }
    return fipsctl_build_command(command, params);
}
