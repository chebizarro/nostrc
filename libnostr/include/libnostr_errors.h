#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  LN_OK = 0,
  LN_ERR_BACKEND_NOT_FOUND = 1001,
  LN_ERR_DB_OPEN = 1002,
  LN_ERR_DB_TXN = 1003,
  LN_ERR_INGEST = 1004,
  LN_ERR_FILTER_PARSE = 1005,
  LN_ERR_QUERY = 1006,
  LN_ERR_TEXTSEARCH = 1007,
  LN_ERR_OOM = 1008,
  LN_ERR_NOT_FOUND = 1009
} ln_err_t;

#ifdef __cplusplus
}
#endif
