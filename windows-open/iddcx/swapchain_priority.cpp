#include "swapchain_priority.hpp"
#include "driver_parameters.hpp"
#include "trace.h"

#include <dxgi1_4.h>
#include <avrt.h>
#include <wil/com.h>

#include "swapchain_priority.tmh"

namespace nv::iddcx {

HRESULT ReadRealtimeGpuPriority(bool* enabled) noexcept
{
    ULONG value = 0;
    const NTSTATUS status = QueryRealtimeGpuPriorityEnabled(&value);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "Invalid RealtimeGpuPriority: status=%!STATUS! value=%u", status, value);
        return HRESULT_FROM_NT(status);
    }
    *enabled = value != 0;
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "Realtime GPU priority requested=%u", *enabled);
    return S_OK;
}

void TrySetRealtimeGpuPriority(
    IDDCX_SWAPCHAIN swapChain, IUnknown* device, PCSTR api) noexcept
{
    if (!IDD_IS_FUNCTION_AVAILABLE(IddCxSetRealtimeGPUPriority))
    {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
            "Realtime GPU priority API unavailable: api=%s; priority unchanged", api);
        return;
    }
    wil::com_ptr_nothrow<IDXGIDevice> dxgiDevice;
    const HRESULT queryResult = device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()));
    if (FAILED(queryResult))
    {
        TraceEvents(queryResult == E_NOINTERFACE ? TRACE_LEVEL_WARNING : TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "Realtime GPU priority IDXGIDevice unavailable: api=%s result=0x%08X; API not invoked, priority unchanged",
            api, queryResult);
        return;
    }
    IDARG_IN_SETREALTIMEGPUPRIORITY arguments = {};
    arguments.pDevice = dxgiDevice.get();
    const HRESULT result = IddCxSetRealtimeGPUPriority(swapChain, &arguments);
    TraceEvents(SUCCEEDED(result) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_WARNING,
        TRACE_DRIVER,
        "IddCxSetRealtimeGPUPriority: api=%s result=0x%08X applied=%u device=%p",
        api, result, SUCCEEDED(result), dxgiDevice.get());
}

HRESULT CreateSourceCopyQueue(
    ID3D12Device* device, bool realtime, ID3D12CommandQueue** queue) noexcept
{
    D3D12_COMMAND_QUEUE_DESC description = {};
    description.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (realtime)
    {
        D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY support = {};
        support.CommandListType = description.Type;
        support.Priority = D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
        const HRESULT supportResult = device->CheckFeatureSupport(
            D3D12_FEATURE_COMMAND_QUEUE_PRIORITY, &support, sizeof(support));
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "D3D12 realtime COPY queue support: result=0x%08X supported=%u",
            supportResult, support.PriorityForTypeIsSupported);
        if (SUCCEEDED(supportResult) && support.PriorityForTypeIsSupported)
        {
            description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
        }
        else
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                "D3D12 realtime COPY queue unavailable; retaining NORMAL priority");
        }
    }
    HRESULT result = device->CreateCommandQueue(&description, IID_PPV_ARGS(queue));
    if (FAILED(result) &&
        description.Priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME)
    {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
            "D3D12 realtime COPY queue rejected: result=0x%08X; retrying NORMAL priority",
            result);
        description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        result = device->CreateCommandQueue(&description, IID_PPV_ARGS(queue));
    }
    if (FAILED(result))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "D3D12 COPY queue creation failed: result=0x%08X priority=%d",
            result, description.Priority);
        return result;
    }
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "D3D12 COPY queue priority: requestedRealtime=%u actual=%d",
        realtime, (*queue)->GetDesc().Priority);
    return result;
}

SwapChainThreadScheduling::SwapChainThreadScheduling(PCSTR api) noexcept
    : api_(api)
{
    ULONG enabled = 0;
    const NTSTATUS status = QueryDisplayPostProcessingEnabled(&enabled);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "Invalid MmcssDisplayPostProcessing: status=%!STATUS! value=%u; normal scheduling retained",
            status, enabled);
        return;
    }
    if (enabled == 0)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "Swapchain MMCSS disabled: api=%s", api_);
        return;
    }
    DWORD taskIndex = 0;
    registration_ = AvSetMmThreadCharacteristicsW(L"DisplayPostProcessing", &taskIndex);
    const DWORD error = registration_ == nullptr ? GetLastError() : ERROR_SUCCESS;
    TraceEvents(registration_ != nullptr ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_WARNING,
        TRACE_DRIVER,
        "Swapchain MMCSS DisplayPostProcessing: api=%s registered=%u taskIndex=%u error=%u thread=%u",
        api_, registration_ != nullptr, taskIndex, error, GetCurrentThreadId());
}

SwapChainThreadScheduling::~SwapChainThreadScheduling() noexcept
{
    if (registration_ != nullptr)
    {
        const BOOL reverted = AvRevertMmThreadCharacteristics(registration_);
        const DWORD error = reverted ? ERROR_SUCCESS : GetLastError();
        TraceEvents(reverted ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "Swapchain MMCSS reverted: api=%s success=%u error=%u thread=%u",
            api_, reverted, error, GetCurrentThreadId());
    }
}

} // namespace nv::iddcx
