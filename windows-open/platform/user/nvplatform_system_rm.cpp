#include "nvplatform_system_rm.h"
#include "nvplatform_common.h"

#include <windows.h>
#include <winternl.h>
#include <cstdio>

extern "C" NTSYSAPI NTSTATUS NTAPI RtlGetVersion(
    PRTL_OSVERSIONINFOW versionInformation);

namespace {

#pragma pack(push, 1)
struct RawSmbiosData
{
    BYTE used20CallingMethod;
    BYTE majorVersion;
    BYTE minorVersion;
    BYTE dmiRevision;
    DWORD length;
    BYTE tableData[1];
};
#pragma pack(pop)

struct CodeViewRsdsHeader
{
    DWORD signature;
    GUID identifier;
    DWORD age;
};

constexpr DWORD CodeViewRsdsSignature = 'SDSR';

NV_STATUS LastErrorToStatus(DWORD error)
{
    switch (error)
    {
        case ERROR_ENVVAR_NOT_FOUND:
        case ERROR_FILE_NOT_FOUND:
        case ERROR_NOT_FOUND:
            return NV_ERR_OBJECT_NOT_FOUND;
        case ERROR_INSUFFICIENT_BUFFER:
        case ERROR_MORE_DATA:
            return NV_ERR_BUFFER_TOO_SMALL;
        case ERROR_NOT_SUPPORTED:
        case ERROR_INVALID_FUNCTION:
            return NV_ERR_NOT_SUPPORTED;
        case ERROR_ACCESS_DENIED:
        case ERROR_PRIVILEGE_NOT_HELD:
            return NV_ERR_INSUFFICIENT_PERMISSIONS;
        default:
            return NV_ERR_OPERATING_SYSTEM;
    }
}

NV_STATUS CountSmbiosStructures(
    const BYTE* table,
    DWORD tableSize,
    NvU64* structureCount)
{
    DWORD offset = 0;
    NvU64 count = 0;
    while (offset < tableSize)
    {
        if (tableSize - offset < 4 || table[offset + 1] < 4 ||
            table[offset + 1] > tableSize - offset)
        {
            return NV_ERR_INVALID_DATA;
        }

        const BYTE type = table[offset];
        offset += table[offset + 1];
        while (offset + 1 < tableSize &&
               (table[offset] != 0 || table[offset + 1] != 0))
        {
            ++offset;
        }
        if (offset + 1 >= tableSize)
        {
            return NV_ERR_INVALID_DATA;
        }
        offset += 2;
        ++count;
        if (type == 127)
        {
            break;
        }
    }

    *structureCount = count;
    return NV_OK;
}

} // namespace

extern "C" NvBool NvPlatformRmIsNotebook(void)
{
    SYSTEM_POWER_STATUS status{};
    return GetSystemPowerStatus(&status) &&
        status.BatteryFlag != 255 && (status.BatteryFlag & 128) == 0
        ? NV_TRUE
        : NV_FALSE;
}

extern "C" NvBool NvPlatformRmIsDebuggerPresent(void)
{
    return IsDebuggerPresent() ? NV_TRUE : NV_FALSE;
}

extern "C" NvBool NvPlatformRmIsCurrentSubjectAdministrator(void)
{
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID administrators = nullptr;
    BOOL isMember = FALSE;
    if (!AllocateAndInitializeSid(
            &authority,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0,
            &administrators))
    {
        return NV_FALSE;
    }
    CheckTokenMembership(nullptr, administrators, &isMember);
    FreeSid(administrators);
    return isMember ? NV_TRUE : NV_FALSE;
}

