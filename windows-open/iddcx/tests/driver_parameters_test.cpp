#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include "..\driver_parameters.hpp"

#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace {

void Require(bool value)
{
    if (!value)
    {
        std::fputs("Driver-parameter test failed\n", stderr);
        std::abort();
    }
}

struct Parameters
{
    ULONG api = 0;
    ULONG mode = 0;
    ULONG dirty = 0;
    ULONG priority = 0;
    ULONG mmcss = 0;
    NTSTATUS apiStatus = STATUS_OBJECT_NAME_NOT_FOUND;
    NTSTATUS modeStatus = STATUS_OBJECT_NAME_NOT_FOUND;
    NTSTATUS dirtyStatus = STATUS_OBJECT_NAME_NOT_FOUND;
    NTSTATUS priorityStatus = STATUS_OBJECT_NAME_NOT_FOUND;
    NTSTATUS mmcssStatus = STATUS_OBJECT_NAME_NOT_FOUND;

    NTSTATUS operator()(PCWSTR name, ULONG* value) const noexcept
    {
        if (std::wcscmp(name, L"SwapChainApi") == 0)
        {
            *value = api;
            return apiStatus;
        }
        if (std::wcscmp(name, L"D3D12Mode") == 0)
        {
            *value = mode;
            return modeStatus;
        }
        if (std::wcscmp(name, L"RealtimeGpuPriority") == 0)
        {
            *value = priority;
            return priorityStatus;
        }
        if (std::wcscmp(name, L"MmcssDisplayPostProcessing") == 0)
        {
            *value = mmcss;
            return mmcssStatus;
        }
        Require(std::wcscmp(name, L"DirtyRects") == 0);
        *value = dirty;
        return dirtyStatus;
    }
};

void ExpectMode(const Parameters& parameters, ULONG expected)
{
    ULONG mode = 99;
    Require(nv::iddcx::QuerySwapChainMode(&mode, parameters) == STATUS_SUCCESS);
    Require(mode == expected);
}

void ExpectDirty(const Parameters& parameters, ULONG expected)
{
    ULONG enabled = 99;
    Require(nv::iddcx::QueryDirtyRectsEnabled(&enabled, parameters) == STATUS_SUCCESS);
    Require(enabled == expected);
}

void ExpectPriority(const Parameters& parameters, ULONG expected)
{
    ULONG enabled = 99;
    Require(nv::iddcx::QueryRealtimeGpuPriorityEnabled(&enabled, parameters) == STATUS_SUCCESS);
    Require(enabled == expected);
}

void ExpectMmcss(const Parameters& parameters, ULONG expected)
{
    ULONG enabled = 99;
    Require(nv::iddcx::QueryDisplayPostProcessingEnabled(&enabled, parameters) == STATUS_SUCCESS);
    Require(enabled == expected);
}

}

