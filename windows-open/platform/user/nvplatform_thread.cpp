#include "nvplatform_thread.h"

#include <limits.h>
#include <windows.h>

namespace {

struct ThreadState
{
    ThreadState* next;
    HANDLE handle;
    NV_PLATFORM_THREAD_MAIN threadMain;
    void* argument;
    DWORD threadId;
};

CRITICAL_SECTION threadListLock;
ThreadState* threadList;
bool threadRegistryInitialized;

ThreadState* FindThreadStateLocked(NvU64 threadId)
{
    for (ThreadState* state = threadList; state != nullptr; state = state->next)
    {
        if (state->threadId == threadId)
        {
            return state;
        }
    }

    return nullptr;
}

HANDLE OpenRegisteredThread(NvU64 threadId)
{
    HANDLE duplicate = nullptr;

    if (!threadRegistryInitialized)
    {
        return nullptr;
    }

    EnterCriticalSection(&threadListLock);
    ThreadState* state = FindThreadStateLocked(threadId);
    if (state != nullptr)
    {
        DuplicateHandle(
            GetCurrentProcess(),
            state->handle,
            GetCurrentProcess(),
            &duplicate,
            0,
            FALSE,
            DUPLICATE_SAME_ACCESS);
    }
    LeaveCriticalSection(&threadListLock);
    return duplicate;
}

DWORD WINAPI ThreadStart(void* context)
{
    ThreadState* state = static_cast<ThreadState*>(context);
    return static_cast<DWORD>(state->threadMain(state->argument));
}

} // namespace

extern "C" NV_STATUS NvPlatformThreadInitialize(void)
{
    if (threadRegistryInitialized)
    {
        return NV_ERR_STATE_IN_USE;
    }

    InitializeCriticalSection(&threadListLock);
    threadList = nullptr;
    threadRegistryInitialized = true;
    return NV_OK;
}

extern "C" void NvPlatformThreadShutdown(void)
{
    while (threadRegistryInitialized)
    {
        EnterCriticalSection(&threadListLock);
        const NvU64 threadId = threadList != nullptr ? threadList->threadId : 0;
        LeaveCriticalSection(&threadListLock);

        if (threadId == 0)
        {
            threadRegistryInitialized = false;
            DeleteCriticalSection(&threadListLock);
            return;
        }

        NvPlatformThreadDestroy(threadId);
    }
}

extern "C" NV_STATUS NvPlatformThreadCreate(
    NvU64* threadId,
    NV_PLATFORM_THREAD_MAIN threadMain,
    void* argument)
{
    if (!threadRegistryInitialized || threadId == nullptr || threadMain == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *threadId = 0;
    ThreadState* state = static_cast<ThreadState*>(HeapAlloc(
        GetProcessHeap(),
        HEAP_ZERO_MEMORY,
        sizeof(ThreadState)));
    if (state == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }

    state->threadMain = threadMain;
    state->argument = argument;
    state->handle = CreateThread(
        nullptr,
        0,
        ThreadStart,
        state,
        CREATE_SUSPENDED,
        &state->threadId);
    if (state->handle == nullptr)
    {
        HeapFree(GetProcessHeap(), 0, state);
        return GetLastError() == ERROR_NOT_ENOUGH_MEMORY ?
            NV_ERR_NO_MEMORY : NV_ERR_OPERATING_SYSTEM;
    }

    EnterCriticalSection(&threadListLock);
    state->next = threadList;
    threadList = state;
    LeaveCriticalSection(&threadListLock);

    *threadId = state->threadId;
    if (ResumeThread(state->handle) == static_cast<DWORD>(-1))
    {
        EnterCriticalSection(&threadListLock);
        threadList = state->next;
        LeaveCriticalSection(&threadListLock);
        CloseHandle(state->handle);
        HeapFree(GetProcessHeap(), 0, state);
        *threadId = 0;
        return NV_ERR_OPERATING_SYSTEM;
    }

    return NV_OK;
}

extern "C" void NvPlatformThreadDestroy(NvU64 threadId)
{
    if (!threadRegistryInitialized || threadId == GetCurrentThreadId())
    {
        return;
    }

    HANDLE threadHandle = OpenRegisteredThread(threadId);
    if (threadHandle == nullptr)
    {
        return;
    }

    WaitForSingleObject(threadHandle, INFINITE);
    CloseHandle(threadHandle);

    EnterCriticalSection(&threadListLock);
    ThreadState** link = &threadList;
    while (*link != nullptr && (*link)->threadId != threadId)
    {
        link = &(*link)->next;
    }

    ThreadState* state = *link;
    if (state != nullptr)
    {
        *link = state->next;
    }
    LeaveCriticalSection(&threadListLock);

    if (state != nullptr)
    {
        CloseHandle(state->handle);
        HeapFree(GetProcessHeap(), 0, state);
    }
}

extern "C" void NvPlatformThreadExit(NvS32 status)
{
    ExitThread(static_cast<DWORD>(status));
}

extern "C" NV_STATUS NvPlatformThreadWait(
    NvU64 threadId,
    NvS32* returnStatus)
{
    HANDLE threadHandle = OpenRegisteredThread(threadId);
    if (threadHandle == nullptr)
    {
        return threadRegistryInitialized ? NV_ERR_OBJECT_NOT_FOUND : NV_ERR_INVALID_STATE;
    }

    const DWORD waitStatus = WaitForSingleObject(threadHandle, INFINITE);
    DWORD exitCode = 0;
    const BOOL queried = waitStatus == WAIT_OBJECT_0 &&
        GetExitCodeThread(threadHandle, &exitCode);
    CloseHandle(threadHandle);
    if (!queried)
    {
        return NV_ERR_OPERATING_SYSTEM;
    }

    if (returnStatus != nullptr)
    {
        *returnStatus = static_cast<NvS32>(exitCode);
    }
    return NV_OK;
}

extern "C" NvU64 NvPlatformThreadGetPriority(NvU64 threadId)
{
    HANDLE threadHandle = OpenRegisteredThread(threadId);
    if (threadHandle == nullptr)
    {
        threadHandle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(threadId));
    }
    if (threadHandle == nullptr)
    {
        return 0;
    }

    const int priority = GetThreadPriority(threadHandle);
    CloseHandle(threadHandle);
    return priority == THREAD_PRIORITY_ERROR_RETURN ? 0 :
        static_cast<NvU64>(priority + 8);
}

