#include "nvplatform_registry.h"
#include "nvplatform_registry_rm.h"

namespace {

HANDLE registryKey;
constexpr SIZE_T MaximumRmRegistryNameLength = 255;
constexpr ULONG RegistryQueryTag = 'gRvN';

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
    _In_ ACCESS_MASK desiredAccess,
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

    return ZwOpenKey(key, desiredAccess, &attributes);
}

NV_STATUS ConvertRegistryStatus(NTSTATUS status) noexcept
{
    if (NT_SUCCESS(status))
    {
        return NV_OK;
    }

    if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
        status == STATUS_OBJECT_PATH_NOT_FOUND)
    {
        return NV_ERR_OBJECT_NOT_FOUND;
    }

    if (status == STATUS_BUFFER_TOO_SMALL ||
        status == STATUS_BUFFER_OVERFLOW)
    {
        return NV_ERR_BUFFER_TOO_SMALL;
    }

    if (status == STATUS_INVALID_PARAMETER ||
        status == STATUS_OBJECT_TYPE_MISMATCH ||
        status == STATUS_NO_UNICODE_TRANSLATION)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    return NV_ERR_OPERATING_SYSTEM;
}

NTSTATUS QueryRmValue(
    _In_ const wchar_t* valueName,
    _Outptr_ KEY_VALUE_PARTIAL_INFORMATION** information) noexcept
{
    HANDLE key = nullptr;
    NTSTATUS status = OpenRelativeKey(L"Parameters", KEY_QUERY_VALUE, &key);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    UNICODE_STRING name{};
    RtlInitUnicodeString(&name, valueName);

    ULONG requiredLength = 0;
    status = ZwQueryValueKey(
        key,
        &name,
        KeyValuePartialInformation,
        nullptr,
        0,
        &requiredLength);
    if (status != STATUS_BUFFER_TOO_SMALL && status != STATUS_BUFFER_OVERFLOW)
    {
        ZwClose(key);
        return status;
    }

    auto* value = static_cast<KEY_VALUE_PARTIAL_INFORMATION*>(
        ExAllocatePool2(POOL_FLAG_PAGED, requiredLength, RegistryQueryTag));
    if (value == nullptr)
    {
        ZwClose(key);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = ZwQueryValueKey(
        key,
        &name,
        KeyValuePartialInformation,
        value,
        requiredLength,
        &requiredLength);
    ZwClose(key);

    if (!NT_SUCCESS(status))
    {
        ExFreePoolWithTag(value, RegistryQueryTag);
        return status;
    }

    *information = value;
    return STATUS_SUCCESS;
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

    return ZwOpenKey(
        &registryKey,
        KEY_QUERY_VALUE | KEY_SET_VALUE,
        &attributes);
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
    NTSTATUS status = OpenRelativeKey(relativeSubkey, KEY_QUERY_VALUE, &key);
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
    return ConvertRegistryStatus(status);
}

extern "C" NV_STATUS NvPlatformWriteRmRegistryDword(
    const char* valueName,
    NvU32 value)
{
    PAGED_CODE();

    wchar_t wideValueName[MaximumRmRegistryNameLength + 1]{};
    NTSTATUS status = ConvertAsciiName(
        valueName,
        wideValueName,
        RTL_NUMBER_OF(wideValueName));
    if (!NT_SUCCESS(status))
    {
        return ConvertRegistryStatus(status);
    }

    HANDLE key = nullptr;
    status = OpenRelativeKey(L"Parameters", KEY_SET_VALUE, &key);
    if (!NT_SUCCESS(status))
    {
        return ConvertRegistryStatus(status);
    }

    UNICODE_STRING name{};
    RtlInitUnicodeString(&name, wideValueName);
    status = ZwSetValueKey(key, &name, 0, REG_DWORD, &value, sizeof(value));
    ZwClose(key);
    return ConvertRegistryStatus(status);
}

extern "C" NV_STATUS NvPlatformReadRmRegistryBinary(
    const char* valueName,
    NvU8* data,
    NvU32* dataLength)
{
    PAGED_CODE();

    if (data == nullptr || dataLength == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    wchar_t wideValueName[MaximumRmRegistryNameLength + 1]{};
    NTSTATUS status = ConvertAsciiName(
        valueName,
        wideValueName,
        RTL_NUMBER_OF(wideValueName));
    if (!NT_SUCCESS(status))
    {
        return ConvertRegistryStatus(status);
    }

    KEY_VALUE_PARTIAL_INFORMATION* information = nullptr;
    status = QueryRmValue(wideValueName, &information);
    if (!NT_SUCCESS(status))
    {
        return ConvertRegistryStatus(status);
    }

    NV_STATUS nvStatus = NV_OK;
    if (information->Type != REG_BINARY)
    {
        nvStatus = NV_ERR_INVALID_ARGUMENT;
    }
    else if (*dataLength < information->DataLength)
    {
        *dataLength = information->DataLength;
        nvStatus = NV_ERR_BUFFER_TOO_SMALL;
    }
    else
    {
        RtlCopyMemory(data, information->Data, information->DataLength);
        *dataLength = information->DataLength;
    }

    ExFreePoolWithTag(information, RegistryQueryTag);
    return nvStatus;
}

extern "C" NV_STATUS NvPlatformReadRmRegistryString(
    const char* valueName,
    NvU8* data,
    NvU32* dataLength)
{
    PAGED_CODE();

    if (data == nullptr || dataLength == nullptr || *dataLength == 0)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    wchar_t wideValueName[MaximumRmRegistryNameLength + 1]{};
    NTSTATUS status = ConvertAsciiName(
        valueName,
        wideValueName,
        RTL_NUMBER_OF(wideValueName));
    if (!NT_SUCCESS(status))
    {
        return ConvertRegistryStatus(status);
    }

    KEY_VALUE_PARTIAL_INFORMATION* information = nullptr;
    status = QueryRmValue(wideValueName, &information);
    if (!NT_SUCCESS(status))
    {
        return ConvertRegistryStatus(status);
    }

    NV_STATUS nvStatus = NV_OK;
    if (information->Type != REG_SZ ||
        information->DataLength < sizeof(wchar_t) ||
        (information->DataLength % sizeof(wchar_t)) != 0)
    {
        nvStatus = NV_ERR_INVALID_ARGUMENT;
    }
    else
    {
        if (information->DataLength > MAXUSHORT)
        {
            ExFreePoolWithTag(information, RegistryQueryTag);
            return NV_ERR_INVALID_LIMIT;
        }

        UNICODE_STRING unicode{};
        unicode.Buffer = reinterpret_cast<wchar_t*>(information->Data);
        unicode.Length = static_cast<USHORT>(information->DataLength - sizeof(wchar_t));
        unicode.MaximumLength = static_cast<USHORT>(information->DataLength);

        const ULONG requiredLength = RtlUnicodeStringToAnsiSize(&unicode);
        if (requiredLength > *dataLength || requiredLength > MAXUSHORT)
        {
            *dataLength = requiredLength;
            nvStatus = NV_ERR_BUFFER_TOO_SMALL;
        }
        else
        {
            ANSI_STRING ansi{};
            ansi.Buffer = reinterpret_cast<char*>(data);
            ansi.Length = 0;
            ansi.MaximumLength = static_cast<USHORT>(*dataLength);
            status = RtlUnicodeStringToAnsiString(&ansi, &unicode, FALSE);
            if (NT_SUCCESS(status))
            {
                *dataLength = ansi.Length + 1;
            }
            else
            {
                nvStatus = ConvertRegistryStatus(status);
            }
        }
    }

    ExFreePoolWithTag(information, RegistryQueryTag);
    return nvStatus;
}

extern "C" NV_STATUS NvPlatformEnumerateRmRegistry(
    NV_PLATFORM_RM_REGISTRY_CALLBACK callback,
    void* context)
{
    PAGED_CODE();

    if (callback == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    HANDLE key = nullptr;
    NTSTATUS status = OpenRelativeKey(L"Parameters", KEY_QUERY_VALUE, &key);
    if (!NT_SUCCESS(status))
    {
        return ConvertRegistryStatus(status);
    }

    NV_STATUS nvStatus = NV_OK;
    for (ULONG index = 0;; ++index)
    {
        ULONG requiredLength = 0;
        status = ZwEnumerateValueKey(
            key,
            index,
            KeyValueFullInformation,
            nullptr,
            0,
            &requiredLength);
        if (status == STATUS_NO_MORE_ENTRIES)
        {
            break;
        }
        if (status != STATUS_BUFFER_TOO_SMALL && status != STATUS_BUFFER_OVERFLOW)
        {
            nvStatus = ConvertRegistryStatus(status);
            break;
        }

        auto* information = static_cast<KEY_VALUE_FULL_INFORMATION*>(
            ExAllocatePool2(POOL_FLAG_PAGED, requiredLength, RegistryQueryTag));
        if (information == nullptr)
        {
            nvStatus = NV_ERR_NO_MEMORY;
            break;
        }

        status = ZwEnumerateValueKey(
            key,
            index,
            KeyValueFullInformation,
            information,
            requiredLength,
            &requiredLength);
        if (!NT_SUCCESS(status))
        {
            ExFreePoolWithTag(information, RegistryQueryTag);
            nvStatus = ConvertRegistryStatus(status);
            break;
        }

        if (information->NameLength > MAXUSHORT)
        {
            ExFreePoolWithTag(information, RegistryQueryTag);
            nvStatus = NV_ERR_INVALID_LIMIT;
            break;
        }

        UNICODE_STRING unicodeName{};
        unicodeName.Buffer = information->Name;
        unicodeName.Length = static_cast<USHORT>(information->NameLength);
        unicodeName.MaximumLength = unicodeName.Length;

        char valueName[MaximumRmRegistryNameLength + 1]{};
        ANSI_STRING ansiName{};
        ansiName.Buffer = valueName;
        ansiName.MaximumLength = sizeof(valueName);
        status = RtlUnicodeStringToAnsiString(&ansiName, &unicodeName, FALSE);
        if (!NT_SUCCESS(status))
        {
            ExFreePoolWithTag(information, RegistryQueryTag);
            nvStatus = ConvertRegistryStatus(status);
            break;
        }
        valueName[ansiName.Length] = '\0';

        const NvU8* valueData = reinterpret_cast<const NvU8*>(information) +
            information->DataOffset;
        NvU32 valueLength = information->DataLength;
        NV_PLATFORM_RM_REGISTRY_TYPE valueType{};
        NvU8* convertedString = nullptr;

        if (information->Type == REG_DWORD && valueLength == sizeof(NvU32))
        {
            valueType = NV_PLATFORM_RM_REGISTRY_DWORD;
        }
        else if (information->Type == REG_BINARY)
        {
            valueType = NV_PLATFORM_RM_REGISTRY_BINARY;
        }
        else if (information->Type == REG_SZ &&
                 valueLength >= sizeof(wchar_t) &&
                 (valueLength % sizeof(wchar_t)) == 0)
        {
            if (valueLength > MAXUSHORT)
            {
                ExFreePoolWithTag(information, RegistryQueryTag);
                nvStatus = NV_ERR_INVALID_LIMIT;
                break;
            }

            UNICODE_STRING unicodeValue{};
            unicodeValue.Buffer = reinterpret_cast<wchar_t*>(
                const_cast<NvU8*>(valueData));
            unicodeValue.Length = static_cast<USHORT>(valueLength - sizeof(wchar_t));
            unicodeValue.MaximumLength = static_cast<USHORT>(valueLength);

            const ULONG convertedLength = RtlUnicodeStringToAnsiSize(&unicodeValue);
            if (convertedLength > MAXUSHORT)
            {
                ExFreePoolWithTag(information, RegistryQueryTag);
                nvStatus = NV_ERR_INVALID_LIMIT;
                break;
            }
            convertedString = static_cast<NvU8*>(
                ExAllocatePool2(POOL_FLAG_PAGED, convertedLength, RegistryQueryTag));
            if (convertedString == nullptr)
            {
                ExFreePoolWithTag(information, RegistryQueryTag);
                nvStatus = NV_ERR_NO_MEMORY;
                break;
            }

            ANSI_STRING ansiValue{};
            ansiValue.Buffer = reinterpret_cast<char*>(convertedString);
            ansiValue.MaximumLength = static_cast<USHORT>(convertedLength);
            status = RtlUnicodeStringToAnsiString(&ansiValue, &unicodeValue, FALSE);
            if (!NT_SUCCESS(status))
            {
                ExFreePoolWithTag(convertedString, RegistryQueryTag);
                ExFreePoolWithTag(information, RegistryQueryTag);
                nvStatus = ConvertRegistryStatus(status);
                break;
            }

            valueType = NV_PLATFORM_RM_REGISTRY_STRING;
            valueData = convertedString;
            valueLength = ansiValue.Length + 1;
        }

        if (valueType != 0)
        {
            nvStatus = callback(
                context,
                valueName,
                valueType,
                valueData,
                valueLength);
        }

        if (convertedString != nullptr)
        {
            ExFreePoolWithTag(convertedString, RegistryQueryTag);
        }
        ExFreePoolWithTag(information, RegistryQueryTag);

        if (nvStatus != NV_OK)
        {
            break;
        }
    }

    ZwClose(key);
    return nvStatus;
}