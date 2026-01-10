#include "utils.h"

gboolean str_has_prefix_http(const char *s) {
    return s && (g_str_has_prefix(s, "http://") || g_str_has_prefix(s, "https://"));
  }
  
  