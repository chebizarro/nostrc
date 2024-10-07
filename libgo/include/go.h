#ifndef GO_H
#define GO_H

#include "refptr.h"
#include "context.h"
#include "channel.h"
#include "hash_map.h"
#include "wait_group.h"
#include "string_array.h"
#include "int_array.h"

// Wrapper function to create a new thread
int go(void *(*start_routine)(void *), void *arg);

#endif // GO_H
