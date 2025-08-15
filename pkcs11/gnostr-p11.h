#pragma once
#include <p11-kit/pkcs11.h>
#include <glib.h>

CK_RV gnostr_p11_get_function_list(CK_FUNCTION_LIST_PTR_PTR list);
