/* $Id: IEMAll.cpp 112403 2026-01-11 19:29:08Z knut.osmundsen@oracle.com $ */
/** @file
 * IEM - Interpreted Execution Manager - All Contexts.
 */

/*
 * Copyright (C) 2011-2026 Oracle and/or its affiliates.
 *
 * This file is part of VirtualBox base platform packages, as
 * available from https://www.virtualbox.org.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, in version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses>.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */


/** @page pg_iem    IEM - Interpreted Execution Manager
 *
 * The interpreted exeuction manager (IEM) is for executing short guest code
 * sequences that are causing too many exits / virtualization traps.  It will
 * also be used to interpret single instructions, thus replacing the selective
 * interpreters in EM and IOM.
 *
 * Design goals:
 *      - Relatively small footprint, although we favour speed and correctness
 *        over size.
 *      - Reasonably fast.
 *      - Correctly handle lock prefixed instructions.
 *      - Complete instruction set - eventually.
 *      - Refactorable into a recompiler, maybe.
 *      - Replace EMInterpret*.
 *
 * Using the existing disassembler has been considered, however this is thought
 * to conflict with speed as the disassembler chews things a bit too much while
 * leaving us with a somewhat complicated state to interpret afterwards.
 *
 *
 * The current code is very much work in progress. You've been warned!
 *
 *
 * @section sec_iem_fpu_instr   FPU Instructions
 *
 * On x86 and AMD64 hosts, the FPU instructions are implemented by executing the
 * same or equivalent instructions on the host FPU.  To make life easy, we also
 * let the FPU prioritize the unmasked exceptions for us.  This however, only
 * works reliably when CR0.NE is set, i.e. when using \#MF instead the IRQ 13
 * for FPU exception delivery, because with CR0.NE=0 there is a window where we
 * can trigger spurious FPU exceptions.
 *
 * The guest FPU state is not loaded into the host CPU and kept there till we
 * leave IEM because the calling conventions have declared an all year open
 * season on much of the FPU state.  For instance an innocent looking call to
 * memcpy might end up using a whole bunch of XMM or MM registers if the
 * particular implementation finds it worthwhile.
 *
 *
 * @section sec_iem_logging     Logging
 *
 * The IEM code uses the \"IEM\" log group for the main logging. The different
 * logging levels/flags are generally used for the following purposes:
 *      - Level 1  (Log)  : Errors, exceptions, interrupts and such major events.
 *      - Flow  (LogFlow) : Basic enter/exit IEM state info.
 *      - Level 2  (Log2) : ?
 *      - Level 3  (Log3) : More detailed enter/exit IEM state info.
 *      - Level 4  (Log4) : Decoding mnemonics w/ EIP.
 *      - Level 5  (Log5) : Decoding details.
 *      - Level 6  (Log6) : Enables/disables the lockstep comparison with REM.
 *      - Level 7  (Log7) : iret++ execution logging.
 *      - Level 8  (Log8) :
 *      - Level 9  (Log9) :
 *      - Level 10 (Log10): TLBs.
 *      - Level 11 (Log11): Unmasked FPU exceptions.
 *
 * The \"IEM_MEM\" log group covers most of memory related details logging,
 * except for errors and exceptions:
 *      - Level 1  (Log)  : Reads.
 *      - Level 2  (Log2) : Read fallbacks.
 *      - Level 3  (Log3) : MemMap read.
 *      - Level 4  (Log4) : MemMap read fallbacks.
 *      - Level 5  (Log5) : Writes
 *      - Level 6  (Log6) : Write fallbacks.
 *      - Level 7  (Log7) : MemMap writes and read-writes.
 *      - Level 8  (Log8) : MemMap write and read-write fallbacks.
 *      - Level 9  (Log9) : Stack reads.
 *      - Level 10 (Log10): Stack read fallbacks.
 *      - Level 11 (Log11): Stack writes.
 *      - Level 12 (Log12): Stack write fallbacks.
 *      - Flow  (LogFlow) :
 *
 * The SVM (AMD-V) and VMX (VT-x) code has the following assignments:
 *      - Level 1  (Log)  : Errors and other major events.
 *      - Flow (LogFlow)  : Misc flow stuff (cleanup?)
 *      - Level 2  (Log2) : VM exits.
 *
 * The syscall logging level assignments:
 *      - Level 1: DOS and BIOS.
 *      - Level 2: Windows 3.x
 *      - Level 3: Linux.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP   LOG_GROUP_IEM
#define VMCPU_INCL_CPUM_GST_CTX
#ifdef IN_RING0
# define VBOX_VMM_TARGET_X86
#endif
#include <VBox/vmm/iem.h>
#include <VBox/vmm/cpum.h>
#include <VBox/vmm/pdmapic.h>
#include <VBox/vmm/pdm.h>
#include <VBox/vmm/pgm.h>
#include <VBox/vmm/iom.h>
#include <VBox/vmm/em.h>
#include <VBox/vmm/hm.h>
#include <VBox/vmm/nem.h>
#include <VBox/vmm/gcm.h>
#include <VBox/vmm/gim.h>
#ifdef VBOX_WITH_NESTED_HWVIRT_SVM
# include <VBox/vmm/em.h>
# include <VBox/vmm/hm_svm.h>
#endif
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
# include <VBox/vmm/hmvmxinline.h>
#endif
#include <VBox/vmm/tm.h>
#include <VBox/vmm/cfgm.h>
#include <VBox/vmm/dbgf.h>
#include <VBox/vmm/dbgftrace.h>
#include "IEMInternal.h"
#include <VBox/vmm/vmcc.h>
#include <VBox/log.h>
#include <VBox/err.h>
#include <VBox/param.h>
#include <VBox/dis.h>
#include <iprt/asm-math.h>
#if defined(RT_ARCH_AMD64) || defined(RT_ARCH_X86)
# include <iprt/asm-amd64-x86.h>
#elif defined(RT_ARCH_ARM64) || defined(RT_ARCH_ARM32)
# include <iprt/asm-arm.h>
#endif
#include <iprt/assert.h>
#include <iprt/string.h>
#include <iprt/x86.h>
#ifdef __EMSCRIPTEN__
# include <iprt/time.h>
# include <iprt/stream.h>
#endif

#include "IEMInline.h"
#include "IEMInlineExec.h"
#ifdef VBOX_VMM_TARGET_X86
# include "target-x86/IEMInline-x86.h"
# include "target-x86/IEMInlineDecode-x86.h"
# include "target-x86/IEMInlineExec-x86.h"
#elif defined(VBOX_VMM_TARGET_ARMV8)
# include "target-armv8/IEMInline-armv8.h"
# include "target-armv8/IEMAllIntprA64Tables-armv8.h"
# include "target-armv8/IEMInlineDecode-armv8.h"
# include "target-armv8/IEMInlineExec-armv8.h"
#endif

#ifdef __EMSCRIPTEN__
# include <emscripten.h>
# include <iprt/mem.h>

/* Fast boot state globals — queried from wasm-main.cpp via wasmGetFBState() */
volatile uint32_t g_wasmFBCheckCount = 0;   /* number of FAST-BOOT-CHK checks performed */
volatile uint64_t g_wasmFBLastEIP = 0;      /* EIP during last check */
volatile uint32_t g_wasmFBLastHdrS1 = 0;    /* last 4 bytes read from 0x90202 (as uint32) */
volatile uint32_t g_wasmFBLastHdrS2 = 0;    /* last 4 bytes read from 0x10202 (as uint32) */
volatile uint16_t g_wasmFBBootCS = 0;       /* current CS during boot monitoring */
volatile uint8_t  g_wasmFBBootActive = 0;   /* 1 = boot monitoring active */
volatile uint8_t  g_wasmFBTriggered = 0;    /* 1 = fast boot triggered */

/** When true, skip heavy diagnostics (EARLY-RIP, TASK-WALK, etc.) for speed. */
static bool g_fProductionMode = true;

EM_JS(int, wasmJitExecBlock, (void *pCpumCtx, void *pvRAM, int maxInsn, void *pvHighRAM, int cbHighRAM, int fIrqPending), {
    if (typeof globalThis.VBoxJIT === 'undefined') return 0;
    if (!globalThis.VBoxJIT._initialized) {
        globalThis.VBoxJIT.init(wasmMemory);
        globalThis.VBoxJIT._initialized = true;
    }
    globalThis.VBoxJIT._irqPending = fIrqPending;
    return globalThis.VBoxJIT.execBlock(Number(pCpumCtx), Number(pvRAM), maxInsn, Number(pvHighRAM), cbHighRAM);
});

EM_JS(void, wasmJitSetA20, (int fA20), {
    if (typeof globalThis.VBoxJIT !== 'undefined')
        globalThis.VBoxJIT._a20 = fA20;
});

EM_JS(void, wasmJitLog, (const char *pszMsg), {
    console.log('[JIT-C] ' + UTF8ToString(Number(pszMsg)));
});

EM_JS(void, wasmJitSetRomBuffer, (void *pvROM, int cbROM, int uGCPhysStart), {
    console.log('[JIT] setRomBuffer: ptr=0x' + Number(pvROM).toString(16) + ' size=' + cbROM + ' start=0x' + uGCPhysStart.toString(16));
    if (typeof globalThis.VBoxJIT !== 'undefined' && globalThis.VBoxJIT.setRomBuffer)
        globalThis.VBoxJIT.setRomBuffer(Number(pvROM), cbROM, uGCPhysStart);
});

/* Call JS-side kernel decompressor.  Reads setup header from guest 0x90000,
   decompresses bzImage payload via jsGunzip, loads ELF segments, builds page
   tables, and writes D64B metadata at guest 0x7200.  Returns 1 on success. */
EM_JS(int, wasmFastBootDecompress, (void), {
    if (typeof globalThis.VBoxJIT !== 'undefined' && globalThis.VBoxJIT.fastBootDecompress)
        return globalThis.VBoxJIT.fastBootDecompress();
    return 0;
});

static void    *s_pvJitRAM      = NULL;  /* NULL until PGMPhysGCPhys2CCPtr succeeds */
static void    *s_pvJitHighRAM  = NULL;  /* NULL until high RAM init succeeds */
static uint32_t s_cbJitHighRAM  = 0;     /* high RAM size in bytes */
static bool     s_fHighRAMTried = false; /* true once high RAM init has been attempted */
static bool     s_fJitRomDone   = false; /* true once ROM copy succeeded */
static uint32_t s_cJitRetries   = 0;     /* throttle counter for RAM init retries */
static uint32_t s_cRomRetries   = 0;     /* throttle counter for ROM copy retries */
static void    *s_pvJitRomBuf   = NULL;  /* ROM buffer pointer (for re-copy) */
static uint64_t s_cRomRefreshNext = 20000; /* next instruction milestone for ROM re-copy */

/* Defined in wasm-main.cpp — stores RAM base in shared Wasm memory for JS display */
extern "C" void wasmJitSetGuestRAM(void *pv);

/* Defined in wasm-kbd-drv.cpp — drain keyboard ring buffer on EMT thread */
extern "C" int wasmKbdDrainQueue(void);

/* Delay caller info — stored by EMT, readable from JS via Module._wasmGetDelay*() */
static volatile uint64_t s_aDelayStack[8];   /* return address chain from RSP */
static volatile uint64_t s_uDelayInfoRip = 0;
static volatile uint64_t s_uDelayInfoRsp = 0;
static volatile uint64_t s_uDelayInfoRbp = 0;
static volatile uint32_t s_fDelayInfoValid = 0;

extern "C" {
    /* Return delay caller info as double (avoids BigInt serialization issues in JS).
     * These are EMSCRIPTEN_KEEPALIVE so they appear as Module._wasmGetDelay*() */
    EMSCRIPTEN_KEEPALIVE double wasmGetDelayRip(void)   { return (double)s_uDelayInfoRip; }
    EMSCRIPTEN_KEEPALIVE double wasmGetDelayRsp(void)   { return (double)s_uDelayInfoRsp; }
    EMSCRIPTEN_KEEPALIVE double wasmGetDelayRbp(void)   { return (double)s_uDelayInfoRbp; }
    EMSCRIPTEN_KEEPALIVE double wasmGetDelayRet(int n)  { return (double)s_aDelayStack[n & 7]; }
    EMSCRIPTEN_KEEPALIVE int    wasmGetDelayValid(void) { return s_fDelayInfoValid; }
}

/* ── Guest CPUID & physical memory diagnostics ── */
static volatile uint32_t s_fCpuidDumped = 0;
static PVMCC s_pVMForRead = NULL;
static uint8_t s_abGuestReadBuf[4096];

static void wasmDumpGuestCpuid(PVMCPUCC pVCpu)
{
    if (s_fCpuidDumped)
        return;
    s_fCpuidDumped = 1;
    s_pVMForRead = pVCpu->CTX_SUFF(pVM);

    uint32_t eax, ebx, ecx, edx;

    /* CPUID leaf 0 — vendor */
    CPUMGetGuestCpuId(pVCpu, 0, 0, -1, &eax, &ebx, &ecx, &edx);
    char szVendor[13];
    *(uint32_t *)&szVendor[0] = ebx;
    *(uint32_t *)&szVendor[4] = edx;
    *(uint32_t *)&szVendor[8] = ecx;
    szVendor[12] = '\0';
    RTPrintf("[CPUID-DIAG] Leaf 0: maxLeaf=%u vendor='%s'\n", eax, szVendor);

    /* CPUID leaf 1 — features */
    CPUMGetGuestCpuId(pVCpu, 1, 0, -1, &eax, &ebx, &ecx, &edx);
    RTPrintf("[CPUID-DIAG] Leaf 1: EAX=%#x EBX=%#x ECX=%#x EDX=%#x\n", eax, ebx, ecx, edx);
    RTPrintf("[CPUID-DIAG]   PSE=%d PAE=%d PGE=%d SSE2=%d FXSR=%d\n",
             !!(edx & RT_BIT(3)), !!(edx & RT_BIT(6)), !!(edx & RT_BIT(13)),
             !!(edx & RT_BIT(26)), !!(edx & RT_BIT(24)));

    /* CPUID leaf 0x80000001 — extended features */
    CPUMGetGuestCpuId(pVCpu, 0x80000001, 0, -1, &eax, &ebx, &ecx, &edx);
    RTPrintf("[CPUID-DIAG] Leaf 80000001h: EAX=%#x EDX=%#x\n", eax, edx);
    RTPrintf("[CPUID-DIAG]   NX=%d LM=%d 1GB-pages=%d\n",
             !!(edx & RT_BIT(20)), !!(edx & RT_BIT(29)), !!(edx & RT_BIT(26)));

    /* CPUID leaf 0x80000008 — address widths */
    CPUMGetGuestCpuId(pVCpu, 0x80000008, 0, -1, &eax, &ebx, &ecx, &edx);
    RTPrintf("[CPUID-DIAG] Leaf 80000008h: phys=%u virt=%u\n",
             eax & 0xff, (eax >> 8) & 0xff);

    /* CR4 — check PSE/PAE/PGE bits */
    RTPrintf("[CPUID-DIAG] CR4=%#llx  (PSE=%d PAE=%d PGE=%d)\n",
             (unsigned long long)pVCpu->cpum.GstCtx.cr4,
             !!(pVCpu->cpum.GstCtx.cr4 & RT_BIT(4)),
             !!(pVCpu->cpum.GstCtx.cr4 & RT_BIT(5)),
             !!(pVCpu->cpum.GstCtx.cr4 & RT_BIT(7)));

    /* ── IVT check: where does INT 15h point? ── */
    {
        uint8_t abIvt[4];
        RT_ZERO(abIvt);
        PGMPhysRead(pVCpu->CTX_SUFF(pVM), 0x54, abIvt, 4, PGMACCESSORIGIN_DEBUGGER);
        uint16_t off = *(uint16_t *)&abIvt[0];
        uint16_t seg = *(uint16_t *)&abIvt[2];
        RTPrintf("[IVT-DIAG] INT 15h vector = %04x:%04x (linear %#x)\n",
                 seg, off, ((uint32_t)seg << 4) + off);
    }

    /* ── BDA: base memory size ── */
    {
        uint8_t abBda[2];
        RT_ZERO(abBda);
        PGMPhysRead(pVCpu->CTX_SUFF(pVM), 0x413, abBda, 2, PGMACCESSORIGIN_DEBUGGER);
        uint16_t cbBaseMem = *(uint16_t *)abBda;
        RTPrintf("[BDA-DIAG] Base memory = %u KB\n", cbBaseMem);
    }

    /* ── PGM physical memory accessibility check ── */
    {
        static const RTGCPHYS aTestAddrs[] = {
            0x100000, 0xA00000, 0x3200000, 0x6400000, 0x7F00000
        };
        for (unsigned t = 0; t < RT_ELEMENTS(aTestAddrs); t++)
        {
            uint8_t abTest[4];
            int rc = PGMPhysRead(pVCpu->CTX_SUFF(pVM), aTestAddrs[t], abTest, 4, PGMACCESSORIGIN_DEBUGGER);
            RTPrintf("[PGM-DIAG] Read phys %#llx: rc=%d data=%02x%02x%02x%02x\n",
                     (unsigned long long)aTestAddrs[t], rc,
                     abTest[0], abTest[1], abTest[2], abTest[3]);
        }
    }

    /* ── E820 scan: check boot_params at various physical addresses ── */
    uint8_t abBuf[32];
    for (uint32_t base = 0x10000; base <= 0x90000; base += 0x10000)
    {
        RT_ZERO(abBuf);
        PGMPhysRead(pVCpu->CTX_SUFF(pVM), (RTGCPHYS)base + 0x1E8, abBuf, 4, PGMACCESSORIGIN_DEBUGGER);
        uint32_t e820Count = *(uint32_t *)abBuf;
        if (e820Count > 0 && e820Count <= 128)
        {
            RTPrintf("[E820-DIAG] Candidate boot_params at phys 0x%x: e820_entries=%u\n",
                     base, e820Count);
            uint8_t abE820[160]; /* 8 entries * 20 bytes */
            RT_ZERO(abE820);
            unsigned cbRead = RT_MIN(e820Count * 20, sizeof(abE820));
            PGMPhysRead(pVCpu->CTX_SUFF(pVM), (RTGCPHYS)base + 0x2D0, abE820, cbRead, PGMACCESSORIGIN_DEBUGGER);
            for (unsigned e = 0; e < 8 && e < e820Count; e++)
            {
                uint64_t addr = *(uint64_t *)&abE820[e * 20];
                uint64_t size = *(uint64_t *)&abE820[e * 20 + 8];
                uint32_t type = *(uint32_t *)&abE820[e * 20 + 16];
                RTPrintf("[E820-DIAG]   [%u] addr=%#llx size=%#llx type=%u\n",
                         e, (unsigned long long)addr, (unsigned long long)size, type);
            }
        }
    }

    /* ── Also try to find E820 via kernel's internal structure.
     * The kernel's __e820_table is at an unknown virtual address, but we can
     * search physical memory near the kernel base for the E820 signature pattern:
     * a usable entry starting at 0 with size ~0x9FC00 followed by reserved at 0x9FC00. ── */
    {
        PVMCC pVM = pVCpu->CTX_SUFF(pVM);
        /* Scan from 1MB to 32MB in 4KB steps, looking for the E820 pattern */
        for (RTGCPHYS scan = 0x100000; scan < 0x2000000; scan += 0x1000)
        {
            uint8_t abScan[40]; /* 2 E820 entries */
            RT_ZERO(abScan);
            int rc = PGMPhysRead(pVM, scan, abScan, sizeof(abScan), PGMACCESSORIGIN_DEBUGGER);
            if (RT_FAILURE(rc))
                continue;
            uint64_t a0 = *(uint64_t *)&abScan[0];  /* first entry addr */
            uint64_t s0 = *(uint64_t *)&abScan[8];  /* first entry size */
            uint32_t t0 = *(uint32_t *)&abScan[16]; /* first entry type */
            uint64_t a1 = *(uint64_t *)&abScan[20]; /* second entry addr */
            uint32_t t1 = *(uint32_t *)&abScan[36]; /* second entry type */
            /* Pattern: entry0 = {addr=0, size=0x9FC00, type=1}, entry1 = {addr=0x9FC00, type=2} */
            if (a0 == 0 && s0 == 0x9FC00 && t0 == 1 && a1 == 0x9FC00 && t1 == 2)
            {
                RTPrintf("[E820-SCAN] Found E820 table at phys %#llx!\n",
                         (unsigned long long)scan);
                /* Dump up to 8 entries */
                uint8_t abFull[160];
                RT_ZERO(abFull);
                PGMPhysRead(pVM, scan, abFull, sizeof(abFull), PGMACCESSORIGIN_DEBUGGER);
                for (unsigned e = 0; e < 8; e++)
                {
                    uint64_t addr = *(uint64_t *)&abFull[e * 20];
                    uint64_t size = *(uint64_t *)&abFull[e * 20 + 8];
                    uint32_t type = *(uint32_t *)&abFull[e * 20 + 16];
                    if (addr == 0 && size == 0 && type == 0)
                        break;
                    RTPrintf("[E820-SCAN]   [%u] addr=%#llx size=%#llx type=%u\n",
                             e, (unsigned long long)addr, (unsigned long long)size, type);
                }
                break; /* found it, stop scanning */
            }
        }
    }

    RTStrmFlush(g_pStdOut);
}

/* Guest physical memory read — callable from JS via Module._wasmReadGuestPhys() */
extern "C" {
    EMSCRIPTEN_KEEPALIVE int wasmReadGuestPhys(double dGCPhys, int cb)
    {
        RTGCPHYS GCPhys = (RTGCPHYS)(uint64_t)dGCPhys;
        if (cb <= 0 || cb > (int)sizeof(s_abGuestReadBuf))
            cb = sizeof(s_abGuestReadBuf);
        RT_ZERO(s_abGuestReadBuf);
        if (!s_pVMForRead)
            return -1;
        int rc = PGMPhysRead(s_pVMForRead, GCPhys, s_abGuestReadBuf, cb, PGMACCESSORIGIN_DEBUGGER);
        return rc;
    }
    EMSCRIPTEN_KEEPALIVE double wasmGetGuestReadByte(int off)
    {
        if (off < 0 || off >= (int)sizeof(s_abGuestReadBuf))
            return -1;
        return (double)s_abGuestReadBuf[off];
    }
}

static void iemJitEnsureInit(PVMCC pVM)
{
    /* Fast path: fully initialised (high RAM is optional — don't block on it) */
    if (RT_LIKELY(s_pvJitRAM && s_fHighRAMTried && s_fJitRomDone))
        return;

    /* ── Phase 1: get flat RAM pointer ── */
    if (!s_pvJitRAM)
    {
        /* Throttle retries: try on call #1, then every 4096th call thereafter */
        ++s_cJitRetries;
        if (s_cJitRetries != 1 && (s_cJitRetries & 0xFFF) != 0)
            return;

        /* Try several physical addresses in case page 0 is not yet faulted in.
         * The offset between the returned host pointer and GCPhys is constant
         * for the flat guest RAM mapping. */
        static const RTGCPHYS s_aGCPhysTry[] = { 0x1000, 0x400, 0, 0x7000 };
        for (unsigned iTry = 0; iTry < RT_ELEMENTS(s_aGCPhysTry); iTry++)
        {
            PGMPAGEMAPLOCK Lock;
            void *pv = NULL;
            int rc = PGMPhysGCPhys2CCPtr(pVM, s_aGCPhysTry[iTry], &pv, &Lock);
            if (RT_SUCCESS(rc) && pv)
            {
                /* Compute base: host ptr at GCPhys X  →  base = pv - X */
                s_pvJitRAM = (uint8_t *)pv - (size_t)s_aGCPhysTry[iTry];
                wasmJitSetGuestRAM(s_pvJitRAM);
                char szMsg[128];
                RTStrPrintf(szMsg, sizeof(szMsg),
                            "RAM init OK: gcPhys=0x%x pv=%p base=%p attempt=%u",
                            (unsigned)s_aGCPhysTry[iTry], pv, s_pvJitRAM, s_cJitRetries);
                wasmJitLog(szMsg);

                /* Note: ramBase only covers the low 640K range (0-0x9FFFF).
                 * PGM allocates separate ranges for VGA hole and high RAM (>1MB),
                 * each with its own pbR3. The JIT bails to IEM for addresses
                 * >= 0xA0000 to handle this correctly. */
                /* Don't release the lock — we want the mapping to stay live */
                break;
            }
            else
            {
                char szMsg[80];
                RTStrPrintf(szMsg, sizeof(szMsg),
                            "RAM init fail: gcPhys=0x%x rc=%d attempt=%u",
                            (unsigned)s_aGCPhysTry[iTry], rc, s_cJitRetries);
                wasmJitLog(szMsg);
            }
        }
        if (!s_pvJitRAM)
            return; /* Will retry on next throttle tick */
    }

    /* ── Phase 1b: get high RAM pointer (>= 0x100000) ── */
    if (!s_fHighRAMTried && s_pvJitRAM)
    {
        s_fHighRAMTried = true; /* only try once — don't block fast path on failure */
        PGMPAGEMAPLOCK Lock;
        void *pv = NULL;
        int rc = PGMPhysGCPhys2CCPtr(pVM, 0x100000, &pv, &Lock);
        if (RT_SUCCESS(rc) && pv)
        {
            s_pvJitHighRAM = pv;
            /* Probe how far contiguous RAM extends above 1MB by trying to
               map a page near the configured RamSize. Read RamSize from CFGM. */
            uint64_t cbRamSize = 0;
            PCFGMNODE pRoot = CFGMR3GetRoot(pVM);
            if (pRoot)
                CFGMR3QueryU64Def(pRoot, "RamSize", &cbRamSize, 32U * _1M);
            if (cbRamSize > 0x100000)
                s_cbJitHighRAM = (uint32_t)(cbRamSize - 0x100000);
            else
                s_cbJitHighRAM = (32U * _1M) - 0x100000;
            RTPrintf("[JIT-INIT] High RAM: ptr=%p size=%u MB (RamSize=%llu MB)\n",
                     pv, s_cbJitHighRAM >> 20, (unsigned long long)(cbRamSize >> 20));
        }
    }

    /* ── Phase 2: copy ROM (0xC0000–0xFFFFF = 256 KB) ── */
    if (!s_fJitRomDone)
    {
        /* Throttle ROM retries: try every 4096th call to avoid spending too
         * much time on page reads if ROM isn't ready yet */
        ++s_cRomRetries;
        if (s_cRomRetries != 1 && (s_cRomRetries & 0xFFF) != 0)
            return;

        s_fJitRomDone = true; /* set before attempt; reset below if rejected */

        wasmJitLog("Copying ROM 0xC0000-0xFFFFF via PGMPhysRead...");
        const uint32_t cbROM     = 0x40000;  /* 256 KB */
        const uint32_t uROMStart = 0xC0000;
        void *pvROM = RTMemAllocZ(cbROM);
        if (!pvROM) { wasmJitLog("RTMemAllocZ failed for ROM buffer"); return; }

        uint32_t cPagesOK = 0, cPagesFail = 0;
        int lastFailRc = 0;
        for (uint32_t off = 0; off < cbROM; off += GUEST_PAGE_SIZE)
        {
            VBOXSTRICTRC rcPage = PGMPhysRead(pVM, (RTGCPHYS)(uROMStart + off),
                                              (uint8_t *)pvROM + off, GUEST_PAGE_SIZE,
                                              PGMACCESSORIGIN_IEM);
            if (RT_SUCCESS((int)rcPage)) cPagesOK++;
            else { cPagesFail++; lastFailRc = (int)rcPage; }
        }

        uint8_t *pb = (uint8_t *)pvROM;
        uint32_t cNonTrivial = 0;
        for (uint32_t i = 0; i < cbROM; i++)
            if (pb[i] != 0xFF && pb[i] != 0x00) cNonTrivial++;

        char szMsg[256];
        RTStrPrintf(szMsg, sizeof(szMsg),
                    "ROM: %u pages OK, %u fail (lastRc=%d) non-trivial=%u"
                    " vga[0:1]=0x%02x,0x%02x bios[F0000]=0x%02x",
                    cPagesOK, cPagesFail, lastFailRc, cNonTrivial,
                    pb[0], pb[1], pb[0x30000]);
        wasmJitLog(szMsg);

        if (cPagesOK > 0 && cNonTrivial > 100)
        {
            s_pvJitRomBuf = pvROM;
            wasmJitSetRomBuffer(pvROM, (int)cbROM, (int)uROMStart);
        }
        else
        {
            wasmJitLog("ROM buffer REJECTED: content looks empty");
            RTMemFree(pvROM);
            /* Allow re-attempt: reset the done flag so we try again later */
            s_fJitRomDone = false;
        }
    }
}
#endif /* __EMSCRIPTEN__ */


/**
 * Initializes the decoder state.
 *
 * iemReInitDecoder is mostly a copy of this function.
 *
 * @param   pVCpu               The cross context virtual CPU structure of the
 *                              calling thread.
 * @param   fExecOpts           Optional execution flags:
 *                                  - IEM_F_BYPASS_HANDLERS
 *                                  - IEM_F_X86_DISREGARD_LOCK
 */
DECLINLINE(void) iemInitDecoder(PVMCPUCC pVCpu, uint32_t fExecOpts)
{
    IEM_CTX_ASSERT(pVCpu, IEM_CPUMCTX_EXTRN_MUST_MASK);
    Assert(!VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_IEM));
#ifdef VBOX_STRICT
    iemInitDecoderStrictTarget(pVCpu);
