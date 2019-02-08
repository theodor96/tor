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
void TorSyncInitializePrimitives(void);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * TODO TSB
 */
void TorSyncLockMutex(void);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * TODO TSB
 */
int TorStart(int iArgc, char* iArgv[]);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * TODO TSB
 */
int TorSyncCheckReadiness(void);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * TODO TSB
 */
void TorSyncSetReadiness(int iReadiness);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * TODO TSB
 */
void TorSyncWaitForReadiness(void);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * TODO TSB
 */
void TorSyncNotifyWaiters(void);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * TODO TSB
 */
void TorSyncUnlockMutex(void);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * TODO TSB
 */
void TorSyncCleanupPrimitives(void);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * This function may be called from anywhere (any thread, any library), without any synchronization, to
 * insert an event in Tor's main event loop such that on the next loop iteration, Tor will exit cleanly
 */
void TorStop(void);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifdef TOR_TOKENPAY_API_PRIVATE

#include <event2/util.h>
void StopMainloopEventCallback(evutil_socket_t iEventFd, short iEventFlags, void* iEventArg);

#endif // ifdef TOR_TOKENPAY_API_PRIVATE

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif // ifdef __cplusplus

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#endif // ifndef TOR_TOKENPAY_API_H
