#include <fips/node/session.h>
#include <fips/identity/key.h>
#include <sodium.h>
#include <string.h>

fips_status fips_session_encode_sealed(const fips_rekey_profile *p,
    const fips_rekey_sealed *s, const uint8_t *plain, size_t len,
    uint8_t *wire, size_t cap, size_t *written) {
    size_t aad = p->aad, n = 0;
    if (len > UINT16_MAX || aad > cap || len > cap - aad ||
        FIPS_NOISE_TAG_SIZE > cap - aad - len) return FIPS_ERR_RANGE;
    if (p == &fips_fmp_rekey_profile) {
        if (!s->theirs.some) return FIPS_ERR_RANGE;
        fips_fmp_build_established_header((fips_session_index){s->theirs.value}, s->counter,
            s->k ? FIPS_FMP_FLAG_KEY_EPOCH : 0, (uint16_t)len, wire);
    } else if (p == &fips_fsp_rekey_profile)
        fips_fsp_build_header(s->counter, s->k ? FIPS_FSP_FLAG_K : 0, (uint16_t)len, wire);
    else return FIPS_ERR_RANGE;
    if (fips_cipher_state_seal_with_counter(&s->key, s->counter, wire, aad, plain, len,
                                            wire + aad, cap - aad, &n) != FIPS_NOISE_OK)
        return FIPS_ERR_RANGE;
    *written = aad + n;
    return FIPS_OK;
}
static fips_status handshake_wire(const fips_rekey *m, const fips_rekey_output *o,
                                  const fips_session_executor *x, size_t *len) {
    if (m->profile == &fips_fmp_rekey_profile) {
        if (o->data.msg.len > x->capacity) return FIPS_ERR_RANGE;
        memcpy(x->wire, o->data.msg.bytes, o->data.msg.len);
        *len = o->data.msg.len;
        return FIPS_OK;
    }
    fips_writer w;
    fips_writer_init(&w, x->wire, x->capacity);
    fips_fsp_bytes hs = {o->data.msg.bytes, o->data.msg.len};
    fips_status status;
    if (o->message_kind == FIPS_REKEY_MSG_Msg3) {
        fips_fsp_session_msg3 msg = {.handshake_payload = hs};
        status = fips_fsp_session_msg3_encode(&msg, &w, NULL);
    } else {
        fips_fsp_coords src, dest;
        if (!x->coords || !x->coords(x->ctx, o->route, x->arrival, &src, &dest)) return FIPS_ERR_RANGE;
        if (o->message_kind == FIPS_REKEY_MSG_Setup) {
            fips_fsp_session_setup msg = {.src_coords = src, .dest_coords = dest, .handshake_payload = hs};
            status = fips_fsp_session_setup_encode(&msg, &w, NULL);
        } else if (o->message_kind == FIPS_REKEY_MSG_Ack) {
            fips_fsp_session_ack msg = {.src_coords = src, .dest_coords = dest, .handshake_payload = hs};
            status = fips_fsp_session_ack_encode(&msg, &w, NULL);
        } else return FIPS_ERR_RANGE;
    }
    if (status == FIPS_OK) *len = fips_writer_len(&w);
    return status;
}
static fips_status prompt_wire(fips_rekey *m, fips_instant now, const fips_rekey_output *o,
                               const fips_session_executor *x, size_t *len) {
    fips_rekey_sealed_prompt prompt = {0};
    fips_rekey_seal_refusal refusal = fips_rekey_seal_prompt(m, now, o->tag, &prompt);
    if (refusal != FIPS_REKEY_SEAL_OK) return FIPS_ERR_RANGE;
    fips_fsp_coords src = {0}, dest = {0};
    bool coords = m->profile == &fips_fsp_rekey_profile && x->coords &&
        x->coords(x->ctx, FIPS_REKEY_ROUTE_EstablishedFallback,
                  prompt.has_route ? prompt.route : 0, &src, &dest);
    fips_status status;
    if (!coords) status = fips_session_encode_sealed(m->profile, &prompt.sealed,
        prompt.plaintext.bytes, prompt.plaintext.len, x->wire, x->capacity, len);
    else {
        size_t plain = prompt.plaintext.len, body = FIPS_FRAMING_FSP_HEADER_SIZE;
        /* CP coordinates are outside AAD and ciphertext, exactly as deployed. */
        size_t coord_count = src.count + dest.count;
        if (src.count > UINT16_MAX || dest.count > UINT16_MAX ||
            coord_count > (SIZE_MAX - body - 4 - plain - FIPS_NOISE_TAG_SIZE) / 16 ||
            body + 4 + coord_count * 16 + plain + FIPS_NOISE_TAG_SIZE > x->capacity)
            status = FIPS_ERR_RANGE;
        else {
            body += 4 + coord_count * 16;
            fips_fsp_build_header(prompt.sealed.counter, FIPS_FSP_FLAG_CP |
                (prompt.sealed.k ? FIPS_FSP_FLAG_K : 0), (uint16_t)plain, x->wire);
            size_t pos = FIPS_FRAMING_FSP_HEADER_SIZE;
            x->wire[pos++] = (uint8_t)src.count; x->wire[pos++] = (uint8_t)(src.count >> 8);
            if (src.count) memcpy(x->wire + pos, src.addrs, src.count * 16);
            pos += src.count * 16;
            x->wire[pos++] = (uint8_t)dest.count; x->wire[pos++] = (uint8_t)(dest.count >> 8);
            if (dest.count) memcpy(x->wire + pos, dest.addrs, dest.count * 16);
            size_t n = 0;
            status = fips_cipher_state_seal_with_counter(&prompt.sealed.key, prompt.sealed.counter,
                x->wire, FIPS_FRAMING_FSP_HEADER_SIZE, prompt.plaintext.bytes, plain,
                x->wire + body, x->capacity - body, &n) == FIPS_NOISE_OK ? FIPS_OK : FIPS_ERR_RANGE;
            if (status == FIPS_OK) *len = body + n;
        }
    }
    fips_rekey_sealed_prompt_clear(&prompt);
    return status;
}
fips_status fips_session_execute(fips_rekey *m, fips_instant now, fips_rekey_outputs *out,
    const uint8_t *input, size_t len, const fips_session_executor *x) {
    fips_status result = FIPS_OK;
    for (size_t i = 0; i < out->count; ++i) {
        fips_rekey_output *o = &out->items[i];
        fips_status status = FIPS_OK;
        if (o->kind == FIPS_REKEY_OUT_Send || o->kind == FIPS_REKEY_OUT_Prompt) {
            size_t n = 0;
            status = o->kind == FIPS_REKEY_OUT_Send ? handshake_wire(m, o, x, &n)
                                                     : prompt_wire(m, now, o, x, &n);
            if (status == FIPS_OK) status = x->send(x->ctx, o, x->wire, n);
        } else if (o->kind == FIPS_REKEY_OUT_Release) {
            uint32_t index;
            if (fips_rekey_index_lease_release(&o->lease, &index)) x->release(x->ctx, index);
            else status = FIPS_ERR_RANGE;
        } else if (o->kind == FIPS_REKEY_OUT_Deliver) {
            if (o->begin > o->end || o->end > len) status = FIPS_ERR_RANGE;
            else {
                /* Preserve the deployed plaintext gates on EVERY epoch path. */
                if (m->profile == &fips_fsp_rekey_profile) {
                    fips_fsp_action action;
                    fips_fsp_action_list list = {&action, 0, 1};
                    status = fips_fsp_receive_plaintext(fips_rekey_bond_addr(&m->bond),
                        input + o->begin, o->end - o->begin, &list);
                    if (status == FIPS_OK && action.kind == FIPS_FSP_DropFrame) status = FIPS_ERR_RANGE;
                } else {
                    fips_fmp_conn_action action;
                    fips_fmp_action_list list = {&action, 0, 1};
                    status = fips_fmp_receive_plaintext((fips_link_id){0},
                        input + o->begin, o->end - o->begin, &list);
                    if (status == FIPS_OK && action.kind == FIPS_FMP_DropFrame) status = FIPS_ERR_RANGE;
                }
                if (status == FIPS_OK) x->effect(x->ctx, o, input + o->begin, o->end - o->begin);
            }
        } else x->effect(x->ctx, o, NULL, 0);
        if (status != FIPS_OK && result == FIPS_OK) result = status;
    }
    fips_rekey_outputs_clear(out);
    return result;
}