#endif

    /* Execution state: */
    uint32_t fExec;
    ICORE(pVCpu).fExec = fExec = iemCalcExecFlags(pVCpu) | fExecOpts;

    /* Decoder state: */
#ifdef VBOX_VMM_TARGET_X86
    ICORE(pVCpu).enmDefAddrMode     = fExec & IEM_F_MODE_X86_CPUMODE_MASK;  /** @todo check if this is correct... */
    ICORE(pVCpu).enmEffAddrMode     = fExec & IEM_F_MODE_X86_CPUMODE_MASK;
    if ((fExec & IEM_F_MODE_X86_CPUMODE_MASK) != IEMMODE_64BIT)
    {
        ICORE(pVCpu).enmDefOpSize   = fExec & IEM_F_MODE_X86_CPUMODE_MASK;  /** @todo check if this is correct... */
        ICORE(pVCpu).enmEffOpSize   = fExec & IEM_F_MODE_X86_CPUMODE_MASK;
    }
    else
    {
        ICORE(pVCpu).enmDefOpSize   = IEMMODE_32BIT;
        ICORE(pVCpu).enmEffOpSize   = IEMMODE_32BIT;
    }
    ICORE(pVCpu).fPrefixes          = 0;
    ICORE(pVCpu).uRexReg            = 0;
    ICORE(pVCpu).uRexB              = 0;
    ICORE(pVCpu).uRexIndex          = 0;
    ICORE(pVCpu).idxPrefix          = 0;
    ICORE(pVCpu).uVex3rdReg         = 0;
    ICORE(pVCpu).uVexLength         = 0;
    ICORE(pVCpu).fEvexStuff         = 0;
    ICORE(pVCpu).iEffSeg            = X86_SREG_DS;
    ICORE(pVCpu).offModRm           = 0;
#endif /* VBOX_VMM_TARGET_X86 */
#ifdef IEM_WITH_CODE_TLB_IN_CUR_CTX
    ICORE(pVCpu).pbInstrBuf         = NULL;
    ICORE(pVCpu).offInstrNextByte   = 0;
# ifdef VBOX_VMM_TARGET_X86
    ICORE(pVCpu).offCurInstrStart   = 0;
# endif
# ifdef IEM_WITH_CODE_TLB_AND_OPCODE_BUF
    ICORE(pVCpu).offOpcode          = 0;
# endif
# ifdef VBOX_STRICT
    ICORE(pVCpu).GCPhysInstrBuf     = NIL_RTGCPHYS;
# ifdef VBOX_VMM_TARGET_X86
    ICORE(pVCpu).cbInstrBuf         = UINT16_MAX;
# endif
    ICORE(pVCpu).cbInstrBufTotal    = UINT16_MAX;
    ICORE(pVCpu).uInstrBufPc        = UINT64_C(0xc0ffc0ffcff0c0ff);
# endif
#else  /* !IEM_WITH_CODE_TLB_IN_CUR_CTX */
    ICORE(pVCpu).offOpcode          = 0;
    ICORE(pVCpu).cbOpcode           = 0;
#endif /* !IEM_WITH_CODE_TLB_IN_CUR_CTX */
    ICORE(pVCpu).cActiveMappings    = 0;
    ICORE(pVCpu).iNextMapping       = 0;
    ICORE(pVCpu).rcPassUp           = VINF_SUCCESS;

#ifdef DBGFTRACE_ENABLED
    iemInitDecoderTraceTargetPc(pVCpu, fExec);
#endif
}


/**
 * Reinitializes the decoder state 2nd+ loop of IEMExecLots.
 *
 * This is mostly a copy of iemInitDecoder.
 *
 * @param   pVCpu               The cross context virtual CPU structure of the calling EMT.
 */
DECLINLINE(void) iemReInitDecoder(PVMCPUCC pVCpu)
{
    Assert(!VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_IEM));
#ifdef VBOX_STRICT
    iemInitDecoderStrictTarget(pVCpu);
#endif

    /* ASSUMES: Anyone changing CPU state affecting the fExec bits will update them! */
    AssertMsg((ICORE(pVCpu).fExec & ~IEM_F_USER_OPTS) == iemCalcExecFlags(pVCpu),
              ("fExec=%#x iemCalcExecModeFlags=%#x\n", ICORE(pVCpu).fExec, iemCalcExecFlags(pVCpu)));

#ifdef VBOX_VMM_TARGET_X86
    IEMMODE const enmMode = IEM_GET_CPU_MODE(pVCpu);
    ICORE(pVCpu).enmDefAddrMode     = enmMode;  /** @todo check if this is correct... */
    ICORE(pVCpu).enmEffAddrMode     = enmMode;
    if (enmMode != IEMMODE_64BIT)
    {
        ICORE(pVCpu).enmDefOpSize   = enmMode;  /** @todo check if this is correct... */
        ICORE(pVCpu).enmEffOpSize   = enmMode;
    }
    else
    {
        ICORE(pVCpu).enmDefOpSize   = IEMMODE_32BIT;
        ICORE(pVCpu).enmEffOpSize   = IEMMODE_32BIT;
    }
    ICORE(pVCpu).fPrefixes          = 0;
    ICORE(pVCpu).uRexReg            = 0;
    ICORE(pVCpu).uRexB              = 0;
    ICORE(pVCpu).uRexIndex          = 0;
    ICORE(pVCpu).idxPrefix          = 0;
    ICORE(pVCpu).uVex3rdReg         = 0;
    ICORE(pVCpu).uVexLength         = 0;
    ICORE(pVCpu).fEvexStuff         = 0;
    ICORE(pVCpu).iEffSeg            = X86_SREG_DS;
    ICORE(pVCpu).offModRm           = 0;
#endif
#ifdef IEM_WITH_CODE_TLB_IN_CUR_CTX
    if (ICORE(pVCpu).pbInstrBuf)
    {
# ifdef VBOX_VMM_TARGET_X86
        uint64_t off = (enmMode == IEMMODE_64BIT
                        ? pVCpu->cpum.GstCtx.rip
                        : pVCpu->cpum.GstCtx.eip + (uint32_t)pVCpu->cpum.GstCtx.cs.u64Base)
                     - ICORE(pVCpu).uInstrBufPc;
        if (off < ICORE(pVCpu).cbInstrBufTotal)
# elif defined(VBOX_VMM_TARGET_ARMV8)
        uint64_t const off = pVCpu->cpum.GstCtx.Pc.u64 - ICORE(pVCpu).uInstrBufPc;
        if (off + sizeof(uint32_t) <= ICORE(pVCpu).cbInstrBufTotal)
# endif
        {
            ICORE(pVCpu).offInstrNextByte = (uint32_t)off;
# ifdef VBOX_VMM_TARGET_X86
            ICORE(pVCpu).offCurInstrStart = (uint16_t)off;
            if ((uint16_t)off + 15 <= ICORE(pVCpu).cbInstrBufTotal)
                ICORE(pVCpu).cbInstrBuf = (uint16_t)off + 15;
            else
                ICORE(pVCpu).cbInstrBuf = ICORE(pVCpu).cbInstrBufTotal;
# endif
        }
        else
        {
            ICORE(pVCpu).pbInstrBuf       = NULL;
            ICORE(pVCpu).offInstrNextByte = 0;
# ifdef VBOX_VMM_TARGET_X86
            ICORE(pVCpu).offCurInstrStart = 0;
            ICORE(pVCpu).cbInstrBuf       = 0;
# endif
            ICORE(pVCpu).cbInstrBufTotal  = 0;
            ICORE(pVCpu).GCPhysInstrBuf   = NIL_RTGCPHYS;
        }
    }
    else
    {
        ICORE(pVCpu).offInstrNextByte = 0;
# ifdef VBOX_VMM_TARGET_X86
        ICORE(pVCpu).offCurInstrStart = 0;
        ICORE(pVCpu).cbInstrBuf       = 0;
# endif
        ICORE(pVCpu).cbInstrBufTotal  = 0;
# ifdef VBOX_STRICT
        ICORE(pVCpu).GCPhysInstrBuf   = NIL_RTGCPHYS;
# endif
    }
# ifdef IEM_WITH_CODE_TLB_AND_OPCODE_BUF
    ICORE(pVCpu).offOpcode          = 0;
# endif
#else  /* !IEM_WITH_CODE_TLB_IN_CUR_CTX */
    ICORE(pVCpu).cbOpcode           = 0;
    ICORE(pVCpu).offOpcode          = 0;
#endif /* !IEM_WITH_CODE_TLB_IN_CUR_CTX */
    Assert(ICORE(pVCpu).cActiveMappings == 0);
    ICORE(pVCpu).iNextMapping       = 0;
    Assert(ICORE(pVCpu).rcPassUp   == VINF_SUCCESS);
    Assert(!(ICORE(pVCpu).fExec & IEM_F_BYPASS_HANDLERS));

#ifdef DBGFTRACE_ENABLED
    iemInitDecoderTraceTargetPc(pVCpu, ICORE(pVCpu).fExec);
#endif
}


/**
 * Prefetch opcodes the first time when starting executing.
 *
 * @returns Strict VBox status code.
 * @param   pVCpu               The cross context virtual CPU structure of the
 *                              calling thread.
 * @param   fExecOpts           Optional execution flags:
 *                                  - IEM_F_BYPASS_HANDLERS
 *                                  - IEM_F_X86_DISREGARD_LOCK
 */
DECLINLINE(VBOXSTRICTRC) iemInitDecoderAndPrefetchOpcodes(PVMCPUCC pVCpu, uint32_t fExecOpts) RT_NOEXCEPT
{
    iemInitDecoder(pVCpu, fExecOpts);

#if !defined(IEM_WITH_CODE_TLB_IN_CUR_CTX) && defined(VBOX_VMM_TARGET_X86)
    return iemOpcodeFetchPrefetch(pVCpu);
#else
    return VINF_SUCCESS;
#endif
}


#ifdef LOG_ENABLED
/**
 * Logs the current instruction.
 * @param   pVCpu       The cross context virtual CPU structure of the calling EMT.
 * @param   pszFunction The IEM function doing the execution.
 */
static void iemLogCurInstr(PVMCPUCC pVCpu, const char *pszFunction) RT_NOEXCEPT
{
# ifdef IN_RING3
    if (LogIs2Enabled())
    {
        char     szInstr[256];
        uint32_t cbInstr = 0;
        DBGFR3DisasInstrEx(pVCpu->pVMR3->pUVM, pVCpu->idCpu, 0, 0,
                           DBGF_DISAS_FLAGS_CURRENT_GUEST | DBGF_DISAS_FLAGS_DEFAULT_MODE,
                           szInstr, sizeof(szInstr), &cbInstr);

#  ifdef VBOX_VMM_TARGET_X86
        PCX86FXSTATE pFpuCtx = &pVCpu->cpum.GstCtx.XState.x87;
        Log2(("**** %s fExec=%x\n"
              " eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x\n"
              " eip=%08x esp=%08x ebp=%08x iopl=%d tr=%04x\n"
              " cs=%04x ss=%04x ds=%04x es=%04x fs=%04x gs=%04x efl=%08x\n"
              " fsw=%04x fcw=%04x ftw=%02x mxcsr=%04x/%04x\n"
              " %s\n"
              , pszFunction, ICORE(pVCpu).fExec,
              pVCpu->cpum.GstCtx.eax, pVCpu->cpum.GstCtx.ebx, pVCpu->cpum.GstCtx.ecx, pVCpu->cpum.GstCtx.edx, pVCpu->cpum.GstCtx.esi, pVCpu->cpum.GstCtx.edi,
              pVCpu->cpum.GstCtx.eip, pVCpu->cpum.GstCtx.esp, pVCpu->cpum.GstCtx.ebp, pVCpu->cpum.GstCtx.eflags.Bits.u2IOPL, pVCpu->cpum.GstCtx.tr.Sel,
              pVCpu->cpum.GstCtx.cs.Sel, pVCpu->cpum.GstCtx.ss.Sel, pVCpu->cpum.GstCtx.ds.Sel, pVCpu->cpum.GstCtx.es.Sel,
              pVCpu->cpum.GstCtx.fs.Sel, pVCpu->cpum.GstCtx.gs.Sel, pVCpu->cpum.GstCtx.eflags.u,
              pFpuCtx->FSW, pFpuCtx->FCW, pFpuCtx->FTW, pFpuCtx->MXCSR, pFpuCtx->MXCSR_MASK,
              szInstr));
#  elif defined(VBOX_VMM_TARGET_ARMV8)
        char szPState[160];
        DBGFR3RegFormatArmV8PState(szPState, pVCpu->cpum.GstCtx.fPState);
        if (pVCpu->iem.s.cLogFpuCountdown == 0)
            Log2(("**** %s fExec=%x\n"
                  "  x0=%016RX64  x1=%016RX64  x2=%016RX64  x3=%016RX64\n"
                  "  x4=%016RX64  x5=%016RX64  x6=%016RX64  x7=%016RX64\n"
                  "  x8=%016RX64  x9=%016RX64 x10=%016RX64 x11=%016RX64\n"
                  " x12=%016RX64 x13=%016RX64 x14=%016RX64 x15=%016RX64\n"
                  " x16=%016RX64 x17=%016RX64 x18=%016RX64 x19=%016RX64\n"
                  " x20=%016RX64 x21=%016RX64 x22=%016RX64 x23=%016RX64\n"
                  " x24=%016RX64 x25=%016RX64 x26=%016RX64 x27=%016RX64\n"
                  " x28=%016RX64  bp=%016RX64  lr=%016RX64  sp=%016RX64\n"
                  "  pc=%016RX64 psr=%012RX64 %s\n"
                  " %s\n"
                  , pszFunction, ICORE(pVCpu).fExec,
                  pVCpu->cpum.GstCtx.aGRegs[0],  pVCpu->cpum.GstCtx.aGRegs[1],  pVCpu->cpum.GstCtx.aGRegs[2],  pVCpu->cpum.GstCtx.aGRegs[3],
                  pVCpu->cpum.GstCtx.aGRegs[4],  pVCpu->cpum.GstCtx.aGRegs[5],  pVCpu->cpum.GstCtx.aGRegs[6],  pVCpu->cpum.GstCtx.aGRegs[7],
                  pVCpu->cpum.GstCtx.aGRegs[8],  pVCpu->cpum.GstCtx.aGRegs[9],  pVCpu->cpum.GstCtx.aGRegs[10], pVCpu->cpum.GstCtx.aGRegs[11],
                  pVCpu->cpum.GstCtx.aGRegs[12], pVCpu->cpum.GstCtx.aGRegs[13], pVCpu->cpum.GstCtx.aGRegs[14], pVCpu->cpum.GstCtx.aGRegs[15],
                  pVCpu->cpum.GstCtx.aGRegs[16], pVCpu->cpum.GstCtx.aGRegs[17], pVCpu->cpum.GstCtx.aGRegs[18], pVCpu->cpum.GstCtx.aGRegs[19],
                  pVCpu->cpum.GstCtx.aGRegs[20], pVCpu->cpum.GstCtx.aGRegs[21], pVCpu->cpum.GstCtx.aGRegs[22], pVCpu->cpum.GstCtx.aGRegs[23],
                  pVCpu->cpum.GstCtx.aGRegs[24], pVCpu->cpum.GstCtx.aGRegs[25], pVCpu->cpum.GstCtx.aGRegs[26], pVCpu->cpum.GstCtx.aGRegs[27],
                  pVCpu->cpum.GstCtx.aGRegs[28], pVCpu->cpum.GstCtx.aGRegs[29], pVCpu->cpum.GstCtx.aGRegs[30],
                  pVCpu->cpum.GstCtx.aSpReg[IEM_F_MODE_ARM_GET_EL(ICORE(pVCpu).fExec) > 0],
                  pVCpu->cpum.GstCtx.Pc, pVCpu->cpum.GstCtx.fPState, szPState,
                  szInstr));
        else
        {
            pVCpu->iem.s.cLogFpuCountdown -= 1;
            Log2(("**** %s fExec=%x\n"
                  "  x0=%016RX64  x1=%016RX64  x2=%016RX64  x3=%016RX64\n"
                  "  x4=%016RX64  x5=%016RX64  x6=%016RX64  x7=%016RX64\n"
                  "  x8=%016RX64  x9=%016RX64 x10=%016RX64 x11=%016RX64\n"
                  " x12=%016RX64 x13=%016RX64 x14=%016RX64 x15=%016RX64\n"
                  " x16=%016RX64 x17=%016RX64 x18=%016RX64 x19=%016RX64\n"
                  " x20=%016RX64 x21=%016RX64 x22=%016RX64 x23=%016RX64\n"
                  " x24=%016RX64 x25=%016RX64 x26=%016RX64 x27=%016RX64\n"
                  " x28=%016RX64  bp=%016RX64  lr=%016RX64  sp=%016RX64\n"
                  "  pc=%016RX64 psr=%012RX64 %s\n"
                  "  v0=%016RX64'%016RX64  v1=%016RX64'%016RX64\n"
                  "  v2=%016RX64'%016RX64  v3=%016RX64'%016RX64\n"
                  "  v4=%016RX64'%016RX64  v5=%016RX64'%016RX64\n"
                  "  v6=%016RX64'%016RX64  v7=%016RX64'%016RX64\n"
                  "  v8=%016RX64'%016RX64  v9=%016RX64'%016RX64\n"
                  " v10=%016RX64'%016RX64 v11=%016RX64'%016RX64\n"
                  " v12=%016RX64'%016RX64 v13=%016RX64'%016RX64\n"
                  " v14=%016RX64'%016RX64 v15=%016RX64'%016RX64\n"
                  " v16=%016RX64'%016RX64 v17=%016RX64'%016RX64\n"
                  " v18=%016RX64'%016RX64 v19=%016RX64'%016RX64\n"
                  " v20=%016RX64'%016RX64 v21=%016RX64'%016RX64\n"
                  " v22=%016RX64'%016RX64 v23=%016RX64'%016RX64\n"
                  " v24=%016RX64'%016RX64 v25=%016RX64'%016RX64\n"
                  " v26=%016RX64'%016RX64 v27=%016RX64'%016RX64\n"
                  " v28=%016RX64'%016RX64 v29=%016RX64'%016RX64\n"
                  " v30=%016RX64'%016RX64 v31=%016RX64'%016RX64\n"
                  " %s\n"
                  , pszFunction, ICORE(pVCpu).fExec,
                  pVCpu->cpum.GstCtx.aGRegs[0],  pVCpu->cpum.GstCtx.aGRegs[1],  pVCpu->cpum.GstCtx.aGRegs[2],  pVCpu->cpum.GstCtx.aGRegs[3],
                  pVCpu->cpum.GstCtx.aGRegs[4],  pVCpu->cpum.GstCtx.aGRegs[5],  pVCpu->cpum.GstCtx.aGRegs[6],  pVCpu->cpum.GstCtx.aGRegs[7],
                  pVCpu->cpum.GstCtx.aGRegs[8],  pVCpu->cpum.GstCtx.aGRegs[9],  pVCpu->cpum.GstCtx.aGRegs[10], pVCpu->cpum.GstCtx.aGRegs[11],
                  pVCpu->cpum.GstCtx.aGRegs[12], pVCpu->cpum.GstCtx.aGRegs[13], pVCpu->cpum.GstCtx.aGRegs[14], pVCpu->cpum.GstCtx.aGRegs[15],
                  pVCpu->cpum.GstCtx.aGRegs[16], pVCpu->cpum.GstCtx.aGRegs[17], pVCpu->cpum.GstCtx.aGRegs[18], pVCpu->cpum.GstCtx.aGRegs[19],
                  pVCpu->cpum.GstCtx.aGRegs[20], pVCpu->cpum.GstCtx.aGRegs[21], pVCpu->cpum.GstCtx.aGRegs[22], pVCpu->cpum.GstCtx.aGRegs[23],
                  pVCpu->cpum.GstCtx.aGRegs[24], pVCpu->cpum.GstCtx.aGRegs[25], pVCpu->cpum.GstCtx.aGRegs[26], pVCpu->cpum.GstCtx.aGRegs[27],
                  pVCpu->cpum.GstCtx.aGRegs[28], pVCpu->cpum.GstCtx.aGRegs[29], pVCpu->cpum.GstCtx.aGRegs[30],
                  pVCpu->cpum.GstCtx.aSpReg[IEM_F_MODE_ARM_GET_EL(ICORE(pVCpu).fExec) > 0],
                  pVCpu->cpum.GstCtx.Pc, pVCpu->cpum.GstCtx.fPState, szPState,
                  pVCpu->cpum.GstCtx.aVRegs[ 0].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[ 0].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[ 1].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[ 1].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[ 2].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[ 2].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[ 3].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[ 3].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[ 4].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[ 4].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[ 5].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[ 5].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[ 6].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[ 6].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[ 7].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[ 7].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[ 8].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[ 8].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[ 9].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[ 9].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[10].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[10].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[11].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[11].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[12].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[12].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[13].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[13].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[14].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[14].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[15].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[15].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[16].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[16].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[17].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[17].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[18].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[18].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[19].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[19].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[20].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[20].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[21].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[21].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[22].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[22].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[23].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[23].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[24].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[24].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[25].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[25].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[26].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[26].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[27].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[27].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[28].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[28].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[29].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[29].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[30].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[30].v.s.Lo,
                  pVCpu->cpum.GstCtx.aVRegs[31].v.s.Hi, pVCpu->cpum.GstCtx.aVRegs[31].v.s.Lo,
                  /** @todo more.    */
                  szInstr));
        }
#  else
#   error "port me"
#  endif

        /* This stuff sucks atm. as it fills the log with MSRs. */
        //if (LogIs3Enabled())
        //    DBGFR3InfoEx(pVCpu->pVMR3->pUVM, pVCpu->idCpu, "cpumguest", "verbose", NULL);
        return;
    }
# endif

# ifdef VBOX_VMM_TARGET_X86
    LogFlow(("%s: cs:rip=%04x:%08RX64 ss:rsp=%04x:%08RX64 EFL=%06x\n",
             pszFunction, pVCpu->cpum.GstCtx.cs.Sel, pVCpu->cpum.GstCtx.rip, pVCpu->cpum.GstCtx.ss.Sel, pVCpu->cpum.GstCtx.rsp,
             pVCpu->cpum.GstCtx.eflags.u));
#  define LOGFLOW_REG_STATE_EX(a_pszName, a_szExtraFmt, ...) \
    LogFlow(("%s: cs:rip=%04x:%08RX64 ss:rsp=%04x:%08RX64 EFL=%06x" a_szExtraFmt "\n", \
             (a_pszName), pVCpu->cpum.GstCtx.cs.Sel, pVCpu->cpum.GstCtx.rip, pVCpu->cpum.GstCtx.ss.Sel, pVCpu->cpum.GstCtx.rsp, \
             pVCpu->cpum.GstCtx.eflags.u, __VA_ARGS__))

# elif defined(VBOX_VMM_TARGET_ARMV8)
    LogFlow(("%s: pc=%08RX64 lr=%08RX64 sp=%08RX64 psr=%012RX64 EL%u\n",
             pszFunction, pVCpu->cpum.GstCtx.Pc, pVCpu->cpum.GstCtx.aGRegs[ARMV8_A64_REG_LR],
             pVCpu->cpum.GstCtx.aSpReg[IEM_F_MODE_ARM_GET_EL(ICORE(pVCpu).fExec) > 0], pVCpu->cpum.GstCtx.fPState,
             IEM_F_MODE_ARM_GET_EL(ICORE(pVCpu).fExec) ));
#  define LOGFLOW_REG_STATE_EX(a_pszName, a_szExtraFmt, ...) \
    LogFlow(("%s: pc=%08RX64 lr=%08RX64 sp=%08RX64 psr=%012RX64 EL%u" a_szExtraFmt "\n", \
             (a_pszName), pVCpu->cpum.GstCtx.Pc, pVCpu->cpum.GstCtx.aGRegs[ARMV8_A64_REG_LR], \
             pVCpu->cpum.GstCtx.aSpReg[IEM_F_MODE_ARM_GET_EL(ICORE(pVCpu).fExec) > 0], pVCpu->cpum.GstCtx.fPState, \
             IEM_F_MODE_ARM_GET_EL(ICORE(pVCpu).fExec), __VA_ARGS__))

# else
#  error "port me"
# endif
    LOGFLOW_REG_STATE_EX(pszFunction, " #%u", pVCpu->iem.s.cInstructions); /* have to have something here. sigh. */
    RT_NOREF_PV(pVCpu);
}
#endif /* LOG_ENABLED */


#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
/**
 * Deals with VMCPU_FF_VMX_APIC_WRITE, VMCPU_FF_VMX_MTF, VMCPU_FF_VMX_NMI_WINDOW,
 * VMCPU_FF_VMX_PREEMPT_TIMER and VMCPU_FF_VMX_INT_WINDOW.
 *
 * @returns Modified rcStrict.
 * @param   pVCpu       The cross context virtual CPU structure of the calling thread.
 * @param   rcStrict    The instruction execution status.
 */
static VBOXSTRICTRC iemHandleNestedInstructionBoundaryFFs(PVMCPUCC pVCpu, VBOXSTRICTRC rcStrict) RT_NOEXCEPT
{
    Assert(CPUMIsGuestInVmxNonRootMode(IEM_GET_CTX(pVCpu)));
    if (!VMCPU_FF_IS_ANY_SET(pVCpu, VMCPU_FF_VMX_APIC_WRITE | VMCPU_FF_VMX_MTF))
    {
        /* VMX preemption timer takes priority over NMI-window exits. */
        if (VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_VMX_PREEMPT_TIMER))
        {
            rcStrict = iemVmxVmexitPreemptTimer(pVCpu);
            Assert(!VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_VMX_PREEMPT_TIMER));
        }
        /*
         * Check remaining intercepts.
         *
         * NMI-window and Interrupt-window VM-exits.
         * Interrupt shadow (block-by-STI and Mov SS) inhibits interrupts and may also block NMIs.
         * Event injection during VM-entry takes priority over NMI-window and interrupt-window VM-exits.
         *
         * See Intel spec. 26.7.6 "NMI-Window Exiting".
         * See Intel spec. 26.7.5 "Interrupt-Window Exiting and Virtual-Interrupt Delivery".
         */
        else if (   VMCPU_FF_IS_ANY_SET(pVCpu, VMCPU_FF_VMX_NMI_WINDOW | VMCPU_FF_VMX_INT_WINDOW)
                 && !CPUMIsInInterruptShadow(&pVCpu->cpum.GstCtx)
                 && !TRPMHasTrap(pVCpu))
        {
            Assert(CPUMIsGuestVmxInterceptEvents(&pVCpu->cpum.GstCtx));
            if (   VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_VMX_NMI_WINDOW)
                && CPUMIsGuestVmxVirtNmiBlocking(&pVCpu->cpum.GstCtx))
            {
                rcStrict = iemVmxVmexit(pVCpu, VMX_EXIT_NMI_WINDOW, 0 /* u64ExitQual */);
                Assert(!VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_VMX_NMI_WINDOW));
            }
            else if (   VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_VMX_INT_WINDOW)
                     && CPUMIsGuestVmxVirtIntrEnabled(&pVCpu->cpum.GstCtx))
            {
                rcStrict = iemVmxVmexit(pVCpu, VMX_EXIT_INT_WINDOW, 0 /* u64ExitQual */);
                Assert(!VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_VMX_INT_WINDOW));
            }
        }
    }
    /* TPR-below threshold/APIC write has the highest priority. */
    else  if (VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_VMX_APIC_WRITE))
    {
        rcStrict = iemVmxApicWriteEmulation(pVCpu);
        Assert(!CPUMIsInInterruptShadow(&pVCpu->cpum.GstCtx));
        Assert(!VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_VMX_APIC_WRITE));
    }
    /* MTF takes priority over VMX-preemption timer. */
    else
    {
        rcStrict = iemVmxVmexit(pVCpu, VMX_EXIT_MTF, 0 /* u64ExitQual */);
        Assert(!CPUMIsInInterruptShadow(&pVCpu->cpum.GstCtx));
        Assert(!VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_VMX_MTF));
    }
    return rcStrict;
}
#endif /* VBOX_WITH_NESTED_HWVIRT_VMX */


/**
 * The actual code execution bits of IEMExecOne, IEMExecOneWithPrefetchedByPC,
 * IEMExecOneBypass and friends.
 *
 * Similar code is found in IEMExecLots.
 *
 * @return  Strict VBox status code.
 * @param   pVCpu               The cross context virtual CPU structure of the
 *                              calling EMT.
 * @param   pszFunction         The calling function name.
 * @tparam  a_fExecuteInhibit   X86: If set, execute the instruction following
 *                              CLI, POP SS and MOV SS,GR.
 */
