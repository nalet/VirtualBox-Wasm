/* $Id: wasm-stubs.cpp $ */
/** @file
 * IPRT - Wasm/Emscripten platform stubs for Ring-3.
 *
 * Provides minimal implementations of platform-specific functions that
 * are normally supplied by r3/linux/ or r3/posix/ files but use
 * kernel/syscall features unavailable under Emscripten.
 */

/*
 * Copyright (C) 2026 Oracle and/or its affiliates.
 * SPDX-License-Identifier: GPL-3.0-only OR CDDL-1.0
 */

#define LOG_GROUP RTLOGGROUP_DEFAULT
#include <iprt/assert.h>
#include <iprt/errcore.h>
#include <iprt/mp.h>
#include <iprt/string.h>
#include <iprt/system.h>
#include <iprt/thread.h>
#include <iprt/mem.h>
#include <iprt/semaphore.h>
#include <iprt/time.h>
#include <iprt/err.h>
#include <iprt/env.h>
#include <iprt/file.h>
#include <iprt/log.h>
#include <iprt/process.h>

#include "internal/iprt.h"
#include "internal/process.h"
#include "internal/sched.h"
#include "internal/thread.h"
#include "../init.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <iprt/asm.h>

/**
 * Allocate zeroed, over-aligned memory suitable for pthread types.
 *
 * Emscripten's pthreads implementation uses Wasm atomic instructions
 * (i32.atomic.load, i64.atomic.store, Atomics.wait/notify) on fields
 * inside pthread_mutex_t / pthread_cond_t / pthread_rwlock_t.  These
 * Wasm atomics require naturally aligned addresses.  malloc/calloc
 * guarantee 8-byte alignment on wasm64 which is normally enough, but
 * struct packing can place atomic fields at odd offsets.
 *
 * Using 16-byte alignment gives the compiler room to satisfy the
 * alignment requirements for any internal atomic fields.
 */
static void *rtMemAllocZAligned(size_t cb)
{
    /* Round up to a multiple of 16 (required by aligned_alloc). */
    size_t cbAligned = (cb + 15) & ~(size_t)15;
    void *pv = aligned_alloc(16, cbAligned);
    if (pv)
        memset(pv, 0, cbAligned);
    return pv;
}


/*
 * ── Init stubs ──
 *
 * r3/linux/init-linux.cpp provides these using Linux-specific APIs
 * (signal handling, /proc, etc.).  For Wasm we just return success.
 */
DECLHIDDEN(int) rtR3InitNativeFirst(uint32_t fFlags)
{
    RT_NOREF_PV(fFlags);
    return VINF_SUCCESS;
}

DECLHIDDEN(int) rtR3InitNativeFinal(uint32_t fFlags)
{
    RT_NOREF_PV(fFlags);
    return VINF_SUCCESS;
}

DECLHIDDEN(void) rtR3InitNativeObtrusive(uint32_t fFlags)
{
    RT_NOREF_PV(fFlags);
}


/*
 * ── Process path stub ──
 *
 * r3/linux/rtProcInitExePath-linux.cpp reads /proc/self/exe.
 * We just set a fake path since Wasm has no real filesystem exe.
 */
DECLHIDDEN(int) rtProcInitExePath(char *pszPath, size_t cchPath)
{
    const char szFakePath[] = "/wasm/VirtualBox";
    size_t cch = sizeof(szFakePath);
    if (cch > cchPath)
        return VERR_BUFFER_OVERFLOW;
    memcpy(pszPath, szFakePath, cch);
    return VINF_SUCCESS;
}


/*
 * ── Scheduler stub ──
 *
 * r3/linux/sched-linux.cpp calculates thread priorities using
 * sched_getscheduler / sched_get_priority_min/max.
 * Wasm is single-threaded; just return default priority.
 */
DECLHIDDEN(int) rtSchedNativeCalcDefaultPriority(RTTHREADTYPE enmType)
{
    RT_NOREF_PV(enmType);
    return VINF_SUCCESS;
}


/*
 * ── MP (multiprocessor) stubs ──
 *
 * r3/linux/mp-linux.cpp reads /sys/devices/system/cpu/.
 * Wasm has exactly 1 CPU.
 */
RTDECL(RTCPUID) RTMpCpuId(void)
{
    return 0;
}


/*
 * ── String stub ──
 *
 * RTStrEnd is normally an optimized .asm file.
 * Provide a simple C implementation.
 */
RTDECL(char *) RTStrEnd(char const *pszString, size_t cchMax)
{
    const char *psz = (const char *)memchr(pszString, '\0', cchMax);
    return (char *)psz;
}


/*
 * ── Time stubs ──
 *
 * RTTimeSystemNanoTS is normally provided by r3/linux/time-linux.cpp
 * or r3/posix/time-posix.cpp using clock_gettime(CLOCK_MONOTONIC).
 * Emscripten supports clock_gettime.
 */
