/* $Id: acpi-ast.cpp 112663 2026-01-21 15:02:23Z alexander.eichner@oracle.com $ */
/** @file
 * IPRT - Advanced Configuration and Power Interface (ACPI) AST handling.
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
#include <iprt/assert.h>
#include <iprt/errcore.h>
#include <iprt/list.h>
#include <iprt/mem.h>

#include <iprt/formats/acpi-aml.h>

#include "internal/acpi.h"


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/


/*********************************************************************************************************************************
*   Internal Functions                                                                                                           *
*********************************************************************************************************************************/

DECLHIDDEN(PRTACPIASTNODE) rtAcpiAstNodeAlloc(PCRTACPINSROOT pNs, RTACPIASTNODEOP enmOp, uint32_t fFlags, uint8_t cArgs)
{
    PRTACPIASTNODE pAstNd = (PRTACPIASTNODE)RTMemAllocZ(RT_UOFFSETOF_DYN(RTACPIASTNODE, aArgs[cArgs]));
    if (pAstNd)
    {
        pAstNd->pNsEntry = rtAcpiNsGetCurrent(pNs);
        pAstNd->enmOp    = enmOp;
        pAstNd->fFlags   = fFlags;
        pAstNd->cArgs    = cArgs;
        RTListInit(&pAstNd->LstScopeNodes);
    }

    return pAstNd;
}


DECLHIDDEN(void) rtAcpiAstNodeFree(PRTACPIASTNODE pAstNd)
{
    /* Free all the arguments first. */
    for (uint8_t i = 0; i < pAstNd->cArgs; i++)
    {
        if (   pAstNd->aArgs[i].enmType == kAcpiAstArgType_AstNode
            && pAstNd->aArgs[i].u.pAstNd)
            rtAcpiAstNodeFree(pAstNd->aArgs[i].u.pAstNd);
    }

    if (pAstNd->fFlags & RTACPI_AST_NODE_F_NEW_SCOPE)
    {
        PRTACPIASTNODE pIt, pItNext;
        /* Do transformations on the nodes first. */
        RTListForEachSafe(&pAstNd->LstScopeNodes, pIt, pItNext, RTACPIASTNODE, NdAst)
        {
            RTListNodeRemove(&pIt->NdAst);
            rtAcpiAstNodeFree(pIt);
        }
    }

    switch (pAstNd->enmOp)
    {
        case kAcpiAstNodeOp_Field:
        case kAcpiAstNodeOp_IndexField:
        {
            RTMemFree(pAstNd->Fields.paFields);
            pAstNd->Fields.paFields = NULL;
            pAstNd->Fields.cFields  = 0;
            break;
        }
        case kAcpiAstNodeOp_ResourceTemplate:
        {
            RTAcpiResourceDestroy(pAstNd->hAcpiRes);
            pAstNd->hAcpiRes = NIL_RTACPIRES;
            break;
        }
        default:
            break;
    }

    pAstNd->enmOp  = kAcpiAstNodeOp_Invalid;
    pAstNd->cArgs  = 0;
    pAstNd->fFlags = 0;
    RTMemFree(pAstNd);
}


/**
 * Evaluates the given AST node to an integer if possible.
 *
 * @returns IPRT status code.
 * @param   pAstNd                  The AST node to evaluate.
 * @param   pNsRoot                 The namespace root this AST belongs to.
 * @param   fResolveIdentifiers     Flag whether to try resolving identifiers to constant integers.
 * @param   pu64                    Where to store the integer on success.
 */
static int rtAcpiAstNodeEvaluateToInteger(PCRTACPIASTNODE pAstNd, PRTACPINSROOT pNsRoot, bool fResolveIdentifiers, uint64_t *pu64)
{
    /* Easy way out?. */
    if (pAstNd->enmOp == kAcpiAstNodeOp_Number)
    {
        *pu64 = pAstNd->u64;
        return VINF_SUCCESS;
    }

    if (pAstNd->enmOp == kAcpiAstNodeOp_One)
    {
        *pu64 = 1;
        return VINF_SUCCESS;
    }

    if (pAstNd->enmOp == kAcpiAstNodeOp_Zero)
    {
        *pu64 = 0;
        return VINF_SUCCESS;
    }

    if (   pAstNd->enmOp == kAcpiAstNodeOp_Identifier
        && fResolveIdentifiers)
    {
        /* Look it up in the namespace and use the result. */
        PCRTACPINSENTRY pNsEntry = rtAcpiNsLookup(pNsRoot, pAstNd->pszIde);
        if (!pNsEntry)
            return VERR_NOT_FOUND;
        if (pNsEntry->enmType != kAcpiNsEntryType_ResourceField)
            return VERR_NOT_SUPPORTED;

        *pu64 = pNsEntry->RsrcFld.offBits;
        return VINF_SUCCESS;
    }

    /** @todo */
    return VERR_NOT_IMPLEMENTED;
}


DECLHIDDEN(int) rtAcpiAstNodeTransform(PRTACPIASTNODE pAstNd, PRTACPINSROOT pNsRoot, PRTERRINFO pErrInfo)
{
    /* Walk all arguments containing AST nodes first. */
    for (uint8_t i = 0; i < pAstNd->cArgs; i++)
    {
        if (   pAstNd->aArgs[i].enmType == kAcpiAstArgType_AstNode
            && pAstNd->aArgs[i].u.pAstNd)
        {
            int rc = rtAcpiAstNodeTransform(pAstNd->aArgs[i].u.pAstNd, pNsRoot, pErrInfo);
            if (RT_FAILURE(rc))
                return rc;
        }
    }

    if (pAstNd->fFlags & RTACPI_AST_NODE_F_NEW_SCOPE)
    {
        PRTACPIASTNODE pIt/*, pItPrev*/;
        /* Do transformations on the nodes first. */
        RTListForEach(&pAstNd->LstScopeNodes, pIt, RTACPIASTNODE, NdAst)
        {
            int rc = rtAcpiAstNodeTransform(pIt, pNsRoot, pErrInfo);
            if (RT_FAILURE(rc))
                return rc;
        }
    }

    /* Now do optimizations we can do here. */
    switch (pAstNd->enmOp)
    {
        case kAcpiAstNodeOp_ShiftLeft:
        {
            /*
             * If both arguments evaluate to constant integers we can convert this
             * to the final result.
             */
            /** @todo Skips the 3 operand variant (no target), check what iasl is doing here. */
            if (!pAstNd->aArgs[2].u.pAstNd)
            {
                uint64_t u64ValToShift = 0;
                uint64_t u64ValShift = 0;
                int rc = rtAcpiAstNodeEvaluateToInteger(pAstNd->aArgs[0].u.pAstNd, pNsRoot, false /*fResolveIdentifiers*/, &u64ValToShift);
                if (RT_SUCCESS(rc))
                    rc = rtAcpiAstNodeEvaluateToInteger(pAstNd->aArgs[1].u.pAstNd, pNsRoot, false /*fResolveIdentifiers*/, &u64ValShift);
                if (   RT_SUCCESS(rc)
                    && u64ValShift <= 63)
                {
                    /** @todo Check overflow handling. */
                    rtAcpiAstNodeFree(pAstNd->aArgs[0].u.pAstNd);
                    rtAcpiAstNodeFree(pAstNd->aArgs[1].u.pAstNd);

                    pAstNd->aArgs[0].u.pAstNd = NULL;
                    pAstNd->aArgs[1].u.pAstNd = NULL;
                    pAstNd->cArgs             = 0;
                    pAstNd->enmOp             = kAcpiAstNodeOp_Number;
                    pAstNd->u64               = u64ValToShift << u64ValShift;
                }
            }
            break;
        }
        default:
            break;
    }

    return VINF_SUCCESS;
}


static int rtAcpiAstDumpAstList(PCRTLISTANCHOR pLst, PRTACPINSROOT pNsRoot, RTACPITBL hAcpiTbl)
{
    PCRTACPIASTNODE pIt;
    RTListForEach(pLst, pIt, RTACPIASTNODE, NdAst)
    {
        int rc = rtAcpiAstDumpToTbl(pIt, pNsRoot, hAcpiTbl);
        if (RT_FAILURE(rc))
            return rc;
    }

    return VINF_SUCCESS;
}


