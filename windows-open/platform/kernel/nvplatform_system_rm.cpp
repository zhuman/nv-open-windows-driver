#include "nvplatform_system.h"
#include "nvplatform_system_rm.h"
#include "nvplatform_pci.h"

extern "C" {
#include <ntpoapi.h>
}

namespace {

constexpr ULONG SystemWorkPoolTag = 'wRvN';
constexpr LONG SystemWorkStateNeverInitialized = 0;
constexpr LONG SystemWorkStateActive = 1;
constexpr LONG SystemWorkStateStopped = 2;

struct SystemWorkItem
{
    PIO_WORKITEM ioWorkItem;
    NvPlatformRmSystemWorkFunction* function;
    void* context;
};

EX_RUNDOWN_REF systemWorkRundown;
volatile LONG systemWorkState = SystemWorkStateNeverInitialized;
PDRIVER_OBJECT systemWorkDriverObject;
KSEMAPHORE rmSema;

void SystemWorkRoutine(
    void* ioObject,
    void* parameter,
    PIO_WORKITEM ioWorkItem)
{
    UNREFERENCED_PARAMETER(ioObject);

    auto* work = static_cast<SystemWorkItem*>(parameter);
    work->function(work->context);
    IoUninitializeWorkItem(ioWorkItem);
    ExFreePoolWithTag(ioWorkItem, SystemWorkPoolTag);
    ExFreePoolWithTag(work, SystemWorkPoolTag);
    ExReleaseRundownProtection(&systemWorkRundown);
}

} // namespace

NTSTATUS NvPlatformSystemInitialize(_In_ PDRIVER_OBJECT driverObject) noexcept
{
    if (driverObject == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(&systemWorkDriverObject),
        driverObject,
        nullptr) != nullptr)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    const NTSTATUS pciStatus = NvPlatformPciInitialize();
    if (!NT_SUCCESS(pciStatus))
    {
        InterlockedExchangePointer(
            reinterpret_cast<void* volatile*>(&systemWorkDriverObject),
            nullptr);
        return pciStatus;
    }

    KeInitializeSemaphore(&rmSema, 1, 1);
    return STATUS_SUCCESS;
}

void NvPlatformSystemShutdown(void) noexcept
{
    NvPlatformRmDrainSystemWork();
    NvPlatformPciShutdown();
    InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(&systemWorkDriverObject),
        nullptr);
}

extern "C" NvBool NvPlatformRmIsNotebook(void)
{
    SYSTEM_POWER_CAPABILITIES capabilities{};
    const NTSTATUS status = ZwPowerInformation(
        SystemPowerCapabilities,
        nullptr,
        0,
        &capabilities,
        sizeof(capabilities));

    return NT_SUCCESS(status) && capabilities.SystemBatteriesPresent
        ? NV_TRUE
        : NV_FALSE;
}

extern "C" NV_STATUS NvPlatformRmSystemWorkInitialize(void)
{
    if (systemWorkDriverObject == nullptr)
    {
        return NV_ERR_INVALID_STATE;
    }

    const LONG previousState = InterlockedCompareExchange(
        &systemWorkState,
        SystemWorkStateActive,
        SystemWorkStateNeverInitialized);
    if (previousState == SystemWorkStateNeverInitialized)
    {
        ExInitializeRundownProtection(&systemWorkRundown);
        return NV_OK;
    }

    if (previousState == SystemWorkStateStopped &&
        InterlockedCompareExchange(
            &systemWorkState,
            SystemWorkStateActive,
            SystemWorkStateStopped) == SystemWorkStateStopped)
    {
        ExReInitializeRundownProtection(&systemWorkRundown);
        return NV_OK;
    }

    return previousState == SystemWorkStateActive
        ? NV_OK
        : NV_ERR_INVALID_STATE;
}

extern "C" NV_STATUS NvPlatformRmSemaAcquire(void)
{
    if (systemWorkDriverObject == nullptr || KeGetCurrentIrql() > APC_LEVEL)
    {
        return NV_ERR_INVALID_STATE;
    }

    return KeWaitForSingleObject(
        &rmSema,
        Executive,
        KernelMode,
        FALSE,
        nullptr) == STATUS_SUCCESS
        ? NV_OK
        : NV_ERR_INVALID_STATE;
}

extern "C" NV_STATUS NvPlatformRmSemaTryAcquire(void)
{
    LARGE_INTEGER timeout{};

    if (systemWorkDriverObject == nullptr)
    {
        return NV_ERR_INVALID_STATE;
    }

    return KeWaitForSingleObject(
        &rmSema,
        Executive,
        KernelMode,
        FALSE,
        &timeout) == STATUS_SUCCESS
        ? NV_OK
        : NV_ERR_STATE_IN_USE;
}

extern "C" void NvPlatformRmSemaRelease(void)
{
    KeReleaseSemaphore(&rmSema, IO_NO_INCREMENT, 1, FALSE);
}

extern "C" NV_STATUS NvPlatformRmQueueSystemWork(
    NvPlatformRmSystemWorkFunction* function,
    void* context)
{
    if (function == nullptr ||
        InterlockedCompareExchange(
            &systemWorkState,
            SystemWorkStateActive,
            SystemWorkStateActive) != SystemWorkStateActive ||
        !ExAcquireRundownProtection(&systemWorkRundown))
    {
        return NV_ERR_INVALID_STATE;
    }

    auto* work = static_cast<SystemWorkItem*>(ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(SystemWorkItem),
        SystemWorkPoolTag));
    if (work == nullptr)
    {
        ExReleaseRundownProtection(&systemWorkRundown);
        return NV_ERR_NO_MEMORY;
    }

    work->ioWorkItem = static_cast<PIO_WORKITEM>(ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        IoSizeofWorkItem(),
        SystemWorkPoolTag));
    if (work->ioWorkItem == nullptr)
    {
        ExFreePoolWithTag(work, SystemWorkPoolTag);
        ExReleaseRundownProtection(&systemWorkRundown);
        return NV_ERR_NO_MEMORY;
    }

    work->function = function;
    work->context = context;
    IoInitializeWorkItem(systemWorkDriverObject, work->ioWorkItem);
    IoQueueWorkItemEx(
        work->ioWorkItem,
        SystemWorkRoutine,
        DelayedWorkQueue,
        work);
    return NV_OK;
}

extern "C" void NvPlatformRmDrainSystemWork(void)
{
    if (InterlockedCompareExchange(
            &systemWorkState,
            SystemWorkStateStopped,
            SystemWorkStateActive) == SystemWorkStateActive)
    {
        ExWaitForRundownProtectionRelease(&systemWorkRundown);
    }
}