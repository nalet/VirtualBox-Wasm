/* $Id: APICKvmInternal.h 112727 2026-01-28 17:02:49Z alexander.eichner@oracle.com $ */
/** @file
 * APIC - Advanced Programmable Interrupt Controller - NEM Hyper-V backend, Internal header.
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
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef VMM_INCLUDED_SRC_include_APICKvmInternal_h
#define VMM_INCLUDED_SRC_include_APICKvmInternal_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

#include <VBox/apic.h>
#include <VBox/vmm/pdmapic.h>
#include <VBox/vmm/stam.h>


/** @defgroup grp_apic_kvm_int       Internal
 * @ingroup grp_apic
 * @internal
 * @{
 */

#ifdef VBOX_INCLUDED_vmm_pdmapic_h
/** The NEM KVM APIC backend. */
extern const PDMAPICBACKEND g_ApicNemBackend;
#endif

/** The APIC hardware version we are emulating. */
#define XAPIC_HARDWARE_VERSION                  XAPIC_HARDWARE_VERSION_P4

#if XAPIC_HARDWARE_VERSION == XAPIC_HARDWARE_VERSION_P4
#define XAPIC_SVR_VALID                         XAPIC_SVR_VALID_P4
#define XAPIC_ID_BROADCAST_MASK                 XAPIC_ID_BROADCAST_MASK_P4
#else
# error "Implement Pentium and P6 family APIC architectures"
#endif

#define VMCPU_TO_XAPICPAGE(a_pVCpu)             ((PXAPICPAGE)(CTX_SUFF((a_pVCpu)->apic.s.pvApicPage)))
#define VMCPU_TO_CXAPICPAGE(a_pVCpu)            ((PCXAPICPAGE)(CTX_SUFF((a_pVCpu)->apic.s.pvApicPage)))

#define VMCPU_TO_X2APICPAGE(a_pVCpu)            ((PX2APICPAGE)(CTX_SUFF((a_pVCpu)->apic.s.pvApicPage)))
#define VMCPU_TO_CX2APICPAGE(a_pVCpu)           ((PCX2APICPAGE)(CTX_SUFF((a_pVCpu)->apic.s.pvApicPage)))

#define VMCPU_TO_KVMAPICCPU(a_pVCpu)             (&(a_pVCpu)->apic.s)
#define VM_TO_KVMAPIC(a_pVM)                     (&(a_pVM)->apic.s)
#define VM_TO_KVMAPICDEV(a_pVM)                  CTX_SUFF(VM_TO_APIC(a_pVM)->pApicDev)
#ifdef IN_RING3
# define VMCPU_TO_DEVINS(a_pVCpu)               ((a_pVCpu)->pVMR3->apic.s.pDevInsR3)
#elif defined(IN_RING0)
# define VMCPU_TO_DEVINS(a_pVCpu)               ((a_pVCpu)->pGVM->apicr0.s.pDevInsR0)
#endif

#define KVMAPICCPU_TO_KVMXAPICPAGE(a_ApicCpu)     ((PXAPICPAGE)(CTX_SUFF((a_ApicCpu)->pvApicPage)))
#define KVMAPICCPU_TO_CKVMXAPICPAGE(a_ApicCpu)    ((PCXAPICPAGE)(CTX_SUFF((a_ApicCpu)->pvApicPage)))

/**
 * KVM APIC VM instance data.
 */
typedef struct KVMAPIC
{
    /** The ring-3 device instance. */
    PPDMDEVINSR3                pDevInsR3;
    /** The VM file descriptor. */
    int                         iFdVm;
    /** The MSI routes required for KVM properly handling IO-APIC pin based interrupts.
     * Required for proper EOI handling. */
    struct kvm_irq_routing      *pMsiRoutes;

    /** @name Miscellaneous data.
     * @{ */
    /** Flag whether an IO-APIC is present in the system. */
    bool                        fIoApicPresent;
    /** Whether Hyper-V x2APIC compatibility mode is enabled. */
    bool                        fHyperVCompatMode;
    /** Whether TSC-deadline timer mode is supported for the guest. */
    bool                        fSupportsTscDeadline;
    /** The max supported APIC mode from CFGM.  */
    PDMAPICMODE                 enmMaxMode;
    /** @} */
} KVMAPIC;
/** Pointer to KVM APIC VM instance data. */
typedef KVMAPIC *PKVMAPIC;
/** Pointer to const KVM APIC VM instance data. */
typedef KVMAPIC const *PCKVMAPIC;

/**
 * KVM APIC VMCPU instance data.
 */
typedef struct KVMAPICCPU
{
    /** The APIC page virtual address - R3 ptr. */
    R3PTRTYPE(void *)           pvApicPageR3;
    /** The APIC page passed to and used by KVM - R3 ptr. */
    R3PTRTYPE(void *)           pvKvmPageR3;

    /** The APIC base MSR.*/
    uint64_t                    uApicBaseMsr;
    /** The error status register's internal state. */
    uint32_t                    uEsrInternal;
    /** The vCPU file descriptor. */
    int                         iFdVCpu;

    /** @name APIC statistics.
     * @{ */
#ifdef VBOX_WITH_STATISTICS
    /** Number of times the one-shot timer is invoked. */
    STAMCOUNTER                 StatTimerOneShot;
    /** Number of times the periodic timer is invoked. */
    STAMCOUNTER                 StatTimerPeriodic;
    /** Number of times the TSC deadline timer is invoked. */
    STAMCOUNTER                 StatTimerTscDeadline;
#endif
    /** @} */
} KVMAPICCPU;
/** Pointer to KVM APIC VMCPU instance data. */
typedef KVMAPICCPU *PKVMAPICCPU;
/** Pointer to a const KVM APIC VMCPU instance data. */
typedef KVMAPICCPU const *PCKVMAPICCPU;

/** @} */

#endif /* !VMM_INCLUDED_SRC_include_APICKvmInternal_h */

