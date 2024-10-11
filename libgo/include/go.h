#ifndef GO_H
#define GO_H

#include "channel.h"
#include "context.h"
#include "counter.h"
#include "hash_map.h"
#include "int_array.h"
#include "refptr.h"
#include "string_array.h"
#include "wait_group.h"
#include "gtime.h"
#include "ticker.h"
#include "error.h"
#include "select.h"

// Wrapper function to create a new thread
int go(void *(*start_routine)(void *), void *arg);

#endif // GO_H
