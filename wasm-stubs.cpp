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
#include <iprt/stream.h>

/*
 * Debug: shared ring buffer for cross-thread stub logging.
 * EMT thread output gets lost because Emscripten's printf proxy
 * can't deliver while the main thread is blocked in VMR3ReqCallU.
 * This buffer is read by the main thread after VMR3Create returns.
 */
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>

#define STUB_LOG_SIZE 4096
static char g_szStubLog[STUB_LOG_SIZE];
static volatile int g_iStubLogPos = 0;

static void stubLogAppend(const char *pszName)
{
    char szMsg[128];
    int n = snprintf(szMsg, sizeof(szMsg), "STUB: %s\n", pszName);
    if (n <= 0) return;
    int pos = __atomic_fetch_add(&g_iStubLogPos, n, __ATOMIC_SEQ_CST);
    if (pos + n < STUB_LOG_SIZE)
        memcpy(&g_szStubLog[pos], szMsg, n);
}

/* Also try RTPrintf in case it works */
#define STUB_NOT_SUPPORTED(name) \
    do { stubLogAppend(name); RTPrintf("STUB: %s returning VERR_NOT_SUPPORTED\n", name); } while (0)

/* Exported so wasm-main.cpp can read it */
extern "C" const char *wasmStubGetLog(void) { return g_szStubLog; }
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
#include <time.h>


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
    STUB_NOT_SUPPORTED("SUPR3HardenedLdrLoadPlugIn");
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
    STUB_NOT_SUPPORTED("SUPR3CallVMMR0");
    return VERR_NOT_SUPPORTED;
}

SUPR3DECL(int) SUPR3CallVMMR0Ex(PVMR0 pVMR0, VMCPUID idCpu, unsigned uOperation,
                                 uint64_t u64Arg, PSUPVMMR0REQHDR pReqHdr)
{
    RT_NOREF(pVMR0, idCpu, uOperation, u64Arg, pReqHdr);
    STUB_NOT_SUPPORTED("SUPR3CallVMMR0Ex");
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
    STUB_NOT_SUPPORTED("SUPR3GipMap");
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
    STUB_NOT_SUPPORTED("SUPR3LoadModule");
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
    STUB_NOT_SUPPORTED("SUPR3GetSymbolR0");
    return VERR_NOT_SUPPORTED;
}


/*************************************************************************
 * x86 CPUID stubs — fake a basic AMD64 CPU for IEM
 *************************************************************************/

#include <iprt/x86.h>

/* We can't include asm-amd64-x86.h (guarded by RT_ARCH_AMD64 which cdefs.h
   undefines for __EMSCRIPTEN__).  Declare the types we need directly. */
#pragma pack(1)
typedef struct RTIDTR { uint16_t cbIdt; uint64_t pIdt; } RTIDTR, *PRTIDTR;
typedef struct RTGDTR { uint16_t cbGdt; uint64_t pGdt; } RTGDTR, *PRTGDTR;
#pragma pack()

/**
 * Fake CPUID results for a minimal AMD64 CPU.
 * We report SSE2 (required by VMR3.cpp:570) and basic features
 * that VBox expects from a 64-bit host CPU.
 */
static void asmFakeCpuId(uint32_t uLeaf, uint32_t uSubLeaf,
                          uint32_t *pEAX, uint32_t *pEBX, uint32_t *pECX, uint32_t *pEDX)
{
    RT_NOREF(uSubLeaf);
    *pEAX = *pEBX = *pECX = *pEDX = 0;

    switch (uLeaf)
    {
        case 0x00000000: /* Max standard leaf + vendor */
            *pEAX = 0x0000000d; /* max leaf */
            /* "GenuineIntel" */
            *pEBX = 0x756e6547;
            *pEDX = 0x49656e69;
            *pECX = 0x6c65746e;
            break;

        case 0x00000001: /* Family/model/stepping + features */
            *pEAX = 0x000306c3; /* Haswell-like family 6 model 60 stepping 3 */
            *pEBX = 0x00010800; /* CLFLUSH=8, logical CPUs=1, APIC ID=0 */
            *pECX = X86_CPUID_FEATURE_ECX_SSE3
                  | X86_CPUID_FEATURE_ECX_SSSE3
                  | X86_CPUID_FEATURE_ECX_SSE4_1
                  | X86_CPUID_FEATURE_ECX_SSE4_2
                  | X86_CPUID_FEATURE_ECX_POPCNT;
            *pEDX = X86_CPUID_FEATURE_EDX_FPU
                  | X86_CPUID_FEATURE_EDX_TSC
                  | X86_CPUID_FEATURE_EDX_MSR
                  | X86_CPUID_FEATURE_EDX_CX8
                  | X86_CPUID_FEATURE_EDX_APIC
                  | X86_CPUID_FEATURE_EDX_SEP
                  | X86_CPUID_FEATURE_EDX_CMOV
                  | X86_CPUID_FEATURE_EDX_PAT
                  | X86_CPUID_FEATURE_EDX_PSE36
                  | X86_CPUID_FEATURE_EDX_CLFSH
                  | X86_CPUID_FEATURE_EDX_MMX
                  | X86_CPUID_FEATURE_EDX_FXSR
                  | X86_CPUID_FEATURE_EDX_SSE
                  | X86_CPUID_FEATURE_EDX_SSE2;
            break;

        case 0x80000000: /* Max extended leaf */
            *pEAX = 0x80000008;
            break;

        case 0x80000001: /* Extended features */
            *pECX = 0;
            *pEDX = RT_BIT_32(29) /* Long Mode */
                  | RT_BIT_32(20) /* NX */;
            break;

        case 0x80000008: /* Address sizes */
            *pEAX = 0x00003028; /* 48-bit virtual, 40-bit physical */
            break;

        default:
            break;
    }
}

