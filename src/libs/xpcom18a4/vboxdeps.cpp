/* $Id: vboxdeps.cpp 112399 2026-01-11 18:46:00Z knut.osmundsen@oracle.com $ */
/** @file
 * XPCOM - The usual story: drag stuff from the libraries into the link.
 */

/*
 * Copyright (C) 2007-2026 Oracle and/or its affiliates.
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


#include <nsDeque.h>
#include <nsHashSets.h>
#include <xptcall.h>
#include <nsProxyRelease.h>
#include "xpcom/proxy/src/nsProxyEventPrivate.h"
#include "nsTraceRefcnt.h"
#include "nsDebug.h"
#include "nsString.h"

uintptr_t deps[] =
{
    (uintptr_t)PL_HashString,
    (uintptr_t)NS_ProxyRelease,
    (uintptr_t)nsTraceRefcnt::LogRelease,
    (uintptr_t)nsDebug::Assertion,
    0
};

class foobardep : public nsXPTCStubBase
{
public:
    NS_IMETHOD_(nsrefcnt) AddRef(void)
    {
        return 1;
    }

    NS_IMETHOD_(nsrefcnt) Release(void)
    {
        return 0;
    }

    NS_IMETHOD GetInterfaceInfo(nsIInterfaceInfo** info)
    {
        (void)info;
        return 0;
    }

    // call this method and return result
    NS_IMETHOD CallMethod(PRUint16 methodIndex, const nsXPTMethodInfo* info, nsXPTCMiniVariant* params)
    {
        (void)methodIndex;
        (void)info;
        (void)params;
        return 0;
    }

};



void foodep(void)
{
    nsVoidHashSetSuper *a = new nsVoidHashSetSuper();
    a->Init(123);
    nsDeque *b = new nsDeque(); RT_NOREF(b);

    //nsXPTCStubBase
    nsProxyEventObject *c = new nsProxyEventObject();
    c->Release();

    foobardep *d = new foobardep();
    nsXPTCStubBase *e = d;
    e->Release();

    // Dragged in by TestCRT.
    nsAutoString t1;
    t1.AssignWithConversion(NULL);
}