int main()
{
    for (const NTSTATUS absent : {STATUS_OBJECT_NAME_NOT_FOUND,
        STATUS_OBJECT_PATH_NOT_FOUND, STATUS_NO_SUCH_FILE})
    {
        nv::iddcx::PresentationPolicy policy;
        const auto query = [absent](PCWSTR, ULONG* value) noexcept {
            *value = 99;
            return absent;
        };
        Require(nv::iddcx::QueryPresentationPolicy(&policy, query) == STATUS_SUCCESS);
        Require(policy.vrr == 1 && policy.flip == 1 && policy.pipeline == 1 && policy.buffers == 3);
    }
    for (const PCWSTR name : {L"VrrPreference", L"FlipMode", L"FramePipeline", L"ScanoutBufferCount"})
    {
        for (ULONG value = 0; value <= 5; ++value)
        {
            nv::iddcx::PresentationPolicy policy;
            const auto query = [name, value](PCWSTR key, ULONG* output) noexcept {
                *output = value;
                return std::wcscmp(key, name) == 0 ? STATUS_SUCCESS : STATUS_NO_SUCH_FILE;
            };
            const bool valid = std::wcscmp(name, L"ScanoutBufferCount") == 0 ?
                value >= 2 && value <= 4 : value <= 1;
            Require(nv::iddcx::QueryPresentationPolicy(&policy, query) ==
                (valid ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER));
            if (valid)
            {
                Require(policy.vrr == (std::wcscmp(name, L"VrrPreference") == 0 ? value : 1));
                Require(policy.flip == (std::wcscmp(name, L"FlipMode") == 0 ? value : 1));
                Require(policy.pipeline == (std::wcscmp(name, L"FramePipeline") == 0 ? value : 1));
                Require(policy.buffers == (std::wcscmp(name, L"ScanoutBufferCount") == 0 ? value : 3));
            }
        }
        for (const NTSTATUS error : {STATUS_ACCESS_DENIED, STATUS_OBJECT_TYPE_MISMATCH})
        {
            nv::iddcx::PresentationPolicy policy;
            const auto query = [name, error](PCWSTR key, ULONG* value) noexcept {
                *value = 99;
                return std::wcscmp(key, name) == 0 ? error : STATUS_NO_SUCH_FILE;
            };
            Require(nv::iddcx::QueryPresentationPolicy(&policy, query) == error);
            Require(policy.vrr == 1 && policy.flip == 1 && policy.pipeline == 1 && policy.buffers == 3);
        }
    }
    std::puts("PASS: VRR/flip/pipeline/buffer policies, transactional errors, and missing-key defaults");
    Parameters parameters;
    ExpectMode(parameters, 1);
    ExpectDirty(parameters, 1);
    ExpectPriority(parameters, 1);
    ExpectMmcss(parameters, 1);
    for (const NTSTATUS absent : {STATUS_OBJECT_NAME_NOT_FOUND,
        STATUS_OBJECT_PATH_NOT_FOUND, STATUS_NO_SUCH_FILE})
    {
        parameters.priorityStatus = absent;
        parameters.priority = 99;
        ExpectPriority(parameters, 1);
        parameters.mmcssStatus = absent;
        parameters.mmcss = 99;
        ExpectMmcss(parameters, 1);
    }
    parameters.priorityStatus = STATUS_SUCCESS;
    parameters.mmcssStatus = STATUS_SUCCESS;
    for (ULONG priority = 0; priority <= 1; ++priority)
    {
        parameters.priority = priority;
        ExpectPriority(parameters, priority);
        parameters.mmcss = priority;
        ExpectMmcss(parameters, priority);
    }
    ULONG priority = 99;
    parameters.priority = 2;
    Require(nv::iddcx::QueryRealtimeGpuPriorityEnabled(&priority, parameters) == STATUS_INVALID_PARAMETER);
    parameters.mmcss = 2;
    Require(nv::iddcx::QueryDisplayPostProcessingEnabled(&priority, parameters) == STATUS_INVALID_PARAMETER);
    for (const NTSTATUS error : {STATUS_ACCESS_DENIED, STATUS_OBJECT_TYPE_MISMATCH})
    {
        parameters.priorityStatus = error;
        Require(nv::iddcx::QueryRealtimeGpuPriorityEnabled(&priority, parameters) == error);
        parameters.mmcssStatus = error;
        Require(nv::iddcx::QueryDisplayPostProcessingEnabled(&priority, parameters) == error);
    }
    parameters.apiStatus = parameters.modeStatus = parameters.dirtyStatus =
        STATUS_OBJECT_PATH_NOT_FOUND;
    parameters.api = parameters.mode = parameters.dirty = 99;
    ExpectMode(parameters, 1);
    ExpectDirty(parameters, 1);
    parameters.apiStatus = parameters.modeStatus = parameters.dirtyStatus =
        STATUS_NO_SUCH_FILE;
    ExpectMode(parameters, 1);
    ExpectDirty(parameters, 1);

    parameters.apiStatus = parameters.modeStatus = parameters.dirtyStatus = STATUS_SUCCESS;
    for (const ULONG api : {0UL, 11UL, 12UL})
    {
        parameters.api = api;
        for (ULONG mode = 0; mode <= 3; ++mode)
        {
            parameters.mode = mode;
            ExpectMode(parameters, api == 11 ? 0 : api == 12 && mode == 0 ? 1 : mode);
        }
        parameters.modeStatus = STATUS_OBJECT_NAME_NOT_FOUND;
        ExpectMode(parameters, api == 11 ? 0 : 1);
        parameters.modeStatus = STATUS_SUCCESS;
    }
    for (ULONG dirty = 0; dirty <= 1; ++dirty)
    {
        parameters.dirty = dirty;
        ExpectDirty(parameters, dirty);
    }

    ULONG value = 99;
    parameters.api = 13;
    Require(nv::iddcx::QuerySwapChainMode(&value, parameters) == STATUS_INVALID_PARAMETER);
    parameters.api = 0;
    parameters.mode = 4;
    Require(nv::iddcx::QuerySwapChainMode(&value, parameters) == STATUS_INVALID_PARAMETER);
    parameters.dirty = 2;
    Require(nv::iddcx::QueryDirtyRectsEnabled(&value, parameters) == STATUS_INVALID_PARAMETER);
    parameters.apiStatus = STATUS_ACCESS_DENIED;
    Require(nv::iddcx::QuerySwapChainMode(&value, parameters) == STATUS_ACCESS_DENIED);
    parameters.apiStatus = STATUS_SUCCESS;
    parameters.modeStatus = STATUS_OBJECT_TYPE_MISMATCH;
    Require(nv::iddcx::QuerySwapChainMode(&value, parameters) == STATUS_OBJECT_TYPE_MISMATCH);
    parameters.dirtyStatus = STATUS_ACCESS_DENIED;
    Require(nv::iddcx::QueryDirtyRectsEnabled(&value, parameters) == STATUS_ACCESS_DENIED);
    std::puts("PASS: native-direct/dirty/realtime/MMCSS defaults, explicit overrides, and registry errors");
}
