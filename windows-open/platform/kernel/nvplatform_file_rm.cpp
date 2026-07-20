extern "C" {
#include <ntifs.h>
#include <ntstrsafe.h>
}

#include "nvplatform_file_rm.h"

namespace {

volatile LONG64 temporaryFileSequence;

NV_STATUS TransferFile(
    void* file,
    void* buffer,
    NvU64 size,
    NvU64 offset,
    bool write)
{
    if (file == nullptr || (buffer == nullptr && size != 0) ||
        size > MAXULONG || offset > MAXLONGLONG ||
        KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    IO_STATUS_BLOCK ioStatus{};
    LARGE_INTEGER byteOffset{};
    byteOffset.QuadPart = static_cast<LONGLONG>(offset);
    const NTSTATUS status = write
        ? ZwWriteFile(
            static_cast<HANDLE>(file),
            nullptr,
            nullptr,
            nullptr,
            &ioStatus,
            buffer,
            static_cast<ULONG>(size),
            &byteOffset,
            nullptr)
        : ZwReadFile(
            static_cast<HANDLE>(file),
            nullptr,
            nullptr,
            nullptr,
            &ioStatus,
            buffer,
            static_cast<ULONG>(size),
            &byteOffset,
            nullptr);
    return NT_SUCCESS(status) && ioStatus.Information == size
        ? NV_OK
        : NV_ERR_OPERATING_SYSTEM;
}

} // namespace

extern "C" NV_STATUS NvPlatformRmOpenTemporaryFile(void** file)
{
    if (file == nullptr || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *file = nullptr;
    wchar_t pathBuffer[96]{};
    const LONG64 sequence = InterlockedIncrement64(&temporaryFileSequence);
    NTSTATUS status = RtlStringCchPrintfW(
        pathBuffer,
        RTL_NUMBER_OF(pathBuffer),
        L"\\SystemRoot\\Temp\\nvrm-%p-%I64x.tmp",
        PsGetCurrentProcessId(),
        sequence);
    if (!NT_SUCCESS(status))
    {
        return NV_ERR_INVALID_LIMIT;
    }

    UNICODE_STRING path{};
    RtlInitUnicodeString(&path, pathBuffer);
    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(
        &attributes,
        &path,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        nullptr,
        nullptr);

    HANDLE handle = nullptr;
    IO_STATUS_BLOCK ioStatus{};
    status = ZwCreateFile(
        &handle,
        GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE | DELETE,
        &attributes,
        &ioStatus,
        nullptr,
        FILE_ATTRIBUTE_TEMPORARY,
        0,
        FILE_CREATE,
        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
            FILE_DELETE_ON_CLOSE | FILE_RANDOM_ACCESS,
        nullptr,
        0);
    if (!NT_SUCCESS(status))
    {
        return NV_ERR_OPERATING_SYSTEM;
    }

    *file = handle;
    return NV_OK;
}

extern "C" void NvPlatformRmCloseFile(void* file)
{
    if (file != nullptr)
    {
        ZwClose(static_cast<HANDLE>(file));
    }
}

extern "C" NV_STATUS NvPlatformRmWriteFile(
    void* file,
    const NvU8* buffer,
    NvU64 size,
    NvU64 offset)
{
    return TransferFile(
        file,
        const_cast<NvU8*>(buffer),
        size,
        offset,
        true);
}

extern "C" NV_STATUS NvPlatformRmReadFile(
    void* file,
    NvU8* buffer,
    NvU64 size,
    NvU64 offset)
{
    return TransferFile(file, buffer, size, offset, false);
}