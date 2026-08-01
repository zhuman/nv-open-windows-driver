#include "nvplatform_registry_rm.h"
#include "g_os_nvoc.h"
#include "nvport/nvport.h"

typedef struct OBJGPU OBJGPU;

void osQADbgRegistryInit(void)
{
    // Windows has no separate QA registry source to merge before normal reads.
}

NV_STATUS osReadRegistryDwordBase(
    OBJGPU* gpu,
    const char* valueName,
    NvU32* value)
{
    (void)gpu;
    return NvPlatformReadRmRegistryDword(valueName, value);
}

NV_STATUS osWriteRegistryDword(
    OBJGPU* gpu,
    const char* valueName,
    NvU32 value)
{
    (void)gpu;
    return NvPlatformWriteRmRegistryDword(valueName, value);
}

NV_STATUS osReadRegistryBinary(
    OBJGPU* gpu,
    const char* valueName,
    NvU8* data,
    NvU32* dataLength)
{
    (void)gpu;
    return NvPlatformReadRmRegistryBinary(valueName, data, dataLength);
}

NV_STATUS osReadRegistryStringBase(
    OBJGPU* gpu,
    const char* valueName,
    NvU8* data,
    NvU32* dataLength)
{
    (void)gpu;
    return NvPlatformReadRmRegistryString(valueName, data, dataLength);
}

NV_STATUS osGetNvGlobalRegistryDword(
    OBJGPU* gpu,
    const char* valueName,
    NvU32* value)
{
    (void)gpu;
    return NvPlatformReadRmRegistryDword(valueName, value);
}

typedef struct RM_REGISTRY_PACKAGE_CONTEXT
{
    PACKED_REGISTRY_TABLE* table;
    NvU32 entryIndex;
    NvU32 dataOffset;
} RM_REGISTRY_PACKAGE_CONTEXT;

static NV_STATUS packageRegistryEntry(
    void* context,
    const char* valueName,
    NV_PLATFORM_RM_REGISTRY_TYPE type,
    const NvU8* data,
    NvU32 dataLength)
{
    RM_REGISTRY_PACKAGE_CONTEXT* package = context;
    NvU32 nameLength = (NvU32)portStringLength(valueName) + 1;
    NvU32 payloadLength = type == NV_PLATFORM_RM_REGISTRY_DWORD
        ? 0
        : dataLength;

    if (nameLength > NV_U32_MAX - package->dataOffset ||
        payloadLength > NV_U32_MAX - package->dataOffset - nameLength)
    {
        return NV_ERR_INVALID_LIMIT;
    }

    if (package->table == NULL)
    {
        package->dataOffset += nameLength + payloadLength;
        ++package->entryIndex;
        return NV_OK;
    }

    if (package->entryIndex >= package->table->numEntries ||
        package->dataOffset + nameLength + payloadLength > package->table->size)
    {
        return NV_ERR_INVALID_STATE;
    }

    PACKED_REGISTRY_ENTRY* entry = &package->table->entries[package->entryIndex];
    NvU8* tableBytes = (NvU8*)package->table;
    entry->nameOffset = package->dataOffset;
    portMemCopy(
        tableBytes + package->dataOffset,
        nameLength,
        valueName,
        nameLength);
    package->dataOffset += nameLength;

    entry->type = (NvU8)type;
    entry->length = dataLength;
    if (type == NV_PLATFORM_RM_REGISTRY_DWORD)
    {
        portMemCopy(&entry->data, sizeof(entry->data), data, sizeof(entry->data));
    }
    else
    {
        entry->data = package->dataOffset;
        if (dataLength != 0)
        {
            portMemCopy(
                tableBytes + package->dataOffset,
                dataLength,
                data,
                dataLength);
            package->dataOffset += dataLength;
        }
    }

    ++package->entryIndex;
    return NV_OK;
}

NV_STATUS osPackageRegistry(
    OBJGPU* gpu,
    PACKED_REGISTRY_TABLE* table,
    NvU32* tableSize)
{
    (void)gpu;

    if (tableSize == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    RM_REGISTRY_PACKAGE_CONTEXT package = {
        .table = NULL,
        .entryIndex = 0,
        .dataOffset = NV_OFFSETOF(PACKED_REGISTRY_TABLE, entries)
    };
    NV_STATUS status = NvPlatformEnumerateRmRegistry(
        packageRegistryEntry,
        &package);
    if (status != NV_OK)
    {
        return status;
    }

    if (package.entryIndex >
        (NV_U32_MAX - package.dataOffset) / sizeof(PACKED_REGISTRY_ENTRY))
    {
        return NV_ERR_INVALID_LIMIT;
    }

    NvU32 requiredSize = package.dataOffset +
        package.entryIndex * sizeof(PACKED_REGISTRY_ENTRY);
    if (table == NULL)
    {
        *tableSize = requiredSize;
        return NV_OK;
    }
    if (package.entryIndex == 0)
    {
        return NV_WARN_NOTHING_TO_DO;
    }
    if (*tableSize < requiredSize)
    {
        *tableSize = requiredSize;
        return NV_ERR_BUFFER_TOO_SMALL;
    }

    table->size = requiredSize;
    table->numEntries = package.entryIndex;
    package.table = table;
    package.entryIndex = 0;
    package.dataOffset = NV_OFFSETOF(PACKED_REGISTRY_TABLE, entries) +
        table->numEntries * sizeof(PACKED_REGISTRY_ENTRY);

    status = NvPlatformEnumerateRmRegistry(packageRegistryEntry, &package);
    if (status != NV_OK)
    {
        return status;
    }
    if (package.entryIndex != table->numEntries ||
        package.dataOffset != table->size)
    {
        return NV_ERR_INVALID_STATE;
    }

    *tableSize = table->size;
    return NV_OK;
}

NV_STATUS osWriteRegistryVolatile(
    OBJGPU* gpu,
    const char* valueName,
    NvU8* data,
    NvU32 dataLength)
{
    (void)gpu;
    (void)valueName;
    (void)data;
    (void)dataLength;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osReadRegistryVolatile(
    OBJGPU* gpu,
    const char* valueName,
    NvU8* data,
    NvU32 dataLength)
{
    (void)gpu;
    (void)valueName;
    (void)data;
    (void)dataLength;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osReadRegistryVolatileSize(
    OBJGPU* gpu,
    const char* valueName,
    NvU32* dataLength)
{
    (void)gpu;
    (void)valueName;
    (void)dataLength;
    return NV_ERR_NOT_SUPPORTED;
}