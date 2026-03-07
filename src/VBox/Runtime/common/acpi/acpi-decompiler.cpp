/* $Id: acpi-decompiler.cpp 112663 2026-01-21 15:02:23Z alexander.eichner@oracle.com $ */
/** @file
 * IPRT - Advanced Configuration and Power Interface (ACPI) Table generation API.
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
#define LOG_GROUP RTLOGGROUP_ACPI
#include <iprt/acpi.h>
#include <iprt/asm.h>
#include <iprt/ctype.h>
#include <iprt/err.h>
#include <iprt/file.h>
#include <iprt/list.h>
#include <iprt/log.h>
#include <iprt/mem.h>
#include <iprt/string.h>
#include <iprt/strcache.h>
#include <iprt/uuid.h>

#include <iprt/formats/acpi-aml.h>
#include <iprt/formats/acpi-resources.h>

#include "internal/acpi.h"


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/



/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/


/**
 * AML object type.
 */
typedef enum RTACPITBLAMLOBJTYPE
{
    /** Invalid object type. */
    kAcpiAmlObjType_Invalid = 0,
    /** Unknown object type. */
    kAcpiAmlObjType_Unknown,
    /** Method object type. */
    kAcpiAmlObjType_Method,
    /** 32bit hack. */
    kAcpiAmlObjType_32Bit_Hack = 0x7fffffff
} RTACPITBLAMLOBJTYPE;


/**
 * Known object in namespace.
 */
typedef struct RTACPITBLAMLOBJ
{
    /** List node. */
    RTLISTNODE          NdObjs;
    /** Object Type. */
    RTACPITBLAMLOBJTYPE enmType;
    /** Additional data depending on the type. */
    union
    {
        /** Method object argument count. */
        uint32_t        cMethodArgs;
    } u;
    /** Zero terminated object name - variable in size. */
    char                szName[1];
} RTACPITBLAMLOBJ;
typedef RTACPITBLAMLOBJ *PRTACPITBLAMLOBJ;
typedef const RTACPITBLAMLOBJ *PCRTACPITBLAMLOBJ;



/**
 * ACPI AML -> ASL decoder state.
 */
typedef struct RTACPITBLAMLDECODE
{
    /** Pointe to the raw table data. */
    const uint8_t *pbTbl;
    /** Size of the table. */
    uint32_t      cbTbl;
    /** Offset into the table. */
    uint32_t      offTbl;
    /** Current stack level. */
    uint32_t      iLvl;
    /** Number of entries in the package stack. */
    uint32_t      cPkgStackMax;
    /** Stack of package lengths. */
    size_t        *pacbPkgLeft;
    /** Stack of original package lengths. */
    size_t        *pacbPkg;
    /** Stack of the list of AST nodes to append new nodes to. */
    PRTLISTANCHOR *papLstScopeNodes;
    /** The string cache holding identifiers and string literals. */
    RTSTRCACHE    hStrCache;
    /** The namespace. */
    PRTACPINSROOT pNs;
    /** List of known objects. */
    RTLISTANCHOR  LstObjs;
    /** Statement list on the root level. */
    RTLISTANCHOR  LstStmts;
} RTACPITBLAMLDECODE;
/** Pointer to a ACPI AML -> ASL decoder state. */
typedef RTACPITBLAMLDECODE *PRTACPITBLAMLDECODE;


/** Forward declaration of the AML opcode descriptor. */
typedef const struct RTACPIAMLOPC *PCRTACPIAMLOPC;

/**
 * ACPI AML -> ASL decode callback
 *
 * @returns IPRT status code.
 * @param   pThis               ACPI table decoder state.
 * @param   pAmlOpc             The AML opcode descriptor.
 * @param   bOpc                The opcode byte.
 * @param   pErrInfo            Where to return additional error information.
 */
typedef DECLCALLBACKTYPE(int, FNRTACPITBLAMLOPCDECODE,(PRTACPITBLAMLDECODE pThis, PCRTACPIAMLOPC pAmlOpc, uint8_t bOpc, PRTACPIASTNODE *ppAstNd, PRTERRINFO pErrInfo));
/** Pointer to a ACPI AML -> ASL decode callback. */
typedef FNRTACPITBLAMLOPCDECODE *PFNRTACPITBLAMLOPCDECODE;


typedef enum ACPIAMLOPCTYPE
{
    kAcpiAmlOpcType_Invalid = 0,
    kAcpiAmlOpcType_Byte,
    kAcpiAmlOpcType_Word,
    kAcpiAmlOpcType_DWord,
    kAcpiAmlOpcType_NameString,
    kAcpiAmlOpcType_TermArg,
    kAcpiAmlOpcType_SuperName,
    kAcpiAmlOpcType_Target,
    kAcpiAmlOpcType_RegionSpace,
    kAcpiAmlOpcType_32BitHack = 0x7fffffff
} ACPIAMLOPCTYPE;


typedef struct RTACPIAMLOPC
{
    /** The node type. */
    RTACPIASTNODEOP          enmOp;
    /** Flags for the opcode. */
    uint32_t                 fFlags;
    /** Number of arguments. */
    uint32_t                 cArgs;
    /** Opcode type for the fields following. */
    ACPIAMLOPCTYPE           aenmTypes[5];
    /** Optional decoder callback. */
    PFNRTACPITBLAMLOPCDECODE pfnDecode;
} RTACPIAMLOPC;
typedef RTACPIAMLOPC *PRTACPIAMLOPC;

#define RTACPI_AML_OPC_F_NONE           0
#define RTACPI_AML_OPC_F_HAS_PKG_LENGTH RT_BIT_32(0)
#define RTACPI_AML_OPC_F_NEW_SCOPE      RT_BIT_32(1)


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/


/*********************************************************************************************************************************
*   Internal Functions                                                                                                           *
*********************************************************************************************************************************/

static int rtAcpiTblAmlDecodeTerminal(PRTACPITBLAMLDECODE pThis, PRTACPIASTNODE *ppAstNd, PRTERRINFO pErrInfo);


DECLINLINE(int) rtAcpiTblAmlDecodeReadU8(PRTACPITBLAMLDECODE pThis, uint8_t *pb, PRTERRINFO pErrInfo)
{
    if (pThis->offTbl < pThis->cbTbl)
    { /* probable */ }
    else
        return RTErrInfoSetF(pErrInfo, VERR_EOF, "AML stream ended prematurely at offset '%#x' trying to read a byte", pThis->offTbl);

    if (!pThis->pacbPkgLeft[pThis->iLvl])
        return RTErrInfoSetF(pErrInfo, VERR_INVALID_STATE, "Data overflows current package limitation");

    pThis->pacbPkgLeft[pThis->iLvl]--;

    *pb = pThis->pbTbl[pThis->offTbl++];
    return VINF_SUCCESS;
}


DECLINLINE(int) rtAcpiTblAmlDecodeSkipU8IfEqual(PRTACPITBLAMLDECODE pThis, uint8_t ch, bool *pfSkipped, PRTERRINFO pErrInfo)
{
    if (pThis->offTbl < pThis->cbTbl)
    { /* probable */ }
    else
        return RTErrInfoSetF(pErrInfo, VERR_EOF, "AML stream ended prematurely at offset '%#x' trying to read a byte", pThis->offTbl);

    if (!pThis->pacbPkgLeft[pThis->iLvl])
        return RTErrInfoSetF(pErrInfo, VERR_INVALID_STATE, "Data overflows current package limitation");

    if (pThis->pbTbl[pThis->offTbl] == ch)
    {
        pThis->pacbPkgLeft[pThis->iLvl]--;
        pThis->offTbl++;
        *pfSkipped = true;
    }
    else
        *pfSkipped = false;
    return VINF_SUCCESS;
}


DECLINLINE(int) rtAcpiTblAmlDecodeReadU16(PRTACPITBLAMLDECODE pThis, uint16_t *pu16, PRTERRINFO pErrInfo)
{
    if (pThis->offTbl <= pThis->cbTbl + sizeof(uint16_t))
    { /* probable */ }
    else
        return RTErrInfoSetF(pErrInfo, VERR_EOF, "AML stream ended prematurely at offset '%#x' trying to read two bytes", pThis->offTbl);

    if (pThis->pacbPkgLeft[pThis->iLvl] < sizeof(uint16_t))
        return RTErrInfoSetF(pErrInfo, VERR_INVALID_STATE, "Data overflows current package limitation");

    pThis->pacbPkgLeft[pThis->iLvl] -= sizeof(uint16_t);

    *pu16 =  pThis->pbTbl[pThis->offTbl++];
    *pu16 |= (uint16_t)pThis->pbTbl[pThis->offTbl++] << 8;
    return VINF_SUCCESS;
}


DECLINLINE(int) rtAcpiTblAmlDecodeReadU32(PRTACPITBLAMLDECODE pThis, uint32_t *pu32, PRTERRINFO pErrInfo)
{
    if (pThis->offTbl <= pThis->cbTbl + sizeof(uint32_t))
    { /* probable */ }
    else
        return RTErrInfoSetF(pErrInfo, VERR_EOF, "AML stream ended prematurely at offset '%#x' trying to read four bytes", pThis->offTbl);

    if (pThis->pacbPkgLeft[pThis->iLvl] < sizeof(uint32_t))
        return RTErrInfoSetF(pErrInfo, VERR_INVALID_STATE, "Data overflows current package limitation");

    pThis->pacbPkgLeft[pThis->iLvl] -= sizeof(uint32_t);

    *pu32 =  pThis->pbTbl[pThis->offTbl++];
    *pu32 |= (uint32_t)pThis->pbTbl[pThis->offTbl++] <<  8;
    *pu32 |= (uint32_t)pThis->pbTbl[pThis->offTbl++] << 16;
    *pu32 |= (uint32_t)pThis->pbTbl[pThis->offTbl++] << 24;
    return VINF_SUCCESS;
}


DECLINLINE(int) rtAcpiTblAmlDecodeReadU64(PRTACPITBLAMLDECODE pThis, uint64_t *pu64, PRTERRINFO pErrInfo)
{
    if (pThis->offTbl <= pThis->cbTbl + sizeof(uint64_t))
    { /* probable */ }
    else
        return RTErrInfoSetF(pErrInfo, VERR_EOF, "AML stream ended prematurely at offset '%#x' trying to read eight bytes", pThis->offTbl);

    if (pThis->pacbPkgLeft[pThis->iLvl] < sizeof(uint64_t))
        return RTErrInfoSetF(pErrInfo, VERR_INVALID_STATE, "Data overflows current package limitation");

    pThis->pacbPkgLeft[pThis->iLvl] -= sizeof(uint64_t);

    *pu64 =  pThis->pbTbl[pThis->offTbl++];
    *pu64 |= (uint64_t)pThis->pbTbl[pThis->offTbl++] <<  8;
    *pu64 |= (uint64_t)pThis->pbTbl[pThis->offTbl++] << 16;
    *pu64 |= (uint64_t)pThis->pbTbl[pThis->offTbl++] << 24;
    *pu64 |= (uint64_t)pThis->pbTbl[pThis->offTbl++] << 32;
    *pu64 |= (uint64_t)pThis->pbTbl[pThis->offTbl++] << 40;
    *pu64 |= (uint64_t)pThis->pbTbl[pThis->offTbl++] << 48;
    *pu64 |= (uint64_t)pThis->pbTbl[pThis->offTbl++] << 54;
    return VINF_SUCCESS;
}


static int rtAcpiTblAmlDecodeNameSeg(PRTACPITBLAMLDECODE pThis, char *pszNameString, PRTERRINFO pErrInfo)
{
    uint8_t abNameSeg[4];
    for (uint8_t i = 0; i < sizeof(abNameSeg); i++)
    {
        int rc = rtAcpiTblAmlDecodeReadU8(pThis, &abNameSeg[i], pErrInfo);
        if (RT_FAILURE(rc)) return rc;
    }

    /* LeadNameChar */
    if (   abNameSeg[0] != '_'
        && !RTLocCIsUpper(abNameSeg[0]))
        return RTErrInfoSetF(pErrInfo, VERR_INVALID_PARAMETER, "AML stream contains invalid lead name character '%#02RX8'", abNameSeg[0]);

    for (uint8_t i = 1; i < sizeof(abNameSeg); i++)
    {
        if (   abNameSeg[i] != '_'
            && !RTLocCIsUpper(abNameSeg[i])
            && !RTLocCIsDigit(abNameSeg[i]))
            return RTErrInfoSetF(pErrInfo, VERR_INVALID_PARAMETER, "AML stream contains invalid name character '%#02RX8", abNameSeg[i]);
    }

    pszNameString[0] = (char)abNameSeg[0];
    pszNameString[1] = (char)abNameSeg[1];
    pszNameString[2] = (char)abNameSeg[2];
    pszNameString[3] = (char)abNameSeg[3];
    return VINF_SUCCESS;
}


static int rtAcpiTblAmlDecodeNameSegWithoutLeadChar(PRTACPITBLAMLDECODE pThis, uint8_t bLeadChar, char *pszNameString, PRTERRINFO pErrInfo)
{
    uint8_t abNameSeg[3];
    for (uint8_t i = 0; i < sizeof(abNameSeg); i++)
    {
        int rc = rtAcpiTblAmlDecodeReadU8(pThis, &abNameSeg[i], pErrInfo);
        if (RT_FAILURE(rc)) return rc;
    }

    /* LeadNameChar */
    if (   bLeadChar != '_'
        && !RTLocCIsUpper(bLeadChar))
        return RTErrInfoSetF(pErrInfo, VERR_INVALID_PARAMETER, "AML stream contains invalid lead name character '%#02RX8'", bLeadChar);

    for (uint8_t i = 1; i < sizeof(abNameSeg); i++)
    {
        if (   abNameSeg[i] != '_'
            && !RTLocCIsUpper(abNameSeg[i])
            && !RTLocCIsDigit(abNameSeg[i]))
            return RTErrInfoSetF(pErrInfo, VERR_INVALID_PARAMETER, "AML stream contains invalid name character '%#02RX8'", abNameSeg[i]);
    }

    pszNameString[0] = (char)bLeadChar;
    pszNameString[1] = (char)abNameSeg[0];
    pszNameString[2] = (char)abNameSeg[1];
    pszNameString[3] = (char)abNameSeg[2];
    return VINF_SUCCESS;
}


