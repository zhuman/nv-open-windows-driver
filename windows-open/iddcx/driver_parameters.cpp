#include "driver_parameters.hpp"
#include "output.hpp"
#include "trace.h"
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <sddl.h>

#include "driver_parameters.tmh"

namespace nv::iddcx {

DriverParameters::~DriverParameters() noexcept { Stop(); }

NTSTATUS DriverParameters::Start() noexcept
{
    stop_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stop_) return STATUS_INSUFFICIENT_RESOURCES;
    Refresh();
    watchThread_.reset(CreateThread(nullptr, 0, WatchEntry, this, 0, nullptr));
    statusThread_.reset(CreateThread(nullptr, 0, StatusEntry, this, 0, nullptr));
    if (!watchThread_ || !statusThread_)
    {
        Stop();
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

void DriverParameters::Stop() noexcept
{
    if (stop_) SetEvent(stop_.get());
    for (auto* thread : {std::addressof(watchThread_), std::addressof(statusThread_)})
        if (*thread)
        {
            WaitForSingleObject(thread->get(), INFINITE);
            thread->reset();
        }
    if (key_)
    {
        WdfRegistryClose(key_);
        key_ = nullptr;
    }
}

SwapChainConfig DriverParameters::Snapshot() noexcept
{
    auto guard = lock_.lock_shared();
    return config_;
}

void DriverParameters::Reject(const char* reason) noexcept
{
    auto guard = lock_.lock_exclusive();
    status_ = "rejected";
    std::snprintf(message_, sizeof(message_), "%s", reason);
    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Live configuration rejected: %s", reason);
}

NTSTATUS DriverParameters::Read(SwapChainConfig* config) noexcept
{
    if (!key_)
    {
        const NTSTATUS opened = WdfDriverOpenParametersRegistryKey(
            WdfGetDriver(), KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &key_);
        if (!NT_SUCCESS(opened) && !DriverParameterAbsent(opened)) return opened;
    }
    const auto query = [&](PCWSTR name, ULONG* value) noexcept {
        NTSTATUS status = STATUS_OBJECT_NAME_NOT_FOUND;
        if (key_)
        {
            UNICODE_STRING keyName;
            RtlInitUnicodeString(&keyName, name);
            status = WdfRegistryQueryULong(key_, &keyName, value);
            if (!NT_SUCCESS(status) && !DriverParameterAbsent(status))
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                    "Cannot read parameter %ws: %!STATUS!", name, status);
        }
        if (DriverParameterAbsent(status) && std::wcscmp(name, L"D3D11ReadbackMode") == 0)
        {
            DWORD size = sizeof(*value);
            const LSTATUS legacy = RegGetValueW(HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\NVIDIA Corporation\\nv-iddcx", L"ReadbackMode",
                RRF_RT_REG_DWORD, nullptr, value, &size);
            if (legacy == ERROR_SUCCESS) return STATUS_SUCCESS;
            if (legacy != ERROR_FILE_NOT_FOUND && legacy != ERROR_PATH_NOT_FOUND)
                return legacy == ERROR_ACCESS_DENIED ? STATUS_ACCESS_DENIED : STATUS_OBJECT_TYPE_MISMATCH;
        }
        return status;
    };
    return ReadSwapChainConfig(config, query, &invalidField_);
}

void DriverParameters::Refresh() noexcept
{
    SwapChainConfig candidate;
    NTSTATUS status = Read(&candidate);
    if (NT_SUCCESS(status))
    {
        if (WaitForSingleObject(stop_.get(), 100) == WAIT_OBJECT_0) return;
        SwapChainConfig confirmed;
        status = Read(&confirmed);
        if (NT_SUCCESS(status) && candidate != confirmed) status = STATUS_DEVICE_BUSY;
    }
    if (NT_SUCCESS(status) && candidate.demo != 0)
    {
        const char* reason = nullptr;
        {
            auto guard = lock_.lock_shared();
            const auto& target = outputs_[candidate.connector];
            if (!target.present || !target.active)
                reason = "Selected MPO connector is not active";
            else if (candidate.demoPlanes + 1 > target.layers)
                reason = "Requested MPO planes exceed this head's layer budget";
            else if (candidate.cadence * 1000 > target.refreshMillihertz + 100)
                reason = "MPO cadence exceeds the active monitor refresh rate";
            else if (candidate.demo == 1 && !target.pixelAlpha)
                reason = "Selected MPO layer cannot blend per-pixel alpha";
            else if (candidate.demo == 5 && !target.lut)
                reason = "Selected MPO layer cannot use an independent input LUT";
            else if (candidate.demo == 3 && !target.csc)
                reason = "Selected MPO layer cannot apply independent CSC";
            else if (candidate.demo == 4 && !target.gain)
                reason = "Selected MPO layer cannot apply independent color gain";
        }
        if (reason) { Reject(reason); return; }
    }
    if (!NT_SUCCESS(status))
    {
        invalidRegistry_ = true;
        if (status == STATUS_KEY_DELETED && key_)
        {
            WdfRegistryClose(key_);
            key_ = nullptr;
        }
        char reason[256] = {};
        std::snprintf(reason, sizeof(reason), "%s: %ls (status 0x%08lX)",
            status == STATUS_DEVICE_BUSY ? "Incomplete or changing configuration" :
                "Invalid registry configuration",
            invalidField_ ? invalidField_ : L"Parameters key", static_cast<ULONG>(status));
        Reject(reason);
        return;
    }
    if (!Snapshot().Equivalent(candidate))
    {
        auto lifetime = outputLock_.lock_shared();
        if (candidate.demo != 0 && !outputObjects_[candidate.connector])
        {
            Reject("Selected MPO connector disappeared during validation");
            return;
        }
        for (Output* output : outputObjects_)
            if (output && FAILED(output->ValidateConfiguration(candidate)))
            {
                Reject("Candidate failed hardware/resource preflight; running configuration retained");
                return;
            }
    }
    auto guard = lock_.lock_exclusive();
    bool retryNative = false;
    if (candidate.Mode() != 0 && candidate.revision != config_.revision)
        for (const auto& output : outputs_)
            retryNative = retryNative || (output.present && output.active && output.api == 11);
    const bool changed = !config_.Equivalent(candidate) || retryNative;
    if (!changed && candidate.revision == config_.revision && config_.generation != 0 && !invalidRegistry_)
        return;
    invalidRegistry_ = false;
    if (config_.generation == MaximumDword && changed)
    {
        status_ = "rejected";
        std::snprintf(message_, sizeof(message_), "Configuration generation exhausted; reload the driver");
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Configuration generation exhausted");
        return;
    }
    candidate.generation = config_.generation + (changed || config_.generation == 0 ? 1 : 0);
    config_ = candidate;
    status_ = changed ? "pending" : "accepted";
    std::snprintf(message_, sizeof(message_), "%s", changed ?
        "Valid configuration awaiting swapchain recreation" : "Valid configuration");
    if (changed)
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "Live configuration accepted: generation=%u revision=%u mode=%u demo=%u",
            config_.generation, config_.revision, config_.Mode(), config_.demo);
    else
        UpdateStatus();
}

