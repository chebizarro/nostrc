#include <fips/core.h>
#include <string.h>

int main(void)
{
    return strcmp(fips_core_version(), "0.1.0") != 0;
}