DECLASM(void) ASMCpuId(uint32_t uOperator, void RT_FAR *pvEAX, void RT_FAR *pvEBX,
                        void RT_FAR *pvECX, void RT_FAR *pvEDX)
{
    asmFakeCpuId(uOperator, 0, (uint32_t *)pvEAX, (uint32_t *)pvEBX,
                 (uint32_t *)pvECX, (uint32_t *)pvEDX);
}

DECLASM(void) ASMCpuId_Idx_ECX(uint32_t uOperator, uint32_t uIdxECX,
                                void RT_FAR *pvEAX, void RT_FAR *pvEBX,
                                void RT_FAR *pvECX, void RT_FAR *pvEDX)
{
    asmFakeCpuId(uOperator, uIdxECX, (uint32_t *)pvEAX, (uint32_t *)pvEBX,
                 (uint32_t *)pvECX, (uint32_t *)pvEDX);
}

DECLASM(uint32_t) ASMCpuIdExSlow(uint32_t uOperator, uint32_t uInitEBX, uint32_t uInitECX,
                                  uint32_t uInitEDX, uint32_t *pEAX, uint32_t *pEBX,
                                  uint32_t *pECX, uint32_t *pEDX)
{
    RT_NOREF(uInitEBX, uInitEDX);
    asmFakeCpuId(uOperator, uInitECX, pEAX, pEBX, pECX, pEDX);
    return *pEAX;
}

uint32_t ASMCpuId_EAX(uint32_t uOperator) RT_NOTHROW_DEF
{
    uint32_t eax, ebx, ecx, edx;
    asmFakeCpuId(uOperator, 0, &eax, &ebx, &ecx, &edx);
    return eax;
}

uint32_t ASMCpuId_EBX(uint32_t uOperator) RT_NOTHROW_DEF
{
    uint32_t eax, ebx, ecx, edx;
    asmFakeCpuId(uOperator, 0, &eax, &ebx, &ecx, &edx);
    return ebx;
}

uint32_t ASMCpuId_ECX(uint32_t uOperator) RT_NOTHROW_DEF
{
    uint32_t eax, ebx, ecx, edx;
    asmFakeCpuId(uOperator, 0, &eax, &ebx, &ecx, &edx);
    return ecx;
}

uint32_t ASMCpuId_EDX(uint32_t uOperator) RT_NOTHROW_DEF
{
    uint32_t eax, ebx, ecx, edx;
    asmFakeCpuId(uOperator, 0, &eax, &ebx, &ecx, &edx);
    return edx;
}

void ASMCpuId_ECX_EDX(uint32_t uOperator, void RT_FAR *pvECX, void RT_FAR *pvEDX) RT_NOTHROW_DEF
{
    uint32_t eax, ebx;
    asmFakeCpuId(uOperator, 0, &eax, &ebx, (uint32_t *)pvECX, (uint32_t *)pvEDX);
}

DECLASM(bool) ASMHasCpuId(void)
{
    return true;
}

uint8_t ASMGetApicId(void) RT_NOTHROW_DEF
{
    return 0;
}

uint32_t ASMGetApicIdExt0B(void) RT_NOTHROW_DEF
{
    return 0;
}


/*************************************************************************
 * x86 MSR / CR / TSC / segment register stubs
 *************************************************************************/

uint64_t ASMRdMsr(uint32_t uRegister) RT_NOTHROW_DEF
{
    RT_NOREF(uRegister);
    return 0;
}

uint64_t ASMRdMsrEx(uint32_t uRegister, RTCCUINTXREG uXDI) RT_NOTHROW_DEF
{
    RT_NOREF(uRegister, uXDI);
    return 0;
}

void ASMWrMsr(uint32_t uRegister, uint64_t u64Val) RT_NOTHROW_DEF
{
    RT_NOREF(uRegister, u64Val);
}

void ASMWrMsrEx(uint32_t uRegister, RTCCUINTXREG uXDI, uint64_t u64Val) RT_NOTHROW_DEF
{
    RT_NOREF(uRegister, uXDI, u64Val);
}

uint32_t ASMRdMsr_Low(uint32_t uRegister) RT_NOTHROW_DEF
{
    RT_NOREF(uRegister);
    return 0;
}

uint32_t ASMRdMsr_High(uint32_t uRegister) RT_NOTHROW_DEF
{
    RT_NOREF(uRegister);
    return 0;
}

