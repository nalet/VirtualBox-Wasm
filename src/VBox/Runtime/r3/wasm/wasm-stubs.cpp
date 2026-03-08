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
#include <time.h>
#include <errno.h>


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
 * ── Semaphore stubs ──
 *
 * Wasm is single-threaded. Semaphores are no-ops.
 */
RTDECL(int) RTSemEventCreate(PRTSEMEVENT phEventSem)
{
    *phEventSem = (RTSEMEVENT)(uintptr_t)1;
    return VINF_SUCCESS;
}

RTDECL(int) RTSemEventCreateEx(PRTSEMEVENT phEventSem, uint32_t fFlags, RTLOCKVALCLASS hClass, const char *pszNameFmt, ...)
{
    RT_NOREF(fFlags, hClass, pszNameFmt);
    *phEventSem = (RTSEMEVENT)(uintptr_t)1;
    return VINF_SUCCESS;
}

RTDECL(int) RTSemEventDestroy(RTSEMEVENT hEventSem)
{
    RT_NOREF_PV(hEventSem);
    return VINF_SUCCESS;
}

RTDECL(int) RTSemEventSignal(RTSEMEVENT hEventSem)
{
    RT_NOREF_PV(hEventSem);
    return VINF_SUCCESS;
}

RTDECL(int) RTSemEventWait(RTSEMEVENT hEventSem, RTMSINTERVAL cMillies)
{
    RT_NOREF(hEventSem, cMillies);
    return VINF_SUCCESS;
}

RTDECL(int) RTSemEventWaitNoResume(RTSEMEVENT hEventSem, RTMSINTERVAL cMillies)
{
    RT_NOREF(hEventSem, cMillies);
    return VINF_SUCCESS;
}

RTDECL(int) RTSemEventMultiCreate(PRTSEMEVENTMULTI phEventMultiSem)
{
    *phEventMultiSem = (RTSEMEVENTMULTI)(uintptr_t)1;
    return VINF_SUCCESS;
}

RTDECL(int) RTSemEventMultiCreateEx(PRTSEMEVENTMULTI phEventMultiSem, uint32_t fFlags, RTLOCKVALCLASS hClass, const char *pszNameFmt, ...)
{
    RT_NOREF(fFlags, hClass, pszNameFmt);
    *phEventMultiSem = (RTSEMEVENTMULTI)(uintptr_t)1;
    return VINF_SUCCESS;
}

RTDECL(int) RTSemEventMultiDestroy(RTSEMEVENTMULTI hEventMultiSem)
{
    RT_NOREF_PV(hEventMultiSem);
    return VINF_SUCCESS;
}

RTDECL(int) RTSemEventMultiSignal(RTSEMEVENTMULTI hEventMultiSem)
{
    RT_NOREF_PV(hEventMultiSem);
    return VINF_SUCCESS;
}

RTDECL(int) RTSemEventMultiReset(RTSEMEVENTMULTI hEventMultiSem)
{
    RT_NOREF_PV(hEventMultiSem);
    return VINF_SUCCESS;
}

RTDECL(int) RTSemEventMultiWait(RTSEMEVENTMULTI hEventMultiSem, RTMSINTERVAL cMillies)
{
    RT_NOREF(hEventMultiSem, cMillies);
    return VINF_SUCCESS;
}

RTDECL(int) RTSemEventMultiWaitNoResume(RTSEMEVENTMULTI hEventMultiSem, RTMSINTERVAL cMillies)
{
    RT_NOREF(hEventMultiSem, cMillies);
    return VINF_SUCCESS;
}

RTDECL(int) RTSemMutexCreate(PRTSEMMUTEX phMutexSem)
{
    *phMutexSem = (RTSEMMUTEX)(uintptr_t)1;
    return VINF_SUCCESS;
}

RTDECL(int) RTSemMutexCreateEx(PRTSEMMUTEX phMutexSem, uint32_t fFlags, RTLOCKVALCLASS hClass, uint32_t uSubClass, const char *pszNameFmt, ...)
{
    RT_NOREF(fFlags, hClass, uSubClass, pszNameFmt);
    *phMutexSem = (RTSEMMUTEX)(uintptr_t)1;
    return VINF_SUCCESS;
}

RTDECL(int) RTSemMutexDestroy(RTSEMMUTEX hMutexSem)
{
    RT_NOREF_PV(hMutexSem);
    return VINF_SUCCESS;
}