extern "C" NV_STATUS NvPlatformRmGetFirmwareVariable(
    const char* name,
    const void* vendorGuid,
    NvU8* data,
    NvU32* dataLength)
{
    if (name == nullptr || vendorGuid == nullptr || dataLength == nullptr ||
        (data == nullptr && *dataLength != 0))
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    wchar_t wideName[256]{};
    if (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, name, -1,
            wideName, ARRAYSIZE(wideName)) == 0)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    const auto* guid = static_cast<const GUID*>(vendorGuid);
    wchar_t guidString[39]{};
    const int guidLength = _snwprintf_s(
        guidString,
        ARRAYSIZE(guidString),
        _TRUNCATE,
        L"{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
        guid->Data1,
        guid->Data2,
        guid->Data3,
        guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3],
        guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]);
    if (guidLength != 38)
    {
        return NV_ERR_INVALID_DATA;
    }

    SetLastError(ERROR_SUCCESS);
    const DWORD length = GetFirmwareEnvironmentVariableW(
        wideName,
        guidString,
        data,
        *dataLength);
    if (length == 0)
    {
        return LastErrorToStatus(GetLastError());
    }
    *dataLength = length;
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformRmGetAcpiTable(
    NvU32 tableSignature,
    void* table,
    NvU32 tableSize,
    NvU32* returnSize)
{
    if (returnSize == nullptr || (table == nullptr && tableSize != 0))
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    SetLastError(ERROR_SUCCESS);
    const UINT length = GetSystemFirmwareTable(
        'ACPI', tableSignature, table, tableSize);
    *returnSize = length;
    if (length == 0)
    {
        return LastErrorToStatus(GetLastError());
    }
    return length > tableSize ? NV_ERR_BUFFER_TOO_SMALL : NV_OK;
}