void fips_session_table_init(fips_session_table *t, fips_session_entry *entries, size_t capacity) {
    *t = (fips_session_table){entries, 0, capacity};
    if (capacity) memset(entries, 0, capacity * sizeof *entries);
}
fips_session_entry *fips_session_table_by_peer(fips_session_table *t, fips_node_addr peer) {
    for (size_t i = 0; i < t->capacity; ++i)
        if (t->entries[i].occupied && !memcmp(t->entries[i].peer.bytes, peer.bytes, 16)) return &t->entries[i];
    return NULL;
}
fips_session_entry *fips_session_table_by_index(fips_session_table *t,
                                              fips_rekey_transport transport, uint32_t index) {
    for (size_t i = 0; i < t->capacity; ++i) if (t->entries[i].occupied) {
        fips_rekey_binding bindings[3];
        size_t n = fips_rekey_bindings(&t->entries[i].machine, bindings);
        for (size_t j = 0; j < n; ++j)
            if (bindings[j].transport == transport && bindings[j].index == index) return &t->entries[i];
    }
    return NULL;
}
static fips_session_entry *vacant(fips_session_table *t, fips_node_addr peer) {
    if (fips_session_table_by_peer(t, peer)) return NULL;
    for (size_t i = 0; i < t->capacity; ++i) if (!t->entries[i].occupied) return &t->entries[i];
    return NULL;
}
static void occupied(fips_session_table *t, fips_session_entry *e, fips_node_addr peer,
                     fips_instant now, fips_session_entry **out) {
    e->occupied = true; e->peer = peer; e->created = now; e->last_activity = now;
    ++t->count; *out = e;
}
fips_status fips_session_table_open(fips_session_table *t, fips_peer_key peer,
    fips_node_addr me, fips_rekey_params params, fips_instant now,
    fips_rekey_env *env, fips_rekey_outputs *out, fips_session_entry **entry) {
    fips_node_addr addr = fips_peer_key_addr(&peer);
    fips_session_entry *e = fips_session_table_by_peer(t, addr);
    if (e) {
        if (e->machine.profile != &fips_fsp_rekey_profile) return FIPS_ERR_RANGE;
        fips_status status = fips_rekey_step(&e->machine, now,
            (fips_rekey_input){.kind = FIPS_REKEY_IN_Open, .peer = peer}, env, out);
        if (status == FIPS_OK) *entry = e;
        return status;
    }
    e = vacant(t, addr);
    if (!e) return FIPS_ERR_RANGE;
    fips_status status = fips_rekey_opening_outbound(&e->machine, &fips_fsp_rekey_profile,
        params, peer, me, now, env, out);
    if (status == FIPS_OK) occupied(t, e, addr, now, entry);
    return status;
}
fips_status fips_session_table_answer(fips_session_table *t, fips_node_addr claimed,
    fips_node_addr me, fips_rekey_params params, fips_instant now, fips_rekey_input setup,
    fips_rekey_env *env, fips_rekey_outputs *out, fips_session_entry **entry) {
    if (setup.kind != FIPS_REKEY_IN_Setup) return FIPS_ERR_RANGE;
    fips_session_entry *e = fips_session_table_by_peer(t, claimed);
    if (e) {
        if (e->machine.profile != &fips_fsp_rekey_profile) return FIPS_ERR_RANGE;
        fips_status status = fips_rekey_step(&e->machine, now, setup, env, out);
        if (status == FIPS_OK) *entry = e;
        return status;
    }
    e = vacant(t, claimed);
    if (!e) return FIPS_ERR_RANGE;
    fips_status status = fips_rekey_opening_inbound(&e->machine, &fips_fsp_rekey_profile,
        params, claimed, me, now, setup, env, out);
    if (status == FIPS_OK) occupied(t, e, claimed, now, entry);
    return status;
}
fips_status fips_session_table_established(fips_session_table *t, const fips_rekey_profile *p,
    fips_node_addr me, fips_rekey_params params, fips_rekey_completed *done,
    fips_rekey_ids *ids, fips_rekey_setup_facts *facts, fips_instant now,
    fips_rekey_env *env, fips_rekey_outputs *out, fips_session_entry **entry) {
    fips_node_addr peer = fips_peer_key_addr(&done->remote);
    fips_session_entry *e = vacant(t, peer);
    if (!e) return FIPS_ERR_RANGE;
    fips_status status = fips_rekey_established(&e->machine, p, params,
        (fips_rekey_bond){.kind = FIPS_REKEY_BOND_Key, .key = done->remote}, me,
        done, ids, facts, now, env, out);
    if (status == FIPS_OK) occupied(t, e, peer, now, entry);
    return status;
}
bool fips_session_table_remove(fips_session_table *t, fips_session_entry *e,
    fips_rekey_close why, fips_rekey_outputs *out, fips_rekey_carry *carry) {
    /* Membership by equality, not relational comparisons of unrelated pointers. */
    for (size_t i = 0; i < t->capacity; ++i) if (&t->entries[i] == e && e->occupied) {
        (void)fips_rekey_close_machine(&e->machine, why, out, carry);
        sodium_memzero(e, sizeof *e); --t->count;
        return true;
    }
    return false;
}
bool fips_session_table_evict(fips_session_table *t, fips_instant cutoff,
                             fips_rekey_outputs *out, fips_rekey_carry *carry) {
    fips_session_entry *oldest = NULL;
    for (size_t i = 0; i < t->capacity; ++i) {
        fips_session_entry *e = &t->entries[i];
        if (!e->occupied || !e->machine.live) continue;
        fips_rekey_view v = fips_rekey_get_view(&e->machine);
        if (!v.current.present || v.cycle != FIPS_REKEY_Idle || v.candidates || v.previous.present ||
            e->last_activity.ops->compare(e->last_activity, cutoff) > 0) continue;
        if (!oldest || e->last_activity.ops->compare(e->last_activity, oldest->last_activity) < 0) oldest = e;
    }
    return oldest && fips_session_table_remove(t, oldest, FIPS_REKEY_CLOSE_IdlePurge, out, carry);
}
void fips_session_note_activity(fips_session_entry *e, fips_instant now) {
    if (e->occupied && now.ops->compare(now, e->last_activity) >= 0) e->last_activity = now;
}
size_t fips_session_table_half_open(const fips_session_table *t) {
    size_t total = 0;
    for (size_t i = 0; i < t->capacity; ++i) if (t->entries[i].occupied) {
        fips_rekey_view v = fips_rekey_get_view(&t->entries[i].machine);
        if (!v.current.present) total += v.candidates;
    }
    return total;
}
