#include "g_system_nvoc.h"
#include "nvplatform_system_rm.h"

void osInitSystemStaticConfig(SYS_STATIC_CONFIG* config)
{
    if (config == NULL)
    {
        return;
    }

    config->bIsNotebook = NvPlatformRmIsNotebook();
}