RTDECL(uint64_t) RTTimeSystemNanoTS(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

RTDECL(uint64_t) RTTimeSystemMilliTS(void)
{
    return RTTimeSystemNanoTS() / UINT64_C(1000000);
}


/*
 * ── Error variable stubs ──
 *
 * Normally in r3/posix/errvars-posix.cpp or generic/errvars-generic.cpp.
 * Save/restore errno for assertion handling.
 */
RTDECL(PRTERRVARS) RTErrVarsSave(PRTERRVARS pVars)
{
    pVars->ai32Vars[0] = 0x2f2f2f2f;  /* magic */
    pVars->ai32Vars[1] = errno;
    return pVars;
}

RTDECL(void) RTErrVarsRestore(PCRTERRVARS pVars)
{
    if (pVars->ai32Vars[0] == 0x2f2f2f2f)
        errno = pVars->ai32Vars[1];
}

RTDECL(bool) RTErrVarsAreEqual(PCRTERRVARS pVars1, PCRTERRVARS pVars2)
{
    return pVars1->ai32Vars[0] == pVars2->ai32Vars[0]
        && pVars1->ai32Vars[1] == pVars2->ai32Vars[1];
}

RTDECL(bool) RTErrVarsHaveChanged(PCRTERRVARS pVars)
{
    return pVars->ai32Vars[1] != errno;
}


/*
 * ── Semaphore implementations (pthread-based) ──
 *
 * Emscripten with -pthread supports real pthreads via Web Workers and
 * SharedArrayBuffer.  These implementations replace the old no-op stubs
 * so that cross-thread synchronisation (e.g. ATA async I/O ↔ EMT) works.
 */
#include <unistd.h>
#include <sys/time.h>


/*********************************************************************************************************************************
 * RTSemEvent — Auto-reset event semaphore                                                                                      *
 *********************************************************************************************************************************/

/** The values of the u32State variable in RTSEMEVENTINTERNAL. */
#define WASM_EVENT_STATE_UNINITIALIZED   0
#define WASM_EVENT_STATE_SIGNALED        0xff00ff00
#define WASM_EVENT_STATE_NOT_SIGNALED    0x00ff00ff

struct RTSEMEVENTINTERNAL
{
    /** pthread condition variable. */
    pthread_cond_t      Cond;
    /** pthread mutex protecting the condition and state. */
    pthread_mutex_t     Mutex;
    /** Semaphore state (signaled / not-signaled / uninitialized). */
    volatile uint32_t   u32State;
    /** Number of waiters. */
    volatile uint32_t   cWaiters;
    /** Creation flags. */
    uint32_t            fFlags;
};

RTDECL(int) RTSemEventCreate(PRTSEMEVENT phEventSem)
{
    return RTSemEventCreateEx(phEventSem, 0 /*fFlags*/, NIL_RTLOCKVALCLASS, NULL);
}

RTDECL(int) RTSemEventCreateEx(PRTSEMEVENT phEventSem, uint32_t fFlags, RTLOCKVALCLASS hClass, const char *pszNameFmt, ...)
{
    RT_NOREF(hClass, pszNameFmt);

    struct RTSEMEVENTINTERNAL *pThis = (struct RTSEMEVENTINTERNAL *)rtMemAllocZAligned(sizeof(*pThis));
    if (!pThis)
        return VERR_NO_MEMORY;

    int rc = pthread_mutex_init(&pThis->Mutex, NULL);
    if (rc)
    {
        free(pThis);
        return RTErrConvertFromErrno(rc);
    }

    rc = pthread_cond_init(&pThis->Cond, NULL);
    if (rc)
    {
        pthread_mutex_destroy(&pThis->Mutex);
        free(pThis);
        return RTErrConvertFromErrno(rc);
    }

    ASMAtomicWriteU32(&pThis->u32State, WASM_EVENT_STATE_NOT_SIGNALED);
    ASMAtomicWriteU32(&pThis->cWaiters, 0);
    pThis->fFlags = fFlags;

    *phEventSem = pThis;
    return VINF_SUCCESS;
}

RTDECL(int) RTSemEventDestroy(RTSEMEVENT hEventSem)
{
    struct RTSEMEVENTINTERNAL *pThis = hEventSem;
    if (pThis == NIL_RTSEMEVENT)
        return VINF_SUCCESS;
    AssertPtrReturn(pThis, VERR_INVALID_HANDLE);

    /* Mark as uninitialized and wake all waiters so they bail out. */
    int rc;
    for (int i = 30; i > 0; i--)
    {
        ASMAtomicWriteU32(&pThis->u32State, WASM_EVENT_STATE_UNINITIALIZED);
        rc = pthread_cond_destroy(&pThis->Cond);
        if (rc != EBUSY)
            break;
        pthread_cond_broadcast(&pThis->Cond);
        usleep(1000);
    }

    for (int i = 30; i > 0; i--)
    {
        rc = pthread_mutex_destroy(&pThis->Mutex);
        if (rc != EBUSY)
            break;
        usleep(1000);
    }

    free(pThis);
    return VINF_SUCCESS;
}

RTDECL(int) RTSemEventSignal(RTSEMEVENT hEventSem)
{
    struct RTSEMEVENTINTERNAL *pThis = hEventSem;
    AssertPtrReturn(pThis, VERR_INVALID_HANDLE);

    int rc = pthread_mutex_lock(&pThis->Mutex);
    if (rc)
        return RTErrConvertFromErrno(rc);

    if (pThis->u32State == WASM_EVENT_STATE_NOT_SIGNALED)
    {
        ASMAtomicWriteU32(&pThis->u32State, WASM_EVENT_STATE_SIGNALED);
        pthread_cond_signal(&pThis->Cond);
    }
    else if (pThis->u32State == WASM_EVENT_STATE_SIGNALED)
    {
        /* Already signaled, give another kick in case of spurious issues. */
        pthread_cond_signal(&pThis->Cond);
    }

    pthread_mutex_unlock(&pThis->Mutex);
    return VINF_SUCCESS;
}

/**
 * Internal: indefinite wait on auto-reset event.
 */
static int rtSemEventWasmWaitIndefinite(struct RTSEMEVENTINTERNAL *pThis)
{
    if (ASMAtomicIncU32(&pThis->cWaiters) > 1
        && pThis->u32State == WASM_EVENT_STATE_SIGNALED)
        sched_yield();

    int rc = pthread_mutex_lock(&pThis->Mutex);
    if (rc)
    {
        ASMAtomicDecU32(&pThis->cWaiters);
        return RTErrConvertFromErrno(rc);
    }

    for (;;)
    {
        if (pThis->u32State == WASM_EVENT_STATE_SIGNALED)
        {
            ASMAtomicWriteU32(&pThis->u32State, WASM_EVENT_STATE_NOT_SIGNALED); /* auto-reset */
            ASMAtomicDecU32(&pThis->cWaiters);
            pthread_mutex_unlock(&pThis->Mutex);
            return VINF_SUCCESS;
        }
        if (pThis->u32State == WASM_EVENT_STATE_UNINITIALIZED)
        {
            ASMAtomicDecU32(&pThis->cWaiters);
            pthread_mutex_unlock(&pThis->Mutex);
            return VERR_SEM_DESTROYED;
        }

        rc = pthread_cond_wait(&pThis->Cond, &pThis->Mutex);
        if (rc)
        {
            ASMAtomicDecU32(&pThis->cWaiters);
            pthread_mutex_unlock(&pThis->Mutex);
            return RTErrConvertFromErrno(rc);
        }
    }
}

/**
 * Internal: timed wait on auto-reset event.
 */
static int rtSemEventWasmWaitTimed(struct RTSEMEVENTINTERNAL *pThis, RTMSINTERVAL cMillies)
{
    /* Calculate absolute deadline. */
    struct timespec AbsDeadline;
    clock_gettime(CLOCK_REALTIME, &AbsDeadline);
    uint64_t nsTimeout = (uint64_t)cMillies * UINT64_C(1000000);
    AbsDeadline.tv_sec  += (time_t)(nsTimeout / UINT64_C(1000000000));
    AbsDeadline.tv_nsec += (long)(nsTimeout % UINT64_C(1000000000));
    if (AbsDeadline.tv_nsec >= 1000000000L)
    {
        AbsDeadline.tv_nsec -= 1000000000L;
        AbsDeadline.tv_sec++;
    }

    ASMAtomicIncU32(&pThis->cWaiters);

    int rc = pthread_mutex_lock(&pThis->Mutex);
    if (rc)
    {
        ASMAtomicDecU32(&pThis->cWaiters);
        return RTErrConvertFromErrno(rc);
    }

    for (;;)
    {
        if (pThis->u32State == WASM_EVENT_STATE_SIGNALED)
        {
            ASMAtomicWriteU32(&pThis->u32State, WASM_EVENT_STATE_NOT_SIGNALED); /* auto-reset */
            ASMAtomicDecU32(&pThis->cWaiters);
            pthread_mutex_unlock(&pThis->Mutex);
            return VINF_SUCCESS;
        }
        if (pThis->u32State == WASM_EVENT_STATE_UNINITIALIZED)
        {
            ASMAtomicDecU32(&pThis->cWaiters);
            pthread_mutex_unlock(&pThis->Mutex);
            return VERR_SEM_DESTROYED;
        }

        rc = pthread_cond_timedwait(&pThis->Cond, &pThis->Mutex, &AbsDeadline);
        if (rc == ETIMEDOUT)
        {
            /* One last check — signal may have arrived just as we timed out. */
            if (pThis->u32State == WASM_EVENT_STATE_SIGNALED)
            {
                ASMAtomicWriteU32(&pThis->u32State, WASM_EVENT_STATE_NOT_SIGNALED);
                ASMAtomicDecU32(&pThis->cWaiters);
                pthread_mutex_unlock(&pThis->Mutex);
                return VINF_SUCCESS;
            }
            ASMAtomicDecU32(&pThis->cWaiters);
            pthread_mutex_unlock(&pThis->Mutex);
            return VERR_TIMEOUT;
        }
        if (rc && rc != EINTR)
        {
            ASMAtomicDecU32(&pThis->cWaiters);
            pthread_mutex_unlock(&pThis->Mutex);
            return RTErrConvertFromErrno(rc);
        }
    }
}

RTDECL(int) RTSemEventWait(RTSEMEVENT hEventSem, RTMSINTERVAL cMillies)
{
    struct RTSEMEVENTINTERNAL *pThis = hEventSem;
    AssertPtrReturn(pThis, VERR_INVALID_HANDLE);

    if (cMillies == RT_INDEFINITE_WAIT)
        return rtSemEventWasmWaitIndefinite(pThis);

    /* Poll (zero timeout): just check state under lock. */
    if (cMillies == 0)
    {
        int rc = pthread_mutex_lock(&pThis->Mutex);
        if (rc)
            return RTErrConvertFromErrno(rc);
        if (pThis->u32State == WASM_EVENT_STATE_SIGNALED)
        {
            ASMAtomicWriteU32(&pThis->u32State, WASM_EVENT_STATE_NOT_SIGNALED);
            pthread_mutex_unlock(&pThis->Mutex);
            return VINF_SUCCESS;
        }
        int rcRet = (pThis->u32State == WASM_EVENT_STATE_UNINITIALIZED) ? VERR_SEM_DESTROYED : VERR_TIMEOUT;
        pthread_mutex_unlock(&pThis->Mutex);
        return rcRet;
    }

    return rtSemEventWasmWaitTimed(pThis, cMillies);
}

RTDECL(int) RTSemEventWaitNoResume(RTSEMEVENT hEventSem, RTMSINTERVAL cMillies)
{
    /* In Emscripten pthread_cond_wait doesn't return EINTR, so this is equivalent. */
    return RTSemEventWait(hEventSem, cMillies);
}

RTDECL(int) RTSemEventWaitEx(RTSEMEVENT hEventSem, uint32_t fFlags, uint64_t uTimeout)
{
    struct RTSEMEVENTINTERNAL *pThis = hEventSem;
    AssertPtrReturn(pThis, VERR_INVALID_HANDLE);

    if (fFlags & RTSEMWAIT_FLAGS_INDEFINITE)
        return rtSemEventWasmWaitIndefinite(pThis);

    /* Convert to milliseconds. */
    RTMSINTERVAL cMillies;
    if (fFlags & RTSEMWAIT_FLAGS_NANOSECS)
        cMillies = (RTMSINTERVAL)((uTimeout + RT_NS_1MS - 1) / RT_NS_1MS);
    else
        cMillies = (RTMSINTERVAL)uTimeout;

    if (fFlags & RTSEMWAIT_FLAGS_ABSOLUTE)
    {
        /* Convert absolute to relative. */
        uint64_t u64Now = RTTimeSystemNanoTS();
        uint64_t u64Abs;
        if (fFlags & RTSEMWAIT_FLAGS_NANOSECS)
            u64Abs = uTimeout;
        else
            u64Abs = uTimeout * RT_NS_1MS;
        if (u64Abs <= u64Now)
            cMillies = 0;
        else
            cMillies = (RTMSINTERVAL)((u64Abs - u64Now + RT_NS_1MS - 1) / RT_NS_1MS);
    }

    return RTSemEventWait(hEventSem, cMillies);
}

RTDECL(int) RTSemEventWaitExDebug(RTSEMEVENT hEventSem, uint32_t fFlags, uint64_t uTimeout,
                                   RTHCUINTPTR uId, RT_SRC_POS_DECL)
{
    RT_NOREF(uId, RT_SRC_POS_ARGS);
    return RTSemEventWaitEx(hEventSem, fFlags, uTimeout);
}

RTDECL(uint32_t) RTSemEventGetResolution(void)
{
    return 1000000; /* 1ms */
}

RTDECL(void) RTSemEventSetSignaller(RTSEMEVENT hEventSem, RTTHREAD hThread)
{
    RT_NOREF(hEventSem, hThread);
}

RTDECL(void) RTSemEventAddSignaller(RTSEMEVENT hEventSem, RTTHREAD hThread)
{
    RT_NOREF(hEventSem, hThread);
}

RTDECL(void) RTSemEventRemoveSignaller(RTSEMEVENT hEventSem, RTTHREAD hThread)
{
    RT_NOREF(hEventSem, hThread);
}


/*********************************************************************************************************************************
 * RTSemEventMulti — Manual-reset event semaphore                                                                                *
 *********************************************************************************************************************************/

#define WASM_EVENTMULTI_STATE_UNINITIALIZED  0
#define WASM_EVENTMULTI_STATE_SIGNALED       0xff00ff00
#define WASM_EVENTMULTI_STATE_NOT_SIGNALED   0x00ff00ff

struct RTSEMEVENTMULTIINTERNAL
{
    /** pthread condition variable. */
    pthread_cond_t      Cond;
    /** pthread mutex protecting the condition and state. */
    pthread_mutex_t     Mutex;
    /** Semaphore state (signaled / not-signaled / uninitialized). */
    volatile uint32_t   u32State;
    /** Number of waiters. */
    volatile uint32_t   cWaiters;
};

RTDECL(int) RTSemEventMultiCreate(PRTSEMEVENTMULTI phEventMultiSem)
{
    return RTSemEventMultiCreateEx(phEventMultiSem, 0 /*fFlags*/, NIL_RTLOCKVALCLASS, NULL);
}

RTDECL(int) RTSemEventMultiCreateEx(PRTSEMEVENTMULTI phEventMultiSem, uint32_t fFlags, RTLOCKVALCLASS hClass, const char *pszNameFmt, ...)
{
    RT_NOREF(fFlags, hClass, pszNameFmt);

    struct RTSEMEVENTMULTIINTERNAL *pThis = (struct RTSEMEVENTMULTIINTERNAL *)rtMemAllocZAligned(sizeof(*pThis));
    if (!pThis)
        return VERR_NO_MEMORY;

    int rc = pthread_mutex_init(&pThis->Mutex, NULL);
    if (rc)
    {
        free(pThis);
        return RTErrConvertFromErrno(rc);
    }

    rc = pthread_cond_init(&pThis->Cond, NULL);
    if (rc)
    {
        pthread_mutex_destroy(&pThis->Mutex);
        free(pThis);
        return RTErrConvertFromErrno(rc);
    }

    ASMAtomicWriteU32(&pThis->u32State, WASM_EVENTMULTI_STATE_NOT_SIGNALED);
    ASMAtomicWriteU32(&pThis->cWaiters, 0);

    *phEventMultiSem = pThis;
    return VINF_SUCCESS;
}

RTDECL(int) RTSemEventMultiDestroy(RTSEMEVENTMULTI hEventMultiSem)
{
    struct RTSEMEVENTMULTIINTERNAL *pThis = hEventMultiSem;
    if (pThis == NIL_RTSEMEVENTMULTI)
        return VINF_SUCCESS;
    AssertPtrReturn(pThis, VERR_INVALID_HANDLE);

    int rc;
    for (int i = 30; i > 0; i--)
    {
        ASMAtomicWriteU32(&pThis->u32State, WASM_EVENTMULTI_STATE_UNINITIALIZED);
        rc = pthread_cond_destroy(&pThis->Cond);
        if (rc != EBUSY)
            break;
        pthread_cond_broadcast(&pThis->Cond);
        usleep(1000);
    }

    for (int i = 30; i > 0; i--)
    {
        rc = pthread_mutex_destroy(&pThis->Mutex);
        if (rc != EBUSY)
            break;
        usleep(1000);
    }

    free(pThis);
    return VINF_SUCCESS;
}

RTDECL(int) RTSemEventMultiSignal(RTSEMEVENTMULTI hEventMultiSem)
{
    struct RTSEMEVENTMULTIINTERNAL *pThis = hEventMultiSem;
    AssertPtrReturn(pThis, VERR_INVALID_HANDLE);

    int rc = pthread_mutex_lock(&pThis->Mutex);
    if (rc)
        return RTErrConvertFromErrno(rc);

    if (pThis->u32State == WASM_EVENTMULTI_STATE_NOT_SIGNALED)
    {
        ASMAtomicWriteU32(&pThis->u32State, WASM_EVENTMULTI_STATE_SIGNALED);
        pthread_cond_broadcast(&pThis->Cond); /* Wake ALL waiters for manual-reset. */
    }
    else if (pThis->u32State == WASM_EVENTMULTI_STATE_SIGNALED)
    {
        pthread_cond_broadcast(&pThis->Cond);
    }

    pthread_mutex_unlock(&pThis->Mutex);
    return VINF_SUCCESS;
}

RTDECL(int) RTSemEventMultiReset(RTSEMEVENTMULTI hEventMultiSem)
{
    struct RTSEMEVENTMULTIINTERNAL *pThis = hEventMultiSem;
    AssertPtrReturn(pThis, VERR_INVALID_HANDLE);

    int rc = pthread_mutex_lock(&pThis->Mutex);
    if (rc)
        return RTErrConvertFromErrno(rc);

    if (pThis->u32State == WASM_EVENTMULTI_STATE_SIGNALED)
        ASMAtomicWriteU32(&pThis->u32State, WASM_EVENTMULTI_STATE_NOT_SIGNALED);
    else if (pThis->u32State == WASM_EVENTMULTI_STATE_UNINITIALIZED)
    {
        pthread_mutex_unlock(&pThis->Mutex);
        return VERR_SEM_DESTROYED;
    }

    pthread_mutex_unlock(&pThis->Mutex);
    return VINF_SUCCESS;
}

/**
 * Internal: indefinite wait on manual-reset event.
 */
static int rtSemEventMultiWasmWaitIndefinite(struct RTSEMEVENTMULTIINTERNAL *pThis)
{
    int rc = pthread_mutex_lock(&pThis->Mutex);
    if (rc)
        return RTErrConvertFromErrno(rc);
    ASMAtomicIncU32(&pThis->cWaiters);

    for (;;)
    {
        uint32_t const u32State = pThis->u32State;
        if (u32State != WASM_EVENTMULTI_STATE_NOT_SIGNALED)
        {
            ASMAtomicDecU32(&pThis->cWaiters);
            pthread_mutex_unlock(&pThis->Mutex);
            return u32State == WASM_EVENTMULTI_STATE_SIGNALED ? VINF_SUCCESS : VERR_SEM_DESTROYED;
        }

        rc = pthread_cond_wait(&pThis->Cond, &pThis->Mutex);
        if (rc)
        {
            ASMAtomicDecU32(&pThis->cWaiters);
            pthread_mutex_unlock(&pThis->Mutex);
            return RTErrConvertFromErrno(rc);
        }
    }
}

/**
 * Internal: timed wait on manual-reset event.
 */
static int rtSemEventMultiWasmWaitTimed(struct RTSEMEVENTMULTIINTERNAL *pThis, RTMSINTERVAL cMillies)
{
    struct timespec AbsDeadline;
    clock_gettime(CLOCK_REALTIME, &AbsDeadline);
    uint64_t nsTimeout = (uint64_t)cMillies * UINT64_C(1000000);
    AbsDeadline.tv_sec  += (time_t)(nsTimeout / UINT64_C(1000000000));
    AbsDeadline.tv_nsec += (long)(nsTimeout % UINT64_C(1000000000));
    if (AbsDeadline.tv_nsec >= 1000000000L)
    {
        AbsDeadline.tv_nsec -= 1000000000L;
        AbsDeadline.tv_sec++;
    }

    int rc = pthread_mutex_lock(&pThis->Mutex);
    if (rc)
        return RTErrConvertFromErrno(rc);
    ASMAtomicIncU32(&pThis->cWaiters);

    for (;;)
    {
        uint32_t const u32State = pThis->u32State;
        if (u32State != WASM_EVENTMULTI_STATE_NOT_SIGNALED)
        {
            ASMAtomicDecU32(&pThis->cWaiters);
            pthread_mutex_unlock(&pThis->Mutex);
            return u32State == WASM_EVENTMULTI_STATE_SIGNALED ? VINF_SUCCESS : VERR_SEM_DESTROYED;
        }

        rc = pthread_cond_timedwait(&pThis->Cond, &pThis->Mutex, &AbsDeadline);
        if (rc == ETIMEDOUT)
        {
            /* Last check before reporting timeout. */
            if (pThis->u32State == WASM_EVENTMULTI_STATE_SIGNALED)
            {
                ASMAtomicDecU32(&pThis->cWaiters);
                pthread_mutex_unlock(&pThis->Mutex);
                return VINF_SUCCESS;
            }
            ASMAtomicDecU32(&pThis->cWaiters);
            pthread_mutex_unlock(&pThis->Mutex);
            return VERR_TIMEOUT;
        }
        if (rc && rc != EINTR)
        {
            ASMAtomicDecU32(&pThis->cWaiters);
            pthread_mutex_unlock(&pThis->Mutex);
            return RTErrConvertFromErrno(rc);
        }
    }
}

RTDECL(int) RTSemEventMultiWait(RTSEMEVENTMULTI hEventMultiSem, RTMSINTERVAL cMillies)
{
    struct RTSEMEVENTMULTIINTERNAL *pThis = hEventMultiSem;
    AssertPtrReturn(pThis, VERR_INVALID_HANDLE);

    if (cMillies == RT_INDEFINITE_WAIT)
        return rtSemEventMultiWasmWaitIndefinite(pThis);

    if (cMillies == 0)
    {
        int rc = pthread_mutex_lock(&pThis->Mutex);
        if (rc)
            return RTErrConvertFromErrno(rc);
        uint32_t const u32State = pThis->u32State;
        pthread_mutex_unlock(&pThis->Mutex);
        return u32State == WASM_EVENTMULTI_STATE_SIGNALED ? VINF_SUCCESS
             : u32State != WASM_EVENTMULTI_STATE_UNINITIALIZED ? VERR_TIMEOUT
             : VERR_SEM_DESTROYED;
    }

    return rtSemEventMultiWasmWaitTimed(pThis, cMillies);
}

RTDECL(int) RTSemEventMultiWaitNoResume(RTSEMEVENTMULTI hEventMultiSem, RTMSINTERVAL cMillies)
{
    return RTSemEventMultiWait(hEventMultiSem, cMillies);
}

RTDECL(int) RTSemEventMultiWaitEx(RTSEMEVENTMULTI hEventMultiSem, uint32_t fFlags, uint64_t uTimeout)
{
    struct RTSEMEVENTMULTIINTERNAL *pThis = hEventMultiSem;
    AssertPtrReturn(pThis, VERR_INVALID_HANDLE);

    if (fFlags & RTSEMWAIT_FLAGS_INDEFINITE)
        return rtSemEventMultiWasmWaitIndefinite(pThis);

    RTMSINTERVAL cMillies;
    if (fFlags & RTSEMWAIT_FLAGS_NANOSECS)
        cMillies = (RTMSINTERVAL)((uTimeout + RT_NS_1MS - 1) / RT_NS_1MS);
    else
        cMillies = (RTMSINTERVAL)uTimeout;

    if (fFlags & RTSEMWAIT_FLAGS_ABSOLUTE)
    {
        uint64_t u64Now = RTTimeSystemNanoTS();
        uint64_t u64Abs = (fFlags & RTSEMWAIT_FLAGS_NANOSECS) ? uTimeout : uTimeout * RT_NS_1MS;
        if (u64Abs <= u64Now)
            cMillies = 0;
        else
            cMillies = (RTMSINTERVAL)((u64Abs - u64Now + RT_NS_1MS - 1) / RT_NS_1MS);
    }

    return RTSemEventMultiWait(hEventMultiSem, cMillies);
}

RTDECL(int) RTSemEventMultiWaitExDebug(RTSEMEVENTMULTI hEventMultiSem, uint32_t fFlags, uint64_t uTimeout,
                                        RTHCUINTPTR uId, RT_SRC_POS_DECL)
{
    RT_NOREF(uId, RT_SRC_POS_ARGS);
    return RTSemEventMultiWaitEx(hEventMultiSem, fFlags, uTimeout);
}

RTDECL(uint32_t) RTSemEventMultiGetResolution(void)
{
    return 1000000; /* 1ms */
}

RTDECL(void) RTSemEventMultiSetSignaller(RTSEMEVENTMULTI hEventMultiSem, RTTHREAD hThread)
{
    RT_NOREF(hEventMultiSem, hThread);
}

RTDECL(void) RTSemEventMultiAddSignaller(RTSEMEVENTMULTI hEventMultiSem, RTTHREAD hThread)
{
    RT_NOREF(hEventMultiSem, hThread);
}

RTDECL(void) RTSemEventMultiRemoveSignaller(RTSEMEVENTMULTI hEventMultiSem, RTTHREAD hThread)
{
    RT_NOREF(hEventMultiSem, hThread);
}


/*********************************************************************************************************************************
 * RTSemMutex — Recursive mutex semaphore                                                                                        *
 *********************************************************************************************************************************/

struct RTSEMMUTEXINTERNAL
{
    /** Recursive pthread mutex. */
    pthread_mutex_t     Mutex;
    /** Owner thread (for debugging). */
    volatile pthread_t  Owner;
    /** Recursion count. */
    volatile uint32_t   cRecursions;
};

RTDECL(int) RTSemMutexCreate(PRTSEMMUTEX phMutexSem)
{
    return RTSemMutexCreateEx(phMutexSem, 0, NIL_RTLOCKVALCLASS, 0, NULL);
}

RTDECL(int) RTSemMutexCreateEx(PRTSEMMUTEX phMutexSem, uint32_t fFlags, RTLOCKVALCLASS hClass, uint32_t uSubClass, const char *pszNameFmt, ...)
{
    RT_NOREF(fFlags, hClass, uSubClass, pszNameFmt);

    struct RTSEMMUTEXINTERNAL *pThis = (struct RTSEMMUTEXINTERNAL *)rtMemAllocZAligned(sizeof(*pThis));
    if (!pThis)
        return VERR_NO_MEMORY;

    pthread_mutexattr_t Attr;
    int rc = pthread_mutexattr_init(&Attr);
    if (rc)
    {
        free(pThis);
        return RTErrConvertFromErrno(rc);
    }

    rc = pthread_mutexattr_settype(&Attr, PTHREAD_MUTEX_RECURSIVE);
    if (rc)
    {
        pthread_mutexattr_destroy(&Attr);
        free(pThis);
        return RTErrConvertFromErrno(rc);
    }

    rc = pthread_mutex_init(&pThis->Mutex, &Attr);
    pthread_mutexattr_destroy(&Attr);
    if (rc)
    {
        free(pThis);
        return RTErrConvertFromErrno(rc);
    }

    pThis->Owner = (pthread_t)0;
    pThis->cRecursions = 0;

    *phMutexSem = pThis;
    return VINF_SUCCESS;
}

RTDECL(int) RTSemMutexDestroy(RTSEMMUTEX hMutexSem)
{
    struct RTSEMMUTEXINTERNAL *pThis = hMutexSem;
    if (pThis == NIL_RTSEMMUTEX)
        return VINF_SUCCESS;
    AssertPtrReturn(pThis, VERR_INVALID_HANDLE);

    int rc = pthread_mutex_destroy(&pThis->Mutex);
    free(pThis);
    return rc ? RTErrConvertFromErrno(rc) : VINF_SUCCESS;
}

RTDECL(int) RTSemMutexRequest(RTSEMMUTEX hMutexSem, RTMSINTERVAL cMillies)
{
    struct RTSEMMUTEXINTERNAL *pThis = hMutexSem;
    AssertPtrReturn(pThis, VERR_INVALID_HANDLE);
    RT_NOREF(cMillies);

    int rc = pthread_mutex_lock(&pThis->Mutex);
    if (rc)
        return RTErrConvertFromErrno(rc);

    ASMAtomicIncU32(&pThis->cRecursions);
    pThis->Owner = pthread_self();
    return VINF_SUCCESS;
}

RTDECL(int) RTSemMutexRequestNoResume(RTSEMMUTEX hMutexSem, RTMSINTERVAL cMillies)
{
    return RTSemMutexRequest(hMutexSem, cMillies);
}

RTDECL(int) RTSemMutexRelease(RTSEMMUTEX hMutexSem)
{
    struct RTSEMMUTEXINTERNAL *pThis = hMutexSem;
    AssertPtrReturn(pThis, VERR_INVALID_HANDLE);

    ASMAtomicDecU32(&pThis->cRecursions);
    int rc = pthread_mutex_unlock(&pThis->Mutex);
    return rc ? RTErrConvertFromErrno(rc) : VINF_SUCCESS;
}


/*********************************************************************************************************************************
 * RTSemRW — Read-write semaphore                                                                                                *
 *********************************************************************************************************************************/

struct RTSEMRWINTERNAL
{
    /** pthread read-write lock. */
    pthread_rwlock_t    RWLock;
};

RTDECL(int) RTSemRWCreate(PRTSEMRW phRWSem)
{
    return RTSemRWCreateEx(phRWSem, 0, NIL_RTLOCKVALCLASS, 0, NULL);
}

RTDECL(int) RTSemRWCreateEx(PRTSEMRW phRWSem, uint32_t fFlags, RTLOCKVALCLASS hClass, uint32_t uSubClass, const char *pszNameFmt, ...)
{
    RT_NOREF(fFlags, hClass, uSubClass, pszNameFmt);

    struct RTSEMRWINTERNAL *pThis = (struct RTSEMRWINTERNAL *)rtMemAllocZAligned(sizeof(*pThis));
    if (!pThis)
        return VERR_NO_MEMORY;

    int rc = pthread_rwlock_init(&pThis->RWLock, NULL);
    if (rc)
    {
        free(pThis);
        return RTErrConvertFromErrno(rc);
    }

    *phRWSem = pThis;
    return VINF_SUCCESS;
}

RTDECL(int) RTSemRWDestroy(RTSEMRW hRWSem)
{
    struct RTSEMRWINTERNAL *pThis = hRWSem;
    if (pThis == NIL_RTSEMRW)
        return VINF_SUCCESS;
    AssertPtrReturn(pThis, VERR_INVALID_HANDLE);

    int rc = pthread_rwlock_destroy(&pThis->RWLock);
    free(pThis);
    return rc ? RTErrConvertFromErrno(rc) : VINF_SUCCESS;
}

RTDECL(int) RTSemRWRequestRead(RTSEMRW hRWSem, RTMSINTERVAL cMillies)
{
    struct RTSEMRWINTERNAL *pThis = hRWSem;
    AssertPtrReturn(pThis, VERR_INVALID_HANDLE);
    RT_NOREF(cMillies);

    int rc = pthread_rwlock_rdlock(&pThis->RWLock);
    return rc ? RTErrConvertFromErrno(rc) : VINF_SUCCESS;
}

RTDECL(int) RTSemRWReleaseRead(RTSEMRW hRWSem)
{
    struct RTSEMRWINTERNAL *pThis = hRWSem;
    AssertPtrReturn(pThis, VERR_INVALID_HANDLE);

    int rc = pthread_rwlock_unlock(&pThis->RWLock);
    return rc ? RTErrConvertFromErrno(rc) : VINF_SUCCESS;
}

RTDECL(int) RTSemRWRequestWrite(RTSEMRW hRWSem, RTMSINTERVAL cMillies)
{
    struct RTSEMRWINTERNAL *pThis = hRWSem;
    AssertPtrReturn(pThis, VERR_INVALID_HANDLE);
    RT_NOREF(cMillies);

    int rc = pthread_rwlock_wrlock(&pThis->RWLock);
    return rc ? RTErrConvertFromErrno(rc) : VINF_SUCCESS;
}

RTDECL(int) RTSemRWReleaseWrite(RTSEMRW hRWSem)
{
    struct RTSEMRWINTERNAL *pThis = hRWSem;
    AssertPtrReturn(pThis, VERR_INVALID_HANDLE);

    int rc = pthread_rwlock_unlock(&pThis->RWLock);
    return rc ? RTErrConvertFromErrno(rc) : VINF_SUCCESS;
}

RTDECL(bool) RTSemRWIsWriteOwner(RTSEMRW hRWSem)
{
    /* pthread_rwlock doesn't track ownership; conservatively return true
       if we can try-lock for write (and immediately release). */
    struct RTSEMRWINTERNAL *pThis = hRWSem;
    if (!pThis || pThis == NIL_RTSEMRW)
        return false;
    int rc = pthread_rwlock_trywrlock(&pThis->RWLock);
    if (rc == 0)
    {
        pthread_rwlock_unlock(&pThis->RWLock);
        return false; /* We got it, so we weren't the owner. */
    }
    /* EBUSY or EDEADLK — could be us or someone else.  Return true to be safe
       for VBox callers that use this defensively. */
    return true;
}

RTDECL(bool) RTSemRWIsReadOwner(RTSEMRW hRWSem, bool fWannaHear)
{
    RT_NOREF(hRWSem, fWannaHear);
    return true; /* Conservative — can't query pthread_rwlock ownership. */
}

RTDECL(uint32_t) RTSemRWGetWriteRecursion(RTSEMRW hRWSem)
{
    RT_NOREF(hRWSem);
    return 1; /* Conservative — can't query pthread_rwlock recursion. */
}


/*
 * ── Thread implementation (pthread-based for Emscripten with pthreads) ──
 *
 * Emscripten supports pthreads via Web Workers when compiled with -pthread.
 * This provides real thread creation using pthread_create, unlike the old
 * single-threaded stubs that returned VERR_NOT_SUPPORTED.
 */

/** TLS key for storing the PRTTHREADINT pointer for the current thread. */
static pthread_key_t g_SelfKey;

/**
 * Thread entry point wrapper.  Called by pthread_create.
 * Sets up TLS and calls the common IPRT rtThreadMain().
 */
static void *rtThreadNativeMain(void *pvArgs)
{
    PRTTHREADINT pThread = (PRTTHREADINT)pvArgs;
    pthread_t Self = pthread_self();

    int rc = pthread_setspecific(g_SelfKey, pThread);
    AssertReleaseMsg(!rc, ("failed to set self TLS. rc=%d thread '%s'\n", rc, pThread->szName));

    rc = rtThreadMain(pThread, (uintptr_t)Self, &pThread->szName[0]);

    pthread_setspecific(g_SelfKey, NULL);
    return (void *)(intptr_t)rc;
}

DECLHIDDEN(int) rtThreadNativeInit(void)
{
    int rc = pthread_key_create(&g_SelfKey, NULL);
    if (rc)
        return VERR_NO_TLS_FOR_SELF;
    return VINF_SUCCESS;
}

RTDECL(RTNATIVETHREAD) RTThreadNativeSelf(void)
{
    return (RTNATIVETHREAD)pthread_self();
}

DECLHIDDEN(int) rtThreadNativeSetPriority(PRTTHREADINT pThread, RTTHREADTYPE enmType)
{
    /* Thread priorities not meaningful in Emscripten/Wasm. */
    RT_NOREF(pThread, enmType);
    return VINF_SUCCESS;
}

DECLHIDDEN(int) rtThreadNativeAdopt(PRTTHREADINT pThread)
{
    int rc = pthread_setspecific(g_SelfKey, pThread);
    if (!rc)
        return VINF_SUCCESS;
    return VERR_FAILED_TO_SET_SELF_TLS;
}

DECLHIDDEN(void) rtThreadNativeWaitKludge(PRTTHREADINT pThread)
{
    RT_NOREF_PV(pThread);
}

DECLHIDDEN(void) rtThreadNativeDestroy(PRTTHREADINT pThread)
{
    if (pThread == (PRTTHREADINT)pthread_getspecific(g_SelfKey))
        pthread_setspecific(g_SelfKey, NULL);
}

DECLHIDDEN(int) rtThreadNativeCreate(PRTTHREADINT pThreadInt, PRTNATIVETHREAD pNativeThread)
{
    /* Default stack size if not specified. */
    if (!pThreadInt->cbStack)
        pThreadInt->cbStack = 512 * 1024;

    pthread_attr_t ThreadAttr;
    int rc = pthread_attr_init(&ThreadAttr);
    if (!rc)
    {
        rc = pthread_attr_setdetachstate(&ThreadAttr, PTHREAD_CREATE_DETACHED);
        if (!rc)
        {
            rc = pthread_attr_setstacksize(&ThreadAttr, pThreadInt->cbStack);
            if (!rc)
            {
                pthread_t ThreadId;
                rc = pthread_create(&ThreadId, &ThreadAttr, rtThreadNativeMain, pThreadInt);
                if (!rc)
                {
                    pthread_attr_destroy(&ThreadAttr);
                    *pNativeThread = (uintptr_t)ThreadId;
                    return VINF_SUCCESS;
                }
            }
        }
        pthread_attr_destroy(&ThreadAttr);
    }
    return RTErrConvertFromErrno(rc);
}


/*
 * ── TLS stubs ──
 */
RTDECL(int) RTTlsAllocEx(PRTTLS piTls, PFNRTTLSDTOR pfnDestructor)
{
    RT_NOREF_PV(pfnDestructor);
    static int32_t s_iTls = 0;
    *piTls = s_iTls++;
    return VINF_SUCCESS;
}


/*
 * ── Native loader stub ──
 */
DECLHIDDEN(int) rtldrNativeLoad(const char *pszFilename, uintptr_t *phHandle, uint32_t fFlags, PRTERRINFO pErrInfo)
{
    RT_NOREF(pszFilename, fFlags, pErrInfo);
    *phHandle = 0;
    return VERR_NOT_SUPPORTED;
}

DECLHIDDEN(int) rtldrNativeGetSymbol(uintptr_t hHandle, const char *pszSymbol, void **ppvValue)
{
    RT_NOREF(hHandle, pszSymbol);
    *ppvValue = NULL;
    return VERR_SYMBOL_NOT_FOUND;
}

DECLHIDDEN(int) rtldrNativeClose(uintptr_t hHandle)
{
    RT_NOREF_PV(hHandle);
    return VINF_SUCCESS;
}

DECLHIDDEN(int) rtldrNativeLoadSystem(const char *pszFilename, const char *pszExt, uint32_t fFlags, uintptr_t *phHandle)
{
    RT_NOREF(pszFilename, pszExt, fFlags);
    *phHandle = 0;
    return VERR_NOT_SUPPORTED;
}


/*
 * ── Thread identity ──
 */
RTDECL(RTTHREAD) RTThreadSelf(void)
{
    PRTTHREADINT pThread = (PRTTHREADINT)pthread_getspecific(g_SelfKey);
    return pThread;
}

RTDECL(int) RTThreadSleep(RTMSINTERVAL cMillies)
{
    if (!cMillies)
    {
        sched_yield();
        return VINF_SUCCESS;
    }
    struct timespec ts;
    ts.tv_sec  = cMillies / 1000;
    ts.tv_nsec = (cMillies % 1000) * 1000000;
    nanosleep(&ts, NULL);
    return VINF_SUCCESS;
}

RTDECL(int) RTThreadSleepNoLog(RTMSINTERVAL cMillies)
{
    return RTThreadSleep(cMillies);
}

DECLHIDDEN(void) rtThreadNativeReInitObtrusive(void)
{
}


/*
 * ── Iconv cache stubs ──
 *
 * Normally in r3/posix/utf8-posix.cpp or r3/win/utf8-win.cpp.
 */
DECLHIDDEN(void) rtStrIconvCacheInit(PRTTHREADINT pThread)
{
    RT_NOREF_PV(pThread);
}

DECLHIDDEN(void) rtStrIconvCacheDestroy(PRTTHREADINT pThread)
{
    RT_NOREF_PV(pThread);
}


/*
 * ── Environment stubs ──
 */
RTDECL(const char *) RTEnvGet(const char *pszVar)
{
    RT_NOREF_PV(pszVar);
    return NULL;
}


/*
 * ── Logging stubs ──
 */
RTDECL(void) RTLogWriteDebugger(const char *pch, size_t cb)
{
    RT_NOREF(pch, cb);
}


/*
 * ── Time stubs ──
 */
RTDECL(PRTTIMESPEC) RTTimeNow(PRTTIMESPEC pTime)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    RTTimeSpecSetNano(pTime, (int64_t)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec);
    return pTime;
}


