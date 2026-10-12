#pragma once

#include <windows.h>
#include <wdf.h>
#include <wil/resource.h>
#include <array>

namespace nv::iddcx {

class Output;
inline constexpr ULONG MaximumDword = ~ULONG{0};

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
    bool operator==(const PresentationPolicy&) const noexcept = default;
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

struct SwapChainConfig
{
    ULONG api = 0;
    ULONG nativeMode = 1;
    ULONG readback = 0;
    ULONG cache = 0;
    ULONG dirty = 1;
    ULONG debug = 0;
    ULONG priority = 1;
    ULONG mmcss = 1;
    PresentationPolicy presentation;
    ULONG demo = 0;
    ULONG demoPlanes = 1;
    ULONG connector = MaximumDword;
    ULONG cadence = 30;
    ULONG speed = 100;
    ULONG revision = 0;
    ULONG generation = 0;

    ULONG Mode() const noexcept
    {
        return api == 11 ? 0 : api == 12 && nativeMode == 0 ? 1 : nativeMode;
    }
    bool HonorCache() const noexcept { return cache == 2 || (cache == 0 && Mode() != 0); }
    bool operator==(const SwapChainConfig&) const noexcept = default;
    bool Equivalent(const SwapChainConfig& other) const noexcept
    {
        return Mode() == other.Mode() && HonorCache() == other.HonorCache() &&
            (Mode() != 0 || readback == other.readback) &&
            (Mode() == 0 || debug == other.debug) && dirty == other.dirty &&
            priority == other.priority && mmcss == other.mmcss &&
            presentation == other.presentation && demo == other.demo && (demo == 0 ||
                (demoPlanes == other.demoPlanes && connector == other.connector &&
                 cadence == other.cadence && speed == other.speed));
    }
};

template <typename Query>
NTSTATUS ReadSwapChainConfig(SwapChainConfig* config, Query&& query,
    PCWSTR* invalidField = nullptr) noexcept
{
    if (invalidField) *invalidField = L"ConfigRevision";
    SwapChainConfig candidate;
    struct Field { PCWSTR name; ULONG* value; ULONG fallback; ULONG minimum; ULONG maximum; };
    const Field fields[] = {
        {L"SwapChainApi", &candidate.api, 0, 0, 12},
        {L"D3D12Mode", &candidate.nativeMode, 1, 0, 3},
        {L"D3D11ReadbackMode", &candidate.readback, 0, 0, 2},
        {L"SurfaceCachePolicy", &candidate.cache, 0, 0, 2},
        {L"DirtyRects", &candidate.dirty, 1, 0, 1},
        {L"D3D12Debug", &candidate.debug, 0, 0, 1},
        {L"RealtimeGpuPriority", &candidate.priority, 1, 0, 1},
        {L"MmcssDisplayPostProcessing", &candidate.mmcss, 1, 0, 1},
        {L"MpoDemoMode", &candidate.demo, 0, 0, 6},
        {L"MpoDemoPlaneCount", &candidate.demoPlanes, 1, 1, 7},
        {L"MpoDemoConnectorIndex", &candidate.connector, MaximumDword, 0, MaximumDword},
        {L"MpoDemoCadenceHz", &candidate.cadence, 30, 15, 60},
        {L"MpoDemoSpeedPercent", &candidate.speed, 100, 25, 200},
    };
    NTSTATUS status = QueryDriverParameterDwordOrDefault(
        L"ConfigRevision", 0, &candidate.revision, query);
    if (!NT_SUCCESS(status)) return status;
    if (candidate.revision & 1) return STATUS_DEVICE_BUSY;
    for (const auto& field : fields)
    {
        if (invalidField) *invalidField = field.name;
        status = QueryDriverParameterDwordOrDefault(field.name, field.fallback, field.value, query);
        if (!NT_SUCCESS(status)) return status;
        if (*field.value < field.minimum || *field.value > field.maximum)
            return STATUS_INVALID_PARAMETER;
    }
    if (invalidField) *invalidField = L"Presentation policy";
    status = QueryPresentationPolicy(&candidate.presentation, query);
    if (!NT_SUCCESS(status)) return status;
    ULONG revision = 0;
    if (invalidField) *invalidField = L"ConfigRevision";
    status = QueryDriverParameterDwordOrDefault(L"ConfigRevision", 0, &revision, query);
    if (!NT_SUCCESS(status)) return status;
    if (revision != candidate.revision || (revision & 1)) return STATUS_DEVICE_BUSY;
    if (candidate.api != 0 && candidate.api != 11 && candidate.api != 12)
    {
        if (invalidField) *invalidField = L"SwapChainApi";
        return STATUS_INVALID_PARAMETER;
    }
    if (candidate.cadence != 15 && candidate.cadence != 30 && candidate.cadence != 60)
    {
        if (invalidField) *invalidField = L"MpoDemoCadenceHz";
        return STATUS_INVALID_PARAMETER;
    }
    if (candidate.demo != 0 && candidate.connector >= 16)
    {
        if (invalidField) *invalidField = L"MpoDemoConnectorIndex";
        return STATUS_INVALID_PARAMETER;
    }
    if (invalidField) *invalidField = nullptr;
    *config = candidate;
    return STATUS_SUCCESS;
}

class DriverParameters final
{
public:
    ~DriverParameters() noexcept;
    NTSTATUS Start() noexcept;
    void Stop() noexcept;
    SwapChainConfig Snapshot() noexcept;
    void Report(ULONG connector, bool active, ULONG applied, ULONG api,
        ULONG demo, ULONG layers, bool rotation, bool lut, bool gain,
        bool pixelAlpha = false, ULONG refreshMillihertz = 0, bool csc = false,
        bool vrrEnabled = false) noexcept;
    void Reject(const char* reason) noexcept;
    void Applied(ULONG connector, ULONG generation) noexcept;
    void RegisterOutput(Output& output) noexcept;
    void UnregisterOutput(Output& output) noexcept;

private:
    static DWORD WINAPI WatchEntry(void* context) noexcept;
    static DWORD WINAPI StatusEntry(void* context) noexcept;
    void Watch() noexcept;
    void ServeStatus() noexcept;
    NTSTATUS Read(SwapChainConfig* config) noexcept;
    void Refresh() noexcept;
    void UpdateStatus() noexcept;
    wil::srwlock lock_;
    wil::srwlock outputLock_;
    std::array<Output*, 16> outputObjects_ = {};
    SwapChainConfig config_;
    const char* status_ = "starting";
    char message_[256] = "Driver starting";
    struct OutputStatus
    {
        bool present = false;
        bool active = false;
        ULONG applied = 0;
        ULONG api = 0;
        ULONG demo = 0;
        ULONG layers = 0;
        bool rotation = false;
        bool lut = false;
        bool gain = false;
        bool pixelAlpha = false;
        ULONG refreshMillihertz = 0;
        bool csc = false;
        bool vrrEnabled = false;
    };
    std::array<OutputStatus, 16> outputs_{};
    wil::unique_handle stop_;
    wil::unique_handle watchThread_;
    wil::unique_handle statusThread_;
    WDFKEY key_ = nullptr;
    PCWSTR invalidField_ = nullptr;
    bool invalidRegistry_ = false;
};

DriverParameters& Parameters() noexcept;

} // namespace nv::iddcx
