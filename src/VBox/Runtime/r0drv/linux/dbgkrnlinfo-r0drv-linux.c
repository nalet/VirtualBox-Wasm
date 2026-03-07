/* $Id: dbgkrnlinfo-r0drv-linux.c 112632 2026-01-19 10:51:49Z knut.osmundsen@oracle.com $ */
/** @file
 * IPRT - Kernel Debug Information, R0 Driver, Linux.
 */

/*
 * Copyright (C) 2025-2026 Oracle and/or its affiliates.
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
 * The contents of this file may alternatively be used under the terms
 * of the Common Development and Distribution License Version 1.0
 * (CDDL), a copy of it is provided in the "COPYING.CDDL" file included
 * in the VirtualBox distribution, in which case the provisions of the
 * CDDL are applicable instead of those of the GPL.
 *
 * You may elect to license modified versions of this file under the
 * terms and conditions of either the GPL or the CDDL or both.
 *
 * SPDX-License-Identifier: GPL-3.0-only OR CDDL-1.0
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#ifdef IN_RING0
# include "the-linux-kernel.h"
# include <linux/uio.h>
#else
# include <iprt/stream.h>
# define printk RTPrintf
# define RTLNX_VER_MIN(x, y, z) 1
#endif

#if !defined(IN_RING0) && !defined(DOXYGEN_RUNNING) /* A linking tweak for the testcase: */
# include <iprt/cdefs.h>
# undef  RTR0DECL
# define RTR0DECL(type) DECLHIDDEN(type) RTCALL
#endif

#include "internal/iprt.h"
#include <iprt/dbg.h>

#include <iprt/asm.h>
#include <iprt/assert.h>
#include <iprt/ctype.h>
#include <iprt/err.h>
#include <iprt/file.h>
#include <iprt/mem.h>
#include <iprt/string.h>
#include "internal/magics.h"

#if RTLNX_VER_MIN(3,16,0) || defined(IN_RING3) /** @todo support this for older kernels (see also initterm-r0drv-linux.c and fileio-r0drv-linux.c) */


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
/**
 * Linux kernel debug info instance data.
 */
typedef struct RTDBGKRNLINFOINT
{
    /** Magic value (RTDBGKRNLINFO_MAGIC). */
    uint32_t            u32Magic;
    /** Reference counter.  */
    uint32_t volatile   cRefs;
    /** The /proc/kallsyms file handle. */
    RTFILE              hFile;
    /** Buffer space (the file is typically several MBs, so larger is better). */
    char                abBuf[_16K - 64];
} RTDBGKRNLINFOINT;


/**
 * Destructor.
 *
 * @param   pThis               The instance to destroy.
 */
static void rtR0DbgKrnlLinuxDtor(RTDBGKRNLINFOINT *pThis)
{
    pThis->u32Magic = ~RTDBGKRNLINFO_MAGIC;

    RTFileClose(pThis->hFile);
    pThis->hFile = NIL_RTFILE;

    RTMemFree(pThis);
}

#ifdef IN_RING3
extern const char *g_pszTestKallsyms;
#endif

RTR0DECL(int) RTR0DbgKrnlInfoOpen(PRTDBGKRNLINFO phKrnlInfo, uint32_t fFlags)
{
#ifdef IN_RING3
    const char * const pszFilename = g_pszTestKallsyms ? g_pszTestKallsyms : "/proc/kallsyms";
#else
    const char * const pszFilename = "/proc/kallsyms";
#endif
    struct RTDBGKRNLINFOINT *pThis;
    RTFILE hFile;
    int rc;

    AssertPtrReturn(phKrnlInfo, VERR_INVALID_POINTER);
    *phKrnlInfo = NIL_RTDBGKRNLINFO;
    AssertReturn(!fFlags, VERR_INVALID_PARAMETER);

    /*
     * Try open the kernel symbol file.
     */
    rc = RTFileOpen(&hFile, pszFilename, RTFILE_O_READ | RTFILE_O_DENY_NONE | RTFILE_O_OPEN);
    if (RT_FAILURE(rc))
        return rc;

    /*
     * Allocate a handle structure for it.
     */
    pThis = RTMemAllocZ(sizeof(*pThis));
    if (pThis)
    {
        pThis->u32Magic = RTDBGKRNLINFO_MAGIC;
        pThis->cRefs    = 1;
        pThis->hFile    = hFile;

        *phKrnlInfo = pThis;
        return VINF_SUCCESS;
    }

    RTFileClose(hFile);
    return VERR_NO_MEMORY;
}