extern "C" NV_STATUS NvPlatformRmGetSmbiosTable(
    void** table,
    NvU64* tableSize,
    NvU64* structureCount,
    NvU32* version)
{
    if (table == nullptr || tableSize == nullptr || structureCount == nullptr ||
        version == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    *table = nullptr;
    *tableSize = 0;
    *structureCount = 0;
    *version = 0;

    const UINT required = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
    if (required < offsetof(RawSmbiosData, tableData))
    {
        return required == 0 ? LastErrorToStatus(GetLastError()) :
            NV_ERR_INVALID_DATA;
    }
    auto* raw = static_cast<RawSmbiosData*>(
        HeapAlloc(GetProcessHeap(), 0, required));
    if (raw == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }

    const UINT returned = GetSystemFirmwareTable('RSMB', 0, raw, required);
    if (returned != required ||
        raw->length > returned - offsetof(RawSmbiosData, tableData))
    {
        HeapFree(GetProcessHeap(), 0, raw);
        return returned > required ? NV_ERR_BUFFER_TOO_SMALL : NV_ERR_INVALID_DATA;
    }
    const NV_STATUS countStatus = CountSmbiosStructures(
        raw->tableData, raw->length, structureCount);
    if (countStatus != NV_OK)
    {
        HeapFree(GetProcessHeap(), 0, raw);
        return countStatus;
    }

    *table = raw->tableData;
    *tableSize = raw->length;
    *version = (static_cast<NvU32>(raw->majorVersion) << 8) | raw->minorVersion;
    return NV_OK;
}

extern "C" void NvPlatformRmPutSmbiosTable(void* table)
{
    if (table != nullptr)
    {
        auto* raw = reinterpret_cast<RawSmbiosData*>(
            static_cast<BYTE*>(table) - offsetof(RawSmbiosData, tableData));
        HeapFree(GetProcessHeap(), 0, raw);
    }
}

extern "C" NV_STATUS NvPlatformRmGetDriverImageInfo(
    void** imageBase,
    NvU8 debugIdentifier[16],
    NvU32* debugAge,
    NvU32* codeOffset)
{
    if (imageBase == nullptr || debugIdentifier == nullptr ||
        debugAge == nullptr || codeOffset == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&NvPlatformRmGetDriverImageInfo),
            &module))
    {
        return NV_ERR_OPERATING_SYSTEM;
    }

    auto* base = reinterpret_cast<BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0)
    {
        return NV_ERR_INVALID_DATA;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
    {
        return NV_ERR_INVALID_DATA;
    }

    *imageBase = base;
    *debugAge = 0;
    *codeOffset = 0;
    ZeroMemory(debugIdentifier, 16);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index)
    {
        if ((section[index].Characteristics & IMAGE_SCN_CNT_CODE) != 0)
        {
            *codeOffset = section[index].VirtualAddress;
            break;
        }
    }

    const IMAGE_DATA_DIRECTORY& entry =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (entry.VirtualAddress == 0 || entry.Size < sizeof(IMAGE_DEBUG_DIRECTORY))
    {
        return NV_OK;
    }
    const auto* directories = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(
        base + entry.VirtualAddress);
    const DWORD count = entry.Size / sizeof(*directories);
    for (DWORD index = 0; index < count; ++index)
    {
        if (directories[index].Type != IMAGE_DEBUG_TYPE_CODEVIEW ||
            directories[index].SizeOfData < sizeof(CodeViewRsdsHeader))
        {
            continue;
        }
        const auto* codeView = reinterpret_cast<const CodeViewRsdsHeader*>(
            base + directories[index].AddressOfRawData);
        if (codeView->signature == CodeViewRsdsSignature)
        {
            CopyMemory(debugIdentifier, &codeView->identifier, 16);
            *debugAge = codeView->age;
            break;
        }
    }
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformRmGetOsVersion(
    NvU32* majorVersion,
    NvU32* minorVersion,
    NvU32* buildNumber,
    NvU32* productType)
{
    if (majorVersion == nullptr || minorVersion == nullptr ||
        buildNumber == nullptr || productType == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    RTL_OSVERSIONINFOEXW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    const NTSTATUS status = RtlGetVersion(
        reinterpret_cast<PRTL_OSVERSIONINFOW>(&info));
    if (status < 0)
    {
        return NV_ERR_OPERATING_SYSTEM;
    }
    *majorVersion = info.dwMajorVersion;
    *minorVersion = info.dwMinorVersion;
    *buildNumber = info.dwBuildNumber;
    *productType = info.wProductType;
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformRmAddCrashLog(void*, NvU32)
{
    return NV_ERR_NOT_SUPPORTED;
}

extern "C" void NvPlatformRmDeleteCrashLog(void*)
{
}

extern "C" void NvPlatformRmLogErrorV(
    NvU32 xid,
    const char* format,
    va_list arguments)
{
    if (format == nullptr)
    {
        return;
    }
    char message[1024]{};
    const int prefixLength = std::snprintf(message, sizeof(message), "NVRM: Xid %u: ", xid);
    if (prefixLength < 0 || static_cast<size_t>(prefixLength) >= sizeof(message))
    {
        return;
    }
    std::vsnprintf(
        message + prefixLength,
        sizeof(message) - static_cast<size_t>(prefixLength),
        format,
        arguments);
    NvPlatformDebugPrintf(0, "%s", message);
}

extern "C" void NvPlatformRmCaptureAssertion(void)
{
    void* frames[16]{};
    const USHORT count = CaptureStackBackTrace(0, ARRAYSIZE(frames), frames, nullptr);
    NvPlatformDebugPrintf(0, "NVRM: assertion captured (%u frames)\n", count);
    for (USHORT index = 0; index < count; ++index)
    {
        NvPlatformDebugPrintf(0, "NVRM: assertion frame %u: %p\n", index, frames[index]);
    }
}

extern "C" __declspec(noreturn) void NvPlatformRmBugCheck(NvU32 bugCode)
{
    NvPlatformDebugPrintf(0, "NVRM: fail-fast bugCode=0x%08x\n", bugCode);
    NvPlatformRmCaptureAssertion();
    RaiseFailFastException(nullptr, nullptr, FAIL_FAST_GENERATE_EXCEPTION_ADDRESS);
    __assume(false);
}