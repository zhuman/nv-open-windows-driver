#include <gtest/gtest.h>

#include "nvtypes.h"
#include "nvstatus.h"

#include <initializer_list>

struct OBJGPU;
struct KernelBus;
struct KernelGmmu {};
struct KernelGsp { NvBool bPollingForRpcResponse; };
struct OBJRPC {};
using Membar = NV_STATUS (*)(OBJGPU*, KernelBus*);
struct KernelBus { Membar membar; };
struct OBJGPU
{
    NvBool bMemsubsysErrDetectionEnabled;
    KernelGsp gsp;
    KernelGmmu gmmu;
    KernelBus bus;
};

static unsigned probes = 0, warnings = 0, errors = 0;
static NV_STATUS probeResult = NV_OK;

NV_STATUS kbusSendSysmembarSingle_KERNEL(OBJGPU*, KernelBus*) { return NV_OK; }
NV_STATUS LocalMembar(OBJGPU*, KernelBus*) { return NV_OK; }
NV_STATUS kgmmuCheckMemSubsysError_HAL(OBJGPU*, KernelGmmu*)
{
    ++probes;
    return probeResult;
}

#define GPU_GET_KERNEL_GMMU(gpu) (&(gpu)->gmmu)
#define GPU_GET_KERNEL_GSP(gpu) (&(gpu)->gsp)
#define GPU_GET_KERNEL_BUS(gpu) (&(gpu)->bus)
#define kbusSendSysmembarSingle_FNPTR(bus) ((bus)->membar)
#ifndef NV_GSP_TIMEOUT_TEST_NONWINDOWS
#define NV_WINDOWS_IDDCX 1
#endif
#define LEVEL_WARNING 1
#define LEVEL_ERROR 2
#define NV_PRINTF(level, ...) ((level) == LEVEL_WARNING ? ++warnings : ++errors)
#define portDbgPrintf(...) (++warnings)
#include "gsp_timeout_probe.inc"


TEST(GspTimeoutProbe, HonorsPlatformPollingAndMembarPolicy)
{
    // A GSP-backed membar would re-enter RPC while already polling. Windows
    // rejects that path before probing, but must not suppress a local membar.
    for (NvBool enabled : {NV_FALSE, NV_TRUE})
        for (NvBool polling : {NV_FALSE, NV_TRUE})
            for (Membar membar : {LocalMembar, kbusSendSysmembarSingle_KERNEL})
            {
                OBJGPU gpu{enabled, {polling}, {}, {membar}};
                OBJRPC rpc{};
                probes = warnings = errors = 0;
                probeResult = NV_ERR_MEMORY_ERROR;
                const auto status = _kgspCheckIfMemsysError(&gpu, &rpc, 1, 2);
                const bool unsupported =
#if defined(NV_WINDOWS_IDDCX)
                    enabled && polling &&
                    membar == kbusSendSysmembarSingle_KERNEL;
#else
                    false;
#endif
                EXPECT_TRUE(status == (unsupported ? NV_ERR_NOT_SUPPORTED : probeResult));
                EXPECT_TRUE(probes == (unsupported ? 0U : 1U));
                EXPECT_TRUE(warnings == (unsupported ? 1U : 0U));
                EXPECT_TRUE(errors == (unsupported ? 0U : 1U));
            }
}

TEST(GspTimeoutProbe, SuccessfulProbeDoesNotLog)
{
    OBJGPU gpu{NV_TRUE, {NV_TRUE}, {}, {LocalMembar}};
    OBJRPC rpc{};
    probes = warnings = errors = 0;
    probeResult = NV_OK;
    EXPECT_TRUE(_kgspCheckIfMemsysError(&gpu, &rpc, 1, 2) == NV_OK);
    EXPECT_TRUE(probes == 1 && warnings == 0 && errors == 0);
}
