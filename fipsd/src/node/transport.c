#include "node/transport.h"
#include <string.h>
int fipsd_transport_register(fipsd_transport_registry *r, fipsd_transport *t)
{
    if (!r || r->sealed || r->count == FIPSD_TRANSPORT_MAX || !t || !t->ops ||
        !t->name || !t->type || !t->ops->start_async || !t->ops->stop ||
        !t->ops->state || !t->ops->mtu || !t->ops->send) return -1;
    for (size_t i = 0; i < r->count; ++i)
        if (fips_transport_id_equal(r->items[i]->id, t->id) ||
            !strcmp(r->items[i]->name, t->name)) return -1;
    r->items[r->count++] = t;
    return 0;
}
fipsd_transport *fipsd_transport_find(const fipsd_transport_registry *r, fips_transport_id id)
{
    for (size_t i = 0; i < r->count; ++i)
        if (fips_transport_id_equal(r->items[i]->id, id)) return r->items[i];
    return NULL;
}

fips_status fipsd_transport_parse_address(const fipsd_transport *t, const char *text, fips_transport_addr *out)
{
    if (!t || !text) return FIPS_ERR_RANGE;
    if (!strcmp(t->type, "ethernet")) {
        const char *slash = strchr(text, '/');
        if (slash) {
            size_t n = (size_t)(slash - text);
            if (!t->interface_name || strlen(t->interface_name) != n || memcmp(t->interface_name, text, n))
                return FIPS_ERR_RANGE;
            text = slash + 1;
        }
        return fips_transport_addr_parse(text, strlen(text), FIPS_TRANSPORT_ADDR_PARSE_MAC, out);
    }
    return fips_transport_addr_from_string(text, strlen(text), out);
}
