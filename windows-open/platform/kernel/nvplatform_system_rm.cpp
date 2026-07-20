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

struct CodeViewRsdsHeader
{
    ULONG signature;
    GUID identifier;
    ULONG age;
};

constexpr ULONG CodeViewRsdsSignature = 'SDSR';

struct CrashLogRecord
{
    LIST_ENTRY listEntry;
    KBUGCHECK_REASON_CALLBACK_RECORD callbackRecord;
    void* buffer;
    NvU32 length;
};

constexpr ULONG CrashLogPoolTag = 'cRvN';
UCHAR crashLogComponent[] = "NVIDIA RM nvlog";
const GUID crashLogGuid =
    {0x4f6f38b7, 0x2e7d, 0x4a61, {0xa8, 0x5f, 0x1b, 0x56, 0xea, 0xe5, 0x33, 0x70}};

EX_RUNDOWN_REF systemWorkRundown;
EX_RUNDOWN_REF gpuWorkRundown;
volatile LONG systemWorkState = SystemWorkStateNeverInitialized;
PDRIVER_OBJECT systemWorkDriverObject;
KSEMAPHORE rmSema;
LIST_ENTRY crashLogRecords;
KSPIN_LOCK crashLogLock;

void CrashLogDumpCallback(
    KBUGCHECK_CALLBACK_REASON reason,
    PKBUGCHECK_REASON_CALLBACK_RECORD callbackRecord,
    void* reasonSpecificData,
    ULONG reasonSpecificDataLength)
{
    if (reason != KbCallbackSecondaryDumpData ||
        reasonSpecificData == nullptr ||
        reasonSpecificDataLength < sizeof(KBUGCHECK_SECONDARY_DUMP_DATA))
    {
        return;
    }

    auto* record = CONTAINING_RECORD(
        callbackRecord,
        CrashLogRecord,
        callbackRecord);
    auto* dumpData = static_cast<PKBUGCHECK_SECONDARY_DUMP_DATA>(
        reasonSpecificData);
    dumpData->Guid = crashLogGuid;
    dumpData->OutBuffer = record->buffer;
    dumpData->OutBufferLength = min(record->length, dumpData->MaximumAllowed);
}

void DrainCrashLogs()
{
    for (;;)
    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&crashLogLock, &oldIrql);
        if (IsListEmpty(&crashLogRecords))
        {
            KeReleaseSpinLock(&crashLogLock, oldIrql);
            break;
        }

        PLIST_ENTRY entry = RemoveHeadList(&crashLogRecords);
        KeReleaseSpinLock(&crashLogLock, oldIrql);
        auto* record = CONTAINING_RECORD(entry, CrashLogRecord, listEntry);
        if (KeDeregisterBugCheckReasonCallback(&record->callbackRecord))
        {
            ExFreePoolWithTag(record, CrashLogPoolTag);
        }
    }
}

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
    ExInitializeRundownProtection(&gpuWorkRundown);
    InitializeListHead(&crashLogRecords);
    KeInitializeSpinLock(&crashLogLock);
    return STATUS_SUCCESS;
}