DECLHIDDEN(int) rtAcpiAstDumpToTbl(PCRTACPIASTNODE pAstNd, PRTACPINSROOT pNsRoot, RTACPITBL hAcpiTbl)
{
    int rc = VINF_SUCCESS;
    char szNameString[_1K];

    switch (pAstNd->enmOp)
    {
        case kAcpiAstNodeOp_Identifier:
        {
            rc = rtAcpiNsAbsoluteNameStringToRelative(pNsRoot, pAstNd->pNsEntry, pAstNd->pszIde, &szNameString[0], sizeof(szNameString));
            AssertRC(rc);

            rc = RTAcpiTblNameStringAppend(hAcpiTbl, szNameString);
            if (RT_SUCCESS(rc))
            {
                for (uint8_t i = 0; i < pAstNd->cArgs; i++)
                {
                    Assert(pAstNd->aArgs[i].enmType == kAcpiAstArgType_AstNode);
                    rc = rtAcpiAstDumpToTbl(pAstNd->aArgs[i].u.pAstNd, pNsRoot, hAcpiTbl);
                    if (RT_FAILURE(rc))
                        break;
                }
            }
            break;
        }
        case kAcpiAstNodeOp_StringLiteral:
            rc = RTAcpiTblStringAppend(hAcpiTbl, pAstNd->pszStrLit);
            break;
        case kAcpiAstNodeOp_Number:
            rc = RTAcpiTblIntegerAppend(hAcpiTbl, pAstNd->u64);
            break;
        case kAcpiAstNodeOp_Scope:
        {
            AssertBreakStmt(   pAstNd->cArgs == 1
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_NameString,
                            rc = VERR_INTERNAL_ERROR);
            rc = rtAcpiNsCompressNameString(pNsRoot, pAstNd->pNsEntry, pAstNd->aArgs[0].u.pszNameString,
                                            &szNameString[0], sizeof(szNameString));
            AssertRC(rc);

            rc = RTAcpiTblScopeStart(hAcpiTbl, szNameString);
            if (RT_SUCCESS(rc))
            {
                /* Walk all the other AST nodes. */
                rc = rtAcpiAstDumpAstList(&pAstNd->LstScopeNodes, pNsRoot, hAcpiTbl);
                if (RT_SUCCESS(rc))
                    rc = RTAcpiTblScopeFinalize(hAcpiTbl);
            }
            break;
        }
        case kAcpiAstNodeOp_Processor:
        {
            AssertBreakStmt(   pAstNd->cArgs == 4
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_NameString
                            && pAstNd->aArgs[1].enmType == kAcpiAstArgType_U8
                            && pAstNd->aArgs[2].enmType == kAcpiAstArgType_U32
                            && pAstNd->aArgs[3].enmType == kAcpiAstArgType_U8,
                            rc = VERR_INTERNAL_ERROR);
            rc = RTAcpiTblProcessorStart(hAcpiTbl,
                                         pAstNd->aArgs[0].u.pszNameString,
                                         pAstNd->aArgs[1].u.u8,
                                         pAstNd->aArgs[2].u.u32,
                                         pAstNd->aArgs[3].u.u8);
            if (RT_SUCCESS(rc))
            {
                /* Walk all the other AST nodes. */
                rc = rtAcpiAstDumpAstList(&pAstNd->LstScopeNodes, pNsRoot, hAcpiTbl);
                if (RT_SUCCESS(rc))
                    rc = RTAcpiTblProcessorFinalize(hAcpiTbl);
            }
            break;
        }
        case kAcpiAstNodeOp_Method:
        {
            AssertBreakStmt(   pAstNd->cArgs == 4
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_NameString
                            && pAstNd->aArgs[1].enmType == kAcpiAstArgType_U8
                            && pAstNd->aArgs[2].enmType == kAcpiAstArgType_Bool
                            && pAstNd->aArgs[3].enmType == kAcpiAstArgType_U8,
                            rc = VERR_INTERNAL_ERROR);
            rc = rtAcpiNsCompressNameString(pNsRoot, pAstNd->pNsEntry, pAstNd->aArgs[0].u.pszNameString,
                                            &szNameString[0], sizeof(szNameString));
            AssertRC(rc);

            rc = RTAcpiTblMethodStart(hAcpiTbl, szNameString,
                                      pAstNd->aArgs[1].u.u8,
                                      pAstNd->aArgs[2].u.f ? RTACPI_METHOD_F_SERIALIZED : RTACPI_METHOD_F_NOT_SERIALIZED,
                                      pAstNd->aArgs[3].u.u8);
            if (RT_SUCCESS(rc))
            {
                /* Walk all the other AST nodes. */
                rc = rtAcpiAstDumpAstList(&pAstNd->LstScopeNodes, pNsRoot, hAcpiTbl);
                if (RT_SUCCESS(rc))
                    rc = RTAcpiTblMethodFinalize(hAcpiTbl);
            }
            break;
        }
        case kAcpiAstNodeOp_Device:
        {
            AssertBreakStmt(   pAstNd->cArgs == 1
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_NameString,
                            rc = VERR_INTERNAL_ERROR);
            rc = RTAcpiTblDeviceStart(hAcpiTbl, pAstNd->aArgs[0].u.pszNameString);
            if (RT_SUCCESS(rc))
            {
                /* Walk all the other AST nodes. */
                rc = rtAcpiAstDumpAstList(&pAstNd->LstScopeNodes, pNsRoot, hAcpiTbl);
                if (RT_SUCCESS(rc))
                    rc = RTAcpiTblDeviceFinalize(hAcpiTbl);
            }
            break;
        }
        case kAcpiAstNodeOp_If:
        {
            AssertBreakStmt(   pAstNd->cArgs == 1
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_AstNode,
                            rc = VERR_INTERNAL_ERROR);
            rc = RTAcpiTblIfStart(hAcpiTbl);
            if (RT_SUCCESS(rc))
            {
                /* Predicate. */
                rc = rtAcpiAstDumpToTbl(pAstNd->aArgs[0].u.pAstNd, pNsRoot, hAcpiTbl);
                if (RT_SUCCESS(rc))
                {
                    /* Walk all the other AST nodes. */
                    rc = rtAcpiAstDumpAstList(&pAstNd->LstScopeNodes, pNsRoot, hAcpiTbl);
                    if (RT_SUCCESS(rc))
                        rc = RTAcpiTblIfFinalize(hAcpiTbl);
                }
            }
            break;
        }
        case kAcpiAstNodeOp_Else:
        {
            AssertBreakStmt(pAstNd->cArgs == 0, rc = VERR_INTERNAL_ERROR);
            rc = RTAcpiTblElseStart(hAcpiTbl);
            if (RT_SUCCESS(rc))
            {
                /* Walk all the other AST nodes. */
                rc = rtAcpiAstDumpAstList(&pAstNd->LstScopeNodes, pNsRoot, hAcpiTbl);
                if (RT_SUCCESS(rc))
                    rc = RTAcpiTblElseFinalize(hAcpiTbl);
            }
            break;
        }
        case kAcpiAstNodeOp_While:
        {
            AssertBreakStmt(   pAstNd->cArgs == 1
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_AstNode,
                            rc = VERR_INTERNAL_ERROR);
            rc = RTAcpiTblWhileStart(hAcpiTbl);
            if (RT_SUCCESS(rc))
            {
                /* Predicate. */
                rc = rtAcpiAstDumpToTbl(pAstNd->aArgs[0].u.pAstNd, pNsRoot, hAcpiTbl);
                if (RT_SUCCESS(rc))
                {
                    /* Walk all the other AST nodes. */
                    rc = rtAcpiAstDumpAstList(&pAstNd->LstScopeNodes, pNsRoot, hAcpiTbl);
                    if (RT_SUCCESS(rc))
                        rc = RTAcpiTblWhileFinalize(hAcpiTbl);
                }
            }
            break;
        }
        case kAcpiAstNodeOp_LAnd:
        case kAcpiAstNodeOp_LOr:
        case kAcpiAstNodeOp_LEqual:
        case kAcpiAstNodeOp_LGreater:
        case kAcpiAstNodeOp_LGreaterEqual:
        case kAcpiAstNodeOp_LLess:
        case kAcpiAstNodeOp_LLessEqual:
        case kAcpiAstNodeOp_LNotEqual:
        {
            AssertBreakStmt(   pAstNd->cArgs == 2
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_AstNode
                            && pAstNd->aArgs[1].enmType == kAcpiAstArgType_AstNode,
                            rc = VERR_INTERNAL_ERROR);
            RTACPIBINARYOP enmOp;
            switch (pAstNd->enmOp)
            {
                case kAcpiAstNodeOp_LAnd:           enmOp = kAcpiBinaryOp_LAnd; break;
                case kAcpiAstNodeOp_LOr:            enmOp = kAcpiBinaryOp_LOr; break;
                case kAcpiAstNodeOp_LEqual:         enmOp = kAcpiBinaryOp_LEqual; break;
                case kAcpiAstNodeOp_LGreater:       enmOp = kAcpiBinaryOp_LGreater; break;
                case kAcpiAstNodeOp_LGreaterEqual:  enmOp = kAcpiBinaryOp_LGreaterEqual; break;
                case kAcpiAstNodeOp_LLess:          enmOp = kAcpiBinaryOp_LLess; break;
                case kAcpiAstNodeOp_LLessEqual:     enmOp = kAcpiBinaryOp_LLessEqual; break;
                case kAcpiAstNodeOp_LNotEqual:      enmOp = kAcpiBinaryOp_LNotEqual; break;
                default:
                    AssertReleaseFailed(); /* Impossible */
                    return VERR_INTERNAL_ERROR;
            }

            rc = RTAcpiTblBinaryOpAppend(hAcpiTbl, enmOp);
            if (RT_SUCCESS(rc))
            {
                rc = rtAcpiAstDumpToTbl(pAstNd->aArgs[0].u.pAstNd, pNsRoot, hAcpiTbl);
                if (RT_SUCCESS(rc))
                    rc = rtAcpiAstDumpToTbl(pAstNd->aArgs[1].u.pAstNd, pNsRoot, hAcpiTbl);
            }
            break;
        }
        case kAcpiAstNodeOp_LNot:
            AssertBreakStmt(   pAstNd->cArgs == 1
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_AstNode,
                            rc = VERR_INTERNAL_ERROR);
            rc = RTAcpiTblStmtSimpleAppend(hAcpiTbl, kAcpiStmt_LNot);
            if (RT_SUCCESS(rc))
                rc = rtAcpiAstDumpToTbl(pAstNd->aArgs[0].u.pAstNd, pNsRoot, hAcpiTbl);
            break;
        case kAcpiAstNodeOp_Zero:
        {
            AssertBreakStmt(pAstNd->cArgs == 0, rc = VERR_INTERNAL_ERROR);
            rc = RTAcpiTblIntegerAppend(hAcpiTbl, 0);
            break;
        }
        case kAcpiAstNodeOp_One:
        {
            AssertBreakStmt(pAstNd->cArgs == 0, rc = VERR_INTERNAL_ERROR);
            rc = RTAcpiTblIntegerAppend(hAcpiTbl, 1);
            break;
        }
        case kAcpiAstNodeOp_Ones:
        {
            AssertBreakStmt(pAstNd->cArgs == 0, rc = VERR_INTERNAL_ERROR);
            rc = RTAcpiTblStmtSimpleAppend(hAcpiTbl, kAcpiStmt_Ones);
            break;
        }
        case kAcpiAstNodeOp_Return:
        {
            AssertBreakStmt(   pAstNd->cArgs == 1
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_AstNode,
                            rc = VERR_INTERNAL_ERROR);
            rc = RTAcpiTblStmtSimpleAppend(hAcpiTbl, kAcpiStmt_Return);
            if (RT_SUCCESS(rc))
            {
                if (pAstNd->aArgs[0].u.pAstNd)
                    rc = rtAcpiAstDumpToTbl(pAstNd->aArgs[0].u.pAstNd, pNsRoot, hAcpiTbl);
                else
                    rc = RTAcpiTblNullNameAppend(hAcpiTbl);
            }
            break;
        }
        case kAcpiAstNodeOp_Unicode:
        {
            AssertBreakStmt(   pAstNd->cArgs == 1
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_AstNode
                            && pAstNd->aArgs[0].u.pAstNd->enmOp == kAcpiAstNodeOp_StringLiteral,
                            rc = VERR_INTERNAL_ERROR);

            rc = RTAcpiTblStringAppendAsUtf16(hAcpiTbl, pAstNd->aArgs[0].u.pAstNd->pszStrLit);
            break;
        }
        case kAcpiAstNodeOp_OperationRegion:
        {
            AssertBreakStmt(   pAstNd->cArgs == 4
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_NameString
                            && pAstNd->aArgs[1].enmType == kAcpiAstArgType_RegionSpace
                            && pAstNd->aArgs[2].enmType == kAcpiAstArgType_AstNode
                            && pAstNd->aArgs[3].enmType == kAcpiAstArgType_AstNode,
                            rc = VERR_INTERNAL_ERROR);

            rc = RTAcpiTblOpRegionAppendEx(hAcpiTbl, pAstNd->aArgs[0].u.pszNameString, pAstNd->aArgs[1].u.enmRegionSpace);
            if (RT_SUCCESS(rc))
                rc = rtAcpiAstDumpToTbl(pAstNd->aArgs[2].u.pAstNd, pNsRoot, hAcpiTbl);
            if (RT_SUCCESS(rc))
                rc = rtAcpiAstDumpToTbl(pAstNd->aArgs[3].u.pAstNd, pNsRoot, hAcpiTbl);
            break;
        }
        case kAcpiAstNodeOp_Field:
        {
            AssertBreakStmt(   pAstNd->cArgs == 4
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_NameString
                            && pAstNd->aArgs[1].enmType == kAcpiAstArgType_FieldAcc
                            && pAstNd->aArgs[2].enmType == kAcpiAstArgType_Bool
                            && pAstNd->aArgs[3].enmType == kAcpiAstArgType_FieldUpdate,
                            rc = VERR_INTERNAL_ERROR);
            rc = rtAcpiNsAbsoluteNameStringToRelative(pNsRoot, pAstNd->pNsEntry, pAstNd->aArgs[0].u.pszNameString, &szNameString[0], sizeof(szNameString));
            AssertRC(rc);

            rc = RTAcpiTblFieldAppend(hAcpiTbl, szNameString, pAstNd->aArgs[1].u.enmFieldAcc,
                                      pAstNd->aArgs[2].u.f, pAstNd->aArgs[3].u.enmFieldUpdate, pAstNd->Fields.paFields,
                                      pAstNd->Fields.cFields);
            break;
        }
        case kAcpiAstNodeOp_IndexField:
        {
            AssertBreakStmt(   pAstNd->cArgs == 5
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_NameString
                            && pAstNd->aArgs[1].enmType == kAcpiAstArgType_NameString
                            && pAstNd->aArgs[2].enmType == kAcpiAstArgType_FieldAcc
                            && pAstNd->aArgs[3].enmType == kAcpiAstArgType_Bool
                            && pAstNd->aArgs[4].enmType == kAcpiAstArgType_FieldUpdate,
                            rc = VERR_INTERNAL_ERROR);

            rc = RTAcpiTblIndexFieldAppend(hAcpiTbl, pAstNd->aArgs[0].u.pszNameString, pAstNd->aArgs[1].u.pszNameString,
                                           pAstNd->aArgs[2].u.enmFieldAcc, pAstNd->aArgs[3].u.f, pAstNd->aArgs[4].u.enmFieldUpdate,
                                           pAstNd->Fields.paFields, pAstNd->Fields.cFields);
            break;
        }
        case kAcpiAstNodeOp_Name:
        {
            AssertBreakStmt(   pAstNd->cArgs == 2
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_NameString
                            && pAstNd->aArgs[1].enmType == kAcpiAstArgType_AstNode,
                            rc = VERR_INTERNAL_ERROR);

            rc = RTAcpiTblNameAppend(hAcpiTbl, pAstNd->aArgs[0].u.pszNameString);
            if (RT_SUCCESS(rc))
                rc = rtAcpiAstDumpToTbl(pAstNd->aArgs[1].u.pAstNd, pNsRoot, hAcpiTbl);
            break;
        }
        case kAcpiAstNodeOp_ResourceTemplate:
            rc = RTAcpiTblResourceAppend(hAcpiTbl, pAstNd->hAcpiRes);
            break;
        case kAcpiAstNodeOp_Arg0:
        case kAcpiAstNodeOp_Arg1:
        case kAcpiAstNodeOp_Arg2:
        case kAcpiAstNodeOp_Arg3:
        case kAcpiAstNodeOp_Arg4:
        case kAcpiAstNodeOp_Arg5:
        case kAcpiAstNodeOp_Arg6:
            rc = RTAcpiTblArgOpAppend(hAcpiTbl, pAstNd->enmOp - kAcpiAstNodeOp_Arg0);
            break;
        case kAcpiAstNodeOp_Local0:
        case kAcpiAstNodeOp_Local1:
        case kAcpiAstNodeOp_Local2:
        case kAcpiAstNodeOp_Local3:
        case kAcpiAstNodeOp_Local4:
        case kAcpiAstNodeOp_Local5:
        case kAcpiAstNodeOp_Local6:
        case kAcpiAstNodeOp_Local7:
            rc = RTAcpiTblLocalOpAppend(hAcpiTbl, pAstNd->enmOp - kAcpiAstNodeOp_Local0);
            break;
        case kAcpiAstNodeOp_Package:
        {
            AssertBreakStmt(   pAstNd->cArgs == 1
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_AstNode,
                            rc = VERR_INTERNAL_ERROR);

            /* Try to gather the number of elements. */
            uint64_t cElems = 0;
            if (pAstNd->aArgs[0].u.pAstNd)
            {
                /* Try resolving to a constant expression. */
                rc = rtAcpiAstNodeEvaluateToInteger(pAstNd->aArgs[0].u.pAstNd, pNsRoot, true /*fResolveIdentifiers*/, &cElems);
                if (RT_FAILURE(rc))
                    break;
            }
            else
            {
                /* Count elements. */
                PRTACPIASTNODE pIt;
                RTListForEach(&pAstNd->LstScopeNodes, pIt, RTACPIASTNODE, NdAst)
                {
                    cElems++;
                }
            }
            if (RT_SUCCESS(rc))
            {
                if (cElems > 255)
                {
                    rc = VERR_BUFFER_OVERFLOW;
                    break;
                }

                rc = RTAcpiTblPackageStart(hAcpiTbl, (uint8_t)cElems);
                if (RT_SUCCESS(rc))
                    rc = rtAcpiAstDumpAstList(&pAstNd->LstScopeNodes, pNsRoot, hAcpiTbl);
                if (RT_SUCCESS(rc))
                    rc = RTAcpiTblPackageFinalize(hAcpiTbl);
            }
            break;
        }
        case kAcpiAstNodeOp_Buffer:
        {
            AssertBreakStmt(   pAstNd->cArgs == 1
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_AstNode,
                            rc = VERR_INTERNAL_ERROR);

            rc = RTAcpiTblBufferStart(hAcpiTbl);
            if (RT_FAILURE(rc))
                break;

            /* Try to gather the number of elements. */
            uint64_t cElems = 0;
            /* Count elements. */
            PRTACPIASTNODE pIt;
            RTListForEach(&pAstNd->LstScopeNodes, pIt, RTACPIASTNODE, NdAst)
            {
                cElems++;
            }

            /*
             * If the buffer size is empty (no AST node) the number of elements
             * in the initializer serve as tehe buffer size.
             */
            /** @todo Strings. */
            if (pAstNd->aArgs[0].u.pAstNd)
                rc = rtAcpiAstDumpToTbl(pAstNd->aArgs[0].u.pAstNd, pNsRoot, hAcpiTbl);
            else
                rc = RTAcpiTblIntegerAppend(hAcpiTbl, cElems);

            if (   RT_SUCCESS(rc)
                && cElems)
            {
                uint8_t *pb = (uint8_t *)RTMemAlloc(cElems);
                if (pb)
                {
                    uint64_t i = 0;
                    RTListForEach(&pAstNd->LstScopeNodes, pIt, RTACPIASTNODE, NdAst)
                    {
                        /* Try resolving to a constant expression. */
                        uint64_t u64 = 0;
                        rc = rtAcpiAstNodeEvaluateToInteger(pIt, pNsRoot, true /*fResolveIdentifiers*/, &u64);
                        if (RT_FAILURE(rc))
                            break;
                        if (u64 > UINT8_MAX)
                        {
                            rc = VERR_BUFFER_OVERFLOW;
                            break;
                        }

                        pb[i++] = (uint8_t)u64;
                    }

                    if (RT_SUCCESS(rc))
                        rc = RTAcpiTblBufferAppendRawData(hAcpiTbl, pb, cElems);
                    RTMemFree(pb);
                }
                else
                    rc = VERR_NO_MEMORY;
            }

            if (RT_SUCCESS(rc))
                rc = RTAcpiTblBufferFinalize(hAcpiTbl);
            break;
        }
        case kAcpiAstNodeOp_ToUuid:
        {
            AssertBreakStmt(   pAstNd->cArgs == 1
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_AstNode
                            && pAstNd->aArgs[0].u.pAstNd->enmOp == kAcpiAstNodeOp_StringLiteral,
                            rc = VERR_INTERNAL_ERROR);
            rc = RTAcpiTblUuidAppendFromStr(hAcpiTbl, pAstNd->aArgs[0].u.pAstNd->pszStrLit);
            break;
        }
        case kAcpiAstNodeOp_Break:
        {
            AssertBreakStmt(pAstNd->cArgs == 0, rc = VERR_INTERNAL_ERROR);
            rc = RTAcpiTblStmtSimpleAppend(hAcpiTbl, kAcpiStmt_Break);
            break;
        }
        case kAcpiAstNodeOp_Continue:
        {
            AssertBreakStmt(pAstNd->cArgs == 0, rc = VERR_INTERNAL_ERROR);
            rc = RTAcpiTblStmtSimpleAppend(hAcpiTbl, kAcpiStmt_Continue);
            break;
        }
        case kAcpiAstNodeOp_DerefOf:
        case kAcpiAstNodeOp_SizeOf:
        case kAcpiAstNodeOp_Increment:
        case kAcpiAstNodeOp_Decrement:
        {
            AssertBreakStmt(   pAstNd->cArgs == 1
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_AstNode,
                            rc = VERR_INTERNAL_ERROR);

            RTACPISTMT enmStmt;
            switch (pAstNd->enmOp)
            {
                case kAcpiAstNodeOp_DerefOf:   enmStmt = kAcpiStmt_DerefOf;   break;
                case kAcpiAstNodeOp_SizeOf:    enmStmt = kAcpiStmt_SizeOf;    break;
                case kAcpiAstNodeOp_Increment: enmStmt = kAcpiStmt_Increment; break;
                case kAcpiAstNodeOp_Decrement: enmStmt = kAcpiStmt_Decrement; break;
                default:
                    AssertReleaseFailed(); /* Impossible */
                    return VERR_INTERNAL_ERROR;
            }


            rc = RTAcpiTblStmtSimpleAppend(hAcpiTbl, enmStmt);
            if (RT_SUCCESS(rc))
                rc = rtAcpiAstDumpToTbl(pAstNd->aArgs[0].u.pAstNd, pNsRoot, hAcpiTbl);
            break;
        }
        case kAcpiAstNodeOp_Store:
        case kAcpiAstNodeOp_Notify:
        {
            AssertBreakStmt(   pAstNd->cArgs == 2
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_AstNode
                            && pAstNd->aArgs[1].enmType == kAcpiAstArgType_AstNode,
                            rc = VERR_INTERNAL_ERROR);
            rc = RTAcpiTblStmtSimpleAppend(hAcpiTbl,
                                             pAstNd->enmOp == kAcpiAstNodeOp_Store
                                           ? kAcpiStmt_Store
                                           : kAcpiStmt_Notify);
            if (RT_SUCCESS(rc))
                rc = rtAcpiAstDumpToTbl(pAstNd->aArgs[0].u.pAstNd, pNsRoot, hAcpiTbl);
            if (RT_SUCCESS(rc))
                rc = rtAcpiAstDumpToTbl(pAstNd->aArgs[1].u.pAstNd, pNsRoot, hAcpiTbl);
            break;
        }
        case kAcpiAstNodeOp_Not:
        case kAcpiAstNodeOp_CondRefOf:
        case kAcpiAstNodeOp_FindSetLeftBit:
        case kAcpiAstNodeOp_FindSetRightBit:
        {
            AssertBreakStmt(   pAstNd->cArgs == 2
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_AstNode
                            && pAstNd->aArgs[1].enmType == kAcpiAstArgType_AstNode,
                            rc = VERR_INTERNAL_ERROR);

            RTACPISTMT enmStmt;
            switch (pAstNd->enmOp)
            {
                case kAcpiAstNodeOp_Not:             enmStmt = kAcpiStmt_Not;             break;
                case kAcpiAstNodeOp_CondRefOf:       enmStmt = kAcpiStmt_CondRefOf;       break;
                case kAcpiAstNodeOp_FindSetLeftBit:  enmStmt = kAcpiStmt_FindSetLeftBit;  break;
                case kAcpiAstNodeOp_FindSetRightBit: enmStmt = kAcpiStmt_FindSetRightBit; break;
                default:
                    AssertReleaseFailed(); /* Impossible */
                    return VERR_INTERNAL_ERROR;
            }

            rc = RTAcpiTblStmtSimpleAppend(hAcpiTbl, enmStmt);
            if (RT_SUCCESS(rc))
                rc = rtAcpiAstDumpToTbl(pAstNd->aArgs[0].u.pAstNd, pNsRoot, hAcpiTbl);
            if (RT_SUCCESS(rc))
            {
                if (pAstNd->aArgs[1].u.pAstNd)
                    rc = rtAcpiAstDumpToTbl(pAstNd->aArgs[1].u.pAstNd, pNsRoot, hAcpiTbl);
                else
                    rc = RTAcpiTblNullNameAppend(hAcpiTbl);
            }
            break;
        }
        case kAcpiAstNodeOp_Index:
        case kAcpiAstNodeOp_Add:
        case kAcpiAstNodeOp_Subtract:
        case kAcpiAstNodeOp_Multiply:
        case kAcpiAstNodeOp_And:
        case kAcpiAstNodeOp_Nand:
        case kAcpiAstNodeOp_Or:
        case kAcpiAstNodeOp_Xor:
        case kAcpiAstNodeOp_ShiftLeft:
        case kAcpiAstNodeOp_ShiftRight:
        case kAcpiAstNodeOp_ConcatenateResTemplate:
        {
            AssertBreakStmt(   pAstNd->cArgs == 3
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_AstNode
                            && pAstNd->aArgs[1].enmType == kAcpiAstArgType_AstNode
                            && pAstNd->aArgs[2].enmType == kAcpiAstArgType_AstNode,
                            rc = VERR_INTERNAL_ERROR);

            RTACPISTMT enmStmt;
            switch (pAstNd->enmOp)
            {
                case kAcpiAstNodeOp_Index:      enmStmt = kAcpiStmt_Index;      break;
                case kAcpiAstNodeOp_Add:        enmStmt = kAcpiStmt_Add;        break;
                case kAcpiAstNodeOp_Subtract:   enmStmt = kAcpiStmt_Subtract;   break;
                case kAcpiAstNodeOp_Multiply:   enmStmt = kAcpiStmt_Multiply;   break;
                case kAcpiAstNodeOp_And:        enmStmt = kAcpiStmt_And;        break;
                case kAcpiAstNodeOp_Nand:       enmStmt = kAcpiStmt_Nand;       break;
                case kAcpiAstNodeOp_Or:         enmStmt = kAcpiStmt_Or;         break;
                case kAcpiAstNodeOp_Xor:        enmStmt = kAcpiStmt_Xor;        break;
                case kAcpiAstNodeOp_ShiftLeft:  enmStmt = kAcpiStmt_ShiftLeft;  break;
                case kAcpiAstNodeOp_ShiftRight: enmStmt = kAcpiStmt_ShiftRight; break;
                case kAcpiAstNodeOp_ConcatenateResTemplate: enmStmt = kAcpiStmt_ConcatenateResTemplate; break;
                default:
                    AssertReleaseFailed(); /* Impossible */
                    return VERR_INTERNAL_ERROR;
            }

            rc = RTAcpiTblStmtSimpleAppend(hAcpiTbl, enmStmt);
            if (RT_SUCCESS(rc))
                rc = rtAcpiAstDumpToTbl(pAstNd->aArgs[0].u.pAstNd, pNsRoot, hAcpiTbl);
            if (RT_SUCCESS(rc))
                rc = rtAcpiAstDumpToTbl(pAstNd->aArgs[1].u.pAstNd, pNsRoot, hAcpiTbl);
            if (RT_SUCCESS(rc))
            {
                if (pAstNd->aArgs[2].u.pAstNd)
                    rc = rtAcpiAstDumpToTbl(pAstNd->aArgs[2].u.pAstNd, pNsRoot, hAcpiTbl);
                else
                    rc = RTAcpiTblNullNameAppend(hAcpiTbl);
            }
            break;
        }
        case kAcpiAstNodeOp_EisaId:
        {
            AssertBreakStmt(   pAstNd->cArgs == 1
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_StringLiteral,
                            rc = VERR_INTERNAL_ERROR);
            rc = RTAcpiTblEisaIdAppend(hAcpiTbl, pAstNd->aArgs[0].u.pszStrLit);
            break;
        }
        case kAcpiAstNodeOp_CreateBitField:
        case kAcpiAstNodeOp_CreateByteField:
        case kAcpiAstNodeOp_CreateWordField:
        case kAcpiAstNodeOp_CreateDWordField:
        case kAcpiAstNodeOp_CreateQWordField:
        {
            AssertBreakStmt(   pAstNd->cArgs == 3
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_AstNode
                            && pAstNd->aArgs[1].enmType == kAcpiAstArgType_AstNode
                            && pAstNd->aArgs[2].enmType == kAcpiAstArgType_NameString,
                            rc = VERR_INTERNAL_ERROR);

            RTACPISTMT enmStmt;
            switch (pAstNd->enmOp)
            {
                case kAcpiAstNodeOp_CreateBitField:   enmStmt = kAcpiStmt_CreateBitField;   break;
                case kAcpiAstNodeOp_CreateByteField:  enmStmt = kAcpiStmt_CreateByteField;  break;
                case kAcpiAstNodeOp_CreateWordField:  enmStmt = kAcpiStmt_CreateWordField;  break;
                case kAcpiAstNodeOp_CreateDWordField: enmStmt = kAcpiStmt_CreateDWordField; break;
                case kAcpiAstNodeOp_CreateQWordField: enmStmt = kAcpiStmt_CreateQWordField; break;
                default:
                    AssertReleaseFailed(); /* Impossible */
                    return VERR_INTERNAL_ERROR;
            }

            rc = RTAcpiTblStmtSimpleAppend(hAcpiTbl, enmStmt);
            if (RT_SUCCESS(rc))
                rc = rtAcpiAstDumpToTbl(pAstNd->aArgs[0].u.pAstNd, pNsRoot, hAcpiTbl);
            if (RT_SUCCESS(rc))
            {
                /* Try to resolve to an integer. */
                uint64_t off = 0;
                if (pAstNd->aArgs[1].u.pAstNd->enmOp == kAcpiAstNodeOp_Number)
                    off = pAstNd->aArgs[1].u.pAstNd->u64;
                else
                {
                    rc = rtAcpiAstNodeEvaluateToInteger(pAstNd->aArgs[1].u.pAstNd, pNsRoot, true /*fResolveIdentifiers*/, &off);
                    off = pAstNd->enmOp == kAcpiAstNodeOp_CreateBitField ? off : off / 8;
                }
                if (RT_SUCCESS(rc))
                    rc = RTAcpiTblIntegerAppend(hAcpiTbl, off);
                else
                    rc = rtAcpiAstDumpToTbl(pAstNd->aArgs[1].u.pAstNd, pNsRoot, hAcpiTbl);
            }
            if (RT_SUCCESS(rc))
                rc = RTAcpiTblNameStringAppend(hAcpiTbl, pAstNd->aArgs[2].u.pszNameString);
            break;
        }
        case kAcpiAstNodeOp_External:
        default:
            AssertFailedStmt(rc = VERR_NOT_IMPLEMENTED);
    }

    AssertRC(rc);
    return rc;
}