RTCCUINTXREG ASMGetCR0(void) RT_NOTHROW_DEF { return 0x80000011; /* PE|ET|PG */ }
void ASMSetCR0(RTCCUINTXREG uCR0) RT_NOTHROW_DEF { RT_NOREF(uCR0); }
RTCCUINTXREG ASMGetCR2(void) RT_NOTHROW_DEF { return 0; }
void ASMSetCR2(RTCCUINTXREG uCR2) RT_NOTHROW_DEF { RT_NOREF(uCR2); }
RTCCUINTXREG ASMGetCR3(void) RT_NOTHROW_DEF { return 0; }
void ASMSetCR3(RTCCUINTXREG uCR3) RT_NOTHROW_DEF { RT_NOREF(uCR3); }
void ASMReloadCR3(void) RT_NOTHROW_DEF {}
RTCCUINTXREG ASMGetCR4(void) RT_NOTHROW_DEF { return 0; }
void ASMSetCR4(RTCCUINTXREG uCR4) RT_NOTHROW_DEF { RT_NOREF(uCR4); }

RTCCUINTXREG ASMGetDR0(void) RT_NOTHROW_DEF { return 0; }
RTCCUINTXREG ASMGetDR1(void) RT_NOTHROW_DEF { return 0; }
RTCCUINTXREG ASMGetDR2(void) RT_NOTHROW_DEF { return 0; }
RTCCUINTXREG ASMGetDR3(void) RT_NOTHROW_DEF { return 0; }
RTCCUINTXREG ASMGetDR6(void) RT_NOTHROW_DEF { return 0xFFFF0FF0; }
RTCCUINTXREG ASMGetAndClearDR6(void) RT_NOTHROW_DEF { return 0xFFFF0FF0; }
RTCCUINTXREG ASMGetDR7(void) RT_NOTHROW_DEF { return 0x00000400; }
void ASMSetDR0(RTCCUINTXREG uDRVal) RT_NOTHROW_DEF { RT_NOREF(uDRVal); }
void ASMSetDR1(RTCCUINTXREG uDRVal) RT_NOTHROW_DEF { RT_NOREF(uDRVal); }
void ASMSetDR2(RTCCUINTXREG uDRVal) RT_NOTHROW_DEF { RT_NOREF(uDRVal); }
void ASMSetDR3(RTCCUINTXREG uDRVal) RT_NOTHROW_DEF { RT_NOREF(uDRVal); }
void ASMSetDR6(RTCCUINTXREG uDRVal) RT_NOTHROW_DEF { RT_NOREF(uDRVal); }
void ASMSetDR7(RTCCUINTXREG uDRVal) RT_NOTHROW_DEF { RT_NOREF(uDRVal); }

RTCCUINTREG ASMGetFlags(void) RT_NOTHROW_DEF { return 0x202; /* IF */ }
void ASMSetFlags(RTCCUINTREG uFlags) RT_NOTHROW_DEF { RT_NOREF(uFlags); }
RTCCUINTREG ASMChangeFlags(RTCCUINTREG fAndEfl, RTCCUINTREG fOrEfl) RT_NOTHROW_DEF { RT_NOREF(fAndEfl, fOrEfl); return 0x202; }
RTCCUINTREG ASMAddFlags(RTCCUINTREG fOrEfl) RT_NOTHROW_DEF { RT_NOREF(fOrEfl); return 0x202; }
RTCCUINTREG ASMClearFlags(RTCCUINTREG fAndEfl) RT_NOTHROW_DEF { RT_NOREF(fAndEfl); return 0x202; }

void ASMIntEnable(void) RT_NOTHROW_DEF {}
void ASMIntDisable(void) RT_NOTHROW_DEF {}
RTCCUINTREG ASMIntDisableFlags(void) RT_NOTHROW_DEF { return 0x202; }
void ASMHalt(void) RT_NOTHROW_DEF {}

RTSEL ASMGetCS(void) RT_NOTHROW_DEF { return 0x08; }
RTSEL ASMGetDS(void) RT_NOTHROW_DEF { return 0x10; }
RTSEL ASMGetES(void) RT_NOTHROW_DEF { return 0x10; }
RTSEL ASMGetFS(void) RT_NOTHROW_DEF { return 0; }
RTSEL ASMGetGS(void) RT_NOTHROW_DEF { return 0; }
RTSEL ASMGetSS(void) RT_NOTHROW_DEF { return 0x10; }
RTSEL ASMGetTR(void) RT_NOTHROW_DEF { return 0x28; }
RTSEL ASMGetLDTR(void) RT_NOTHROW_DEF { return 0; }
uint32_t ASMGetSegAttr(uint32_t uSel) RT_NOTHROW_DEF { RT_NOREF(uSel); return 0; }

void ASMGetIDTR(PRTIDTR pIdtr) RT_NOTHROW_DEF { memset(pIdtr, 0, sizeof(*pIdtr)); }
uint16_t ASMGetIdtrLimit(void) RT_NOTHROW_DEF { return 0; }
void ASMSetIDTR(const RTIDTR RT_FAR *pIdtr) RT_NOTHROW_DEF { RT_NOREF(pIdtr); }
void ASMGetGDTR(PRTGDTR pGdtr) RT_NOTHROW_DEF { memset(pGdtr, 0, sizeof(*pGdtr)); }
void ASMSetGDTR(const RTGDTR RT_FAR *pGdtr) RT_NOTHROW_DEF { RT_NOREF(pGdtr); }

