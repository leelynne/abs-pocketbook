#include "core/version.h"

#include <stdio.h>

#ifndef ABS_BUILD_ID
#define ABS_BUILD_ID "dev"
#endif

const char *abs_version_string(void)
{
    static char buf[64];
    snprintf(buf, sizeof buf, "ABS Client %s (build %s)",
             ABS_CLIENT_VERSION, ABS_BUILD_ID);
    return buf;
}
