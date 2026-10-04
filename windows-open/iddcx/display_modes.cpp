#include "display_modes.hpp"
#include "trace.h"

#include <cstring>

#include "display_modes.tmh"

namespace nv::iddcx::display_modes {

void FillSignalInfo(
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO* info,
    const NV_KMS_WINDOWS_MODE& mode,
    bool monitorMode) noexcept
{
    info->pixelRate = mode.pixelClockHz;
    info->hSyncFreq = { mode.pixelClockHz, mode.hTotal };
    info->vSyncFreq = { mode.refreshRateMillihertz, 1000 };
    info->activeSize.cx = mode.hVisible;
    info->activeSize.cy = mode.vVisible;
    info->totalSize.cx = mode.hTotal;
    info->totalSize.cy = mode.vTotal;
    info->videoStandard = 255;
    info->scanLineOrdering = mode.interlaced
        ? DISPLAYCONFIG_SCANLINE_ORDERING_INTERLACED_UPPERFIELDFIRST
        : DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    info->AdditionalSignalInfo.vSyncFreqDivider = monitorMode ? 0 : 1;
}

IDDCX_MONITOR_MODE CreateMonitorMode(
    const NV_KMS_WINDOWS_MODE& displayMode,
    IDDCX_MONITOR_MODE_ORIGIN origin) noexcept
{
    IDDCX_MONITOR_MODE mode = {};
    mode.Size = sizeof(mode);
    mode.Origin = origin;
    FillSignalInfo(&mode.MonitorVideoSignalInfo, displayMode, true);
    return mode;
}

IDDCX_MONITOR_MODE2 CreateMonitorMode2(
    const NV_KMS_WINDOWS_MODE& displayMode,
    IDDCX_MONITOR_MODE_ORIGIN origin) noexcept
{
    IDDCX_MONITOR_MODE2 mode = {};
    mode.Size = sizeof(mode);
    mode.Origin = origin;
    FillSignalInfo(&mode.MonitorVideoSignalInfo, displayMode, true);
    mode.BitsPerComponent.Rgb = IDDCX_BITS_PER_COMPONENT_8;
    return mode;
}

IDDCX_TARGET_MODE CreateTargetMode(const NV_KMS_WINDOWS_MODE& displayMode) noexcept
{
    IDDCX_TARGET_MODE mode = {};
    mode.Size = sizeof(mode);
    FillSignalInfo(
        &mode.TargetVideoSignalInfo.targetVideoSignalInfo,
        displayMode,
        false);
    return mode;
}

IDDCX_TARGET_MODE2 CreateTargetMode2(const NV_KMS_WINDOWS_MODE& displayMode) noexcept
{
    IDDCX_TARGET_MODE2 mode = {};
    mode.Size = sizeof(mode);
    FillSignalInfo(
        &mode.TargetVideoSignalInfo.targetVideoSignalInfo,
        displayMode,
        false);
    mode.RequiredBandwidth =
        mode.TargetVideoSignalInfo.targetVideoSignalInfo.pixelRate;
    mode.BitsPerComponent.Rgb = IDDCX_BITS_PER_COMPONENT_8;
    return mode;
}

DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY GetOutputTechnology(NvU32 connectorType) noexcept
{
    switch (connectorType)
    {
    case 0:
    case 7:
        return DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL;
    case 1:
        return DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15;
    case 2:
    case 3:
        return DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI;
    case 5:
        return DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS;
    case 6:
        return DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    default:
        return DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER;
    }
}

NTSTATUS GetDisplayState(NvU32 index, NV_KMS_WINDOWS_DISPLAY_STATE* state) noexcept
{
    const NV_STATUS status = NvKmsWindowsGetDisplay(index, state);
    if (status == NV_OK)
    {
        return STATUS_SUCCESS;
    }
    TraceEvents(
        TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "NvKmsWindowsGetDisplay[%u] failed: 0x%08X",
        index,
        status);
    return STATUS_DEVICE_NOT_READY;
}

NTSTATUS GetDisplayState(
    const IDDCX_MONITOR_DESCRIPTION& description,
    NV_KMS_WINDOWS_DISPLAY_STATE* state) noexcept
{
    NvU32 count = 0;
    if (description.pData == nullptr || NvKmsWindowsGetConnectedDisplayCount(&count) != NV_OK)
    {
        return STATUS_DEVICE_NOT_READY;
    }
    for (NvU32 index = 0; index < count; ++index)
    {
        NV_KMS_WINDOWS_DISPLAY_STATE candidate = {};
        if (NvKmsWindowsGetDisplay(index, &candidate) == NV_OK &&
            candidate.edidBytes == description.DataSize &&
            std::memcmp(candidate.edid, description.pData, candidate.edidBytes) == 0)
        {
            *state = candidate;
            return STATUS_SUCCESS;
        }
    }
    return STATUS_NOT_FOUND;
}

NTSTATUS FillMonitorModeBuffer(
    const NV_KMS_WINDOWS_DISPLAY_STATE& state,
    UINT inputCount,
    IDDCX_MONITOR_MODE* modes,
    IDDCX_MONITOR_MODE_ORIGIN origin,
    UINT* outputCount,
    UINT* preferredIndex) noexcept
{
    *outputCount = state.modeCount;
    *preferredIndex = state.preferredModeIndex < state.modeCount
        ? state.preferredModeIndex
        : 0;
    if (inputCount == 0)
    {
        return STATUS_SUCCESS;
    }
    if (modes == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (inputCount < state.modeCount)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (UINT index = 0; index < state.modeCount; ++index)
    {
        modes[index] = CreateMonitorMode(state.modes[index], origin);
    }
    return STATUS_SUCCESS;
}

NTSTATUS FillMonitorMode2Buffer(
    const NV_KMS_WINDOWS_DISPLAY_STATE& state,
    UINT inputCount,
    IDDCX_MONITOR_MODE2* modes,
    UINT* outputCount,
    UINT* preferredIndex) noexcept
{
    *outputCount = state.modeCount;
    *preferredIndex = state.preferredModeIndex < state.modeCount
        ? state.preferredModeIndex
        : 0;
    if (inputCount == 0)
    {
        return STATUS_SUCCESS;
    }
    if (modes == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (inputCount < state.modeCount)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (UINT index = 0; index < state.modeCount; ++index)
    {
        modes[index] = CreateMonitorMode2(
            state.modes[index],
            IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
    }
    return STATUS_SUCCESS;
}

const NV_KMS_WINDOWS_MODE* FindDisplayMode(
    const NV_KMS_WINDOWS_DISPLAY_STATE& state,
    const DISPLAYCONFIG_VIDEO_SIGNAL_INFO& info) noexcept
{
    for (NvU32 index = 0; index < state.modeCount; ++index)
    {
        const auto& mode = state.modes[index];
        if (mode.pixelClockHz == info.pixelRate && mode.hVisible == info.activeSize.cx &&
            mode.vVisible == info.activeSize.cy && mode.hTotal == info.totalSize.cx &&
            mode.vTotal == info.totalSize.cy)
        {
            return &mode;
        }
    }
    return nullptr;
}

const NV_KMS_WINDOWS_MODE* FindDisplayMode(
    const NV_KMS_WINDOWS_DISPLAY_STATE& state,
    NvU32 modeIndex) noexcept
{
    for (NvU32 index = 0; index < state.modeCount; ++index)
    {
        if (state.modes[index].kmsModeIndex == modeIndex)
        {
            return &state.modes[index];
        }
    }
    return nullptr;
}

} // namespace nv::iddcx::display_modes

namespace nv::iddcx {

_Use_decl_annotations_
NTSTATUS ParseMonitorDescription(
    const IDARG_IN_PARSEMONITORDESCRIPTION* input,
    IDARG_OUT_PARSEMONITORDESCRIPTION* output)
{
    if (input == nullptr || output == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    NV_KMS_WINDOWS_DISPLAY_STATE state = {};
    NTSTATUS status = display_modes::GetDisplayState(input->MonitorDescription, &state);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    return display_modes::FillMonitorModeBuffer(
        state,
        input->MonitorModeBufferInputCount,
        input->pMonitorModes,
        IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR,
        &output->MonitorModeBufferOutputCount,
        &output->PreferredMonitorModeIdx);
}

_Use_decl_annotations_
NTSTATUS ParseMonitorDescription2(
    const IDARG_IN_PARSEMONITORDESCRIPTION2* input,
    IDARG_OUT_PARSEMONITORDESCRIPTION* output)
{
    if (input == nullptr || output == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    NV_KMS_WINDOWS_DISPLAY_STATE state = {};
    NTSTATUS status = display_modes::GetDisplayState(input->MonitorDescription, &state);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    return display_modes::FillMonitorMode2Buffer(
        state,
        input->MonitorModeBufferInputCount,
        input->pMonitorModes,
        &output->MonitorModeBufferOutputCount,
        &output->PreferredMonitorModeIdx);
}

} // namespace nv::iddcx