static int rtAcpiTblAmlDecodeNameStringWithLead(PRTACPITBLAMLDECODE pThis, uint8_t bLeadChar, char *pszNameString, size_t cchNameString, size_t *pcbNameString, PRTERRINFO pErrInfo)
{
    AssertReturn(cchNameString >= 5, VERR_INVALID_PARAMETER); /* One name segment is at least 4 bytes (+ terminator). */

    /* Check for a root path. */
    int rc = VINF_SUCCESS;
    uint8_t bTmp = bLeadChar;
    size_t idxName = 0;
    if (bTmp == '\\')
    {
        pszNameString[idxName++] = '\\';

        rc = rtAcpiTblAmlDecodeReadU8(pThis, &bTmp, pErrInfo);
        if (RT_FAILURE(rc)) return rc;
    }
    else if (bTmp == '^')
    {
        /* Prefix path, can have multiple ^ prefixes. */
        pszNameString[idxName++] = '^';

        for (;;)
        {
            rc = rtAcpiTblAmlDecodeReadU8(pThis, &bTmp, pErrInfo);
            if (RT_FAILURE(rc)) return rc;

            if (bTmp != '^')
                break;

            if (idxName == cchNameString - 1)
                return RTErrInfoSetF(pErrInfo, VERR_BUFFER_OVERFLOW, "PrefixPath in AML byte stream is too long to fit into a %zu byte buffer",
                                     cchNameString - 1);

            pszNameString[idxName++] = '^';
        }
    }

    if (bTmp == ACPI_AML_BYTE_CODE_PREFIX_DUAL_NAME)
    {
        if (idxName + 8 < cchNameString)
        {
            rc = rtAcpiTblAmlDecodeNameSeg(pThis, &pszNameString[idxName], pErrInfo);
            if (RT_FAILURE(rc)) return rc;

            rc = rtAcpiTblAmlDecodeNameSeg(pThis, &pszNameString[idxName + 4], pErrInfo);
            if (RT_FAILURE(rc)) return rc;

            idxName += 8;
            pszNameString[idxName] = '\0';
        }
        else
            rc = RTErrInfoSetF(pErrInfo, VERR_BUFFER_OVERFLOW, "DualNamePrefix string in AML byte stream is too long to fit into a %zu byte buffer",
                               cchNameString - 1);
    }
    else if (bTmp == ACPI_AML_BYTE_CODE_PREFIX_MULTI_NAME)
    {
        uint8_t cSegs = 0;
        rc = rtAcpiTblAmlDecodeReadU8(pThis, &cSegs, pErrInfo);
        if (RT_FAILURE(rc)) return rc;

        if (idxName + cSegs * 4 < cchNameString)
        {
            for (uint8_t i = 0; i < cSegs; i++)
            {
                rc = rtAcpiTblAmlDecodeNameSeg(pThis, &pszNameString[idxName + i * 4], pErrInfo);
                if (RT_FAILURE(rc)) return rc;
            }

            idxName += cSegs * 4;
            pszNameString[idxName] = '\0';
        }
        else
            rc = RTErrInfoSetF(pErrInfo, VERR_BUFFER_OVERFLOW, "MultiNamePrefix string in AML byte stream is too long to fit into a %zu byte buffer",
                               cchNameString - 1);
    }
    else if (bTmp == ACPI_AML_BYTE_CODE_PREFIX_NULL_NAME)
        pszNameString[idxName] = '\0';
    else
    {
        if (idxName + 4 < cchNameString)
        {
            rc = rtAcpiTblAmlDecodeNameSegWithoutLeadChar(pThis, bTmp, &pszNameString[idxName], pErrInfo);
            if (RT_FAILURE(rc)) return rc;

            idxName += 4;
            pszNameString[idxName] = '\0';
        }
        else
            rc = RTErrInfoSetF(pErrInfo, VERR_BUFFER_OVERFLOW, "Name string in AML byte stream is too long to fit into a %zu byte buffer",
                               cchNameString - 1);
    }

    *pcbNameString = idxName;
    return rc;
}


static int rtAcpiTblAmlDecodeNameString(PRTACPITBLAMLDECODE pThis, char *pszNameString, size_t cchNameString, size_t *pcbNameString, PRTERRINFO pErrInfo)
{
    AssertReturn(cchNameString >= 5, VERR_INVALID_PARAMETER); /* One name segment is at least 4 bytes (+ terminator). */

    uint8_t bLead = 0; /* shut up gcc */
    int rc = rtAcpiTblAmlDecodeReadU8(pThis, &bLead, pErrInfo);
    if (RT_FAILURE(rc)) return rc;

    return rtAcpiTblAmlDecodeNameStringWithLead( pThis, bLead, pszNameString, cchNameString, pcbNameString, pErrInfo);
}


static int rtAcpiTblAmlDecodePkgLength(PRTACPITBLAMLDECODE pThis, size_t *pcbPkg, size_t *pcbPkgLength, PRTERRINFO pErrInfo)
{
    uint8_t bTmp = 0; /* shut up gcc */
    int rc = rtAcpiTblAmlDecodeReadU8(pThis, &bTmp, pErrInfo);
    if (RT_FAILURE(rc))
        return rc;

    /* High 2 bits give the remaining bytes following to form the final package length. */
    uint8_t cBytesRemaining = (bTmp >> 6) & 0x3;
    *pcbPkgLength = 1 + cBytesRemaining;

    if (cBytesRemaining)
    {
        size_t cbPkg = bTmp & 0xf;
        for (uint8_t i = 0; i < cBytesRemaining; i++)
        {
            rc = rtAcpiTblAmlDecodeReadU8(pThis, &bTmp, pErrInfo);
            if (RT_FAILURE(rc))
                return rc;

            cbPkg |= (size_t)bTmp << (i * 8 + 4);
            *pcbPkg = cbPkg;
        }
    }
    else
        *pcbPkg = bTmp & 0x3f;

    return VINF_SUCCESS;
}


static int rtAcpiTblAmlDecodePkgPush(PRTACPITBLAMLDECODE pThis, size_t cbPkg, PRTLISTANCHOR pLstNodes, PRTERRINFO pErrInfo)
{
    /* Get a new stack element. */
    if (pThis->iLvl == pThis->cPkgStackMax)
    {
        uint32_t const cPkgElemsNew = pThis->cPkgStackMax + 8;
        size_t *pacbPkgLeftNew            = (size_t *)RTMemRealloc(pThis->pacbPkgLeft, cPkgElemsNew * sizeof(*pacbPkgLeftNew));
        size_t *pacbPkgNew                = (size_t *)RTMemRealloc(pThis->pacbPkg, cPkgElemsNew * sizeof(*pacbPkgNew));
        PRTLISTANCHOR *papLstScopeNodesNew = (PRTLISTANCHOR *)RTMemRealloc(pThis->papLstScopeNodes, cPkgElemsNew * sizeof(*papLstScopeNodesNew));

        if (!pacbPkgLeftNew || !pacbPkgNew || !papLstScopeNodesNew)
        {
            if (pacbPkgLeftNew)
                RTMemFree(pacbPkgLeftNew);
            if (pacbPkgNew)
                RTMemFree(pacbPkgNew);
            if (papLstScopeNodesNew)
                RTMemFree(papLstScopeNodesNew);
            return RTErrInfoSetF(pErrInfo, VERR_NO_MEMORY, "Out of memory pushing a new package onto the stack");
        }

        pThis->pacbPkgLeft      = pacbPkgLeftNew;
        pThis->pacbPkg          = pacbPkgNew;
        pThis->papLstScopeNodes = papLstScopeNodesNew;
        pThis->cPkgStackMax = cPkgElemsNew;

        /* Make sure the first elements in the stacks are initialized, even though we are never using them. */
        pThis->pacbPkgLeft[0] = 0;
        pThis->pacbPkg[0]     = 0;
    }

    uint32_t const iLvlNew = pThis->iLvl + 1;
    pThis->pacbPkgLeft[iLvlNew]      = cbPkg;
    pThis->pacbPkg[iLvlNew]          = cbPkg;
    pThis->papLstScopeNodes[iLvlNew] = pLstNodes;
    pThis->iLvl = iLvlNew;
    return VINF_SUCCESS;
}


DECLINLINE(int) rtAcpiTblAmlDecodePkgPop(PRTACPITBLAMLDECODE pThis, PRTERRINFO pErrInfo)
{
    Assert(pThis->iLvl > 0);

    while (!pThis->pacbPkgLeft[pThis->iLvl])
    {
        size_t cbPkg = pThis->pacbPkg[pThis->iLvl];
        pThis->iLvl--;

        /* Are we at the root of the table? Stop the walk in any case. */
        if (!pThis->iLvl)
            break;

        if (pThis->pacbPkgLeft[pThis->iLvl] < cbPkg)
            return RTErrInfoSetF(pErrInfo, VERR_INVALID_STATE, "AML contains invalid package length encoding");

        pThis->pacbPkgLeft[pThis->iLvl] -= cbPkg;
        Assert(pThis->iLvl > 0);
    }

    return VINF_SUCCESS;
}


DECLINLINE(void) rtAcpiTblAmlDecodePkgAddNodeToCurrentScope(PRTACPITBLAMLDECODE pThis, PRTACPIASTNODE pAstNd)
{
    RTListAppend(pThis->papLstScopeNodes[pThis->iLvl], &pAstNd->NdAst);
}


DECLINLINE(void) rtAcpiTblAmlDecodePkgAddNodeToScope(PRTLISTANCHOR pThis, PRTACPIASTNODE pAstNd)
{
    RTListAppend(pThis, &pAstNd->NdAst);
}


DECLINLINE(PRTLISTANCHOR) rtAcpiTblAmlDecodePkgGetCurrentScope(PRTACPITBLAMLDECODE pThis)
{
    return pThis->papLstScopeNodes[pThis->iLvl];
}


static int rtAcpiTblAmlDecodeIntegerFromPrefix(PRTACPITBLAMLDECODE pThis, uint8_t bPrefix, uint64_t *pu64, size_t cbDecodeMax, size_t *pcbDecoded, PRTERRINFO pErrInfo)
{
    switch (bPrefix)
    {
        case ACPI_AML_BYTE_CODE_OP_ZERO:
        case ACPI_AML_BYTE_CODE_OP_ONE:
        {
            *pu64       = bPrefix == ACPI_AML_BYTE_CODE_OP_ZERO ? 0 : 1;
            *pcbDecoded = 1;
            break;
        }
        case ACPI_AML_BYTE_CODE_PREFIX_BYTE:
        {
            if (!cbDecodeMax)
                return RTErrInfoSetF(pErrInfo, VERR_INVALID_STATE, "Not enough data left to decode byte integer in AML stream");

            uint8_t bInt = 0;
            int rc = rtAcpiTblAmlDecodeReadU8(pThis, &bInt, pErrInfo);
            if (RT_FAILURE(rc))
                return rc;

            *pu64       = bInt;
            *pcbDecoded = 2;
            break;
        }
        case ACPI_AML_BYTE_CODE_PREFIX_WORD:
        {
            if (cbDecodeMax < 2)
                return RTErrInfoSetF(pErrInfo, VERR_INVALID_STATE, "Not enough data left to decode word integer in AML stream");

            uint16_t u16 = 0;
            int rc = rtAcpiTblAmlDecodeReadU16(pThis, &u16, pErrInfo);
            if (RT_FAILURE(rc))
                return rc;

            *pu64       = u16;
            *pcbDecoded = 3;
            break;
        }
        case ACPI_AML_BYTE_CODE_PREFIX_DWORD:
        {
            if (cbDecodeMax < 4)
                return RTErrInfoSetF(pErrInfo, VERR_INVALID_STATE, "Not enough data left to decode double word integer in AML stream");

            uint32_t u32 = 0;
            int rc = rtAcpiTblAmlDecodeReadU32(pThis, &u32, pErrInfo);
            if (RT_FAILURE(rc))
                return rc;

            *pu64       = u32;
            *pcbDecoded = 5;
            break;
        }
        case ACPI_AML_BYTE_CODE_PREFIX_QWORD:
        {
            if (cbDecodeMax < 8)
                return RTErrInfoSetF(pErrInfo, VERR_INVALID_STATE, "Not enough data left to decode quad word integer in AML stream");

            uint64_t u64 = 0;
            int rc = rtAcpiTblAmlDecodeReadU64(pThis, &u64, pErrInfo);
            if (RT_FAILURE(rc))
                return rc;

            *pu64       = u64;
            *pcbDecoded = 9;
            break;
        }
        default:
            return RTErrInfoSetF(pErrInfo, VERR_INVALID_STATE, "Invalid integer prefix '%#02RX8'", bPrefix);
    }

    return VINF_SUCCESS;
}


static int rtAcpiTblAmlDecodeIntegerWorker(PRTACPITBLAMLDECODE pThis, uint64_t *pu64, size_t cbDecodeMax, size_t *pcbDecoded, PRTERRINFO pErrInfo)
{
    AssertReturn(cbDecodeMax >= 1, VERR_INVALID_PARAMETER);

    uint8_t bPrefix = 0; /* shut up gcc */
    int rc = rtAcpiTblAmlDecodeReadU8(pThis, &bPrefix, pErrInfo);
    if (RT_FAILURE(rc))
        return rc;

    cbDecodeMax--;
    return rtAcpiTblAmlDecodeIntegerFromPrefix(pThis, bPrefix, pu64, cbDecodeMax, pcbDecoded, pErrInfo);
}


