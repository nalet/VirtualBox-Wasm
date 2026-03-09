/*
 * VirtualBox/Wasm — Stub implementations for symbols missing in Wasm builds.
 *
 * These stubs provide minimal implementations of platform-specific functions
 * that don't have Emscripten equivalents but are referenced by VBoxVMM/VBoxDD.
 *
 * Categories:
 *   - SUPR3: Support library (kernel driver interface) — driverless stubs
 *   - RTMp:  Multiprocessor queries — single-CPU stubs
 *   - RTFile: File existence check
 *   - ASM:   Architecture-specific intrinsics
 */

#include <iprt/types.h>
#include <iprt/mem.h>
#include <iprt/err.h>
#include <iprt/assert.h>
#include <iprt/string.h>
#include <iprt/uuid.h>
#include <iprt/file.h>
#include <iprt/mp.h>
#include <iprt/cpuset.h>
#include <iprt/process.h>
#include <iprt/path.h>

#include <VBox/sup.h>
#include <VBox/err.h>

#include <sys/stat.h>
#include <string.h>


/*************************************************************************
 * SUPR3 — Support Library Stubs (driverless mode)
 *************************************************************************/

extern "C" {

SUPR3DECL(int) SUPR3Init(PSUPDRVSESSION *ppSession)
{
    if (ppSession)
        *ppSession = NIL_RTR0PTR;
    return VINF_SUCCESS;
}

SUPR3DECL(int) SUPR3InitEx(uint32_t fFlags, PSUPDRVSESSION *ppSession)
{
    RT_NOREF(fFlags);
    if (ppSession)
        *ppSession = NIL_RTR0PTR;
    return VINF_SUCCESS;
}

SUPR3DECL(bool) SUPR3IsDriverless(void)
{
    return true; /* Always driverless in Wasm */
}

SUPR3DECL(int) SUPR3PageAllocEx(size_t cPages, uint32_t fFlags, void **ppvPages,
                                 PRTR0PTR pR0Ptr, PSUPPAGE paPages)
{
    RT_NOREF(fFlags);
    size_t cb = cPages << 12; /* PAGE_SIZE = 4096 */
    void *pv = RTMemPageAllocZ(cb);
    if (!pv)
        return VERR_NO_MEMORY;
    *ppvPages = pv;
    if (pR0Ptr)
        *pR0Ptr = NIL_RTR0PTR;
    if (paPages)
    {
        for (size_t i = 0; i < cPages; i++)
        {
            paPages[i].Phys = NIL_RTHCPHYS;
            paPages[i].uReserved = 0;
        }
    }
    return VINF_SUCCESS;
}

SUPR3DECL(int) SUPR3PageFreeEx(void *pvPages, size_t cPages)
{
    RT_NOREF(cPages);
    if (pvPages)
        RTMemPageFree(pvPages, cPages << 12);
    return VINF_SUCCESS;
}

SUPR3DECL(void *) SUPR3ContAlloc(size_t cPages, PRTR0PTR pR0Ptr, PRTHCPHYS pHCPhys)
{
    size_t cb = cPages << 12;
    void *pv = RTMemPageAllocZ(cb);
    if (pR0Ptr)
        *pR0Ptr = NIL_RTR0PTR;
    if (pHCPhys)
        *pHCPhys = NIL_RTHCPHYS;
    return pv;
}

SUPR3DECL(int) SUPR3ContFree(void *pvPages, size_t cPages)
{
    if (pvPages)
        RTMemPageFree(pvPages, cPages << 12);
    return VINF_SUCCESS;
}

SUPR3DECL(int) SUPR3PageAlloc(size_t cPages, uint32_t fFlags, void **ppvPages)
{
    RT_NOREF(fFlags);
    size_t cb = cPages << 12;
    void *pv = RTMemPageAllocZ(cb);
    if (!pv)
        return VERR_NO_MEMORY;
    *ppvPages = pv;
    return VINF_SUCCESS;
}

SUPR3DECL(int) SUPR3PageFree(void *pvPages, size_t cPages)
{
    if (pvPages)
        RTMemPageFree(pvPages, cPages << 12);
    return VINF_SUCCESS;
}

SUPR3DECL(int) SUPR3PageProtect(void *pvStart, RTR0PTR R0Ptrstart, uint32_t offSub, uint32_t cbSub, uint32_t fProt)
{
    RT_NOREF(pvStart, R0Ptrstart, offSub, cbSub, fProt);
    return VINF_SUCCESS; /* No-op in Wasm */
}

SUPR3DECL(int) SUPR3HardenedLdrLoadPlugIn(const char *pszFilename, PRTLDRMOD phLdrMod, PRTERRINFO pErrInfo)
{
    RT_NOREF(pszFilename, phLdrMod, pErrInfo);
    return VERR_NOT_SUPPORTED; /* Dynamic loading not supported in Wasm */
}

SUPR3DECL(int) SUPR3Term(bool fForced)
{
    RT_NOREF(fForced);
    return VINF_SUCCESS;
}

SUPR3DECL(SUPPAGINGMODE) SUPR3GetPagingMode(void)
{
    return SUPPAGINGMODE_AMD64_GLOBAL_NX;
}

SUPR3DECL(int) SUPR3CallVMMR0(PVMR0 pVMR0, VMCPUID idCpu, unsigned uOperation,
                                void *pvArg)
{
    RT_NOREF(pVMR0, idCpu, uOperation, pvArg);
    return VERR_NOT_SUPPORTED;
}

SUPR3DECL(int) SUPR3CallVMMR0Ex(PVMR0 pVMR0, VMCPUID idCpu, unsigned uOperation,
                                 uint64_t u64Arg, PSUPVMMR0REQHDR pReqHdr)
{
    RT_NOREF(pVMR0, idCpu, uOperation, u64Arg, pReqHdr);
    return VERR_NOT_SUPPORTED;
}

SUPR3DECL(int) SUPR3SetVMForFastIOCtl(PVMR0 pVMR0)
{
    RT_NOREF(pVMR0);
    return VINF_SUCCESS;
}

SUPR3DECL(int) SUPR3GipMap(PSUPGLOBALINFOPAGE *ppGip)
{
    RT_NOREF(ppGip);
    return VERR_NOT_SUPPORTED;
}

SUPR3DECL(int) SUPR3GipUnmap(PSUPGLOBALINFOPAGE pGip)
{
    RT_NOREF(pGip);
    return VINF_SUCCESS;
}

SUPR3DECL(PSUPGLOBALINFOPAGE) SUPR3GetGIP(void)
{
    return NULL;
}

SUPR3DECL(int) SUPR3LockMem(void *pv, uint32_t cPages, PSUPPAGE paPages)
{
    RT_NOREF(pv, cPages, paPages);
    return VINF_SUCCESS;
}

SUPR3DECL(int) SUPR3UnlockMem(void *pv)
{
    RT_NOREF(pv);
    return VINF_SUCCESS;
}

SUPR3DECL(int) SUPR3LowAlloc(size_t cPages, void **ppvPages, PRTR0PTR ppvPagesR0, PSUPPAGE paPages)
{
    return SUPR3PageAllocEx(cPages, 0, ppvPages, ppvPagesR0, paPages);
}

SUPR3DECL(int) SUPR3LowFree(void *pvPages, size_t cPages)
{
    return SUPR3PageFreeEx(pvPages, cPages);
}

} /* extern "C" */


