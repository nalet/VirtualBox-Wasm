/* $Id: 03-grammar.asl 112663 2026-01-21 15:02:23Z alexander.eichner@oracle.com $ */
/** @file
 * VirtualBox ACPI - Testcase.
 */

/*
 * Copyright (C) 2026 Oracle and/or its affiliates.
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

DefinitionBlock ("", "SSDT", 1, "VBOX  ", "VBOXTPMT", 2)
{
    Scope (_SB_)
    {
        OperationRegion(REGI, SystemMemory, 0xdeadc0de, 0x1000)

        Field(TPMR, AnyAcc, NoLock, Preserve)
        {
            Offset(0x30),
            IFID,       1,
        }

        IndexField (IDX0, DAT0, DwordAcc, NoLock, Preserve)
        {
            STAT, 32,
            PRAT, 32,
            RCAP, 32,
            PVOL, 32,

            UNIT, 32,
            DCAP, 32,
            LFCP, 32,
            BTEC, 32,
            DVOL, 32,
            DWRN, 32,
            DLOW, 32,
            GRN1, 32,
            GRN2, 32,

            BSTA, 32,
            APSR, 32,
        }

        Name(PACK, Package (3) { Zero, One, 0x2})

        Device (DUT_)
        {
            Method (TEST, 7, NotSerialized, 0)
            {
                If (LEqual(IFID, One))
                {
                    Return ("PNP0C31")
                }
                Else
                {
                    Return ("MSFT0101")
                }

                And(Arg0, Arg1, Arg2)
                Or(Arg0, Arg1, Arg2)
                Xor(Arg0, Arg1, Arg2)
                Nand(Local0, Local1, Local2)
                ShiftLeft(Arg0, Arg1, Arg2)
                ShiftRight(Arg0, Arg1, Arg2)

                Index(Local0, Local1, Local2)
                Add(Local0, Local1, Local2)
                Subtract(Local0, Local1, Local2)
                Multiply(Local7, Arg6, Arg5)

                Store(Local0, Local7)
                Increment(Local7)
                Decrement(Local6)

                Notify(DUT_, 0xbeefca5e)

                CreateWordField (CRS_, TPMR, MIN1)
                CreateDWordField(CRS_, TPMR, MIN2)
                CreateQWordField(CRS_, TPMR, MIN3)
                CreateByteField (CRS_, TPMR, MIN4)
                CreateBitField  (CRS_, TPMR, MIN5)

                While (LAnd(LOr(LEqual(Local0, Arg3),
                                LEqual(Local1, Arg4)),
                            LEqual(Local3, Arg6)))
                {
                    If (LGreater(Local2, 0x3))
                    {
                        Continue
                    }

                    If (LLess(Local4, Add(0x100, Arg5)))
                    {
                        Break
                    }

                    Return (Name(C0DE, Buffer (100) { 0x1, 0x2, 0xaa }))
                }
            }

            Method(SLEN, 1, NotSerialized, 0)
            {
                Store(Arg0, Not(Local0))
                FindSetLeftBit(Arg0, Local1)
                FindSetRightBit(Arg0, Local2)
                ConcatenateResTemplate (CRS_, TOM_, Local2)
                Return(Sizeof(Local0))
            }
        }
    }
}

/*
 * Local Variables:
 * comment-start: "//"
 * End:
 */