DWORD WINAPI DriverParameters::WatchEntry(void* context) noexcept
{
    static_cast<DriverParameters*>(context)->Watch();
    return 0;
}

void DriverParameters::Watch() noexcept
{
    wil::unique_handle changed(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    wil::unique_handle legacyChanged(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    wil::unique_hkey legacyKey;
    if (!changed || !legacyChanged) { Reject("Cannot create registry notification event"); return; }
    bool armed = false;
    bool legacyArmed = false;
    for (;;)
    {
        if (key_ && !armed)
        {
            const LSTATUS status = RegNotifyChangeKeyValue(
                static_cast<HKEY>(WdfRegistryWdmGetHandle(key_)), FALSE,
                REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC, changed.get(), TRUE);
            armed = status == ERROR_SUCCESS;
            if (!armed)
            {
                Reject("Registry notification failed; retrying key discovery");
                WdfRegistryClose(key_);
                key_ = nullptr;
            }
        }
        if (!legacyKey)
        {
            const LSTATUS opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\NVIDIA Corporation\\nv-iddcx", 0, KEY_READ, legacyKey.put());
            if (opened != ERROR_SUCCESS && opened != ERROR_FILE_NOT_FOUND && opened != ERROR_PATH_NOT_FOUND)
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                    "Cannot watch legacy ReadbackMode key: %u", opened);
        }
        if (legacyKey && !legacyArmed)
        {
            const LSTATUS status = RegNotifyChangeKeyValue(legacyKey.get(), FALSE,
                REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC, legacyChanged.get(), TRUE);
            legacyArmed = status == ERROR_SUCCESS;
            if (!legacyArmed)
            {
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                    "Legacy parameter notification failed: %u", status);
                legacyKey.reset();
            }
        }
        const HANDLE events[] = {stop_.get(), changed.get(), legacyChanged.get()};
        const DWORD wait = WaitForMultipleObjects(3, events, FALSE, 500);
        if (wait == WAIT_OBJECT_0) return;
        if (wait == WAIT_FAILED) { Reject("Registry notification wait failed"); return; }
        if (wait == WAIT_OBJECT_0 + 1)
        {
            armed = false;
            if (WaitForSingleObject(stop_.get(), 100) == WAIT_OBJECT_0) return;
            // Rearm before reading so a second edit cannot disappear during validation.
            if (key_)
            {
                const LSTATUS status = RegNotifyChangeKeyValue(
                    static_cast<HKEY>(WdfRegistryWdmGetHandle(key_)), FALSE,
                    REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC, changed.get(), TRUE);
                if (status != ERROR_SUCCESS) Reject("Registry notification rearm failed");
                armed = status == ERROR_SUCCESS;
            }
            if (wait == WAIT_OBJECT_0 + 2) legacyArmed = false;
        }
        Refresh();
        if (!key_) armed = false;
    }
}

