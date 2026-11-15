#ifndef REKEY_TEST_H
#define REKEY_TEST_H
#include "fakes/rekey.h"
#include <sodium.h>
#include <stdio.h>
#include <string.h>
#define T UINT64_C(120000)
#define SEC(x) ((fips_duration){(x), 0})
#define VIEW(n) fips_rekey_get_view(&(n)->machine)
#define RUN(f)                                                                                     \
    do {                                                                                           \
        printf("rekey: %s\n", #f);                                                                 \
        fflush(stdout);                                                                            \
        f();                                                                                       \
        ++tests;                                                                                   \
    } while (0)
extern rk_test_node a, b, c, d;
extern unsigned tests;
static inline int rk_cmp(fips_instant x, fips_instant y) {
    assert(x.ops && y.ops);
    return x.ops->compare(x, y);
}
static const uint8_t DATA[6] = {0x52, 1, 2, 3, 4, 5};
static inline void pair(const fips_rekey_profile *p) {
    rk_test_close(&a);
    rk_test_close(&b);
    rk_test_close(&c);
    rk_test_close(&d);
    rk_test_pair(&a, &b, p,
                 p->layer == FIPS_REKEY_LAYER_Fmp ? fips_rekey_config_fmp()
                                                  : fips_rekey_config_fsp());
}
static inline void flush(rk_test_node *from, rk_test_node *to, uint64_t at) {
    while (rk_test_has_packet(from, false, 0)) {
        rk_test_packet f = rk_test_pop(from, false, 0);
        rk_test_recv(to, at, &f);
    }
}
static inline rk_test_packet complete(rk_test_node *from, rk_test_node *to, uint64_t at) {
    rk_test_poll(from, at);
    rk_test_packet s = rk_test_pop(
        from, true, from->profile.messages == 2 ? FIPS_REKEY_MSG_Msg1 : FIPS_REKEY_MSG_Setup);
    rk_test_recv(to, at + 10, &s);
    rk_test_packet ack = rk_test_pop(
        to, true, from->profile.messages == 2 ? FIPS_REKEY_MSG_Msg2 : FIPS_REKEY_MSG_Ack);
    rk_test_recv(from, at + 20, &ack);
    return from->profile.messages == 3 ? rk_test_pop(from, true, FIPS_REKEY_MSG_Msg3)
                                       : (rk_test_packet){0};
}
static inline void cycle(const fips_rekey_profile *p) {
    pair(p);
    rk_test_packet fin = complete(&a, &b, T);
    if (p->messages == 2)
        rk_test_poll(&a, T + 30);
    else
        rk_test_recv(&b, T + 30, &fin);
    flush(&b, &a, T + 40);
    flush(&a, &b, T + 50);
    flush(&b, &a, T + 60);
    assert(VIEW(&a).current.tag == 1 && VIEW(&b).current.tag == 1);
}
static inline void idle(rk_test_node *n, uint64_t base, unsigned first, unsigned last) {
    for (unsigned i = first; i <= last; ++i) {
        rk_test_poll(n, base + i * 1000);
        n->wires = 0;
    }
}
static inline bool output_before(const rk_test_node *n, fips_rekey_output_kind first,
                                 fips_rekey_output_kind second) {
    bool found = false;
    for (size_t i = 0; i < n->out.count; ++i) {
        if (n->out.items[i].kind == first)
            found = true;
        if (n->out.items[i].kind == second)
            return found;
    }
    return false;
}
static inline bool sends(const rk_test_node *n, fips_rekey_msg_kind k) {
    for (size_t i = 0; i < n->out.count; ++i)
        if (n->out.items[i].kind == FIPS_REKEY_OUT_Send && n->out.items[i].message_kind == k)
            return true;
    return false;
}
static inline rk_test_packet data(rk_test_node *n, uint64_t at) {
    return rk_test_data(n, at, DATA, sizeof DATA);
}
static inline void ordered_open(void) {
    pair(&rk_test_xk);
    uint8_t x = a.local.key.bytes[0], y = b.local.key.bytes[0];
    rk_test_close(&a);
    rk_test_close(&b);
    rk_test_node_init(&a, x, &rk_test_xk);
    rk_test_node_init(&b, y, &rk_test_xk);
}
static inline rk_test_packet first(void) {
    ordered_open();
    rk_test_open(&a, &b.local, fips_rekey_params_fsp(), 0);
    rk_test_packet s = rk_test_pop(&a, true, FIPS_REKEY_MSG_Setup);
    rk_test_inbound(&b, &a.local, fips_rekey_params_fsp(), 10, &s);
    rk_test_packet ack = rk_test_pop(&b, true, FIPS_REKEY_MSG_Ack);
    rk_test_recv(&a, 20, &ack);
    return rk_test_pop(&a, true, FIPS_REKEY_MSG_Msg3);
}
void rekey_params_tests(void);
void rekey_fmp_tests(void);
void rekey_fsp_tests(void);
void rekey_next_tests(void);
void rekey_crossing_tests(void);
#endif
