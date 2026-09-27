#include "nvplatform_pci.h"
#include "nvplatform_pci_rm.h"

namespace {

constexpr ULONG PciExpressConfigurationLength = 4096;
constexpr ULONG MaximumRegisteredPciFunctions = 32;
constexpr NTSTATUS StatusSuccess = 0;
constexpr NTSTATUS StatusInvalidHandle = static_cast<NTSTATUS>(0xC0000008L);
constexpr NTSTATUS StatusInvalidParameter = static_cast<NTSTATUS>(0xC000000DL);
constexpr NTSTATUS StatusObjectNameCollision = static_cast<NTSTATUS>(0xC0000035L);
constexpr NTSTATUS StatusInsufficientResources = static_cast<NTSTATUS>(0xC000009AL);
constexpr NTSTATUS StatusDeviceConfigurationError = static_cast<NTSTATUS>(0xC0000182L);

struct PciConfigRegistration
{
    NV_PLATFORM_PCI_LOCATION location;
    NV_PLATFORM_PCI_CONFIG_ACCESS access;
    bool registered;
};

SRWLOCK registrationLock = SRWLOCK_INIT;
PciConfigRegistration registrations[MaximumRegisteredPciFunctions];
LONG initializationCount;

bool IsValidConfigurationRange(ULONG offset, ULONG length) noexcept
{
    return length != 0 && length <= PciExpressConfigurationLength &&
        offset <= PciExpressConfigurationLength - length;
}

bool IsValidLocation(const NV_PLATFORM_PCI_LOCATION& location) noexcept
{
    return location.device < 32 && location.function < 8;
}

bool LocationsEqual(
    const NV_PLATFORM_PCI_LOCATION& left,
    const NV_PLATFORM_PCI_LOCATION& right) noexcept
{
    return left.domain == right.domain && left.bus == right.bus &&
        left.device == right.device && left.function == right.function;
}

PciConfigRegistration* RegistrationFromHandle(void* handle) noexcept
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
    InterlockedIncrement(&initializationCount);
    return StatusSuccess;
}

extern "C" void NvPlatformPciShutdown(void) noexcept
{
    if (InterlockedDecrement(&initializationCount) != 0)
    {
        return;
    }

    AcquireSRWLockExclusive(&registrationLock);
    ZeroMemory(registrations, sizeof(registrations));
    ReleaseSRWLockExclusive(&registrationLock);
}

extern "C" NTSTATUS NvPlatformRegisterPciConfig(
    const NV_PLATFORM_PCI_LOCATION* location,
    const NV_PLATFORM_PCI_CONFIG_ACCESS* access,
    void** handle) noexcept
{
    if (location == nullptr || access == nullptr || handle == nullptr ||
        access->read == nullptr || !IsValidLocation(*location) ||
        initializationCount <= 0)
    {
        return StatusInvalidParameter;
    }

    *handle = nullptr;
    NTSTATUS status = StatusInsufficientResources;
    AcquireSRWLockExclusive(&registrationLock);
    PciConfigRegistration* available = nullptr;
    for (auto& registration : registrations)
    {
        if (registration.registered && LocationsEqual(registration.location, *location))
        {
            status = StatusObjectNameCollision;
            available = nullptr;
            break;
        }
        if (!registration.registered && available == nullptr)
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
        status = StatusSuccess;
    }
    ReleaseSRWLockExclusive(&registrationLock);
    return status;
}

extern "C" void NvPlatformUnregisterPciConfig(void* handle) noexcept
{
    PciConfigRegistration* registration = RegistrationFromHandle(handle);
    if (registration == nullptr)
    {
        return;
    }

    AcquireSRWLockExclusive(&registrationLock);
    ZeroMemory(registration, sizeof(*registration));
    ReleaseSRWLockExclusive(&registrationLock);
}