/**
 * Adds the proper indentation before a new line.
 *
 * @returns IPRT status code.
 * @param   hVfsIos         The VFS I/O stream handle to dump the ASL to.
 * @param   uIndentLvl      The level of indentation.
 */
static int rtAcpiAstNodeFormatIndent(RTVFSIOSTREAM hVfsIos, uint32_t uIndentLvl)
{
    ssize_t cch = RTVfsIoStrmPrintf(hVfsIos, "\n");
    if (cch != 1)
        return cch < 0 ? (int)cch : VERR_BUFFER_UNDERFLOW;

    while (uIndentLvl--)
    {
        cch = RTVfsIoStrmPrintf(hVfsIos, "    ");
        if (cch != 4)
            return cch < 0 ? (int)cch : VERR_BUFFER_UNDERFLOW;
    }

    return VINF_SUCCESS;
}


static int rtAcpiAstNodeFormat(uint32_t uLvl, RTVFSIOSTREAM hVfsIos, const char *pszFmt, ...)
{
    int rc = VINF_SUCCESS;
    if (uLvl)
        rc = rtAcpiAstNodeFormatIndent(hVfsIos, uLvl);
    if (RT_SUCCESS(rc))
    {
        va_list VaArgs;
        va_start(VaArgs, pszFmt);
        ssize_t cch = RTVfsIoStrmPrintfV(hVfsIos, pszFmt, VaArgs);
        va_end(VaArgs);
        if (cch <= 0)
            rc = cch < 0 ? (int)cch : VERR_NO_MEMORY;
    }

    return rc;
}