/*
 * ── File stubs ──
 */
RTDECL(int) RTFileDelete(const char *pszFilename)
{
    RT_NOREF_PV(pszFilename);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTFileRename(const char *pszSrc, const char *pszDst, unsigned fRename)
{
    RT_NOREF(pszSrc, pszDst, fRename);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTFileOpen(PRTFILE phFile, const char *pszFilename, uint64_t fOpen)
{
    RT_NOREF(pszFilename, fOpen);
    *phFile = NIL_RTFILE;
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTFileClose(RTFILE hFile)
{
    RT_NOREF_PV(hFile);
    return VINF_SUCCESS;
}

RTDECL(int) RTFileQuerySize(RTFILE hFile, uint64_t *pcbSize)
{
    RT_NOREF_PV(hFile);
    *pcbSize = 0;
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTFileWrite(RTFILE hFile, const void *pvBuf, size_t cbToWrite, size_t *pcbWritten)
{
    RT_NOREF(hFile, pvBuf, cbToWrite);
    if (pcbWritten)
        *pcbWritten = cbToWrite;
    return VINF_SUCCESS;
}

RTDECL(int) RTFileFlush(RTFILE hFile)
{
    RT_NOREF_PV(hFile);
    return VINF_SUCCESS;
}


/*
 * ── Thread yield / misc ──
 *
 * sched_yield() works in Emscripten with -pthread (Web Workers).
 * Without a real yield the EMT thread busy-loops and starves the
 * async I/O worker threads (e.g. ATA).
 */
RTDECL(bool) RTThreadYield(void)
{
    sched_yield();
    return true;
}


/*
 * ── Codepage conversion stubs ──
 *
 * Wasm runs in UTF-8 only. No conversion needed.
 */
RTDECL(int) RTStrCurrentCPToUtf8Tag(char **ppszString, const char *pszString, const char *pszTag)
{
    RT_NOREF_PV(pszTag);
    size_t cb = strlen(pszString) + 1;
    *ppszString = (char *)RTMemAlloc(cb);
    if (!*ppszString)
        return VERR_NO_MEMORY;
    memcpy(*ppszString, pszString, cb);
    return VINF_SUCCESS;
}

/*
 * ── Process stubs ──
 */
RTR3DECL(int) RTProcQueryParent(RTPROCESS hProcess, PRTPROCESS phParent)
{
    RT_NOREF_PV(hProcess);
    if (phParent)
        *phParent = NIL_RTPROCESS;
    return VERR_NOT_SUPPORTED;
}


RTDECL(int) RTStrUtf8ToCurrentCPTag(char **ppszString, const char *pszString, const char *pszTag)
{
    RT_NOREF_PV(pszTag);
    size_t cb = strlen(pszString) + 1;
    *ppszString = (char *)RTMemAlloc(cb);
    if (!*ppszString)
        return VERR_NO_MEMORY;
    memcpy(*ppszString, pszString, cb);
    return VINF_SUCCESS;
}