static int rtAcpiTblAmlDecodeFieldFlags(PRTACPITBLAMLDECODE pThis, RTACPIFIELDACC *penmAcc, bool *pfLock, RTACPIFIELDUPDATE *penmUpdate, PRTERRINFO pErrInfo)
{
    uint8_t bFlags = 0; /* shut up gcc */
    int rc = rtAcpiTblAmlDecodeReadU8(pThis, &bFlags, pErrInfo);
    if (RT_FAILURE(rc))
        return rc;

    switch (bFlags & 0xf)
    {
        case 0: *penmAcc = kAcpiFieldAcc_Any; break;
        case 1: *penmAcc = kAcpiFieldAcc_Byte; break;
        case 2: *penmAcc = kAcpiFieldAcc_Word; break;
        case 3: *penmAcc = kAcpiFieldAcc_DWord; break;
        case 4: *penmAcc = kAcpiFieldAcc_QWord; break;
        case 5: *penmAcc = kAcpiFieldAcc_Buffer; break;
        default: *penmAcc = kAcpiFieldAcc_Invalid; break;
    }

    *pfLock = RT_BOOL(bFlags & RT_BIT(4));

    switch ((bFlags >> 5) & 0x3)
    {
        case 0: *penmUpdate = kAcpiFieldUpdate_Preserve; break;
        case 1: *penmUpdate = kAcpiFieldUpdate_WriteAsOnes; break;
        case 2: *penmUpdate = kAcpiFieldUpdate_WriteAsZeroes; break;
        default: *penmUpdate = kAcpiFieldUpdate_Invalid; break;
    }

    return VINF_SUCCESS;
}


static DECLCALLBACK(int) rtAcpiTblAmlDecodeNameObject(PRTACPITBLAMLDECODE pThis, PCRTACPIAMLOPC pAmlOpc, uint8_t bOpc, PRTACPIASTNODE *ppAstNd, PRTERRINFO pErrInfo)
{
    char szName[512];
    size_t cbName = 0;

    int rc = rtAcpiTblAmlDecodeNameStringWithLead(pThis, bOpc, &szName[0], sizeof(szName), &cbName, pErrInfo);
    if (RT_FAILURE(rc)) return rc;

    PRTACPITBLAMLOBJ pIt;
    bool fFound = false;
    RTListForEach(&pThis->LstObjs, pIt, RTACPITBLAMLOBJ, NdObjs)
    {
        if (!strcmp(pIt->szName, szName))
        {
            fFound = true;
            break;
        }
    }

    const char *pszIde = RTStrCacheEnter(pThis->hStrCache, szName);
    if (pszIde)
    {
        uint32_t const cArgs =   (   fFound
                                  && pIt->enmType == kAcpiAmlObjType_Method)
                               ? pIt->u.cMethodArgs
                               : 0;
        PRTACPIASTNODE pAstNd = rtAcpiAstNodeAlloc(pThis->pNs, pAmlOpc->enmOp, RTACPI_AST_NODE_F_DEFAULT, cArgs);
        if (pAstNd)
        {
            PRTLISTANCHOR pScope = rtAcpiTblAmlDecodePkgGetCurrentScope(pThis);

            pAstNd->pszIde = pszIde;

            if (   fFound
                && pIt->enmType == kAcpiAmlObjType_Method)
            {
                for (uint32_t iArg = 0; iArg < pIt->u.cMethodArgs; iArg++)
                {
                    pAstNd->aArgs[iArg].enmType = kAcpiAstArgType_AstNode;
                    rc = rtAcpiTblAmlDecodeTerminal(pThis, &pAstNd->aArgs[iArg].u.pAstNd, pErrInfo);
                    if (RT_FAILURE(rc))
                    {
                        rtAcpiAstNodeFree(pAstNd);
                        return rc;
                    }
                }
            }

            if (ppAstNd)
                *ppAstNd = pAstNd;
            else
                rtAcpiTblAmlDecodePkgAddNodeToScope(pScope, pAstNd);

            return VINF_SUCCESS;
        }

        return RTErrInfoSetF(pErrInfo, VERR_NO_MEMORY, "Out of memory trying to allocate AST node for name object \"%s\"", szName);
    }

    return RTErrInfoSetF(pErrInfo, VERR_NO_STR_MEMORY, "Out of string memory while adding \"%s\" to the string cache", szName);
}


static DECLCALLBACK(int) rtAcpiTblAmlDecodeString(PRTACPITBLAMLDECODE pThis, PCRTACPIAMLOPC pAmlOpc, uint8_t bOpc, PRTACPIASTNODE *ppAstNd, PRTERRINFO pErrInfo)
{
    RT_NOREF(bOpc);

    char szStr[512];
    uint32_t i = 0;
    for (;;)
    {
        uint8_t bTmp = 0; /* shut up gcc */
        int rc = rtAcpiTblAmlDecodeReadU8(pThis, &bTmp, pErrInfo);
        if (RT_FAILURE(rc))
            return rc;

        if (bTmp >= 0x1 && bTmp <= 0x7f)
        {
            if (bTmp == '\n')
            {
                szStr[i++] = '\\';
                szStr[i++] = 'n';
            }
            else
                szStr[i++] = (char)bTmp;
        }
        else if (bTmp == 0x00)
        {
            szStr[i++] = '\0';
            break;
        }
        else
            return RTErrInfoSetF(pErrInfo, VERR_INVALID_STATE, "Invalid ASCII string character %#x in string", bTmp);

        if (i == sizeof(szStr))
            return RTErrInfoSetF(pErrInfo, VERR_BUFFER_OVERFLOW, "ASCII string is out of bounds");
    }

    const char *pszStrLit = RTStrCacheEnter(pThis->hStrCache, szStr);
    if (pszStrLit)
    {
        PRTACPIASTNODE pAstNd = rtAcpiAstNodeAlloc(pThis->pNs, pAmlOpc->enmOp, RTACPI_AST_NODE_F_DEFAULT, 0 /*cArgs*/);
        if (pAstNd)
        {
            pAstNd->pszStrLit = pszStrLit;
            if (ppAstNd)
                *ppAstNd = pAstNd;
            else
                rtAcpiTblAmlDecodePkgAddNodeToCurrentScope(pThis, pAstNd);

            return VINF_SUCCESS;
        }

        return RTErrInfoSetF(pErrInfo, VERR_NO_MEMORY, "Out of memory trying to allocate AST node for string literal \"%s\"", szStr);
    }

    return RTErrInfoSetF(pErrInfo, VERR_NO_STR_MEMORY, "Out of string memory while adding \"%s\" to the string cache", szStr);
}


static DECLCALLBACK(int) rtAcpiTblAmlDecodeBuffer(PRTACPITBLAMLDECODE pThis, PCRTACPIAMLOPC pAmlOpc, uint8_t bOpc, PRTACPIASTNODE *ppAstNd, PRTERRINFO pErrInfo)
{
    RT_NOREF(bOpc);

    size_t cbPkg = 0;
    size_t cbPkgLength = 0;
    int rc = rtAcpiTblAmlDecodePkgLength(pThis, &cbPkg, &cbPkgLength, pErrInfo);
    if (RT_FAILURE(rc))
        return rc;

    cbPkg -= cbPkgLength;
    uint64_t u64 = 0;
    size_t cbInt = 0;
    rc = rtAcpiTblAmlDecodeIntegerWorker(pThis, &u64, cbPkg, &cbInt, pErrInfo);
    if (RT_FAILURE(rc))
        return rc;

    cbPkg -= cbInt;

    PRTACPIASTNODE pAstNd = rtAcpiAstNodeAlloc(pThis->pNs, pAmlOpc->enmOp, RTACPI_AST_NODE_F_NEW_SCOPE, 1 /*cArgs*/);
    if (pAstNd)
    {
        pAstNd->aArgs[0].enmType = kAcpiAstArgType_U64;
        pAstNd->aArgs[0].u.u64   = u64;

        /* Decode remaining bytes. */
        while (cbPkg--)
        {
            uint8_t bTmp = 0;
            rc = rtAcpiTblAmlDecodeReadU8(pThis, &bTmp, pErrInfo);
            if (RT_FAILURE(rc))
                return rc;

            PRTACPIASTNODE pAstNdElem = rtAcpiAstNodeAlloc(pThis->pNs, kAcpiAstNodeOp_Number, RTACPI_AST_NODE_F_DEFAULT, 1 /*cArgs*/);
            if (!pAstNdElem)
                return RTErrInfoSetF(pErrInfo, VERR_NO_MEMORY, "Out of memory trying to allocate AST node for buffer");

            pAstNdElem->u64 = bTmp;
            RTListAppend(&pAstNd->LstScopeNodes, &pAstNdElem->NdAst);
        }

        if (ppAstNd)
            *ppAstNd = pAstNd;
        else
            rtAcpiTblAmlDecodePkgAddNodeToCurrentScope(pThis, pAstNd);

        return VINF_SUCCESS;
    }

    return RTErrInfoSetF(pErrInfo, VERR_NO_MEMORY, "Out of memory trying to allocate AST node for buffer");
}


static DECLCALLBACK(int) rtAcpiTblAmlDecodeInteger(PRTACPITBLAMLDECODE pThis, PCRTACPIAMLOPC pAmlOpc, uint8_t bOpc, PRTACPIASTNODE *ppAstNd, PRTERRINFO pErrInfo)
{
    uint64_t u64 = 0;
    size_t cbDecoded = 0;
    int rc = rtAcpiTblAmlDecodeIntegerFromPrefix(pThis, bOpc, &u64, sizeof(uint64_t), &cbDecoded, pErrInfo);
    if (RT_FAILURE(rc))
        return rc;

    PRTACPIASTNODE pAstNd = rtAcpiAstNodeAlloc(pThis->pNs, pAmlOpc->enmOp, RTACPI_AST_NODE_F_DEFAULT, 0 /*cArgs*/);
    if (pAstNd)
    {
        pAstNd->u64 = u64;

        if (ppAstNd)
            *ppAstNd = pAstNd;
        else
            rtAcpiTblAmlDecodePkgAddNodeToCurrentScope(pThis, pAstNd);

        return VINF_SUCCESS;
    }

    return RTErrInfoSetF(pErrInfo, VERR_NO_MEMORY, "Out of memory trying to allocate AST node for number %#RX64", u64);
}


static DECLCALLBACK(int) rtAcpiTblAmlDecodeMethod(PRTACPITBLAMLDECODE pThis, PCRTACPIAMLOPC pAmlOpc, uint8_t bOpc, PRTACPIASTNODE *ppAstNd, PRTERRINFO pErrInfo)
{
    RT_NOREF(bOpc);

    size_t cbPkg = 0;
    size_t cbPkgLength = 0;
    int rc = rtAcpiTblAmlDecodePkgLength(pThis, &cbPkg, &cbPkgLength, pErrInfo);
    if (RT_FAILURE(rc))
        return rc;

    size_t cbPkgConsumed = cbPkgLength;
    char szName[512]; RT_ZERO(szName);
    size_t cchName = 0;
    rc = rtAcpiTblAmlDecodeNameString(pThis, &szName[0], sizeof(szName), &cchName, pErrInfo);
    if (RT_FAILURE(rc))
        return rc;

    cbPkgConsumed += cchName;

    uint8_t bMethod = 0; /* shut up gcc */
    rc = rtAcpiTblAmlDecodeReadU8(pThis, &bMethod, pErrInfo);
    if (RT_FAILURE(rc))
        return rc;

    cbPkgConsumed++;

    if (cbPkg < cbPkgConsumed)
        return RTErrInfoSetF(pErrInfo, VERR_INVALID_STATE, "Number of bytes consumed for the current package exceeds package length (%zu vs %zu)",
                             cbPkgConsumed, cbPkg);

    PRTACPITBLAMLOBJ pObj = (PRTACPITBLAMLOBJ)RTMemAllocZ(RT_UOFFSETOF_DYN(RTACPITBLAMLOBJ, szName[cchName + 1]));
    if (!pObj)
        return RTErrInfoSetF(pErrInfo, VERR_NO_MEMORY, "Failed to allocate %zu bytes for method object %s",
                             RT_UOFFSETOF_DYN(RTACPITBLAMLOBJ, szName[cchName + 1]), szName);

    pObj->enmType       = kAcpiAmlObjType_Method;
    pObj->u.cMethodArgs = bMethod & 0x7;
    memcpy(&pObj->szName[0], &szName[0], cchName);
    RTListAppend(&pThis->LstObjs, &pObj->NdObjs);

    const char *pszName = RTStrCacheEnter(pThis->hStrCache, szName);
    if (!pszName)
        return RTErrInfoSetF(pErrInfo, VERR_NO_STR_MEMORY, "Out of memory trying to allocate memory for string \"%s\"", szName);

    PRTACPIASTNODE pAstNd = rtAcpiAstNodeAlloc(pThis->pNs, pAmlOpc->enmOp, RTACPI_AST_NODE_F_NEW_SCOPE, 4 /*cArgs*/);
    if (pAstNd)
    {
        pAstNd->aArgs[0].enmType = kAcpiAstArgType_NameString;
        pAstNd->aArgs[0].u.pszNameString = pszName;

        pAstNd->aArgs[1].enmType = kAcpiAstArgType_U8;
        pAstNd->aArgs[1].u.u8 = pObj->u.cMethodArgs;

        pAstNd->aArgs[2].enmType = kAcpiAstArgType_Bool;
        pAstNd->aArgs[2].u.f = RT_BOOL(bMethod & RT_BIT(3));

        pAstNd->aArgs[3].enmType = kAcpiAstArgType_U8;
        pAstNd->aArgs[3].u.u8 = bMethod >> 4;

        if (ppAstNd)
            *ppAstNd = pAstNd;
        else
            rtAcpiTblAmlDecodePkgAddNodeToCurrentScope(pThis, pAstNd);

        return rtAcpiTblAmlDecodePkgPush(pThis, cbPkg - cbPkgConsumed, &pAstNd->LstScopeNodes, pErrInfo);
    }

    return RTErrInfoSetF(pErrInfo, VERR_NO_MEMORY, "Out of memory trying to allocate AST node for method \"%s\"", szName);
}


/**
 * Decode a FieldOp.
 *
 * @returns IPRT status code.
 * @param   pThis       The AML decoder state.
 * @param   hVfsIosOut  Where to store the ASL output
 * @param   bOp         The FieldOp opcode.
 * @param   pErrInfo    Where to return any additional error information.
 *
 * @note DefField := FieldOp PkgLength NameString FieldFlags FieldList
 */