void ASMInvalidatePage(RTCCUINTXREG uPtr) RT_NOTHROW_DEF { RT_NOREF(uPtr); }
void ASMWriteBackAndInvalidateCaches(void) RT_NOTHROW_DEF {}
void ASMInvalidateInternalCaches(void) RT_NOTHROW_DEF {}


/*************************************************************************
 * x86 I/O port stubs (for host-side — guest I/O is emulated by IEM)
 *************************************************************************/

void    ASMOutU8(RTIOPORT Port, uint8_t u8) RT_NOTHROW_DEF { RT_NOREF(Port, u8); }
uint8_t ASMInU8(RTIOPORT Port) RT_NOTHROW_DEF { RT_NOREF(Port); return 0xFF; }
void    ASMOutU16(RTIOPORT Port, uint16_t u16) RT_NOTHROW_DEF { RT_NOREF(Port, u16); }
uint16_t ASMInU16(RTIOPORT Port) RT_NOTHROW_DEF { RT_NOREF(Port); return 0xFFFF; }
void    ASMOutU32(RTIOPORT Port, uint32_t u32) RT_NOTHROW_DEF { RT_NOREF(Port, u32); }
uint32_t ASMInU32(RTIOPORT Port) RT_NOTHROW_DEF { RT_NOREF(Port); return 0xFFFFFFFF; }
void ASMOutStrU8(RTIOPORT Port, uint8_t const RT_FAR *pau8, size_t c) RT_NOTHROW_DEF { RT_NOREF(Port, pau8, c); }
void ASMInStrU8(RTIOPORT Port, uint8_t RT_FAR *pau8, size_t c) RT_NOTHROW_DEF { RT_NOREF(Port, pau8, c); }
void ASMOutStrU16(RTIOPORT Port, uint16_t const RT_FAR *pau16, size_t c) RT_NOTHROW_DEF { RT_NOREF(Port, pau16, c); }
void ASMInStrU16(RTIOPORT Port, uint16_t RT_FAR *pau16, size_t c) RT_NOTHROW_DEF { RT_NOREF(Port, pau16, c); }
void ASMOutStrU32(RTIOPORT Port, uint32_t const RT_FAR *pau32, size_t c) RT_NOTHROW_DEF { RT_NOREF(Port, pau32, c); }
void ASMInStrU32(RTIOPORT Port, uint32_t RT_FAR *pau32, size_t c) RT_NOTHROW_DEF { RT_NOREF(Port, pau32, c); }


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
 * RTTimer — pthread-based periodic timer for Wasm
 *************************************************************************/

#include <iprt/thread.h>
#include <pthread.h>

/** Minimal RTTIMER structure — the real one is opaque. */
typedef struct RTTIMERINT
{
    PFNRTTIMER      pfnTimer;
    void           *pvUser;
    uint64_t        u64NanoInterval;
    pthread_t       hThread;
    volatile bool   fRunning;
    volatile bool   fActive;
} RTTIMERINT;

static void *rtTimerThread(void *pvArg)
{
    RTTIMERINT *pThis = (RTTIMERINT *)pvArg;
    while (pThis->fRunning)
    {
        if (pThis->fActive)
            pThis->pfnTimer((PRTTIMER)pThis, pThis->pvUser, 0);

        /* Sleep for the interval (milliseconds) */
        uint64_t ms = pThis->u64NanoInterval / RT_NS_1MS;
        if (ms < 1) ms = 1;
        struct timespec ts;
        ts.tv_sec = (time_t)(ms / 1000);
        ts.tv_nsec = (long)((ms % 1000) * 1000000);
        nanosleep(&ts, NULL);
    }
    return NULL;
}

RTDECL(int) RTTimerCreate(PRTTIMER *ppTimer, unsigned uMilliesInterval, PFNRTTIMER pfnTimer, void *pvUser)
{
    RTTIMERINT *pThis = (RTTIMERINT *)RTMemAllocZ(sizeof(*pThis));
    if (!pThis)
        return VERR_NO_MEMORY;
    pThis->pfnTimer = pfnTimer;
    pThis->pvUser = pvUser;
    pThis->u64NanoInterval = (uint64_t)uMilliesInterval * RT_NS_1MS;
    pThis->fRunning = true;
    pThis->fActive = true;
    if (pthread_create(&pThis->hThread, NULL, rtTimerThread, pThis) != 0)
    {
        RTMemFree(pThis);
        STUB_NOT_SUPPORTED("RTTimerCreate");
        return VERR_NOT_SUPPORTED;
    }
    *ppTimer = (PRTTIMER)pThis;
    return VINF_SUCCESS;
}

