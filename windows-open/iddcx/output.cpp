#include "output.hpp"
#include "adapter.hpp"
#include "display_modes.hpp"
#include "swapchain_d3d11.hpp"
#include "trace.h"

#include <cstring>
#include <new>

#include "output.tmh"

#ifndef NV_IDDCX_STATIC_GRADIENT
#define NV_IDDCX_STATIC_GRADIENT 1
#endif

namespace nv::iddcx {
namespace {

// The optional startup pattern proves scanout before the first OS frame arrives.
void FillGradient(
    void* pixels,
    NvU32 pitch,
    NvU32 width,
    NvU32 height,
    NvU32 outputIndex) noexcept
{
    std::memset(pixels, 0, static_cast<size_t>(pitch) * height);
    auto* bytes = static_cast<NvU8*>(pixels);
    for (NvU32 y = 0; y < height; ++y)
    {
        auto* row = reinterpret_cast<NvU32*>(bytes + pitch * y);
        const NvU32 green = height > 1 ? 255U * y / (height - 1) : 0;
        for (NvU32 x = 0; x < width; ++x)
        {
            const NvU32 red = width > 1 ? 255U * x / (width - 1) : 0;
            const NvU32 blue = 255U - (red + green) / 2U;
            const bool marker = ((x / 64U) ^ (y / 64U) ^ outputIndex) & 1U;
            row[x] = 0xff000000U | ((marker ? red : red / 2U) << 16) |
                ((marker ? green : green / 2U) << 8) | blue;
        }
    }
}

} // namespace

Output::Output(
        Adapter& owner,
        IDDCX_MONITOR monitor,
        const NV_KMS_WINDOWS_DISPLAY_STATE& state) noexcept
        : owner_(&owner),
            displayState_(state),
            monitor_(monitor),
            scanoutSurface_(),
            scanoutPixels_(nullptr),
            scanoutPitch_(0),
            activeModeIndex_(0),
            assignedHead_(0),
            active_(false),
            stateLock_(),
            swapChain_(nullptr)
{
}

Output::~Output() noexcept
{
    // The monitor outlives its swapchain context and scanout allocation.
    StopSwapChain();
    DestroyScanout();
    if (owner_ != nullptr)
    {
        owner_->OnOutputDestroyed(this);
        owner_ = nullptr;
    }
}

NTSTATUS Output::Create(
    Adapter& owner,
    const NV_KMS_WINDOWS_DISPLAY_STATE& state,
    Output** output) noexcept
{
    *output = nullptr;
    IDDCX_MONITOR_INFO info = {};
    info.Size = sizeof(info);
    info.MonitorType = display_modes::GetOutputTechnology(state.connectorType);
    info.ConnectorIndex = state.connectorIndex;
    info.MonitorDescription.Size = sizeof(info.MonitorDescription);
    info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    info.MonitorDescription.DataSize = state.edidBytes;
    info.MonitorDescription.pData = const_cast<NvU8*>(state.edid);
    info.MonitorContainerId = {
        0x1500c36c ^ state.displayHandle, 0x73e5, 0x4bd2,
        { 0x93, 0xa7, 0xe6, 0x8c, 0x59, 0xd4, 0xe7, 0x9c } };

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, Output);
    attributes.EvtCleanupCallback = Cleanup;
    IDARG_IN_MONITORCREATE input = {};
    input.ObjectAttributes = &attributes;
    input.pMonitorInfo = &info;
    IDARG_OUT_MONITORCREATE createOutput = {};
    NTSTATUS status = IddCxMonitorCreate(owner.Handle(), &input, &createOutput);
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "IddCxMonitorCreate: %!STATUS! monitor=%p",
        status,
        createOutput.MonitorObject);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    // IddCx allocated raw context storage; construct the non-heap Output in place.
    Output* instance = ::new (GetOutput(createOutput.MonitorObject)) Output(
        owner,
        createOutput.MonitorObject,
        state);
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "NVKMS monitor: connector=%u physical=%u EDID=%u modes=%u heads=%u layers=%u multiLayer=%u",
        state.connectorType,
        state.connectorIndex,
        state.edidBytes,
        state.modeCount,
        state.headCount,
        state.maxLayersPerHead,
        state.supportsMultiLayerFlip);
    IDARG_OUT_MONITORARRIVAL arrival = {};
    status = IddCxMonitorArrival(instance->monitor_, &arrival);
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "IddCxMonitorArrival: %!STATUS! target=%u",
        status,
        arrival.OsTargetId);
    if (!NT_SUCCESS(status))
    {
        WdfObjectDelete(instance->monitor_);
        return status;
    }
    *output = instance;
    return STATUS_SUCCESS;
}

NTSTATUS Output::PrepareScanout(
    const NV_KMS_WINDOWS_MODE& mode,
    NvU32 outputIndex,
    NV_KMS_WINDOWS_SURFACE** surface,
    void** pixels,
    NvU32* pitch) noexcept
{
    const NV_STATUS status = NvKmsWindowsCreateScanoutSurface(
        mode.hVisible, mode.vVisible, surface, pixels, pitch);
    if (status != NV_OK)
    {
        return status == NV_ERR_NO_MEMORY
            ? STATUS_INSUFFICIENT_RESOURCES
            : STATUS_DEVICE_NOT_READY;
    }
#if NV_IDDCX_STATIC_GRADIENT
    FillGradient(*pixels, *pitch, mode.hVisible, mode.vVisible, outputIndex);
#else
    std::memset(*pixels, 0, static_cast<size_t>(*pitch) * mode.vVisible);
#endif
    return STATUS_SUCCESS;
}

