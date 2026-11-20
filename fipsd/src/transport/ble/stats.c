/* Upstream transport/ble/stats.rs BleStats: counted in the shared transport
 * stats block (stats_common.h, the BLE-only names appended last), rendered by
 * show_transports as exactly BleStatsSnapshot's fields in its order. */
#include "transport/ble/internal.h"
fipsd_ble_stats_view fipsd_ble_stats_snapshot(const fipsd_transport *t)
{ return fipsd_transport_stats_read(&t->shared->stats); }