static DECLCALLBACK(int) rtAcpiTblAmlDecodeField(PRTACPITBLAMLDECODE pThis, PCRTACPIAMLOPC pAmlOpc, uint8_t bOpc, PRTACPIASTNODE *ppAstNd, PRTERRINFO pErrInfo)
{
    Assert(bOpc == ACPI_AML_BYTE_CODE_EXT_OP_FIELD || ACPI_AML_BYTE_CODE_EXT_OP_INDEX_FIELD); RT_NOREF(bOpc);

    size_t cbPkg = 0;
    size_t cbPkgLength = 0;
    int rc = rtAcpiTblAmlDecodePkgLength(pThis, &cbPkg, &cbPkgLength, pErrInfo);
    if (RT_FAILURE(rc))
        return rc;

    PRTACPIASTNODE pAstNd = rtAcpiAstNodeAlloc(pThis->pNs, pAmlOpc->enmOp, RTACPI_AST_NODE_F_DEFAULT,
                                                  pAmlOpc->enmOp == kAcpiAstNodeOp_IndexField
                                               ? 5
                                               : 4 /*cArgs*/);
    if (!pAstNd)
        return RTErrInfoSetF(pErrInfo, VERR_NO_MEMORY, "Out of memory trying to allocate AST node for Field/IndexField");

    size_t cbPkgConsumed = cbPkgLength;
    char szName[512]; RT_ZERO(szName);
    size_t cchName = 0;
    rc = rtAcpiTblAmlDecodeNameString(pThis, &szName[0], sizeof(szName), &cchName, pErrInfo);
    if (RT_FAILURE(rc))
    {
        rtAcpiAstNodeFree(pAstNd);
        return rc;
    }

    cbPkgConsumed += cchName;

    uint8_t idxArg = 0;
    pAstNd->aArgs[idxArg].enmType         = kAcpiAstArgType_NameString;
    pAstNd->aArgs[idxArg].u.pszNameString = RTStrCacheEnter(pThis->hStrCache, szName);
    if (!pAstNd->aArgs[idxArg].u.pszNameString)
    {
        rtAcpiAstNodeFree(pAstNd);
        return RTErrInfoSetF(pErrInfo, VERR_NO_STR_MEMORY, "Out of memory trying to allocate space for string \"%s\"", szName);
    }
    idxArg++;

    /* IndexField has a second name string. */
    if (pAmlOpc->enmOp == kAcpiAstNodeOp_IndexField)
    {
        RT_ZERO(szName); cchName = 0;
        rc = rtAcpiTblAmlDecodeNameString(pThis, &szName[0], sizeof(szName), &cchName, pErrInfo);
        if (RT_FAILURE(rc))
        {
            rtAcpiAstNodeFree(pAstNd);
            return rc;
        }

        pAstNd->aArgs[idxArg].enmType         = kAcpiAstArgType_NameString;
        pAstNd->aArgs[idxArg].u.pszNameString = RTStrCacheEnter(pThis->hStrCache, szName);
        if (!pAstNd->aArgs[idxArg].u.pszNameString)
        {
            rtAcpiAstNodeFree(pAstNd);
            return RTErrInfoSetF(pErrInfo, VERR_NO_STR_MEMORY, "Out of memory trying to allocate space for string \"%s\"", szName);
        }

        cbPkgConsumed += cchName;
        idxArg++;
    }

    /* Decode the field flags. */
    RTACPIFIELDACC    enmAcc    = kAcpiFieldAcc_Invalid;
    bool              fLock     = false;
    RTACPIFIELDUPDATE enmUpdate = kAcpiFieldUpdate_Invalid;
    rc = rtAcpiTblAmlDecodeFieldFlags(pThis, &enmAcc, &fLock, &enmUpdate, pErrInfo);
    if (RT_FAILURE(rc))
        return rc;

    cbPkgConsumed++;

    if (cbPkg < cbPkgConsumed)
        return RTErrInfoSetF(pErrInfo, VERR_INVALID_STATE,
                             "Number of bytes consumed for the current package exceeds package length while decoding a FieldOp (%zu vs %zu)",
                             cbPkgConsumed, cbPkg);

    pAstNd->aArgs[idxArg].enmType            = kAcpiAstArgType_FieldAcc;
    pAstNd->aArgs[idxArg++].u.enmFieldAcc    = enmAcc;
    pAstNd->aArgs[idxArg].enmType            = kAcpiAstArgType_Bool;
    pAstNd->aArgs[idxArg++].u.f              = fLock;
    pAstNd->aArgs[idxArg].enmType            = kAcpiAstArgType_FieldUpdate;
    pAstNd->aArgs[idxArg++].u.enmFieldUpdate = enmUpdate;

    /* Decode the individual fields. */
    uint32_t          cFieldsMax = 8;
    uint32_t          cFields    = 0;
    PRTACPIFIELDENTRY paFields   = (PRTACPIFIELDENTRY)RTMemAllocZ(cFieldsMax * sizeof(*paFields));
    if (!paFields)
    {
        RTMemFree(pAstNd);
        return RTErrInfoSetF(pErrInfo, VERR_NO_MEMORY, "Out of memory trying to allocate AST node for Field \"%s\"", szName);
    }

    for (;;)
    {
        uint8_t bField = 0;
        rc = rtAcpiTblAmlDecodeReadU8(pThis, &bField, pErrInfo);
        if (RT_FAILURE(rc))
            break;

        cbPkgConsumed++;

        if (cFields == cFieldsMax)
        {
            uint32_t const cFieldsMaxNew = cFieldsMax + 8;
            PRTACPIFIELDENTRY paFieldsNew = (PRTACPIFIELDENTRY)RTMemRealloc(paFields, cFieldsMaxNew * sizeof(*paFields));
            if (!paFieldsNew)
            {
                rc = RTErrInfoSetF(pErrInfo, VERR_NO_MEMORY, "Out of memory trying to grow fields array of node for Field \"%s\"", szName);
                break;
            }

            paFields = paFieldsNew;
        }

        if (bField == 0) /* ReservedField. */
        {
            size_t cBitsField       = 0;
            size_t cbFieldPkgLength = 0;
            rc = rtAcpiTblAmlDecodePkgLength(pThis, &cBitsField, &cbFieldPkgLength, pErrInfo);
            if (RT_FAILURE(rc))
                break;

            paFields[cFields].pszName = NULL;
            paFields[cFields].cBits   = cBitsField;

            cbPkgConsumed += cbFieldPkgLength;
        }
        else if (bField == 1 || bField == 2 || bField == 3) /* Not supported right now. */
        {
            rc = RTErrInfoSetF(pErrInfo, VERR_NOT_SUPPORTED,
                               "Decoding AccessField, ExtendedAccessField and ConnectField items are not yet supported");
            break;
        }
        else /* NamedField */
        {
            /* NameSeg PkgLength */
            char achNameSeg[5]; RT_ZERO(achNameSeg);
            rc = rtAcpiTblAmlDecodeNameSegWithoutLeadChar(pThis, bField, &achNameSeg[0], pErrInfo);
            if (RT_FAILURE(rc))
                break;

            size_t cbField          = 0;
            size_t cbFieldPkgLength = 0;
            rc = rtAcpiTblAmlDecodePkgLength(pThis, &cbField, &cbFieldPkgLength, pErrInfo);
            if (RT_FAILURE(rc))
                break;

            paFields[cFields].pszName = RTStrCacheEnterN(pThis->hStrCache, &achNameSeg[0], RT_ELEMENTS(achNameSeg));
            paFields[cFields].cBits   = cbField * 8;

            if (!paFields[cFields].pszName)
            {
                rc = RTErrInfoSetF(pErrInfo, VERR_NO_STR_MEMORY, "Out of memory trying to enter \"%.4s\" into the string cache for Field \"%s\"",
                                    achNameSeg, szName);
                break;
            }

            cbPkgConsumed += 3 + cbFieldPkgLength;
        }

        cFields++;

        if (cbPkg == cbPkgConsumed) /* Done? */
            break;
        else if (cbPkg < cbPkgConsumed)
        {
            rc = RTErrInfoSetF(pErrInfo, VERR_INVALID_STATE,
                               "Number of bytes consumed for the current package exceeds package length while decoding a FieldOp (%zu vs %zu)",
                               cbPkgConsumed, cbPkg);
            break;
        }
    }

    if (RT_SUCCESS(rc))
    {
        pAstNd->Fields.paFields = paFields;
        pAstNd->Fields.cFields  = cFields;

        if (ppAstNd)
            *ppAstNd = pAstNd;
        else
            rtAcpiTblAmlDecodePkgAddNodeToCurrentScope(pThis, pAstNd);
    }
    else
    {
        RTMemFree(pAstNd);
        RTMemFree(paFields);
    }

    return rc;
}


static DECLCALLBACK(int) rtAcpiTblAmlDecodeSimple(PRTACPITBLAMLDECODE pThis, PCRTACPIAMLOPC pAmlOpc, uint8_t bOpc,
                                                  PRTACPIASTNODE *ppAstNd, PRTERRINFO pErrInfo)
{
    RT_NOREF(bOpc);

    int rc = VINF_SUCCESS;

    /* Decode any package length field first. */
    size_t cbPkg         = 0;
    size_t cbPkgLength   = 0;
    size_t cbPkgConsumed = 0;
    if (pAmlOpc->fFlags & RTACPI_AML_OPC_F_HAS_PKG_LENGTH)
    {
        rc = rtAcpiTblAmlDecodePkgLength(pThis, &cbPkg, &cbPkgLength, pErrInfo);
        if (RT_FAILURE(rc)) return rc;

        cbPkgConsumed += cbPkgLength;
    }

    uint32_t fAstNdFlags = RTACPI_AST_NODE_F_DEFAULT;
    if (pAmlOpc->fFlags & RTACPI_AML_OPC_F_NEW_SCOPE)
        fAstNdFlags |= RTACPI_AST_NODE_F_NEW_SCOPE;
    PRTACPIASTNODE pAstNd = rtAcpiAstNodeAlloc(pThis->pNs, pAmlOpc->enmOp, fAstNdFlags, pAmlOpc->cArgs);
    if (!pAstNd)
        return RTErrInfoSetF(pErrInfo, VERR_NO_MEMORY, "Out of memory trying to allocate AST node for opcode %#RX8", bOpc);

    PRTLISTANCHOR pScope = rtAcpiTblAmlDecodePkgGetCurrentScope(pThis);

    /* Process any arguments */
    for (uint32_t i = 0; i < pAmlOpc->cArgs; i++)
    {
        switch (pAmlOpc->aenmTypes[i])
        {
            case kAcpiAmlOpcType_Byte:
            {
                pAstNd->aArgs[i].enmType = kAcpiAstArgType_U8;

                rc = rtAcpiTblAmlDecodeReadU8(pThis, &pAstNd->aArgs[i].u.u8, pErrInfo);
                if (RT_FAILURE(rc)) return rc;

                cbPkgConsumed++;
                break;
            }
            case kAcpiAmlOpcType_Word:
            {
                pAstNd->aArgs[i].enmType = kAcpiAstArgType_U16;

                rc = rtAcpiTblAmlDecodeReadU16(pThis, &pAstNd->aArgs[i].u.u16, pErrInfo);
                if (RT_FAILURE(rc)) return rc;

                cbPkgConsumed += sizeof(uint16_t);
                break;
            }
            case kAcpiAmlOpcType_DWord:
            {
                pAstNd->aArgs[i].enmType = kAcpiAstArgType_U32;
                rc = rtAcpiTblAmlDecodeReadU32(pThis, &pAstNd->aArgs[i].u.u32, pErrInfo);
                if (RT_FAILURE(rc)) return rc;

                cbPkgConsumed += sizeof(uint32_t);
                break;
            }
            case kAcpiAmlOpcType_NameString:
            {
                char szName[512];
                size_t cbName = 0;
                rc = rtAcpiTblAmlDecodeNameString(pThis, &szName[0], sizeof(szName), &cbName, pErrInfo);
                if (RT_FAILURE(rc)) return rc;

                pAstNd->aArgs[i].enmType         = kAcpiAstArgType_NameString;
                pAstNd->aArgs[i].u.pszNameString = RTStrCacheEnter(pThis->hStrCache, szName);

                cbPkgConsumed += cbName;
                break;
            }
            case kAcpiAmlOpcType_TermArg:
            case kAcpiAmlOpcType_SuperName:
            {
                pAstNd->aArgs[i].enmType = kAcpiAstArgType_AstNode;

                size_t offTblOrig = pThis->offTbl;
                /** @todo SuperName has limited allowed arguments. */
                rc = rtAcpiTblAmlDecodeTerminal(pThis, &pAstNd->aArgs[i].u.pAstNd, pErrInfo);
                if (RT_FAILURE(rc)) return rc;

                cbPkgConsumed += pThis->offTbl - offTblOrig;
                break;
            }
            case kAcpiAmlOpcType_Target:
            {
                pAstNd->aArgs[i].enmType = kAcpiAstArgType_AstNode;

                bool fSkipped = false;
                rc = rtAcpiTblAmlDecodeSkipU8IfEqual(pThis, 0x00, &fSkipped, pErrInfo);
                if (RT_FAILURE(rc)) return rc;

                size_t offTblOrig = pThis->offTbl;
                /* The NullName will be identified by a NULL AST node. */
                if (!fSkipped)
                {
                    /** @todo Target has limited allowed arguments. */
                    rc = rtAcpiTblAmlDecodeTerminal(pThis, &pAstNd->aArgs[i].u.pAstNd, pErrInfo);
                    if (RT_FAILURE(rc)) return rc;
                }

                cbPkgConsumed += pThis->offTbl - offTblOrig;
                break;
            }
            case kAcpiAmlOpcType_RegionSpace:
            {
                uint8_t bVal = 0;
                rc = rtAcpiTblAmlDecodeReadU8(pThis, &bVal, pErrInfo);
                if (RT_FAILURE(rc)) return rc;

                pAstNd->aArgs[i].enmType = kAcpiAstArgType_RegionSpace;
                switch (bVal)
                {
                    case 0x00: pAstNd->aArgs[i].u.enmRegionSpace = kAcpiOperationRegionSpace_SystemMemory;     break;
                    case 0x01: pAstNd->aArgs[i].u.enmRegionSpace = kAcpiOperationRegionSpace_SystemIo;         break;
                    case 0x02: pAstNd->aArgs[i].u.enmRegionSpace = kAcpiOperationRegionSpace_PciConfig;        break;
                    case 0x03: pAstNd->aArgs[i].u.enmRegionSpace = kAcpiOperationRegionSpace_EmbeddedControl;  break;
                    case 0x04: pAstNd->aArgs[i].u.enmRegionSpace = kAcpiOperationRegionSpace_SmBus;            break;
                    case 0x05: pAstNd->aArgs[i].u.enmRegionSpace = kAcpiOperationRegionSpace_SystemCmos;       break;
                    case 0x06: pAstNd->aArgs[i].u.enmRegionSpace = kAcpiOperationRegionSpace_PciBarTarget;     break;
                    case 0x07: pAstNd->aArgs[i].u.enmRegionSpace = kAcpiOperationRegionSpace_Ipmi;             break;
                    case 0x08: pAstNd->aArgs[i].u.enmRegionSpace = kAcpiOperationRegionSpace_Gpio;             break;
                    case 0x09: pAstNd->aArgs[i].u.enmRegionSpace = kAcpiOperationRegionSpace_GenericSerialBus; break;
                    case 0x0a: pAstNd->aArgs[i].u.enmRegionSpace = kAcpiOperationRegionSpace_Pcc;              break;
                    default:
                        AssertFailedReturn(VERR_INTERNAL_ERROR);
                }

                break;
            }
            case kAcpiAmlOpcType_Invalid:
            default:
                AssertReleaseFailed();
        }
    }

    if (ppAstNd)
        *ppAstNd = pAstNd;
    else
        rtAcpiTblAmlDecodePkgAddNodeToScope(pScope, pAstNd);

    if (pAmlOpc->fFlags & RTACPI_AML_OPC_F_HAS_PKG_LENGTH)
    {
        if (cbPkg < cbPkgConsumed)
            return RTErrInfoSetF(pErrInfo, VERR_BUFFER_OVERFLOW, "Opcode arguments consumed more than the package length indicated (%zu vs %zu)", cbPkg, cbPkgConsumed);
        rc = rtAcpiTblAmlDecodePkgPush(pThis, cbPkg - cbPkgConsumed, &pAstNd->LstScopeNodes, pErrInfo);
    }

    return rc;
}


