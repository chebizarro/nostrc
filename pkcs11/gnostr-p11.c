#include "gnostr-p11.h"

static CK_RV not_implemented(void){ return CKR_FUNCTION_NOT_SUPPORTED; }

static CK_FUNCTION_LIST function_list = {
  { 2, 40 }, /* version */
  /* Initialize/Finalize */
  not_implemented, not_implemented,
  /* GetInfo/GetFunctionList (we implement only GetFunctionList via exported symbol) */
  not_implemented, NULL,
  /* Slot, Token, Session, Object, Mechanism management etc. (stubs) */
};

CK_RV gnostr_p11_get_function_list(CK_FUNCTION_LIST_PTR_PTR list){
  if (!list) return CKR_ARGUMENTS_BAD;
  *list = &function_list; return CKR_OK;
}

/* p11-kit entry point */
CK_RV C_GetFunctionList(CK_FUNCTION_LIST_PTR_PTR list){
  return gnostr_p11_get_function_list(list);
}
