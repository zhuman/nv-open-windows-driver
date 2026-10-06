#pragma once

#include <windows.h>
#include <wdf.h>
#include <wil/resource.h>

namespace nv::iddcx {

inline NTSTATUS QueryDriverParameterDword(PCWSTR name, ULONG* value) noexcept
{
    WDFKEY key = nullptr;
    const NTSTATUS status = WdfDriverOpenParametersRegistryKey(
        WdfGetDriver(), KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    auto close = wil::scope_exit([&]() noexcept { WdfRegistryClose(key); });
    UNICODE_STRING valueName = {};
    RtlInitUnicodeString(&valueName, name);
    return WdfRegistryQueryULong(key, &valueName, value);
}

inline bool DriverParameterAbsent(NTSTATUS status) noexcept
{
    return status == STATUS_NO_SUCH_FILE ||
        status == STATUS_OBJECT_NAME_NOT_FOUND ||
        status == STATUS_OBJECT_PATH_NOT_FOUND;
}

template <typename Query>
inline NTSTATUS QueryDriverParameterDwordOrDefault(
    PCWSTR name, ULONG defaultValue, ULONG* value, Query&& query) noexcept
{
    const NTSTATUS status = query(name, value);
    if (DriverParameterAbsent(status))
    {
        *value = defaultValue;
        return STATUS_SUCCESS;
    }
    return status;
}

template <typename Query>
inline NTSTATUS QueryDirtyRectsEnabled(ULONG* enabled, Query&& query) noexcept
{
    const NTSTATUS status = QueryDriverParameterDwordOrDefault(
        L"DirtyRects", 1, enabled, query);
    return NT_SUCCESS(status) && *enabled > 1 ? STATUS_INVALID_PARAMETER : status;
}

inline NTSTATUS QueryDirtyRectsEnabled(ULONG* enabled) noexcept
{
    return QueryDirtyRectsEnabled(enabled, QueryDriverParameterDword);
}

template <typename Query>
inline NTSTATUS QueryRealtimeGpuPriorityEnabled(ULONG* enabled, Query&& query) noexcept
{
    const NTSTATUS status = QueryDriverParameterDwordOrDefault(
        L"RealtimeGpuPriority", 1, enabled, query);
    return NT_SUCCESS(status) && *enabled > 1 ? STATUS_INVALID_PARAMETER : status;
}

inline NTSTATUS QueryRealtimeGpuPriorityEnabled(ULONG* enabled) noexcept
{
    return QueryRealtimeGpuPriorityEnabled(enabled, QueryDriverParameterDword);
}

template <typename Query>
inline NTSTATUS QueryDisplayPostProcessingEnabled(ULONG* enabled, Query&& query) noexcept
{
    const NTSTATUS status = QueryDriverParameterDwordOrDefault(
        L"MmcssDisplayPostProcessing", 1, enabled, query);
    return NT_SUCCESS(status) && *enabled > 1 ? STATUS_INVALID_PARAMETER : status;
}

inline NTSTATUS QueryDisplayPostProcessingEnabled(ULONG* enabled) noexcept
{
    return QueryDisplayPostProcessingEnabled(enabled, QueryDriverParameterDword);
}

struct PresentationPolicy
{
    ULONG vrr = 1;
    ULONG flip = 1;
    ULONG pipeline = 1;
    ULONG buffers = 3;
};

template <typename Query>
inline NTSTATUS QueryPresentationPolicy(PresentationPolicy* policy, Query&& query) noexcept
{
    PresentationPolicy result;
    NTSTATUS status = QueryDriverParameterDwordOrDefault(L"VrrPreference", 1, &result.vrr, query);
    if (!NT_SUCCESS(status) || result.vrr > 1)
        return NT_SUCCESS(status) ? STATUS_INVALID_PARAMETER : status;
    status = QueryDriverParameterDwordOrDefault(L"FlipMode", 1, &result.flip, query);
    if (!NT_SUCCESS(status) || result.flip > 1)
        return NT_SUCCESS(status) ? STATUS_INVALID_PARAMETER : status;
    status = QueryDriverParameterDwordOrDefault(L"FramePipeline", 1, &result.pipeline, query);
    if (!NT_SUCCESS(status) || result.pipeline > 1)
        return NT_SUCCESS(status) ? STATUS_INVALID_PARAMETER : status;
    status = QueryDriverParameterDwordOrDefault(L"ScanoutBufferCount", 3, &result.buffers, query);
    if (!NT_SUCCESS(status) || result.buffers < 2 || result.buffers > 4)
        return NT_SUCCESS(status) ? STATUS_INVALID_PARAMETER : status;
    *policy = result;
    return STATUS_SUCCESS;
}

inline NTSTATUS QueryPresentationPolicy(PresentationPolicy* policy) noexcept
{
    return QueryPresentationPolicy(policy, QueryDriverParameterDword);
}

template <typename Query>
inline NTSTATUS QuerySwapChainMode(ULONG* mode, Query&& query) noexcept
{
    ULONG api = 0;
    ULONG nativeMode = 0;
    NTSTATUS status = QueryDriverParameterDwordOrDefault(L"SwapChainApi", 0, &api, query);
    if (!NT_SUCCESS(status) ||
        (api != 0 && api != 11 && api != 12))
    {
        return NT_SUCCESS(status) ? STATUS_INVALID_PARAMETER : status;
    }
    status = QueryDriverParameterDwordOrDefault(L"D3D12Mode", 1, &nativeMode, query);
    if (!NT_SUCCESS(status) || nativeMode > 3)
    {
        return NT_SUCCESS(status) ? STATUS_INVALID_PARAMETER : status;
    }
    *mode = api == 11 ? 0 : api == 12 && nativeMode == 0 ? 1 : nativeMode;
    return STATUS_SUCCESS;
}

inline NTSTATUS QuerySwapChainMode(ULONG* mode) noexcept
{
    return QuerySwapChainMode(mode, QueryDriverParameterDword);
}

} // namespace nv::iddcx
