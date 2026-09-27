/*
 * notify_pump.c — see notify_pump.h (nostrc-a16c).
 */
#include "notify_pump.h"

#include <stdbool.h>

#include "select.h"

uint64_t nsn_pump_run(GoChannel *const *chans, size_t n_chans,
                      _Atomic int *stop, uint64_t tick_ms,
                      NsnPumpFn on_item, void *user_data) {
  if (!chans || !on_item || n_chans == 0 || n_chans > NSN_PUMP_MAX_CHANNELS)
    return 0;
  GoChannel *live[NSN_PUMP_MAX_CHANNELS] = {0};
  size_t n_live = 0;
  for (size_t i = 0; i < n_chans; i++) {
    live[i] = chans[i];
    if (live[i]) n_live++;
  }
  uint64_t delivered = 0;
  while (n_live > 0 && !(stop && atomic_load(stop))) {
    void *out[NSN_PUMP_MAX_CHANNELS] = {0};
    GoSelectCase cases[NSN_PUMP_MAX_CHANNELS];
    for (size_t i = 0; i < n_chans; i++) {
      cases[i].op = GO_SELECT_RECEIVE;
      cases[i].chan = live[i];      /* NULL cases are skipped by select */
      cases[i].value = NULL;
      cases[i].recv_buf = &out[i];
    }
    GoSelectResult r = go_select_timeout(cases, n_chans, tick_ms);
    if (r.selected_case < 0) continue; /* tick: re-check stop */
    size_t idx = (size_t)r.selected_case;
    if (!r.ok) {
      /* Closed and drained: stop waiting on it. */
      live[idx] = NULL;
      n_live--;
      continue;
    }
    if (out[idx]) {
      on_item(user_data, idx, out[idx]);
      delivered++;
    }
  }
  return delivered;
}