static int rtAcpiAstDumpAstListToAsl(PCRTLISTANCHOR pLst, RTVFSIOSTREAM hVfsIosOut, uint32_t uLvl)
{
    PCRTACPIASTNODE pIt;
    RTListForEach(pLst, pIt, RTACPIASTNODE, NdAst)
    {
        int rc = rtAcpiAstDumpToAsl(pIt, hVfsIosOut, uLvl);
        if (RT_FAILURE(rc))
            return rc;
    }

    return VINF_SUCCESS;
}


/**
 * Operations encoding table, indexed by kAcpiAstNodeOp_XXX.
 */
static const char *g_apszAslOps[] =
{
    /* kAcpiAstNodeOp_Invalid                 */  NULL,
    /* kAcpiAstNodeOp_Identifier              */  NULL,
    /* kAcpiAstNodeOp_StringLiteral           */  NULL,
    /* kAcpiAstNodeOp_Number                  */  NULL,
    /* kAcpiAstNodeOp_Scope                   */  "Scope",
    /* kAcpiAstNodeOp_Processor               */  "Processor",
    /* kAcpiAstNodeOp_External                */  "External",
    /* kAcpiAstNodeOp_Method                  */  "Method",
    /* kAcpiAstNodeOp_Device                  */  "Device",
    /* kAcpiAstNodeOp_If                      */  "If",
    /* kAcpiAstNodeOp_Else                    */  "Else",
    /* kAcpiAstNodeOp_LAnd                    */  "LAnd",
    /* kAcpiAstNodeOp_LOr                     */  "LOr",
    /* kAcpiAstNodeOp_LEqual                  */  "LEqual",
    /* kAcpiAstNodeOp_LGreater                */  "LGreater",
    /* kAcpiAstNodeOp_LGreaterEqual           */  "LGreaterEqual",
    /* kAcpiAstNodeOp_LLess                   */  "LLess",
    /* kAcpiAstNodeOp_LLessEqual              */  "LLessEqual",
    /* kAcpiAstNodeOp_LNot                    */  "LNot",
    /* kAcpiAstNodeOp_LNotEqual               */  "LNotEqual",
    /* kAcpiAstNodeOp_Zero                    */  "Zero",
    /* kAcpiAstNodeOp_One                     */  "One",
    /* kAcpiAstNodeOp_Ones                    */  "Ones",
    /* kAcpiAstNodeOp_Return                  */  "Return",
    /* kAcpiAstNodeOp_Unicode                 */  "Unicode",
    /* kAcpiAstNodeOp_OperationRegion         */  "OperationRegion",
    /* kAcpiAstNodeOp_Field                   */  "Field",
    /* kAcpiAstNodeOp_Name                    */  "Name",
    /* kAcpiAstNodeOp_ResourceTemplate        */  "ResourceTemplate",
    /* kAcpiAstNodeOp_Arg0                    */  "Arg0",
    /* kAcpiAstNodeOp_Arg1                    */  "Arg1",
    /* kAcpiAstNodeOp_Arg2                    */  "Arg2",
    /* kAcpiAstNodeOp_Arg3                    */  "Arg3",
    /* kAcpiAstNodeOp_Arg4                    */  "Arg4",
    /* kAcpiAstNodeOp_Arg5                    */  "Arg5",
    /* kAcpiAstNodeOp_Arg6                    */  "Arg6",
    /* kAcpiAstNodeOp_Local0                  */  "Local0",
    /* kAcpiAstNodeOp_Local1                  */  "Local1",
    /* kAcpiAstNodeOp_Local2                  */  "Local2",
    /* kAcpiAstNodeOp_Local3                  */  "Local3",
    /* kAcpiAstNodeOp_Local4                  */  "Local4",
    /* kAcpiAstNodeOp_Local5                  */  "Local5",
    /* kAcpiAstNodeOp_Local6                  */  "Local6",
    /* kAcpiAstNodeOp_Local7                  */  "Local7",
    /* kAcpiAstNodeOp_Package                 */  "Package",
    /* kAcpiAstNodeOp_Buffer                  */  "Buffer",
    /* kAcpiAstNodeOp_ToUUid                  */  "ToUUID",
    /* kAcpiAstNodeOp_DerefOf                 */  "DerefOf",
    /* kAcpiAstNodeOp_Index                   */  "Index",
    /* kAcpiAstNodeOp_Store                   */  "Store",
    /* kAcpiAstNodeOp_Break                   */  "Break",
    /* kAcpiAstNodeOp_Continue                */  "Continue",
    /* kAcpiAstNodeOp_Add                     */  "Add",
    /* kAcpiAstNodeOp_Subtract                */  "Subtract",
    /* kAcpiAstNodeOp_Multiply                */  "Multiply",
    /* kAcpiAstNodeOp_And                     */  "And",
    /* kAcpiAstNodeOp_Nand                    */  "Nand",
    /* kAcpiAstNodeOp_Or                      */  "Or",
    /* kAcpiAstNodeOp_Xor                     */  "Xor",
    /* kAcpiAstNodeOp_ShiftLeft               */  "ShiftLeft",
    /* kAcpiAstNodeOp_ShiftRight              */  "ShiftRight",
    /* kAcpiAstNodeOp_Not                     */  "Not",
    /* kAcpiAstNodeOp_Notify                  */  "Notify",
    /* kAcpiAstNodeOp_SizeOf                  */  "SizeOf",
    /* kAcpiAstNodeOp_While                   */  "While",
    /* kAcpiAstNodeOp_Increment               */  "Increment",
    /* kAcpiAstNodeOp_Decrement               */  "Decrement",
    /* kAcpiAstNodeOp_CondRefOf               */  "CondRefOf",
    /* kAcpiAstNodeOp_IndexField              */  "IndexField",
    /* kAcpiAstNodeOp_EisaId                  */  "EisaId",
    /* kAcpiAstNodeOp_CreateField             */  "CreateField",
    /* kAcpiAstNodeOp_CreateBitField          */  "CreateBitField",
    /* kAcpiAstNodeOp_CreateByteField         */  "CreateByteField",
    /* kAcpiAstNodeOp_CreateWordField         */  "CreateWordField",
    /* kAcpiAstNodeOp_CreateDWordField        */  "CreateDWordField",
    /* kAcpiAstNodeOp_CreateQWordField        */  "CreateQWordField",
    /* kAcpiAstNodeOp_ConcatenateResTemplate  */  "ConcatenateResTemplate",
    /* kAcpiAstNodeOp_FindSetLeftBit          */  "FindSetLeftBit",
    /* kAcpiAstNodeOp_FindSetRightBit         */  "FindSetRightBit",
};


