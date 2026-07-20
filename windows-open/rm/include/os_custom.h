#pragma once

#include "nvtypes.h"
#include "nvstatus.h"

void os_dump_stack(void);
NV_STATUS os_iommu_sva_bind(void* arg, void** handle, NvU32* pasid);
void os_iommu_sva_unbind(void* handle);