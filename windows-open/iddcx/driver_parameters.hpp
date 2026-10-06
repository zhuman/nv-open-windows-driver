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
    return status == STATUS_OBJECT_NAME_NOT_FOUND ||
        status == STATUS_OBJECT_PATH_NOT_FOUND;
}

} // namespace nv::iddcx