extern "C" void* NvPlatformOpenPciConfig(
    const NV_PLATFORM_PCI_LOCATION* location) noexcept
{
    if (location == nullptr || !IsValidLocation(*location) || initializationCount <= 0)
    {
        return nullptr;
    }

    PciConfigRegistration* result = nullptr;
    AcquireSRWLockShared(&registrationLock);
    for (auto& registration : registrations)
    {
        if (registration.registered && LocationsEqual(registration.location, *location))
        {
            result = &registration;
            break;
        }
    }
    ReleaseSRWLockShared(&registrationLock);
    return result;
}

extern "C" NTSTATUS NvPlatformReadPciConfig(
    const NV_PLATFORM_PCI_CONFIG_ACCESS* access,
    ULONG offset,
    void* buffer,
    ULONG length) noexcept
{
    if (access == nullptr || buffer == nullptr ||
        !IsValidConfigurationRange(offset, length) || access->read == nullptr)
    {
        return StatusInvalidParameter;
    }

    ULONG bytesRead = 0;
    const NTSTATUS status = access->read(
        access->context, buffer, offset, length, &bytesRead);
    return status >= 0 && bytesRead == length ?
        StatusSuccess : StatusDeviceConfigurationError;
}

extern "C" NTSTATUS NvPlatformWritePciConfig(
    const NV_PLATFORM_PCI_CONFIG_ACCESS* access,
    ULONG offset,
    const void* buffer,
    ULONG length) noexcept
{
    if (access == nullptr || buffer == nullptr ||
        !IsValidConfigurationRange(offset, length) || access->write == nullptr)
    {
        return StatusInvalidParameter;
    }

    ULONG bytesWritten = 0;
    const NTSTATUS status = access->write(
        access->context, buffer, offset, length, &bytesWritten);
    return status >= 0 && bytesWritten == length ?
        StatusSuccess : StatusDeviceConfigurationError;
}

extern "C" NTSTATUS NvPlatformReadPciConfigHandle(
    void* handle,
    ULONG offset,
    void* buffer,
    ULONG length) noexcept
{
    PciConfigRegistration* registration = RegistrationFromHandle(handle);
    if (registration == nullptr)
    {
        return StatusInvalidHandle;
    }

    AcquireSRWLockShared(&registrationLock);
    const NTSTATUS status = registration->registered ?
        NvPlatformReadPciConfig(&registration->access, offset, buffer, length) :
        StatusInvalidHandle;
    ReleaseSRWLockShared(&registrationLock);
    return status;
}

extern "C" NTSTATUS NvPlatformWritePciConfigHandle(
    void* handle,
    ULONG offset,
    const void* buffer,
    ULONG length) noexcept
{
    PciConfigRegistration* registration = RegistrationFromHandle(handle);
    if (registration == nullptr)
    {
        return StatusInvalidHandle;
    }

    AcquireSRWLockShared(&registrationLock);
    const NTSTATUS status = registration->registered ?
        NvPlatformWritePciConfig(&registration->access, offset, buffer, length) :
        StatusInvalidHandle;
    ReleaseSRWLockShared(&registrationLock);
    return status;
}

extern "C" void* NvPlatformRmOpenPciConfig(
    NvU32 domain,
    NvU8 bus,
    NvU8 device,
    NvU8 function)
{
    const NV_PLATFORM_PCI_LOCATION location{domain, bus, device, function};
    return NvPlatformOpenPciConfig(&location);
}

extern "C" NV_STATUS NvPlatformRmReadPciConfig(
    void* handle,
    NvU32 offset,
    void* buffer,
    NvU32 length)
{
    return NvPlatformReadPciConfigHandle(handle, offset, buffer, length) >= 0 ?
        NV_OK : NV_ERR_GENERIC;
}

extern "C" NV_STATUS NvPlatformRmWritePciConfig(
    void* handle,
    NvU32 offset,
    const void* buffer,
    NvU32 length)
{
    return NvPlatformWritePciConfigHandle(handle, offset, buffer, length) >= 0 ?
        NV_OK : NV_ERR_GENERIC;
}