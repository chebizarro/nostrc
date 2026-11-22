/*
 * libmarmot - Own sender ratchet high-water mark (nostrc-ow0c)
 *
 * Internal: used by every operation that steps the group's own sender
 * ratchet (messages.c, proposals.c).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_SEND_GUARD_H
#define MARMOT_SEND_GUARD_H

#include "marmot-internal-visibility.h"

#include "marmot-internal.h"
#include "mls/mls_group.h"

#ifdef __cplusplus
extern "C" {
#endif

/* mls_kv label (key: group id) of the generations our own sender ratchets
 * have handed out in the group's current epoch.  A storage snapshot must
 * never cover it (marmot-storage.h, rollback_snapshot): it is what keeps a
 * rolled-back state from sending at a generation already used. */
#define MARMOT_MLS_SEND_HWM_LABEL "mls_send_hwm"

/**
 * Before `g` steps its own sender ratchet: when the high-water mark is of
 * `g`'s epoch and branch (confirmed transcript hash) and own leaf and `g`'s
 * ratchets are behind it -- the state was rolled back below what it sent --
 * move both own chains past it (mls_secret_tree_skip()).  A mark of another
 * epoch, branch or leaf is ignored.  MARMOT_ERR_STORAGE for an unreadable
 * mark (fail closed), MARMOT_ERR_MLS if the ratchet cannot move.
 */
MARMOT_TEST_API MarmotError marmot_send_guard_apply(Marmot *m, MlsGroup *g);

/** After `g` stepped its own sender ratchet: record its heads, in the same
 *  storage transaction as `g`'s state. */
MARMOT_TEST_API MarmotError marmot_send_guard_record(Marmot *m, const MlsGroup *g);

#ifdef __cplusplus
}
#endif

#endif /* MARMOT_SEND_GUARD_H */
