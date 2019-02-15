/**
 * Copyright (c) 2019 TokenPay
 *
 * Distributed under the MIT/X11 software license,
 * see the accompanying file COPYING or http://www.opensource.org/licenses/mit-license.php.
 */

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef TOR_TOKENPAY_API_H
#define TOR_TOKENPAY_API_H

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
extern "C"
{
#endif // ifdef __cplusplus

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * TODO TSB
 */
void TorTokenpayApi_InitializeSyncPrimitives(void);

/**
 * TODO TSB
 */
void TorTokenpayApi_AcquireMutex(void);

/**
 * TODO TSB
 */
int TorTokenpayApi_StartDaemon(int iArgc, char* iArgv[]);

/**
 * TODO TSB
 */
int TorTokenpayApi_IsMainLoopReady(void);

/**
 * TODO TSB
 */
void TorTokenpayApi_Private_SetMainLoopReady(int iMainLoopReady);

/**
 * TODO TSB
 */
int TorTokenpayApi_IsBootstrapReady(void);

/**
 * TODO TSB
 */
void TorTokenpayApi_Private_SetBootstrapReady(int iBootstrapReady);

/**
 * TODO TSB
 */
int TorTokenpayApi_HasAnyErrorOccurred(void);

/**
 * TODO TSB
 */
void TorTokenpayApi_Private_SetErrorOccurred(int iErrorOccurred);

/**
 * TODO TSB
 */
int TorTokenpayApi_HasShutdownBeenRequested(void);

/**
 * TODO TSB
 */
void TorTokenpayApi_Private_SetShutdownRequested(int iShutdownRequested);

/**
 * TODO TSB
 */
void TorTokenpayApi_WaitOnConditionVariable(void);

/**
 * TODO TSB
 */
void TorTokenpayApi_Private_NotifyConditionVariableWaiters(void);

/**
 * TODO TSB
 */
void TorTokenpayApi_ReleaseMutex(void);

/**
 * TODO TSB
 */
void TorTokenpayApi_CleanUpSyncPrimitives(void);

/**
 * This function may be called from anywhere (any thread, any library), without any synchronization, to
 * insert an event in Tor's main event loop such that on the next loop iteration, Tor will exit cleanly
 */
void TorTokenpayApi_StopDaemon(void);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifdef TOR_TOKENPAY_API_PRIVATE

#include <event2/util.h>

void StopMainLoopEventCallback(evutil_socket_t iEventFd, short iEventFlags, void* iEventArg);

#endif // ifdef TOR_TOKENPAY_API_PRIVATE

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif // ifdef __cplusplus

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#endif // ifndef TOR_TOKENPAY_API_H