/*************************************************************************
 * RTMp — Multiprocessor stubs (single CPU for Wasm)
 *************************************************************************/

extern "C" {

RTDECL(RTCPUID) RTMpGetCount(void)
{
    return 1;
}

RTDECL(RTCPUID) RTMpGetOnlineCount(void)
{
    return 1;
}

RTDECL(RTCPUID) RTMpGetPresentCount(void)
{
    return 1;
}

RTDECL(RTCPUID) RTMpGetCoreCount(void)
{
    return 1;
}

RTDECL(PRTCPUSET) RTMpGetOnlineSet(PRTCPUSET pSet)
{
    RTCpuSetEmpty(pSet);
    RTCpuSetAdd(pSet, 0);
    return pSet;
}

RTDECL(PRTCPUSET) RTMpGetSet(PRTCPUSET pSet)
{
    RTCpuSetEmpty(pSet);
    RTCpuSetAdd(pSet, 0);
    return pSet;
}

RTDECL(PRTCPUSET) RTMpGetPresentSet(PRTCPUSET pSet)
{
    RTCpuSetEmpty(pSet);
    RTCpuSetAdd(pSet, 0);
    return pSet;
}

RTDECL(RTCPUID) RTMpCpuId(void)
{
    return 0;
}

RTDECL(int) RTMpGetDescription(RTCPUID idCpu, char *pszBuf, size_t cbBuf)
{
    RT_NOREF(idCpu);
    return RTStrCopy(pszBuf, cbBuf, "WebAssembly vCPU");
}

RTDECL(bool) RTMpIsCpuOnline(RTCPUID idCpu)
{
    return idCpu == 0;
}

RTDECL(bool) RTMpIsCpuPresent(RTCPUID idCpu)
{
    return idCpu == 0;
}

RTDECL(int) RTMpCpuIdToSetIndex(RTCPUID idCpu)
{
    return idCpu == 0 ? 0 : -1;
}

RTDECL(RTCPUID) RTMpCpuIdFromSetIndex(int iCpu)
{
    return iCpu == 0 ? 0 : NIL_RTCPUID;
}

RTDECL(RTCPUID) RTMpGetMaxCpuId(void)
{
    return 0;
}

} /* extern "C" */


/*************************************************************************
 * RTFile — File operations stubs
 *************************************************************************/

extern "C" {

RTDECL(bool) RTFileExists(const char *pszPath)
{
    struct stat st;
    return stat(pszPath, &st) == 0 && S_ISREG(st.st_mode);
}

RTDECL(int) RTFileQuerySizeByPath(const char *pszPath, uint64_t *pcbFile)
{
    struct stat st;
    if (stat(pszPath, &st) != 0)
        return VERR_FILE_NOT_FOUND;
    if (pcbFile)
        *pcbFile = st.st_size;
    return VINF_SUCCESS;
}

} /* extern "C" */


/*************************************************************************
 * RTUuid
 *************************************************************************/

extern "C" {

RTDECL(int) RTUuidClear(PRTUUID pUuid)
{
    AssertReturn(pUuid, VERR_INVALID_PARAMETER);
    memset(pUuid, 0, sizeof(*pUuid));
    return VINF_SUCCESS;
}

RTDECL(bool) RTUuidIsNull(PCRTUUID pUuid)
{
    AssertReturn(pUuid, true);
    static const RTUUID s_Null = { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } };
    return memcmp(pUuid, &s_Null, sizeof(*pUuid)) == 0;
}

RTDECL(int) RTUuidCompare(PCRTUUID pUuid1, PCRTUUID pUuid2)
{
    return memcmp(pUuid1, pUuid2, sizeof(RTUUID));
}

} /* extern "C" */


/*************************************************************************
 * ASM — Architecture-specific intrinsics
 *
 * These must match the RT_NOTHROW_PROTO declarations in iprt/asm.h.
 *************************************************************************/

#include <iprt/asm.h>

