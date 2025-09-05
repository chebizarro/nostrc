#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct gof_chan gof_chan_t;

gof_chan_t* gof_chan_make(size_t capacity);
void        gof_chan_close(gof_chan_t* c);

/* Blocking cooperative send/recv of a single pointer-sized value */
int         gof_chan_send(gof_chan_t* c, void* value);
int         gof_chan_recv(gof_chan_t* c, void** out_value);

/* Non-blocking try variants: return 1 on success, 0 if would block, <0 on closed */
int         gof_chan_try_send(gof_chan_t* c, void* value);
int         gof_chan_try_recv(gof_chan_t* c, void** out_value);

#ifdef __cplusplus
}
#endif
