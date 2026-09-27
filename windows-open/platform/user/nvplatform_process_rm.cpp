#include "nvplatform_process_rm.h"

#include <windows.h>

namespace {

NvBool HasPrivilege(const wchar_t* privilegeName)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        return NV_FALSE;
    }

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, privilegeName, &luid))
    {
        CloseHandle(token);
        return NV_FALSE;
    }

    DWORD requiredLength = 0;
    GetTokenInformation(token, TokenPrivileges, nullptr, 0, &requiredLength);
    TOKEN_PRIVILEGES* privileges = static_cast<TOKEN_PRIVILEGES*>(
        HeapAlloc(GetProcessHeap(), 0, requiredLength));
    if (privileges == nullptr)
    {
        CloseHandle(token);
        return NV_FALSE;
    }

    NvBool enabled = NV_FALSE;
    if (GetTokenInformation(
            token, TokenPrivileges, privileges, requiredLength, &requiredLength))
    {
        for (DWORD index = 0; index < privileges->PrivilegeCount; ++index)
        {
            const LUID_AND_ATTRIBUTES& privilege = privileges->Privileges[index];
            if (privilege.Luid.LowPart == luid.LowPart &&
                privilege.Luid.HighPart == luid.HighPart &&
                (privilege.Attributes & SE_PRIVILEGE_ENABLED) != 0)
            {
                enabled = NV_TRUE;
                break;
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, privileges);
    CloseHandle(token);
    return enabled;
}

} // namespace

extern "C" NV_STATUS NvPlatformRmAttachToProcess(
    void** processState,
    NvU32 processId)
{
    if (processState == nullptr || processId == 0)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *processState = nullptr;
    return processId == GetCurrentProcessId() ? NV_OK : NV_ERR_NOT_SUPPORTED;
}

extern "C" void NvPlatformRmDetachFromProcess(void*)
{
}

extern "C" NV_STATUS NvPlatformRmValidateKernelBuffer(
    const void* address,
    NvU32 size)
{
    if (size == 0)
    {
        return NV_OK;
    }
    if (address == nullptr ||
        reinterpret_cast<ULONG_PTR>(address) > MAXULONG_PTR - (size - 1))
    {
        return NV_ERR_INVALID_ADDRESS;
    }
    return NV_OK;
}

extern "C" void* NvPlatformRmReferenceCurrentProcess(void)
{
    HANDLE process = nullptr;
    return DuplicateHandle(
        GetCurrentProcess(),
        GetCurrentProcess(),
        GetCurrentProcess(),
        &process,
        PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        0) ? process : nullptr;
}

extern "C" void NvPlatformRmDereferenceProcess(void* process)
{
    if (process != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(process));
    }
}

extern "C" NV_STATUS NvPlatformRmGetProcessId(void* process, NvU32* processId)
{
    if (process == nullptr || processId == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    const DWORD id = GetProcessId(static_cast<HANDLE>(process));
    if (id == 0)
    {
        return NV_ERR_OPERATING_SYSTEM;
    }
    *processId = id;
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformRmGetCurrentProcessId(NvU32* processId)
{
    if (processId == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    *processId = GetCurrentProcessId();
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformRmGetCurrentUserSid(
    NvU8* sid,
    NvU32* sidLength)
{
    if (sidLength == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        return NV_ERR_OPERATING_SYSTEM;
    }

    DWORD requiredLength = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &requiredLength);
    TOKEN_USER* tokenUser = static_cast<TOKEN_USER*>(
        HeapAlloc(GetProcessHeap(), 0, requiredLength));
    if (tokenUser == nullptr)
    {
        CloseHandle(token);
        return NV_ERR_NO_MEMORY;
    }

    NV_STATUS status = NV_ERR_OPERATING_SYSTEM;
    if (GetTokenInformation(token, TokenUser, tokenUser, requiredLength, &requiredLength))
    {
        const DWORD requiredSidLength = GetLengthSid(tokenUser->User.Sid);
        if (sid == nullptr || *sidLength < requiredSidLength)
        {
            *sidLength = requiredSidLength;
            status = NV_ERR_BUFFER_TOO_SMALL;
        }
        else if (CopySid(*sidLength, sid, tokenUser->User.Sid))
        {
            *sidLength = requiredSidLength;
            status = NV_OK;
        }
    }

    HeapFree(GetProcessHeap(), 0, tokenUser);
    CloseHandle(token);
    return status;
}

extern "C" NvBool NvPlatformRmHasSystemProfilePrivilege(void)
{
    return HasPrivilege(L"SeSystemProfilePrivilege");
}

extern "C" NvBool NvPlatformRmHasIncreaseBasePriorityPrivilege(void)
{
    return HasPrivilege(L"SeIncreaseBasePriorityPrivilege");
}