RTDECL(int) RTSemMutexRequest(RTSEMMUTEX hMutexSem, RTMSINTERVAL cMillies)
{
    RT_NOREF(hMutexSem, cMillies);
    return VINF_SUCCESS;
}

RTDECL(int) RTSemMutexRequestNoResume(RTSEMMUTEX hMutexSem, RTMSINTERVAL cMillies)
{
    RT_NOREF(hMutexSem, cMillies);
    return VINF_SUCCESS;
}

RTDECL(int) RTSemMutexRelease(RTSEMMUTEX hMutexSem)
{
    RT_NOREF_PV(hMutexSem);
    return VINF_SUCCESS;
}

RTDECL(int) RTSemRWCreate(PRTSEMRW phRWSem)
{
    *phRWSem = (RTSEMRW)(uintptr_t)1;
    return VINF_SUCCESS;
}

RTDECL(int) RTSemRWCreateEx(PRTSEMRW phRWSem, uint32_t fFlags, RTLOCKVALCLASS hClass, uint32_t uSubClass, const char *pszNameFmt, ...)
{
    RT_NOREF(fFlags, hClass, uSubClass, pszNameFmt);
    *phRWSem = (RTSEMRW)(uintptr_t)1;
    return VINF_SUCCESS;
}

RTDECL(int) RTSemRWDestroy(RTSEMRW hRWSem)
{
    RT_NOREF_PV(hRWSem);
    return VINF_SUCCESS;
}

RTDECL(int) RTSemRWRequestRead(RTSEMRW hRWSem, RTMSINTERVAL cMillies)
{
    RT_NOREF(hRWSem, cMillies);
    return VINF_SUCCESS;
}

RTDECL(int) RTSemRWReleaseRead(RTSEMRW hRWSem)
{
    RT_NOREF_PV(hRWSem);
    return VINF_SUCCESS;
}

RTDECL(int) RTSemRWRequestWrite(RTSEMRW hRWSem, RTMSINTERVAL cMillies)
{
    RT_NOREF(hRWSem, cMillies);
    return VINF_SUCCESS;
}

RTDECL(int) RTSemRWReleaseWrite(RTSEMRW hRWSem)
{
    RT_NOREF_PV(hRWSem);
    return VINF_SUCCESS;
}


/*
 * ── Thread stubs ──
 *
 * Wasm is single-threaded. Provide minimal thread identity.
 */
DECLHIDDEN(int) rtThreadNativeInit(void)
{
    return VINF_SUCCESS;
}

RTDECL(RTNATIVETHREAD) RTThreadNativeSelf(void)
{
    return (RTNATIVETHREAD)1;
}

DECLHIDDEN(int) rtThreadNativeSetPriority(PRTTHREADINT pThread, RTTHREADTYPE enmType)
{
    RT_NOREF(pThread, enmType);
    return VINF_SUCCESS;
}

DECLHIDDEN(int) rtThreadNativeAdopt(PRTTHREADINT pThread)
{
    RT_NOREF_PV(pThread);
    return VINF_SUCCESS;
}

DECLHIDDEN(void) rtThreadNativeWaitKludge(PRTTHREADINT pThread)
{
    RT_NOREF_PV(pThread);
}

DECLHIDDEN(void) rtThreadNativeDestroy(PRTTHREADINT pThread)
{
    RT_NOREF_PV(pThread);
}

DECLHIDDEN(int) rtThreadNativeCreate(PRTTHREADINT pThreadInt, PRTNATIVETHREAD pNativeThread)
{
    RT_NOREF(pThreadInt, pNativeThread);
    return VERR_NOT_SUPPORTED;
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
 *
 * RTThreadSelf is complex (returns RTTHREAD handle, not native).
 * For single-threaded Wasm we return NIL since no threads are created.
 */
RTDECL(RTTHREAD) RTThreadSelf(void)
{
    return NIL_RTTHREAD;
}

RTDECL(int) RTThreadSleep(RTMSINTERVAL cMillies)
{
    RT_NOREF_PV(cMillies);
    return VINF_SUCCESS;
}

RTDECL(int) RTThreadSleepNoLog(RTMSINTERVAL cMillies)
{
    RT_NOREF_PV(cMillies);
    return VINF_SUCCESS;
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
 */
RTDECL(bool) RTThreadYield(void)
{
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