/**
 * AML Opcode -> ASL decoder array.
 */
static const RTACPIAMLOPC g_aAmlOpcodeDecode[] =
{
#define RTACPI_AML_OPC_INVALID \
    { kAcpiAstNodeOp_Invalid, RTACPI_AML_OPC_F_NONE,  0, { kAcpiAmlOpcType_Invalid, kAcpiAmlOpcType_Invalid, kAcpiAmlOpcType_Invalid, kAcpiAmlOpcType_Invalid, kAcpiAmlOpcType_Invalid }, NULL }
#define RTACPI_AML_OPC_SIMPLE_0(a_enmOp, a_fFlags) \
    { a_enmOp, a_fFlags, 0, { kAcpiAmlOpcType_Invalid, kAcpiAmlOpcType_Invalid, kAcpiAmlOpcType_Invalid, kAcpiAmlOpcType_Invalid, kAcpiAmlOpcType_Invalid }, rtAcpiTblAmlDecodeSimple }
#define RTACPI_AML_OPC_SIMPLE_1(a_enmOp, a_fFlags, a_enmType0) \
    { a_enmOp, a_fFlags, 1, { a_enmType0,              kAcpiAmlOpcType_Invalid, kAcpiAmlOpcType_Invalid, kAcpiAmlOpcType_Invalid, kAcpiAmlOpcType_Invalid }, rtAcpiTblAmlDecodeSimple }
#define RTACPI_AML_OPC_SIMPLE_2(a_enmOp, a_fFlags, a_enmType0, a_enmType1) \
    { a_enmOp, a_fFlags, 2, { a_enmType0,              a_enmType1,              kAcpiAmlOpcType_Invalid, kAcpiAmlOpcType_Invalid, kAcpiAmlOpcType_Invalid }, rtAcpiTblAmlDecodeSimple }
#define RTACPI_AML_OPC_SIMPLE_3(a_enmOp, a_fFlags, a_enmType0, a_enmType1, a_enmType2) \
    { a_enmOp, a_fFlags, 3, { a_enmType0,              a_enmType1,              a_enmType2,              kAcpiAmlOpcType_Invalid, kAcpiAmlOpcType_Invalid }, rtAcpiTblAmlDecodeSimple }
#define RTACPI_AML_OPC_SIMPLE_4(a_enmOp, a_fFlags, a_enmType0, a_enmType1, a_enmType2, a_enmType3) \
    { a_enmOp, a_fFlags, 4, { a_enmType0,              a_enmType1,              a_enmType2,              a_enmType3,              kAcpiAmlOpcType_Invalid }, rtAcpiTblAmlDecodeSimple }
#define RTACPI_AML_OPC_SIMPLE_5(a_enmOp, a_fFlags, a_enmType0, a_enmType1, a_enmType2, a_enmType3, a_enmType4) \
    { a_enmOp, a_fFlags, 5, { a_enmType0,              a_enmType1,              a_enmType2,              a_enmType3,              a_enmType4              }, rtAcpiTblAmlDecodeSimple }
#define RTACPI_AML_OPC_HANDLER(a_enmOp, a_pfnHandler) \
    { a_enmOp, RTACPI_AML_OPC_F_NONE, 0, { kAcpiAmlOpcType_Invalid, kAcpiAmlOpcType_Invalid, kAcpiAmlOpcType_Invalid, kAcpiAmlOpcType_Invalid, kAcpiAmlOpcType_Invalid }, a_pfnHandler }

    /* 0x00 */ RTACPI_AML_OPC_SIMPLE_0(kAcpiAstNodeOp_Zero,                    RTACPI_AML_OPC_F_NONE),
    /* 0x01 */ RTACPI_AML_OPC_SIMPLE_0(kAcpiAstNodeOp_One,                     RTACPI_AML_OPC_F_NONE),
    /* 0x02 */ RTACPI_AML_OPC_INVALID,
    /* 0x03 */ RTACPI_AML_OPC_INVALID,
    /* 0x04 */ RTACPI_AML_OPC_INVALID,
    /* 0x05 */ RTACPI_AML_OPC_INVALID,
    /* 0x06 */ RTACPI_AML_OPC_INVALID,
    /* 0x07 */ RTACPI_AML_OPC_INVALID,
    /* 0x08 */ RTACPI_AML_OPC_SIMPLE_2(kAcpiAstNodeOp_Name,                    RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_NameString, kAcpiAmlOpcType_TermArg),
    /* 0x09 */ RTACPI_AML_OPC_INVALID,
    /* 0x0a */ RTACPI_AML_OPC_HANDLER( kAcpiAstNodeOp_Number,                  rtAcpiTblAmlDecodeInteger),
    /* 0x0b */ RTACPI_AML_OPC_HANDLER( kAcpiAstNodeOp_Number,                  rtAcpiTblAmlDecodeInteger),
    /* 0x0c */ RTACPI_AML_OPC_HANDLER( kAcpiAstNodeOp_Number,                  rtAcpiTblAmlDecodeInteger),
    /* 0x0d */ RTACPI_AML_OPC_HANDLER( kAcpiAstNodeOp_StringLiteral,           rtAcpiTblAmlDecodeString),
    /* 0x0e */ RTACPI_AML_OPC_HANDLER( kAcpiAstNodeOp_Number,                  rtAcpiTblAmlDecodeInteger),
    /* 0x0f */ RTACPI_AML_OPC_INVALID,

    /* 0x10 */ RTACPI_AML_OPC_SIMPLE_1(kAcpiAstNodeOp_Scope,                   RTACPI_AML_OPC_F_HAS_PKG_LENGTH | RTACPI_AML_OPC_F_NEW_SCOPE,  kAcpiAmlOpcType_NameString),
    /* 0x11 */ RTACPI_AML_OPC_HANDLER( kAcpiAstNodeOp_Buffer,                  rtAcpiTblAmlDecodeBuffer),
    /* 0x12 */ RTACPI_AML_OPC_SIMPLE_1(kAcpiAstNodeOp_Package,                 RTACPI_AML_OPC_F_HAS_PKG_LENGTH | RTACPI_AML_OPC_F_NEW_SCOPE,  kAcpiAmlOpcType_Byte),
    /* 0x13 */ RTACPI_AML_OPC_INVALID,
    /* 0x14 */ RTACPI_AML_OPC_HANDLER( kAcpiAstNodeOp_Method,                  rtAcpiTblAmlDecodeMethod),
    /* 0x15 */ RTACPI_AML_OPC_SIMPLE_3(kAcpiAstNodeOp_External,                RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_NameString, kAcpiAmlOpcType_Byte, kAcpiAmlOpcType_Byte),
    /* 0x16 */ RTACPI_AML_OPC_INVALID,
    /* 0x17 */ RTACPI_AML_OPC_INVALID,
    /* 0x18 */ RTACPI_AML_OPC_INVALID,
    /* 0x19 */ RTACPI_AML_OPC_INVALID,
    /* 0x1a */ RTACPI_AML_OPC_INVALID,
    /* 0x1b */ RTACPI_AML_OPC_INVALID,
    /* 0x1c */ RTACPI_AML_OPC_INVALID,
    /* 0x1d */ RTACPI_AML_OPC_INVALID,
    /* 0x1e */ RTACPI_AML_OPC_INVALID,
    /* 0x1f */ RTACPI_AML_OPC_INVALID,

    /* 0x20 */ RTACPI_AML_OPC_INVALID,
    /* 0x21 */ RTACPI_AML_OPC_INVALID,
    /* 0x22 */ RTACPI_AML_OPC_INVALID,
    /* 0x23 */ RTACPI_AML_OPC_INVALID,
    /* 0x24 */ RTACPI_AML_OPC_INVALID,
    /* 0x25 */ RTACPI_AML_OPC_INVALID,
    /* 0x26 */ RTACPI_AML_OPC_INVALID,
    /* 0x27 */ RTACPI_AML_OPC_INVALID,
    /* 0x28 */ RTACPI_AML_OPC_INVALID,
    /* 0x29 */ RTACPI_AML_OPC_INVALID,
    /* 0x2a */ RTACPI_AML_OPC_INVALID,
    /* 0x2b */ RTACPI_AML_OPC_INVALID,
    /* 0x2c */ RTACPI_AML_OPC_INVALID,
    /* 0x2d */ RTACPI_AML_OPC_INVALID,
    /* 0x2e */ RTACPI_AML_OPC_INVALID,
    /* 0x2f */ RTACPI_AML_OPC_INVALID,

    /* 0x30 */ RTACPI_AML_OPC_INVALID,
    /* 0x31 */ RTACPI_AML_OPC_INVALID,
    /* 0x32 */ RTACPI_AML_OPC_INVALID,
    /* 0x33 */ RTACPI_AML_OPC_INVALID,
    /* 0x34 */ RTACPI_AML_OPC_INVALID,
    /* 0x35 */ RTACPI_AML_OPC_INVALID,
    /* 0x36 */ RTACPI_AML_OPC_INVALID,
    /* 0x37 */ RTACPI_AML_OPC_INVALID,
    /* 0x38 */ RTACPI_AML_OPC_INVALID,
    /* 0x39 */ RTACPI_AML_OPC_INVALID,
    /* 0x3a */ RTACPI_AML_OPC_INVALID,
    /* 0x3b */ RTACPI_AML_OPC_INVALID,
    /* 0x3c */ RTACPI_AML_OPC_INVALID,
    /* 0x3d */ RTACPI_AML_OPC_INVALID,
    /* 0x3e */ RTACPI_AML_OPC_INVALID,
    /* 0x3f */ RTACPI_AML_OPC_INVALID,

    /* 0x40 */ RTACPI_AML_OPC_INVALID,
    /* 0x41 */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x42 */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x43 */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x44 */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x45 */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x46 */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x47 */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x48 */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x49 */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x4a */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x4b */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x4c */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x4d */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x4e */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x4f */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),

    /* 0x50 */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x51 */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x52 */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x53 */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x54 */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x55 */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x56 */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x57 */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x58 */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x59 */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x5a */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x5b */ RTACPI_AML_OPC_INVALID,
    /* 0x5c */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x5d */ RTACPI_AML_OPC_INVALID,
    /* 0x5e */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),
    /* 0x5f */ RTACPI_AML_OPC_HANDLER(kAcpiAstNodeOp_Identifier,               rtAcpiTblAmlDecodeNameObject),

    /* 0x60 */ RTACPI_AML_OPC_SIMPLE_0(kAcpiAstNodeOp_Local0,                   RTACPI_AML_OPC_F_NONE),
    /* 0x61 */ RTACPI_AML_OPC_SIMPLE_0(kAcpiAstNodeOp_Local1,                   RTACPI_AML_OPC_F_NONE),
    /* 0x62 */ RTACPI_AML_OPC_SIMPLE_0(kAcpiAstNodeOp_Local2,                   RTACPI_AML_OPC_F_NONE),
    /* 0x63 */ RTACPI_AML_OPC_SIMPLE_0(kAcpiAstNodeOp_Local3,                   RTACPI_AML_OPC_F_NONE),
    /* 0x64 */ RTACPI_AML_OPC_SIMPLE_0(kAcpiAstNodeOp_Local4,                   RTACPI_AML_OPC_F_NONE),
    /* 0x65 */ RTACPI_AML_OPC_SIMPLE_0(kAcpiAstNodeOp_Local5,                   RTACPI_AML_OPC_F_NONE),
    /* 0x66 */ RTACPI_AML_OPC_SIMPLE_0(kAcpiAstNodeOp_Local6,                   RTACPI_AML_OPC_F_NONE),
    /* 0x67 */ RTACPI_AML_OPC_SIMPLE_0(kAcpiAstNodeOp_Local7,                   RTACPI_AML_OPC_F_NONE),
    /* 0x68 */ RTACPI_AML_OPC_SIMPLE_0(kAcpiAstNodeOp_Arg0,                     RTACPI_AML_OPC_F_NONE),
    /* 0x69 */ RTACPI_AML_OPC_SIMPLE_0(kAcpiAstNodeOp_Arg1,                     RTACPI_AML_OPC_F_NONE),
    /* 0x6a */ RTACPI_AML_OPC_SIMPLE_0(kAcpiAstNodeOp_Arg2,                     RTACPI_AML_OPC_F_NONE),
    /* 0x6b */ RTACPI_AML_OPC_SIMPLE_0(kAcpiAstNodeOp_Arg3,                     RTACPI_AML_OPC_F_NONE),
    /* 0x6c */ RTACPI_AML_OPC_SIMPLE_0(kAcpiAstNodeOp_Arg4,                     RTACPI_AML_OPC_F_NONE),
    /* 0x6d */ RTACPI_AML_OPC_SIMPLE_0(kAcpiAstNodeOp_Arg5,                     RTACPI_AML_OPC_F_NONE),
    /* 0x6e */ RTACPI_AML_OPC_SIMPLE_0(kAcpiAstNodeOp_Arg6,                     RTACPI_AML_OPC_F_NONE),
    /* 0x6f */ RTACPI_AML_OPC_INVALID,

    /* 0x70 */ RTACPI_AML_OPC_SIMPLE_2(kAcpiAstNodeOp_Store,                    RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_SuperName),
    /* 0x71 */ RTACPI_AML_OPC_INVALID,
    /* 0x72 */ RTACPI_AML_OPC_SIMPLE_3(kAcpiAstNodeOp_Add,                      RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_Target),
    /* 0x73 */ RTACPI_AML_OPC_INVALID,
    /* 0x74 */ RTACPI_AML_OPC_SIMPLE_3(kAcpiAstNodeOp_Subtract,                 RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_Target),
    /* 0x75 */ RTACPI_AML_OPC_SIMPLE_1(kAcpiAstNodeOp_Increment,                RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_SuperName),
    /* 0x76 */ RTACPI_AML_OPC_SIMPLE_1(kAcpiAstNodeOp_Decrement,                RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_SuperName),
    /* 0x77 */ RTACPI_AML_OPC_SIMPLE_3(kAcpiAstNodeOp_Multiply,                 RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_Target),
    /* 0x78 */ RTACPI_AML_OPC_INVALID,
    /* 0x79 */ RTACPI_AML_OPC_SIMPLE_3(kAcpiAstNodeOp_ShiftLeft,                RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_Target),
    /* 0x7a */ RTACPI_AML_OPC_SIMPLE_3(kAcpiAstNodeOp_ShiftRight,               RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_Target),
    /* 0x7b */ RTACPI_AML_OPC_SIMPLE_3(kAcpiAstNodeOp_And,                      RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_Target),
    /* 0x7c */ RTACPI_AML_OPC_SIMPLE_3(kAcpiAstNodeOp_Nand,                     RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_Target),
    /* 0x7d */ RTACPI_AML_OPC_SIMPLE_3(kAcpiAstNodeOp_Or,                       RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_Target),
    /* 0x7e */ RTACPI_AML_OPC_INVALID,
    /* 0x7f */ RTACPI_AML_OPC_SIMPLE_3(kAcpiAstNodeOp_Xor,                      RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_Target),

    /* 0x80 */ RTACPI_AML_OPC_SIMPLE_2(kAcpiAstNodeOp_Not,                      RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_Target),
    /* 0x81 */ RTACPI_AML_OPC_SIMPLE_2(kAcpiAstNodeOp_FindSetLeftBit,           RTACPI_AML_OPC_F_NONE,  kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_Target),
    /* 0x82 */ RTACPI_AML_OPC_SIMPLE_2(kAcpiAstNodeOp_FindSetRightBit,          RTACPI_AML_OPC_F_NONE,  kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_Target),
    /* 0x83 */ RTACPI_AML_OPC_SIMPLE_1(kAcpiAstNodeOp_DerefOf,                  RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg),
    /* 0x84 */ RTACPI_AML_OPC_SIMPLE_3(kAcpiAstNodeOp_ConcatenateResTemplate,   RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_Target),
    /* 0x85 */ RTACPI_AML_OPC_INVALID,
    /* 0x86 */ RTACPI_AML_OPC_SIMPLE_2(kAcpiAstNodeOp_Notify,                   RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_SuperName, kAcpiAmlOpcType_TermArg),
    /* 0x87 */ RTACPI_AML_OPC_SIMPLE_1(kAcpiAstNodeOp_SizeOf,                   RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_SuperName),
    /* 0x88 */ RTACPI_AML_OPC_SIMPLE_3(kAcpiAstNodeOp_Index,                    RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg,   kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_SuperName),
    /* 0x89 */ RTACPI_AML_OPC_INVALID,
    /* 0x8a */ RTACPI_AML_OPC_SIMPLE_3(kAcpiAstNodeOp_CreateDWordField,         RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg,   kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_NameString),
    /* 0x8b */ RTACPI_AML_OPC_SIMPLE_3(kAcpiAstNodeOp_CreateWordField,          RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg,   kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_NameString),
    /* 0x8c */ RTACPI_AML_OPC_SIMPLE_3(kAcpiAstNodeOp_CreateByteField,          RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg,   kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_NameString),
    /* 0x8d */ RTACPI_AML_OPC_SIMPLE_3(kAcpiAstNodeOp_CreateBitField,           RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg,   kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_NameString),
    /* 0x8e */ RTACPI_AML_OPC_INVALID,
    /* 0x8f */ RTACPI_AML_OPC_SIMPLE_3(kAcpiAstNodeOp_CreateQWordField,         RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg,   kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_NameString),

    /* 0x90 */ RTACPI_AML_OPC_SIMPLE_2(kAcpiAstNodeOp_LAnd,                     RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_TermArg),
    /* 0x91 */ RTACPI_AML_OPC_SIMPLE_2(kAcpiAstNodeOp_LOr,                      RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_TermArg),
    /* 0x92 */ RTACPI_AML_OPC_SIMPLE_1(kAcpiAstNodeOp_LNot,                     RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg),
    /* 0x93 */ RTACPI_AML_OPC_SIMPLE_2(kAcpiAstNodeOp_LEqual,                   RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_TermArg),
    /* 0x94 */ RTACPI_AML_OPC_SIMPLE_2(kAcpiAstNodeOp_LGreater,                 RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_TermArg),
    /* 0x95 */ RTACPI_AML_OPC_SIMPLE_2(kAcpiAstNodeOp_LLess,                    RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_TermArg),
    /* 0x96 */ RTACPI_AML_OPC_INVALID,
    /* 0x97 */ RTACPI_AML_OPC_INVALID,
    /* 0x98 */ RTACPI_AML_OPC_INVALID,
    /* 0x99 */ RTACPI_AML_OPC_INVALID,
    /* 0x9a */ RTACPI_AML_OPC_INVALID,
    /* 0x9b */ RTACPI_AML_OPC_INVALID,
    /* 0x9c */ RTACPI_AML_OPC_INVALID,
    /* 0x9d */ RTACPI_AML_OPC_INVALID,
    /* 0x9e */ RTACPI_AML_OPC_INVALID,
    /* 0x9f */ RTACPI_AML_OPC_SIMPLE_0(kAcpiAstNodeOp_Continue,                 RTACPI_AML_OPC_F_NONE),

    /* 0xa0 */ RTACPI_AML_OPC_SIMPLE_1(kAcpiAstNodeOp_If,                       RTACPI_AML_OPC_F_HAS_PKG_LENGTH | RTACPI_AML_OPC_F_NEW_SCOPE,     kAcpiAmlOpcType_TermArg),
    /* 0xa1 */ RTACPI_AML_OPC_SIMPLE_0(kAcpiAstNodeOp_Else,                     RTACPI_AML_OPC_F_HAS_PKG_LENGTH | RTACPI_AML_OPC_F_NEW_SCOPE),
    /* 0xa2 */ RTACPI_AML_OPC_SIMPLE_1(kAcpiAstNodeOp_While,                    RTACPI_AML_OPC_F_HAS_PKG_LENGTH | RTACPI_AML_OPC_F_NEW_SCOPE,     kAcpiAmlOpcType_TermArg),
    /* 0xa3 */ RTACPI_AML_OPC_INVALID,
    /* 0xa4 */ RTACPI_AML_OPC_SIMPLE_1(kAcpiAstNodeOp_Return,                   RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg),
    /* 0xa5 */ RTACPI_AML_OPC_SIMPLE_0(kAcpiAstNodeOp_Break,                    RTACPI_AML_OPC_F_NONE),
    /* 0xa6 */ RTACPI_AML_OPC_INVALID,
    /* 0xa7 */ RTACPI_AML_OPC_INVALID,
    /* 0xa8 */ RTACPI_AML_OPC_INVALID,
    /* 0xa9 */ RTACPI_AML_OPC_INVALID,
    /* 0xaa */ RTACPI_AML_OPC_INVALID,
    /* 0xab */ RTACPI_AML_OPC_INVALID,
    /* 0xac */ RTACPI_AML_OPC_INVALID,
    /* 0xad */ RTACPI_AML_OPC_INVALID,
    /* 0xae */ RTACPI_AML_OPC_INVALID,
    /* 0xaf */ RTACPI_AML_OPC_INVALID,

    /* 0xb0 */ RTACPI_AML_OPC_INVALID,
    /* 0xb1 */ RTACPI_AML_OPC_INVALID,
    /* 0xb2 */ RTACPI_AML_OPC_INVALID,
    /* 0xb3 */ RTACPI_AML_OPC_INVALID,
    /* 0xb4 */ RTACPI_AML_OPC_INVALID,
    /* 0xb5 */ RTACPI_AML_OPC_INVALID,
    /* 0xb6 */ RTACPI_AML_OPC_INVALID,
    /* 0xb7 */ RTACPI_AML_OPC_INVALID,
    /* 0xb8 */ RTACPI_AML_OPC_INVALID,
    /* 0xb9 */ RTACPI_AML_OPC_INVALID,
    /* 0xba */ RTACPI_AML_OPC_INVALID,
    /* 0xbb */ RTACPI_AML_OPC_INVALID,
    /* 0xbc */ RTACPI_AML_OPC_INVALID,
    /* 0xbd */ RTACPI_AML_OPC_INVALID,
    /* 0xbe */ RTACPI_AML_OPC_INVALID,
    /* 0xbf */ RTACPI_AML_OPC_INVALID,

    /* 0xc0 */ RTACPI_AML_OPC_INVALID,
    /* 0xc1 */ RTACPI_AML_OPC_INVALID,
    /* 0xc2 */ RTACPI_AML_OPC_INVALID,
    /* 0xc3 */ RTACPI_AML_OPC_INVALID,
    /* 0xc4 */ RTACPI_AML_OPC_INVALID,
    /* 0xc5 */ RTACPI_AML_OPC_INVALID,
    /* 0xc6 */ RTACPI_AML_OPC_INVALID,
    /* 0xc7 */ RTACPI_AML_OPC_INVALID,
    /* 0xc8 */ RTACPI_AML_OPC_INVALID,
    /* 0xc9 */ RTACPI_AML_OPC_INVALID,
    /* 0xca */ RTACPI_AML_OPC_INVALID,
    /* 0xcb */ RTACPI_AML_OPC_INVALID,
    /* 0xcc */ RTACPI_AML_OPC_INVALID,
    /* 0xcd */ RTACPI_AML_OPC_INVALID,
    /* 0xce */ RTACPI_AML_OPC_INVALID,
    /* 0xcf */ RTACPI_AML_OPC_INVALID,

    /* 0xd0 */ RTACPI_AML_OPC_INVALID,
    /* 0xd1 */ RTACPI_AML_OPC_INVALID,
    /* 0xd2 */ RTACPI_AML_OPC_INVALID,
    /* 0xd3 */ RTACPI_AML_OPC_INVALID,
    /* 0xd4 */ RTACPI_AML_OPC_INVALID,
    /* 0xd5 */ RTACPI_AML_OPC_INVALID,
    /* 0xd6 */ RTACPI_AML_OPC_INVALID,
    /* 0xd7 */ RTACPI_AML_OPC_INVALID,
    /* 0xd8 */ RTACPI_AML_OPC_INVALID,
    /* 0xd9 */ RTACPI_AML_OPC_INVALID,
    /* 0xda */ RTACPI_AML_OPC_INVALID,
    /* 0xdb */ RTACPI_AML_OPC_INVALID,
    /* 0xdc */ RTACPI_AML_OPC_INVALID,
    /* 0xdd */ RTACPI_AML_OPC_INVALID,
    /* 0xde */ RTACPI_AML_OPC_INVALID,
    /* 0xdf */ RTACPI_AML_OPC_INVALID,

    /* 0xe0 */ RTACPI_AML_OPC_INVALID,
    /* 0xe1 */ RTACPI_AML_OPC_INVALID,
    /* 0xe2 */ RTACPI_AML_OPC_INVALID,
    /* 0xe3 */ RTACPI_AML_OPC_INVALID,
    /* 0xe4 */ RTACPI_AML_OPC_INVALID,
    /* 0xe5 */ RTACPI_AML_OPC_INVALID,
    /* 0xe6 */ RTACPI_AML_OPC_INVALID,
    /* 0xe7 */ RTACPI_AML_OPC_INVALID,
    /* 0xe8 */ RTACPI_AML_OPC_INVALID,
    /* 0xe9 */ RTACPI_AML_OPC_INVALID,
    /* 0xea */ RTACPI_AML_OPC_INVALID,
    /* 0xeb */ RTACPI_AML_OPC_INVALID,
    /* 0xec */ RTACPI_AML_OPC_INVALID,
    /* 0xed */ RTACPI_AML_OPC_INVALID,
    /* 0xee */ RTACPI_AML_OPC_INVALID,
    /* 0xef */ RTACPI_AML_OPC_INVALID,

    /* 0xf0 */ RTACPI_AML_OPC_INVALID,
    /* 0xf1 */ RTACPI_AML_OPC_INVALID,
    /* 0xf2 */ RTACPI_AML_OPC_INVALID,
    /* 0xf3 */ RTACPI_AML_OPC_INVALID,
    /* 0xf4 */ RTACPI_AML_OPC_INVALID,
    /* 0xf5 */ RTACPI_AML_OPC_INVALID,
    /* 0xf6 */ RTACPI_AML_OPC_INVALID,
    /* 0xf7 */ RTACPI_AML_OPC_INVALID,
    /* 0xf8 */ RTACPI_AML_OPC_INVALID,
    /* 0xf9 */ RTACPI_AML_OPC_INVALID,
    /* 0xfa */ RTACPI_AML_OPC_INVALID,
    /* 0xfb */ RTACPI_AML_OPC_INVALID,
    /* 0xfc */ RTACPI_AML_OPC_INVALID,
    /* 0xfd */ RTACPI_AML_OPC_INVALID,
    /* 0xfe */ RTACPI_AML_OPC_INVALID,
    /* 0xff */ RTACPI_AML_OPC_INVALID
};