void DriverParameters::Report(ULONG connector, bool active, ULONG applied, ULONG api,
    ULONG demo, ULONG layers, bool rotation, bool lut, bool gain,
    bool pixelAlpha, ULONG refreshMillihertz, bool csc, bool vrrEnabled) noexcept
{
    if (connector >= outputs_.size())
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "Status connector out of range: %u", connector);
        return;
    }
    auto guard = lock_.lock_exclusive();
    outputs_[connector] = {true, active, applied, api, demo, layers, rotation, lut, gain,
        pixelAlpha, refreshMillihertz, csc, vrrEnabled};
    UpdateStatus();
}

void DriverParameters::Applied(ULONG connector, ULONG generation) noexcept
{
    if (connector >= outputs_.size())
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Applied connector out of range: %u", connector);
        return;
    }
    auto guard = lock_.lock_exclusive();
    outputs_[connector].applied = generation;
    UpdateStatus();
}

void DriverParameters::RegisterOutput(Output& output) noexcept
{
    const auto index = output.DisplayState().connectorIndex;
    if (index >= outputObjects_.size())
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Registered connector out of range: %u", index);
        return;
    }
    auto guard = outputLock_.lock_exclusive();
    outputObjects_[index] = &output;
}

void DriverParameters::UnregisterOutput(Output& output) noexcept
{
    const auto index = output.DisplayState().connectorIndex;
    if (index >= outputObjects_.size()) return;
    auto guard = outputLock_.lock_exclusive();
    if (outputObjects_[index] == &output) outputObjects_[index] = nullptr;
}

void DriverParameters::UpdateStatus() noexcept
{
    bool active = false;
    bool limitedShowcase = false;
    bool limitedGeometry = false;
    bool unmetVrr = false;
    for (const auto& output : outputs_)
        if (output.present && output.active)
        {
            active = true;
            if (output.applied != config_.generation) return;
            limitedShowcase = limitedShowcase || (output.demo == 6 &&
                (!output.rotation || !output.lut || !output.gain || !output.csc || !output.pixelAlpha));
            limitedGeometry = limitedGeometry || (output.demo == 2 && !output.rotation);
            unmetVrr = unmetVrr || (config_.presentation.vrr != 0 && !output.vrrEnabled);
        }
    if (active)
    {
        status_ = "applied";
        std::snprintf(message_, sizeof(message_), "%s", limitedGeometry ?
            (unmetVrr ? "Applied; VRR unavailable; Geometry uses validated transforms/scales only" :
             "Geometry applied; unsupported transforms/scales are omitted") : unmetVrr && limitedShowcase ?
            "Applied; VRR is unavailable and unsupported showcase features are omitted" : unmetVrr ?
            "Applied; VRR preference is unavailable on at least one active output" : limitedShowcase ?
            "Showcase applied; unsupported features are explicitly omitted" :
            "Configuration applied to active swapchains");
    }
}