RTR0DECL(uint32_t) RTR0DbgKrnlInfoRetain(RTDBGKRNLINFO hKrnlInfo)
{
    RTDBGKRNLINFOINT *pThis = hKrnlInfo;
    AssertPtrReturn(pThis, UINT32_MAX);
    AssertMsgReturn(pThis->u32Magic == RTDBGKRNLINFO_MAGIC, ("%p: u32Magic=%RX32\n", pThis, pThis->u32Magic), UINT32_MAX);

    uint32_t cRefs = ASMAtomicIncU32(&pThis->cRefs);
    Assert(cRefs && cRefs < 100000);
    return cRefs;
}


RTR0DECL(uint32_t) RTR0DbgKrnlInfoRelease(RTDBGKRNLINFO hKrnlInfo)
{
    RTDBGKRNLINFOINT *pThis = hKrnlInfo;
    if (pThis == NIL_RTDBGKRNLINFO)
        return 0;
    AssertPtrReturn(pThis, UINT32_MAX);
    AssertMsgReturn(pThis->u32Magic == RTDBGKRNLINFO_MAGIC, ("%p: u32Magic=%RX32\n", pThis, pThis->u32Magic), UINT32_MAX);

    uint32_t cRefs = ASMAtomicDecU32(&pThis->cRefs);
    if (cRefs == 0)
        rtR0DbgKrnlLinuxDtor(pThis);
    return cRefs;
}


RTR0DECL(int) RTR0DbgKrnlInfoQueryMember(RTDBGKRNLINFO hKrnlInfo, const char *pszModule, const char *pszStructure,
                                         const char *pszMember, size_t *poffMember)
{
    RTDBGKRNLINFOINT *pThis = hKrnlInfo;
    AssertPtrReturn(pThis, VERR_INVALID_HANDLE);
    AssertMsgReturn(pThis->u32Magic == RTDBGKRNLINFO_MAGIC, ("%p: u32Magic=%RX32\n", pThis, pThis->u32Magic), VERR_INVALID_HANDLE);
    AssertPtrReturn(pszMember, VERR_INVALID_POINTER);
    AssertPtrReturn(pszModule, VERR_INVALID_POINTER);
    AssertPtrReturn(pszStructure, VERR_INVALID_POINTER);
    AssertPtrReturn(poffMember, VERR_INVALID_POINTER);
    return VERR_NOT_FOUND;
}