extern "C" {

unsigned ASMCountLeadingZerosU64(uint64_t u64) RT_NOTHROW_DEF
{
    if (u64 == 0)
        return 64;
    return __builtin_clzll(u64);
}

unsigned ASMCountLeadingZerosU32(uint32_t u32) RT_NOTHROW_DEF
{
    if (u32 == 0)
        return 32;
    return __builtin_clz(u32);
}

unsigned ASMCountTrailingZerosU64(uint64_t u64) RT_NOTHROW_DEF
{
    if (u64 == 0)
        return 64;
    return __builtin_ctzll(u64);
}

unsigned ASMCountTrailingZerosU32(uint32_t u32) RT_NOTHROW_DEF
{
    if (u32 == 0)
        return 32;
    return __builtin_ctz(u32);
}

unsigned ASMBitFirstSetU64(uint64_t u64) RT_NOTHROW_DEF
{
    if (u64 == 0)
        return 0;
    return __builtin_ctzll(u64) + 1;
}

unsigned ASMBitFirstSetU32(uint32_t u32) RT_NOTHROW_DEF
{
    if (u32 == 0)
        return 0;
    return __builtin_ctz(u32) + 1;
}

unsigned ASMBitLastSetU64(uint64_t u64) RT_NOTHROW_DEF
{
    if (u64 == 0)
        return 0;
    return 64 - __builtin_clzll(u64);
}

unsigned ASMBitLastSetU32(uint32_t u32) RT_NOTHROW_DEF
{
    if (u32 == 0)
        return 0;
    return 32 - __builtin_clz(u32);
}

} /* extern "C" */


/*************************************************************************
 * Process stubs
 *************************************************************************/

extern "C" {

RTDECL(RTPROCESS) RTProcSelf(void)
{
    return 1; /* Fake PID */
}

} /* extern "C" */


/*************************************************************************
 * Global data stubs
 *************************************************************************/

#include <VBox/sup.h>

extern "C" {

/** IPRT global zero-filled buffers, referenced by VBoxVMM. */
extern const uint8_t g_abRTZero4K[_4K];
extern const uint8_t g_abRTZero16K[_16K];
extern const uint8_t g_abRTZero32K[_32K];
extern const uint8_t g_abRTZero64K[_64K];
extern const char    g_szRTZero64K[_64K];

alignas(64) const uint8_t g_abRTZero4K[_4K]   = {0};
alignas(64) const uint8_t g_abRTZero16K[_16K] = {0};
alignas(64) const uint8_t g_abRTZero32K[_32K] = {0};
alignas(64) const uint8_t g_abRTZero64K[_64K] = {0};
const char                g_szRTZero64K[_64K]  = {0};

/** GIP (Global Info Page) — not available in driverless mode. */
extern PSUPGLOBALINFOPAGE g_pSUPGlobalInfoPage;
PSUPGLOBALINFOPAGE g_pSUPGlobalInfoPage = NULL;

} /* extern "C" */


/*************************************************************************
 * SUPSem — Support library semaphore stubs
 *
 * VBoxVMM's scheduler uses these for event signaling between threads.
 * In driverless mode we delegate to IPRT semaphores.
 *************************************************************************/

#include <iprt/semaphore.h>
#include <iprt/timer.h>
#include <iprt/rand.h>

