#include "core/thread_state.h"
#include "g_hal_register.h"

NV_STATUS osRmInitRm(void)
{
    NV_STATUS status = REGISTER_ALL_HALS();
    if (status != NV_OK)
    {
        return status;
    }

    threadStateInitSetupFlags(
        THREAD_STATE_SETUP_FLAGS_ENABLED |
        THREAD_STATE_SETUP_FLAGS_TIMEOUT_ENABLED |
        THREAD_STATE_SETUP_FLAGS_SLI_LOGIC_ENABLED |
        THREAD_STATE_SETUP_FLAGS_DO_NOT_INCLUDE_SLEEP_TIME_ENABLED);

    return NV_OK;
}