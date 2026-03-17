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
                 * TMAllVirtual.cpp reads this to drive timer expiration under Wasm. */
                {
                    extern volatile uint64_t g_cWasmVirtualInstructions;

                    /* Set up s_pVMForRead early so wasmReadGuestPhys() works from JS
                     * before DELAY-ACCEL fires (e.g., during decompression). */
                    if (RT_UNLIKELY(!s_pVMForRead))
                        s_pVMForRead = pVCpu->CTX_SUFF(pVM);
                    /* Track virtual time boost separately so we don't corrupt
                     * IEM's internal instruction counter (pVCpu->iem.s.cInstructions).
                     * The boost is accumulated from skipped delay loops and added to
                     * the real counter to form the virtual counter that drives timers. */
                    static uint64_t s_cVirtualTimeBoost = 0;
                    g_cWasmVirtualInstructions = pVCpu->iem.s.cInstructions + s_cVirtualTimeBoost;

                    /* High-frequency __delay() fast path: once we've identified the
                     * delay loop address, check every 1000 instructions and skip it
                     * immediately by setting the counter register to 1.  This is much
                     * faster than waiting for the 1M-instruction diagnostic interval. */
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
                                /* Advance virtual time by the number of instructions we're skipping.
                                 * Each loop iteration is 2 instructions (dec + jnz).  This ensures
                                 * the timer system sees time passing, so timeout loops in hardware
                                 * init code eventually exit instead of spinning forever. */
                                uint64_t remaining = pVCpu->cpum.GstCtx.rax;
                                if (remaining > 1)
                                    s_cVirtualTimeBoost += remaining * 2;
                                g_cWasmVirtualInstructions = pVCpu->iem.s.cInstructions + s_cVirtualTimeBoost;
                                pVCpu->cpum.GstCtx.rax = 1;
                            }
                        }
                    }

                    /* Periodic diagnostic: log timer/FF state.
                     * Interval: 1M when exploring, 1B when delay accelerator is active
                     * (to prevent IEM-DIAG from flooding the console and hiding
                     * DELAY-ACCEL stack dumps). */
                    static uint64_t s_cNextDiag = 1000000;
                    if (g_cWasmVirtualInstructions >= s_cNextDiag)
                    {
                        s_cNextDiag = g_cWasmVirtualInstructions
                                    + (s_uDelayRip ? UINT64_C(1000000000) : UINT64_C(1000000));
                        uint64_t fFFs = pVCpu->fLocalForcedActions;
                        RTPrintf("[IEM-DIAG] insns=%llu CR2=%#llx CR0=%#llx EFER=%#llx IF=%d FFs=%#RX64 EIP=%#llx\n",
                                 (unsigned long long)g_cWasmVirtualInstructions,
                                 (unsigned long long)pVCpu->cpum.GstCtx.cr2,
                                 (unsigned long long)pVCpu->cpum.GstCtx.cr0,
                                 (unsigned long long)pVCpu->cpum.GstCtx.msrEFER,
                                 !!(pVCpu->cpum.GstCtx.eflags.u & X86_EFL_IF),
                                 fFFs,
                                 (unsigned long long)pVCpu->cpum.GstCtx.rip);

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

                        /* Force-enable IF if kernel is stuck with IF=0 AFTER
                           decompression completes (RIP in high virtual addresses).
                           The decompressor runs with IF=0 legitimately; forcing IF=1
                           during decompression corrupts it.  Only trigger once the
                           kernel has jumped to decompressed code (RIP > 0xFFFF8000...). */
                        {
                            static bool s_fForceIF = false;
                            if (!s_fForceIF
                                && (pVCpu->cpum.GstCtx.msrEFER & MSR_K6_EFER_LMA)
                                && !(pVCpu->cpum.GstCtx.eflags.u & X86_EFL_IF)
                                && pVCpu->cpum.GstCtx.rip > UINT64_C(0xFFFF800000000000))
                            {
                                s_fForceIF = true;
                                pVCpu->cpum.GstCtx.eflags.u |= X86_EFL_IF;
                                RTPrintf("[FORCE-IF] Enabling IF at insns=%llu RIP=%#llx — unsticking kernel init\n",
                                    (unsigned long long)g_cWasmVirtualInstructions,
                                    (unsigned long long)pVCpu->cpum.GstCtx.rip);
                            }
                        }

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
                            if (s_cSameRip == 10)
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
                            if (s_cSameRip >= 20 && curRip != s_uLastAccelRip)
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
                    if (++s_cDiagCounter >= 500000)
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
                {
                    static bool     s_fBootActive = false;
                    static uint16_t s_uLastCS = 0;
                    static uint64_t s_cBootInsns = 0;
                    static uint64_t s_cLastReport = 0;
                    if (!s_fBootActive
                        && (   pVCpu->cpum.GstCtx.cr2 == UINT64_C(0xC0DEBA5E)
                            || (pVCpu->cpum.GstCtx.msrEFER & MSR_K6_EFER_LMA)))
                    {
                        s_fBootActive = true;
                        s_uLastCS = pVCpu->cpum.GstCtx.cs.Sel;
                        RTPrintf("[DBOOT] Kernel boot active (%s), CS=%04x RIP=%#018llx CR0=%08llx\n",
                                 (pVCpu->cpum.GstCtx.msrEFER & MSR_K6_EFER_LMA) ? "64-bit LM" : "32-bit PM",
                                 s_uLastCS, (unsigned long long)pVCpu->cpum.GstCtx.rip,
                                 (unsigned long long)pVCpu->cpum.GstCtx.cr0);
                        RTStrmFlush(g_pStdOut);

                        /* Try JS fast boot right when 32-bit PM boot starts —
                           BEFORE the decompressor runs millions of instructions.
                           The JIT already loaded the kernel to 0x100000 and set
                           boot_params at 0x90000.  Call JS to decompress payload,
                           build page tables, and write D64B metadata at 0x7200. */
                        if (!(pVCpu->cpum.GstCtx.msrEFER & MSR_K6_EFER_LMA))
                        {
                            RTPrintf("[FAST-BOOT-CPP] Attempting JS fast boot decompress...\n");
                            RTStrmFlush(g_pStdOut);
                            int rcFB = wasmFastBootDecompress();
                            if (rcFB == 1)
                            {
                                /* Read D64B metadata written by JS */
                                uint8_t abMeta64[20];
                                PGMPhysRead(pVM, (RTGCPHYS)0x7200, abMeta64, sizeof(abMeta64), PGMACCESSORIGIN_IEM);
                                uint32_t uMagic64 = *(uint32_t *)&abMeta64[0];
                                if (uMagic64 == 0x42343644) /* "D64B" */
                                {
                                    PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
                                    uint32_t uCR3 = *(uint32_t *)&abMeta64[4];
                                    uint64_t uEntry = *(uint64_t *)&abMeta64[8];
                                    uint32_t uBootParams = *(uint32_t *)&abMeta64[16];

                                    RTPrintf("[FAST-BOOT-CPP] D64B: CR3=0x%08x entry=0x%016llx bp=0x%08x\n",
                                             uCR3, (unsigned long long)uEntry, uBootParams);

                                    /* Set up GDT from guest 0x7300 (written by JS) */
                                    pCtx->gdtr.pGdt  = 0x7300;
                                    pCtx->gdtr.cbGdt = 31;

                                    /* Enable paging + long mode */
                                    pCtx->cr3 = uCR3;
                                    pCtx->cr4 = X86_CR4_PAE | X86_CR4_PSE;
                                    pCtx->cr0 = X86_CR0_PE | X86_CR0_PG | X86_CR0_ET
                                              | X86_CR0_NE | X86_CR0_WP | X86_CR0_MP;
                                    pCtx->msrEFER = MSR_K6_EFER_LME | MSR_K6_EFER_LMA | MSR_K6_EFER_NXE;

                                    /* CS = 64-bit code segment (selector 0x10) */
                                    pCtx->cs.Sel      = 0x10;
                                    pCtx->cs.ValidSel = 0x10;
                                    pCtx->cs.fFlags   = CPUMSELREG_FLAGS_VALID;
                                    pCtx->cs.u64Base  = 0;
                                    pCtx->cs.u32Limit = UINT32_MAX;
                                    pCtx->cs.Attr.u   = 0xA09B;

                                    /* DS=ES=SS=FS=GS = 64-bit data (selector 0x18) */
                                    PCPUMSELREG apS[] = { &pCtx->ds, &pCtx->es, &pCtx->fs, &pCtx->gs, &pCtx->ss };
                                    for (unsigned i = 0; i < RT_ELEMENTS(apS); i++)
                                    {
                                        apS[i]->Sel      = 0x18;
                                        apS[i]->ValidSel = 0x18;
                                        apS[i]->fFlags   = CPUMSELREG_FLAGS_VALID;
                                        apS[i]->u64Base  = 0;
                                        apS[i]->u32Limit = UINT32_MAX;
                                        apS[i]->Attr.u   = 0xC093;
                                    }

                                    pCtx->rip = uEntry;
                                    pCtx->rsi = uBootParams;
                                    pCtx->rax = 0; pCtx->rbx = 0; pCtx->rcx = 0; pCtx->rdx = 0;
                                    pCtx->rdi = 0; pCtx->rbp = 0;
                                    pCtx->rsp = 0x9FFC0;
                                    pCtx->rflags.u = X86_EFL_1;
                                    pCtx->idtr.pIdt = 0;
                                    pCtx->idtr.cbIdt = 0;
                                    pCtx->cr2 = 0;

                                    VMCPU_FF_SET(pVCpu, VMCPU_FF_PGM_SYNC_CR3);

                                    RTPrintf("[FAST-BOOT-CPP] Entering 64-bit kernel: RIP=%#018llx RSI=%#010x\n",
                                             (unsigned long long)pCtx->rip, (unsigned)pCtx->rsi);
                                    RTStrmFlush(g_pStdOut);
                                }
                                else
                                    RTPrintf("[FAST-BOOT-CPP] D64B magic mismatch: 0x%08x\n", uMagic64);
                            }
                            else
                                RTPrintf("[FAST-BOOT-CPP] JS decompress returned %d (falling back to slow boot)\n", rcFB);
                            RTStrmFlush(g_pStdOut);
                        }
                    }
                    if (s_fBootActive)
                    {
                        s_cBootInsns++;
                        uint16_t uCS = pVCpu->cpum.GstCtx.cs.Sel;
                        /* Detailed trace: first 200 IEM instructions after boot */
                        if (s_cBootInsns <= 200)
                        {
                            RTPrintf("[DBOOT-INSN] #%llu CS=%04x EIP=%08llx FL=%08x ESP=%08x EAX=%08x\n",
                                     (unsigned long long)s_cBootInsns, uCS,
                                     (unsigned long long)pVCpu->cpum.GstCtx.rip,
                                     pVCpu->cpum.GstCtx.eflags.u,
                                     (unsigned)pVCpu->cpum.GstCtx.rsp,
                                     (unsigned)pVCpu->cpum.GstCtx.rax);
                            if (s_cBootInsns % 50 == 0) RTStrmFlush(g_pStdOut);
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
                        /* Periodic progress every 1M instructions */
                        if (s_cBootInsns - s_cLastReport >= 1000000)
                        {
                            s_cLastReport = s_cBootInsns;
                            uint64_t cr3 = pVCpu->cpum.GstCtx.cr3;
                            RTPrintf("[DBOOT] Progress: %lluK insns CS=%04x EIP=%08llx CR0=%08llx CR3=%08llx FL=%08x\n",
                                     (unsigned long long)(s_cBootInsns / 1000), uCS,
                                     (unsigned long long)pVCpu->cpum.GstCtx.rip,
                                     (unsigned long long)pVCpu->cpum.GstCtx.cr0,
                                     (unsigned long long)cr3,
                                     pVCpu->cpum.GstCtx.eflags.u);
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
                                    if (++s_cTimerHits <= 5 || (s_cTimerHits % 100) == 0)
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