/**
 * Region space encoding table, indexed by kAcpiOperationRegionSpace_XXX.
 */
static const char *g_apszRegionSpace[] =
{
    /* kAcpiOperationRegionSpace_Invalid          */ NULL,
    /* kAcpiOperationRegionSpace_SystemMemory     */ "SystemMemory",
    /* kAcpiOperationRegionSpace_SystemIo         */ "SystemIO",
    /* kAcpiOperationRegionSpace_PciConfig        */ "PCI_Config",
    /* kAcpiOperationRegionSpace_EmbeddedControl  */ "EmbeddedControl",
    /* kAcpiOperationRegionSpace_SmBus            */ "SMBus",
    /* kAcpiOperationRegionSpace_SystemCmos       */ "SystemCMOS",
    /* kAcpiOperationRegionSpace_PciBarTarget     */ "PciBarTarget",
    /* kAcpiOperationRegionSpace_Ipmi             */ "IPMI",
    /* kAcpiOperationRegionSpace_Gpio             */ "GeneralPurposeIO",
    /* kAcpiOperationRegionSpace_GenericSerialBus */ "GenericSerialBus",
    /* kAcpiOperationRegionSpace_Pcc              */ "PCC"
};


DECLHIDDEN(const char *) rtAcpiTblAstNodeOp2Str(RTACPIASTNODEOP enmOp)
{
    return g_apszAslOps[enmOp];
}


