#ifndef FIPSD_SOCKBUF_H
#define FIPSD_SOCKBUF_H
#include <stddef.h>
#include <stdbool.h>
#include "libgo/fiber.h"
typedef enum fipsd_socket_buffer { FIPSD_SOCKET_RECV, FIPSD_SOCKET_SEND } fipsd_socket_buffer;
typedef struct fipsd_buffer_granted { size_t requested, actual; } fipsd_buffer_granted;
/* Blocking setup: call off the scheduler. Linux force then plain fallback;
 * actual is the kernel value (Linux doubles it), just as upstream. */
int fipsd_sockbuf_set(gof_socket_t fd, fipsd_socket_buffer, size_t, fipsd_buffer_granted *);
bool fipsd_sockbuf_clamped(const fipsd_buffer_granted *);
#endif
