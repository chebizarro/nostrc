/* Check the executable contract with a real link before handing the same
 * CMake-owned archive set to external build systems and the GIR scanner. */
#include <nostr-gobject-1.0/nostr_event.h>
#include <nostr-gobject-1.0/gnostr-list-service.h>
#include <nostr-kinds.h>

int main(void)
{
  return gnostr_event_get_type() == G_TYPE_INVALID ||
    gnostr_list_document_get_type() == G_TYPE_INVALID ||
    gnostr_list_service_get_type() == G_TYPE_INVALID ||
    nostr_nip51_schema_lookup(NOSTR_KIND_MUTE_LIST) == NULL;
}
