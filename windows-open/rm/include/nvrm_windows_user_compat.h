#pragma once

#include "nvport_windows_profile.h"
#include "nvport/nvport.h"
#include "nvlog_defs.h"

#undef PORT_ATOMIC
#define PORT_ATOMIC _Atomic