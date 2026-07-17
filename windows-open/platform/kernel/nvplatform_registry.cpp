#include "nvplatform_registry.h"
#include "nvplatform_registry_rm.h"

namespace {

HANDLE registryKey;
constexpr SIZE_T MaximumRmRegistryNameLength = 255;

NTSTATUS ConvertAsciiName(
    _In_ const char* source,
    _Out_writes_(destinationCount) wchar_t* destination,
    _In_ SIZE_T destinationCount) noexcept
{
    if (source == nullptr || destination == nullptr || destinationCount == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    SIZE_T index = 0;
    while (source[index] != '\0')
    {
        const UCHAR character = static_cast<UCHAR>(source[index]);
        if (character > 0x7f)
        {
            return STATUS_NO_UNICODE_TRANSLATION;
        }

        if (index + 1 >= destinationCount)
        {
            return STATUS_NAME_TOO_LONG;
        }

        destination[index] = static_cast<wchar_t>(character);
        ++index;
    }

    if (index == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    destination[index] = L'\0';
    return STATUS_SUCCESS;
}

NTSTATUS OpenRelativeKey(
    _In_ const wchar_t* relativeSubkey,
    _Out_ HANDLE* key) noexcept
{
    if (relativeSubkey == nullptr || relativeSubkey[0] == L'\0')
    {
        *key = registryKey;
        return STATUS_SUCCESS;
    }

    UNICODE_STRING subkeyName{};
    RtlInitUnicodeString(&subkeyName, relativeSubkey);

    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(
        &attributes,
        &subkeyName,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        registryKey,
        nullptr);

    return ZwOpenKey(key, KEY_QUERY_VALUE, &attributes);
}

} // namespace

extern "C" NTSTATUS NvPlatformRegistryInitialize(
    _In_ const UNICODE_STRING* registryPath) noexcept
{
    PAGED_CODE();

    if (registryPath == nullptr || registryPath->Buffer == nullptr ||
        registryPath->Length == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (registryKey != nullptr)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(
        &attributes,
        const_cast<UNICODE_STRING*>(registryPath),
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        nullptr,
        nullptr);

    return ZwOpenKey(&registryKey, KEY_QUERY_VALUE, &attributes);
}

extern "C" void NvPlatformRegistryShutdown(void) noexcept
{
    PAGED_CODE();

    if (registryKey != nullptr)
    {
        ZwClose(registryKey);
        registryKey = nullptr;
    }
}

extern "C" NTSTATUS NvPlatformReadRegistryDword(
    _In_opt_ const wchar_t* relativeSubkey,
    _In_ const wchar_t* valueName,
    _Out_ ULONG* value) noexcept
{
    PAGED_CODE();

    if (registryKey == nullptr)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (valueName == nullptr || valueName[0] == L'\0' || value == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }

    HANDLE key = nullptr;
    NTSTATUS status = OpenRelativeKey(relativeSubkey, &key);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    UNICODE_STRING name{};
    RtlInitUnicodeString(&name, valueName);

    alignas(KEY_VALUE_PARTIAL_INFORMATION)
        UCHAR buffer[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(ULONG)]{};
    auto* information = reinterpret_cast<KEY_VALUE_PARTIAL_INFORMATION*>(buffer);
    ULONG resultLength = 0;
    status = ZwQueryValueKey(
        key,
        &name,
        KeyValuePartialInformation,
        information,
        sizeof(buffer),
        &resultLength);

    if (key != registryKey)
    {
        ZwClose(key);
    }

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    if (information->Type != REG_DWORD || information->DataLength != sizeof(ULONG))
    {
        return STATUS_OBJECT_TYPE_MISMATCH;
    }

    RtlCopyMemory(value, information->Data, sizeof(*value));
    return STATUS_SUCCESS;
}

extern "C" NV_STATUS NvPlatformReadRmRegistryDword(
    const char* valueName,
    NvU32* value)
{
    PAGED_CODE();

    wchar_t wideValueName[MaximumRmRegistryNameLength + 1]{};
    NTSTATUS status = ConvertAsciiName(
        valueName,
        wideValueName,
        RTL_NUMBER_OF(wideValueName));
    if (!NT_SUCCESS(status))
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    status = NvPlatformReadRegistryDword(L"Parameters", wideValueName, value);
    if (NT_SUCCESS(status))
    {
        return NV_OK;
    }

    if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
        status == STATUS_OBJECT_PATH_NOT_FOUND)
    {
        return NV_ERR_OBJECT_NOT_FOUND;
    }

    if (status == STATUS_INVALID_PARAMETER ||
        status == STATUS_OBJECT_TYPE_MISMATCH)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    return NV_ERR_OPERATING_SYSTEM;
}