#include "nvplatform_process_rm.h"
#include "nvport/nvport.h"
#include "rs_access.h"

typedef struct WINDOWS_RM_SECURITY_TOKEN
{
    NvU32 processId;
    NvU32 sidLength;
    NvU8 sid[0];
} WINDOWS_RM_SECURITY_TOKEN;

typedef struct WINDOWS_RM_UID_TOKEN
{
    NvU32 sidLength;
    NvU8 sid[0];
} WINDOWS_RM_UID_TOKEN;

NV_STATUS osAttachToProcess(void** processState, NvU32 processId)
{
    return NvPlatformRmAttachToProcess(processState, processId);
}

void osDetachFromProcess(void* processState)
{
    NvPlatformRmDetachFromProcess(processState);
}

NV_STATUS osIsKernelBuffer(void* address, NvU32 size)
{
    return NvPlatformRmValidateKernelBuffer(address, size);
}

void* osGetPidInfo(void)
{
    return NvPlatformRmReferenceCurrentProcess();
}

void osPutPidInfo(void* processInfo)
{
    NvPlatformRmDereferenceProcess(processInfo);
}

NV_STATUS osFindNsPid(void* processInfo, NvU32* processId)
{
    return NvPlatformRmGetProcessId(processInfo, processId);
}

NvBool osIsInitNs(void)
{
    return NV_TRUE;
}

void* osGetSecurityToken(void)
{
    NvU32 sidLength = 0;
    if (NvPlatformRmGetCurrentUserSid(NULL, &sidLength) !=
        NV_ERR_BUFFER_TOO_SMALL)
    {
        return NULL;
    }

    WINDOWS_RM_SECURITY_TOKEN* token = portMemAllocNonPaged(
        sizeof(*token) + sidLength);
    if (token == NULL)
    {
        return NULL;
    }

    token->sidLength = sidLength;
    if (NvPlatformRmGetCurrentProcessId(&token->processId) != NV_OK ||
        NvPlatformRmGetCurrentUserSid(token->sid, &token->sidLength) != NV_OK)
    {
        portMemFree(token);
        return NULL;
    }

    return token;
}

void* osGetCurrentUidToken(void)
{
    NvU32 sidLength = 0;
    if (NvPlatformRmGetCurrentUserSid(NULL, &sidLength) !=
        NV_ERR_BUFFER_TOO_SMALL)
    {
        return NULL;
    }

    WINDOWS_RM_UID_TOKEN* token = portMemAllocNonPaged(
        sizeof(*token) + sidLength);
    if (token == NULL)
    {
        return NULL;
    }

    token->sidLength = sidLength;
    if (NvPlatformRmGetCurrentUserSid(token->sid, &token->sidLength) != NV_OK)
    {
        portMemFree(token);
        return NULL;
    }

    return token;
}

NV_STATUS osValidateClientTokens(void* clientToken, void* currentToken)
{
    WINDOWS_RM_SECURITY_TOKEN* client = clientToken;
    WINDOWS_RM_SECURITY_TOKEN* current = currentToken;
    if (client == NULL || current == NULL)
    {
        return NV_ERR_INVALID_POINTER;
    }

    if (client->processId != current->processId &&
        (client->sidLength != current->sidLength ||
         portMemCmp(client->sid, current->sid, client->sidLength) != 0))
    {
        return NV_ERR_INVALID_CLIENT;
    }

    return NV_OK;
}

NvBool osUidTokensEqual(void* firstToken, void* secondToken)
{
    WINDOWS_RM_UID_TOKEN* first = firstToken;
    WINDOWS_RM_UID_TOKEN* second = secondToken;
    if (first == NULL || second == NULL ||
        first->sidLength != second->sidLength)
    {
        return NV_FALSE;
    }

    return portMemCmp(first->sid, second->sid, first->sidLength) == 0;
}

NvBool osCheckAccess(RsAccessRight accessRight)
{
    switch (accessRight)
    {
        case RS_ACCESS_PERFMON:
            return NvPlatformRmHasSystemProfilePrivilege();

        case RS_ACCESS_NICE:
            return NvPlatformRmHasIncreaseBasePriorityPrivilege();

        default:
            return NV_FALSE;
    }
}