extern "C" {

SUPR3DECL(int) SUPSemEventCreate(PSUPDRVSESSION pSession, PSUPSEMEVENT phEvent)
{
    RT_NOREF(pSession);
    RTSEMEVENT hEvent;
    int rc = RTSemEventCreate(&hEvent);
    if (RT_SUCCESS(rc))
        *phEvent = (SUPSEMEVENT)(uintptr_t)hEvent;
    return rc;
}

SUPR3DECL(int) SUPSemEventClose(PSUPDRVSESSION pSession, SUPSEMEVENT hEvent)
{
    RT_NOREF(pSession);
    if (hEvent == NIL_SUPSEMEVENT)
        return VINF_SUCCESS;
    return RTSemEventDestroy((RTSEMEVENT)(uintptr_t)hEvent);
}

SUPR3DECL(int) SUPSemEventSignal(PSUPDRVSESSION pSession, SUPSEMEVENT hEvent)
{
    RT_NOREF(pSession);
    return RTSemEventSignal((RTSEMEVENT)(uintptr_t)hEvent);
}

SUPR3DECL(int) SUPSemEventWaitNoResume(PSUPDRVSESSION pSession, SUPSEMEVENT hEvent, uint32_t cMillies)
{
    RT_NOREF(pSession);
    return RTSemEventWaitNoResume((RTSEMEVENT)(uintptr_t)hEvent, cMillies);
}

SUPR3DECL(int) SUPSemEventWaitNsAbsIntr(PSUPDRVSESSION pSession, SUPSEMEVENT hEvent, uint64_t uNsTimeout)
{
    RT_NOREF(pSession);
    return RTSemEventWaitNoResume((RTSEMEVENT)(uintptr_t)hEvent,
                                  uNsTimeout > 0 ? RT_MAX(uNsTimeout / RT_NS_1MS, 1) : 0);
}

SUPR3DECL(int) SUPSemEventWaitNsRelIntr(PSUPDRVSESSION pSession, SUPSEMEVENT hEvent, uint64_t cNsTimeout)
{
    RT_NOREF(pSession);
    return RTSemEventWaitNoResume((RTSEMEVENT)(uintptr_t)hEvent,
                                  cNsTimeout > 0 ? RT_MAX(cNsTimeout / RT_NS_1MS, 1) : 0);
}

SUPR3DECL(uint32_t) SUPSemEventGetResolution(PSUPDRVSESSION pSession)
{
    RT_NOREF(pSession);
    return 1000000; /* 1ms */
}

/* Multi-event semaphores */
SUPR3DECL(int) SUPSemEventMultiCreate(PSUPDRVSESSION pSession, PSUPSEMEVENTMULTI phEventMulti)
{
    RT_NOREF(pSession);
    RTSEMEVENTMULTI hEvent;
    int rc = RTSemEventMultiCreate(&hEvent);
    if (RT_SUCCESS(rc))
        *phEventMulti = (SUPSEMEVENTMULTI)(uintptr_t)hEvent;
    return rc;
}

SUPR3DECL(int) SUPSemEventMultiClose(PSUPDRVSESSION pSession, SUPSEMEVENTMULTI hEventMulti)
{
    RT_NOREF(pSession);
    if (hEventMulti == NIL_SUPSEMEVENTMULTI)
        return VINF_SUCCESS;
    return RTSemEventMultiDestroy((RTSEMEVENTMULTI)(uintptr_t)hEventMulti);
}

SUPR3DECL(int) SUPSemEventMultiSignal(PSUPDRVSESSION pSession, SUPSEMEVENTMULTI hEventMulti)
{
    RT_NOREF(pSession);
    return RTSemEventMultiSignal((RTSEMEVENTMULTI)(uintptr_t)hEventMulti);
}

SUPR3DECL(int) SUPSemEventMultiReset(PSUPDRVSESSION pSession, SUPSEMEVENTMULTI hEventMulti)
{
    RT_NOREF(pSession);
    return RTSemEventMultiReset((RTSEMEVENTMULTI)(uintptr_t)hEventMulti);
}

SUPR3DECL(int) SUPSemEventMultiWaitNoResume(PSUPDRVSESSION pSession, SUPSEMEVENTMULTI hEventMulti, uint32_t cMillies)
{
    RT_NOREF(pSession);
    return RTSemEventMultiWaitNoResume((RTSEMEVENTMULTI)(uintptr_t)hEventMulti, cMillies);
}

SUPR3DECL(int) SUPSemEventMultiWaitNsAbsIntr(PSUPDRVSESSION pSession, SUPSEMEVENTMULTI hEventMulti, uint64_t uNsTimeout)
{
    RT_NOREF(pSession);
    return RTSemEventMultiWaitNoResume((RTSEMEVENTMULTI)(uintptr_t)hEventMulti,
                                       uNsTimeout > 0 ? RT_MAX(uNsTimeout / RT_NS_1MS, 1) : 0);
}

SUPR3DECL(int) SUPSemEventMultiWaitNsRelIntr(PSUPDRVSESSION pSession, SUPSEMEVENTMULTI hEventMulti, uint64_t cNsTimeout)
{
    RT_NOREF(pSession);
    return RTSemEventMultiWaitNoResume((RTSEMEVENTMULTI)(uintptr_t)hEventMulti,
                                       cNsTimeout > 0 ? RT_MAX(cNsTimeout / RT_NS_1MS, 1) : 0);
}

SUPR3DECL(uint32_t) SUPSemEventMultiGetResolution(PSUPDRVSESSION pSession)
{
    RT_NOREF(pSession);
    return 1000000; /* 1ms */
}


/*************************************************************************
 * Additional SUPR3 stubs
 *************************************************************************/

SUPR3DECL(uint64_t) SUPGetCpuHzFromGipForAsyncMode(PSUPGLOBALINFOPAGE pGip)
{
    RT_NOREF(pGip);
    return UINT64_C(3000000000); /* Fake 3 GHz */
}

SUPR3DECL(int) SUPR3QueryVTCaps(uint32_t *pfCaps)
{
    if (pfCaps)
        *pfCaps = 0;
    return VERR_SUP_DRIVERLESS;
}

SUPR3DECL(int) SUPR3QueryVTxSupported(const char **ppszWhy)
{
    if (ppszWhy)
        *ppszWhy = "WebAssembly does not support VT-x/AMD-V";
    return VERR_VMX_NO_VMX;
}

SUPR3DECL(int) SUPR3QueryMicrocodeRev(uint32_t *puMicrocodeRev)
{
    if (puMicrocodeRev)
        *puMicrocodeRev = 0;
    return VINF_SUCCESS;
}

SUPR3DECL(int) SUPR3HardenedVerifyPlugIn(const char *pszFilename, PRTERRINFO pErrInfo)
{
    RT_NOREF(pszFilename, pErrInfo);
    return VINF_SUCCESS;
}

SUPR3DECL(int) SUPR3LoadModule(const char *pszFilename, const char *pszModule, void **ppvImageBase, PRTERRINFO pErrInfo)
{
    RT_NOREF(pszFilename, pszModule, ppvImageBase, pErrInfo);
    return VERR_NOT_SUPPORTED;
}

SUPR3DECL(int) SUPR3FreeModule(void *pvImageBase)
{
    RT_NOREF(pvImageBase);
    return VINF_SUCCESS;
}

SUPR3DECL(int) SUPR3GetSymbolR0(void *pvImageBase, const char *pszSymbol, void **ppvValue)
{
    RT_NOREF(pvImageBase, pszSymbol, ppvValue);
    return VERR_NOT_SUPPORTED;
}


/*************************************************************************
 * Additional ASM intrinsic stubs
 *************************************************************************/

unsigned ASMBitFirstSetU16(uint16_t u16) RT_NOTHROW_DEF
{
    if (u16 == 0)
        return 0;
    return __builtin_ctz((uint32_t)u16) + 1;
}

unsigned ASMBitLastSetU16(uint16_t u16) RT_NOTHROW_DEF
{
    if (u16 == 0)
        return 0;
    return 16 - __builtin_clz((uint32_t)u16);
}

unsigned ASMCountLeadingZerosU16(uint16_t u16) RT_NOTHROW_DEF
{
    if (u16 == 0)
        return 16;
    return __builtin_clz((uint32_t)u16) - 16;
}

unsigned ASMCountTrailingZerosU16(uint16_t u16) RT_NOTHROW_DEF
{
    if (u16 == 0)
        return 16;
    return __builtin_ctz((uint32_t)u16);
}

bool ASMAtomicCmpXchgExU8(volatile uint8_t *pu8, const uint8_t u8New, const uint8_t u8Old, uint8_t *pu8Old) RT_NOTHROW_DEF
{
    *pu8Old = __sync_val_compare_and_swap(pu8, u8Old, u8New);
    return *pu8Old == u8Old;
}

bool ASMAtomicCmpXchgExU16(volatile uint16_t *pu16, const uint16_t u16New, const uint16_t u16Old, uint16_t *pu16Old) RT_NOTHROW_DEF
{
    *pu16Old = __sync_val_compare_and_swap(pu16, u16Old, u16New);
    return *pu16Old == u16Old;
}

bool ASMAtomicCmpXchgU128v2(volatile uint128_t *pu128, uint64_t u64NewHi, uint64_t u64NewLo,
                             uint64_t u64OldHi, uint64_t u64OldLo, uint128_t *pu128Old) RT_NOTHROW_DEF
{
    RT_NOREF(pu128, u64NewHi, u64NewLo, u64OldHi, u64OldLo, pu128Old);
    /* 128-bit CAS not available in Wasm — return failure */
    return false;
}

void ASMAtomicUoOrU64(volatile uint64_t *pu64, uint64_t u64) RT_NOTHROW_DEF
{
    __sync_fetch_and_or(pu64, u64);
}

uint64_t ASMMultU64ByU32DivByU32(uint64_t u64, uint32_t u32A, uint32_t u32B) RT_NOTHROW_DEF
{
    /* Use 128-bit arithmetic via compiler built-in or fall back */
    __uint128_t u128 = (__uint128_t)u64 * u32A;
    return (uint64_t)(u128 / u32B);
}


/*************************************************************************
 * RTTimer stubs
 *************************************************************************/

#include <iprt/thread.h>

RTDECL(int) RTTimerCreate(PRTTIMER *ppTimer, unsigned uMilliesInterval, PFNRTTIMER pfnTimer, void *pvUser)
{
    RT_NOREF(ppTimer, uMilliesInterval, pfnTimer, pvUser);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTTimerCreateEx(PRTTIMER *ppTimer, uint64_t u64NanoInterval, uint32_t fFlags,
                             PFNRTTIMER pfnTimer, void *pvUser)
{
    RT_NOREF(ppTimer, u64NanoInterval, fFlags, pfnTimer, pvUser);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTTimerDestroy(PRTTIMER pTimer)
{
    RT_NOREF(pTimer);
    return VINF_SUCCESS;
}

RTDECL(int) RTTimerStart(PRTTIMER pTimer, uint64_t u64First)
{
    RT_NOREF(pTimer, u64First);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTTimerStop(PRTTIMER pTimer)
{
    RT_NOREF(pTimer);
    return VINF_SUCCESS;
}


/*************************************************************************
 * RTSemEvent extended wait stubs
 *************************************************************************/

RTDECL(int) RTSemEventWaitEx(RTSEMEVENT hEvent, uint32_t fFlags, uint64_t uTimeout)
{
    RT_NOREF(fFlags);
    uint32_t cMillies = uTimeout > 0 ? RT_MAX(uTimeout / RT_NS_1MS, 1) : 0;
    return RTSemEventWaitNoResume(hEvent, cMillies);
}

RTDECL(uint32_t) RTSemEventGetResolution(void)
{
    return 1000000; /* 1ms */
}


/*************************************************************************
 * Misc IPRT stubs
 *************************************************************************/

RTDECL(int) RTMemProtect(void *pv, size_t cb, unsigned fProtect)
{
    RT_NOREF(pv, cb, fProtect);
    return VINF_SUCCESS; /* No-op — Wasm has no memory protection */
}

RTDECL(int) RTRandAdvCreateSystemFaster(PRTRAND phRand)
{
    RT_NOREF(phRand);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTThreadPoke(RTTHREAD hThread)
{
    RT_NOREF(hThread);
    return VINF_SUCCESS;
}

RTDECL(int) RTThreadGetExecutionTimeMilli(uint64_t *pcMsKernelTime, uint64_t *pcMsUserTime)
{
    if (pcMsKernelTime) *pcMsKernelTime = 0;
    if (pcMsUserTime) *pcMsUserTime = 0;
    return VINF_SUCCESS;
}

RTDECL(bool) RTPathExists(const char *pszPath)
{
    struct stat st;
    return stat(pszPath, &st) == 0;
}

RTDECL(int) RTPathGetCurrent(char *pszPath, size_t cbPath)
{
    return RTStrCopy(pszPath, cbPath, "/");
}

RTDECL(int) RTPathGetCurrentOnDrive(char chDrive, char *pszPath, size_t cbPath)
{
    RT_NOREF(chDrive);
    return RTStrCopy(pszPath, cbPath, "/");
}

RTDECL(int) RTPathQueryInfo(const char *pszPath, PRTFSOBJINFO pObjInfo, RTFSOBJATTRADD enmAdditionalAttribs)
{
    RT_NOREF(enmAdditionalAttribs);
    struct stat st;
    if (stat(pszPath, &st) != 0)
        return VERR_FILE_NOT_FOUND;
    memset(pObjInfo, 0, sizeof(*pObjInfo));
    pObjInfo->cbObject = st.st_size;
    return VINF_SUCCESS;
}

RTDECL(int) RTPathQueryInfoEx(const char *pszPath, PRTFSOBJINFO pObjInfo, RTFSOBJATTRADD enmAdditionalAttribs, uint32_t fFlags)
{
    RT_NOREF(fFlags);
    return RTPathQueryInfo(pszPath, pObjInfo, enmAdditionalAttribs);
}


/*************************************************************************
 * RTFile — Extended file ops stubs
 *************************************************************************/

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

RTDECL(int) RTFileOpenEx(const char *pszFilename, uint64_t fOpen, PRTFILE phFile, PRTFILEACTION penmActionTaken)
{
    RT_NOREF(pszFilename, fOpen, phFile, penmActionTaken);
    return VERR_NOT_SUPPORTED;
}

RTDECL(bool) RTFileIsValid(RTFILE hFile)
{
    RT_NOREF(hFile);
    return false;
}

RTDECL(int) RTFileRead(RTFILE hFile, void *pvBuf, size_t cbToRead, size_t *pcbRead)
{
    RT_NOREF(hFile, pvBuf, cbToRead, pcbRead);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTFileReadAt(RTFILE hFile, RTFOFF off, void *pvBuf, size_t cbToRead, size_t *pcbRead)
{
    RT_NOREF(hFile, off, pvBuf, cbToRead, pcbRead);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTFileWriteAt(RTFILE hFile, RTFOFF off, const void *pvBuf, size_t cbToWrite, size_t *pcbWritten)
{
    RT_NOREF(hFile, off, pvBuf, cbToWrite, pcbWritten);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTFileSeek(RTFILE hFile, int64_t offSeek, unsigned uMethod, uint64_t *poffActual)
{
    RT_NOREF(hFile, offSeek, uMethod, poffActual);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTFileQueryInfo(RTFILE hFile, PRTFSOBJINFO pObjInfo, RTFSOBJATTRADD enmAdditionalAttribs)
{
    RT_NOREF(hFile, pObjInfo, enmAdditionalAttribs);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTFileSetSize(RTFILE hFile, uint64_t cbSize)
{
    RT_NOREF(hFile, cbSize);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTFileSetTimes(RTFILE hFile, PCRTTIMESPEC pAccessTime, PCRTTIMESPEC pModificationTime,
                            PCRTTIMESPEC pChangeTime, PCRTTIMESPEC pBirthTime)
{
    RT_NOREF(hFile, pAccessTime, pModificationTime, pChangeTime, pBirthTime);
    return VINF_SUCCESS;
}

RTDECL(int) RTFileSetMode(RTFILE hFile, RTFMODE fMode)
{
    RT_NOREF(hFile, fMode);
    return VINF_SUCCESS;
}

RTDECL(int) RTFileSetAllocationSize(RTFILE hFile, uint64_t cbSize, uint32_t fFlags)
{
    RT_NOREF(hFile, cbSize, fFlags);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTFileQueryFsSizes(RTFILE hFile, PRTFOFF pcbTotal, RTFOFF *pcbFree, uint32_t *pcbBlock, uint32_t *pcbSector)
{
    RT_NOREF(hFile);
    if (pcbTotal) *pcbTotal = 1024*1024*1024;
    if (pcbFree) *pcbFree = 512*1024*1024;
    if (pcbBlock) *pcbBlock = 4096;
    if (pcbSector) *pcbSector = 512;
    return VINF_SUCCESS;
}

RTDECL(int) RTFileQueryMaxSizeEx(RTFILE hFile, PRTFOFF pcbMax)
{
    RT_NOREF(hFile);
    if (pcbMax) *pcbMax = INT64_MAX;
    return VINF_SUCCESS;
}

RTDECL(int) RTFileQuerySectorSize(RTFILE hFile, uint32_t *pcbSector)
{
    RT_NOREF(hFile);
    if (pcbSector) *pcbSector = 512;
    return VINF_SUCCESS;
}

RTDECL(int) RTFileMove(const char *pszSrc, const char *pszDst, unsigned fMove)
{
    RT_NOREF(pszSrc, pszDst, fMove);
    return VERR_NOT_SUPPORTED;
}

RTDECL(RTHCINTPTR) RTFileToNative(RTFILE hFile)
{
    return (RTHCINTPTR)hFile;
}

RTDECL(int) RTFileCopyPartPrep(PRTFILECOPYPARTBUFSTATE pBufState, uint64_t cbToCopy)
{
    RT_NOREF(pBufState, cbToCopy);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTFileCopyPartEx(RTFILE hFileSrc, RTFOFF offSrc, RTFILE hFileDst, RTFOFF offDst,
                              uint64_t cbToCopy, uint32_t fFlags, PRTFILECOPYPARTBUFSTATE pBufState,
                              uint64_t *pcbCopied)
{
    RT_NOREF(hFileSrc, offSrc, hFileDst, offDst, cbToCopy, fFlags, pBufState, pcbCopied);
    return VERR_NOT_SUPPORTED;
}

RTDECL(void) RTFileCopyPartCleanup(PRTFILECOPYPARTBUFSTATE pBufState)
{
    RT_NOREF(pBufState);
}


/*************************************************************************
 * RTDir stubs
 *************************************************************************/

#include <iprt/dir.h>

RTDECL(int) RTDirClose(RTDIR hDir)
{
    RT_NOREF(hDir);
    return VINF_SUCCESS;
}

RTDECL(int) RTDirCreate(const char *pszPath, RTFMODE fMode, uint32_t fCreate)
{
    RT_NOREF(pszPath, fMode, fCreate);
    return VERR_NOT_SUPPORTED;
}

RTDECL(bool) RTDirExists(const char *pszPath)
{
    struct stat st;
    return stat(pszPath, &st) == 0 && S_ISDIR(st.st_mode);
}

RTDECL(int) RTDirQueryInfo(RTDIR hDir, PRTFSOBJINFO pObjInfo, RTFSOBJATTRADD enmAdditionalAttribs)
{
    RT_NOREF(hDir, pObjInfo, enmAdditionalAttribs);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTDirRead(RTDIR hDir, PRTDIRENTRY pDirEntry, size_t *pcbDirEntry)
{
    RT_NOREF(hDir, pDirEntry, pcbDirEntry);
    return VERR_NO_MORE_FILES;
}

RTDECL(int) RTDirReadEx(RTDIR hDir, PRTDIRENTRYEX pDirEntry, size_t *pcbDirEntry,
                         RTFSOBJATTRADD enmAdditionalAttribs, uint32_t fFlags)
{
    RT_NOREF(hDir, pDirEntry, pcbDirEntry, enmAdditionalAttribs, fFlags);
    return VERR_NO_MORE_FILES;
}

RTDECL(int) RTDirSetTimes(RTDIR hDir, PCRTTIMESPEC pAccessTime, PCRTTIMESPEC pModificationTime,
                           PCRTTIMESPEC pChangeTime, PCRTTIMESPEC pBirthTime)
{
    RT_NOREF(hDir, pAccessTime, pModificationTime, pChangeTime, pBirthTime);
    return VINF_SUCCESS;
}


/*************************************************************************
 * RTFs stubs
 *************************************************************************/

RTDECL(int) RTFsQueryProperties(const char *pszFsPath, PRTFSPROPERTIES pProperties)
{
    RT_NOREF(pszFsPath);
    if (pProperties)
    {
        memset(pProperties, 0, sizeof(*pProperties));
        pProperties->cbMaxComponent = 255;
        pProperties->fCaseSensitive = true;
    }
    return VINF_SUCCESS;
}

RTDECL(int) RTFsQuerySizes(const char *pszFsPath, RTFOFF *pcbTotal, RTFOFF *pcbFree,
                             uint32_t *pcbBlock, uint32_t *pcbSector)
{
    RT_NOREF(pszFsPath);
    if (pcbTotal) *pcbTotal = 1024*1024*1024;
    if (pcbFree) *pcbFree = 512*1024*1024;
    if (pcbBlock) *pcbBlock = 4096;
    if (pcbSector) *pcbSector = 512;
    return VINF_SUCCESS;
}


/*************************************************************************
 * RTTls stubs
 *************************************************************************/

static void *g_aTlsSlots[64] = {0};

RTDECL(int) RTTlsFree(RTTLS iTls)
{
    if (iTls < 64)
        g_aTlsSlots[iTls] = NULL;
    return VINF_SUCCESS;
}

RTDECL(void *) RTTlsGet(RTTLS iTls)
{
    if (iTls < 64)
        return g_aTlsSlots[iTls];
    return NULL;
}

RTDECL(int) RTTlsSet(RTTLS iTls, void *pvValue)
{
    if (iTls < 64)
    {
        g_aTlsSlots[iTls] = pvValue;
        return VINF_SUCCESS;
    }
    return VERR_INVALID_PARAMETER;
}


/*************************************************************************
 * RTUuid — Extended stubs
 *************************************************************************/

RTDECL(int) RTUuidCreate(PRTUUID pUuid)
{
    AssertReturn(pUuid, VERR_INVALID_PARAMETER);
    /* Generate a pseudo-random UUID */
    for (unsigned i = 0; i < 16; i++)
        pUuid->au8[i] = (uint8_t)(i * 17 + 42);
    /* Set version 4 (random) and variant bits */
    pUuid->au8[6] = (pUuid->au8[6] & 0x0f) | 0x40; /* version 4 */
    pUuid->au8[8] = (pUuid->au8[8] & 0x3f) | 0x80; /* variant 1 */
    return VINF_SUCCESS;
}

RTDECL(int) RTUuidFromStr(PRTUUID pUuid, const char *pszString)
{
    RT_NOREF(pszString);
    AssertReturn(pUuid, VERR_INVALID_PARAMETER);
    memset(pUuid, 0, sizeof(*pUuid));
    return VINF_SUCCESS;
}

RTDECL(int) RTUuidToStr(PCRTUUID pUuid, char *pszString, size_t cchString)
{
    RT_NOREF(pUuid);
    return RTStrCopy(pszString, cchString, "00000000-0000-0000-0000-000000000000");
}

RTDECL(int) RTUuidCompareStr(PCRTUUID pUuid1, const char *pszString)
{
    RT_NOREF(pUuid1, pszString);
    return 0;
}


/*************************************************************************
 * RTProcCreate stub
 *************************************************************************/

RTDECL(int) RTProcCreate(const char *pszExec, const char * const *papszArgs,
                          RTENV hEnv, unsigned fFlags, PRTPROCESS phProcess)
{
    RT_NOREF(pszExec, papszArgs, hEnv, fFlags, phProcess);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTProcWait(RTPROCESS hProcess, unsigned fFlags, PRTPROCSTATUS pProcStatus)
{
    RT_NOREF(hProcess, fFlags, pProcStatus);
    return VERR_NOT_SUPPORTED;
}


/*************************************************************************
 * RTDirRel stubs (used by VD)
 *************************************************************************/

RTDECL(int) RTDirRelDirCreate(RTDIR hDir, const char *pszRelPath, RTFMODE fMode, uint32_t fFlags, RTDIR *phSubDir)
{
    RT_NOREF(hDir, pszRelPath, fMode, fFlags, phSubDir);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTDirRelDirOpenFiltered(RTDIR hDir, const char *pszFilter, RTDIRFILTER enmFilter,
                                     uint32_t fFlags, RTDIR *phSubDir)
{
    RT_NOREF(hDir, pszFilter, enmFilter, fFlags, phSubDir);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTDirRelDirRemove(RTDIR hDir, const char *pszRelPath)
{
    RT_NOREF(hDir, pszRelPath);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTDirRelFileOpen(RTDIR hDir, const char *pszRelFilename, uint64_t fOpen, PRTFILE phFile)
{
    RT_NOREF(hDir, pszRelFilename, fOpen, phFile);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTDirRelPathQueryInfo(RTDIR hDir, const char *pszRelPath, PRTFSOBJINFO pObjInfo,
                                   RTFSOBJATTRADD enmAdditionalAttribs, uint32_t fFlags)
{
    RT_NOREF(hDir, pszRelPath, pObjInfo, enmAdditionalAttribs, fFlags);
    return VERR_FILE_NOT_FOUND;
}

RTDECL(int) RTDirRelPathRename(RTDIR hDirSrc, const char *pszSrc, RTDIR hDirDst, const char *pszDst, unsigned fRename)
{
    RT_NOREF(hDirSrc, pszSrc, hDirDst, pszDst, fRename);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTDirRelPathSetOwner(RTDIR hDir, const char *pszRelPath, uint32_t uid, uint32_t gid, uint32_t fFlags)
{
    RT_NOREF(hDir, pszRelPath, uid, gid, fFlags);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTDirRelPathUnlink(RTDIR hDir, const char *pszRelPath, uint32_t fUnlink)
{
    RT_NOREF(hDir, pszRelPath, fUnlink);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTDirRelSymlinkCreate(RTDIR hDir, const char *pszSymlink, const char *pszTarget,
                                    RTSYMLINKTYPE enmType, uint32_t fCreate)
{
    RT_NOREF(hDir, pszSymlink, pszTarget, enmType, fCreate);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTDirRelSymlinkRead(RTDIR hDir, const char *pszSymlink, char *pszTarget, size_t cbTarget, uint32_t fRead)
{
    RT_NOREF(hDir, pszSymlink, pszTarget, cbTarget, fRead);
    return VERR_NOT_SUPPORTED;
}


/*************************************************************************
 * rtMemPageNative stubs — used by RTMemPageAlloc
 *************************************************************************/

int rtMemPageNativeAlloc(size_t cb, uint32_t fFlags, void **ppv)
{
    RT_NOREF(fFlags);
    *ppv = calloc(1, cb);
    return *ppv ? VINF_SUCCESS : VERR_NO_MEMORY;
}

int rtMemPageNativeFree(void *pv, size_t cb)
{
    RT_NOREF(cb);
    free(pv);
    return VINF_SUCCESS;
}

int rtMemPageNativeApplyFlags(void *pv, size_t cb, uint32_t fFlags)
{
    RT_NOREF(pv, cb, fFlags);
    return VINF_SUCCESS;
}

int rtMemPageNativeRevertFlags(void *pv, size_t cb, uint32_t fFlags)
{
    RT_NOREF(pv, cb, fFlags);
    return VINF_SUCCESS;
}


/*************************************************************************
 * rtldrNative stubs — dynamic library loading
 *************************************************************************/

int rtldrNativeLoad(const char *pszFilename, uintptr_t *phHandle, uint32_t fFlags, PRTERRINFO pErrInfo)
{
    RT_NOREF(pszFilename, phHandle, fFlags, pErrInfo);
    return VERR_NOT_SUPPORTED;
}

int rtldrNativeGetSymbol(uintptr_t hLdrMod, const char *pszSymbol, void **ppvValue)
{
    RT_NOREF(hLdrMod, pszSymbol, ppvValue);
    return VERR_SYMBOL_NOT_FOUND;
}

int rtldrNativeClose(uintptr_t hLdrMod)
{
    RT_NOREF(hLdrMod);
    return VINF_SUCCESS;
}


/*************************************************************************
 * rtDirNative stubs
 *************************************************************************/

struct RTDIRINTERNAL;

size_t rtDirNativeGetStructSize(const char *pszPath)
{
    RT_NOREF(pszPath);
    return 256;
}

int rtDirNativeOpen(struct RTDIRINTERNAL *pDir, uintptr_t hRelativeDir, void *pvNativeRelative)
{
    RT_NOREF(pDir, hRelativeDir, pvNativeRelative);
    return VERR_NOT_SUPPORTED;
}


/*************************************************************************
 * RTCr* crypto stubs
 *************************************************************************/

RTDECL(int) RTCrCipherOpenByType(void **phCipher, uint32_t enmType, uint32_t fFlags)
{
    RT_NOREF(phCipher, enmType, fFlags);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTCrCipherRelease(void *hCipher)
{
    RT_NOREF(hCipher);
    return VINF_SUCCESS;
}

RTDECL(int) RTCrCipherEncrypt(void *hCipher, const void *pvKey, size_t cbKey,
                               const void *pvInitVector, size_t cbInitVector,
                               const void *pvPlainText, size_t cbPlainText,
                               void *pvEncrypted, size_t cbEncrypted, size_t *pcbEncrypted,
                               void *pvTag, size_t cbTag, size_t *pcbTag)
{
    RT_NOREF(hCipher, pvKey, cbKey, pvInitVector, cbInitVector, pvPlainText, cbPlainText,
             pvEncrypted, cbEncrypted, pcbEncrypted, pvTag, cbTag, pcbTag);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTCrCipherDecrypt(void *hCipher, const void *pvKey, size_t cbKey,
                               const void *pvInitVector, size_t cbInitVector,
                               const void *pvEncrypted, size_t cbEncrypted,
                               void *pvPlainText, size_t cbPlainText, size_t *pcbPlainText,
                               void *pvTag, size_t cbTag)
{
    RT_NOREF(hCipher, pvKey, cbKey, pvInitVector, cbInitVector, pvEncrypted, cbEncrypted,
             pvPlainText, cbPlainText, pcbPlainText, pvTag, cbTag);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTCrPkcs5Pbkdf2Hmac(void *pvOutput, size_t cbOutput, const void *pvInput, size_t cbInput,
                                  const void *pvSalt, size_t cbSalt, uint32_t cIterations, uint32_t enmDigestType)
{
    RT_NOREF(pvOutput, cbOutput, pvInput, cbInput, pvSalt, cbSalt, cIterations, enmDigestType);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTCrRandBytes(void *pvBuf, size_t cbBuf)
{
    memset(pvBuf, 0, cbBuf);
    return VINF_SUCCESS;
}


/*************************************************************************
 * VBoxDD driver stubs — symbols for drivers not compiled in Wasm
 *************************************************************************/
#include <VBox/vmm/pdmdrv.h>

extern const PDMDRVREG g_DrvIfTrace;
const PDMDRVREG g_DrvIfTrace = {0};


/*************************************************************************
 * libxml2 stubs — VBox OVF/settings code links against libxml2
 *************************************************************************/

void xmlCheckVersion(int version)
{
    RT_NOREF(version);
}

void xmlInitParser(void)
{
}

void xmlCleanupParser(void)
{
}

void *xmlNewParserCtxt(void)
{
    return NULL;
}

void xmlFreeParserCtxt(void *ctxt)
{
    RT_NOREF(ctxt);
}

void *xmlCtxtReadMemory(void *ctxt, const char *buffer, int size, const char *URL,
                         const char *encoding, int options)
{
    RT_NOREF(ctxt, buffer, size, URL, encoding, options);
    return NULL;
}

void *xmlCtxtGetLastError(void *ctxt)
{
    RT_NOREF(ctxt);
    return NULL;
}

void *xmlDocGetRootElement(void *doc)
{
    RT_NOREF(doc);
    return NULL;
}

void xmlFreeDoc(void *doc)
{
    RT_NOREF(doc);
}

void xmlSetGenericErrorFunc(void *ctx, void *handler)
{
    RT_NOREF(ctx, handler);
}

void xmlSetStructuredErrorFunc(void *ctx, void *handler)
{
    RT_NOREF(ctx, handler);
}

typedef void *(*xmlExternalEntityLoader)(const char *, const char *, void *);

xmlExternalEntityLoader xmlGetExternalEntityLoader(void)
{
    return NULL;
}

void xmlSetExternalEntityLoader(xmlExternalEntityLoader loader)
{
    RT_NOREF(loader);
}

} /* extern "C" */