/**
 * AML extended opcode -> ASL decoder array.
 */
static const RTACPIAMLOPC g_aAmlExtOpcodeDecode[] =
{
    /* 0x00 */ RTACPI_AML_OPC_INVALID,
    /* 0x01 */ RTACPI_AML_OPC_INVALID,
    /* 0x02 */ RTACPI_AML_OPC_INVALID,
    /* 0x03 */ RTACPI_AML_OPC_INVALID,
    /* 0x04 */ RTACPI_AML_OPC_INVALID,
    /* 0x05 */ RTACPI_AML_OPC_INVALID,
    /* 0x06 */ RTACPI_AML_OPC_INVALID,
    /* 0x07 */ RTACPI_AML_OPC_INVALID,
    /* 0x08 */ RTACPI_AML_OPC_INVALID,
    /* 0x09 */ RTACPI_AML_OPC_INVALID,
    /* 0x0a */ RTACPI_AML_OPC_INVALID,
    /* 0x0b */ RTACPI_AML_OPC_INVALID,
    /* 0x0c */ RTACPI_AML_OPC_INVALID,
    /* 0x0d */ RTACPI_AML_OPC_INVALID,
    /* 0x0e */ RTACPI_AML_OPC_INVALID,
    /* 0x0f */ RTACPI_AML_OPC_INVALID,

    /* 0x10 */ RTACPI_AML_OPC_INVALID,
    /* 0x11 */ RTACPI_AML_OPC_INVALID,
    /* 0x12 */ RTACPI_AML_OPC_INVALID,
    /* 0x13 */ RTACPI_AML_OPC_SIMPLE_4(kAcpiAstNodeOp_CreateField, RTACPI_AML_OPC_F_NONE,     kAcpiAmlOpcType_TermArg,   kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_NameString),
    /* 0x14 */ RTACPI_AML_OPC_INVALID,
    /* 0x15 */ RTACPI_AML_OPC_INVALID,
    /* 0x16 */ RTACPI_AML_OPC_INVALID,
    /* 0x17 */ RTACPI_AML_OPC_INVALID,
    /* 0x18 */ RTACPI_AML_OPC_INVALID,
    /* 0x19 */ RTACPI_AML_OPC_INVALID,
    /* 0x1a */ RTACPI_AML_OPC_INVALID,
    /* 0x1b */ RTACPI_AML_OPC_INVALID,
    /* 0x1c */ RTACPI_AML_OPC_INVALID,
    /* 0x1d */ RTACPI_AML_OPC_INVALID,
    /* 0x1e */ RTACPI_AML_OPC_INVALID,
    /* 0x1f */ RTACPI_AML_OPC_INVALID,

    /* 0x20 */ RTACPI_AML_OPC_INVALID,
    /* 0x21 */ RTACPI_AML_OPC_INVALID,
    /* 0x22 */ RTACPI_AML_OPC_INVALID,
    /* 0x23 */ RTACPI_AML_OPC_INVALID,
    /* 0x24 */ RTACPI_AML_OPC_INVALID,
    /* 0x25 */ RTACPI_AML_OPC_INVALID,
    /* 0x26 */ RTACPI_AML_OPC_INVALID,
    /* 0x27 */ RTACPI_AML_OPC_INVALID,
    /* 0x28 */ RTACPI_AML_OPC_INVALID,
    /* 0x29 */ RTACPI_AML_OPC_INVALID,
    /* 0x2a */ RTACPI_AML_OPC_INVALID,
    /* 0x2b */ RTACPI_AML_OPC_INVALID,
    /* 0x2c */ RTACPI_AML_OPC_INVALID,
    /* 0x2d */ RTACPI_AML_OPC_INVALID,
    /* 0x2e */ RTACPI_AML_OPC_INVALID,
    /* 0x2f */ RTACPI_AML_OPC_INVALID,

    /* 0x30 */ RTACPI_AML_OPC_INVALID,
    /* 0x31 */ RTACPI_AML_OPC_SIMPLE_0(kAcpiAstNodeOp_Invalid, RTACPI_AML_OPC_F_NONE),
    /* 0x32 */ RTACPI_AML_OPC_INVALID,
    /* 0x33 */ RTACPI_AML_OPC_INVALID,
    /* 0x34 */ RTACPI_AML_OPC_INVALID,
    /* 0x35 */ RTACPI_AML_OPC_INVALID,
    /* 0x36 */ RTACPI_AML_OPC_INVALID,
    /* 0x37 */ RTACPI_AML_OPC_INVALID,
    /* 0x38 */ RTACPI_AML_OPC_INVALID,
    /* 0x39 */ RTACPI_AML_OPC_INVALID,
    /* 0x3a */ RTACPI_AML_OPC_INVALID,
    /* 0x3b */ RTACPI_AML_OPC_INVALID,
    /* 0x3c */ RTACPI_AML_OPC_INVALID,
    /* 0x3d */ RTACPI_AML_OPC_INVALID,
    /* 0x3e */ RTACPI_AML_OPC_INVALID,
    /* 0x3f */ RTACPI_AML_OPC_INVALID,

    /* 0x40 */ RTACPI_AML_OPC_INVALID,
    /* 0x41 */ RTACPI_AML_OPC_INVALID,
    /* 0x42 */ RTACPI_AML_OPC_INVALID,
    /* 0x43 */ RTACPI_AML_OPC_INVALID,
    /* 0x44 */ RTACPI_AML_OPC_INVALID,
    /* 0x45 */ RTACPI_AML_OPC_INVALID,
    /* 0x46 */ RTACPI_AML_OPC_INVALID,
    /* 0x47 */ RTACPI_AML_OPC_INVALID,
    /* 0x48 */ RTACPI_AML_OPC_INVALID,
    /* 0x49 */ RTACPI_AML_OPC_INVALID,
    /* 0x4a */ RTACPI_AML_OPC_INVALID,
    /* 0x4b */ RTACPI_AML_OPC_INVALID,
    /* 0x4c */ RTACPI_AML_OPC_INVALID,
    /* 0x4d */ RTACPI_AML_OPC_INVALID,
    /* 0x4e */ RTACPI_AML_OPC_INVALID,
    /* 0x4f */ RTACPI_AML_OPC_INVALID,

    /* 0x50 */ RTACPI_AML_OPC_INVALID,
    /* 0x51 */ RTACPI_AML_OPC_INVALID,
    /* 0x52 */ RTACPI_AML_OPC_INVALID,
    /* 0x53 */ RTACPI_AML_OPC_INVALID,
    /* 0x54 */ RTACPI_AML_OPC_INVALID,
    /* 0x55 */ RTACPI_AML_OPC_INVALID,
    /* 0x56 */ RTACPI_AML_OPC_INVALID,
    /* 0x57 */ RTACPI_AML_OPC_INVALID,
    /* 0x58 */ RTACPI_AML_OPC_INVALID,
    /* 0x59 */ RTACPI_AML_OPC_INVALID,
    /* 0x5a */ RTACPI_AML_OPC_INVALID,
    /* 0x5b */ RTACPI_AML_OPC_INVALID,
    /* 0x5c */ RTACPI_AML_OPC_INVALID,
    /* 0x5d */ RTACPI_AML_OPC_INVALID,
    /* 0x5e */ RTACPI_AML_OPC_INVALID,
    /* 0x5f */ RTACPI_AML_OPC_INVALID,

    /* 0x60 */ RTACPI_AML_OPC_INVALID,
    /* 0x61 */ RTACPI_AML_OPC_INVALID,
    /* 0x62 */ RTACPI_AML_OPC_INVALID,
    /* 0x63 */ RTACPI_AML_OPC_INVALID,
    /* 0x64 */ RTACPI_AML_OPC_INVALID,
    /* 0x65 */ RTACPI_AML_OPC_INVALID,
    /* 0x66 */ RTACPI_AML_OPC_INVALID,
    /* 0x67 */ RTACPI_AML_OPC_INVALID,
    /* 0x68 */ RTACPI_AML_OPC_INVALID,
    /* 0x69 */ RTACPI_AML_OPC_INVALID,
    /* 0x6a */ RTACPI_AML_OPC_INVALID,
    /* 0x6b */ RTACPI_AML_OPC_INVALID,
    /* 0x6c */ RTACPI_AML_OPC_INVALID,
    /* 0x6d */ RTACPI_AML_OPC_INVALID,
    /* 0x6e */ RTACPI_AML_OPC_INVALID,
    /* 0x6f */ RTACPI_AML_OPC_INVALID,

    /* 0x70 */ RTACPI_AML_OPC_INVALID,
    /* 0x71 */ RTACPI_AML_OPC_INVALID,
    /* 0x72 */ RTACPI_AML_OPC_INVALID,
    /* 0x73 */ RTACPI_AML_OPC_INVALID,
    /* 0x74 */ RTACPI_AML_OPC_INVALID,
    /* 0x75 */ RTACPI_AML_OPC_INVALID,
    /* 0x76 */ RTACPI_AML_OPC_INVALID,
    /* 0x77 */ RTACPI_AML_OPC_INVALID,
    /* 0x78 */ RTACPI_AML_OPC_INVALID,
    /* 0x79 */ RTACPI_AML_OPC_INVALID,
    /* 0x7a */ RTACPI_AML_OPC_INVALID,
    /* 0x7b */ RTACPI_AML_OPC_INVALID,
    /* 0x7c */ RTACPI_AML_OPC_INVALID,
    /* 0x7d */ RTACPI_AML_OPC_INVALID,
    /* 0x7e */ RTACPI_AML_OPC_INVALID,
    /* 0x7f */ RTACPI_AML_OPC_INVALID,

    /* 0x80 */ RTACPI_AML_OPC_SIMPLE_4(kAcpiAstNodeOp_OperationRegion, RTACPI_AML_OPC_F_NONE, kAcpiAmlOpcType_NameString, kAcpiAmlOpcType_RegionSpace, kAcpiAmlOpcType_TermArg, kAcpiAmlOpcType_TermArg),
    /* 0x81 */ RTACPI_AML_OPC_HANDLER( kAcpiAstNodeOp_Field,           rtAcpiTblAmlDecodeField),
    /* 0x82 */ RTACPI_AML_OPC_SIMPLE_1(kAcpiAstNodeOp_Device,          RTACPI_AML_OPC_F_HAS_PKG_LENGTH | RTACPI_AML_OPC_F_NEW_SCOPE, kAcpiAmlOpcType_NameString),
    /* 0x83 */ RTACPI_AML_OPC_SIMPLE_4(kAcpiAstNodeOp_Processor,       RTACPI_AML_OPC_F_HAS_PKG_LENGTH | RTACPI_AML_OPC_F_NEW_SCOPE, kAcpiAmlOpcType_NameString, kAcpiAmlOpcType_Byte, kAcpiAmlOpcType_DWord,   kAcpiAmlOpcType_Byte),
    /* 0x84 */ RTACPI_AML_OPC_INVALID,
    /* 0x85 */ RTACPI_AML_OPC_INVALID,
    /* 0x86 */ RTACPI_AML_OPC_HANDLER( kAcpiAstNodeOp_IndexField,      rtAcpiTblAmlDecodeField),
    /* 0x87 */ RTACPI_AML_OPC_INVALID,
    /* 0x88 */ RTACPI_AML_OPC_INVALID,
    /* 0x89 */ RTACPI_AML_OPC_INVALID,
    /* 0x8a */ RTACPI_AML_OPC_INVALID,
    /* 0x8b */ RTACPI_AML_OPC_INVALID,
    /* 0x8c */ RTACPI_AML_OPC_INVALID,
    /* 0x8d */ RTACPI_AML_OPC_INVALID,
    /* 0x8e */ RTACPI_AML_OPC_INVALID,
    /* 0x8f */ RTACPI_AML_OPC_INVALID,

    /* 0x90 */ RTACPI_AML_OPC_INVALID,
    /* 0x91 */ RTACPI_AML_OPC_INVALID,
    /* 0x92 */ RTACPI_AML_OPC_INVALID,
    /* 0x93 */ RTACPI_AML_OPC_INVALID,
    /* 0x94 */ RTACPI_AML_OPC_INVALID,
    /* 0x95 */ RTACPI_AML_OPC_INVALID,
    /* 0x96 */ RTACPI_AML_OPC_INVALID,
    /* 0x97 */ RTACPI_AML_OPC_INVALID,
    /* 0x98 */ RTACPI_AML_OPC_INVALID,
    /* 0x99 */ RTACPI_AML_OPC_INVALID,
    /* 0x9a */ RTACPI_AML_OPC_INVALID,
    /* 0x9b */ RTACPI_AML_OPC_INVALID,
    /* 0x9c */ RTACPI_AML_OPC_INVALID,
    /* 0x9d */ RTACPI_AML_OPC_INVALID,
    /* 0x9e */ RTACPI_AML_OPC_INVALID,
    /* 0x9f */ RTACPI_AML_OPC_INVALID,

    /* 0xa0 */ RTACPI_AML_OPC_INVALID,
    /* 0xa1 */ RTACPI_AML_OPC_INVALID,
    /* 0xa2 */ RTACPI_AML_OPC_INVALID,
    /* 0xa3 */ RTACPI_AML_OPC_INVALID,
    /* 0xa4 */ RTACPI_AML_OPC_INVALID,
    /* 0xa5 */ RTACPI_AML_OPC_INVALID,
    /* 0xa6 */ RTACPI_AML_OPC_INVALID,
    /* 0xa7 */ RTACPI_AML_OPC_INVALID,
    /* 0xa8 */ RTACPI_AML_OPC_INVALID,
    /* 0xa9 */ RTACPI_AML_OPC_INVALID,
    /* 0xaa */ RTACPI_AML_OPC_INVALID,
    /* 0xab */ RTACPI_AML_OPC_INVALID,
    /* 0xac */ RTACPI_AML_OPC_INVALID,
    /* 0xad */ RTACPI_AML_OPC_INVALID,
    /* 0xae */ RTACPI_AML_OPC_INVALID,
    /* 0xaf */ RTACPI_AML_OPC_INVALID,

    /* 0xb0 */ RTACPI_AML_OPC_INVALID,
    /* 0xb1 */ RTACPI_AML_OPC_INVALID,
    /* 0xb2 */ RTACPI_AML_OPC_INVALID,
    /* 0xb3 */ RTACPI_AML_OPC_INVALID,
    /* 0xb4 */ RTACPI_AML_OPC_INVALID,
    /* 0xb5 */ RTACPI_AML_OPC_INVALID,
    /* 0xb6 */ RTACPI_AML_OPC_INVALID,
    /* 0xb7 */ RTACPI_AML_OPC_INVALID,
    /* 0xb8 */ RTACPI_AML_OPC_INVALID,
    /* 0xb9 */ RTACPI_AML_OPC_INVALID,
    /* 0xba */ RTACPI_AML_OPC_INVALID,
    /* 0xbb */ RTACPI_AML_OPC_INVALID,
    /* 0xbc */ RTACPI_AML_OPC_INVALID,
    /* 0xbd */ RTACPI_AML_OPC_INVALID,
    /* 0xbe */ RTACPI_AML_OPC_INVALID,
    /* 0xbf */ RTACPI_AML_OPC_INVALID,

    /* 0xc0 */ RTACPI_AML_OPC_INVALID,
    /* 0xc1 */ RTACPI_AML_OPC_INVALID,
    /* 0xc2 */ RTACPI_AML_OPC_INVALID,
    /* 0xc3 */ RTACPI_AML_OPC_INVALID,
    /* 0xc4 */ RTACPI_AML_OPC_INVALID,
    /* 0xc5 */ RTACPI_AML_OPC_INVALID,
    /* 0xc6 */ RTACPI_AML_OPC_INVALID,
    /* 0xc7 */ RTACPI_AML_OPC_INVALID,
    /* 0xc8 */ RTACPI_AML_OPC_INVALID,
    /* 0xc9 */ RTACPI_AML_OPC_INVALID,
    /* 0xca */ RTACPI_AML_OPC_INVALID,
    /* 0xcb */ RTACPI_AML_OPC_INVALID,
    /* 0xcc */ RTACPI_AML_OPC_INVALID,
    /* 0xcd */ RTACPI_AML_OPC_INVALID,
    /* 0xce */ RTACPI_AML_OPC_INVALID,
    /* 0xcf */ RTACPI_AML_OPC_INVALID,

    /* 0xd0 */ RTACPI_AML_OPC_INVALID,
    /* 0xd1 */ RTACPI_AML_OPC_INVALID,
    /* 0xd2 */ RTACPI_AML_OPC_INVALID,
    /* 0xd3 */ RTACPI_AML_OPC_INVALID,
    /* 0xd4 */ RTACPI_AML_OPC_INVALID,
    /* 0xd5 */ RTACPI_AML_OPC_INVALID,
    /* 0xd6 */ RTACPI_AML_OPC_INVALID,
    /* 0xd7 */ RTACPI_AML_OPC_INVALID,
    /* 0xd8 */ RTACPI_AML_OPC_INVALID,
    /* 0xd9 */ RTACPI_AML_OPC_INVALID,
    /* 0xda */ RTACPI_AML_OPC_INVALID,
    /* 0xdb */ RTACPI_AML_OPC_INVALID,
    /* 0xdc */ RTACPI_AML_OPC_INVALID,
    /* 0xdd */ RTACPI_AML_OPC_INVALID,
    /* 0xde */ RTACPI_AML_OPC_INVALID,
    /* 0xdf */ RTACPI_AML_OPC_INVALID,

    /* 0xe0 */ RTACPI_AML_OPC_INVALID,
    /* 0xe1 */ RTACPI_AML_OPC_INVALID,
    /* 0xe2 */ RTACPI_AML_OPC_INVALID,
    /* 0xe3 */ RTACPI_AML_OPC_INVALID,
    /* 0xe4 */ RTACPI_AML_OPC_INVALID,
    /* 0xe5 */ RTACPI_AML_OPC_INVALID,
    /* 0xe6 */ RTACPI_AML_OPC_INVALID,
    /* 0xe7 */ RTACPI_AML_OPC_INVALID,
    /* 0xe8 */ RTACPI_AML_OPC_INVALID,
    /* 0xe9 */ RTACPI_AML_OPC_INVALID,
    /* 0xea */ RTACPI_AML_OPC_INVALID,
    /* 0xeb */ RTACPI_AML_OPC_INVALID,
    /* 0xec */ RTACPI_AML_OPC_INVALID,
    /* 0xed */ RTACPI_AML_OPC_INVALID,
    /* 0xee */ RTACPI_AML_OPC_INVALID,
    /* 0xef */ RTACPI_AML_OPC_INVALID,

    /* 0xf0 */ RTACPI_AML_OPC_INVALID,
    /* 0xf1 */ RTACPI_AML_OPC_INVALID,
    /* 0xf2 */ RTACPI_AML_OPC_INVALID,
    /* 0xf3 */ RTACPI_AML_OPC_INVALID,
    /* 0xf4 */ RTACPI_AML_OPC_INVALID,
    /* 0xf5 */ RTACPI_AML_OPC_INVALID,
    /* 0xf6 */ RTACPI_AML_OPC_INVALID,
    /* 0xf7 */ RTACPI_AML_OPC_INVALID,
    /* 0xf8 */ RTACPI_AML_OPC_INVALID,
    /* 0xf9 */ RTACPI_AML_OPC_INVALID,
    /* 0xfa */ RTACPI_AML_OPC_INVALID,
    /* 0xfb */ RTACPI_AML_OPC_INVALID,
    /* 0xfc */ RTACPI_AML_OPC_INVALID,
    /* 0xfd */ RTACPI_AML_OPC_INVALID,
    /* 0xfe */ RTACPI_AML_OPC_INVALID,
    /* 0xff */ RTACPI_AML_OPC_INVALID
};


