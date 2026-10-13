#ifndef GROTTO_MAIN_APP_H
#define GROTTO_MAIN_APP_H
#include <adwaita.h>
/* Production startup/action wiring, shared by main and private-bus acceptance. */
AdwApplication *grotto_application_new(GApplicationFlags flags);
#endif