DWORD WINAPI DriverParameters::StatusEntry(void* context) noexcept
{
    static_cast<DriverParameters*>(context)->ServeStatus();
    return 0;
}

void DriverParameters::ServeStatus() noexcept
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;IU)", SDDL_REVISION_1, &descriptor, nullptr))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "Status pipe security initialization failed: %u", GetLastError());
        return;
    }
    auto release = wil::scope_exit([&]() noexcept { LocalFree(descriptor); });
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
    wil::unique_handle completed(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!completed)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "Status pipe event creation failed: %u", GetLastError());
        return;
    }
    while (WaitForSingleObject(stop_.get(), 0) != WAIT_OBJECT_0)
    {
        wil::unique_hfile pipe(CreateNamedPipeW(L"\\\\.\\pipe\\NvIddCx.Status",
            PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 8192, 0, 1000, &attributes));
        if (!pipe)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "Status pipe creation failed: %u", GetLastError());
            if (WaitForSingleObject(stop_.get(), 1000) == WAIT_OBJECT_0) return;
            continue;
        }
        OVERLAPPED operation = {};
        operation.hEvent = completed.get();
        ResetEvent(completed.get());
        const BOOL connected = ConnectNamedPipe(pipe.get(), &operation);
        const DWORD error = connected ? ERROR_SUCCESS : GetLastError();
        const HANDLE waits[] = {stop_.get(), completed.get()};
        if (error == ERROR_IO_PENDING)
        {
            const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (wait != WAIT_OBJECT_0 + 1)
            {
                CancelIoEx(pipe.get(), &operation);
                WaitForSingleObject(completed.get(), INFINITE);
                if (wait == WAIT_FAILED)
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                        "Status pipe connection wait failed: %u", GetLastError());
                return;
            }
            DWORD transferred = 0;
            if (!GetOverlappedResult(pipe.get(), &operation, &transferred, FALSE))
            {
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                    "Status client connection completion failed: %u", GetLastError());
                continue;
            }
        }
        else if (error != ERROR_SUCCESS && error != ERROR_PIPE_CONNECTED)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "Status pipe connection failed: %u", error);
            continue;
        }
        char json[8192] = {};
        size_t length = 0;
        bool overflow = false;
        const auto append = [&](const char* format, auto... args) noexcept {
            if (overflow) return;
            const int count = std::snprintf(json + length, sizeof(json) - length, format, args...);
            if (count < 0 || static_cast<size_t>(count) >= sizeof(json) - length)
                overflow = true;
            else
                length += static_cast<size_t>(count);
        };
        {
            auto guard = lock_.lock_shared();
            append(
                "{\"generation\":%lu,\"revision\":%lu,\"status\":\"%s\",\"message\":\"%s\",\"outputs\":[",
                config_.generation, config_.revision, status_, message_);
            bool first = true;
            for (ULONG index = 0; index < outputs_.size(); ++index)
            {
                const auto& output = outputs_[index];
                if (!output.present) continue;
                append(
                    "%s{\"connectorIndex\":%lu,\"active\":%s,\"appliedGeneration\":%lu,\"api\":%lu,\"demo\":%lu,"
                    "\"layers\":%lu,\"rotation\":%s,\"lut\":%s,\"gain\":%s,\"vrrEnabled\":%s}",
                    first ? "" : ",", index, output.active ? "true" : "false", output.applied,
                    output.api, output.demo, output.layers, output.rotation ? "true" : "false",
                    output.lut ? "true" : "false", output.gain ? "true" : "false",
                    output.vrrEnabled ? "true" : "false");
                first = false;
            }
            append("]}\n");
        }
        if (overflow || length == 0)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Status pipe payload overflow");
            return;
        }
        ResetEvent(completed.get());
        operation = {};
        operation.hEvent = completed.get();
        DWORD written = 0;
        if (!WriteFile(pipe.get(), json, static_cast<DWORD>(length), &written, &operation))
        {
            if (GetLastError() == ERROR_IO_PENDING)
            {
                const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 1000);
                if (wait != WAIT_OBJECT_0 + 1) CancelIoEx(pipe.get(), &operation);
                if (!GetOverlappedResult(pipe.get(), &operation, &written, TRUE))
                    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                        "Status client write completion failed: %u", GetLastError());
            }
            else
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                    "Status client write failed: %u", GetLastError());
        }
    }
}

} // namespace nv::iddcx