static int rtAcpiTblAmlDecodeTerminal(PRTACPITBLAMLDECODE pThis, PRTACPIASTNODE *ppAstNd, PRTERRINFO pErrInfo)
{
    PCRTACPIAMLOPC pAmlOpc = NULL;
    uint8_t bOpc = 0; /* shut up gcc */
    int rc = rtAcpiTblAmlDecodeReadU8(pThis, &bOpc, pErrInfo);
    if (RT_SUCCESS(rc))
    {
        if (bOpc == ACPI_AML_BYTE_CODE_PREFIX_EXT_OP)
        {
            rc = rtAcpiTblAmlDecodeReadU8(pThis, &bOpc, pErrInfo);
            if (RT_FAILURE(rc))
                return rc;

            pAmlOpc = &g_aAmlExtOpcodeDecode[bOpc];
        }
        else
            pAmlOpc = &g_aAmlOpcodeDecode[bOpc];

        if (pAmlOpc->enmOp != kAcpiAstNodeOp_Invalid)
        {
            LogFlowFunc(("Decoding %s\n", rtAcpiTblAstNodeOp2Str(pAmlOpc->enmOp)));
            return pAmlOpc->pfnDecode(pThis, pAmlOpc, bOpc, ppAstNd, pErrInfo);
        }

        return RTErrInfoSetF(pErrInfo, VERR_INVALID_STATE, "Invalid opcode %#x in ACPI table at offset %u", bOpc, pThis->offTbl);
    }

    return rc;
}


