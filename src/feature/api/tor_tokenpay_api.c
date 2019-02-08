/**
 * Copyright (c) 2019 TokenPay
 *
 * Distributed under the MIT/X11 software license,
 * see the accompanying file COPYING or http://www.opensource.org/licenses/mit-license.php.
 */

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define TOR_TOKENPAY_API_PRIVATE
#include <feature/api/tor_tokenpay_api.h>
#undef TOR_TOKENPAY_API_PRIVATE

#include <lib/lock/compat_mutex.h>
#include <lib/thread/threads.h>
#include <event2/event.h>
#include <lib/log/log.h>
#include <feature/api/tor_api.h>
#include <lib/evloop/compat_libevent.h>
#include <core/or/or.h>
#include <core/mainloop/mainloop.h>

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const evutil_socket_t C_INVALID_FD = -1;
tor_mutex_t* SyncMutex = NULL;
tor_cond_t* SyncConditionVariable = NULL;
int SyncReadiness = 0;
struct event* StopMainloopEvent = NULL;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TorSyncInitializePrimitives(void)
{
    SyncMutex = tor_mutex_new_nonrecursive();
    SyncConditionVariable = tor_cond_new();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TorSyncLockMutex(void)
{
    if (NULL == SyncMutex)
    {
        log_err(LD_GENERAL, "TorSyncLockMutex(): SyncMutex is NULL");
        return;
    }

    tor_mutex_acquire(SyncMutex);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int TorStart(int iArgc, char* iArgv[])
{
    tor_main_configuration_t* config = tor_main_configuration_new();
    if (tor_main_configuration_set_command_line(config, iArgc, iArgv))
    {
        log_err(LD_GENERAL, "TorStart(): tor_main_configuration_set_command_line() failed");
        return -1;
    }

    int oResult = tor_run_main(config);
    tor_main_configuration_free(config);

    return oResult;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int TorSyncCheckReadiness(void)
{
    return SyncReadiness;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TorSyncWaitForReadiness(void)
{
    if (NULL == SyncMutex)
    {
        log_err(LD_GENERAL, "TorSyncWaitForReadiness(): SyncMutex is NULL");
        return;
    }

    if (NULL == SyncConditionVariable)
    {
        log_err(LD_GENERAL, "TorSyncWaitForReadiness(): SyncConditionVariable is NULL");
        return;
    }

    // at this point SyncMutex should already be locked
    // in pthreads we can check that with pthread_mutex_trylock(), but there is no Tor compatibility implementation
    //
    if (tor_cond_wait(SyncConditionVariable, SyncMutex, NULL))
    {
        log_err(LD_GENERAL, "TorSyncWaitForReadiness(): tor_cond_wait() failed");
        return;
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TorSyncUnlockMutex(void)
{
    if (NULL == SyncMutex)
    {
        log_err(LD_GENERAL, "TorSyncUnlockMutex(): SyncMutex is NULL");
        return;
    }

    tor_mutex_release(SyncMutex);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TorSyncCleanupPrimitives(void)
{
    if (NULL == SyncMutex)
    {
        log_err(LD_GENERAL, "TorSyncCleanupPrimitives(): SyncMutex is NULL");
        return;
    }

    if (NULL == SyncConditionVariable)
    {
        log_err(LD_GENERAL, "TorSyncCleanupPrimitives(): SyncConditionVariable is NULL");
        return;
    }

    tor_cond_free(SyncConditionVariable);
    tor_mutex_free(SyncMutex);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TorStop(void)
{
    TorSyncLockMutex();
    if (0 == TorSyncCheckReadiness())
    {
        TorSyncUnlockMutex();

        log_notice(LD_GENERAL, "TorStop(): the main event loop hasn't started yet or has already been stopped");
        return;
    }
    else
    {
        TorSyncUnlockMutex();
    }

    if (StopMainloopEvent)
    {
        log_notice(LD_GENERAL,
                   "TorStop(): this function has been called before, it will take effect on next loop iteration");
        return;
    }

    if (NULL == tor_libevent_get_base())
    {
        log_err(LD_GENERAL, "TorStop(): tor_libevent_get_base() is NULL");
        return;
    }

    StopMainloopEvent = tor_event_new(tor_libevent_get_base(), C_INVALID_FD, 0, &StopMainloopEventCallback, NULL);
    if (event_add(StopMainloopEvent, NULL))
    {
        log_err(LD_GENERAL, "Error from libevent when adding the interrupt_mainloop_event");
        return;
    }

    event_active(StopMainloopEvent, 0, 0);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void StopMainloopEventCallback(evutil_socket_t iEventFd, short iEventFlags, void* iEventArg)
{
    (void) iEventFd;
    (void) iEventFlags;
    (void) iEventArg;

    if (NULL == StopMainloopEvent)
    {
        log_err(LD_GENERAL, "StopMainloopEventCallback(): StopMainloopEvent is NULL");
        return;
    }

    event_free(StopMainloopEvent);
    update_current_time(time(NULL));

    log_notice(LD_GENERAL, "StopMainloopEventCallback(): exiting cleanly");
    tor_shutdown_event_loop_and_exit(0);
}