/**
 * Field access encoding table, indexed by kAcpiFieldAcc_XXX.
 */
static const char *g_apszFieldAcc[] =
{
    /* kAcpiFieldAcc_Invalid */ NULL,
    /* kAcpiFieldAcc_Any     */ "AnyAcc",
    /* kAcpiFieldAcc_Byte    */ "ByteAcc",
    /* kAcpiFieldAcc_Word    */ "WordAcc",
    /* kAcpiFieldAcc_DWord   */ "DWordAcc",
    /* kAcpiFieldAcc_QWord   */ "QWordAcc",
    /* kAcpiFieldAcc_Buffer  */ "BufferAcc"
};


/**
 * Field udpate encoding table, indexed by kAcpiFieldUpdate_XXX.
 */
static const char *g_apszFieldUpdate[] =
{
    /* kAcpiFieldUpdate_Invalid       */ NULL,
    /* kAcpiFieldUpdate_Preserve      */ "Preserve",
    /* kAcpiFieldUpdate_WriteAsOnes   */ "WriteAsOnes",
    /* kAcpiFieldUpdate_WriteAsZeroes */ "WriteAsZeros",
};


DECLHIDDEN(int) rtAcpiAstDumpToAsl(PCRTACPIASTNODE pAstNd, RTVFSIOSTREAM hVfsIosOut, uint32_t uLvl)
{
    int rc = VINF_SUCCESS;
    switch (pAstNd->enmOp)
    {
        case kAcpiAstNodeOp_Identifier:
        {
            rc = rtAcpiAstNodeFormat(uLvl, hVfsIosOut, "%s", pAstNd->pszIde);
            if (   RT_SUCCESS(rc)
                && pAstNd->cArgs)
            {
                rc = rtAcpiAstNodeFormat(0, hVfsIosOut, "(");
                if (RT_SUCCESS(rc))
                    for (uint8_t i = 0; i < pAstNd->cArgs; i++)
                    {
                        Assert(pAstNd->aArgs[i].enmType == kAcpiAstArgType_AstNode);
                        rc = rtAcpiAstDumpToAsl(pAstNd->aArgs[i].u.pAstNd, hVfsIosOut, 0);
                        if (RT_FAILURE(rc))
                            break;

                        if (i < pAstNd->cArgs - 1)
                        {
                            rc = rtAcpiAstNodeFormat(0, hVfsIosOut, ", ");
                            if (RT_FAILURE(rc))
                                break;
                        }
                    }
                if (RT_SUCCESS(rc))
                    rc = rtAcpiAstNodeFormat(0, hVfsIosOut, ")");
            }
            break;
        }
        case kAcpiAstNodeOp_StringLiteral:
            rc = rtAcpiAstNodeFormat(uLvl, hVfsIosOut, "\"%s\"", pAstNd->pszStrLit);
            break;
        case kAcpiAstNodeOp_Number:
            rc = rtAcpiAstNodeFormat(uLvl, hVfsIosOut, "%#RX64", pAstNd->u64);
            break;
        case kAcpiAstNodeOp_Method:
        {
            AssertBreakStmt(   pAstNd->cArgs == 4
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_NameString
                            && pAstNd->aArgs[1].enmType == kAcpiAstArgType_U8
                            && pAstNd->aArgs[2].enmType == kAcpiAstArgType_Bool
                            && pAstNd->aArgs[3].enmType == kAcpiAstArgType_U8,
                            rc = VERR_INTERNAL_ERROR);
            rc = rtAcpiAstNodeFormat(uLvl, hVfsIosOut, "Method(%s, %u, %s, %u)",
                                     pAstNd->aArgs[0].u.pszNameString,
                                     pAstNd->aArgs[1].u.u8,
                                     pAstNd->aArgs[2].u.f ? "Serialized" : "NotSerialized",
                                     pAstNd->aArgs[3].u.u8);
            if (RT_SUCCESS(rc))
                rc = rtAcpiAstNodeFormat(uLvl, hVfsIosOut, "{");
            if (RT_SUCCESS(rc))
                rc = rtAcpiAstDumpAstListToAsl(&pAstNd->LstScopeNodes, hVfsIosOut, uLvl + 1);
            if (RT_SUCCESS(rc))
                rc = rtAcpiAstNodeFormat(uLvl, hVfsIosOut, "}\n");
            break;
        }
        case kAcpiAstNodeOp_Buffer:
        {
            AssertBreakStmt(   pAstNd->cArgs == 1
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_U64,
                            rc = VERR_INTERNAL_ERROR);

            rc = rtAcpiAstNodeFormat(uLvl, hVfsIosOut, "Buffer (%#RX64) {", pAstNd->aArgs[0].u.u64);
            if (RT_SUCCESS(rc))
            {
                PRTACPIASTNODE pIt;
                RTListForEach(&pAstNd->LstScopeNodes, pIt, RTACPIASTNODE, NdAst)
                {
                    AssertBreakStmt(pIt->enmOp == kAcpiAstNodeOp_Number,
                                    rc = VERR_INTERNAL_ERROR);

                    rc = rtAcpiAstNodeFormat(0, hVfsIosOut, "%#RX64", pIt->u64);
                    if (RT_FAILURE(rc))
                        break;

                    if (!RTListNodeIsLast(&pAstNd->LstScopeNodes, &pIt->NdAst))
                    {
                        rc = rtAcpiAstNodeFormat(0, hVfsIosOut, ", ");
                        if (RT_FAILURE(rc))
                            break;
                    }
                }

                if (RT_SUCCESS(rc))
                    rc = rtAcpiAstNodeFormat(uLvl, hVfsIosOut, "}");
            }
            break;
        }
        case kAcpiAstNodeOp_Field:
        case kAcpiAstNodeOp_IndexField:
        {
            if (pAstNd->enmOp == kAcpiAstNodeOp_Field)
            {
                AssertBreakStmt(   pAstNd->cArgs == 4
                                && pAstNd->aArgs[0].enmType == kAcpiAstArgType_NameString
                                && pAstNd->aArgs[1].enmType == kAcpiAstArgType_FieldAcc
                                && pAstNd->aArgs[2].enmType == kAcpiAstArgType_Bool
                                && pAstNd->aArgs[3].enmType == kAcpiAstArgType_FieldUpdate,
                                rc = VERR_INTERNAL_ERROR);

                rc = rtAcpiAstNodeFormat(uLvl, hVfsIosOut,
                                         "Field(%s, %s, %s, %s)",
                                         pAstNd->aArgs[0].u.pszNameString,
                                         g_apszFieldAcc[pAstNd->aArgs[1].u.enmFieldAcc],
                                         pAstNd->aArgs[2].u.f ? "Lock" : "NoLock",
                                         g_apszFieldUpdate[pAstNd->aArgs[3].u.enmFieldUpdate]);
            }
            else
            {
                AssertBreakStmt(   pAstNd->cArgs == 5
                                && pAstNd->aArgs[0].enmType == kAcpiAstArgType_NameString
                                && pAstNd->aArgs[1].enmType == kAcpiAstArgType_NameString
                                && pAstNd->aArgs[2].enmType == kAcpiAstArgType_FieldAcc
                                && pAstNd->aArgs[3].enmType == kAcpiAstArgType_Bool
                                && pAstNd->aArgs[4].enmType == kAcpiAstArgType_FieldUpdate,
                                rc = VERR_INTERNAL_ERROR);

                rc = rtAcpiAstNodeFormat(uLvl, hVfsIosOut,
                                         "IndexField(%s, %s, %s, %s, %s)",
                                         pAstNd->aArgs[0].u.pszNameString,
                                         pAstNd->aArgs[1].u.pszNameString,
                                         g_apszFieldAcc[pAstNd->aArgs[2].u.enmFieldAcc],
                                         pAstNd->aArgs[3].u.f ? "Lock" : "NoLock",
                                         g_apszFieldUpdate[pAstNd->aArgs[4].u.enmFieldUpdate]);
            }

            if (RT_SUCCESS(rc))
                rc = rtAcpiAstNodeFormat(uLvl, hVfsIosOut, "{");
            if (RT_SUCCESS(rc))
            {
                for (uint32_t i = 0; i < pAstNd->Fields.cFields; i++)
                {
                    PCRTACPIFIELDENTRY pField = &pAstNd->Fields.paFields[i];

                    if (pField->pszName)
                        rc = rtAcpiAstNodeFormat(uLvl + 1, hVfsIosOut, "%s, %RU32,", pField->pszName, pField->cBits / 8);
                    else
                        rc = rtAcpiAstNodeFormat(uLvl + 1, hVfsIosOut, "Offset(%#RX32),", pField->cBits / 8);

                    if (RT_FAILURE(rc))
                        break;
                }
            }

            if (RT_SUCCESS(rc))
                rc = rtAcpiAstNodeFormat(uLvl, hVfsIosOut, "}");

            break;
        }
        case kAcpiAstNodeOp_Package:
        {
            AssertBreakStmt(   pAstNd->cArgs == 1
                            && pAstNd->aArgs[0].enmType == kAcpiAstArgType_U8,
                            rc = VERR_INTERNAL_ERROR);

            rc = rtAcpiAstNodeFormat(uLvl, hVfsIosOut, "Package(%RU8)", pAstNd->aArgs[0].u.u8);
            if (RT_SUCCESS(rc))
                rc = rtAcpiAstNodeFormat(uLvl, hVfsIosOut, "{");
            if (RT_SUCCESS(rc))
            {
                PCRTACPIASTNODE pIt;
                RTListForEach(&pAstNd->LstScopeNodes, pIt, RTACPIASTNODE, NdAst)
                {
                    rc = rtAcpiAstDumpToAsl(pIt, hVfsIosOut, uLvl ? uLvl + 1 : 0);
                    if (RT_FAILURE(rc))
                        return rc;

                    if (!RTListNodeIsLast(&pAstNd->LstScopeNodes, &pIt->NdAst))
                    {
                        rc = rtAcpiAstNodeFormat(uLvl ? uLvl + 1 : 0, hVfsIosOut, ",");
                        if (RT_FAILURE(rc))
                            return rc;
                    }
                }
            }

            if (RT_SUCCESS(rc))
                rc = rtAcpiAstNodeFormat(uLvl, hVfsIosOut, "}");

            break;
        }
        default:
        {
            /* Generic case for most of the types. */
            const char *pszOp = g_apszAslOps[pAstNd->enmOp];
            if (pAstNd->enmOp == kAcpiAstNodeOp_Identifier)
                pszOp = pAstNd->pszIde;

            rc = rtAcpiAstNodeFormat(uLvl, hVfsIosOut, "%s", pszOp);
            if (   RT_SUCCESS(rc)
                && pAstNd->cArgs)
            {
                rc = rtAcpiAstNodeFormat(0, hVfsIosOut, "(");
                if (RT_SUCCESS(rc))
                {
                    for (uint8_t i = 0; i < pAstNd->cArgs; i++)
                    {
                        switch (pAstNd->aArgs[i].enmType)
                        {
                            case kAcpiAstArgType_AstNode:
                                if (pAstNd->aArgs[i].u.pAstNd)
                                    rc = rtAcpiAstDumpToAsl(pAstNd->aArgs[i].u.pAstNd, hVfsIosOut, 0);
                                break;
                            case kAcpiAstArgType_NameString:
                                rc = rtAcpiAstNodeFormat(0, hVfsIosOut, "%s", pAstNd->aArgs[i].u.pszNameString);
                                break;
                            case kAcpiAstArgType_Bool:
                                rc = rtAcpiAstNodeFormat(0, hVfsIosOut, "%s", pAstNd->aArgs[i].u.f ? "True" : "False");
                                break;
                            case kAcpiAstArgType_U8:
                                rc = rtAcpiAstNodeFormat(0, hVfsIosOut, "%RU8", pAstNd->aArgs[i].u.u8);
                                break;
                            case kAcpiAstArgType_U16:
                                rc = rtAcpiAstNodeFormat(0, hVfsIosOut, "%#RX8", pAstNd->aArgs[i].u.u16);
                                break;
                            case kAcpiAstArgType_U32:
                                rc = rtAcpiAstNodeFormat(0, hVfsIosOut, "%#RX32", pAstNd->aArgs[i].u.u32);
                                break;
                            case kAcpiAstArgType_U64:
                                rc = rtAcpiAstNodeFormat(0, hVfsIosOut, "%#RX64", pAstNd->aArgs[i].u.u64);
                                break;
                            case kAcpiAstArgType_StringLiteral:
                                rc = rtAcpiAstNodeFormat(0, hVfsIosOut, "%s", pAstNd->aArgs[i].u.pszStrLit);
                                break;
                            case kAcpiAstArgType_RegionSpace:
                                rc = rtAcpiAstNodeFormat(0, hVfsIosOut, "%s", g_apszRegionSpace[pAstNd->aArgs[i].u.enmRegionSpace]);
                                break;
                            case kAcpiAstArgType_ObjType:
                            case kAcpiAstArgType_FieldAcc:
                            case kAcpiAstArgType_FieldUpdate:
                            default:
                                AssertReleaseFailed();
                        }

                        if (RT_FAILURE(rc))
                            break;

                        if (   (i < pAstNd->cArgs - 1)
                            && (   pAstNd->aArgs[i + 1].enmType != kAcpiAstArgType_AstNode
                                || pAstNd->aArgs[i + 1].u.pAstNd))
                        {
                            rc = rtAcpiAstNodeFormat(0, hVfsIosOut, ", ");
                            if (RT_FAILURE(rc))
                                break;
                        }
                    }
                }
                if (RT_SUCCESS(rc))
                    rc = rtAcpiAstNodeFormat(0, hVfsIosOut, ")");
            }

            if (   RT_SUCCESS(rc)
                && (pAstNd->fFlags & RTACPI_AST_NODE_F_NEW_SCOPE))
            {
                rc = rtAcpiAstNodeFormat(uLvl, hVfsIosOut, "{");
                if (RT_SUCCESS(rc))
                    rc = rtAcpiAstDumpAstListToAsl(&pAstNd->LstScopeNodes, hVfsIosOut, uLvl + 1);
                if (RT_SUCCESS(rc))
                    rc = rtAcpiAstNodeFormat(uLvl, hVfsIosOut, "}\n");
            }
        }
    }

    AssertRC(rc);
    return rc;
}