DECLHIDDEN(int) rtAcpiTblConvertFromAmlToAsl(RTVFSIOSTREAM hVfsIosOut, RTVFSIOSTREAM hVfsIosIn, PRTERRINFO pErrInfo)
{
    ACPITBLHDR Hdr;
    int rc = RTVfsIoStrmRead(hVfsIosIn, &Hdr, sizeof(Hdr), true /*fBlocking*/, NULL /*pcbRead*/);
    if (RT_SUCCESS(rc))
    {
        Hdr.u32Signature       = RT_LE2H_U32(Hdr.u32Signature);
        Hdr.cbTbl              = RT_LE2H_U32(Hdr.cbTbl);
        Hdr.u32OemRevision     = RT_LE2H_U32(Hdr.u32OemRevision);
        Hdr.u32CreatorRevision = RT_LE2H_U32(Hdr.u32CreatorRevision);

        if (   Hdr.u32Signature == ACPI_TABLE_HDR_SIGNATURE_SSDT
            || Hdr.u32Signature == ACPI_TABLE_HDR_SIGNATURE_DSDT)
        {
            /** @todo Verify checksum */
            uint32_t const cbTbl = Hdr.cbTbl - sizeof(Hdr);
            if (cbTbl) /* Do we have something to decode at all? */
            {
                uint8_t *pbTbl = (uint8_t *)RTMemAlloc(cbTbl);
                if (pbTbl)
                {
                    rc = RTVfsIoStrmRead(hVfsIosIn, pbTbl, cbTbl, true /*fBlocking*/, NULL /*pcbRead*/);
                    if (RT_SUCCESS(rc))
                    {
                        RTACPITBLAMLDECODE AmlDecode;
                        AmlDecode.pbTbl            = pbTbl;
                        AmlDecode.cbTbl            = cbTbl;
                        AmlDecode.offTbl           = 0;
                        AmlDecode.iLvl             = 0;
                        AmlDecode.cPkgStackMax     = 0;
                        AmlDecode.pacbPkgLeft      = NULL;
                        AmlDecode.pacbPkg          = NULL;
                        AmlDecode.papLstScopeNodes = NULL;
                        RTListInit(&AmlDecode.LstObjs);
                        RTListInit(&AmlDecode.LstStmts);
                        rc = RTStrCacheCreate(&AmlDecode.hStrCache, "AmlStrLit");
                        if (RT_SUCCESS(rc))
                        {
                            AmlDecode.pNs = rtAcpiNsCreate();
                            if (AmlDecode.pNs)
                            {
                                rc = rtAcpiTblAmlDecodePkgPush(&AmlDecode, AmlDecode.cbTbl, &AmlDecode.LstStmts, pErrInfo);
                                while (   RT_SUCCESS(rc)
                                       && AmlDecode.offTbl < cbTbl)
                                {
                                    rc = rtAcpiTblAmlDecodeTerminal(&AmlDecode, NULL, pErrInfo);
                                    if (RT_SUCCESS(rc))
                                        rc = rtAcpiTblAmlDecodePkgPop(&AmlDecode, pErrInfo);
                                }
                                if (AmlDecode.pacbPkgLeft)
                                    RTMemFree(AmlDecode.pacbPkgLeft);
                                if (AmlDecode.pacbPkg)
                                    RTMemFree(AmlDecode.pacbPkg);
                                if (AmlDecode.papLstScopeNodes)
                                    RTMemFree(AmlDecode.papLstScopeNodes);

                                /** @todo Transform the AST. */

                                /* Dump the AST. */
                                if (RT_SUCCESS(rc))
                                {
                                    ssize_t cch = RTVfsIoStrmPrintf(hVfsIosOut, "DefinitionBlock(\"\", \"%s\", %u, \"%.6s\", \"%.8s\", %u)\n{\n",
                                                                    Hdr.u32Signature == ACPI_TABLE_HDR_SIGNATURE_SSDT ? "SSDT" : "DSDT",
                                                                    1, &Hdr.abOemId[0], &Hdr.abOemTblId[0], Hdr.u32OemRevision);
                                    if (cch > 0)
                                    {
                                        PRTACPIASTNODE pIt;
                                        RTListForEach(&AmlDecode.LstStmts, pIt, RTACPIASTNODE, NdAst)
                                        {
                                            rc = rtAcpiAstDumpToAsl(pIt, hVfsIosOut, 1 /*uLvl*/);
                                            if (RT_FAILURE(rc))
                                                break;
                                        }

                                        RTVfsIoStrmPrintf(hVfsIosOut, "}\n");
                                    }
                                    else
                                        rc = RTErrInfoSetF(pErrInfo, cch == 0 ? VERR_NO_MEMORY : (int)cch, "Failed to emit DefinitionBlock()");
                                }

                                /* Free resources. */
                                {
                                    PRTACPIASTNODE pIt, pItNext;
                                    RTListForEachSafe(&AmlDecode.LstStmts, pIt, pItNext, RTACPIASTNODE, NdAst)
                                    {
                                        RTListNodeRemove(&pIt->NdAst);
                                        rtAcpiAstNodeFree(pIt);
                                    }
                                }

                                {
                                    PRTACPITBLAMLOBJ pIt, pItNext;
                                    RTListForEachSafe(&AmlDecode.LstObjs, pIt, pItNext, RTACPITBLAMLOBJ, NdObjs)
                                    {
                                        RTListNodeRemove(&pIt->NdObjs);
                                        RTMemFree(pIt);
                                    }
                                }

                                rtAcpiNsDestroy(AmlDecode.pNs);
                            }
                            else
                                rc = RTErrInfoSetF(pErrInfo, VERR_NO_MEMORY, "OUt of memory creating the namespace structure");

                            RTStrCacheDestroy(AmlDecode.hStrCache);
                        }
                        else
                            rc = RTErrInfoSetF(pErrInfo, rc, "Failed to create string cache for literals");
                    }
                    else
                        rc = RTErrInfoSetF(pErrInfo, rc, "Reading %u bytes of the ACPI table failed", Hdr.cbTbl);

                    RTMemFree(pbTbl);
                    pbTbl = NULL;
                }
                else
                    rc = RTErrInfoSetF(pErrInfo, VERR_NO_MEMORY, "Allocating memory for the ACPI table failed");
            }
            else
            {
                ssize_t cch = RTVfsIoStrmPrintf(hVfsIosOut, "DefinitionBlock(\"\", \"%s\", %u, \"%.6s\", \"%.8s\", %u)\n{\n}\n",
                                                Hdr.u32Signature == ACPI_TABLE_HDR_SIGNATURE_SSDT ? "SSDT" : "DSDT",
                                                1, &Hdr.abOemId[0], &Hdr.abOemTblId[0], Hdr.u32OemRevision);
                if (cch <= 0)
                    rc = RTErrInfoSetF(pErrInfo, cch == 0 ? VERR_NO_MEMORY : (int)cch, "Failed to emit DefinitionBlock()");
            }
        }
        else
            rc = RTErrInfoSetF(pErrInfo, VERR_NOT_SUPPORTED, "Only DSDT and SSDT ACPI tables are supported");
    }
    else
        rc = RTErrInfoSetF(pErrInfo, rc, "Reading the ACPI table header failed with %Rrc", rc);

    return rc;
}

