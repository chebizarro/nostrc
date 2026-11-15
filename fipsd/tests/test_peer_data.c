#include "peer_test.h"
#include "peer/table.h"
#include "peer/identities.h"
#include <fips/identity/node_addr.h>
int peer_data_table_tests(void)
{
    fipsd_peer_table *t = fipsd_peer_table_create(2, (fipsd_peer_caps){0}, NULL, 0);
    CHECK(t);
    fipsd_active_peer *p = new_peer(2, 1, 2), *q = new_peer(1, 1, 1), *r = new_peer(3, 1, 3);
    CHECK(p && q && r);
    CHECK(fipsd_peer_table_insert(t, p) == FIPSD_ADMIT_OK);
    CHECK(fipsd_peer_table_insert(t, q) == FIPSD_ADMIT_OK);
    CHECK(fipsd_peer_table_insert(t, r) == FIPSD_ADMIT_STORAGE);
    CHECK(fipsd_peer_table_insert(t, p) == FIPSD_ADMIT_DUPLICATE);
    fips_node_addr id = peer_id(2).node_addr;
    CHECK(fipsd_peer_table_find(t, &id) == p);
    fips_transport_addr addr = peer_addr(2);
    CHECK(fipsd_peer_table_endpoint(t, (fips_transport_id){1}, &addr) == p);
    CHECK(fipsd_active_peer_info(fipsd_peer_table_at(t, 0))->identity.node_addr.bytes[15] == 1);
    CHECK(!fipsd_peer_table_at(t, 2));
    addr = peer_addr(1);
    CHECK(fipsd_peer_table_rebind(t, &id, (fips_transport_id){1}, &addr) == FIPSD_ADMIT_DUPLICATE);
    addr = peer_addr(9);
    CHECK(fipsd_peer_table_rebind(t, &id, (fips_transport_id){2}, &addr) == FIPSD_ADMIT_OK);
    CHECK(fipsd_peer_table_endpoint(t, (fips_transport_id){2}, &addr) == p);
    addr = peer_addr(2); CHECK(!fipsd_peer_table_endpoint(t, (fips_transport_id){1}, &addr));
    CHECK(fipsd_peer_table_publish(t));
    GoRefPtr before = fipsd_peer_table_snapshot_load(t);
    const fipsd_peer_table_snapshot *s = before.ptr;
    CHECK(s && s->count == 2 && s->peers[1].transport.value == 2);
    fipsd_peer_data *data = fipsd_active_peer_data(p);
    CHECK(data->pending_filter_update && data->heard && !data->frame_heard);
    CHECK(fipsd_peer_data_tree_due(data, 0));
    fipsd_peer_data_tree_sent(data, 0);
    CHECK(!fipsd_peer_data_tree_due(data, 499) && fipsd_peer_data_tree_due(data, 500));
    fips_parent_declaration decl = fips_parent_declaration_self_root(id, 1, 0);
    fips_coord_entry entries[1] = {fips_coord_entry_addr_only(id)};
    CHECK(fipsd_peer_data_tree(data, &decl, entries, 1));
    CHECK(!fipsd_peer_data_tree(data, &decl, entries, FIPSD_PEER_COORD_CAPACITY + 1));
    CHECK(data->ancestry_count == 1 && data->has_tree);
    fips_bloom_filter filter;
    fips_bloom_init(&filter); fips_bloom_insert(&filter, &id);
    fipsd_peer_data_filter(data, &filter, 7, 1000);
    CHECK(fips_bloom_contains(&data->inbound_filter, &id));
    CHECK(!fipsd_peer_data_filter_stale(data, 2000, 1000));
    CHECK(fipsd_peer_data_filter_stale(data, 2001, 1000));
    CHECK(!fipsd_peer_data_filter_stale(data, 999, 1000));
    CHECK(fipsd_active_peer_mmp_init(p, FIPS_MMP_Full));
    CHECK(fipsd_active_peer_mmp(p));
    CHECK(fipsd_active_peer_publish(p));
    GoRefPtr peer_snapshot = fipsd_active_peer_snapshot_load(p);
    CHECK(((fipsd_peer_info *)peer_snapshot.ptr)->has_mmp);
    CHECK(fipsd_peer_table_publish(t));
    CHECK(!s->peers[1].data.has_filter); /* old reference is immutable */
    CHECK(fipsd_peer_table_remove(t, &id) == FIPSD_ADMIT_OK);
    CHECK(!fipsd_peer_table_find(t, &id));
    CHECK(fipsd_peer_table_insert(t, r) == FIPSD_ADMIT_OK);
    CHECK(fipsd_peer_table_destroy(t));
    CHECK(s->count == 2 && ((fipsd_peer_info *)peer_snapshot.ptr)->data.filter_sequence == 7);
    go_refptr_release(&before); go_refptr_release(&peer_snapshot);
    /* A different node cannot overwrite an occupied transport endpoint. */
    t = fipsd_peer_table_create(3, (fipsd_peer_caps){1, 0, 0}, NULL, 0);
    p = new_peer(1, 1, 1); q = new_peer(2, 1, 1); r = new_peer(3, 1, 2);
    CHECK(t && p && q && r);
    CHECK(fipsd_peer_table_insert(t, p) == FIPSD_ADMIT_OK);
    CHECK(fipsd_peer_table_insert(t, q) == FIPSD_ADMIT_DUPLICATE);
    CHECK(fipsd_peer_table_insert(t, r) == FIPSD_ADMIT_TOTAL);
    CHECK(fipsd_active_peer_destroy(q) && fipsd_active_peer_destroy(r) && fipsd_peer_table_destroy(t));
    t = fipsd_peer_table_create(1, (fipsd_peer_caps){0, 0, 1}, NULL, 0);
    p = new_peer(1, 1, 1); CHECK(t && p);
    fipsd_peer_ticket pending = {0}, extra = {0};
    CHECK(fipsd_peer_table_reserve(t, (fips_transport_id){1}, &pending) == FIPSD_ADMIT_OK);
    CHECK(fipsd_peer_table_reserve(t, (fips_transport_id){2}, &extra) == FIPSD_ADMIT_STORAGE);
    CHECK(!fipsd_peer_table_destroy(t));
    CHECK(fipsd_peer_table_insert_reserved(t, p, &pending) == FIPSD_ADMIT_OK && !pending.owner);
    addr = peer_addr(9); id = peer_id(1).node_addr;
    CHECK(fipsd_peer_table_rebind(t, &id, (fips_transport_id){99}, &addr) == FIPSD_ADMIT_OK);
    CHECK(fipsd_peer_table_endpoint(t, (fips_transport_id){99}, &addr) == p);
    CHECK(fipsd_peer_table_destroy(t));
    /* Real opaque local keys; duplicate/full failures retain caller ownership. */
    fips_fixed_entropy entropy = {3};
    fips_secp *secp = NULL;
    CHECK(fips_identity_init_secp(fips_fixed_entropy_adapter(&entropy), &secp) == FIPS_IDENTITY_OK);
    fips_identity *keys[3] = {0};
    for (unsigned i = 0; i < 3; ++i) {
        uint8_t secret[32] = {0}; secret[31] = (uint8_t)(i + 1);
        CHECK(fips_identity_from_secret_bytes(secp, fips_fixed_entropy_adapter(&entropy), secret, &keys[i]) == FIPS_IDENTITY_OK);
    }
    fipsd_identities *ids = fipsd_identities_create(2); CHECK(ids);
    fips_node_addr local = fips_identity_node_addr(keys[0]);
    CHECK(fipsd_identities_add(ids, keys[0]));
    CHECK(!fipsd_identities_add(ids, keys[0]));
    CHECK(fipsd_identities_add(ids, keys[1]));
    CHECK(!fipsd_identities_add(ids, keys[2]));
    CHECK(fipsd_identities_count(ids) == 2 && fipsd_identities_find(ids, &local) == keys[0]);
    CHECK(fipsd_identities_remove(ids, &local) && !fipsd_identities_remove(ids, &local));
    fipsd_identities_destroy(ids); fips_identity_destroy(keys[2]); fips_identity_destroy_secp(secp);
    fipsd_identity_entry cache_storage[1];
    fipsd_identity_cache cache;
    fipsd_identity_cache_init(&cache, cache_storage, 1);
    fips_peer_identity one = peer_id(1), two = peer_id(2);
    CHECK(fipsd_identity_cache_put(&cache, one, 1));
    CHECK(!fipsd_identity_cache_put(&cache, two, 2));
    fips_node_prefix prefix = fips_node_addr_prefix(&one.node_addr);
    CHECK(fipsd_identity_cache_touch(&cache, prefix, 3) && cache.entries[0].used_ms == 3);
    one.pubkey.bytes[0] = 9;
    CHECK(fipsd_identity_cache_put(&cache, one, 4));
    CHECK(fipsd_identity_cache_touch(&cache, prefix, 5)->pubkey.bytes[0] == 9);
    CHECK(fipsd_identity_cache_remove(&cache, prefix) && !cache.count);
    return 0;
}
