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

NvBool osDispService(NvU32 intr0, NvU32 intr1)
{
    (void)intr0;
    (void)intr1;
    return NV_FALSE;
}

NvBool osQueueDpc(OBJGPU* pGpu)
{
    (void)pGpu;
    return NV_FALSE;
}

NV_STATUS osDeferredIsr(OBJGPU* pGpu)
{
    MC_ENGINE_BITVECTOR nonStallPending;
    MC_ENGINE_BITVECTOR stallPending;
    NV_STATUS status;
    Intr* pIntr;
    NvBool serviced = NV_FALSE;

    if (pGpu == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    pIntr = GPU_GET_INTR(pGpu);
    if (pIntr == NULL || !gpuIsStateLoaded(pGpu) || !osIsGpuAccessible(pGpu) ||
        intrGetIntrEn(pIntr) != INTERRUPT_TYPE_HARDWARE)
    {
        return NV_ERR_NO_INTR_PENDING;
    }

    bitVectorClrAll(&nonStallPending);
    intrGetPendingNonStall_HAL(pGpu, pIntr, &nonStallPending, NULL);
    if (!bitVectorTestAllCleared(&nonStallPending))
    {
        intrServiceNonStall_HAL(pGpu, pIntr, &nonStallPending, NULL);
        serviced = NV_TRUE;
    }

    bitVectorClrAll(&stallPending);
    status = intrGetPendingStall_HAL(pGpu, pIntr, &stallPending, NULL);
    if (status != NV_OK)
    {
        return status;
    }

    if (!bitVectorTestAllCleared(&stallPending))
    {
        intrServiceStall(pGpu, pIntr);
        serviced = NV_TRUE;
    }

    return serviced ? NV_OK : NV_ERR_NO_INTR_PENDING;
}

void osHandleDeferredRecovery(OBJGPU* pGpu)
{
    (void)pGpu;
}

void osQueueMMUFaultHandler(OBJGPU* pGpu)
{
    (void)pGpu;
}