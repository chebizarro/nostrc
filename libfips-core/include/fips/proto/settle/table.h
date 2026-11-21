#ifndef FIPS_PROTO_SETTLE_TABLE_H
#define FIPS_PROTO_SETTLE_TABLE_H
#include <fips/proto/settle/output.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct fips_settle_held {
    bool active, heard, has_last, has_setup, has_sending;
    fips_node_addr peer;
    uint64_t link;
    uint8_t epoch[8], setup[32];
    fips_settle_pair current, sending;
    fips_instant installed, last;
} fips_settle_held;
/* Table-owned records, private-by-contract. A metadata slot is keyed by link,
 * not by the physical slot where its leg happened to be placed. */
typedef struct fips_settle_meta {
    bool active, asked, answered, connecting, redial, pending_redial;
    bool replaces, sent, has_cap, has_held;
    fips_node_addr peer;
    uint64_t link, held;
    uint32_t cap;
    uint8_t ephemeral[32], epoch[8], msg1[FIPS_SETTLE_MESSAGE_CAPACITY];
    size_t msg1_len;
    fips_instant armed;
} fips_settle_meta;
typedef struct fips_settle_table_storage {
    fips_settle_link *links;
    fips_settle_path *paths;
    fips_settle_leg *legs;
    fips_settle_meta *meta;
    fips_settle_held *held;
    fips_settle_memory *memory;
    fips_settle_output *outputs;
    fips_settle_pair *closes;
    size_t capacity, paths_per_link, gate_capacity, output_capacity, close_capacity;
} fips_settle_table_storage;
typedef struct fips_settle_table_state {
    fips_settle_table_storage storage;
    fips_settle_links links;
    fips_settle_legs legs;
    fips_settle_gates gates;
    size_t output_count, close_count;
} fips_settle_table_state;
typedef struct fips_settle_table {
    fips_node_addr me;
    fips_settle_params params;
    fips_settle_table_state states[2];
    unsigned active;
} fips_settle_table;
typedef struct fips_settle_outputs {
    fips_settle_output *items;
    size_t count;
    const fips_settle_pair *closes;
    size_t close_count;
} fips_settle_outputs;
/* Two disjoint caller-owned arenas implement a transaction: the inactive arena
 * is a disposable checkpoint, NOT another owner. No allocation, clock reads,
 * callbacks or global state. Both arenas have identical capacities; arrays
 * must cover those capacities (paths: capacity * paths_per_link). A zero-sized
 * array permits NULL. No storage, input or external output may alias.
 *
 * All successful mutations invalidate borrowed views and replace the output
 * list. A failure preserves the live state, live outputs and input lease.
 * Scratch/checkpoint contents are unspecified. Consume every Install/Free
 * lease before another mutation, or it is refused with FIPS_ERR_RANGE.
 * Drain the table before reinitializing it; do not copy/mutate owning state. */
FIPS_CORE_API fips_status fips_settle_table_init(fips_settle_table *t, fips_node_addr me,
    const fips_settle_params *p, const fips_settle_table_storage storage[2]);
FIPS_CORE_API fips_settle_outputs fips_settle_table_outputs(fips_settle_table *t);
FIPS_CORE_API const fips_settle_held *fips_settle_table_held(const fips_settle_table *t,
    const fips_node_addr *peer);
FIPS_CORE_API const fips_settle_links *fips_settle_table_links(const fips_settle_table *t);
FIPS_CORE_API const fips_settle_legs *fips_settle_table_legs(const fips_settle_table *t);
FIPS_CORE_API const fips_settle_gates *fips_settle_table_gates(const fips_settle_table *t);
FIPS_CORE_API bool fips_settle_table_redialing(const fips_settle_table *t,
    const fips_node_addr *peer, uint64_t *link);
FIPS_CORE_API bool fips_settle_table_next_wake(const fips_settle_table *t, fips_instant *out);
/* Diagnostic writers preserve the existing output list. */
FIPS_CORE_API bool fips_settle_table_count_refused(fips_settle_table *t,
    const fips_node_addr *peer, uint32_t *out);
FIPS_CORE_API uint32_t fips_settle_table_take_refused(fips_settle_table *t,
    const fips_node_addr *peer);
FIPS_CORE_API fips_status fips_settle_table_step(fips_settle_table *t,
    const fips_settle_input *input, fips_instant now);
FIPS_CORE_API fips_status fips_settle_table_poll(fips_settle_table *t, fips_instant now);
FIPS_CORE_API fips_status fips_settle_table_reap_leg(fips_settle_table *t,
    uint64_t link, fips_instant now);
FIPS_CORE_API fips_status fips_settle_table_close(fips_settle_table *t, fips_instant now);
/* Shell adapters for links/legs whose lifetime is managed outside step. */
FIPS_CORE_API fips_status fips_settle_table_open_link(fips_settle_table *t,
    const fips_settle_pair *setup, fips_link_direction direction, uint64_t *link);
FIPS_CORE_API fips_status fips_settle_table_drop_leg(fips_settle_table *t,
    uint64_t link, bool *found, fips_rekey_index_lease *lease);
FIPS_CORE_API fips_status fips_settle_table_unadopt(fips_settle_table *t,
    const fips_node_addr *peer, bool *found, fips_settle_held *out);
#ifdef __cplusplus
}
#endif
#endif
