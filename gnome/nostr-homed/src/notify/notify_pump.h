/*
 * notify_pump — drain several libgo channels from one thread.
 *
 * nostrc-a16c: the connector used to call a blocking go_channel_receive on
 * the group channel, then on the DM channel, in turn — so a quiet group
 * subscription held DM notifications back until the next group event (and
 * vice versa), and a closed channel turned the loop into a busy spin.
 * The pump waits on every channel at once with go_select_timeout, hands
 * each item to `on_item` as soon as it arrives, drops a channel from the
 * set once it is closed and drained, and wakes at least every `tick_ms` to
 * honour `stop`.
 */
#ifndef NOSTR_NOTIFY_PUMP_H
#define NOSTR_NOTIFY_PUMP_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "channel.h"

#define NSN_PUMP_MAX_CHANNELS 4

/* `idx` is the position of the channel in `chans`. */
typedef void (*NsnPumpFn)(void *user_data, size_t idx, void *item);

/*
 * Run until `stop` becomes non-zero or every channel is closed (NULL
 * entries count as closed). Returns the number of items delivered.
 * At most NSN_PUMP_MAX_CHANNELS channels.
 */
uint64_t nsn_pump_run(GoChannel *const *chans, size_t n_chans,
                      _Atomic int *stop, uint64_t tick_ms,
                      NsnPumpFn on_item, void *user_data);

#endif /* NOSTR_NOTIFY_PUMP_H */