template<bool const a_fExecuteInhibit>
DECLINLINE(VBOXSTRICTRC) iemExecOneInner(PVMCPUCC pVCpu, const char *pszFunction)
{
    AssertMsg(ICORE(pVCpu).aMemMappings[0].fAccess == IEM_ACCESS_INVALID, ("0: %#x %RGp\n", ICORE(pVCpu).aMemMappings[0].fAccess, ICORE(pVCpu).aMemBbMappings[0].GCPhysFirst));
    AssertMsg(ICORE(pVCpu).aMemMappings[1].fAccess == IEM_ACCESS_INVALID, ("1: %#x %RGp\n", ICORE(pVCpu).aMemMappings[1].fAccess, ICORE(pVCpu).aMemBbMappings[1].GCPhysFirst));
#if IEM_MAX_MEM_MAPPINGS > 2
    AssertMsg(ICORE(pVCpu).aMemMappings[2].fAccess == IEM_ACCESS_INVALID, ("2: %#x %RGp\n", ICORE(pVCpu).aMemMappings[2].fAccess, ICORE(pVCpu).aMemBbMappings[2].GCPhysFirst));
#endif
    RT_NOREF_PV(pszFunction);

    VBOXSTRICTRC rcStrict;
    IEM_TRY_SETJMP(pVCpu, rcStrict)
    {
        rcStrict = iemExecDecodeAndInterpretTargetInstruction(pVCpu);
    }
    IEM_CATCH_LONGJMP_BEGIN(pVCpu, rcStrict);
    {
        pVCpu->iem.s.cLongJumps++;
    }
    IEM_CATCH_LONGJMP_END(pVCpu);
    if (rcStrict == VINF_SUCCESS)
        pVCpu->iem.s.cInstructions++;
    if (ICORE(pVCpu).cActiveMappings > 0)
    {
        Assert(rcStrict != VINF_SUCCESS);
        iemMemRollback(pVCpu);
    }
    AssertMsg(ICORE(pVCpu).aMemMappings[0].fAccess == IEM_ACCESS_INVALID, ("0: %#x %RGp\n", ICORE(pVCpu).aMemMappings[0].fAccess, ICORE(pVCpu).aMemBbMappings[0].GCPhysFirst));
    AssertMsg(ICORE(pVCpu).aMemMappings[1].fAccess == IEM_ACCESS_INVALID, ("1: %#x %RGp\n", ICORE(pVCpu).aMemMappings[1].fAccess, ICORE(pVCpu).aMemBbMappings[1].GCPhysFirst));
#if IEM_MAX_MEM_MAPPINGS > 2
    AssertMsg(ICORE(pVCpu).aMemMappings[2].fAccess == IEM_ACCESS_INVALID, ("2: %#x %RGp\n", ICORE(pVCpu).aMemMappings[2].fAccess, ICORE(pVCpu).aMemBbMappings[2].GCPhysFirst));
#endif

//#ifdef DEBUG
//    AssertMsg(IEM_GET_INSTR_LEN(pVCpu) == cbInstr || rcStrict != VINF_SUCCESS, ("%u %u\n", IEM_GET_INSTR_LEN(pVCpu), cbInstr));
//#endif

#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
    /*
     * Perform any VMX nested-guest instruction boundary actions.
     *
     * If any of these causes a VM-exit, we must skip executing the next
     * instruction (would run into stale page tables). A VM-exit makes sure
     * there is no interrupt-inhibition, so that should ensure we don't go
     * to try execute the next instruction. Clearing a_fExecuteInhibit is
     * problematic because of the setjmp/longjmp clobbering above.
     */
    if (   !VMCPU_FF_IS_ANY_SET(pVCpu, VMCPU_FF_VMX_APIC_WRITE | VMCPU_FF_VMX_MTF | VMCPU_FF_VMX_PREEMPT_TIMER
                                     | VMCPU_FF_VMX_INT_WINDOW | VMCPU_FF_VMX_NMI_WINDOW)
        || rcStrict != VINF_SUCCESS)
    { /* likely */ }
    else
        rcStrict = iemHandleNestedInstructionBoundaryFFs(pVCpu, rcStrict);
#endif

#ifdef VBOX_VMM_TARGET_X86
    /* Execute the next instruction as well if a cli, pop ss or
       mov ss, Gr has just completed successfully. */
    if RT_CONSTEXPR_IF(a_fExecuteInhibit)
    {
        if (   rcStrict == VINF_SUCCESS
            && CPUMIsInInterruptShadow(&pVCpu->cpum.GstCtx))
        {
            rcStrict = iemInitDecoderAndPrefetchOpcodes(pVCpu,
                                                        ICORE(pVCpu).fExec & (IEM_F_BYPASS_HANDLERS | IEM_F_X86_DISREGARD_LOCK));
            if (rcStrict == VINF_SUCCESS)
            {
# ifdef LOG_ENABLED
                iemLogCurInstr(pVCpu, pszFunction);
# endif
                IEM_TRY_SETJMP_AGAIN(pVCpu, rcStrict)
                {
                    rcStrict = iemExecDecodeAndInterpretTargetInstruction(pVCpu);
                }
                IEM_CATCH_LONGJMP_BEGIN(pVCpu, rcStrict);
                {
                    pVCpu->iem.s.cLongJumps++;
                }
                IEM_CATCH_LONGJMP_END(pVCpu);
                if (rcStrict == VINF_SUCCESS)
                {
                    pVCpu->iem.s.cInstructions++;
# ifdef VBOX_WITH_NESTED_HWVIRT_VMX
                    if (!VMCPU_FF_IS_ANY_SET(pVCpu, VMCPU_FF_VMX_APIC_WRITE | VMCPU_FF_VMX_MTF | VMCPU_FF_VMX_PREEMPT_TIMER
                                                  | VMCPU_FF_VMX_INT_WINDOW | VMCPU_FF_VMX_NMI_WINDOW))
                    { /* likely */ }
                    else
                        rcStrict = iemHandleNestedInstructionBoundaryFFs(pVCpu, rcStrict);
# endif
                }
                if (ICORE(pVCpu).cActiveMappings > 0)
                {
                    Assert(rcStrict != VINF_SUCCESS);
                    iemMemRollback(pVCpu);
                }
                AssertMsg(ICORE(pVCpu).aMemMappings[0].fAccess == IEM_ACCESS_INVALID, ("0: %#x %RGp\n", ICORE(pVCpu).aMemMappings[0].fAccess, ICORE(pVCpu).aMemBbMappings[0].GCPhysFirst));
                AssertMsg(ICORE(pVCpu).aMemMappings[1].fAccess == IEM_ACCESS_INVALID, ("1: %#x %RGp\n", ICORE(pVCpu).aMemMappings[1].fAccess, ICORE(pVCpu).aMemBbMappings[1].GCPhysFirst));
# if IEM_MAX_MEM_MAPPINGS > 2
                AssertMsg(ICORE(pVCpu).aMemMappings[2].fAccess == IEM_ACCESS_INVALID, ("2: %#x %RGp\n", ICORE(pVCpu).aMemMappings[2].fAccess, ICORE(pVCpu).aMemBbMappings[2].GCPhysFirst));
# endif
            }
            else if (ICORE(pVCpu).cActiveMappings > 0)
                iemMemRollback(pVCpu);
            /** @todo drop this after we bake this change into RIP advancing. */
            CPUMClearInterruptShadow(&pVCpu->cpum.GstCtx); /* hope this is correct for all exceptional cases... */
        }
    }
#endif /* VBOX_VMM_TARGET_X86 */

    /*
     * Return value fiddling, statistics and sanity assertions.
     */
    rcStrict = iemExecStatusCodeFiddling(pVCpu, rcStrict);

#ifdef VBOX_STRICT
    iemInitExecTailStrictTarget(pVCpu);
#endif
    return rcStrict;
}


/**
 * Execute one instruction.
 *
 * @return  Strict VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure of the calling EMT.
 */
VMM_INT_DECL(VBOXSTRICTRC) IEMExecOne(PVMCPUCC pVCpu)
{
    AssertCompile(sizeof(pVCpu->iem.s) <= sizeof(pVCpu->iem.padding)); /* (tstVMStruct can't do it's job w/o instruction stats) */
#ifdef LOG_ENABLED
    iemLogCurInstr(pVCpu, "IEMExecOne");
#endif

    /*
     * Do the decoding and emulation.
     */
    VBOXSTRICTRC rcStrict = iemInitDecoderAndPrefetchOpcodes(pVCpu, 0 /*fExecOpts*/);
    if (rcStrict == VINF_SUCCESS)
        rcStrict = iemExecOneInner<true>(pVCpu, "IEMExecOne");
    else if (ICORE(pVCpu).cActiveMappings > 0)
        iemMemRollback(pVCpu);

#ifdef LOG_ENABLED
    if (rcStrict != VINF_SUCCESS)
        LOGFLOW_REG_STATE_EX("IEMExecOne", " - rcStrict=%Rrc", VBOXSTRICTRC_VAL(rcStrict));
#endif
    return rcStrict;
}


VMM_INT_DECL(VBOXSTRICTRC) IEMExecOneWithPrefetchedByPC(PVMCPUCC pVCpu, uint64_t OpcodeBytesPC,
                                                        const void *pvOpcodeBytes, size_t cbOpcodeBytes)
{
    VBOXSTRICTRC rcStrict;
    if (   cbOpcodeBytes
        && iemRegGetPC(pVCpu) == OpcodeBytesPC)
    {
        iemInitDecoder(pVCpu, 0 /*fExecOpts*/);
#ifdef IEM_WITH_CODE_TLB_IN_CUR_CTX
        ICORE(pVCpu).uInstrBufPc      = OpcodeBytesPC;
        ICORE(pVCpu).pbInstrBuf       = (uint8_t const *)pvOpcodeBytes;
        ICORE(pVCpu).cbInstrBufTotal  = (uint16_t)RT_MIN(X86_PAGE_SIZE, cbOpcodeBytes);
# ifdef VBOX_VMM_TARGET_X86
        ICORE(pVCpu).offCurInstrStart = 0;
        ICORE(pVCpu).offInstrNextByte = 0;
# endif
        ICORE(pVCpu).GCPhysInstrBuf   = NIL_RTGCPHYS;
#else
        ICORE(pVCpu).cbOpcode = (uint8_t)RT_MIN(cbOpcodeBytes, sizeof(ICORE(pVCpu).abOpcode));
        memcpy(ICORE(pVCpu).abOpcode, pvOpcodeBytes, ICORE(pVCpu).cbOpcode);
#endif
        rcStrict = VINF_SUCCESS;
    }
    else
        rcStrict = iemInitDecoderAndPrefetchOpcodes(pVCpu, 0 /*fExecOpts*/);
    if (rcStrict == VINF_SUCCESS)
        rcStrict = iemExecOneInner<true>(pVCpu, "IEMExecOneWithPrefetchedByPC");
    else if (ICORE(pVCpu).cActiveMappings > 0)
        iemMemRollback(pVCpu);

    return rcStrict;
}


VMM_INT_DECL(VBOXSTRICTRC) IEMExecOneBypass(PVMCPUCC pVCpu)
{
    VBOXSTRICTRC rcStrict = iemInitDecoderAndPrefetchOpcodes(pVCpu, IEM_F_BYPASS_HANDLERS);
    if (rcStrict == VINF_SUCCESS)
        rcStrict = iemExecOneInner<false>(pVCpu, "IEMExecOneBypass");
    else if (ICORE(pVCpu).cActiveMappings > 0)
        iemMemRollback(pVCpu);

    return rcStrict;
}


VMM_INT_DECL(VBOXSTRICTRC) IEMExecOneBypassWithPrefetchedByPC(PVMCPUCC pVCpu, uint64_t OpcodeBytesPC,
                                                              const void *pvOpcodeBytes, size_t cbOpcodeBytes)
{
    VBOXSTRICTRC rcStrict;
    if (   cbOpcodeBytes
        && iemRegGetPC(pVCpu) == OpcodeBytesPC)
    {
        iemInitDecoder(pVCpu, IEM_F_BYPASS_HANDLERS);
#ifdef IEM_WITH_CODE_TLB_IN_CUR_CTX
        ICORE(pVCpu).uInstrBufPc      = OpcodeBytesPC;
        ICORE(pVCpu).pbInstrBuf       = (uint8_t const *)pvOpcodeBytes;
        ICORE(pVCpu).cbInstrBufTotal  = (uint16_t)RT_MIN(X86_PAGE_SIZE, cbOpcodeBytes);
# ifdef VBOX_VMM_TARGET_X86
        ICORE(pVCpu).offCurInstrStart = 0;
        ICORE(pVCpu).offInstrNextByte = 0;
# endif
        ICORE(pVCpu).GCPhysInstrBuf   = NIL_RTGCPHYS;
#else
        ICORE(pVCpu).cbOpcode = (uint8_t)RT_MIN(cbOpcodeBytes, sizeof(ICORE(pVCpu).abOpcode));
        memcpy(ICORE(pVCpu).abOpcode, pvOpcodeBytes, ICORE(pVCpu).cbOpcode);
#endif
        rcStrict = VINF_SUCCESS;
    }
    else
        rcStrict = iemInitDecoderAndPrefetchOpcodes(pVCpu, IEM_F_BYPASS_HANDLERS);
    if (rcStrict == VINF_SUCCESS)
        rcStrict = iemExecOneInner<false>(pVCpu, "IEMExecOneBypassWithPrefetchedByPC");
    else if (ICORE(pVCpu).cActiveMappings > 0)
        iemMemRollback(pVCpu);

    return rcStrict;
}


/**
 * For handling split cacheline lock operations when the host has split-lock
 * detection enabled.
 *
 * This will cause the interpreter to disregard the lock prefix and implicit
 * locking (xchg).
 *
 * @returns Strict VBox status code.
 * @param   pVCpu   The cross context virtual CPU structure of the calling EMT.
 */
VMM_INT_DECL(VBOXSTRICTRC) IEMExecOneIgnoreLock(PVMCPUCC pVCpu)
{
    /*
     * Do the decoding and emulation.
     */
    VBOXSTRICTRC rcStrict = iemInitDecoderAndPrefetchOpcodes(pVCpu, IEM_F_X86_DISREGARD_LOCK);
    if (rcStrict == VINF_SUCCESS)
        rcStrict = iemExecOneInner<true>(pVCpu, "IEMExecOneIgnoreLock");
    else if (ICORE(pVCpu).cActiveMappings > 0)
        iemMemRollback(pVCpu);

#ifdef LOG_ENABLED
    if (rcStrict != VINF_SUCCESS)
        LOGFLOW_REG_STATE_EX("IEMExecOneIgnoreLock", " - rcStrict=%Rrc", VBOXSTRICTRC_VAL(rcStrict));
#endif
    return rcStrict;
}


/**
 * Code common to IEMExecLots and IEMExecRecompilerThreaded that attempts to
 * inject a pending TRPM trap.
 */
VBOXSTRICTRC iemExecInjectPendingTrap(PVMCPUCC pVCpu)
{
    Assert(TRPMHasTrap(pVCpu));
#ifdef VBOX_VMM_TARGET_X86

    if (   !CPUMIsInInterruptShadow(&pVCpu->cpum.GstCtx)
        && !CPUMAreInterruptsInhibitedByNmi(&pVCpu->cpum.GstCtx))
    {
        /** @todo Can we centralize this under CPUMCanInjectInterrupt()? */
# if defined(VBOX_WITH_NESTED_HWVIRT_SVM) || defined(VBOX_WITH_NESTED_HWVIRT_VMX)
        bool fIntrEnabled = CPUMGetGuestGif(&pVCpu->cpum.GstCtx);
        if (fIntrEnabled)
        {
            if (!CPUMIsGuestInNestedHwvirtMode(IEM_GET_CTX(pVCpu)))
                fIntrEnabled = pVCpu->cpum.GstCtx.eflags.Bits.u1IF;
            else if (CPUMIsGuestInVmxNonRootMode(IEM_GET_CTX(pVCpu)))
                fIntrEnabled = CPUMIsGuestVmxPhysIntrEnabled(IEM_GET_CTX(pVCpu));
            else
            {
                Assert(CPUMIsGuestInSvmNestedHwVirtMode(IEM_GET_CTX(pVCpu)));
                fIntrEnabled = CPUMIsGuestSvmPhysIntrEnabled(pVCpu, IEM_GET_CTX(pVCpu));
            }
        }
# else
        bool fIntrEnabled = pVCpu->cpum.GstCtx.eflags.Bits.u1IF;
# endif
        if (fIntrEnabled)
        {
            uint8_t     u8TrapNo;
            TRPMEVENT   enmType;
            uint32_t    uErrCode;
            RTGCPTR     uCr2;
            int rc2 = TRPMQueryTrapAll(pVCpu, &u8TrapNo, &enmType, &uErrCode, &uCr2, NULL /*pu8InstLen*/, NULL /*fIcebp*/);
            AssertRC(rc2);
            Assert(enmType == TRPM_HARDWARE_INT);
#ifdef __EMSCRIPTEN__
            /* IRQ delivery counter: log every 10K deliveries to track interrupt rate */
            {
                static uint64_t s_cIrqsDelivered = 0;
                static uint64_t s_cIrqsLast = 0;
                extern volatile uint64_t g_cWasmVirtualInstructions;
                s_cIrqsDelivered++;
                if (s_cIrqsDelivered - s_cIrqsLast >= 10000)
                {
                    static uint64_t s_cPrevInsns = 0;
                    uint64_t cDeltaInsns = g_cWasmVirtualInstructions - s_cPrevInsns;
                    RTPrintf("[IRQ-COUNT] #%llu at insns=%llu IRQ=%u delta=%llu (%.1f IRQ/ms)\n",
                             (unsigned long long)s_cIrqsDelivered,
                             (unsigned long long)g_cWasmVirtualInstructions,
                             (unsigned)u8TrapNo,
                             (unsigned long long)cDeltaInsns,
                             cDeltaInsns > 0 ? (10000.0 * 10000.0 / (double)cDeltaInsns) : 0.0);
                    s_cPrevInsns = g_cWasmVirtualInstructions;
                    s_cIrqsLast = s_cIrqsDelivered;
                }
            }
#endif
            VBOXSTRICTRC rcStrict = IEMInjectTrap(pVCpu, u8TrapNo, enmType, (uint16_t)uErrCode, uCr2, 0 /*cbInstr*/);

            TRPMResetTrap(pVCpu);

# if defined(VBOX_WITH_NESTED_HWVIRT_SVM) || defined(VBOX_WITH_NESTED_HWVIRT_VMX)
            /* Injecting an event may cause a VM-exit. */
            if (   rcStrict != VINF_SUCCESS
                && rcStrict != VINF_IEM_RAISED_XCPT)
                return iemExecStatusCodeFiddling(pVCpu, rcStrict);
# else
            NOREF(rcStrict);
# endif
        }
    }

    return VINF_SUCCESS;

#else  /* !VBOX_VMM_TARGET_X86 */
    RT_NOREF(pVCpu);
    AssertFailedReturn(VERR_NOT_IMPLEMENTED);
#endif /* !VBOX_VMM_TARGET_X86 */
}