void NvPlatformSystemShutdown(void) noexcept
{
    NvPlatformRmDrainSystemWork();
    DrainCrashLogs();
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

extern "C" NV_STATUS NvPlatformRmGetDriverImageInfo(
    void** imageBase,
    NvU8 debugIdentifier[16],
    NvU32* debugAge,
    NvU32* codeOffset)
{
    if (imageBase == nullptr || debugIdentifier == nullptr ||
        debugAge == nullptr || codeOffset == nullptr ||
        systemWorkDriverObject == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    auto* base = static_cast<NvU8*>(systemWorkDriverObject->DriverStart);
    const ULONG imageSize = systemWorkDriverObject->DriverSize;
    if (base == nullptr || imageSize < sizeof(IMAGE_DOS_HEADER))
    {
        return NV_ERR_INVALID_STATE;
    }

    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew < 0 ||
        static_cast<ULONG>(dosHeader->e_lfanew) > imageSize - sizeof(IMAGE_NT_HEADERS))
    {
        return NV_ERR_INVALID_DATA;
    }

    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        base + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
    {
        return NV_ERR_INVALID_DATA;
    }

    *imageBase = base;
    *debugAge = 0;
    *codeOffset = 0;
    RtlZeroMemory(debugIdentifier, 16);

    const auto* section = IMAGE_FIRST_SECTION(ntHeaders);
    const SIZE_T sectionOffset =
        reinterpret_cast<const NvU8*>(section) - base;
    if (sectionOffset > imageSize ||
        ntHeaders->FileHeader.NumberOfSections >
            (imageSize - sectionOffset) / sizeof(IMAGE_SECTION_HEADER))
    {
        return NV_ERR_INVALID_DATA;
    }
    for (USHORT index = 0; index < ntHeaders->FileHeader.NumberOfSections; ++index)
    {
        if ((section[index].Characteristics & IMAGE_SCN_CNT_CODE) != 0)
        {
            *codeOffset = section[index].VirtualAddress;
            break;
        }
    }

    if (ntHeaders->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG)
    {
        return NV_OK;
    }
    const IMAGE_DATA_DIRECTORY& debugDirectoryEntry =
        ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (debugDirectoryEntry.VirtualAddress == 0 || debugDirectoryEntry.Size == 0 ||
        debugDirectoryEntry.VirtualAddress > imageSize ||
        debugDirectoryEntry.Size > imageSize - debugDirectoryEntry.VirtualAddress)
    {
        return NV_OK;
    }

    const auto* debugDirectories = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(
        base + debugDirectoryEntry.VirtualAddress);
    const ULONG debugDirectoryCount =
        debugDirectoryEntry.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    for (ULONG index = 0; index < debugDirectoryCount; ++index)
    {
        const IMAGE_DEBUG_DIRECTORY& directory = debugDirectories[index];
        if (directory.Type != IMAGE_DEBUG_TYPE_CODEVIEW ||
            directory.AddressOfRawData > imageSize ||
            directory.SizeOfData < sizeof(CodeViewRsdsHeader) ||
            directory.SizeOfData > imageSize - directory.AddressOfRawData)
        {
            continue;
        }

        const auto* codeView = reinterpret_cast<const CodeViewRsdsHeader*>(
            base + directory.AddressOfRawData);
        if (codeView->signature == CodeViewRsdsSignature)
        {
            RtlCopyMemory(debugIdentifier, &codeView->identifier, 16);
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

    RTL_OSVERSIONINFOEXW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    const NTSTATUS status = RtlGetVersion(
        reinterpret_cast<PRTL_OSVERSIONINFOW>(&version));
    if (!NT_SUCCESS(status))
    {
        return NV_ERR_OPERATING_SYSTEM;
    }

    *majorVersion = version.dwMajorVersion;
    *minorVersion = version.dwMinorVersion;
    *buildNumber = version.dwBuildNumber;
    *productType = version.wProductType;
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformRmAddCrashLog(void* buffer, NvU32 length)
{
    if (buffer == nullptr || length == 0 || systemWorkDriverObject == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    auto* record = static_cast<CrashLogRecord*>(ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(CrashLogRecord),
        CrashLogPoolTag));
    if (record == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }

    record->buffer = buffer;
    record->length = length;
    KeInitializeCallbackRecord(&record->callbackRecord);
    if (!KeRegisterBugCheckReasonCallback(
            &record->callbackRecord,
            CrashLogDumpCallback,
            KbCallbackSecondaryDumpData,
            crashLogComponent))
    {
        ExFreePoolWithTag(record, CrashLogPoolTag);
        return NV_ERR_OPERATING_SYSTEM;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&crashLogLock, &oldIrql);
    InsertTailList(&crashLogRecords, &record->listEntry);
    KeReleaseSpinLock(&crashLogLock, oldIrql);
    return NV_OK;
}

extern "C" void NvPlatformRmDeleteCrashLog(void* buffer)
{
    if (buffer == nullptr)
    {
        return;
    }

    CrashLogRecord* record = nullptr;
    KIRQL oldIrql;
    KeAcquireSpinLock(&crashLogLock, &oldIrql);
    for (PLIST_ENTRY entry = crashLogRecords.Flink;
         entry != &crashLogRecords;
         entry = entry->Flink)
    {
        auto* candidate = CONTAINING_RECORD(entry, CrashLogRecord, listEntry);
        if (candidate->buffer == buffer)
        {
            RemoveEntryList(entry);
            record = candidate;
            break;
        }
    }
    KeReleaseSpinLock(&crashLogLock, oldIrql);

    if (record != nullptr &&
        KeDeregisterBugCheckReasonCallback(&record->callbackRecord))
    {
        ExFreePoolWithTag(record, CrashLogPoolTag);
    }
}

extern "C" void NvPlatformRmLogErrorV(
    NvU32 xid,
    const char* format,
    va_list arguments)
{
    if (format == nullptr || *format == '\0')
    {
        return;
    }

    DbgPrintEx(
        DPFLTR_IHVVIDEO_ID,
        DPFLTR_ERROR_LEVEL,
        "NVRM: Xid %u: ",
        xid);
    vDbgPrintEx(
        DPFLTR_IHVVIDEO_ID,
        DPFLTR_ERROR_LEVEL,
        format,
        arguments);
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

extern "C" NvBool NvPlatformRmSystemWorkIsActive(void)
{
    return InterlockedCompareExchange(
        &systemWorkState,
        SystemWorkStateActive,
        SystemWorkStateActive) == SystemWorkStateActive
        ? NV_TRUE
        : NV_FALSE;
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

extern "C" NvBool NvPlatformRmGpuWorkAcquire(void)
{
    return ExAcquireRundownProtection(&gpuWorkRundown)
        ? NV_TRUE
        : NV_FALSE;
}

extern "C" void NvPlatformRmGpuWorkRelease(void)
{
    ExReleaseRundownProtection(&gpuWorkRundown);
}

extern "C" void NvPlatformRmGpuDestroyBegin(void)
{
    // Unlike IO_REMOVE_LOCK, this gate is reopened because RM can destroy one
    // GPU while retaining others. Callers hold RM's API lock before draining.
    ExWaitForRundownProtectionRelease(&gpuWorkRundown);
}

extern "C" void NvPlatformRmGpuDestroyEnd(void)
{
    ExReInitializeRundownProtection(&gpuWorkRundown);
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