/* channel_debug.c - Debug infrastructure globals for GoChannel primitives */

#include "channel_debug.h"

/* Runtime debug flag (set from env NOSTR_CHAN_DEBUG) */
int g_go_chan_debug_enabled = 0;

/* Quarantine mode: never free channels, just poison them */
int g_go_chan_quarantine_mode = 0;

/* Never-free mode: channels are NEVER freed, not even poisoned */
int g_go_chan_never_free_mode = 0;

/* Operation counter for periodic verification */
_Atomic uint64_t g_go_chan_op_counter = 0;

/* Allocation counter */
_Atomic uint64_t g_go_chan_alloc_counter = 0;

/* Leaked channel counter (for quarantine mode) */
_Atomic uint64_t g_go_chan_leaked_count = 0;

/* Quarantine list for verification */
void *g_go_chan_quarantine_list[GO_CHAN_QUARANTINE_MAX] = {0};
size_t g_go_chan_quarantine_sizes[GO_CHAN_QUARANTINE_MAX] = {0};
_Atomic size_t g_go_chan_quarantine_count = 0;
