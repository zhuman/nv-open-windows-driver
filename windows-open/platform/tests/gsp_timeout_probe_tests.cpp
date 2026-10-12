#include "nvtypes.h"
#include "nvstatus.h"

#include <cstdio>
#include <cstdlib>
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

static unsigned checks = 0, probes = 0, warnings = 0, errors = 0;
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

static void Check(bool value)
{
    ++checks;
    if (!value)
    {
        std::fprintf(stderr, "GSP timeout probe check %u failed\n", checks);
        std::exit(1);
    }
}

int main()
{
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
                Check(status == (unsupported ? NV_ERR_NOT_SUPPORTED : probeResult));
                Check(probes == (unsupported ? 0U : 1U));
                Check(warnings == (unsupported ? 1U : 0U));
                Check(errors == (unsupported ? 0U : 1U));
            }
    OBJGPU gpu{NV_TRUE, {NV_TRUE}, {}, {LocalMembar}};
    OBJRPC rpc{};
    probes = warnings = errors = 0;
    probeResult = NV_OK;
    Check(_kgspCheckIfMemsysError(&gpu, &rpc, 1, 2) == NV_OK);
    Check(probes == 1 && warnings == 0 && errors == 0);
    std::printf("%u production GSP timeout probe checks passed\n", checks);
}
