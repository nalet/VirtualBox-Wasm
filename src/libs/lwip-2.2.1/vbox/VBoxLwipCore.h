/* $Id: VBoxLwipCore.h 112885 2026-02-09 09:34:33Z knut.osmundsen@oracle.com $ */

/** @file
 * VBox Lwip Core Initiatetor/Finilizer.
 */

/*
 * Copyright (C) 2012-2026 Oracle and/or its affiliates.
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

#ifndef VBOX_INCLUDED_SRC_vbox_VBoxLwipCore_h
#define VBOX_INCLUDED_SRC_vbox_VBoxLwipCore_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

int vboxLwipCoreInitialize(PFNRT1 pfnCallback, void *pvCallbackArg);
void vboxLwipCoreFinalize(PFNRT1 pfnCallback, void *pvCallbackArg);

#endif /* !VBOX_INCLUDED_SRC_vbox_VBoxLwipCore_h */