extern "C" void NvPlatformThreadSetPriority(NvU64 threadId, NvU64 priority)
{
    HANDLE threadHandle = OpenRegisteredThread(threadId);
    if (threadHandle == nullptr)
    {
        threadHandle = OpenThread(THREAD_SET_INFORMATION, FALSE, static_cast<DWORD>(threadId));
    }
    if (threadHandle != nullptr)
    {
        int windowsPriority = static_cast<int>(priority) - 8;
        if (windowsPriority < THREAD_PRIORITY_IDLE)
        {
            windowsPriority = THREAD_PRIORITY_IDLE;
        }
        else if (windowsPriority > THREAD_PRIORITY_TIME_CRITICAL)
        {
            windowsPriority = THREAD_PRIORITY_TIME_CRITICAL;
        }
        SetThreadPriority(threadHandle, windowsPriority);
        CloseHandle(threadHandle);
    }
}

extern "C" NV_STATUS NvPlatformThreadSetAffinity(
    NvU64 group,
    NvU64 mask,
    NvU64* previousGroup,
    NvU64* previousMask)
{
    if (previousGroup == nullptr || previousMask == nullptr ||
        group > USHRT_MAX || mask == 0)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    GROUP_AFFINITY affinity{};
    GROUP_AFFINITY previousAffinity{};
    affinity.Group = static_cast<USHORT>(group);
    affinity.Mask = static_cast<KAFFINITY>(mask);
    if (!SetThreadGroupAffinity(GetCurrentThread(), &affinity, &previousAffinity))
    {
        return NV_ERR_OPERATING_SYSTEM;
    }

    *previousGroup = previousAffinity.Group;
    *previousMask = previousAffinity.Mask;
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformThreadSetAffinityByIndex(
    NvU32 coreIndex,
    NvU64* previousGroup,
    NvU64* previousMask)
{
    const WORD groupCount = GetActiveProcessorGroupCount();
    for (WORD group = 0; group < groupCount; ++group)
    {
        const DWORD processorCount = GetActiveProcessorCount(group);
        if (coreIndex < processorCount)
        {
            return NvPlatformThreadSetAffinity(
                group,
                1ULL << coreIndex,
                previousGroup,
                previousMask);
        }
        coreIndex -= processorCount;
    }

    return NV_ERR_INVALID_ARGUMENT;
}

extern "C" NV_STATUS NvPlatformThreadRevertAffinity(
    NvU64 previousGroup,
    NvU64 previousMask)
{
    NvU64 ignoredGroup;
    NvU64 ignoredMask;
    return NvPlatformThreadSetAffinity(
        previousGroup,
        previousMask,
        &ignoredGroup,
        &ignoredMask);
}

extern "C" NV_STATUS NvPlatformThreadSetProcessNotifyRoutine(
    NV_PLATFORM_PROCESS_NOTIFY_ROUTINE,
    void*,
    NV_PLATFORM_PROCESS_NOTIFY_ROUTINE* previousNotifyRoutine,
    void** previousContext)
{
    if (previousNotifyRoutine == nullptr || previousContext == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *previousNotifyRoutine = nullptr;
    *previousContext = nullptr;
    return NV_ERR_NOT_SUPPORTED;
}

extern "C" NvU64 NvPlatformGetCurrentThreadId(void)
{
    return GetCurrentThreadId();
}

extern "C" NvU64 NvPlatformGetCurrentProcessId(void)
{
    return GetCurrentProcessId();
}

extern "C" void NvPlatformGetCurrentProcessName(char* processName, NvU32 length)
{
    if (processName == nullptr || length == 0)
    {
        return;
    }

    processName[0] = '\0';
    DWORD characterCount = length;
    if (!QueryFullProcessImageNameA(GetCurrentProcess(), 0, processName, &characterCount))
    {
        return;
    }

    NvU32 basename = characterCount;
    while (basename > 0 && processName[basename - 1] != '\\' &&
           processName[basename - 1] != '/')
    {
        --basename;
    }
    if (basename != 0)
    {
        NvU32 index = 0;
        do
        {
            processName[index] = processName[basename + index];
        } while (processName[index++] != '\0');
    }
}

extern "C" NvU32 NvPlatformGetCurrentProcessFlags(void)
{
    return 0;
}

extern "C" NvBool NvPlatformIsThreadIdValid(NvU64 threadId)
{
    if (threadId == 0 || threadId > MAXDWORD)
    {
        return NV_FALSE;
    }

    HANDLE threadHandle = OpenThread(
        THREAD_QUERY_LIMITED_INFORMATION,
        FALSE,
        static_cast<DWORD>(threadId));
    if (threadHandle == nullptr)
    {
        return NV_FALSE;
    }

    CloseHandle(threadHandle);
    return NV_TRUE;
}

extern "C" void NvPlatformYieldThread(void)
{
    (void)NvPlatformScheduleThread();
}

extern "C" NV_STATUS NvPlatformScheduleThread(void)
{
    if (!SwitchToThread())
    {
        Sleep(0);
    }
    return NV_OK;
}