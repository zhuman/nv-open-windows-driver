#include "nvstatus.h"

typedef struct OBJOS OBJOS;

void osInitObjOS(OBJOS* os)
{
    (void)os;
}

NV_STATUS constructObjOS(OBJOS* os)
{
    osInitObjOS(os);
    return NV_OK;
}