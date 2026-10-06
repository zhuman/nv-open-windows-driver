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
    NTSTATUS apiStatus = STATUS_OBJECT_NAME_NOT_FOUND;
    NTSTATUS modeStatus = STATUS_OBJECT_NAME_NOT_FOUND;
    NTSTATUS dirtyStatus = STATUS_OBJECT_NAME_NOT_FOUND;

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

}

int main()
{
    Parameters parameters;
    ExpectMode(parameters, 1);
    ExpectDirty(parameters, 1);
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
    std::puts("PASS: native-direct/dirty defaults, explicit API/mode overrides, and registry errors");
}