RTDECL(int) RTTimerCreateEx(PRTTIMER *ppTimer, uint64_t u64NanoInterval, uint32_t fFlags,
                             PFNRTTIMER pfnTimer, void *pvUser)
{
    RT_NOREF(fFlags);
    RTTIMERINT *pThis = (RTTIMERINT *)RTMemAllocZ(sizeof(*pThis));
    if (!pThis)
        return VERR_NO_MEMORY;
    pThis->pfnTimer = pfnTimer;
    pThis->pvUser = pvUser;
    pThis->u64NanoInterval = u64NanoInterval > 0 ? u64NanoInterval : RT_NS_1MS;
    pThis->fRunning = true;
    pThis->fActive = false; /* RTTimerStart activates */
    if (pthread_create(&pThis->hThread, NULL, rtTimerThread, pThis) != 0)
    {
        RTMemFree(pThis);
        STUB_NOT_SUPPORTED("RTTimerCreateEx");
        return VERR_NOT_SUPPORTED;
    }
    *ppTimer = (PRTTIMER)pThis;
    return VINF_SUCCESS;
}

RTDECL(int) RTTimerDestroy(PRTTIMER pTimer)
{
    if (!pTimer)
        return VINF_SUCCESS;
    RTTIMERINT *pThis = (RTTIMERINT *)pTimer;
    pThis->fRunning = false;
    pThis->fActive = false;
    pthread_join(pThis->hThread, NULL);
    RTMemFree(pThis);
    return VINF_SUCCESS;
}

RTDECL(int) RTTimerStart(PRTTIMER pTimer, uint64_t u64First)
{
    RT_NOREF(u64First);
    if (!pTimer)
        return VERR_INVALID_HANDLE;
    RTTIMERINT *pThis = (RTTIMERINT *)pTimer;
    pThis->fActive = true;
    return VINF_SUCCESS;
}