VMM_INT_DECL(VBOXSTRICTRC) IEMExecLots(PVMCPUCC pVCpu, uint32_t cMaxInstructions, uint32_t cPollRate, uint32_t *pcInstructions)
{
    uint32_t const cInstructionsAtStart = pVCpu->iem.s.cInstructions;
    AssertMsg(RT_IS_POWER_OF_TWO(cPollRate + 1), ("%#x\n", cPollRate));
    Assert(cMaxInstructions > 0);

    /*
     * See if there is an interrupt pending in TRPM, inject it if we can.
     */
    /** @todo What if we are injecting an exception and not an interrupt? Is that
     *        possible here? For now we assert it is indeed only an interrupt. */
    if (!TRPMHasTrap(pVCpu))
    { /* likely */ }
    else
    {
        VBOXSTRICTRC rcStrict = iemExecInjectPendingTrap(pVCpu);
        if (RT_LIKELY(rcStrict == VINF_SUCCESS))
        { /*likely */ }
        else
            return rcStrict;
    }

    /*
     * Initial decoder init w/ prefetch, then setup setjmp.
     */
    VBOXSTRICTRC rcStrict = iemInitDecoderAndPrefetchOpcodes(pVCpu, 0 /*fExecOpts*/);
    if (rcStrict == VINF_SUCCESS)
    {
        ICORE(pVCpu).cActiveMappings = 0; /** @todo wtf? */
        IEM_TRY_SETJMP(pVCpu, rcStrict)
        {
            /*
             * The run loop.  We limit ourselves to 4096 instructions right now.
             */
            uint32_t cMaxInstructionsGccStupidity = cMaxInstructions;
            PVMCC pVM = pVCpu->CTX_SUFF(pVM);
            for (;;)
            {
                /*
                 * Log the state.
                 */
#ifdef LOG_ENABLED
                iemLogCurInstr(pVCpu, "IEMExecLots");
#endif

                /*
                 * Do the decoding and emulation.
                 */
#ifdef __EMSCRIPTEN__
                /* Update global instruction counter for TM virtual clock.
                 * TMAllVirtual.cpp reads this to drive timer expiration under Wasm.
                 *
                 * IMPORTANT: Use only the actual instruction count (no boost) so the
                 * timer fires every ~25,000 actual instructions.  The old approach of
                 * adding a "virtual time boost" from skipped delay loops caused the
                 * timer to fire thousands of extra times per delay skip, creating a
                 * permanent interrupt storm that prevented kernel_init from running.
                 * Delay loops now advance jiffies_64 directly via PGMPhysWrite instead. */
                {
                    extern volatile uint64_t g_cWasmVirtualInstructions;

                    /* Set up s_pVMForRead early so wasmReadGuestPhys() works from JS
                     * before DELAY-ACCEL fires (e.g., during decompression). */
                    if (RT_UNLIKELY(!s_pVMForRead))
                        s_pVMForRead = pVCpu->CTX_SUFF(pVM);

                    /* Use accumulated instruction count for the timer clock (no boost).
                     * pVCpu->iem.s.cInstructions is uint32_t and wraps at ~4.3B instructions.
                     * Without wrap handling, g_cWasmVirtualInstructions regresses to 0 after
                     * the wrap, causing the virtual clock to go backward. The timer's next
                     * deadline (~430 seconds) is then permanently in the "future" from the
                     * clock's perspective, so NO MORE timer interrupts ever fire. The kernel
                     * is left without preemption and kernel_init never gets scheduled.
                     * Fix: accumulate into a uint64 using wrap-aware uint32 subtraction.
                     *      delta = (uint32)(curr - prev) handles wrap correctly. */
                    {
                        static uint32_t s_cPrevCInsn = 0;
                        static uint64_t s_cAccumulated = 0;
                        uint32_t uCurr = pVCpu->iem.s.cInstructions;
                        s_cAccumulated += (uint32_t)(uCurr - s_cPrevCInsn);
                        s_cPrevCInsn = uCurr;
                        g_cWasmVirtualInstructions = s_cAccumulated;
                    }

                    /* High-frequency __delay() fast path: once we've identified the
                     * delay loop address, check every 1000 instructions and skip it
                     * immediately by setting the counter register to 1.  This is much
                     * faster than waiting for the 1M-instruction diagnostic interval.
                     * Advance jiffies_64 directly in guest memory (no timer ISR storm). */
                    static uint64_t s_uDelayRip = 0;  /* learned from slow-path */
                    {
                        static uint64_t s_cNextFastCheck = 0;
                        if (s_uDelayRip != 0 && g_cWasmVirtualInstructions >= s_cNextFastCheck)
                        {
                            s_cNextFastCheck = g_cWasmVirtualInstructions + 1000;
                            uint64_t rip = pVCpu->cpum.GstCtx.rip;
                            /* Check if RIP is within the tight loop (±4 bytes of known address) */
                            int64_t d = (int64_t)(rip - s_uDelayRip);
                            if (d >= -4 && d <= 4)
                            {
                                /* Directly advance jiffies_64 in guest memory by the number of
                                 * jiffies the skipped iterations would have consumed.
                                 * Formula: jiffies = insns_skipped * 100ns / 1,000,000ns_per_jiffie
                                 *        = remaining * 2 * 100 / 1000000 = remaining / 5000.
                                 * Skip loops < 5000 iterations → advance 0 jiffies (fine).
                                 * Cap at 100 jiffies per skip to avoid overshooting. */
                                uint64_t remaining = pVCpu->cpum.GstCtx.rax;
                                if (remaining > 1)
                                {
                                    uint64_t jiffies_advance = remaining * 2 * 100 / 1000000;
                                    if (jiffies_advance > 100) jiffies_advance = 100; /* cap */
                                    if (jiffies_advance > 0)
                                    {
                                        PVMCC pVMJ = pVCpu->CTX_SUFF(pVM);
                                        uint64_t uJiffies = 0;
                                        PGMPhysRead(pVMJ, UINT64_C(0x02406980), &uJiffies,
                                                    sizeof(uJiffies), PGMACCESSORIGIN_DEBUGGER);
                                        uJiffies += jiffies_advance;
                                        PGMPhysWrite(pVMJ, UINT64_C(0x02406980), &uJiffies,
                                                     sizeof(uJiffies), PGMACCESSORIGIN_DEBUGGER);
                                    }
                                }
                                pVCpu->cpum.GstCtx.rax = 1;
                            }
                        }
                    }

                    /* Periodic diagnostic: log timer/FF state.
                     * Interval: 1M when exploring, 1B when delay accelerator is active
                     * (to prevent IEM-DIAG from flooding the console and hiding
                     * DELAY-ACCEL stack dumps). */
                    /* One-shot: detect when EIP first enters .init.text (0xffffffff82511000-0x82826000).
                     * kernel_init_freeable() and do_initcalls() live here. This fires every 1M insns
                     * in 64-bit kernel mode to catch the transition quickly. */
                    if ((pVCpu->cpum.GstCtx.msrEFER & MSR_K6_EFER_LMA)
                        && pVCpu->cpum.GstCtx.cs.Sel == 0x10)
                    {
                        static bool    s_fInitTextSeen   = false;
                        static uint64_t s_cNextInitCheck = UINT64_C(100000000); /* start earlier at 100M */
                        if (!s_fInitTextSeen && g_cWasmVirtualInstructions >= s_cNextInitCheck)
                        {
                            s_cNextInitCheck = g_cWasmVirtualInstructions + UINT64_C(1000000); /* 1M */
                            uint64_t rip2 = pVCpu->cpum.GstCtx.rip;
                            if (rip2 >= UINT64_C(0xffffffff82511000) && rip2 < UINT64_C(0xffffffff82826000))
                            {
                                s_fInitTextSeen = true;
                                RTPrintf("[INIT-TEXT] First .init.text EIP detected at insns=%llu EIP=%#llx RSP=%#llx\n",
                                    (unsigned long long)g_cWasmVirtualInstructions,
                                    (unsigned long long)rip2,
                                    (unsigned long long)pVCpu->cpum.GstCtx.rsp);
                                RTStrmFlush(g_pStdOut);
                            }
                        }
                    }

                    /* ── TASK-WALK: walk the task list via init_task.tasks (offset 0x310) ──
                     * init_task is at VA 0xffffffff824114c0 (phys 0x24114c0).
                     * tasks list_head is at offset 0x310 → VA 0xffffffff824117d0 (phys 0x24117d0).
                     * task_struct field offsets: state=0x10, pid=0x488, comm=0x5c8.
                     * Fires at 300M, then every 500M instructions. */
                    if (!g_fProductionMode
                        && (pVCpu->cpum.GstCtx.msrEFER & MSR_K6_EFER_LMA)
                        && g_cWasmVirtualInstructions >= UINT64_C(300000000))
                    {
                        static uint64_t s_cNextTaskWalk = UINT64_C(300000000);
                        if (g_cWasmVirtualInstructions >= s_cNextTaskWalk)
                        {
                            s_cNextTaskWalk = g_cWasmVirtualInstructions + UINT64_C(500000000);
                            PVMCC pVMtw = pVCpu->CTX_SUFF(pVM);
                            /* Read init_task.tasks.next (phys 0x24117d0) */
                            uint64_t uNextTasksVA = 0;
                            PGMPhysSimpleReadGCPhys(pVMtw, &uNextTasksVA,
                                (RTGCPHYS)UINT64_C(0x24117d0), 8);
                            RTPrintf("[TASK-WALK] insns=%llu init_task.tasks.next=%#llx\n",
                                (unsigned long long)g_cWasmVirtualInstructions,
                                (unsigned long long)uNextTasksVA);
                            /* Walk up to 20 tasks */
                            int nTasks = 0;
                            uint64_t uCurTasksVA = uNextTasksVA;
                            while (uCurTasksVA != UINT64_C(0xffffffff824117d0) /* init_task.tasks VA */
                                   && uCurTasksVA != 0
                                   && nTasks < 20)
                            {
                                nTasks++;
                                /* task_struct base = tasks VA - 0x310 */
                                uint64_t uTaskBaseVA = uCurTasksVA - UINT64_C(0x310);
                                /* VA → phys translation */
                                uint64_t uTaskBasePhys;
                                if (uTaskBaseVA >= UINT64_C(0xffffffff80000000))
                                    uTaskBasePhys = uTaskBaseVA - UINT64_C(0xffffffff80000000);
                                else
                                    uTaskBasePhys = uTaskBaseVA - UINT64_C(0xffff888000000000);
                                /* Read state (offset 0x10), on_cpu (offset 0x24 on 5.4),
                                 * pid (offset 0x488), comm (offset 0x5c8) */
                                uint64_t uState = 0xDEAD;
                                uint32_t uPid = 0xDEAD;
                                char szComm[17];
                                __builtin_memset(szComm, 0, sizeof(szComm));
                                PGMPhysSimpleReadGCPhys(pVMtw, &uState,
                                    (RTGCPHYS)(uTaskBasePhys + 0x10), 8);
                                PGMPhysSimpleReadGCPhys(pVMtw, &uPid,
                                    (RTGCPHYS)(uTaskBasePhys + 0x488), 4);
                                PGMPhysSimpleReadGCPhys(pVMtw, szComm,
                                    (RTGCPHYS)(uTaskBasePhys + 0x5c8), 16);
                                szComm[16] = '\0';
                                /* Read thread_info.flags (offset 0x0) for TIF_NEED_RESCHED (bit 3) */
                                uint64_t uTIFlags = 0;
                                PGMPhysSimpleReadGCPhys(pVMtw, &uTIFlags,
                                    (RTGCPHYS)uTaskBasePhys, 8);
                                RTPrintf("[TASK-WALK]   #%d VA=%#llx state=%lld pid=%u comm='%s' ti_flags=%#llx%s\n",
                                    nTasks,
                                    (unsigned long long)uTaskBaseVA,
                                    (long long)uState,
                                    (unsigned)uPid, szComm,
                                    (unsigned long long)uTIFlags,
                                    (uTIFlags & 8) ? " NEED_RESCHED" : "");
                                /* Read next task in list */
                                uint64_t uNextVA = 0;
                                PGMPhysSimpleReadGCPhys(pVMtw, &uNextVA,
                                    (RTGCPHYS)(uTaskBasePhys + 0x310), 8);
                                uCurTasksVA = uNextVA;
                            }
                            RTPrintf("[TASK-WALK]   total=%d\n", nTasks);
                            RTStrmFlush(g_pStdOut);
                        }
                    }

                    /* ── TASK-BORN: detect first task creation by monitoring tasks.next
                     * Check every 10K instructions after 200M. Log transitions. */
                    {
                        static bool s_fTaskBornSeen = false;
                        static bool s_fTaskGone = false;
                        static uint64_t s_uLastTasksNext = 0;
                        if (!s_fTaskGone
                            && (pVCpu->cpum.GstCtx.msrEFER & MSR_K6_EFER_LMA)
                            && g_cWasmVirtualInstructions >= UINT64_C(200000000)
                            && (g_cWasmVirtualInstructions % UINT64_C(10000)) == 0)
                        {
                            const uint64_t kInitTaskPhys = UINT64_C(0x24114c0);
                            const uint64_t kInitTaskVA = UINT64_C(0xffffffff824114c0);
                            const uint64_t kTasksSelf = kInitTaskVA + UINT64_C(0x310);
                            uint64_t uTN = 0;
                            PGMPhysSimpleReadGCPhys(pVCpu->CTX_SUFF(pVM), &uTN,
                                (RTGCPHYS)(kInitTaskPhys + UINT64_C(0x310)), 8);

                            if (!s_fTaskBornSeen && uTN != kTasksSelf)
                            {
                                s_fTaskBornSeen = true;
                                s_uLastTasksNext = uTN;
                                /* Read new task's comm and pid */
                                uint64_t uNewTaskPhys = (uTN - UINT64_C(0x310))
                                    - UINT64_C(0xffffffff80000000);
                                char comm[17];
                                __builtin_memset(comm, 0, 17);
                                uint32_t uPid = 0;
                                PGMPhysSimpleReadGCPhys(pVCpu->CTX_SUFF(pVM), comm,
                                    (RTGCPHYS)(uNewTaskPhys + UINT64_C(0x5c8)), 16);
                                PGMPhysSimpleReadGCPhys(pVCpu->CTX_SUFF(pVM), &uPid,
                                    (RTGCPHYS)(uNewTaskPhys + UINT64_C(0x488)), 4);
                                RTPrintf("[TASK-BORN] insns=%llu tasks.next=%#llx "
                                         "pid=%u comm='%.16s' RIP=%#llx\n",
                                    (unsigned long long)g_cWasmVirtualInstructions,
                                    (unsigned long long)uTN,
                                    (unsigned)uPid, comm,
                                    (unsigned long long)pVCpu->cpum.GstCtx.rip);
                                RTStrmFlush(g_pStdOut);
                            }
                            else if (s_fTaskBornSeen && uTN == kTasksSelf)
                            {
                                s_fTaskGone = true;
                                RTPrintf("[TASK-GONE] insns=%llu tasks back to singleton "
                                         "RIP=%#llx\n",
                                    (unsigned long long)g_cWasmVirtualInstructions,
                                    (unsigned long long)pVCpu->cpum.GstCtx.rip);
                                RTStrmFlush(g_pStdOut);
                            }
                        }
                    }

                    /* ── FC4 (version F): improved nuclear patch with done=1.
                     *
                     * Phase 1 (tasks==singleton): Patch WFC to
                     *   `mov dword [rdi], 1; ret` (7 bytes: c7 07 01 00 00 00 c3).
                     *   This sets completion->done=1 AND returns immediately.
                     *   Better than plain ret because callers see done=1 (no retries).
                     *
                     * Phase 2 (tasks created): Restore original 7 bytes.
                     *
                     * Phase 3 (ongoing): Timeout injection — replace
                     *   MAX_SCHEDULE_TIMEOUT with 1 on any stack. Timeout of 1 jiffy
                     *   means the wait exits after a single timer tick (~25K insns).
                     *   Combined with done=1 from phase 1 for pre-existing completions,
                     *   this prevents cascading deadlocks during do_initcalls.
                     *
                     * wait_for_completion addresses (kernel-dependent):
                     * - FossaPup64 5.4.53: VA 0x810ce640, PA 0x10ce640, orig 0x41
                     * - TinyCorePure64 6.12.11: VA 0x819f18a0, PA 0x19f18a0, orig 0x55
                     * Auto-detect by probing both addresses for known prologues. */
                    {
                        static bool s_fWfcPatched = false;
                        static bool s_fWfcRestored = false;
                        static uint64_t s_cNextFC4 = UINT64_C(100000000);
                        static unsigned s_cTimeoutInjections = 0;
                        static RTGCPHYS s_kWfcPA = 0;
                        static uint8_t  s_origByte = 0;
                        if ((pVCpu->cpum.GstCtx.msrEFER & MSR_K6_EFER_LMA)
                            && g_cWasmVirtualInstructions >= s_cNextFC4)
                        {
                            s_cNextFC4 = g_cWasmVirtualInstructions + UINT64_C(1000000);
                            PVMCC pVMfc = pVCpu->CTX_SUFF(pVM);

                            /* Auto-detect WFC address — only after kernel is
                             * loaded (CR0.PG=1 means paging is on = 64-bit kernel) */
                            if (!s_kWfcPA
                                && (pVCpu->cpum.GstCtx.cr0 & UINT64_C(0x80000000)))
                            {
                                uint8_t b1 = 0, b2 = 0;
                                /* TinyCore 6.12: 0x55 (push rbp) at PA 0x19f18a0 */
                                PGMPhysSimpleReadGCPhys(pVMfc, &b1,
                                    (RTGCPHYS)UINT64_C(0x19f18a0), 1);
                                /* FossaPup 5.4: 0x41 (push r12) at PA 0x10ce640 */
                                PGMPhysSimpleReadGCPhys(pVMfc, &b2,
                                    (RTGCPHYS)UINT64_C(0x10ce640), 1);
                                if (b1 == 0x55) {
                                    s_kWfcPA = (RTGCPHYS)UINT64_C(0x19f18a0);
                                    s_origByte = 0x55;
                                    RTPrintf("[FC4G] Detected TinyCore 6.12 WFC"
                                             " @PA 0x19f18a0\n");
                                } else if (b2 == 0x41) {
                                    s_kWfcPA = (RTGCPHYS)UINT64_C(0x10ce640);
                                    s_origByte = 0x41;
                                    RTPrintf("[FC4G] Detected FossaPup 5.4 WFC"
                                             " @PA 0x10ce640\n");
                                } else {
                                    /* Unknown kernel — skip WFC patching */
                                    s_kWfcPA = 1; /* non-zero = checked */
                                    RTPrintf("[FC4G] Unknown kernel (b1=%#x b2=%#x)"
                                             " — WFC patch disabled\n", b1, b2);
                                }
                                RTStrmFlush(g_pStdOut);
                            }

                            /* init_task address depends on kernel:
                             * - FossaPup 5.4: PA 0x24114c0, tasks.self=0xffffffff824117d0
                             * - TinyCore 6.12: PA 0x200ef80, tasks.self=0xffffffff8200f290 */
                            static uint64_t s_kInitTaskPhys = 0;
                            static uint64_t s_kTasksSelf = 0;
                            if (!s_kInitTaskPhys)
                            {
                                if (s_kWfcPA == (RTGCPHYS)UINT64_C(0x19f18a0))
                                {
                                    s_kInitTaskPhys = UINT64_C(0x200ef80);
                                    s_kTasksSelf = UINT64_C(0xffffffff8200f290);
                                }
                                else
                                {
                                    s_kInitTaskPhys = UINT64_C(0x24114c0);
                                    s_kTasksSelf = UINT64_C(0xffffffff824117d0);
                                }
                            }
                            const uint64_t kInitTaskPhys = s_kInitTaskPhys;
                            const uint64_t kTasksSelf = s_kTasksSelf;

                            uint64_t uTN = 0;
                            PGMPhysSimpleReadGCPhys(pVMfc, &uTN,
                                (RTGCPHYS)(kInitTaskPhys + UINT64_C(0x310)), 8);

                            /* Phase 1: Patch WFC to plain `ret` (0xc3)
                             * while no tasks exist. */
                            if (!s_fWfcPatched && s_kWfcPA > 1
                                && uTN == kTasksSelf)
                            {
                                uint8_t retByte = 0xc3;
                                PGMPhysSimpleWriteGCPhys(pVMfc, s_kWfcPA,
                                    &retByte, 1);
                                s_fWfcPatched = true;
                                RTPrintf("[FC4G] PATCHED WFC @%#llx → ret"
                                         " insns=%llu\n",
                                    (unsigned long long)s_kWfcPA,
                                    (unsigned long long)g_cWasmVirtualInstructions);
                                RTStrmFlush(g_pStdOut);
                            }

                            /* Phase 2: Restore original byte once tasks exist */
                            if (s_fWfcPatched && !s_fWfcRestored
                                && uTN != kTasksSelf)
                            {
                                PGMPhysSimpleWriteGCPhys(pVMfc, s_kWfcPA,
                                    &s_origByte, 1);
                                s_fWfcRestored = true;
                                RTPrintf("[FC4G] RESTORED WFC (0x%02x)"
                                         " insns=%llu\n",
                                    s_origByte,
                                    (unsigned long long)g_cWasmVirtualInstructions);
                                RTStrmFlush(g_pStdOut);
                            }

                            /* Phase 3: Timeout injection — replace
                             * MAX_SCHEDULE_TIMEOUT with 1 (single jiffy).
                             * This makes WFC timeout after ~1 PIT tick (~25K insns)
                             * instead of deadlocking. Much faster than 100 jiffies. */
                            if (s_fWfcRestored)
                            {
                                uint64_t uRsp = pVCpu->cpum.GstCtx.rsp;
                                if (uRsp >= UINT64_C(0xffff888000000000))
                                {
                                    uint64_t uPhysRsp = uRsp
                                        - UINT64_C(0xffff888000000000);
                                    uint64_t stkBuf[256];
                                    __builtin_memset(stkBuf, 0, sizeof(stkBuf));
                                    PGMPhysSimpleReadGCPhys(pVMfc, stkBuf,
                                        (RTGCPHYS)uPhysRsp, sizeof(stkBuf));

                                    const uint64_t kMaxTO =
                                        UINT64_C(0x7fffffffffffffff);
                                    for (int _i = 0; _i < 256; _i++)
                                    {
                                        if (stkBuf[_i] == kMaxTO)
                                        {
                                            uint64_t shortTO = UINT64_C(1);
                                            RTGCPHYS stkSlotPA = (RTGCPHYS)(
                                                uPhysRsp + _i * 8);
                                            PGMPhysSimpleWriteGCPhys(pVMfc,
                                                stkSlotPA, &shortTO, 8);
                                            s_cTimeoutInjections++;
                                            if ((s_cTimeoutInjections % 5000) == 1)
                                            {
                                                RTPrintf("[FC4G] timeout #%u"
                                                    " insns=%llu stk[%d]\n",
                                                    s_cTimeoutInjections,
                                                    (unsigned long long)
                                                        g_cWasmVirtualInstructions,
                                                    _i);
                                                RTStrmFlush(g_pStdOut);
                                            }
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }

                    /* ── INIT-SECTION-ENTRY: fire once when EIP first enters __init text (≥0xffffffff82000000).
                     * This tells us if rest_init / arch_call_rest_init / kernel_init_freeable runs. */
                    if (!g_fProductionMode && (pVCpu->cpum.GstCtx.msrEFER & MSR_K6_EFER_LMA))
                    {
                        static bool s_fInitSectionSeen = false;
                        uint64_t uEipNow = pVCpu->cpum.GstCtx.rip;
                        if (!s_fInitSectionSeen
                            && uEipNow >= UINT64_C(0xffffffff82000000)
                            && uEipNow <= UINT64_C(0xffffffff83000000))
                        {
                            s_fInitSectionSeen = true;
                            /* Dump 32 bytes of stack at current RSP */
                            uint64_t uRspI = pVCpu->cpum.GstCtx.rsp;
                            uint64_t uPhysRspI = uRspI - UINT64_C(0xffff888000000000);
                            uint64_t stkI[4] = {0};
                            PVMCC pVMi = pVCpu->CTX_SUFF(pVM);
                            PGMPhysSimpleReadGCPhys(pVMi, stkI, (RTGCPHYS)uPhysRspI, 32);
                            RTPrintf("[INIT-ENTRY] insns=%llu EIP=%#llx RSP=%#llx\n",
                                (unsigned long long)g_cWasmVirtualInstructions,
                                (unsigned long long)uEipNow,
                                (unsigned long long)uRspI);
                            RTPrintf("[INIT-ENTRY]   stk: %llx %llx %llx %llx\n",
                                (unsigned long long)stkI[0], (unsigned long long)stkI[1],
                                (unsigned long long)stkI[2], (unsigned long long)stkI[3]);
                            RTStrmFlush(g_pStdOut);
                        }
                    }

                    /* ── EARLY-RIP: periodic EIP snapshot every 200M insns from 1.5B onward.
                     * Also fires at milestones before 1.5B.
                     * Shows what the kernel is doing; dumps call stack to find stuck loop. */
                    if (!g_fProductionMode) {
                        static uint64_t s_cNextEarlyRip = UINT64_C(100000000); /* 100M */
                        static int s_nEarlyRipShot = 0;
                        if ((pVCpu->cpum.GstCtx.msrEFER & MSR_K6_EFER_LMA)
                            && g_cWasmVirtualInstructions >= s_cNextEarlyRip)
                        {
                            s_nEarlyRipShot++;
                            /* Next fire: every 100M instructions */
                            s_cNextEarlyRip = g_cWasmVirtualInstructions + UINT64_C(100000000);
                            /* Read tasks.next (init_task + 0x310 = phys 0x24117d0) */
                            PVMCC pVMer = pVCpu->CTX_SUFF(pVM);
                            uint64_t uTN = 0;
                            PGMPhysSimpleReadGCPhys(pVMer, &uTN, (RTGCPHYS)UINT64_C(0x24117d0), 8);
                            /* Read current_task from per-CPU (phys 0x7c15d00) */
                            uint64_t uCT = 0;
                            PGMPhysSimpleReadGCPhys(pVMer, &uCT, (RTGCPHYS)UINT64_C(0x7c15d00), 8);
                            bool fTasksSelf = (uTN == UINT64_C(0xffffffff824117d0));
                            bool fCTisInit  = (uCT == UINT64_C(0xffffffff824114c0));
                            uint64_t uRsp = pVCpu->cpum.GstCtx.rsp;
                            RTPrintf("[EARLY-RIP] #%d insns=%llu EIP=%#llx RSP=%#llx tasks=%s cur=%s\n",
                                s_nEarlyRipShot,
                                (unsigned long long)g_cWasmVirtualInstructions,
                                (unsigned long long)pVCpu->cpum.GstCtx.rip,
                                (unsigned long long)uRsp,
                                fTasksSelf ? "singleton" : "HAS-TASKS",
                                fCTisInit ? "swapper" : "OTHER");
                            /* Dump key registers to find completion pointer */
                            RTPrintf("[EARLY-RIP]   RBP=%#llx RDI=%#llx RBX=%#llx\n",
                                (unsigned long long)pVCpu->cpum.GstCtx.rbp,
                                (unsigned long long)pVCpu->cpum.GstCtx.rdi,
                                (unsigned long long)pVCpu->cpum.GstCtx.rbx);
                            RTPrintf("[EARLY-RIP]   R12=%#llx R13=%#llx R14=%#llx R15=%#llx\n",
                                (unsigned long long)pVCpu->cpum.GstCtx.r12,
                                (unsigned long long)pVCpu->cpum.GstCtx.r13,
                                (unsigned long long)pVCpu->cpum.GstCtx.r14,
                                (unsigned long long)pVCpu->cpum.GstCtx.r15);
                            /* Dump 16 bytes of code at the two return addresses */
                            {
                                uint8_t codeBuf[16] = {0};
                                PGMPhysSimpleReadGCPtr(pVCpu, codeBuf,
                                    (RTGCPTR)UINT64_C(0xffffffff81081270), 16);
                                RTPrintf("[EARLY-RIP]   code@81081270: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                                    codeBuf[0], codeBuf[1], codeBuf[2], codeBuf[3],
                                    codeBuf[4], codeBuf[5], codeBuf[6], codeBuf[7],
                                    codeBuf[8], codeBuf[9], codeBuf[10], codeBuf[11],
                                    codeBuf[12], codeBuf[13], codeBuf[14], codeBuf[15]);
                                PGMPhysSimpleReadGCPtr(pVCpu, codeBuf,
                                    (RTGCPTR)UINT64_C(0xffffffff810c13a0), 16);
                                RTPrintf("[EARLY-RIP]   code@810c13a0: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                                    codeBuf[0], codeBuf[1], codeBuf[2], codeBuf[3],
                                    codeBuf[4], codeBuf[5], codeBuf[6], codeBuf[7],
                                    codeBuf[8], codeBuf[9], codeBuf[10], codeBuf[11],
                                    codeBuf[12], codeBuf[13], codeBuf[14], codeBuf[15]);
                            }
                            /* Dump 8 stack QWORDs (call chain) */
                            uint64_t uPhysRsp = uRsp - UINT64_C(0xffff888000000000);
                            uint64_t stk[8] = {0};
                            PGMPhysSimpleReadGCPhys(pVMer, stk, (RTGCPHYS)uPhysRsp, 64);
                            RTPrintf("[EARLY-RIP]   stk: %llx %llx %llx %llx\n",
                                (unsigned long long)stk[0], (unsigned long long)stk[1],
                                (unsigned long long)stk[2], (unsigned long long)stk[3]);
                            RTPrintf("[EARLY-RIP]   stk: %llx %llx %llx %llx\n",
                                (unsigned long long)stk[4], (unsigned long long)stk[5],
                                (unsigned long long)stk[6], (unsigned long long)stk[7]);
                            /* Dump 16 bytes at EIP to identify the function */
                            uint64_t uEip = pVCpu->cpum.GstCtx.rip;
                            uint64_t uPhysEip = (uEip > UINT64_C(0xffffffff80000000))
                                              ? (uEip - UINT64_C(0xffffffff80000000))
                                              : (uEip - UINT64_C(0xffff888000000000));
                            uint8_t ibytes[16] = {0};
                            PGMPhysSimpleReadGCPhys(pVMer, ibytes, (RTGCPHYS)uPhysEip, 16);
                            RTPrintf("[EARLY-RIP]   code@EIP: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                                ibytes[0], ibytes[1], ibytes[2], ibytes[3],
                                ibytes[4], ibytes[5], ibytes[6], ibytes[7],
                                ibytes[8], ibytes[9], ibytes[10], ibytes[11],
                                ibytes[12], ibytes[13], ibytes[14], ibytes[15]);
                            /* Read init_task.tasks.prev (phys 0x24117d8) to check both pointers */
                            uint64_t uTasksPrev = 0;
                            PGMPhysSimpleReadGCPhys(pVMer, &uTasksPrev, (RTGCPHYS)UINT64_C(0x24117d8), 8);
                            /* GS base = per-CPU area base for CPU0 */
                            uint64_t uGsBase = pVCpu->cpum.GstCtx.gs.u64Base;
                            RTPrintf("[EARLY-RIP]   tasks.prev=%llx current_task=%llx gsbase=%llx\n",
                                (unsigned long long)uTasksPrev,
                                (unsigned long long)uCT,
                                (unsigned long long)uGsBase);
                            /* Scan stk[0..7] + stk2 for a completion ptr in kernel init range,
                             * dump 32 bytes from it to see done+lock+wait.head.next+prev. */
                            RTPrintf("[EARLY-RIP]   tasks.next=%llx\n", (unsigned long long)uTN);
                            /* Always dump 32 bytes from VA 0xffffffff824b8280 (suspected completion) */
                            {
                                uint64_t uComp[4] = {0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL,
                                                     0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL};
                                PGMPhysSimpleReadGCPtr(pVCpu, uComp, (RTGCPTR)UINT64_C(0xffffffff824b8280), 32);
                                RTPrintf("[EARLY-RIP]   comp0: %016llx %016llx\n",
                                    (unsigned long long)uComp[0], (unsigned long long)uComp[1]);
                                RTPrintf("[EARLY-RIP]   comp1: %016llx %016llx\n",
                                    (unsigned long long)uComp[2], (unsigned long long)uComp[3]);
                            }
                            /* Scan stk for init-range return address, dump CALL instruction BEFORE it */
                            for (int _i = 0; _i < 8; _i++)
                            {
                                if (stk[_i] >= UINT64_C(0xffffffff82000000)
                                    && stk[_i] <= UINT64_C(0xffffffff82c00000))
                                {
                                    /* Read 16 bytes BEFORE the return addr to decode the CALL insn */
                                    uint8_t preBytes[16] = {0};
                                    PGMPhysSimpleReadGCPtr(pVCpu, preBytes,
                                        (RTGCPTR)(stk[_i] - 16), 16);
                                    RTPrintf("[EARLY-RIP]   pre@stk[%d]=%llx: "
                                        "%02x %02x %02x %02x %02x %02x %02x %02x "
                                        "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                                        _i, (unsigned long long)stk[_i],
                                        preBytes[0], preBytes[1], preBytes[2], preBytes[3],
                                        preBytes[4], preBytes[5], preBytes[6], preBytes[7],
                                        preBytes[8], preBytes[9], preBytes[10], preBytes[11],
                                        preBytes[12], preBytes[13], preBytes[14], preBytes[15]);
                                    /* Also read 16 bytes FROM the return addr (code after the call) */
                                    uint8_t postBytes[16] = {0};
                                    PGMPhysSimpleReadGCPtr(pVCpu, postBytes,
                                        (RTGCPTR)stk[_i], 16);
                                    RTPrintf("[EARLY-RIP]   post@stk[%d]=%llx: "
                                        "%02x %02x %02x %02x %02x %02x %02x %02x "
                                        "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                                        _i, (unsigned long long)stk[_i],
                                        postBytes[0], postBytes[1], postBytes[2], postBytes[3],
                                        postBytes[4], postBytes[5], postBytes[6], postBytes[7],
                                        postBytes[8], postBytes[9], postBytes[10], postBytes[11],
                                        postBytes[12], postBytes[13], postBytes[14], postBytes[15]);
                                    break;
                                }
                            }
                            /* Read 256 more bytes up the stack to find IRQ interrupt frame */
                            {
                                uint64_t stkUp[32];
                                __builtin_memset(stkUp, 0, sizeof(stkUp));
                                PGMPhysSimpleReadGCPhys(pVMer, stkUp,
                                    (RTGCPHYS)(uPhysRsp + 128), 256);
                                /* Scan for CS=0x10 (kernel code segment) = interrupt frame */
                                for (int _j = 1; _j < 31; _j++)
                                {
                                    if (stkUp[_j] == UINT64_C(0x10)
                                        && _j > 0
                                        && stkUp[_j-1] >= UINT64_C(0xffffffff81000000)
                                        && stkUp[_j-1] <= UINT64_C(0xffffffff83000000))
                                    {
                                        /* Found: RIP=stkUp[j-1], CS=stkUp[j],
                                         * RFLAGS=stkUp[j+1], RSP=stkUp[j+2], SS=stkUp[j+3] */
                                        RTPrintf("[EARLY-RIP]   IRQ-FRAME@+%d: "
                                            "RIP=%#llx CS=%#llx RFLAGS=%#llx RSP=%#llx SS=%#llx\n",
                                            (int)(128 + _j * 8),
                                            (unsigned long long)stkUp[_j-1],
                                            (unsigned long long)stkUp[_j],
                                            _j+1 < 32 ? (unsigned long long)stkUp[_j+1] : 0ULL,
                                            _j+2 < 32 ? (unsigned long long)stkUp[_j+2] : 0ULL,
                                            _j+3 < 32 ? (unsigned long long)stkUp[_j+3] : 0ULL);
                                        break;
                                    }
                                }
                            }
                            RTStrmFlush(g_pStdOut);
                        }
                    }

                    /* ── RSP-page change detector at 50M granularity (post-2B) ──
                     * Fires every 50M instructions once in 64-bit kernel mode.
                     * Logs when RSP moves to a different 4K page, which indicates
                     * a context switch away from the idle thread stack. */
                    if (!g_fProductionMode
                        && (pVCpu->cpum.GstCtx.msrEFER & MSR_K6_EFER_LMA)
                        && g_cWasmVirtualInstructions >= UINT64_C(2000000000))
                    {
                        static uint64_t s_cNextRspCheck = UINT64_C(2000000000);
                        static uint64_t s_uLastRspPage = 0;
                        if (g_cWasmVirtualInstructions >= s_cNextRspCheck)
                        {
                            s_cNextRspCheck = g_cWasmVirtualInstructions + UINT64_C(50000000);
                            uint64_t rspNow = pVCpu->cpum.GstCtx.rsp;
                            uint64_t rspPage = rspNow & ~UINT64_C(0xFFF);
                            if (s_uLastRspPage == 0)
                                s_uLastRspPage = rspPage;
                            if (rspPage != s_uLastRspPage && rspNow > UINT64_C(0xffff800000000000))
                            {
                                RTPrintf("[CTX-SWITCH!] insns=%llu RSP=%#llx RIP=%#llx (was page %#llx)\n",
                                    (unsigned long long)g_cWasmVirtualInstructions,
                                    (unsigned long long)rspNow,
                                    (unsigned long long)pVCpu->cpum.GstCtx.rip,
                                    (unsigned long long)s_uLastRspPage);
                                RTStrmFlush(g_pStdOut);
                                s_uLastRspPage = rspPage;
                            }
                        }
                    }

                    static uint64_t s_cNextDiag = UINT64_C(100000000); /* 100M insns: first fire */
                    if (!g_fProductionMode && g_cWasmVirtualInstructions >= s_cNextDiag)
                    {
                        s_cNextDiag = g_cWasmVirtualInstructions
                                    + (s_uDelayRip ? UINT64_C(1000000000) : UINT64_C(500000000));
                        uint64_t fFFs = pVCpu->fLocalForcedActions;
                        RTPrintf("[IEM-DIAG] insns=%llu CR2=%#llx CR0=%#llx EFER=%#llx IF=%d FFs=%#RX64 EIP=%#llx RSP=%#llx\n",
                                 (unsigned long long)g_cWasmVirtualInstructions,
                                 (unsigned long long)pVCpu->cpum.GstCtx.cr2,
                                 (unsigned long long)pVCpu->cpum.GstCtx.cr0,
                                 (unsigned long long)pVCpu->cpum.GstCtx.msrEFER,
                                 !!(pVCpu->cpum.GstCtx.eflags.u & X86_EFL_IF),
                                 fFFs,
                                 (unsigned long long)pVCpu->cpum.GstCtx.rip,
                                 (unsigned long long)pVCpu->cpum.GstCtx.rsp);
                        RTPrintf("[IEM-DIAG] IDTR base=%#llx size=%u GDTR base=%#llx\n",
                                 (unsigned long long)pVCpu->cpum.GstCtx.idtr.pIdt,
                                 (unsigned)pVCpu->cpum.GstCtx.idtr.cbIdt,
                                 (unsigned long long)pVCpu->cpum.GstCtx.gdtr.pGdt);

                        RTPrintf("[IEM-DIAG-A] post-IDTR insns=%llu\n",
                            (unsigned long long)g_cWasmVirtualInstructions);
                        /* Monitor jiffies_64 at physical 0x02406980 (jiffies virtual 0xffffffff82406980).
                         * The Linux 5.4 FossaPup64 kernel stores jiffies_64 here.
                         * If jiffies doesn't advance, timer-wait loops spin forever.
                         * Use PGMPhysRead (physical) to bypass page-table translation issues. */
                        if ((pVCpu->cpum.GstCtx.msrEFER & MSR_K6_EFER_LMA)
                            && pVCpu->cpum.GstCtx.rip > UINT64_C(0xFFFF800000000000))
                        {
                            static uint64_t s_uPrevJiffies = 0;
                            static unsigned s_cJiffiesStuck = 0;
                            static unsigned s_cJiffiesChecks = 0;
                            uint64_t uJiffies = 0;
                            PVMCC pVMJ = pVCpu->CTX_SUFF(pVM);
                            VBOXSTRICTRC rcJ = PGMPhysRead(pVMJ, UINT64_C(0x02406980),
                                &uJiffies, sizeof(uJiffies), PGMACCESSORIGIN_DEBUGGER);
                            s_cJiffiesChecks++;
                            if (RT_SUCCESS(rcJ))
                            {
                                if (uJiffies != s_uPrevJiffies)
                                {
                                    RTPrintf("[JIFFIES] #%u jiffies_64=0x%llx (was 0x%llx, delta=%lld)\n",
                                        s_cJiffiesChecks,
                                        (unsigned long long)uJiffies,
                                        (unsigned long long)s_uPrevJiffies,
                                        (long long)(uJiffies - s_uPrevJiffies));
                                    s_uPrevJiffies = uJiffies;
                                    s_cJiffiesStuck = 0;
                                }
                                else
                                {
                                    s_cJiffiesStuck++;
                                    /* If jiffies hasn't changed for 5 consecutive 100M-insn intervals
                                     * (500M insns), force-advance it to unblock spin-wait loops. */
                                    if (s_cJiffiesStuck >= 5)
                                    {
                                        uint64_t uNewJiffies = uJiffies + 1;
                                        VBOXSTRICTRC rcW = PGMPhysWrite(pVMJ, UINT64_C(0x02406980),
                                            &uNewJiffies, sizeof(uNewJiffies), PGMACCESSORIGIN_DEBUGGER);
                                        RTPrintf("[JIFFIES-FIX] #%u stuck %u intervals, forcing 0x%llx->0x%llx rc=%d\n",
                                            s_cJiffiesChecks, s_cJiffiesStuck,
                                            (unsigned long long)uJiffies,
                                            (unsigned long long)uNewJiffies,
                                            (int)VBOXSTRICTRC_VAL(rcW));
                                        s_uPrevJiffies = uNewJiffies;
                                        s_cJiffiesStuck = 0;
                                    }
                                    else
                                        RTPrintf("[JIFFIES] #%u stuck at 0x%llx (%u intervals)\n",
                                            s_cJiffiesChecks,
                                            (unsigned long long)uJiffies, s_cJiffiesStuck);
                                }
                            }
                            else
                                RTPrintf("[JIFFIES] #%u PGMPhysRead failed rc=%d\n",
                                    s_cJiffiesChecks, (int)VBOXSTRICTRC_VAL(rcJ));
                        }

                        /* ── One-shot printk ring buffer scan ──
                         * Fires once at 3000M insns while kernel is in 64-bit mode.
                         * Scans BSS area (phys 0x2600000-0x2826000) for printable strings
                         * to reveal if kernel_init has logged anything not yet flushed
                         * to the UART serial console. */
                        {
                            static bool s_fDmesgDumped = false;
                            if (!s_fDmesgDumped
                                && g_cWasmVirtualInstructions >= UINT64_C(3000000000)
                                && (pVCpu->cpum.GstCtx.msrEFER & MSR_K6_EFER_LMA))
                            {
                                s_fDmesgDumped = true;
                                RTPrintf("[DMESG-SCAN] === Scanning BSS for printk messages ===\n");
                                PVMCC pVMd = pVCpu->CTX_SUFF(pVM);
                                int cLines = 0;
                                /* BSS starts at phys 0x2600000 based on kernel segment layout */
                                for (uint64_t phys = 0x02600000ULL; phys < 0x02826000ULL && cLines < 300; phys += 0x1000)
                                {
                                    char szBuf[0x1000];
                                    RT_ZERO(szBuf);
                                    int rc2 = PGMPhysSimpleReadGCPhys(pVMd, szBuf, (RTGCPHYS)phys, sizeof(szBuf));
                                    if (RT_SUCCESS(rc2))
                                    {
                                        int runStart = -1;
                                        for (int off = 0; off <= (int)sizeof(szBuf); off++)
                                        {
                                            bool printable = (off < (int)sizeof(szBuf))
                                                          && ((uint8_t)szBuf[off] >= 0x20
                                                          && (uint8_t)szBuf[off] <= 0x7e);
                                            if (printable && runStart < 0)
                                                runStart = off;
                                            else if (!printable && runStart >= 0)
                                            {
                                                int runLen = off - runStart;
                                                if (runLen >= 10 && cLines < 300)
                                                {
                                                    char szLine[256];
                                                    int copyLen = runLen < (int)sizeof(szLine) - 1 ? runLen : (int)sizeof(szLine) - 1;
                                                    memcpy(szLine, &szBuf[runStart], copyLen);
                                                    szLine[copyLen] = '\0';
                                                    /* Filter: only print if it looks like a kernel message */
                                                    bool fLooksKernelish = false;
                                                    for (int j = 0; j < copyLen - 2; j++)
                                                        if (szLine[j] == ':' || szLine[j] == '/' || szLine[j] == '.')
                                                        { fLooksKernelish = true; break; }
                                                    if (fLooksKernelish)
                                                    {
                                                        RTPrintf("[DMESG-SCAN] phys=%#llx: %s\n",
                                                            (unsigned long long)(phys + runStart), szLine);
                                                        cLines++;
                                                    }
                                                }
                                                runStart = -1;
                                            }
                                        }
                                    }
                                }
                                RTPrintf("[DMESG-SCAN] === Done (%d lines found) ===\n", cLines);
                                /* Also sample the stack at current RSP to get partial call trace */
                                if (pVCpu->cpum.GstCtx.rsp > UINT64_C(0xffff888000000000))
                                {
                                    uint64_t aStk[16];
                                    RT_ZERO(aStk);
                                    PGMPhysSimpleReadGCPtr(pVCpu, aStk, pVCpu->cpum.GstCtx.rsp, sizeof(aStk));
                                    RTPrintf("[DMESG-STK] RSP=%#llx: %#llx %#llx %#llx %#llx\n",
                                        (unsigned long long)pVCpu->cpum.GstCtx.rsp,
                                        (unsigned long long)aStk[0], (unsigned long long)aStk[1],
                                        (unsigned long long)aStk[2], (unsigned long long)aStk[3]);
                                    RTPrintf("[DMESG-STK]           %#llx %#llx %#llx %#llx\n",
                                        (unsigned long long)aStk[4], (unsigned long long)aStk[5],
                                        (unsigned long long)aStk[6], (unsigned long long)aStk[7]);
                                }
                                RTStrmFlush(g_pStdOut);
                            }
                        }

                        /* ── Periodic STUCK-2B diagnostic ──
                         * Fires every 500M insns starting at 2B, while in 64-bit mode.
                         * Dumps RSP, 32 stack words, and RBP frame chain to identify
                         * what function the kernel is stuck in after SSB message. */
                        RTPrintf("[STUCK-2B-REACH] code reached insns=%llu\n",
                            (unsigned long long)g_cWasmVirtualInstructions);
                        {
                            static uint64_t s_cStuck2BNext = UINT64_C(2000000000);
                            if (g_cWasmVirtualInstructions >= s_cStuck2BNext
                                && (pVCpu->cpum.GstCtx.msrEFER & MSR_K6_EFER_LMA))
                            {
                                s_cStuck2BNext += UINT64_C(500000000);
                                uint64_t rsp = pVCpu->cpum.GstCtx.rsp;
                                uint64_t rbp = pVCpu->cpum.GstCtx.rbp;
                                uint64_t rip = pVCpu->cpum.GstCtx.rip;
                                int      fIF = !!(pVCpu->cpum.GstCtx.rflags.u & X86_EFL_IF);
                                RTPrintf("[STUCK-2B] insns=%llu RIP=%#llx RSP=%#llx RBP=%#llx IF=%d\n",
                                    (unsigned long long)g_cWasmVirtualInstructions,
                                    (unsigned long long)rip,
                                    (unsigned long long)rsp,
                                    (unsigned long long)rbp,
                                    fIF);
                                /* Dump 32 words from stack */
                                if (rsp > UINT64_C(0xffff888000000000))
                                {
                                    uint64_t aStk[32];
                                    RT_ZERO(aStk);
                                    PVMCC pVMs = pVCpu->CTX_SUFF(pVM);
                                    PGMPhysSimpleReadGCPtr(pVCpu, aStk, rsp, sizeof(aStk));
                                    for (int si = 0; si < 32; si += 4)
                                        RTPrintf("[STUCK-2B]  stk+%03u: %#llx %#llx %#llx %#llx\n",
                                            (unsigned)(si * 8),
                                            (unsigned long long)aStk[si],   (unsigned long long)aStk[si+1],
                                            (unsigned long long)aStk[si+2], (unsigned long long)aStk[si+3]);
                                    /* Walk RBP frame chain — up to 12 frames */
                                    RTPrintf("[STUCK-2B]  RBP chain:");
                                    uint64_t fp = rbp;
                                    for (int fi = 0; fi < 12 && fp > UINT64_C(0xffff888000000000); fi++)
                                    {
                                        uint64_t frame[2] = {0, 0}; /* [saved_rbp, ret_addr] */
                                        int rcF = PGMPhysSimpleReadGCPtr(pVCpu, frame, fp, sizeof(frame));
                                        if (RT_FAILURE(rcF)) break;
                                        RTPrintf(" %#llx", (unsigned long long)frame[1]);
                                        fp = frame[0];
                                    }
                                    RTPrintf("\n");
                                    (void)pVMs;
                                }
                                /* ── GS.base per-CPU diagnostic ── */
                                {
                                    uint64_t gsBase = pVCpu->cpum.GstCtx.gs.u64Base;
                                    uint64_t fsBase = pVCpu->cpum.GstCtx.fs.u64Base;
                                    RTPrintf("[STUCK-GS] GS.base=%#llx FS.base=%#llx\n",
                                        (unsigned long long)gsBase,
                                        (unsigned long long)fsBase);
                                    /* Dump first 32 qwords of per-CPU area to find current_task */
                                    if (gsBase > UINT64_C(0xffff800000000000) && gsBase < UINT64_C(0xffffffffffffffff))
                                    {
                                        uint64_t aCpuData[32];
                                        RT_ZERO(aCpuData);
                                        PGMPhysSimpleReadGCPtr(pVCpu, aCpuData, gsBase, sizeof(aCpuData));
                                        for (int ii = 0; ii < 32; ii += 4)
                                            RTPrintf("[STUCK-GS-PCPU] +%3u: %#llx %#llx %#llx %#llx\n",
                                                (unsigned)(ii * 8),
                                                (unsigned long long)aCpuData[ii],   (unsigned long long)aCpuData[ii+1],
                                                (unsigned long long)aCpuData[ii+2], (unsigned long long)aCpuData[ii+3]);
                                    }
                                }
                                /* ── One-shot: read instruction bytes at hot idle-loop address ── */
                                {
                                    static bool s_fIdleBytesDumped = false;
                                    if (!s_fIdleBytesDumped && rip > UINT64_C(0xffffffff81000000))
                                    {
                                        s_fIdleBytesDumped = true;
                                        /* Read 128 bytes centred on RIP-64 to capture the loop */
                                        uint64_t uScanBase = rip > 64 ? rip - 64 : rip;
                                        uint8_t abScan[192];
                                        RT_ZERO(abScan);
                                        PGMPhysSimpleReadGCPtr(pVCpu, abScan, uScanBase, sizeof(abScan));
                                        RTPrintf("[IDLE-BYTES] RIP=%#llx scan from %#llx:\n",
                                            (unsigned long long)rip, (unsigned long long)uScanBase);
                                        for (int bb = 0; bb < (int)sizeof(abScan); bb += 16)
                                            RTPrintf("[IDLE-BYTES]  +%3u: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
                                                bb,
                                                abScan[bb+ 0], abScan[bb+ 1], abScan[bb+ 2], abScan[bb+ 3],
                                                abScan[bb+ 4], abScan[bb+ 5], abScan[bb+ 6], abScan[bb+ 7],
                                                abScan[bb+ 8], abScan[bb+ 9], abScan[bb+10], abScan[bb+11],
                                                abScan[bb+12], abScan[bb+13], abScan[bb+14], abScan[bb+15]);
                                        /* Also read at known hot address 0xffffffff81e00060-0xffffffff81e000c0 */
                                        uint8_t abHot[128];
                                        RT_ZERO(abHot);
                                        PGMPhysSimpleReadGCPtr(pVCpu, abHot, UINT64_C(0xffffffff81e00060), sizeof(abHot));
                                        RTPrintf("[IDLE-BYTES] Hot addr 0xffffffff81e00060:\n");
                                        for (int bb = 0; bb < (int)sizeof(abHot); bb += 16)
                                            RTPrintf("[IDLE-BYTES]  +%3u: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
                                                bb,
                                                abHot[bb+ 0], abHot[bb+ 1], abHot[bb+ 2], abHot[bb+ 3],
                                                abHot[bb+ 4], abHot[bb+ 5], abHot[bb+ 6], abHot[bb+ 7],
                                                abHot[bb+ 8], abHot[bb+ 9], abHot[bb+10], abHot[bb+11],
                                                abHot[bb+12], abHot[bb+13], abHot[bb+14], abHot[bb+15]);
                                    }
                                }
                                RTStrmFlush(g_pStdOut);
                            }
                        }

                        /* ── One-shot E820 / BIOS memory diagnostic ──
                         * Fires once at 5M instructions — by this point the BIOS has
                         * finished POST, programmed CMOS, and set up the IVT (including
                         * INT 15h for E820).  We dump:
                         *  1) IVT entry for INT 15h (should point to BIOS handler)
                         *  2) CMOS extended-memory registers (0x17-0x18, 0x30-0x31, 0x34-0x35)
                         *  3) PGM physical memory accessibility at key addresses
                         *  4) Scan for Linux boot_params header signature 'HdrS' (0x53726448)
                         *     at offset 0x202 from candidate base addresses
                         *  5) If boot_params found, dump E820 table from it */
                        {
                            static int s_cE820Dumps = 0;
                            /* Dump at 5M (for IVT/BDA), 100M (ISOLINUX loaded kernel),
                             * and when the decompressor exits (RIP leaves 0x02a2xxxx). */
                            bool fShouldDump = false;
                            if (s_cE820Dumps == 0 && g_cWasmVirtualInstructions >= 5000000)
                                fShouldDump = true;
                            else if (s_cE820Dumps == 1 && g_cWasmVirtualInstructions >= 100000000)
                                fShouldDump = true;
                            if (fShouldDump)
                            {
                                s_cE820Dumps++;
                                RTPrintf("[E820-EARLY] ═══ Dump #%d at insns=%llu ═══\n",
                                    s_cE820Dumps, (unsigned long long)g_cWasmVirtualInstructions);
                                PVMCC pVMRead = pVCpu->CTX_SUFF(pVM);

                                /* 1) IVT INT 15h vector at physical 0x54 */
                                uint8_t abIvt[4] = {0};
                                PGMPhysRead(pVMRead, 0x54, abIvt, 4, PGMACCESSORIGIN_DEBUGGER);
                                uint16_t int15off = abIvt[0] | (abIvt[1] << 8);
                                uint16_t int15seg = abIvt[2] | (abIvt[3] << 8);
                                RTPrintf("[E820-EARLY] INT 15h IVT vector: %04x:%04x (phys %#x)\n",
                                    int15seg, int15off, (int15seg << 4) + int15off);

                                /* 2) BDA base memory at 0x413 */
                                uint8_t abBda[2] = {0};
                                PGMPhysRead(pVMRead, 0x413, abBda, 2, PGMACCESSORIGIN_DEBUGGER);
                                RTPrintf("[E820-EARLY] BDA base memory: %u KB\n",
                                    (unsigned)(abBda[0] | (abBda[1] << 8)));

                                /* 3) PGM accessibility: try reading 4 bytes at key addresses */
                                static const uint64_t aTestAddr[] = {
                                    0x100000, 0xA00000, 0x3200000, 0x6400000, 0x7F00000
                                };
                                for (unsigned t = 0; t < RT_ELEMENTS(aTestAddr); t++)
                                {
                                    uint8_t abTest[4] = {0};
                                    int rc2 = PGMPhysRead(pVMRead, aTestAddr[t], abTest, 4, PGMACCESSORIGIN_DEBUGGER);
                                    RTPrintf("[E820-EARLY] PGM read at %#llx: rc=%d data=%02x%02x%02x%02x\n",
                                        (unsigned long long)aTestAddr[t], rc2,
                                        abTest[0], abTest[1], abTest[2], abTest[3]);
                                }

                                /* 4) Scan for Linux boot_params (signature 'HdrS' at offset 0x202)
                                 * ISOLINUX typically loads setup at various aligned addresses. */
                                static const uint64_t aBpCandidates[] = {
                                    0x10000, 0x15000, 0x18000, 0x1F000, 0x20000,
                                    0x7000, 0x8000, 0x90000, 0x96000, 0x97000,
                                    0x98000, 0x99000, 0x9A000
                                };
                                for (unsigned c = 0; c < RT_ELEMENTS(aBpCandidates); c++)
                                {
                                    uint8_t abSig[4] = {0};
                                    PGMPhysRead(pVMRead, aBpCandidates[c] + 0x202, abSig, 4,
                                        PGMACCESSORIGIN_DEBUGGER);
                                    uint32_t sig = abSig[0] | (abSig[1] << 8)
                                                 | (abSig[2] << 16) | (abSig[3] << 24);
                                    if (sig == 0x53726448) /* 'HdrS' */
                                    {
                                        RTPrintf("[E820-EARLY] boot_params found at %#llx (HdrS signature)\n",
                                            (unsigned long long)aBpCandidates[c]);
                                        /* Read e820_entries at offset 0x1E8 (1 byte) */
                                        uint8_t e820cnt = 0;
                                        PGMPhysRead(pVMRead, aBpCandidates[c] + 0x1E8, &e820cnt, 1,
                                            PGMACCESSORIGIN_DEBUGGER);
                                        RTPrintf("[E820-EARLY] e820_entries=%u\n", e820cnt);
                                        /* Read E820 table at offset 0x2D0 (each entry is 20 bytes) */
                                        for (unsigned e = 0; e < e820cnt && e < 32; e++)
                                        {
                                            uint8_t abEntry[20] = {0};
                                            PGMPhysRead(pVMRead, aBpCandidates[c] + 0x2D0 + e * 20,
                                                abEntry, 20, PGMACCESSORIGIN_DEBUGGER);
                                            uint64_t addr = *(uint64_t *)&abEntry[0];
                                            uint64_t size = *(uint64_t *)&abEntry[8];
                                            uint32_t type = *(uint32_t *)&abEntry[16];
                                            const char *pszType = type == 1 ? "usable"
                                                : type == 2 ? "reserved" : type == 3 ? "ACPI"
                                                : type == 4 ? "ACPI-NVS" : "unknown";
                                            RTPrintf("[E820-EARLY] e820[%u]: %#018llx - %#018llx (%llu KB) type=%u (%s)\n",
                                                e,
                                                (unsigned long long)addr,
                                                (unsigned long long)(addr + size),
                                                (unsigned long long)(size >> 10),
                                                type, pszType);
                                        }
                                    }
                                }

                                /* 5) Also scan for E820 entries in low memory (the BIOS stores them
                                 * in the BDA/EBDA area for its own INT 15h handler) */
                                /* Check if INT 15h works by reading the handler code */
                                uint8_t abHandler[16] = {0};
                                PGMPhysRead(pVMRead, (uint32_t)(int15seg << 4) + int15off,
                                    abHandler, 16, PGMACCESSORIGIN_DEBUGGER);
                                RTPrintf("[E820-EARLY] INT 15h handler code: ");
                                for (int i = 0; i < 16; i++)
                                    RTPrintf("%02x ", abHandler[i]);
                                RTPrintf("\n");

                                RTStrmFlush(g_pStdOut);
                            }
                        }

                        /* ── Decompressor-exit detector ──
                         * The kernel decompressor runs in the 0x02a20000-0x02a30000 range.
                         * When RIP moves outside that range while in long mode, the
                         * decompressed kernel has started. Dump boot_params E820 at that
                         * point since the kernel is about to parse it. */
                        {
                            static bool s_fInDecompressor = false;
                            static bool s_fDecompExitDumped = false;
                            uint64_t rip = pVCpu->cpum.GstCtx.rip;
                            bool fLongMode = !!(pVCpu->cpum.GstCtx.msrEFER & MSR_K6_EFER_LMA);

                            if (!s_fInDecompressor && fLongMode
                                && rip >= 0x02a20000 && rip < 0x02a30000)
                                s_fInDecompressor = true;

                            if (s_fInDecompressor && !s_fDecompExitDumped && fLongMode
                                && (rip < 0x02a20000 || rip >= 0x02a30000))
                            {
                                s_fDecompExitDumped = true;
                                RTPrintf("[DECOMP-EXIT] Decompressor finished! RIP=%#llx CR3=%#llx insns=%llu\n",
                                    (unsigned long long)rip,
                                    (unsigned long long)pVCpu->cpum.GstCtx.cr3,
                                    (unsigned long long)g_cWasmVirtualInstructions);

                                /* Scan for boot_params (HdrS signature) and inject E820 if empty.
                                 * ISOLINUX fails to populate boot_params.e820_table under VBox-Wasm
                                 * IEM emulation (likely because INT 15h AX=E820h doesn't work correctly
                                 * during ISOLINUX's real-mode phase).  Inject the memory map that the
                                 * BIOS *should* have returned for the configured RAM size. */
                                PVMCC pVMRead = pVCpu->CTX_SUFF(pVM);
                                static const uint64_t aBpCandidates[] = {
                                    0x10000, 0x15000, 0x18000, 0x1F000, 0x20000,
                                    0x7000, 0x8000, 0x90000, 0x96000, 0x97000,
                                    0x98000, 0x99000, 0x9A000, 0x100000
                                };
                                for (unsigned c = 0; c < RT_ELEMENTS(aBpCandidates); c++)
                                {
                                    uint8_t abSig[4] = {0};
                                    PGMPhysRead(pVMRead, aBpCandidates[c] + 0x202,
                                        abSig, 4, PGMACCESSORIGIN_DEBUGGER);
                                    uint32_t sig = abSig[0] | (abSig[1] << 8)
                                                 | (abSig[2] << 16) | (abSig[3] << 24);
                                    if (sig != 0x53726448) /* 'HdrS' */
                                        continue;

                                    uint64_t bpBase = aBpCandidates[c];
                                    RTPrintf("[DECOMP-EXIT] boot_params at %#llx\n",
                                        (unsigned long long)bpBase);

                                    uint8_t e820cnt = 0;
                                    PGMPhysRead(pVMRead, bpBase + 0x1E8,
                                        &e820cnt, 1, PGMACCESSORIGIN_DEBUGGER);
                                    RTPrintf("[DECOMP-EXIT] existing e820_entries=%u\n", e820cnt);

                                    if (e820cnt == 0)
                                    {
                                        /* ── Inject E820 memory map for 128 MB RAM ── */
                                        RTPrintf("[E820-FIX] Injecting E820 map (ISOLINUX failed to provide one)\n");

                                        /* Only entries within the 128MB RAM range — no high-address
                                         * entries (I/O APIC, Local APIC, BIOS ROM at 0xFECxxxxx+).
                                         * High-address entries cause the kernel to allocate struct page
                                         * for the entire 4GB address space (~1M pages), taking 70+ hours
                                         * under IEM.  With only 128MB entries, max_pfn = 32K pages. */
                                        struct { uint64_t addr; uint64_t size; uint32_t type; } __attribute__((packed))
                                        aEntries[] = {
                                            { UINT64_C(0x000000000000), UINT64_C(0x000000009FC00), 1 }, /* usable: 0-639KB */
                                            { UINT64_C(0x00000009FC00), UINT64_C(0x000000000400),  2 }, /* reserved: EBDA */
                                            { UINT64_C(0x0000000E8000), UINT64_C(0x000000018000),  2 }, /* reserved: BIOS area */
                                            { UINT64_C(0x000000100000), UINT64_C(0x000007EF0000),  1 }, /* usable: 1MB - ~127.9MB */
                                            { UINT64_C(0x000007FF0000), UINT64_C(0x000000010000),  3 }, /* ACPI reclaim: 64KB at top */
                                        };

                                        /* Write e820_entries count */
                                        uint8_t cnt = (uint8_t)RT_ELEMENTS(aEntries);
                                        PGMPhysWrite(pVMRead, bpBase + 0x1E8, &cnt, 1, PGMACCESSORIGIN_DEBUGGER);

                                        /* Write E820 table at offset 0x2D0 (each entry = 20 bytes) */
                                        for (unsigned e = 0; e < RT_ELEMENTS(aEntries); e++)
                                        {
                                            PGMPhysWrite(pVMRead, bpBase + 0x2D0 + e * 20,
                                                &aEntries[e], 20, PGMACCESSORIGIN_DEBUGGER);
                                            const char *pszType = aEntries[e].type == 1 ? "usable"
                                                : aEntries[e].type == 2 ? "reserved" : "ACPI";
                                            RTPrintf("[E820-FIX] e820[%u]: %#018llx - %#018llx (%llu KB) type=%u (%s)\n",
                                                e,
                                                (unsigned long long)aEntries[e].addr,
                                                (unsigned long long)(aEntries[e].addr + aEntries[e].size),
                                                (unsigned long long)(aEntries[e].size >> 10),
                                                aEntries[e].type, pszType);
                                        }

                                        /* Also set alt_mem_k at offset 0x1E0: extended memory in KB
                                         * = (128MB - 1MB) / 1024 = 130048 */
                                        uint32_t altMemK = 130048;
                                        PGMPhysWrite(pVMRead, bpBase + 0x1E0, &altMemK, 4, PGMACCESSORIGIN_DEBUGGER);
                                        RTPrintf("[E820-FIX] alt_mem_k=%u\n", altMemK);
                                    }
                                    else
                                    {
                                        /* E820 exists — just dump it */
                                        for (unsigned e = 0; e < e820cnt && e < 32; e++)
                                        {
                                            uint8_t abEntry[20] = {0};
                                            PGMPhysRead(pVMRead, bpBase + 0x2D0 + e * 20,
                                                abEntry, 20, PGMACCESSORIGIN_DEBUGGER);
                                            uint64_t eaddr = *(uint64_t *)&abEntry[0];
                                            uint64_t esize = *(uint64_t *)&abEntry[8];
                                            uint32_t etype = *(uint32_t *)&abEntry[16];
                                            RTPrintf("[DECOMP-EXIT] e820[%u]: %#018llx - %#018llx type=%u\n",
                                                e, (unsigned long long)eaddr,
                                                (unsigned long long)(eaddr + esize), etype);
                                        }
                                    }
                                    break; /* found boot_params, done */
                                }
                                RTStrmFlush(g_pStdOut);
                            }
                        }

                        /* ── Robust E820 injection fallback ──
                         * Runs every 1M diagnostic interval while in long mode.
                         * If boot_params at 0x10000 has HdrS but e820_entries==0,
                         * inject correct E820 entries.  This is a belt-and-suspenders
                         * fallback in case the decompressor-exit detection doesn't fire
                         * (e.g., different kernel with different decompressor address). */
                        {
                            static bool s_fE820Injected = false;
                            if (!s_fE820Injected
                                && (pVCpu->cpum.GstCtx.msrEFER & MSR_K6_EFER_LMA))
                            {
                                PVMCC pVMInj = pVCpu->CTX_SUFF(pVM);
                                uint8_t abSig[4] = {0};
                                PGMPhysRead(pVMInj, 0x10000 + 0x202, abSig, 4, PGMACCESSORIGIN_DEBUGGER);
                                uint32_t sig = abSig[0] | (abSig[1]<<8) | (abSig[2]<<16) | (abSig[3]<<24);
                                if (sig == 0x53726448) /* HdrS */
                                {
                                    uint8_t e820cnt = 0;
                                    PGMPhysRead(pVMInj, 0x10000 + 0x1E8, &e820cnt, 1, PGMACCESSORIGIN_DEBUGGER);
                                    if (e820cnt == 0)
                                    {
                                        s_fE820Injected = true;
                                        RTPrintf("[E820-FIX-FALLBACK] Long mode detected, boot_params e820 empty — injecting\n");

                                        struct { uint64_t addr; uint64_t size; uint32_t type; } __attribute__((packed))
                                        aE[] = {
                                            { UINT64_C(0x000000000000), UINT64_C(0x000000009FC00), 1 },
                                            { UINT64_C(0x00000009FC00), UINT64_C(0x000000000400),  2 },
                                            { UINT64_C(0x0000000E8000), UINT64_C(0x000000018000),  2 },
                                            { UINT64_C(0x000000100000), UINT64_C(0x000007EF0000),  1 },
                                            { UINT64_C(0x000007FF0000), UINT64_C(0x000000010000),  3 },
                                        };
                                        uint8_t cnt = (uint8_t)RT_ELEMENTS(aE);
                                        PGMPhysWrite(pVMInj, 0x10000 + 0x1E8, &cnt, 1, PGMACCESSORIGIN_DEBUGGER);
                                        for (unsigned e = 0; e < RT_ELEMENTS(aE); e++)
                                            PGMPhysWrite(pVMInj, 0x10000 + 0x2D0 + e * 20,
                                                &aE[e], 20, PGMACCESSORIGIN_DEBUGGER);
                                        uint32_t altMemK = 130048;
                                        PGMPhysWrite(pVMInj, 0x10000 + 0x1E0, &altMemK, 4, PGMACCESSORIGIN_DEBUGGER);
                                        RTPrintf("[E820-FIX-FALLBACK] Injected %u E820 entries + alt_mem_k=%u\n",
                                            cnt, altMemK);
                                        RTStrmFlush(g_pStdOut);
                                    }
                                    else
                                        s_fE820Injected = true; /* already has entries, don't check again */
                                }
                            }
                        }

                        /* FORCE-IF moved into the SAME-RIP block below so it only fires
                           when the kernel has been stuck at the SAME instruction for
                           many consecutive intervals (≥10 × 100M = 1B instructions).
                           Firing on any transient IF=0 (e.g., spinlock critical sections)
                           caused irq_work_run_list reentrancy → kernel panic. */

                        /* One-shot stack dump to identify stuck function — only after
                           kernel has entered decompressed code (high virtual addresses) */
                        {
                            static bool s_fStackDumped = false;
                            if (!s_fStackDumped
                                && (pVCpu->cpum.GstCtx.msrEFER & MSR_K6_EFER_LMA)
                                && !(pVCpu->cpum.GstCtx.eflags.u & X86_EFL_IF)
                                && pVCpu->cpum.GstCtx.rip > UINT64_C(0xFFFF800000000000))
                            {
                                s_fStackDumped = true;
                                RTPrintf("[STUCK-DIAG] RIP=%#018llx RSP=%#018llx RBP=%#018llx\n",
                                    (unsigned long long)pVCpu->cpum.GstCtx.rip,
                                    (unsigned long long)pVCpu->cpum.GstCtx.rsp,
                                    (unsigned long long)pVCpu->cpum.GstCtx.rbp);
                                RTPrintf("[STUCK-DIAG] RAX=%#018llx RBX=%#018llx RCX=%#018llx RDX=%#018llx\n",
                                    (unsigned long long)pVCpu->cpum.GstCtx.rax,
                                    (unsigned long long)pVCpu->cpum.GstCtx.rbx,
                                    (unsigned long long)pVCpu->cpum.GstCtx.rcx,
                                    (unsigned long long)pVCpu->cpum.GstCtx.rdx);
                                RTPrintf("[STUCK-DIAG] RSI=%#018llx RDI=%#018llx R8=%#018llx R9=%#018llx\n",
                                    (unsigned long long)pVCpu->cpum.GstCtx.r8,
                                    (unsigned long long)pVCpu->cpum.GstCtx.r9,
                                    (unsigned long long)pVCpu->cpum.GstCtx.r10,
                                    (unsigned long long)pVCpu->cpum.GstCtx.r11);
                                /* Dump stack: 16 qwords from RSP */
                                uint64_t auStack[16];
                                RT_ZERO(auStack);
                                PGMPhysSimpleReadGCPtr(pVCpu, auStack, pVCpu->cpum.GstCtx.rsp, sizeof(auStack));
                                for (int i = 0; i < 16; i++)
                                    RTPrintf("[STUCK-STK] RSP+%02x: %#018llx\n", i*8, (unsigned long long)auStack[i]);
                                /* Walk RBP chain (frame pointers) */
                                uint64_t rbp = pVCpu->cpum.GstCtx.rbp;
                                for (int i = 0; i < 10 && rbp > 0xffff800000000000ULL; i++)
                                {
                                    uint64_t frame[2] = {0, 0}; /* [0]=saved RBP, [1]=return addr */
                                    PGMPhysSimpleReadGCPtr(pVCpu, frame, rbp, sizeof(frame));
                                    RTPrintf("[STUCK-FRM] #%d RBP=%#018llx RET=%#018llx\n", i, (unsigned long long)rbp, (unsigned long long)frame[1]);
                                    if (frame[0] == 0 || frame[0] == rbp) break;
                                    rbp = frame[0];
                                }
                                /* Read 64 bytes of code at RIP */
                                uint8_t abCode[64];
                                RT_ZERO(abCode);
                                PGMPhysSimpleReadGCPtr(pVCpu, abCode, pVCpu->cpum.GstCtx.rip, sizeof(abCode));
                                RTPrintf("[STUCK-CODE] @RIP: ");
                                for (int i = 0; i < 64; i++)
                                    RTPrintf("%02x ", abCode[i]);
                                RTPrintf("\n");
                                /* Try to read kernel symbol near RIP by scanning for printable text */
                                for (uint64_t scan = pVCpu->cpum.GstCtx.rip - 0x100; scan < pVCpu->cpum.GstCtx.rip; scan++)
                                {
                                    char szBuf[256];
                                    RT_ZERO(szBuf);
                                    PGMPhysSimpleReadGCPtr(pVCpu, szBuf, scan, sizeof(szBuf) - 1);
                                    /* Look for function name pattern: \0funcname\0 */
                                    if (szBuf[0] == '\0' && szBuf[1] >= 'a' && szBuf[1] <= 'z')
                                    {
                                        int len = 0;
                                        while (len < 60 && szBuf[len+1] >= 0x20 && szBuf[len+1] <= 0x7e) len++;
                                        if (len >= 4)
                                        {
                                            RTPrintf("[STUCK-SYM] near RIP-%#llx: \"%.*s\"\n",
                                                (unsigned long long)(pVCpu->cpum.GstCtx.rip - scan), len, &szBuf[1]);
                                        }
                                    }
                                }
                            }
                        }

                        /* Detect stuck-at-same-RIP and accelerate known patterns.
                           When the same RIP appears for 20+ diagnostic intervals (20M insns),
                           check if the instruction pattern is a CRC32 loop.  If so, compute
                           the CRC natively in C++ instead of iterating byte-by-byte in IEM.
                           This saves ~40 minutes on the 26MB FossaPup64 kernel decompression. */
                        {
                            static uint64_t s_uPrevRip = 0;
                            static unsigned s_cSameRip = 0;
                            static uint64_t s_uLastAccelRip = 0;
                            uint64_t curRip = pVCpu->cpum.GstCtx.rip;
                            /* Use range check (±8 bytes) instead of exact match.
                             * A tight 2-instruction loop like dec rax; jnz causes the
                             * 1M-instruction sample to alternate between the two addresses,
                             * preventing exact match from ever reaching the threshold. */
                            int64_t ripDelta = (int64_t)(curRip - s_uPrevRip);
                            if (ripDelta >= -8 && ripDelta <= 8)
                                s_cSameRip++;
                            else
                            {
                                s_cSameRip = 0;
                                s_uPrevRip = curRip;
                            }

                            /* Log when first detected as stuck */
                            if (s_cSameRip == 3)
                            {
                                uint8_t abCode[32];
                                RT_ZERO(abCode);
                                PGMPhysSimpleReadGCPtr(pVCpu, abCode, curRip, sizeof(abCode));
                                RTPrintf("[SAME-RIP] RIP=%#018llx stuck for %u intervals, insns=%llu\n",
                                    (unsigned long long)curRip, s_cSameRip,
                                    (unsigned long long)g_cWasmVirtualInstructions);
                                RTPrintf("[SAME-RIP] code: ");
                                for (int i = 0; i < 32; i++)
                                    RTPrintf("%02x ", abCode[i]);
                                RTPrintf("\n");
                                RTPrintf("[SAME-RIP] RCX=%#018llx RSI=%#018llx RDI=%#018llx R8=%#018llx R9=%#018llx R12=%#018llx\n",
                                    (unsigned long long)pVCpu->cpum.GstCtx.rcx,
                                    (unsigned long long)pVCpu->cpum.GstCtx.rsi,
                                    (unsigned long long)pVCpu->cpum.GstCtx.rdi,
                                    (unsigned long long)pVCpu->cpum.GstCtx.r8,
                                    (unsigned long long)pVCpu->cpum.GstCtx.r9,
                                    (unsigned long long)pVCpu->cpum.GstCtx.r12);
                                RTStrmFlush(g_pStdOut);
                            }

                            /* CRC32 loop accelerator: fires at 20 intervals (20M insns at same RIP).
                             * Searches the code near the stuck RIP for the CRC32 pattern
                             * (xor ecx,[r12+r9*4]; cmp rdi,r8; jnz).  When found, computes
                             * the CRC from scratch over the entire output buffer in native C++
                             * code, then sets registers and advances RIP past the loop.
                             * This avoids mid-instruction state issues since the from-scratch
                             * CRC always matches the loop's final result. */
                            if (s_cSameRip >= 5 && curRip != s_uLastAccelRip)
                            {
                                /* Read 32 bytes at the stuck RIP to search for the CRC pattern */
                                uint8_t abCode[32];
                                RT_ZERO(abCode);
                                PGMPhysSimpleReadGCPtr(pVCpu, abCode, curRip, sizeof(abCode));

                                /* Search for: 43 33 0c 8c 4c 39 c7 75
                                 *   xor ecx,[r12+r9*4]; cmp rdi,r8; jnz */
                                int iPatOfs = -1;
                                for (int i = 0; i < 24; i++)
                                {
                                    if (abCode[i]   == 0x43 && abCode[i+1] == 0x33
                                        && abCode[i+2] == 0x0c && abCode[i+3] == 0x8c
                                        && abCode[i+4] == 0x4c && abCode[i+5] == 0x39
                                        && abCode[i+6] == 0xc7 && abCode[i+7] == 0x75)
                                    {
                                        iPatOfs = i;
                                        break;
                                    }
                                }

                                if (iPatOfs >= 0)
                                {
                                    s_uLastAccelRip = curRip;
                                    uint64_t r8  = pVCpu->cpum.GstCtx.r8;
                                    uint64_t rsi = pVCpu->cpum.GstCtx.rsi;
                                    uint64_t r12 = pVCpu->cpum.GstCtx.r12;

                                    /* Derive output buffer: R8 = output + output_len,
                                     * RSI = output_len → output_start = R8 - RSI */
                                    uint64_t outputStart = r8 - rsi;
                                    uint64_t outputLen   = rsi;

                                    /* RIP after loop: past XOR(4)+CMP(3)+JNZ(2) from pattern */
                                    uint64_t ripAfterLoop = curRip + (uint64_t)iPatOfs + 9;

                                    RTPrintf("[CRC-ACCEL] Detected CRC32 loop at RIP=%#llx (pattern at +%d)\n",
                                        (unsigned long long)curRip, iPatOfs);
                                    RTPrintf("[CRC-ACCEL] output=%#llx len=%llu r8=%#llx\n",
                                        (unsigned long long)outputStart, (unsigned long long)outputLen,
                                        (unsigned long long)r8);

                                    if (outputLen > 0 && outputLen < 64U * 1024U * 1024U)
                                    {
                                        /* Read CRC32 table from guest memory (R12, 256 entries) */
                                        uint32_t auTable[256];
                                        RT_ZERO(auTable);
                                        int rc = PGMPhysSimpleReadGCPtr(pVCpu, auTable, r12, sizeof(auTable));
                                        if (RT_SUCCESS(rc))
                                        {
                                            /* Compute CRC32 from scratch over the full output.
                                             * Initial CRC = 0xFFFFFFFF (standard gzip pre-condition).
                                             * Read in 64KB chunks to limit memory usage. */
                                            uint32_t crc = 0xFFFFFFFFU;
                                            const size_t cbChunk = 64U * 1024U;
                                            uint8_t *pbBuf = (uint8_t *)RTMemAlloc(cbChunk);
                                            bool fOk = pbBuf != NULL;

                                            if (fOk)
                                            {
                                                uint64_t addr = outputStart;
                                                while (addr < r8)
                                                {
                                                    size_t cbRead = (size_t)RT_MIN((uint64_t)cbChunk, r8 - addr);
                                                    rc = PGMPhysSimpleReadGCPtr(pVCpu, pbBuf, addr, cbRead);
                                                    if (RT_FAILURE(rc))
                                                    {
                                                        RTPrintf("[CRC-ACCEL] FAILED: PGMPhysRead at %#llx rc=%d\n",
                                                            (unsigned long long)addr, rc);
                                                        fOk = false;
                                                        break;
                                                    }
                                                    for (size_t i = 0; i < cbRead; i++)
                                                        crc = (crc >> 8) ^ auTable[(crc ^ pbBuf[i]) & 0xFF];
                                                    addr += cbRead;
                                                }
                                                RTMemFree(pbBuf);
                                            }

                                            if (fOk)
                                            {
                                                /* Update guest state: ECX = raw CRC (before NOT),
                                                 * RDI = R8 (loop exit condition), RIP past the loop */
                                                pVCpu->cpum.GstCtx.rcx = crc;
                                                pVCpu->cpum.GstCtx.rdi = r8;
                                                pVCpu->cpum.GstCtx.rip = ripAfterLoop;

                                                RTPrintf("[CRC-ACCEL] OK! CRC=%#x over %llu bytes, RIP→%#llx\n",
                                                    crc, (unsigned long long)outputLen,
                                                    (unsigned long long)ripAfterLoop);
                                            }
                                        }
                                        else
                                            RTPrintf("[CRC-ACCEL] FAILED: read CRC table at %#llx rc=%d\n",
                                                (unsigned long long)r12, rc);
                                    }
                                    else
                                        RTPrintf("[CRC-ACCEL] Skip: outputLen=%llu out of range\n",
                                            (unsigned long long)outputLen);
                                    RTStrmFlush(g_pStdOut);
                                }
                            }

                            /* __delay() loop accelerator: fires after 2M insns at same RIP.
                             * Detects the kernel's __delay() function: dec rax; jnz <-5>.
                             * Set RAX=1 so the loop exits on the next iteration.
                             * Fires repeatedly (every interval once threshold met) since
                             * __delay() is called many times at the same function address. */
                            if (s_cSameRip >= 2)
                            {
                                uint8_t abCode2[4];
                                RT_ZERO(abCode2);
                                PGMPhysSimpleReadGCPtr(pVCpu, abCode2, curRip, sizeof(abCode2));

                                if (abCode2[0] == 0x75 && abCode2[1] == 0xfb)
                                {
                                    /* JNZ -5: loop body is 3 bytes before this JNZ.
                                     * Read the 3-byte instruction to identify the counter register. */
                                    uint8_t abPrev[3];
                                    RT_ZERO(abPrev);
                                    PGMPhysSimpleReadGCPtr(pVCpu, abPrev, curRip - 3, sizeof(abPrev));

                                    const char *pszReg = NULL;
                                    if (abPrev[0] == 0x48 && abPrev[1] == 0xff && abPrev[2] == 0xc8)
                                    {   /* dec rax */
                                        pVCpu->cpum.GstCtx.rax = 1;
                                        pszReg = "RAX";
                                    }
                                    else if (abPrev[0] == 0x48 && abPrev[1] == 0xff && abPrev[2] == 0xc9)
                                    {   /* dec rcx */
                                        pVCpu->cpum.GstCtx.rcx = 1;
                                        pszReg = "RCX";
                                    }
                                    else if (abPrev[0] == 0xff && abPrev[2] == 0xc8)
                                    {   /* dec eax (no REX) */
                                        pVCpu->cpum.GstCtx.rax = 1;
                                        pszReg = "EAX";
                                    }

                                    if (pszReg)
                                    {
                                        /* Record for fast-path (1K-instruction interval) */
                                        if (s_uDelayRip == 0)
                                        {
                                            s_uDelayRip = curRip;
                                            RTPrintf("[DELAY-ACCEL] Learned delay RIP=%#llx, enabling fast-path\n",
                                                (unsigned long long)curRip);
                                            wasmDumpGuestCpuid(pVCpu);
                                        }
                                        /* Store caller info in globals (readable from JS via Module._wasmGetDelay*())
                                         * and log periodically.  Update every time so JS always sees current caller. */
                                        {
                                            static unsigned s_cDelayLogs = 0;
                                            s_cDelayLogs++;
                                            uint64_t auStk2[8];
                                            RT_ZERO(auStk2);
                                            PGMPhysSimpleReadGCPtr(pVCpu, auStk2, pVCpu->cpum.GstCtx.rsp, sizeof(auStk2));
                                            /* Always update globals for JS access */
                                            s_uDelayInfoRip = curRip;
                                            s_uDelayInfoRsp = pVCpu->cpum.GstCtx.rsp;
                                            s_uDelayInfoRbp = pVCpu->cpum.GstCtx.rbp;
                                            for (int si = 0; si < 8; si++)
                                                s_aDelayStack[si] = auStk2[si];
                                            s_fDelayInfoValid = 1;
                                            if (s_cDelayLogs <= 5 || (s_cDelayLogs % 100) == 0)
                                            {
                                                RTPrintf("[DELAY-ACCEL] #%u RIP=%#llx %s=1 insns=%llu\n",
                                                    s_cDelayLogs, (unsigned long long)curRip, pszReg,
                                                    (unsigned long long)g_cWasmVirtualInstructions);
                                                RTPrintf("[DELAY-STK] ret=%#llx caller=%#llx caller2=%#llx RSP=%#llx\n",
                                                    (unsigned long long)auStk2[0],
                                                    (unsigned long long)auStk2[1],
                                                    (unsigned long long)auStk2[2],
                                                    (unsigned long long)pVCpu->cpum.GstCtx.rsp);
                                            }
                                        }
                                        RTStrmFlush(g_pStdOut);
                                    }
                                }
                                else if (abCode2[0] == 0x48 && abCode2[1] == 0xff && abCode2[2] == 0xc8)
                                {
                                    /* Stuck on DEC RAX (before JNZ).  Check if JNZ follows. */
                                    if (abCode2[3] == 0x75)
                                    {
                                        pVCpu->cpum.GstCtx.rax = 1;
                                        if (s_uDelayRip == 0)
                                        {
                                            s_uDelayRip = curRip;
                                            RTPrintf("[DELAY-ACCEL] Learned delay RIP=%#llx (dec), enabling fast-path\n",
                                                (unsigned long long)curRip);
                                        }
                                        /* Store caller info + periodic log */
                                        {
                                            static unsigned s_cDecLogs = 0;
                                            s_cDecLogs++;
                                            uint64_t auStk3[8];
                                            RT_ZERO(auStk3);
                                            PGMPhysSimpleReadGCPtr(pVCpu, auStk3, pVCpu->cpum.GstCtx.rsp, sizeof(auStk3));
                                            /* Update globals for JS access */
                                            s_uDelayInfoRip = curRip;
                                            s_uDelayInfoRsp = pVCpu->cpum.GstCtx.rsp;
                                            s_uDelayInfoRbp = pVCpu->cpum.GstCtx.rbp;
                                            for (int si = 0; si < 8; si++)
                                                s_aDelayStack[si] = auStk3[si];
                                            s_fDelayInfoValid = 1;
                                            if (s_cDecLogs <= 5 || (s_cDecLogs % 100) == 0)
                                            {
                                                RTPrintf("[DELAY-ACCEL] #%u dec-rax RIP=%#llx insns=%llu ret=%#llx caller=%#llx\n",
                                                    s_cDecLogs, (unsigned long long)curRip,
                                                    (unsigned long long)g_cWasmVirtualInstructions,
                                                    (unsigned long long)auStk3[0],
                                                    (unsigned long long)auStk3[1]);
                                            }
                                        }
                                        RTStrmFlush(g_pStdOut);
                                    }
                                }
                            }

                            /* General-purpose kernel-spin accelerator:
                             * When the kernel is stuck in a non-delay, non-CRC loop in kernel
                             * text range (ffffffff81xxxxxx) with IF=1, advance jiffies_64
                             * directly in guest memory.  The timer (now firing every ~25k
                             * actual insns) will deliver IRQs; this just speeds up jiffies
                             * advancement for loops that wait for jiffies to change. */
                            if (   s_cSameRip >= 3
                                && curRip >= UINT64_C(0xffffffff81000000)
                                && curRip <  UINT64_C(0xffffffff82000000)
                                && (pVCpu->cpum.GstCtx.eflags.u & X86_EFL_IF)
                                && s_uDelayRip == 0   /* delay accel not yet active */
                                && curRip != s_uLastAccelRip)
                            {
                                static uint64_t s_uLastKernSpinRip = 0;
                                static uint32_t s_cKernSpinBoosts  = 0;
                                /* Advance jiffies on first detection and every 10 intervals */
                                if (curRip != s_uLastKernSpinRip || (s_cSameRip % 10) == 3)
                                {
                                    s_uLastKernSpinRip = curRip;
                                    s_cKernSpinBoosts++;
                                    /* Advance jiffies by 10 (≈10ms) to unblock spin-wait loops */
                                    PVMCC pVMJ2 = pVCpu->CTX_SUFF(pVM);
                                    uint64_t uJ2 = 0;
                                    PGMPhysRead(pVMJ2, UINT64_C(0x02406980), &uJ2,
                                                sizeof(uJ2), PGMACCESSORIGIN_DEBUGGER);
                                    uJ2 += 10;
                                    PGMPhysWrite(pVMJ2, UINT64_C(0x02406980), &uJ2,
                                                 sizeof(uJ2), PGMACCESSORIGIN_DEBUGGER);
                                    RTPrintf("[KERN-SPIN] #%u: RIP=%#llx IF=1 insns=%llu jiffies+=10\n",
                                        s_cKernSpinBoosts,
                                        (unsigned long long)curRip,
                                        (unsigned long long)pVCpu->iem.s.cInstructions);
                                    RTStrmFlush(g_pStdOut);
                                }
                            }

                            /* FORCE-IF for IF=0 stuck loops: only enable IF when the kernel
                             * has been stuck at the SAME instruction for ≥5 intervals
                             * (≥500M instructions).  This avoids disrupting normal IF=0
                             * critical sections (spinlocks, interrupt handlers) where CLI
                             * is legitimately used for a short time.  Targeting only truly
                             * stuck loops prevents irq_work_run_list reentrancy panics.
                             * Fire at most once every 5 additional intervals after that. */
                            if (   s_cSameRip >= 5
                                && (pVCpu->cpum.GstCtx.msrEFER & MSR_K6_EFER_LMA)
                                && !(pVCpu->cpum.GstCtx.eflags.u & X86_EFL_IF)
                                && curRip > UINT64_C(0xFFFF800000000000)
                                && (s_cSameRip % 5) == 0)
                            {
                                static uint32_t s_cForceIF = 0;
                                s_cForceIF++;
                                pVCpu->cpum.GstCtx.eflags.u |= X86_EFL_IF;
                                RTPrintf("[FORCE-IF] #%u stuck for %u intervals IF=0, RIP=%#llx insns=%llu — enabling IF\n",
                                    s_cForceIF, s_cSameRip,
                                    (unsigned long long)curRip,
                                    (unsigned long long)g_cWasmVirtualInstructions);
                                RTStrmFlush(g_pStdOut);
                            }
                        }

                        RTStrmFlush(g_pStdOut);
                    }
                }

                /* Clear CR2 magic once kernel is in PM.  The CR2=0xC0DEBA5E FF
                 * bypass was needed during BIOS real-mode to prevent timer interrupt
                 * cascades, but with instruction-count-based virtual time the cascade
                 * no longer occurs.  The direct-boot detection code is inside the JIT
                 * bail handler (real-mode only), so this separate check is needed to
                 * clear CR2 once the kernel enters protected mode. */
                {
                    static bool s_fCr2Cleared = false;
                    if (!s_fCr2Cleared
                        && (pVCpu->cpum.GstCtx.cr0 & X86_CR0_PE)
                        && pVCpu->cpum.GstCtx.cr2 == UINT64_C(0xC0DEBA5E))
                    {
                        s_fCr2Cleared = true;
                        pVCpu->cpum.GstCtx.cr2 = 0;
                        RTPrintf("[TIMER-FIX] Cleared CR2 magic (PE mode) — timer FFs now enabled\n");
                        RTStrmFlush(g_pStdOut);
                    }
                }

                /* Delay loop fast-forward: detect __delay() pattern (dec rax; jnz -5).
                 * Only fast-forward VERY long delays (RAX > 10M iterations = 20M insns
                 * = 2 seconds virtual time).  Do NOT fast-forward small delays because
                 * calibrate_delay() relies on proportional timing to compute
                 * loops_per_jiffy.  With the instruction-count-based virtual clock,
                 * small delays complete naturally in reasonable time. */
                {
                    static uint32_t s_cDelayCheck = 0;
                    if (++s_cDelayCheck >= 4096
                        && (pVCpu->cpum.GstCtx.msrEFER & MSR_K6_EFER_LMA))
                    {
                        s_cDelayCheck = 0;
                        uint64_t rip = pVCpu->cpum.GstCtx.rip;
                        uint8_t abOp[5];
                        int rc2 = PGMPhysSimpleReadGCPtr(pVCpu, abOp, rip, sizeof(abOp));
                        if (RT_SUCCESS(rc2)
                            && abOp[0] == 0x48 && abOp[1] == 0xff && abOp[2] == 0xc8  /* dec rax */
                            && abOp[3] == 0x75 && abOp[4] == 0xfb)                     /* jnz -5  */
                        {
                            if (pVCpu->cpum.GstCtx.rax > 10000000) /* >10M iterations = hardware timeout */
                            {
                                static uint32_t s_cLogCount = 0;
                                if (s_cLogCount < 5)
                                {
                                    RTPrintf("[DELAY-FF] Fast-forwarding __delay() at RIP=%016llx RAX=%016llx (>10M)\n",
                                             (unsigned long long)rip, (unsigned long long)pVCpu->cpum.GstCtx.rax);
                                    RTStrmFlush(g_pStdOut);
                                    s_cLogCount++;
                                }
                                pVCpu->cpum.GstCtx.rax = 1;
                            }
                        }
                    }
                }

                /* JIT fast path: try JS interpreter before IEM decode.
                 *
                 * s_cIemAfterJitBail: after the JIT bails on an I/O instruction (returns 0),
                 * we let IEM execute a few instructions (the rest of the BSY polling loop:
                 * typically TEST + JNZ + maybe a few more) before re-entering the JIT.
                 * This avoids calling the JIT just for 2-3 instructions between consecutive
                 * IN instructions, saving significant overhead during ATA BSY polling. */
                static uint32_t s_cIemAfterJitBail = 0;
                /* Periodic CPU state diagnostic — counter-based (RTTimeNanoTS
                   doesn't advance during synchronous JS execution in Wasm workers) */
                {
                    static uint32_t s_cDiagCounter = 0;
                    if (++s_cDiagCounter >= 500000000) /* suppressed — was 5M, now 500M to avoid flooding console */
                    {
                        s_cDiagCounter = 0;
                        uint16_t cs = pVCpu->cpum.GstCtx.cs.Sel;
                        uint64_t rip = pVCpu->cpum.GstCtx.rip;
                        uint32_t cr0 = pVCpu->cpum.GstCtx.cr0;
                        uint32_t efl = pVCpu->cpum.GstCtx.eflags.u;
                        uint32_t eax = pVCpu->cpum.GstCtx.eax;
                        uint32_t edx = pVCpu->cpum.GstCtx.edx;
                        RTPrintf("[CPU-DIAG] CS=%04x IP=%08llx CR0=%08x FL=%08x EAX=%08x EDX=%08x insns=%llu jitBail=%u\n",
                                 cs, (unsigned long long)rip, cr0, efl, eax, edx,
                                 (unsigned long long)pVCpu->iem.s.cInstructions, s_cIemAfterJitBail);
                        /* Stuck detector: if EIP hasn't changed, dump instruction bytes + extra regs */
                        static uint64_t s_uLastDiagRip = 0;
                        static uint32_t s_cStuckCount = 0;
                        if (rip == s_uLastDiagRip)
                        {
                            if (++s_cStuckCount >= 3 && (s_cStuckCount % 50) == 3)
                            {
                                uint8_t abCode[16];
                                RT_ZERO(abCode);
                                RTGCPHYS GCPhys = 0;
                                uint64_t efer = pVCpu->cpum.GstCtx.msrEFER;
                                uint64_t cr3v = pVCpu->cpum.GstCtx.cr3;
                                if (efer & MSR_K6_EFER_LMA)
                                {
                                    /* 4-level page walk for 64-bit long mode */
                                    uint64_t pml4Base = cr3v & ~UINT64_C(0xFFF);
                                    uint64_t pml4e = 0, pdpe = 0, pde64 = 0, pte64 = 0;
                                    unsigned pml4Idx = (rip >> 39) & 0x1FF;
                                    unsigned pdpIdx  = (rip >> 30) & 0x1FF;
                                    unsigned pdIdx   = (rip >> 21) & 0x1FF;
                                    unsigned ptIdx   = (rip >> 12) & 0x1FF;
                                    PGMPhysRead(pVM, pml4Base + pml4Idx * 8, &pml4e, 8, PGMACCESSORIGIN_IEM);
                                    if (pml4e & 1)
                                    {
                                        PGMPhysRead(pVM, (pml4e & UINT64_C(0x000FFFFFFFFFF000)) + pdpIdx * 8, &pdpe, 8, PGMACCESSORIGIN_IEM);
                                        if (pdpe & 1)
                                        {
                                            if (pdpe & 0x80) /* 1GB page */
                                                GCPhys = (pdpe & UINT64_C(0x000FFFFFC0000000)) | (rip & UINT64_C(0x3FFFFFFF));
                                            else
                                            {
                                                PGMPhysRead(pVM, (pdpe & UINT64_C(0x000FFFFFFFFFF000)) + pdIdx * 8, &pde64, 8, PGMACCESSORIGIN_IEM);
                                                if (pde64 & 1)
                                                {
                                                    if (pde64 & 0x80) /* 2MB page */
                                                        GCPhys = (pde64 & UINT64_C(0x000FFFFFFFE00000)) | (rip & UINT64_C(0x1FFFFF));
                                                    else
                                                    {
                                                        PGMPhysRead(pVM, (pde64 & UINT64_C(0x000FFFFFFFFFF000)) + ptIdx * 8, &pte64, 8, PGMACCESSORIGIN_IEM);
                                                        if (pte64 & 1)
                                                            GCPhys = (pte64 & UINT64_C(0x000FFFFFFFFFF000)) | (rip & 0xFFF);
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    RTPrintf("[CPU-STUCK] virt=%016llx phys=%016llx PML4E=%016llx PDPE=%016llx PDE=%016llx PTE=%016llx\n",
                                             (unsigned long long)rip, (unsigned long long)GCPhys,
                                             (unsigned long long)pml4e, (unsigned long long)pdpe,
                                             (unsigned long long)pde64, (unsigned long long)pte64);
                                }
                                else
                                {
                                    /* 32-bit page walk */
                                    uint32_t pde = 0, pte = 0;
                                    uint32_t cr3_32 = (uint32_t)cr3v & ~UINT32_C(0xFFF);
                                    uint32_t pdeIdx = (uint32_t)(rip >> 22) & 0x3FF;
                                    uint32_t pteIdx = (uint32_t)(rip >> 12) & 0x3FF;
                                    PGMPhysRead(pVM, (RTGCPHYS)(cr3_32 + pdeIdx * 4), &pde, 4, PGMACCESSORIGIN_IEM);
                                    if (pde & 1)
                                    {
                                        if (pde & 0x80)
                                            GCPhys = (pde & 0xFFC00000) | ((uint32_t)rip & 0x003FFFFF);
                                        else
                                        {
                                            PGMPhysRead(pVM, (RTGCPHYS)((pde & ~UINT32_C(0xFFF)) + pteIdx * 4), &pte, 4, PGMACCESSORIGIN_IEM);
                                            if (pte & 1)
                                                GCPhys = (pte & ~UINT32_C(0xFFF)) | ((uint32_t)rip & 0xFFF);
                                        }
                                    }
                                    RTPrintf("[CPU-STUCK] virt=%08llx phys=%08llx PDE=%08x PTE=%08x\n",
                                             (unsigned long long)rip, (unsigned long long)GCPhys, pde, pte);
                                }
                                if (GCPhys)
                                    PGMPhysRead(pVM, GCPhys, abCode, sizeof(abCode), PGMACCESSORIGIN_IEM);
                                RTPrintf("[CPU-STUCK] EIP=%08llx bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                                         (unsigned long long)rip,
                                         abCode[0], abCode[1], abCode[2], abCode[3],
                                         abCode[4], abCode[5], abCode[6], abCode[7],
                                         abCode[8], abCode[9], abCode[10], abCode[11],
                                         abCode[12], abCode[13], abCode[14], abCode[15]);
                                RTPrintf("[CPU-STUCK] EBX=%08x ECX=%08x ESI=%08x EDI=%08x ESP=%08x EBP=%08x CR2=%08llx CR3=%08llx CR4=%08x EFER=%016llx\n",
                                         pVCpu->cpum.GstCtx.ebx, pVCpu->cpum.GstCtx.ecx,
                                         pVCpu->cpum.GstCtx.esi, pVCpu->cpum.GstCtx.edi,
                                         pVCpu->cpum.GstCtx.esp, pVCpu->cpum.GstCtx.ebp,
                                         (unsigned long long)pVCpu->cpum.GstCtx.cr2,
                                         (unsigned long long)pVCpu->cpum.GstCtx.cr3,
                                         (unsigned)pVCpu->cpum.GstCtx.cr4,
                                         (unsigned long long)pVCpu->cpum.GstCtx.msrEFER);
                            }
                        }
                        else
                        {
                            s_cStuckCount = 0;
                            s_uLastDiagRip = rip;
                        }
                        RTStrmFlush(g_pStdOut);
                    }
                }
                iemJitEnsureInit(pVM);
                /* Periodic ROM re-copy: BIOS decompresses code to shadow RAM
                   during POST, so our boot-time ROM snapshot becomes stale.
                   Re-copy at exponentially spaced milestones. */
                if (RT_UNLIKELY(   s_pvJitRomBuf
                                && pVCpu->iem.s.cInstructions >= s_cRomRefreshNext))
                {
                    PVMCC pVMLocal = pVCpu->CTX_SUFF(pVM);
                    for (uint32_t off = 0; off < 0x40000; off += GUEST_PAGE_SIZE)
                        PGMPhysRead(pVMLocal, (RTGCPHYS)(0xC0000 + off),
                                    (uint8_t *)s_pvJitRomBuf + off, GUEST_PAGE_SIZE,
                                    PGMACCESSORIGIN_IEM);
                    char szMsg[80];
                    RTStrPrintf(szMsg, sizeof(szMsg), "ROM re-copy at insn %llu",
                                (unsigned long long)pVCpu->iem.s.cInstructions);
                    wasmJitLog(szMsg);
                    /* Next milestone: double the interval, cap at 50M */
                    if (s_cRomRefreshNext < 50000000)
                        s_cRomRefreshNext = s_cRomRefreshNext * 3;
                    else
                        s_cRomRefreshNext = UINT64_MAX; /* stop re-copying */
                }
                /* Drain keyboard scancodes from JS ring buffer on EMT thread.
                   wasmKbdDrainQueue puts scancodes into DrvKeyboardQueue's PDM
                   queue which sets VM_FF_PDM_QUEUES.  Skip the JIT for this
                   iteration when scancodes were drained so the EM main loop
                   gets to flush the PDM queue promptly and deliver IRQ1.
                   The JIT's own STI bail (jit-pre.js case 0xFB) handles the
                   INT 16h CLI/STI busy-wait: when IF transitions 0→1 the JIT
                   exits, letting the EM loop inject pending interrupts at the
                   correct STI window.  We must NOT skip the JIT on every
                   VMCPU_FF_INTERRUPT_PIC — the PIT timer sets that flag at
                   18.2 Hz which would kill JIT throughput entirely. */
                int cKbdDrained = wasmKbdDrainQueue();
                if (s_pvJitRAM && s_cIemAfterJitBail == 0
                    && !(pVCpu->cpum.GstCtx.cr0 & X86_CR0_PE) /* skip JIT in PM */
                    && cKbdDrained == 0)
                {
                    uint32_t cBatch = RT_MIN(cMaxInstructionsGccStupidity, 4096);
                    wasmJitSetA20(PGMPhysIsA20Enabled(pVCpu) ? 1 : 0);
                    int fIrqPending = VMCPU_FF_IS_ANY_SET(pVCpu, VMCPU_FF_INTERRUPT_APIC | VMCPU_FF_INTERRUPT_PIC) ? 1 : 0;
                    int cJitInsns = wasmJitExecBlock(&pVCpu->cpum.GstCtx, s_pvJitRAM, cBatch,
                                                     s_pvJitHighRAM, (int)s_cbJitHighRAM, fIrqPending);
                    if (cJitInsns > 0)
                    {
                        pVCpu->iem.s.cInstructions += cJitInsns;
                        cMaxInstructionsGccStupidity -= RT_MIN((uint32_t)cJitInsns, cMaxInstructionsGccStupidity);

                        /* Check forced actions and poll timers */
#ifdef __EMSCRIPTEN__
                        /* During kernel direct-boot (CR2 magic), bypass ALL forced-action
                           and timer checks.  The PIT fires continuously and the EM scheduler
                           injects timer IRQs between every IEM loop iteration, creating an
                           infinite cascade.  Kernel setup code doesn't need interrupts. */
                        if (   pVCpu->cpum.GstCtx.cr2 == UINT64_C(0xC0DEBA5E)
                            && cMaxInstructionsGccStupidity > 0)
                        {
                            iemReInitDecoder(pVCpu);
                            continue;
                        }
#endif
                        uint64_t fCpu = pVCpu->fLocalForcedActions;
                        fCpu &= VMCPU_FF_ALL_MASK & ~(  VMCPU_FF_PGM_SYNC_CR3
                                                       | VMCPU_FF_PGM_SYNC_CR3_NON_GLOBAL
                                                       | VMCPU_FF_TLB_FLUSH
                                                       | VMCPU_FF_UNHALT );
                        if (RT_LIKELY(   iemExecLoopTargetCheckMaskedCpuFFs(pVCpu, fCpu)
                                      && !VM_FF_IS_ANY_SET(pVM, VM_FF_ALL_MASK)
                                      && cMaxInstructionsGccStupidity > 0))
                        {
                            /* Always poll timers after JIT batch — the JIT skips
                               cPollRate alignment so we must poll every return to
                               ensure PIT timer interrupts get delivered. */
                            if (!TMTimerPollBool(pVM, pVCpu))
                            {
                                iemReInitDecoder(pVCpu);
                                continue;
                            }
                        }
                        rcStrict = VINF_SUCCESS;
                        break;
                    }
                    /* JIT bailed on the very first instruction (I/O, segment change, etc.).
                       Let IEM handle a few instructions before re-entering JIT.
                       The JIT now checks segment bases each call, so non-flat PM code
                       (ISOLINUX trampoline) safely bails on re-entry.  A short cooldown
                       keeps flat PM responsive for I/O bails (IN/OUT, INT). */
                    s_cIemAfterJitBail = 4;
                    /* Direct boot: JIT copied kernel/initrd/boot_params to guest RAM
                       and set CR2 = 0xC0DEBA5E.  We use the 32-bit boot protocol:
                       enter the kernel at code32_start in protected mode with flat
                       segments, bypassing all real-mode BIOS calls. */
                    {
                        /* ── 64-bit fast boot: kernel decompressed in JS ── */
                        static bool s_fFastBoot64Detected = false;
                        if (!s_fFastBoot64Detected && pVCpu->cpum.GstCtx.cr2 == UINT64_C(0xD64B0001))
                        {
                            /* Read metadata from guest 0x7200 */
                            uint8_t abMeta64[20];
                            PGMPhysRead(pVM, (RTGCPHYS)0x7200, abMeta64, sizeof(abMeta64), PGMACCESSORIGIN_IEM);
                            uint32_t uMagic64 = *(uint32_t *)&abMeta64[0];

                            if (uMagic64 == 0x42343644) /* "D64B" */
                            {
                                s_fFastBoot64Detected = true;
                                PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;

                                uint32_t uCR3 = *(uint32_t *)&abMeta64[4];
                                uint64_t uEntry = *(uint64_t *)&abMeta64[8];
                                uint32_t uBootParams = *(uint32_t *)&abMeta64[16];

                                RTPrintf("[FAST-BOOT-64] CR3=0x%08x entry=0x%016llx boot_params=0x%08x\n",
                                         uCR3, (unsigned long long)uEntry, uBootParams);

                                /* Set up GDT from guest 0x7300 (written by JS) */
                                pCtx->gdtr.pGdt  = 0x7300;
                                pCtx->gdtr.cbGdt = 31; /* 4 entries */

                                /* Enable paging + long mode */
                                pCtx->cr3 = uCR3;
                                pCtx->cr4 = X86_CR4_PAE | X86_CR4_PSE;
                                pCtx->cr0 = X86_CR0_PE | X86_CR0_PG | X86_CR0_ET
                                          | X86_CR0_NE | X86_CR0_WP | X86_CR0_MP;
                                pCtx->msrEFER = MSR_K6_EFER_LME | MSR_K6_EFER_LMA | MSR_K6_EFER_NXE;
                                PGMNotifyNxeChanged(pVCpu, true);

                                /* CS = 64-bit code segment (selector 0x10) */
                                pCtx->cs.Sel      = 0x10;
                                pCtx->cs.ValidSel = 0x10;
                                pCtx->cs.fFlags   = CPUMSELREG_FLAGS_VALID;
                                pCtx->cs.u64Base  = 0;
                                pCtx->cs.u32Limit = UINT32_MAX;
                                pCtx->cs.Attr.u   = 0xA09B; /* G=1 L=1 P=1 DPL=0 code/r/a */

                                /* DS=ES=SS=FS=GS = 64-bit data (selector 0x18) */
                                PCPUMSELREG apSegs64[] = { &pCtx->ds, &pCtx->es, &pCtx->fs, &pCtx->gs, &pCtx->ss };
                                for (unsigned i = 0; i < RT_ELEMENTS(apSegs64); i++)
                                {
                                    apSegs64[i]->Sel      = 0x18;
                                    apSegs64[i]->ValidSel = 0x18;
                                    apSegs64[i]->fFlags   = CPUMSELREG_FLAGS_VALID;
                                    apSegs64[i]->u64Base  = 0;
                                    apSegs64[i]->u32Limit = UINT32_MAX;
                                    apSegs64[i]->Attr.u   = 0xC093; /* G=1 D=1 P=1 DPL=0 data/w/a */
                                }

                                /* RIP = kernel entry point (virtual address) */
                                pCtx->rip = uEntry;
                                /* RSI = boot_params physical address */
                                pCtx->rsi = uBootParams;

                                /* ── Patch kernel cmdline in boot_params ──
                                 * Append "mitigations=off notrace" to skip CPU vulnerability
                                 * mitigation patching (check_bugs → alternative_instructions)
                                 * and ftrace initialization, both of which stall for 2B+ IEM
                                 * instructions at ~1M insns/sec (30+ min wall time).
                                 * boot_params.hdr.cmd_line_ptr is at offset 0x228. */
                                {
                                    uint32_t uCmdLinePtr = 0;
                                    PGMPhysRead(pVM, (RTGCPHYS)(uBootParams + 0x228), &uCmdLinePtr,
                                                sizeof(uCmdLinePtr), PGMACCESSORIGIN_IEM);
                                    RTPrintf("[FAST-BOOT-64] cmd_line_ptr=0x%08x\n", uCmdLinePtr);
                                    if (uCmdLinePtr != 0)
                                    {
                                        /* Read existing cmdline (up to 256 bytes) */
                                        char szCmdLine[512];
                                        RT_ZERO(szCmdLine);
                                        PGMPhysRead(pVM, (RTGCPHYS)uCmdLinePtr, szCmdLine, 256,
                                                    PGMACCESSORIGIN_IEM);
                                        szCmdLine[255] = '\0';
                                        size_t cchExisting = strlen(szCmdLine);
                                        RTPrintf("[FAST-BOOT-64] existing cmdline: '%s'\n", szCmdLine);
                                        /* Append mitigation disablers */
                                        static const char szExtra[] = " mitigations=off notrace";
                                        if (cchExisting + sizeof(szExtra) < sizeof(szCmdLine))
                                            memcpy(szCmdLine + cchExisting, szExtra, sizeof(szExtra));
                                        RTPrintf("[FAST-BOOT-64] new cmdline: '%s'\n", szCmdLine);
                                        RTStrmFlush(g_pStdOut);
                                        /* Write new cmdline to safe physical address 0x8000 */
                                        PGMPhysWrite(pVM, (RTGCPHYS)0x8000, szCmdLine,
                                                     strlen(szCmdLine) + 1, PGMACCESSORIGIN_IEM);
                                        /* Update cmd_line_ptr in boot_params */
                                        uint32_t uNewCmdLinePtr = 0x8000;
                                        PGMPhysWrite(pVM, (RTGCPHYS)(uBootParams + 0x228),
                                                     &uNewCmdLinePtr, sizeof(uNewCmdLinePtr),
                                                     PGMACCESSORIGIN_IEM);
                                    }
                                }

                                /* Zero other GP regs */
                                pCtx->rax = 0; pCtx->rbx = 0; pCtx->rcx = 0; pCtx->rdx = 0;
                                pCtx->rdi = 0; pCtx->rbp = 0;
                                pCtx->rsp = 0x9FFC0; /* temporary stack in conventional memory */
                                /* RFLAGS: IF=0 */
                                pCtx->rflags.u = X86_EFL_1;
                                /* Empty IDT — kernel sets up its own */
                                pCtx->idtr.pIdt  = 0;
                                pCtx->idtr.cbIdt = 0;

                                pCtx->cr2 = 0; /* clear magic */

                                /* Notify PGM about the mode change + new CR3 */
                                VMCPU_FF_SET(pVCpu, VMCPU_FF_PGM_SYNC_CR3);

                                /* Reset TM virtual sync catch-up (same fix as FAST-BOOT-CPP path) */
                                {
                                    extern void TMVirtualSyncResetCatchUpWasm(PVMCC);
                                    TMVirtualSyncResetCatchUpWasm(pVM);
                                    RTPrintf("[FAST-BOOT-64] TM sync reset: offVirtualSync=0 catchUp=false\n");
                                }

                                RTPrintf("[FAST-BOOT-64] Entering 64-bit kernel: RIP=%#018llx RSI=%#010x CR0=%#010x CR3=%#010x EFER=%#06x\n",
                                         (unsigned long long)pCtx->rip, (unsigned)pCtx->rsi,
                                         (unsigned)pCtx->cr0, (unsigned)pCtx->cr3,
                                         (unsigned)pCtx->msrEFER);
                                RTStrmFlush(g_pStdOut);
                            }
                        }

                        static bool s_fDirectBootDetected = false;
                        if (!s_fDirectBootDetected && pVCpu->cpum.GstCtx.cr2 == UINT64_C(0xC0DEBA5E))
                        {
                            /* Read metadata from guest RAM at 0x7000.
                             * CR2 magic can be set by the CPUID LM injection (before
                             * direct boot writes metadata), so only proceed when the
                             * DBOOT magic is present.  Don't set s_fDirectBootDetected
                             * until then, so we retry on subsequent JIT bails.
                             * Also skip if the kernel already entered PM on its own. */
                            uint8_t abMeta[8];
                            PGMPhysRead(pVM, (RTGCPHYS)0x7000, abMeta, sizeof(abMeta), PGMACCESSORIGIN_IEM);
                            uint32_t uMagic = *(uint32_t *)&abMeta[4];

                            if (uMagic == 0x44424F4F && !(pVCpu->cpum.GstCtx.cr0 & X86_CR0_PE))
                            {
                                s_fDirectBootDetected = true;
                                RTPrintf("[DIRECT-BOOT-CPP] CR2 magic + DBOOT metadata ready — 32-bit boot protocol\n");
                                PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;

                                /* ── 32-bit boot protocol ──
                                 * Enter kernel at code32_start (protected-mode entry) with
                                 * flat segments, bypassing all real-mode setup code and BIOS
                                 * INT calls that cause the timer interrupt cascade. */

                                /* Read code32_start from boot header at guest 0x10000+0x214 */
                                uint32_t uCode32Start = 0;
                                PGMPhysRead(pVM, (RTGCPHYS)0x10214, &uCode32Start, sizeof(uCode32Start), PGMACCESSORIGIN_IEM);
                                if (uCode32Start == 0)
                                    uCode32Start = 0x100000; /* fallback: default PM entry */
                                RTPrintf("[DIRECT-BOOT-CPP] code32_start = 0x%08x\n", uCode32Start);

                                /* Write GDT at guest 0x7100:
                                 *   0x00: NULL descriptor  (selector 0x00)
                                 *   0x08: unused           (selector 0x08)
                                 *   0x10: flat code 32-bit (selector 0x10)
                                 *   0x18: flat data 32-bit (selector 0x18) */
                                uint8_t abGdt[32];
                                RT_ZERO(abGdt);
                                /* Flat code (0x10): base=0 limit=4GB code/readable/accessed 32-bit */
                                abGdt[16] = 0xFF; abGdt[17] = 0xFF; /* limit 15:0 */
                                abGdt[20] = 0x00;                    /* base 23:16 */
                                abGdt[21] = 0x9B;                    /* P=1 DPL=0 S=1 type=1011 */
                                abGdt[22] = 0xCF;                    /* G=1 D=1 limit 19:16=F */
                                abGdt[23] = 0x00;                    /* base 31:24 */
                                /* Flat data (0x18): base=0 limit=4GB data/writable/accessed 32-bit */
                                abGdt[24] = 0xFF; abGdt[25] = 0xFF;
                                abGdt[29] = 0x93;                    /* P=1 DPL=0 S=1 type=0011 */
                                abGdt[30] = 0xCF;
                                PGMPhysWrite(pVM, (RTGCPHYS)0x7100, abGdt, sizeof(abGdt), PGMACCESSORIGIN_IEM);

                                /* GDTR: base=0x7100, limit=31 (4 entries × 8 - 1) */
                                pCtx->gdtr.pGdt  = 0x7100;
                                pCtx->gdtr.cbGdt = sizeof(abGdt) - 1;

                                /* Enable protected mode, paging OFF */
                                pCtx->cr0 = (pCtx->cr0 | X86_CR0_PE) & ~X86_CR0_PG;

                                /* CS = selector 0x10 (flat code, 32-bit) */
                                pCtx->cs.Sel      = 0x10;
                                pCtx->cs.ValidSel = 0x10;
                                pCtx->cs.fFlags   = CPUMSELREG_FLAGS_VALID;
                                pCtx->cs.u64Base  = 0;
                                pCtx->cs.u32Limit = UINT32_MAX;
                                pCtx->cs.Attr.u   = 0xC09B; /* G=1 D=1 P=1 DPL=0 S=1 code/r/a */

                                /* DS=ES=SS=FS=GS = selector 0x18 (flat data, 32-bit) */
                                PCPUMSELREG apDataSegs[] = { &pCtx->ds, &pCtx->es, &pCtx->fs, &pCtx->gs, &pCtx->ss };
                                for (unsigned i = 0; i < RT_ELEMENTS(apDataSegs); i++)
                                {
                                    apDataSegs[i]->Sel      = 0x18;
                                    apDataSegs[i]->ValidSel = 0x18;
                                    apDataSegs[i]->fFlags   = CPUMSELREG_FLAGS_VALID;
                                    apDataSegs[i]->u64Base  = 0;
                                    apDataSegs[i]->u32Limit = UINT32_MAX;
                                    apDataSegs[i]->Attr.u   = 0xC093; /* G=1 D=1 P=1 DPL=0 S=1 data/w/a */
                                }

                                /* ── Patch kernel cmdline: append mitigations=off notrace ──
                                 * boot_params is at 0x10000; cmd_line_ptr at +0x228. */
                                {
                                    uint32_t uCmdLinePtr32 = 0;
                                    PGMPhysRead(pVM, (RTGCPHYS)0x10228, &uCmdLinePtr32, sizeof(uCmdLinePtr32), PGMACCESSORIGIN_IEM);
                                    RTPrintf("[DIRECT-BOOT-CPP] cmd_line_ptr=0x%08x\n", uCmdLinePtr32);
                                    if (uCmdLinePtr32 != 0)
                                    {
                                        char szCmdLine[512];
                                        RT_ZERO(szCmdLine);
                                        PGMPhysRead(pVM, (RTGCPHYS)uCmdLinePtr32, szCmdLine, 256, PGMACCESSORIGIN_IEM);
                                        szCmdLine[255] = '\0';
                                        size_t cchExisting = strlen(szCmdLine);
                                        RTPrintf("[DIRECT-BOOT-CPP] existing cmdline: '%s'\n", szCmdLine);
                                        static const char szExtra[] = " mitigations=off notrace";
                                        if (cchExisting + sizeof(szExtra) < sizeof(szCmdLine))
                                            memcpy(szCmdLine + cchExisting, szExtra, sizeof(szExtra));
                                        RTPrintf("[DIRECT-BOOT-CPP] new cmdline: '%s'\n", szCmdLine);
                                        RTStrmFlush(g_pStdOut);
                                        PGMPhysWrite(pVM, (RTGCPHYS)0x8000, szCmdLine,
                                                     strlen(szCmdLine) + 1, PGMACCESSORIGIN_IEM);
                                        uint32_t uNewPtr = 0x8000;
                                        PGMPhysWrite(pVM, (RTGCPHYS)0x10228, &uNewPtr, sizeof(uNewPtr), PGMACCESSORIGIN_IEM);
                                    }
                                }

                                /* EIP = code32_start (PM kernel entry point) */
                                pCtx->rip = uCode32Start;

                                /* ESI = boot_params address (Linux 32-bit boot protocol) */
                                pCtx->rsi = 0x10000;

                                /* GP regs: zero everything else */
                                pCtx->rax = 0; pCtx->rbx = 0; pCtx->rcx = 0; pCtx->rdx = 0;
                                pCtx->rdi = 0; pCtx->rbp = 0;

                                /* ESP: top of conventional memory */
                                pCtx->rsp = 0x9FFC0;

                                /* EFLAGS: reserved bit 1 only, IF=0 (interrupts disabled) */
                                pCtx->rflags.u = 0x00000002;

                                /* Log entry state and first 16 bytes of code */
                                uint8_t abCode[16];
                                PGMPhysRead(pVM, (RTGCPHYS)uCode32Start, abCode, sizeof(abCode), PGMACCESSORIGIN_IEM);
                                RTPrintf("[DIRECT-BOOT-CPP] 32-bit PM: CS=%04x EIP=%08x ESP=%08x ESI=%08x CR0=%08x GDTR=%08x:%04x\n",
                                         pCtx->cs.Sel, (unsigned)pCtx->rip,
                                         (unsigned)pCtx->rsp, (unsigned)pCtx->rsi,
                                         (unsigned)pCtx->cr0,
                                         (unsigned)pCtx->gdtr.pGdt, (unsigned)pCtx->gdtr.cbGdt);
                                RTPrintf("[DIRECT-BOOT-CPP] code@%08x: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
                                         uCode32Start,
                                         abCode[0], abCode[1], abCode[2], abCode[3],
                                         abCode[4], abCode[5], abCode[6], abCode[7],
                                         abCode[8], abCode[9], abCode[10], abCode[11],
                                         abCode[12], abCode[13], abCode[14], abCode[15]);
                                RTStrmFlush(g_pStdOut);
                                /* Clear CR2 magic so the FF bypass deactivates.
                                 * With instruction-count-based virtual time, the timer
                                 * cascade no longer happens (JIT doesn't advance the clock). */
                                pVCpu->cpum.GstCtx.cr2 = 0;
                            }
                            else if (pVCpu->cpum.GstCtx.cr0 & X86_CR0_PE)
                            {
                                /* Kernel already entered PM on its own (via real-mode setup path). */
                                s_fDirectBootDetected = true;
                                RTPrintf("[DIRECT-BOOT-CPP] Kernel already in PM (CR0=%08x) — skipping 32-bit boot\n",
                                         (unsigned)pVCpu->cpum.GstCtx.cr0);
                                RTStrmFlush(g_pStdOut);
                                pVCpu->cpum.GstCtx.cr2 = 0;
                            }
                            /* else: metadata not ready yet (premature CR2 from CPUID LM injection).
                               Don't set s_fDirectBootDetected — retry on next JIT bail. */
                        }
                    }
                    iemReInitDecoder(pVCpu);
                }
                else if (s_cIemAfterJitBail > 0)
                    s_cIemAfterJitBail--;
#endif
                rcStrict = iemExecDecodeAndInterpretTargetInstruction(pVCpu);
#ifdef __EMSCRIPTEN__
                /* Monitor kernel boot progress: log CS changes and milestones */
                if (!g_fProductionMode) {
                    static bool     s_fBootActive = false;
                    static bool     s_fFB64Active = false;
                    static uint16_t s_uLastCS = 0;
                    static uint64_t s_cBootInsns = 0;
                    static uint64_t s_cLastReport = 0;
                    if (!s_fBootActive
                        && (   pVCpu->cpum.GstCtx.cr2 == UINT64_C(0xC0DEBA5E)
                            || (pVCpu->cpum.GstCtx.msrEFER & MSR_K6_EFER_LMA)))
                    {
                        s_fBootActive = true;
                        g_wasmFBBootActive = 1;
                        s_uLastCS = pVCpu->cpum.GstCtx.cs.Sel;
                        RTPrintf("[DBOOT] Kernel boot active (%s), CS=%04x RIP=%#018llx CR0=%08llx\n",
                                 (pVCpu->cpum.GstCtx.msrEFER & MSR_K6_EFER_LMA) ? "64-bit LM" : "32-bit PM",
                                 s_uLastCS, (unsigned long long)pVCpu->cpum.GstCtx.rip,
                                 (unsigned long long)pVCpu->cpum.GstCtx.cr0);

                        RTStrmFlush(g_pStdOut);
                    }
                    if (s_fBootActive)
                    {
                        s_cBootInsns++;

                        /* Trace first 20 instructions after 64-bit kernel entry */
                        if (s_fFB64Active && s_cBootInsns <= 20)
                        {
                            RTStrmPrintf(g_pStdErr, "[FB64-TRACE] #%llu RIP=%#018llx RSP=%#018llx CR0=%#010llx EFER=%#010llx\n",
                                     (unsigned long long)s_cBootInsns,
                                     (unsigned long long)pVCpu->cpum.GstCtx.rip,
                                     (unsigned long long)pVCpu->cpum.GstCtx.rsp,
                                     (unsigned long long)pVCpu->cpum.GstCtx.cr0,
                                     (unsigned long long)pVCpu->cpum.GstCtx.msrEFER);
                            RTStrmFlush(g_pStdErr);
                        }

                        uint16_t uCS = pVCpu->cpum.GstCtx.cs.Sel;

                        /* Fast boot: detect 32-bit PM kernel decompressor running and
                           decompress the kernel in JS instead of letting IEM
                           run the decompressor for 20+ minutes.
                           Trigger: CS=0x10, EIP >= 0x100000, PE set, HdrS at 0x90202.
                           The HdrS check ensures the kernel setup header is actually
                           loaded by ISOLINUX before we attempt decompression.
                           Don't set tried=true until HdrS confirmed (retry otherwise). */
                        {
                            static bool s_fFastBootTried = false;
                            static uint32_t s_cFBCheckCounter = 0;
                            /* Update boot CS global for external querying */
                            g_wasmFBBootCS = uCS;
                            if (!s_fFastBootTried
                                && uCS == 0x10
                                && (pVCpu->cpum.GstCtx.cr0 & X86_CR0_PE)
                                && pVCpu->cpum.GstCtx.rip >= UINT64_C(0x100000)
                                && (++s_cFBCheckCounter % 100000) == 0) /* check every 100K insns */
                            {
                                /* Check for HdrS signature at guest 0x90202 */
                                uint8_t abHdrS[4];
                                PGMPhysRead(pVM, (RTGCPHYS)0x90202, abHdrS, 4, PGMACCESSORIGIN_IEM);
                                /* Also check 0x10202 (some bootloaders load setup to 0x10000) */
                                uint8_t abHdrS2[4];
                                PGMPhysRead(pVM, (RTGCPHYS)0x10202, abHdrS2, 4, PGMACCESSORIGIN_IEM);
                                /* Update globals for external querying */
                                g_wasmFBCheckCount++;
                                g_wasmFBLastEIP = pVCpu->cpum.GstCtx.rip;
                                g_wasmFBLastHdrS1 = *(uint32_t *)abHdrS;
                                g_wasmFBLastHdrS2 = *(uint32_t *)abHdrS2;
                                /* Log first 20 checks, then every 100th */
                                if (g_wasmFBCheckCount <= 20 || (g_wasmFBCheckCount % 100) == 0)
                                {
                                    RTPrintf("[FAST-BOOT-CHK] #%u EIP=%#llx @0x90202=%02x%02x%02x%02x @0x10202=%02x%02x%02x%02x\n",
                                             g_wasmFBCheckCount,
                                             (unsigned long long)pVCpu->cpum.GstCtx.rip,
                                             abHdrS[0], abHdrS[1], abHdrS[2], abHdrS[3],
                                             abHdrS2[0], abHdrS2[1], abHdrS2[2], abHdrS2[3]);
                                    RTStrmFlush(g_pStdOut);
                                }
                                bool fFound = (abHdrS[0] == 'H' && abHdrS[1] == 'd' && abHdrS[2] == 'r' && abHdrS[3] == 'S');
                                if (!fFound)
                                    fFound = (abHdrS2[0] == 'H' && abHdrS2[1] == 'd' && abHdrS2[2] == 'r' && abHdrS2[3] == 'S');
                                if (fFound)
                                {
                                s_fFastBootTried = true;
                                RTPrintf("[FAST-BOOT-CPP] Detected kernel decompressor at EIP=%#llx, HdrS found — attempting JS decompress\n",
                                         (unsigned long long)pVCpu->cpum.GstCtx.rip);
                                RTStrmFlush(g_pStdOut);
                                int rcFB = wasmFastBootDecompress();
                                if (rcFB == 1)
                                {
                                    uint8_t abMeta64[20];
                                    PGMPhysRead(pVM, (RTGCPHYS)0x7200, abMeta64, sizeof(abMeta64), PGMACCESSORIGIN_IEM);
                                    uint32_t uMagic64 = *(uint32_t *)&abMeta64[0];
                                    if (uMagic64 == 0x42343644) /* "D64B" */
                                    {
                                        PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
                                        uint32_t uCR3 = *(uint32_t *)&abMeta64[4];
                                        uint64_t uEntry = *(uint64_t *)&abMeta64[8];
                                        uint32_t uBootParams = *(uint32_t *)&abMeta64[16];

                                        RTStrmPrintf(g_pStdErr, "[FAST-BOOT-CPP] D64B: CR3=0x%08x entry=0x%016llx bp=0x%08x\n",
                                                 uCR3, (unsigned long long)uEntry, uBootParams);

                                        pCtx->gdtr.pGdt  = 0x7300;
                                        pCtx->gdtr.cbGdt = 31;
                                        pCtx->cr3 = uCR3;
                                        pCtx->cr4 = X86_CR4_PAE | X86_CR4_PSE;
                                        pCtx->cr0 = X86_CR0_PE | X86_CR0_PG | X86_CR0_ET
                                                  | X86_CR0_NE | X86_CR0_WP | X86_CR0_MP;
                                        pCtx->msrEFER = MSR_K6_EFER_LME | MSR_K6_EFER_LMA | MSR_K6_EFER_NXE;
                                        PGMNotifyNxeChanged(pVCpu, true);

                                        pCtx->cs.Sel = 0x10; pCtx->cs.ValidSel = 0x10;
                                        pCtx->cs.fFlags = CPUMSELREG_FLAGS_VALID;
                                        pCtx->cs.u64Base = 0; pCtx->cs.u32Limit = UINT32_MAX;
                                        pCtx->cs.Attr.u = 0xA09B;

                                        PCPUMSELREG apS[] = { &pCtx->ds, &pCtx->es, &pCtx->fs, &pCtx->gs, &pCtx->ss };
                                        for (unsigned i = 0; i < RT_ELEMENTS(apS); i++)
                                        {
                                            apS[i]->Sel = 0x18; apS[i]->ValidSel = 0x18;
                                            apS[i]->fFlags = CPUMSELREG_FLAGS_VALID;
                                            apS[i]->u64Base = 0; apS[i]->u32Limit = UINT32_MAX;
                                            apS[i]->Attr.u = 0xC093;
                                        }

                                        pCtx->rip = uEntry;
                                        pCtx->rsi = uBootParams;
                                        pCtx->rax = 0; pCtx->rbx = 0; pCtx->rcx = 0; pCtx->rdx = 0;
                                        pCtx->rdi = 0; pCtx->rbp = 0;
                                        pCtx->rsp = 0x9FFC0;
                                        pCtx->rflags.u = X86_EFL_1;
                                        pCtx->idtr.pIdt = 0; pCtx->idtr.cbIdt = 0;
                                        pCtx->cr2 = 0;

                                        /* Also zero R8-R15 for clean state */
                                        pCtx->r8 = 0; pCtx->r9 = 0; pCtx->r10 = 0; pCtx->r11 = 0;
                                        pCtx->r12 = 0; pCtx->r13 = 0; pCtx->r14 = 0; pCtx->r15 = 0;

                                        VMCPU_FF_SET(pVCpu, VMCPU_FF_PGM_SYNC_CR3);

                                        /* ── Reset TM virtual sync catch-up ──
                                         * See TMVirtualSyncResetCatchUpWasm() in TMAllVirtual.cpp. */
                                        {
                                            extern void TMVirtualSyncResetCatchUpWasm(PVMCC);
                                            TMVirtualSyncResetCatchUpWasm(pVM);
                                            RTStrmPrintf(g_pStdErr, "[FAST-BOOT-CPP] TM sync reset: offVirtualSync=0 catchUp=false\n");
                                        }

                                        s_cBootInsns = 0; /* reset trace counter */
                                        s_fFB64Active = true;

                                        RTStrmPrintf(g_pStdErr, "[FAST-BOOT-CPP] Entering 64-bit kernel: RIP=%#018llx RSI=%#010x CR3=%#010llx\n",
                                                 (unsigned long long)pCtx->rip, (unsigned)pCtx->rsi,
                                                 (unsigned long long)pCtx->cr3);
                                        /* Verify bytes at entry point via PGM */
                                        {
                                            uint8_t abEntry[16];
                                            int rcPgm = PGMPhysRead(pVCpu->CTX_SUFF(pVM), pCtx->rip, abEntry, 16, PGMACCESSORIGIN_IEM);
                                            RTStrmPrintf(g_pStdErr, "[FAST-BOOT-CPP] PGMPhysRead(@RIP=%#llx) rc=%d: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                                                     (unsigned long long)pCtx->rip, rcPgm,
                                                     abEntry[0], abEntry[1], abEntry[2], abEntry[3],
                                                     abEntry[4], abEntry[5], abEntry[6], abEntry[7],
                                                     abEntry[8], abEntry[9], abEntry[10], abEntry[11],
                                                     abEntry[12], abEntry[13], abEntry[14], abEntry[15]);
                                        }
                                        RTStrmFlush(g_pStdErr);
                                        /* Re-init decoder for new mode */
                                        iemReInitDecoder(pVCpu);
                                        continue;
                                    }
                                    else
                                        RTPrintf("[FAST-BOOT-CPP] D64B magic mismatch: 0x%08x\n", uMagic64);
                                }
                                else
                                    RTPrintf("[FAST-BOOT-CPP] JS decompress returned %d (slow boot)\n", rcFB);
                                RTStrmFlush(g_pStdOut);
                            }
                            } /* if HdrS found */
                        }

                        /* Detailed trace: first 200 IEM instructions after boot */
                        if (s_cBootInsns <= 10)
                        {
                            RTPrintf("[DBOOT-INSN] #%llu CS=%04x EIP=%08llx FL=%08x ESP=%08x EAX=%08x\n",
                                     (unsigned long long)s_cBootInsns, uCS,
                                     (unsigned long long)pVCpu->cpum.GstCtx.rip,
                                     pVCpu->cpum.GstCtx.eflags.u,
                                     (unsigned)pVCpu->cpum.GstCtx.rsp,
                                     (unsigned)pVCpu->cpum.GstCtx.rax);
                            if (s_cBootInsns % 5 == 0) RTStrmFlush(g_pStdOut);
                        }
                        /* Log CS changes */
                        if (uCS != s_uLastCS)
                        {
                            RTPrintf("[DBOOT] CS change: %04x->%04x at insn #%llu EIP=%08llx CR0=%08llx FL=%08x\n",
                                     s_uLastCS, uCS, (unsigned long long)s_cBootInsns,
                                     (unsigned long long)pVCpu->cpum.GstCtx.rip,
                                     (unsigned long long)pVCpu->cpum.GstCtx.cr0,
                                     pVCpu->cpum.GstCtx.eflags.u);
                            RTStrmFlush(g_pStdOut);
                            s_uLastCS = uCS;
                        }
                        /* Periodic progress every 50M instructions */
                        if (s_cBootInsns - s_cLastReport >= 200000000) /* was 50M, now 200M */
                        {
                            s_cLastReport = s_cBootInsns;
                            uint64_t cr3 = pVCpu->cpum.GstCtx.cr3;
                            uint64_t rsp = pVCpu->cpum.GstCtx.rsp;
                            RTPrintf("[DBOOT] Progress: %lluK insns CS=%04x EIP=%08llx CR0=%08llx CR3=%08llx FL=%08x RSP=%016llx\n",
                                     (unsigned long long)(s_cBootInsns / 1000), uCS,
                                     (unsigned long long)pVCpu->cpum.GstCtx.rip,
                                     (unsigned long long)pVCpu->cpum.GstCtx.cr0,
                                     (unsigned long long)cr3,
                                     pVCpu->cpum.GstCtx.eflags.u,
                                     (unsigned long long)rsp);
                            RTStrmFlush(g_pStdOut);
                        }
                    }
                }
#endif
#if defined(VBOX_STRICT) && defined(VBOX_VMM_TARGET_X86)
                CPUMAssertGuestRFlagsCookie(pVM, pVCpu);
#endif
                if (RT_LIKELY(rcStrict == VINF_SUCCESS))
                {
                    Assert(ICORE(pVCpu).cActiveMappings == 0);
                    pVCpu->iem.s.cInstructions++;

#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
                    /* Perform any VMX nested-guest instruction boundary actions. */
                    uint64_t fCpu = pVCpu->fLocalForcedActions;
                    if (!(fCpu & (  VMCPU_FF_VMX_APIC_WRITE | VMCPU_FF_VMX_MTF | VMCPU_FF_VMX_PREEMPT_TIMER
                                  | VMCPU_FF_VMX_INT_WINDOW | VMCPU_FF_VMX_NMI_WINDOW)))
                    { /* likely */ }
                    else
                    {
                        rcStrict = iemHandleNestedInstructionBoundaryFFs(pVCpu, rcStrict);
                        if (RT_LIKELY(rcStrict == VINF_SUCCESS))
                            fCpu = pVCpu->fLocalForcedActions;
                        else
                        {
                            rcStrict = iemExecStatusCodeFiddling(pVCpu, rcStrict);
                            break;
                        }
                    }
#endif
                    if (RT_LIKELY(ICORE(pVCpu).rcPassUp == VINF_SUCCESS))
                    {
#ifndef VBOX_WITH_NESTED_HWVIRT_VMX
                        uint64_t fCpu = pVCpu->fLocalForcedActions;
#endif
                        fCpu &= VMCPU_FF_ALL_MASK & ~(  VMCPU_FF_PGM_SYNC_CR3
                                                      | VMCPU_FF_PGM_SYNC_CR3_NON_GLOBAL
                                                      | VMCPU_FF_TLB_FLUSH
                                                      | VMCPU_FF_UNHALT );
#ifdef __EMSCRIPTEN__
                        if (   pVCpu->cpum.GstCtx.cr2 == UINT64_C(0xC0DEBA5E)
                            && --cMaxInstructionsGccStupidity > 0)
                        {
                            Assert(ICORE(pVCpu).cActiveMappings == 0);
                            iemReInitDecoder(pVCpu);
                            continue;
                        }
#endif

                        if (RT_LIKELY(   iemExecLoopTargetCheckMaskedCpuFFs(pVCpu, fCpu)
                                      && !VM_FF_IS_ANY_SET(pVM, VM_FF_ALL_MASK) ))
                        {
                            if (--cMaxInstructionsGccStupidity > 0)
                            {
                                /* Poll timers every now an then according to the caller's specs. */
                                if (   (cMaxInstructionsGccStupidity & cPollRate) != 0
                                    || !TMTimerPollBool(pVM, pVCpu))
                                {
                                    Assert(ICORE(pVCpu).cActiveMappings == 0);
                                    iemReInitDecoder(pVCpu);
                                    continue;
                                }
#ifdef __EMSCRIPTEN__
                                /* Timer poll detected expiry — log periodically */
                                {
                                    static uint32_t s_cTimerHits = 0;
                                    ++s_cTimerHits;
                                    if (0) /* suppressed — TIMER-POLL */
                                    {
                                        extern volatile uint64_t g_cWasmVirtualInstructions;
                                        RTPrintf("[TIMER-POLL] hit #%u insns=%llu FFs=%#RX64 CR2=%#RX64 IF=%d\n",
                                                 s_cTimerHits,
                                                 (unsigned long long)g_cWasmVirtualInstructions,
                                                 (uint64_t)pVCpu->fLocalForcedActions,
                                                 pVCpu->cpum.GstCtx.cr2,
                                                 !!(pVCpu->cpum.GstCtx.eflags.u & X86_EFL_IF));
                                        RTStrmFlush(g_pStdOut);
                                    }
                                }
#endif
                            }
                        }
                    }
                    Assert(ICORE(pVCpu).cActiveMappings == 0);
                }
                else if (ICORE(pVCpu).cActiveMappings > 0)
                    iemMemRollback(pVCpu);
                rcStrict = iemExecStatusCodeFiddling(pVCpu, rcStrict);
                break;
            }
        }
        IEM_CATCH_LONGJMP_BEGIN(pVCpu, rcStrict);
        {
            if (ICORE(pVCpu).cActiveMappings > 0)
                iemMemRollback(pVCpu);
#if defined(VBOX_WITH_NESTED_HWVIRT_SVM) || defined(VBOX_WITH_NESTED_HWVIRT_VMX)
            rcStrict = iemExecStatusCodeFiddling(pVCpu, rcStrict);
#endif
            pVCpu->iem.s.cLongJumps++;
        }
        IEM_CATCH_LONGJMP_END(pVCpu);

#ifdef VBOX_STRICT
        iemInitExecTailStrictTarget(pVCpu);
#endif
    }
    else
    {
        if (ICORE(pVCpu).cActiveMappings > 0)
            iemMemRollback(pVCpu);

#if defined(VBOX_WITH_NESTED_HWVIRT_SVM) || defined(VBOX_WITH_NESTED_HWVIRT_VMX)
        /*
         * When a nested-guest causes an exception intercept (e.g. #PF) when fetching
         * code as part of instruction execution, we need this to fix-up VINF_SVM_VMEXIT.
         */
        rcStrict = iemExecStatusCodeFiddling(pVCpu, rcStrict);
#endif
    }

    /*
     * Maybe re-enter raw-mode and log.
     */
#ifdef LOG_ENABLED
    if (rcStrict != VINF_SUCCESS)
        LOGFLOW_REG_STATE_EX("IEMExecLots", " - rcStrict=%Rrc", VBOXSTRICTRC_VAL(rcStrict));
#endif
    if (pcInstructions)
        *pcInstructions = pVCpu->iem.s.cInstructions - cInstructionsAtStart;
    return rcStrict;
}


/**
 * Interface used by EMExecuteExec, does exit statistics and limits.
 *
 * @returns Strict VBox status code.
 * @param   pVCpu               The cross context virtual CPU structure.
 * @param   fWillExit           To be defined.
 * @param   cMaxInstructions    Maximum number of instructions to execute.
 * @param   cMaxInstructionsWithoutExits
 *                              The max number of instructions without exits.
 * @param   pStats              Where to return statistics.
 * @sa      IEMExecRecompilerForExits, EMExecuteExec
 */
VMM_INT_DECL(VBOXSTRICTRC) IEMExecForExits(PVMCPUCC pVCpu, uint32_t fWillExit, uint32_t cMaxInstructions,
                                           uint32_t cMaxInstructionsWithoutExits, PIEMEXECFOREXITSTATS pStats)
{
    NOREF(fWillExit); /** @todo define flexible exit crits */

    /*
     * Initialize return stats.
     */
    pStats->cInstructions    = 0;
    pStats->cExits           = 0;
    pStats->cMaxExitDistance = 0;
    pStats->enmReturnReason  = kIemExecForExitRetReason_Normal;

    /*
     * Initial decoder init w/ prefetch, then setup setjmp.
     */
    VBOXSTRICTRC rcStrict = iemInitDecoderAndPrefetchOpcodes(pVCpu, 0 /*fExecOpts*/);
    if (rcStrict == VINF_SUCCESS)
    {
        ICORE(pVCpu).cActiveMappings     = 0; /** @todo wtf?!? */
        IEM_TRY_SETJMP(pVCpu, rcStrict)
        {
#ifdef IN_RING0
            bool const fCheckPreemptionPending   = !RTThreadPreemptIsPossible() || !RTThreadPreemptIsEnabled(NIL_RTTHREAD);
#endif
            uint32_t   cInstructionSinceLastExit = 0;

            /*
             * The run loop.  We limit ourselves to 4096 instructions right now.
             */
            PVM pVM = pVCpu->CTX_SUFF(pVM);
            for (;;)
            {
                /*
                 * Log the state.
                 */
#ifdef LOG_ENABLED
                iemLogCurInstr(pVCpu, "IEMExecForExits");
#endif

                /*
                 * Do the decoding and emulation.
                 */
                uint32_t const cPotentialExits = ICORE(pVCpu).cPotentialExits;

                rcStrict = iemExecDecodeAndInterpretTargetInstruction(pVCpu);

                if (   cPotentialExits != ICORE(pVCpu).cPotentialExits
                    && cInstructionSinceLastExit > 0 /* don't count the first */ )
                {
                    pStats->cExits += 1;
                    if (cInstructionSinceLastExit > pStats->cMaxExitDistance)
                        pStats->cMaxExitDistance = cInstructionSinceLastExit;
                    cInstructionSinceLastExit = 0;
                }

                if (RT_LIKELY(rcStrict == VINF_SUCCESS))
                {
                    Assert(ICORE(pVCpu).cActiveMappings == 0);
                    pVCpu->iem.s.cInstructions++;
                    pStats->cInstructions++;
                    cInstructionSinceLastExit++;

#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
                    /* Perform any VMX nested-guest instruction boundary actions. */
                    uint64_t fCpu = pVCpu->fLocalForcedActions;
                    if (!(fCpu & (  VMCPU_FF_VMX_APIC_WRITE | VMCPU_FF_VMX_MTF | VMCPU_FF_VMX_PREEMPT_TIMER
                                  | VMCPU_FF_VMX_INT_WINDOW | VMCPU_FF_VMX_NMI_WINDOW)))
                    { /* likely */ }
                    else
                    {
                        rcStrict = iemHandleNestedInstructionBoundaryFFs(pVCpu, rcStrict);
                        if (RT_LIKELY(rcStrict == VINF_SUCCESS))
                            fCpu = pVCpu->fLocalForcedActions;
                        else
                        {
                            rcStrict = iemExecStatusCodeFiddling(pVCpu, rcStrict);
                            pStats->enmReturnReason = kIemExecForExitRetReason_ForcedFlag;
                            break;
                        }
                    }
#endif
                    if (RT_LIKELY(ICORE(pVCpu).rcPassUp == VINF_SUCCESS))
                    {
#ifndef VBOX_WITH_NESTED_HWVIRT_VMX
                        uint64_t fCpu = pVCpu->fLocalForcedActions;
#endif
                        fCpu &= VMCPU_FF_ALL_MASK & ~(  VMCPU_FF_PGM_SYNC_CR3
                                                      | VMCPU_FF_PGM_SYNC_CR3_NON_GLOBAL
                                                      | VMCPU_FF_TLB_FLUSH
                                                      | VMCPU_FF_UNHALT );
#ifdef __EMSCRIPTEN__
                        if (   pVCpu->cpum.GstCtx.cr2 == UINT64_C(0xC0DEBA5E)
                            && pStats->cInstructions < cMaxInstructions)
                        {
                            Assert(ICORE(pVCpu).cActiveMappings == 0);
                            iemReInitDecoder(pVCpu);
                            continue;
                        }
#endif
                        if (RT_LIKELY(   iemExecLoopTargetCheckMaskedCpuFFs(pVCpu, fCpu)
                                      && !VM_FF_IS_ANY_SET(pVM, VM_FF_ALL_MASK) ))
                        {
                            if (cInstructionSinceLastExit <= cMaxInstructionsWithoutExits)
                            {
                                if (pStats->cInstructions < cMaxInstructions)
                                {
#ifdef IN_RING0
                                    if (   !fCheckPreemptionPending
                                        || !RTThreadPreemptIsPending(NIL_RTTHREAD))
#endif
                                    {
                                        Assert(ICORE(pVCpu).cActiveMappings == 0);
                                        iemReInitDecoder(pVCpu);
                                        continue;
                                    }
#ifdef IN_RING0
                                    rcStrict = VINF_EM_RAW_INTERRUPT;
                                    pStats->enmReturnReason = kIemExecForExitRetReason_HostInterrupt;
                                    break;
#endif
                                }
                                else
                                    pStats->enmReturnReason = kIemExecForExitRetReason_LimitMaxInstructions;
                            }
                            else
                                pStats->enmReturnReason = kIemExecForExitRetReason_LimitMaxDistance;
                        }
                        else
                            pStats->enmReturnReason = kIemExecForExitRetReason_ForcedFlag;
                        Assert(!(fCpu & VMCPU_FF_IEM));
                    }
                    Assert(ICORE(pVCpu).cActiveMappings == 0);
                }
                else if (ICORE(pVCpu).cActiveMappings > 0)
                    iemMemRollback(pVCpu);
                rcStrict = iemExecStatusCodeFiddling(pVCpu, rcStrict);
                break;
            }
        }
        IEM_CATCH_LONGJMP_BEGIN(pVCpu, rcStrict);
        {
            if (ICORE(pVCpu).cActiveMappings > 0)
                iemMemRollback(pVCpu);
            pVCpu->iem.s.cLongJumps++;
            pStats->enmReturnReason = kIemExecForExitRetReason_LongJump;
        }
        IEM_CATCH_LONGJMP_END(pVCpu);

#ifdef VBOX_STRICT
        iemInitExecTailStrictTarget(pVCpu);
#endif
    }
    else
    {
        if (ICORE(pVCpu).cActiveMappings > 0)
            iemMemRollback(pVCpu);

#if defined(VBOX_WITH_NESTED_HWVIRT_SVM) || defined(VBOX_WITH_NESTED_HWVIRT_VMX)
        /*
         * When a nested-guest causes an exception intercept (e.g. #PF) when fetching
         * code as part of instruction execution, we need this to fix-up VINF_SVM_VMEXIT.
         */
        rcStrict = iemExecStatusCodeFiddling(pVCpu, rcStrict);
#endif
    }

    /*
     * Maybe re-enter raw-mode and log.
     */
#ifdef LOG_ENABLED
    if (rcStrict != VINF_SUCCESS)
        LOGFLOW_REG_STATE_EX("IEMExecLots", " - rcStrict=%Rrc; ins=%u exits=%u maxdist=%u",
                             VBOXSTRICTRC_VAL(rcStrict), pStats->cInstructions, pStats->cExits, pStats->cMaxExitDistance);
#endif
    return rcStrict;
}


/**
 * Injects a trap, fault, abort, software interrupt or external interrupt.
 *
 * The parameter list matches TRPMQueryTrapAll pretty closely.
 *
 * @returns Strict VBox status code.
 * @param   pVCpu               The cross context virtual CPU structure of the calling EMT.
 * @param   u8TrapNo            The trap number.
 * @param   enmType             What type is it (trap/fault/abort), software
 *                              interrupt or hardware interrupt.
 * @param   uErrCode            The error code if applicable.
 * @param   uCr2                The CR2 value if applicable.
 * @param   cbInstr             The instruction length (only relevant for
 *                              software interrupts).
 * @note    x86 specific, but difficult to move due to iemInitDecoder dep.
 */
VMM_INT_DECL(VBOXSTRICTRC)
IEMInjectTrap(PVMCPUCC pVCpu, uint8_t u8TrapNo, TRPMEVENT enmType, uint16_t uErrCode, RTGCPTR uCr2, uint8_t cbInstr)
{
#ifdef VBOX_VMM_TARGET_X86
    iemInitDecoder(pVCpu, 0 /*fExecOpts*/); /** @todo wrong init function! */
# ifdef DBGFTRACE_ENABLED
    RTTraceBufAddMsgF(pVCpu->CTX_SUFF(pVM)->CTX_SUFF(hTraceBuf), "IEMInjectTrap: %x %d %x %llx",
                      u8TrapNo, enmType, uErrCode, uCr2);
# endif

    uint32_t fFlags;
    switch (enmType)
    {
        case TRPM_HARDWARE_INT:
            Log(("IEMInjectTrap: %#4x ext\n", u8TrapNo));
            fFlags = IEM_XCPT_FLAGS_T_EXT_INT;
            uErrCode = uCr2 = 0;
            break;

        case TRPM_SOFTWARE_INT:
            Log(("IEMInjectTrap: %#4x soft\n", u8TrapNo));
            fFlags = IEM_XCPT_FLAGS_T_SOFT_INT;
            uErrCode = uCr2 = 0;
            break;

        case TRPM_TRAP:
        case TRPM_NMI: /** @todo Distinguish NMI from exception 2. */
            Log(("IEMInjectTrap: %#4x trap err=%#x cr2=%#RGv\n", u8TrapNo, uErrCode, uCr2));
            fFlags = IEM_XCPT_FLAGS_T_CPU_XCPT;
            if (u8TrapNo == X86_XCPT_PF)
                fFlags |= IEM_XCPT_FLAGS_CR2;
            switch (u8TrapNo)
            {
                case X86_XCPT_DF:
                case X86_XCPT_TS:
                case X86_XCPT_NP:
                case X86_XCPT_SS:
                case X86_XCPT_PF:
                case X86_XCPT_AC:
                case X86_XCPT_GP:
                    fFlags |= IEM_XCPT_FLAGS_ERR;
                    break;
            }
            break;

        IEM_NOT_REACHED_DEFAULT_CASE_RET();
    }

    VBOXSTRICTRC rcStrict = iemRaiseXcptOrInt(pVCpu, cbInstr, u8TrapNo, fFlags, uErrCode, uCr2);

    if (ICORE(pVCpu).cActiveMappings > 0)
        iemMemRollback(pVCpu);

    return rcStrict;

#else  /* !VBOX_VMM_TARGET_X86 */
    RT_NOREF(pVCpu, u8TrapNo, enmType, uErrCode, uCr2, cbInstr);
    AssertFailedReturn(VERR_NOT_IMPLEMENTED);
#endif /* !VBOX_VMM_TARGET_X86 */
}


/**
 * Injects the active TRPM event.
 *
 * @returns Strict VBox status code.
 * @param   pVCpu               The cross context virtual CPU structure.
 */
VMM_INT_DECL(VBOXSTRICTRC) IEMInjectTrpmEvent(PVMCPUCC pVCpu)
{
#ifndef IEM_IMPLEMENTS_TASKSWITCH
    IEM_RETURN_ASPECT_NOT_IMPLEMENTED_LOG(("Event injection\n"));
#else
    uint8_t     u8TrapNo;
    TRPMEVENT   enmType;
    uint32_t    uErrCode;
    RTGCUINTPTR uCr2;
    uint8_t     cbInstr;
    int rc = TRPMQueryTrapAll(pVCpu, &u8TrapNo, &enmType, &uErrCode, &uCr2, &cbInstr, NULL /* fIcebp */);
    if (RT_FAILURE(rc))
        return rc;

    /** @todo r=ramshankar: Pass ICEBP info. to IEMInjectTrap() below and handle
     *        ICEBP \#DB injection as a special case. */
    VBOXSTRICTRC rcStrict = IEMInjectTrap(pVCpu, u8TrapNo, enmType, uErrCode, uCr2, cbInstr);
#ifdef VBOX_WITH_NESTED_HWVIRT_SVM
    if (rcStrict == VINF_SVM_VMEXIT)
        rcStrict = VINF_SUCCESS;
#endif
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
    if (rcStrict == VINF_VMX_VMEXIT)
        rcStrict = VINF_SUCCESS;
#endif
    /** @todo Are there any other codes that imply the event was successfully
     *        delivered to the guest? See @bugref{6607}.  */
    if (   rcStrict == VINF_SUCCESS
        || rcStrict == VINF_IEM_RAISED_XCPT)
        TRPMResetTrap(pVCpu);

    return rcStrict;
#endif
}


VMM_INT_DECL(int) IEMBreakpointSet(PVM pVM, RTGCPTR GCPtrBp)
{
    RT_NOREF_PV(pVM); RT_NOREF_PV(GCPtrBp);
    return VERR_NOT_IMPLEMENTED;
}


VMM_INT_DECL(int) IEMBreakpointClear(PVM pVM, RTGCPTR GCPtrBp)
{
    RT_NOREF_PV(pVM); RT_NOREF_PV(GCPtrBp);
    return VERR_NOT_IMPLEMENTED;
}