RTR0DECL(int) RTR0DbgKrnlInfoQuerySymbol(RTDBGKRNLINFO hKrnlInfo, const char *pszModule,
                                         const char *pszSymbol, void **ppvSymbol)
{
    RTDBGKRNLINFOINT *pThis = hKrnlInfo;
    size_t cchSymbol;
    size_t cchModule;
    size_t cchMinLineLength;
    AssertPtrReturn(pThis, VERR_INVALID_HANDLE);
    AssertMsgReturn(pThis->u32Magic == RTDBGKRNLINFO_MAGIC, ("%p: u32Magic=%RX32\n", pThis, pThis->u32Magic), VERR_INVALID_HANDLE);
    AssertPtrReturn(pszSymbol, VERR_INVALID_PARAMETER);
    AssertPtrNullReturn(ppvSymbol, VERR_INVALID_PARAMETER);
    cchSymbol        = strlen(pszSymbol);
    AssertPtrNullReturn(pszModule, VERR_MODULE_NOT_FOUND);
    cchModule        = pszModule ? strlen(pszModule) : 0;
    cchMinLineLength = ARCH_BITS / 4 + 1 + 1 + 1 + cchSymbol + (cchModule ? 2 + cchModule + 1 : 0);

    /*
     * Scan the entire file for the requested symbol.
     */
    char * const pchBuf  = pThis->abBuf;
    RTFOFF       offFile = 0;
    uint32_t     cbInBuf = 0;
    uint32_t     off     = 0;
    for (;;)
    {
        /*
         * Locate end of the current line, read more file content as needed.
         */
        uint32_t offLine = off;
        while (off < cbInBuf && pchBuf[off] != '\n')
            off++;
        if (off == cbInBuf)
        {
            size_t cbRead = 0;
            int    rc;

            /* Read more. (ASSUMES that we get full buffers returned and that
               the last line has a trailing newline as well.) */
            if (off != offLine)
            {
                if (offLine == 0)
                    break;
                off     -= offLine;
                memmove(pchBuf, &pchBuf[offLine], off);
            }
            else
                off = 0;
            cbInBuf = off;
            offLine = 0;
            rc = RTFileReadAt(pThis->hFile, offFile, &pchBuf[off], sizeof(pThis->abBuf) - off, &cbRead);
            //printk("dbgkrnlinfo: read %#lx bytes (rc=%d)\n", (unsigned long)cbRead, rc);
            if (RT_FAILURE(rc))
                return rc;
            offFile += cbRead;
            cbInBuf += cbRead;

            /* Continue the search for the end of line: */
            while (off < cbInBuf && pchBuf[off] != '\n')
                off++;
            if (off == cbInBuf)
                break;
        }

        /*
         * Try parse it.
         */
        pchBuf[off] = '\0'; /* terminate the line */
        //printk("dbgkrnlinfo: %s\n",  &pchBuf[offLine]);
        if (   off - offLine >= cchMinLineLength
            && off - offLine <= cchMinLineLength + 8) /* parnaoia - we could do an exact match here, actually. */
        {
            /* Parse the address. */
            char    *psz;
            uint64_t uAddr = 0;
            int rc = RTStrToUInt64Ex(&pchBuf[offLine], &psz, 16, &uAddr);
            if (rc == VWRN_TRAILING_CHARS && RT_C_IS_SPACE(*psz))
            {
                psz++; /* skip space */

                /* Check that it's a public symbol (we don't return local symbols at the moment). */
                char const chType = *psz;
                if (   (chType == 'T' || chType == 'D' || chType == 'B' || chType == 'R' || chType == 'V' || chType == 'W')
                    && RT_C_IS_BLANK(psz[1]))
                {
                    psz += 2; /* skip type & following space */

                    /* Match the symbol. */
                    if (strncmp(psz, pszSymbol, cchSymbol) == 0)
                    {
                        psz += cchSymbol;

                        /* If we're matching a kernel symbol and have reached the end of the line now, we're good. */
                        if (!cchModule && *psz == '\0')
                        {
                            if (ppvSymbol)
                                *ppvSymbol = (void *)(uintptr_t)uAddr;
                            return VINF_SUCCESS;
                        }

                        /* If we're matching a specific module, we must check the module name as well. */
                        if (cchModule && RT_C_IS_SPACE(*psz) && psz[1] == '[')
                        {
                            psz += 2;
                            if (strncmp(psz, pszModule, cchModule) == 0)
                            {
                                psz += cchModule;
                                if (*psz == ']')
                                {
                                    if (ppvSymbol)
                                        *ppvSymbol = (void *)(uintptr_t)uAddr;
                                    return VINF_SUCCESS;
                                }
                            }
                        }
                    }
                }
            }
        }

        /*
         * Advance to the next line (skips the newline).
         */
        off++;
    }

    return VERR_SYMBOL_NOT_FOUND;
}

#else  /* !RTLNX_VER_MIN(6,7,0) && !defined(IN_RING3) */

/*
 * Stubs to prevent linking issues with RTR0DbgKrnlInfoGetSymbol and such.
 */

RTR0DECL(int) RTR0DbgKrnlInfoOpen(PRTDBGKRNLINFO phKrnlInfo, uint32_t fFlags)
{
    RT_NOREF(phKrnlInfo, fFlags);
    return VERR_NOT_IMPLEMENTED;
}

RTR0DECL(uint32_t) RTR0DbgKrnlInfoRetain(RTDBGKRNLINFO hKrnlInfo)
{
    RT_NOREF(hKrnlInfo);
    return UINT32_MAX;
}

RTR0DECL(uint32_t) RTR0DbgKrnlInfoRelease(RTDBGKRNLINFO hKrnlInfo)
{
    RT_NOREF(hKrnlInfo);
    return UINT32_MAX;
}

RTR0DECL(int) RTR0DbgKrnlInfoQuerySymbol(RTDBGKRNLINFO hKrnlInfo, const char *pszModule,
                                         const char *pszSymbol, void **ppvSymbol)
{
    RT_NOREF(hKrnlInfo, pszModule, pszSymbol, ppvSymbol);
    return VERR_NOT_IMPLEMENTED;
}

#endif /* !RTLNX_VER_MIN(6,7,0) && !defined(IN_RING3) */

