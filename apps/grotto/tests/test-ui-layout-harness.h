#pragma once
#include <glib.h>

/* Private-bus fixture control; never changes a production signer setting. */
void grotto_test_connection_listening(gboolean listening);
void grotto_test_add_dialog_sizing(void);
