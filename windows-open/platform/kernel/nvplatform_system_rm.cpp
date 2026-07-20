extern "C" {
#include <ntifs.h>
#include <ntpoapi.h>
#include <aux_klib.h>
}

#include "nvplatform_system.h"
#include "nvplatform_system_rm.h"
#include "nvplatform_mmio.h"
#include "nvplatform_pci.h"

namespace {

constexpr ULONG SystemWorkPoolTag = 'wRvN';
constexpr LONG SystemWorkStateNeverInitialized = 0;
constexpr LONG SystemWorkStateActive = 1;
constexpr LONG SystemWorkStateStopped = 2;
constexpr ULONG RmBugCheckCode = 0xE2;
constexpr ULONG_PTR RmBugCheckSignature = 'MRVN';
constexpr ULONG MaximumAssertionFrames = 16;
constexpr USHORT MaximumFirmwareVariableNameLength = 255;

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

    const NTSTATUS mmioStatus = NvPlatformMmioInitialize();
    if (!NT_SUCCESS(mmioStatus))
    {
        InterlockedExchangePointer(
            reinterpret_cast<void* volatile*>(&systemWorkDriverObject),
            nullptr);
        return mmioStatus;
    }

    const NTSTATUS pciStatus = NvPlatformPciInitialize();
    if (!NT_SUCCESS(pciStatus))
    {
        NvPlatformMmioShutdown();
        InterlockedExchangePointer(
            reinterpret_cast<void* volatile*>(&systemWorkDriverObject),
            nullptr);
        return pciStatus;
    }

    const NTSTATUS auxKlibStatus = AuxKlibInitialize();
    if (!NT_SUCCESS(auxKlibStatus))
    {
        NvPlatformPciShutdown();
        NvPlatformMmioShutdown();
        InterlockedExchangePointer(
            reinterpret_cast<void* volatile*>(&systemWorkDriverObject),
            nullptr);
        return auxKlibStatus;
    }

    KeInitializeSemaphore(&rmSema, 1, 1);
    return STATUS_SUCCESS;
}

void NvPlatformSystemShutdown(void) noexcept
{
    NvPlatformRmDrainSystemWork();
    NvPlatformPciShutdown();
    NvPlatformMmioShutdown();
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

extern "C" NvBool NvPlatformRmIsDebuggerPresent(void)
{
    return KD_DEBUGGER_ENABLED && !KD_DEBUGGER_NOT_PRESENT
        ? NV_TRUE
        : NV_FALSE;
}

extern "C" NV_STATUS NvPlatformRmGetFirmwareVariable(
    const char* name,
    const void* vendorGuid,
    NvU8* data,
    NvU32* dataLength)
{
    if (name == nullptr || vendorGuid == nullptr || dataLength == nullptr ||
        KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    ANSI_STRING ansiName{};
    RtlInitAnsiString(&ansiName, name);
    if (ansiName.Length > MaximumFirmwareVariableNameLength)
    {
        return NV_ERR_INVALID_LIMIT;
    }

    wchar_t wideName[MaximumFirmwareVariableNameLength + 1]{};
    UNICODE_STRING unicodeName{};
    unicodeName.Buffer = wideName;
    unicodeName.MaximumLength = sizeof(wideName);
    NTSTATUS status = RtlAnsiStringToUnicodeString(
        &unicodeName,
        &ansiName,
        FALSE);
    if (!NT_SUCCESS(status))
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    ULONG length = *dataLength;
    status = ExGetFirmwareEnvironmentVariable(
        &unicodeName,
        const_cast<LPGUID>(static_cast<const GUID*>(vendorGuid)),
        data,
        &length,
        nullptr);
    *dataLength = length;
    if (NT_SUCCESS(status))
    {
        return NV_OK;
    }
    if (status == STATUS_VARIABLE_NOT_FOUND)
    {
        return NV_ERR_OBJECT_NOT_FOUND;
    }
    if (status == STATUS_BUFFER_TOO_SMALL || status == STATUS_BUFFER_OVERFLOW)
    {
        return NV_ERR_BUFFER_TOO_SMALL;
    }
    if (status == STATUS_NOT_IMPLEMENTED || status == STATUS_NOT_SUPPORTED)
    {
        return NV_ERR_NOT_SUPPORTED;
    }
    return NV_ERR_OPERATING_SYSTEM;
}

extern "C" NV_STATUS NvPlatformRmGetAcpiTable(
    NvU32 tableSignature,
    void* table,
    NvU32 tableSize,
    NvU32* returnSize)
{
    if (returnSize == nullptr || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    ULONG length = 0;
    const NTSTATUS status = AuxKlibGetSystemFirmwareTable(
        'ACPI',
        tableSignature,
        table,
        tableSize,
        &length);
    *returnSize = length;
    if (NT_SUCCESS(status))
    {
        return NV_OK;
    }
    if (status == STATUS_BUFFER_TOO_SMALL || status == STATUS_BUFFER_OVERFLOW)
    {
        return NV_ERR_BUFFER_TOO_SMALL;
    }
    if (status == STATUS_NOT_FOUND || status == STATUS_OBJECT_NAME_NOT_FOUND)
    {
        return NV_ERR_OBJECT_NOT_FOUND;
    }
    return NV_ERR_OPERATING_SYSTEM;
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

extern "C" NvBool NvPlatformRmIsCurrentSubjectAdministrator(void)
{
    if (KeGetCurrentIrql() > APC_LEVEL)
    {
        return NV_FALSE;
    }

    SECURITY_SUBJECT_CONTEXT subjectContext{};
    SeCaptureSubjectContext(&subjectContext);
    SeLockSubjectContext(&subjectContext);
    const BOOLEAN isAdministrator = SeTokenIsAdmin(
        SeQuerySubjectContextToken(&subjectContext));
    SeUnlockSubjectContext(&subjectContext);
    SeReleaseSubjectContext(&subjectContext);
    return isAdministrator ? NV_TRUE : NV_FALSE;
}

extern "C" void NvPlatformRmCaptureAssertion(void)
{
    void* frames[MaximumAssertionFrames]{};
    const USHORT frameCount = RtlCaptureStackBackTrace(
        1,
        RTL_NUMBER_OF(frames),
        frames,
        nullptr);

    DbgPrintEx(
        DPFLTR_IHVVIDEO_ID,
        DPFLTR_ERROR_LEVEL,
        "NVRM: assertion failed, stack depth %hu\n",
        frameCount);
    for (USHORT index = 0; index < frameCount; ++index)
    {
        DbgPrintEx(
            DPFLTR_IHVVIDEO_ID,
            DPFLTR_ERROR_LEVEL,
            "NVRM:   [%hu] %p\n",
            index,
            frames[index]);
    }
}

extern "C" __declspec(noreturn) void NvPlatformRmBugCheck(NvU32 bugCode)
{
    KeBugCheckEx(
        RmBugCheckCode,
        RmBugCheckSignature,
        bugCode,
        reinterpret_cast<ULONG_PTR>(_ReturnAddress()),
        0);
}