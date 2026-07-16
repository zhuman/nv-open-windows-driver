#pragma once

#include "cpuopsys.h"

#undef NVOS_IS_UNIX
#undef NVOS_IS_WINDOWS
#define NVOS_IS_UNIX 0
#define NVOS_IS_WINDOWS 1