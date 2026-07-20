#include "core/core.h"
#include "gpu/gpu.h"
#include "kernel/gpu/intr/intr.h"

NvBool osLockShouldToggleInterrupts(OBJGPU* pGpu)
{
    if (IS_DCE_CLIENT(pGpu))
    {
        return NV_TRUE;
    }

    return !pGpu->getProperty(pGpu, PDB_PROP_GPU_IN_PM_CODEPATH) &&
        gpuIsStateLoaded(pGpu);
}

void osEnableInterrupts(OBJGPU* pGpu)
{
    Intr* pIntr;
    NvU32 intrEn;

    NV_ASSERT_OR_RETURN_VOID(
        !pGpu->getProperty(pGpu, PDB_PROP_GPU_TEGRA_SOC_NVDISPLAY));

    pIntr = GPU_GET_INTR(pGpu);
    if (!pIntr->getProperty(pIntr, PDB_PROP_INTR_USE_INTR_MASK_FOR_LOCKING))
    {
        NV_ASSERT(
            intrGetIntrEnFromHw_HAL(pGpu, pIntr, NULL) ==
            INTERRUPT_TYPE_DISABLED);
    }

    intrEn = intrGetIntrEn(pIntr);
    intrSetIntrEnInHw_HAL(pGpu, pIntr, intrEn, NULL);
    intrSetStall_HAL(pGpu, pIntr, intrEn, NULL);
    intrRestoreNonStall_HAL(pGpu, pIntr, intrEn, NULL);
}

void osDisableInterrupts(OBJGPU* pGpu, NvBool bIsr)
{
    Intr* pIntr;

    (void)bIsr;
    NV_ASSERT_OR_RETURN_VOID(
        !pGpu->getProperty(pGpu, PDB_PROP_GPU_TEGRA_SOC_NVDISPLAY));

    pIntr = GPU_GET_INTR(pGpu);
    intrSetIntrEnInHw_HAL(pGpu, pIntr, INTERRUPT_TYPE_DISABLED, NULL);
    intrSetStall_HAL(pGpu, pIntr, INTERRUPT_TYPE_DISABLED, NULL);
    intrRestoreNonStall_HAL(pGpu, pIntr, intrGetIntrEn(pIntr), NULL);
}