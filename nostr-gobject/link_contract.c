/* Check the executable contract with a real link before handing the same
 * CMake-owned archive set to external build systems and the GIR scanner. */
#include <nostr-gobject-1.0/nostr_event.h>

int main(void)
{
  return gnostr_event_get_type() == G_TYPE_INVALID;
}
