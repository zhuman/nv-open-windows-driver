#include "nvplatform_pci.h"
#include "nvplatform_pci_rm.h"

namespace {

constexpr ULONG PciExpressConfigurationLength = 4096;
constexpr ULONG MaximumRegisteredPciFunctions = 32;

struct PciConfigRegistration
{
    NV_PLATFORM_PCI_LOCATION location;
    NV_PLATFORM_PCI_CONFIG_ACCESS access;
    bool registered;
};

EX_PUSH_LOCK registrationLock;
PciConfigRegistration registrations[MaximumRegisteredPciFunctions];
bool registryInitialized;

bool IsValidConfigurationRange(_In_ ULONG offset, _In_ ULONG length) noexcept
{
    return length != 0 &&
        length <= PciExpressConfigurationLength &&
        offset <= PciExpressConfigurationLength - length;
}

bool IsValidLocation(_In_ const NV_PLATFORM_PCI_LOCATION& location) noexcept
{
    return location.device < 32 && location.function < 8;
}

bool LocationsEqual(
    _In_ const NV_PLATFORM_PCI_LOCATION& left,
    _In_ const NV_PLATFORM_PCI_LOCATION& right) noexcept
{
    return left.domain == right.domain &&
        left.bus == right.bus &&
        left.device == right.device &&
        left.function == right.function;
}

PciConfigRegistration* RegistrationFromHandle(_In_ void* handle) noexcept
{
    const ULONG_PTR address = reinterpret_cast<ULONG_PTR>(handle);
    const ULONG_PTR first = reinterpret_cast<ULONG_PTR>(&registrations[0]);
    const ULONG_PTR end = reinterpret_cast<ULONG_PTR>(
        &registrations[MaximumRegisteredPciFunctions]);
    if (address < first || address >= end ||
        (address - first) % sizeof(PciConfigRegistration) != 0)
    {
        return nullptr;
    }

    return static_cast<PciConfigRegistration*>(handle);
}

} // namespace

extern "C" NTSTATUS NvPlatformPciInitialize(void) noexcept
{
    if (registryInitialized)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    RtlZeroMemory(registrations, sizeof(registrations));
    ExInitializePushLock(&registrationLock);
    registryInitialized = true;
    return STATUS_SUCCESS;
}

extern "C" void NvPlatformPciShutdown(void) noexcept
{
    if (!registryInitialized)
    {
        return;
    }

    ExAcquirePushLockExclusive(&registrationLock);
    RtlZeroMemory(registrations, sizeof(registrations));
    registryInitialized = false;
    ExReleasePushLockExclusive(&registrationLock);
}

extern "C" NTSTATUS NvPlatformRegisterPciConfig(
    _In_ const NV_PLATFORM_PCI_LOCATION* location,
    _In_ const NV_PLATFORM_PCI_CONFIG_ACCESS* access,
    _Outptr_ void** handle) noexcept
{
    if (location == nullptr || access == nullptr || handle == nullptr ||
        access->read == nullptr || !IsValidLocation(*location))
    {
        return STATUS_INVALID_PARAMETER;
    }

    *handle = nullptr;
    if (!registryInitialized)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
    ExAcquirePushLockExclusive(&registrationLock);
    PciConfigRegistration* available = nullptr;
    for (auto& registration : registrations)
    {
        if (registration.registered)
        {
            if (LocationsEqual(registration.location, *location))
            {
                status = STATUS_OBJECT_NAME_COLLISION;
                available = nullptr;
                break;
            }
        }
        else if (available == nullptr)
        {
            available = &registration;
        }
    }

    if (available != nullptr)
    {
        available->location = *location;
        available->access = *access;
        available->registered = true;
        *handle = available;
        status = STATUS_SUCCESS;
    }
    ExReleasePushLockExclusive(&registrationLock);
    return status;
}

extern "C" void NvPlatformUnregisterPciConfig(_In_opt_ void* handle) noexcept
{
    if (!registryInitialized)
    {
        return;
    }

    PciConfigRegistration* registration = RegistrationFromHandle(handle);
    if (registration == nullptr)
    {
        return;
    }

    ExAcquirePushLockExclusive(&registrationLock);
    RtlZeroMemory(registration, sizeof(*registration));
    ExReleasePushLockExclusive(&registrationLock);
}