RTDECL(int) RTTimerStop(PRTTIMER pTimer)
{
    if (!pTimer)
        return VINF_SUCCESS;
    RTTIMERINT *pThis = (RTTIMERINT *)pTimer;
    pThis->fActive = false;
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
    STUB_NOT_SUPPORTED("RTRandAdvCreateSystemFaster");
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
    STUB_NOT_SUPPORTED("RTFileOpenEx");
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
    STUB_NOT_SUPPORTED("RTFileRead");
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTFileReadAt(RTFILE hFile, RTFOFF off, void *pvBuf, size_t cbToRead, size_t *pcbRead)
{
    RT_NOREF(hFile, off, pvBuf, cbToRead, pcbRead);
    STUB_NOT_SUPPORTED("RTFileReadAt");
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTFileWriteAt(RTFILE hFile, RTFOFF off, const void *pvBuf, size_t cbToWrite, size_t *pcbWritten)
{
    RT_NOREF(hFile, off, pvBuf, cbToWrite, pcbWritten);
    STUB_NOT_SUPPORTED("RTFileWriteAt");
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTFileSeek(RTFILE hFile, int64_t offSeek, unsigned uMethod, uint64_t *poffActual)
{
    RT_NOREF(hFile, offSeek, uMethod, poffActual);
    STUB_NOT_SUPPORTED("RTFileSeek");
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTFileQueryInfo(RTFILE hFile, PRTFSOBJINFO pObjInfo, RTFSOBJATTRADD enmAdditionalAttribs)
{
    RT_NOREF(hFile, pObjInfo, enmAdditionalAttribs);
    STUB_NOT_SUPPORTED("RTFileQueryInfo");
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTFileSetSize(RTFILE hFile, uint64_t cbSize)
{
    RT_NOREF(hFile, cbSize);
    STUB_NOT_SUPPORTED("RTFileSetSize");
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
    STUB_NOT_SUPPORTED("RTFileSetAllocationSize");
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
    STUB_NOT_SUPPORTED("RTFileMove");
    return VERR_NOT_SUPPORTED;
}

RTDECL(RTHCINTPTR) RTFileToNative(RTFILE hFile)
{
    return (RTHCINTPTR)hFile;
}

RTDECL(int) RTFileCopyPartPrep(PRTFILECOPYPARTBUFSTATE pBufState, uint64_t cbToCopy)
{
    RT_NOREF(pBufState, cbToCopy);
    STUB_NOT_SUPPORTED("RTFileCopyPartPrep");
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTFileCopyPartEx(RTFILE hFileSrc, RTFOFF offSrc, RTFILE hFileDst, RTFOFF offDst,
                              uint64_t cbToCopy, uint32_t fFlags, PRTFILECOPYPARTBUFSTATE pBufState,
                              uint64_t *pcbCopied)
{
    RT_NOREF(hFileSrc, offSrc, hFileDst, offDst, cbToCopy, fFlags, pBufState, pcbCopied);
    STUB_NOT_SUPPORTED("RTFileCopyPartEx");
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
    STUB_NOT_SUPPORTED("RTDirCreate");
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
    STUB_NOT_SUPPORTED("RTDirQueryInfo");
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
 * RTTls — pthread_key based (thread-safe, needed for EMT threads)
 *************************************************************************/

RTDECL(int) RTTlsAllocEx(PRTTLS piTls, PFNRTTLSDTOR pfnDestructor)
{
    pthread_key_t key;
    int rc = pthread_key_create(&key, (void (*)(void *))pfnDestructor);
    if (rc != 0)
        return RTErrConvertFromErrno(rc);
    *piTls = (RTTLS)key;
    return VINF_SUCCESS;
}

RTDECL(int) RTTlsFree(RTTLS iTls)
{
    RT_NOREF(iTls);
    return VINF_SUCCESS;
}

RTDECL(void *) RTTlsGet(RTTLS iTls)
{
    return pthread_getspecific((pthread_key_t)iTls);
}

RTDECL(int) RTTlsGetEx(RTTLS iTls, void **ppvValue)
{
    if (RT_UNLIKELY(iTls == NIL_RTTLS))
        return VERR_INVALID_PARAMETER;
    *ppvValue = pthread_getspecific((pthread_key_t)iTls);
    return VINF_SUCCESS;
}

RTDECL(int) RTTlsSet(RTTLS iTls, void *pvValue)
{
    int rc = pthread_setspecific((pthread_key_t)iTls, pvValue);
    if (RT_UNLIKELY(rc != 0))
        return RTErrConvertFromErrno(rc);
    return VINF_SUCCESS;
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
    STUB_NOT_SUPPORTED("RTProcCreate");
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTProcWait(RTPROCESS hProcess, unsigned fFlags, PRTPROCSTATUS pProcStatus)
{
    RT_NOREF(hProcess, fFlags, pProcStatus);
    STUB_NOT_SUPPORTED("RTProcWait");
    return VERR_NOT_SUPPORTED;
}


/*************************************************************************
 * RTDirRel stubs (used by VD)
 *************************************************************************/

RTDECL(int) RTDirRelDirCreate(RTDIR hDir, const char *pszRelPath, RTFMODE fMode, uint32_t fFlags, RTDIR *phSubDir)
{
    RT_NOREF(hDir, pszRelPath, fMode, fFlags, phSubDir);
    STUB_NOT_SUPPORTED("RTDirRelDirCreate");
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTDirRelDirOpenFiltered(RTDIR hDir, const char *pszFilter, RTDIRFILTER enmFilter,
                                     uint32_t fFlags, RTDIR *phSubDir)
{
    RT_NOREF(hDir, pszFilter, enmFilter, fFlags, phSubDir);
    STUB_NOT_SUPPORTED("RTDirRelDirOpenFiltered");
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTDirRelDirRemove(RTDIR hDir, const char *pszRelPath)
{
    RT_NOREF(hDir, pszRelPath);
    STUB_NOT_SUPPORTED("RTDirRelDirRemove");
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTDirRelFileOpen(RTDIR hDir, const char *pszRelFilename, uint64_t fOpen, PRTFILE phFile)
{
    RT_NOREF(hDir, pszRelFilename, fOpen, phFile);
    STUB_NOT_SUPPORTED("RTDirRelFileOpen");
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
    STUB_NOT_SUPPORTED("RTDirRelPathRename");
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTDirRelPathSetOwner(RTDIR hDir, const char *pszRelPath, uint32_t uid, uint32_t gid, uint32_t fFlags)
{
    RT_NOREF(hDir, pszRelPath, uid, gid, fFlags);
    STUB_NOT_SUPPORTED("RTDirRelPathSetOwner");
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTDirRelPathUnlink(RTDIR hDir, const char *pszRelPath, uint32_t fUnlink)
{
    RT_NOREF(hDir, pszRelPath, fUnlink);
    STUB_NOT_SUPPORTED("RTDirRelPathUnlink");
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTDirRelSymlinkCreate(RTDIR hDir, const char *pszSymlink, const char *pszTarget,
                                    RTSYMLINKTYPE enmType, uint32_t fCreate)
{
    RT_NOREF(hDir, pszSymlink, pszTarget, enmType, fCreate);
    STUB_NOT_SUPPORTED("RTDirRelSymlinkCreate");
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTDirRelSymlinkRead(RTDIR hDir, const char *pszSymlink, char *pszTarget, size_t cbTarget, uint32_t fRead)
{
    RT_NOREF(hDir, pszSymlink, pszTarget, cbTarget, fRead);
    STUB_NOT_SUPPORTED("RTDirRelSymlinkRead");
    return VERR_NOT_SUPPORTED;
}


/*************************************************************************
 * rtMemPageNative stubs — used by RTMemPageAlloc
 *************************************************************************/

int rtMemPageNativeAlloc(size_t cb, uint32_t fFlags, void **ppv)
{
    RT_NOREF(fFlags);
    /* Must be page-aligned — VM_IS_VALID_EXT checks RT_VALID_ALIGNED_PTR(pVM, PAGE_SIZE). */
    size_t cbAligned = (cb + 4095) & ~(size_t)4095;
    *ppv = aligned_alloc(4096, cbAligned);
    if (*ppv)
    {
        memset(*ppv, 0, cbAligned);
        return VINF_SUCCESS;
    }
    return VERR_NO_MEMORY;
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
    STUB_NOT_SUPPORTED("rtldrNativeLoad");
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
    STUB_NOT_SUPPORTED("rtDirNativeOpen");
    return VERR_NOT_SUPPORTED;
}


/*************************************************************************
 * RTCr* crypto stubs
 *************************************************************************/

RTDECL(int) RTCrCipherOpenByType(void **phCipher, uint32_t enmType, uint32_t fFlags)
{
    RT_NOREF(phCipher, enmType, fFlags);
    STUB_NOT_SUPPORTED("RTCrCipherOpenByType");
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
    STUB_NOT_SUPPORTED("RTCrCipherEncrypt");
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
    STUB_NOT_SUPPORTED("RTCrCipherDecrypt");
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTCrPkcs5Pbkdf2Hmac(void *pvOutput, size_t cbOutput, const void *pvInput, size_t cbInput,
                                  const void *pvSalt, size_t cbSalt, uint32_t cIterations, uint32_t enmDigestType)
{
    RT_NOREF(pvOutput, cbOutput, pvInput, cbInput, pvSalt, cbSalt, cIterations, enmDigestType);
    STUB_NOT_SUPPORTED("RTCrPkcs5Pbkdf2Hmac");
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


/*************************************************************************
 * TLS implementation — tls-posix.cpp was excluded from the build
 *
 * Uses pthread_key as the underlying TLS mechanism (same as tls-posix.cpp).
 * RTTlsAllocEx is in the in-tree wasm-stubs but RTTlsSet/Get/Free are missing.
 *************************************************************************/

/*************************************************************************
 * Thread implementation — overrides the stubs in RuntimeR3/VBoxVMM.so
 *
 * These MUST be in the top-level .cpp (linked as .o before .so/.a)
 * so that --allow-multiple-definition picks them up first.
 *************************************************************************/

#include "internal/thread.h"
#include <iprt/semaphore.h>
#include <iprt/asm.h>
#include <sched.h>

/** TLS key for storing the PRTTHREADINT pointer for the current thread. */
static pthread_key_t g_WasmSelfKey;
static bool g_fWasmSelfKeyInit = false;

/**
 * Thread entry point wrapper. Sets up TLS and calls rtThreadMain().
 */
static void *wasmThreadNativeMain(void *pvArgs)
{
    PRTTHREADINT pThread = (PRTTHREADINT)pvArgs;
    pthread_t Self = pthread_self();

    RTPrintf("[WASM-THREAD] wasmThreadNativeMain entered: name=%s self=%p\n", pThread->szName, (void*)Self);

    if (g_fWasmSelfKeyInit)
        pthread_setspecific(g_WasmSelfKey, pThread);

    RTPrintf("[WASM-THREAD] calling rtThreadMain for '%s'\n", pThread->szName);
    int rc = rtThreadMain(pThread, (uintptr_t)Self, &pThread->szName[0]);
    RTPrintf("[WASM-THREAD] rtThreadMain returned rc=%d for '%s'\n", rc, pThread->szName);

    if (g_fWasmSelfKeyInit)
        pthread_setspecific(g_WasmSelfKey, NULL);

    return (void *)(intptr_t)rc;
}

DECLHIDDEN(int) rtThreadNativeInit(void)
{
    int rc = pthread_key_create(&g_WasmSelfKey, NULL);
    if (rc)
        return VERR_NO_TLS_FOR_SELF;
    g_fWasmSelfKeyInit = true;
    return VINF_SUCCESS;
}

RTDECL(RTNATIVETHREAD) RTThreadNativeSelf(void)
{
    return (RTNATIVETHREAD)pthread_self();
}

DECLHIDDEN(int) rtThreadNativeSetPriority(PRTTHREADINT pThread, RTTHREADTYPE enmType)
{
    RT_NOREF(pThread, enmType);
    return VINF_SUCCESS;
}

DECLHIDDEN(int) rtThreadNativeAdopt(PRTTHREADINT pThread)
{
    if (g_fWasmSelfKeyInit)
    {
        int rc = pthread_setspecific(g_WasmSelfKey, pThread);
        if (!rc)
            return VINF_SUCCESS;
        return VERR_FAILED_TO_SET_SELF_TLS;
    }
    return VINF_SUCCESS;
}

DECLHIDDEN(void) rtThreadNativeWaitKludge(PRTTHREADINT pThread)
{
    RT_NOREF_PV(pThread);
}

DECLHIDDEN(void) rtThreadNativeDestroy(PRTTHREADINT pThread)
{
    if (g_fWasmSelfKeyInit && pThread == (PRTTHREADINT)pthread_getspecific(g_WasmSelfKey))
        pthread_setspecific(g_WasmSelfKey, NULL);
}

DECLHIDDEN(int) rtThreadNativeCreate(PRTTHREADINT pThreadInt, PRTNATIVETHREAD pNativeThread)
{
    RTPrintf("[WASM-THREAD] rtThreadNativeCreate: name=%s cbStack=%zu\n", pThreadInt->szName, pThreadInt->cbStack);

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
                rc = pthread_create(&ThreadId, &ThreadAttr, wasmThreadNativeMain, pThreadInt);
                RTPrintf("[WASM-THREAD] pthread_create for '%s': rc=%d id=%p\n", pThreadInt->szName, rc, (void*)(uintptr_t)ThreadId);
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
    RTPrintf("[WASM-THREAD] rtThreadNativeCreate FAILED for '%s': errno=%d\n", pThreadInt->szName, rc);
    return RTErrConvertFromErrno(rc);
}

RTDECL(RTTHREAD) RTThreadSelf(void)
{
    if (g_fWasmSelfKeyInit)
        return (RTTHREAD)pthread_getspecific(g_WasmSelfKey);
    return NIL_RTTHREAD;
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

RTDECL(bool) RTThreadYield(void)
{
    sched_yield();
    return true;
}


/*************************************************************************
 * Linux-specific stubs — not available in Wasm/Emscripten
 *************************************************************************/

#include <iprt/linux/sysfs.h>

RTDECL(int) RTLinuxSysFsReadIntFile(unsigned uBase, int64_t *pi64, const char *pszFormat, ...)
{
    RT_NOREF(uBase, pszFormat);
    *pi64 = 0;
    return VERR_FILE_NOT_FOUND;
}

RTDECL(int) RTLinuxSysFsReadDevNumFile(dev_t *pDevNum, const char *pszFormat, ...)
{
    RT_NOREF(pszFormat);
    *pDevNum = 0;
    return VERR_FILE_NOT_FOUND;
}

RTDECL(int) RTLinuxConstructPath(char *pszPath, size_t cbPath, const char *pszFormat, ...)
{
    RT_NOREF(pszFormat);
    if (cbPath) pszPath[0] = '\0';
    return VERR_FILE_NOT_FOUND;
}


/*************************************************************************
 * Environment stubs
 *************************************************************************/

#include <iprt/env.h>

RTDECL(bool) RTEnvExist(const char *pszVar)
{
    RT_NOREF(pszVar);
    return false;
}

RTDECL(bool) RTEnvExistEx(RTENV Env, const char *pszVar)
{
    RT_NOREF(Env, pszVar);
    return false;
}


/*************************************************************************
 * Pipe stubs — not needed in Wasm
 *************************************************************************/

#include <iprt/pipe.h>

RTDECL(int) RTPipeCreate(PRTPIPE phPipeRead, PRTPIPE phPipeWrite, uint32_t fFlags)
{
    RT_NOREF(phPipeRead, phPipeWrite, fFlags);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTPipeClose(RTPIPE hPipe)
{
    RT_NOREF(hPipe);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTPipeFromNative(PRTPIPE phPipe, RTHCINTPTR hNativePipe, uint32_t fFlags)
{
    RT_NOREF(phPipe, hNativePipe, fFlags);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTPipeRead(RTPIPE hPipe, void *pvBuf, size_t cbToRead, size_t *pcbRead)
{
    RT_NOREF(hPipe, pvBuf, cbToRead, pcbRead);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTPipeReadBlocking(RTPIPE hPipe, void *pvBuf, size_t cbToRead, size_t *pcbRead)
{
    RT_NOREF(hPipe, pvBuf, cbToRead, pcbRead);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTPipeWrite(RTPIPE hPipe, const void *pvBuf, size_t cbToWrite, size_t *pcbWritten)
{
    RT_NOREF(hPipe, pvBuf, cbToWrite, pcbWritten);
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTPipeWriteBlocking(RTPIPE hPipe, const void *pvBuf, size_t cbToWrite, size_t *pcbWritten)
{
    RT_NOREF(hPipe, pvBuf, cbToWrite, pcbWritten);
    return VERR_NOT_SUPPORTED;
}

RTDECL(uint32_t) rtPipePollGetHandle(RTPIPE hPipe, uint32_t fEvents)
{
    RT_NOREF(hPipe, fEvents);
    return UINT32_MAX;
}


/*************************************************************************
 * System query stubs
 *************************************************************************/

#include <iprt/system.h>

RTDECL(int) RTSystemQueryDmiString(RTSYSDMISTR enmString, char *pszBuf, size_t cbBuf)
{
    RT_NOREF(enmString);
    if (cbBuf) pszBuf[0] = '\0';
    return VERR_NOT_SUPPORTED;
}

RTDECL(int) RTSystemQueryOSInfo(RTSYSOSINFO enmInfo, char *pszInfo, size_t cchInfo)
{
    RT_NOREF(enmInfo);
    if (cchInfo) pszInfo[0] = '\0';
    return VERR_NOT_SUPPORTED;
}


/*************************************************************************
 * Time stubs
 *************************************************************************/

#include <iprt/time.h>

RTDECL(PRTTIME) RTTimeLocalExplode(PRTTIME pTime, PCRTTIMESPEC pTimeSpec)
{
    /* Fall back to UTC */
    return RTTimeExplode(pTime, pTimeSpec);
}


/*************************************************************************
 * UUID stubs
 *************************************************************************/

#include <iprt/uuid.h>

RTDECL(int) RTUuidCompare2Strs(const char *pszUuid1, const char *pszUuid2)
{
    return RTStrCmp(pszUuid1, pszUuid2);
}


/* rtDirNativeOpen/rtDirNativeGetStructSize already defined above */