void Output::CommitMode(
    bool active,
    NvU32 modeIndex,
    NV_KMS_WINDOWS_SURFACE* newSurface,
    void* newPixels,
    NvU32 newPitch) noexcept
{
    // Frame processing holds the shared side through copy and present.
    auto lock = stateLock_.lock_exclusive();
    if (newSurface != nullptr)
    {
        DestroyScanout();
        scanoutSurface_.reset(newSurface);
        scanoutPixels_ = newPixels;
        scanoutPitch_ = newPitch;
    }
    else if (!active)
    {
        DestroyScanout();
    }
    active_ = active;
    activeModeIndex_ = modeIndex;
}

void Output::SetAssignedHead(NvU32 head) noexcept
{
    auto lock = stateLock_.lock_exclusive();
    assignedHead_ = head;
}

NTSTATUS Output::AssignSwapChain(
    IDDCX_MONITOR monitor,
    const IDARG_IN_SETSWAPCHAIN* arguments) noexcept
{
    if (monitor != monitor_ || arguments == nullptr || !owner_->IsReady())
    {
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }
    StopSwapChain();
    SwapChainD3D11* swapChain = nullptr;
    const NTSTATUS status = SwapChainD3D11::Create(*this, *arguments, &swapChain);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    // Publish ownership before the worker can report an asynchronous failure.
    {
        auto lock = stateLock_.lock_exclusive();
        swapChain_ = swapChain;
    }
    const HRESULT startResult = swapChain->StartProcessing();
    if (FAILED(startResult))
    {
        TraceEvents(
            TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "AssignSwapChain: worker start failed 0x%08X",
            startResult);
        StopSwapChain();
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }
    return STATUS_SUCCESS;
}

NTSTATUS Output::UnassignSwapChain(IDDCX_MONITOR monitor) noexcept
{
    if (monitor != monitor_)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    StopSwapChain();
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "UnassignSwapChain");
    return STATUS_SUCCESS;
}

void Output::OnSwapChainDestroyed(SwapChainD3D11* swapChain) noexcept
{
    auto lock = stateLock_.lock_exclusive();
    if (swapChain_ == swapChain)
    {
        swapChain_ = nullptr;
    }
}

void Output::StopSwapChain() noexcept
{
    SwapChainD3D11* swapChain = nullptr;
    {
        auto lock = stateLock_.lock_exclusive();
        swapChain = swapChain_;
        swapChain_ = nullptr;
    }
    if (swapChain != nullptr)
    {
        swapChain->StopAndDelete();
    }
}

void Output::DestroyScanout() noexcept
{
    scanoutSurface_.reset();
    scanoutPixels_ = nullptr;
    scanoutPitch_ = 0;
}

_Use_decl_annotations_
void Output::Cleanup(WDFOBJECT monitorObject)
{
    GetOutput(monitorObject)->~Output();
}

_Use_decl_annotations_
NTSTATUS GetDefaultDescriptionModes(IDDCX_MONITOR monitor,
    const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* input,
    IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* output)
{
    Output* context = GetOutput(monitor);
    if (context == nullptr || input == nullptr || output == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    return display_modes::FillMonitorModeBuffer(
        context->DisplayState(),
        input->DefaultMonitorModeBufferInputCount,
        input->pDefaultMonitorModes,
        IDDCX_MONITOR_MODE_ORIGIN_DRIVER,
        &output->DefaultMonitorModeBufferOutputCount,
        &output->PreferredMonitorModeIdx);
}

_Use_decl_annotations_
NTSTATUS QueryTargetModes(IDDCX_MONITOR monitor, const IDARG_IN_QUERYTARGETMODES* input,
    IDARG_OUT_QUERYTARGETMODES* output)
{
    Output* context = GetOutput(monitor);
    if (context == nullptr || input == nullptr || output == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    const auto& state = context->DisplayState();
    output->TargetModeBufferOutputCount = state.modeCount;
    if (input->TargetModeBufferInputCount == 0)
    {
        return STATUS_SUCCESS;
    }
    if (input->pTargetModes == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (input->TargetModeBufferInputCount < state.modeCount)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (UINT index = 0; index < state.modeCount; ++index)
    {
        input->pTargetModes[index] = display_modes::CreateTargetMode(state.modes[index]);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS QueryTargetModes2(IDDCX_MONITOR monitor, const IDARG_IN_QUERYTARGETMODES2* input,
    IDARG_OUT_QUERYTARGETMODES* output)
{
    Output* context = GetOutput(monitor);
    if (context == nullptr || input == nullptr || output == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    const auto& state = context->DisplayState();
    output->TargetModeBufferOutputCount = state.modeCount;
    if (input->TargetModeBufferInputCount == 0)
    {
        return STATUS_SUCCESS;
    }
    if (input->pTargetModes == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (input->TargetModeBufferInputCount < state.modeCount)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (UINT index = 0; index < state.modeCount; ++index)
    {
        input->pTargetModes[index] = display_modes::CreateTargetMode2(state.modes[index]);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN* input)
{
    Output* context = GetOutput(monitor);
    return context == nullptr
        ? STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN
        : context->AssignSwapChain(monitor, input);
}

_Use_decl_annotations_
NTSTATUS UnassignSwapChain(IDDCX_MONITOR monitor)
{
    Output* context = GetOutput(monitor);
    return context == nullptr
        ? STATUS_INVALID_DEVICE_STATE
        : context->UnassignSwapChain(monitor);
}

} // namespace nv::iddcx