extern "C" void* NvPlatformOpenPciConfig(
    _In_ const NV_PLATFORM_PCI_LOCATION* location) noexcept
{
    if (!registryInitialized || location == nullptr || !IsValidLocation(*location))
    {
        return nullptr;
    }

    PciConfigRegistration* result = nullptr;
    ExAcquirePushLockShared(&registrationLock);
    for (auto& registration : registrations)
    {
        if (registration.registered &&
            LocationsEqual(registration.location, *location))
        {
            result = &registration;
            break;
        }
    }
    ExReleasePushLockShared(&registrationLock);
    return result;
}

extern "C" NTSTATUS NvPlatformReadPciConfigHandle(
    _In_ void* handle,
    _In_ ULONG offset,
    _Out_writes_bytes_(length) void* buffer,
    _In_ ULONG length) noexcept
{
    PciConfigRegistration* registration = RegistrationFromHandle(handle);
    if (!registryInitialized || registration == nullptr)
    {
        return STATUS_INVALID_HANDLE;
    }

    ExAcquirePushLockShared(&registrationLock);
    const NTSTATUS status = registration->registered
        ? NvPlatformReadPciConfig(&registration->access, offset, buffer, length)
        : STATUS_INVALID_HANDLE;
    ExReleasePushLockShared(&registrationLock);
    return status;
}

extern "C" NTSTATUS NvPlatformWritePciConfigHandle(
    _In_ void* handle,
    _In_ ULONG offset,
    _In_reads_bytes_(length) const void* buffer,
    _In_ ULONG length) noexcept
{
    PciConfigRegistration* registration = RegistrationFromHandle(handle);
    if (!registryInitialized || registration == nullptr)
    {
        return STATUS_INVALID_HANDLE;
    }

    ExAcquirePushLockShared(&registrationLock);
    const NTSTATUS status = registration->registered
        ? NvPlatformWritePciConfig(&registration->access, offset, buffer, length)
        : STATUS_INVALID_HANDLE;
    ExReleasePushLockShared(&registrationLock);
    return status;
}

extern "C" NTSTATUS NvPlatformReadPciConfig(
    _In_ const NV_PLATFORM_PCI_CONFIG_ACCESS* access,
    _In_ ULONG offset,
    _Out_writes_bytes_(length) void* buffer,
    _In_ ULONG length) noexcept
{
    if (access == nullptr || buffer == nullptr ||
        !IsValidConfigurationRange(offset, length))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (access->read == nullptr)
    {
        return STATUS_NOT_SUPPORTED;
    }

    ULONG bytesRead = 0;
    const NTSTATUS status = access->read(
        access->context,
        buffer,
        offset,
        length,
        &bytesRead);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    return bytesRead == length
        ? STATUS_SUCCESS
        : STATUS_DEVICE_CONFIGURATION_ERROR;
}

extern "C" NTSTATUS NvPlatformWritePciConfig(
    _In_ const NV_PLATFORM_PCI_CONFIG_ACCESS* access,
    _In_ ULONG offset,
    _In_reads_bytes_(length) const void* buffer,
    _In_ ULONG length) noexcept
{
    if (access == nullptr || buffer == nullptr ||
        !IsValidConfigurationRange(offset, length))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (access->write == nullptr)
    {
        return STATUS_NOT_SUPPORTED;
    }

    ULONG bytesWritten = 0;
    const NTSTATUS status = access->write(
        access->context,
        buffer,
        offset,
        length,
        &bytesWritten);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    return bytesWritten == length
        ? STATUS_SUCCESS
        : STATUS_DEVICE_CONFIGURATION_ERROR;
}

extern "C" void* NvPlatformRmOpenPciConfig(
    NvU32 domain,
    NvU8 bus,
    NvU8 device,
    NvU8 function)
{
    const NV_PLATFORM_PCI_LOCATION location{
        domain,
        bus,
        device,
        function,
    };
    return NvPlatformOpenPciConfig(&location);
}

extern "C" NV_STATUS NvPlatformRmReadPciConfig(
    void* handle,
    NvU32 offset,
    void* buffer,
    NvU32 length)
{
    return NT_SUCCESS(NvPlatformReadPciConfigHandle(
        handle,
        offset,
        buffer,
        length))
        ? NV_OK
        : NV_ERR_GENERIC;
}

extern "C" NV_STATUS NvPlatformRmWritePciConfig(
    void* handle,
    NvU32 offset,
    const void* buffer,
    NvU32 length)
{
    return NT_SUCCESS(NvPlatformWritePciConfigHandle(
        handle,
        offset,
        buffer,
        length))
        ? NV_OK
        : NV_ERR_GENERIC;
}