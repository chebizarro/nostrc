/* Vendored nostrdb headers use compiler extensions and define static helpers.
 * Keep their diagnostics separate from libmarmot's own warning policy. */
#ifndef MARMOT_NOSTRDB_HEADERS_H
#define MARMOT_NOSTRDB_HEADERS_H
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC system_header
#endif
#include "nostrdb.h"
#include "lmdb.h"
#endif
