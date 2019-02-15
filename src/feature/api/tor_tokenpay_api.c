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

#include <stdatomic.h>
#include <lib/lock/compat_mutex.h>
#include <lib/thread/threads.h>
#include <event2/event.h>
#include <lib/log/log.h>
#include <feature/api/tor_api.h>
#include <lib/evloop/compat_libevent.h>
#include <core/or/or.h>
#include <core/mainloop/mainloop.h>

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct TorTokenpayApi_Private_StateContainer
{
    tor_mutex_t* mutex;
    tor_cond_t* conditionVariable;
    atomic_bool isMainLoopReady;
    atomic_bool isBootstrapReady;
    atomic_bool hasAnyErrorOccurred;
    atomic_bool hasShutdownBeenRequested;
    struct event* stopMainLoopEvent;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static const evutil_socket_t C_INVALID_FD = -1;
static struct TorTokenpayApi_Private_StateContainer State =
{ NULL, NULL, ATOMIC_VAR_INIT(0), ATOMIC_VAR_INIT(0), ATOMIC_VAR_INIT(0), ATOMIC_VAR_INIT(0), NULL };

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TorTokenpayApi_InitializeSyncPrimitives(void)
{
    State.mutex = tor_mutex_new_nonrecursive();
    State.conditionVariable = tor_cond_new();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TorTokenpayApi_AcquireMutex(void)
{
    if (NULL == State.mutex)
    {
        log_err(LD_GENERAL, "TorTokenpayApi_AcquireMutex(): mutex is NULL");
        return;
    }

    tor_mutex_acquire(State.mutex);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int TorTokenpayApi_StartDaemon(int iArgc, char* iArgv[])
{
    tor_main_configuration_t* config = tor_main_configuration_new();
    if (tor_main_configuration_set_command_line(config, iArgc, iArgv))
    {
        return -1;
    }

    int oResult = tor_run_main(config);
    tor_main_configuration_free(config);

    return oResult;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int TorTokenpayApi_IsMainLoopReady(void)
{
    return atomic_load(&State.isMainLoopReady);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TorTokenpayApi_Private_SetMainLoopReady(int iMainLoopReady)
{
    atomic_store(&State.isMainLoopReady, iMainLoopReady);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int TorTokenpayApi_IsBootstrapReady(void)
{
    return atomic_load(&State.isBootstrapReady);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TorTokenpayApi_Private_SetBootstrapReady(int iBootstrapReady)
{
    atomic_store(&State.isBootstrapReady, iBootstrapReady);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int TorTokenpayApi_HasAnyErrorOccurred(void)
{
    return atomic_load(&State.hasAnyErrorOccurred);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TorTokenpayApi_Private_SetErrorOccurred(int iErrorOccurred)
{
    atomic_store(&State.hasAnyErrorOccurred, iErrorOccurred);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int TorTokenpayApi_HasShutdownBeenRequested(void)
{
    return atomic_load(&State.hasShutdownBeenRequested);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TorTokenpayApi_Private_SetShutdownRequested(int iShutdownRequested)
{
    atomic_store(&State.hasShutdownBeenRequested, iShutdownRequested);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TorTokenpayApi_WaitOnConditionVariable(void)
{
    if (NULL == State.mutex)
    {
        log_err(LD_GENERAL, "TorTokenpayApi_WaitOnConditionVariable(): mutex is NULL");
        return;
    }

    if (NULL == State.conditionVariable)
    {
        log_err(LD_GENERAL, "TorTokenpayApi_WaitOnConditionVariable(): condition variable is NULL");
        return;
    }

    if (tor_cond_wait(State.conditionVariable, State.mutex, NULL))
    {
        log_err(LD_GENERAL, "TorTokenpayApi_WaitOnConditionVariable(): tor_cond_wait() failed");
        return;
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TorTokenpayApi_Private_NotifyConditionVariableWaiters(void)
{
    if (NULL == State.conditionVariable)
    {
        log_err(LD_GENERAL, "TorTokenpayApi_Private_NotifyConditionVariableWaiters(): condition variable is NULL");
        return;
    }

    tor_cond_signal_all(State.conditionVariable);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TorTokenpayApi_ReleaseMutex(void)
{
    if (NULL == State.mutex)
    {
        log_err(LD_GENERAL, "TorTokenpayApi_ReleaseMutex(): mutex is NULL");
        return;
    }

    tor_mutex_release(State.mutex);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TorTokenpayApi_CleanUpSyncPrimitives(void)
{
    if (NULL == State.mutex)
    {
        log_err(LD_GENERAL, "TorTokenpayApi_CleanUpSyncPrimitives(): mutex is NULL");
        return;
    }

    if (NULL == State.conditionVariable)
    {
        log_err(LD_GENERAL, "TorTokenpayApi_CleanUpSyncPrimitives(): conditionVariable is NULL");
        return;
    }

    TorTokenpayApi_Private_SetMainLoopReady(0);
    TorTokenpayApi_Private_SetBootstrapReady(0);
    TorTokenpayApi_Private_SetErrorOccurred(0);
    TorTokenpayApi_Private_SetShutdownRequested(0);

    tor_cond_free(State.conditionVariable);
    tor_mutex_free(State.mutex);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TorTokenpayApi_StopDaemon(void)
{
    if (TorTokenpayApi_HasShutdownBeenRequested())
    {
        log_notice(LD_GENERAL,
                   "TorTokenpayApi_StopDaemon(): this function has been called before, wait until next loop iteration");
        return;
    }

    TorTokenpayApi_Private_SetShutdownRequested(1);

    if (0 == TorTokenpayApi_IsMainLoopReady())
    {
        log_notice(LD_GENERAL, "TorTokenpayApi_StopDaemon(): MainLoop isn't ready, notifying cv waiters");
        TorTokenpayApi_Private_NotifyConditionVariableWaiters();

        return;
    }

    if (NULL == tor_libevent_get_base())
    {
        log_err(LD_GENERAL, "TorTokenpayApi_StopDaemon(): tor_libevent_get_base() is NULL");
        return;
    }

    State.stopMainLoopEvent = tor_event_new(tor_libevent_get_base(), C_INVALID_FD, 0, &StopMainLoopEventCallback, NULL);
    if (event_add(State.stopMainLoopEvent, NULL))
    {
        log_err(LD_GENERAL, "TorTokenpayApi_StopDaemon(): Error from libevent when adding the stopMainLoopEvent");
        return;
    }

    event_active(State.stopMainLoopEvent, 0, 0);

    // after inserting the stopping event, also wake up waiters since bootstrapping will never get ready
    //
    if (0 == TorTokenpayApi_IsBootstrapReady())
    {
        TorTokenpayApi_Private_NotifyConditionVariableWaiters();
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void StopMainLoopEventCallback(evutil_socket_t iEventFd, short iEventFlags, void* iEventArg)
{
    (void) iEventFd;
    (void) iEventFlags;
    (void) iEventArg;

    if (NULL == State.stopMainLoopEvent)
    {
        log_err(LD_GENERAL, "StopMainLoopEventCallback(): stopMainloopEvent is NULL");
        return;
    }

    event_free(State.stopMainLoopEvent);
    update_current_time(time(NULL));

    log_notice(LD_GENERAL, "StopMainLoopEventCallback(): exiting cleanly");
    tor_shutdown_event_loop_and_exit(0);
}
