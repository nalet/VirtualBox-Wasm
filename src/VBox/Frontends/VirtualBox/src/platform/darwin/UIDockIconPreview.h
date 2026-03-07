/* $Id: UIDockIconPreview.h 112818 2026-02-04 13:57:23Z sergey.dubov@oracle.com $ */
/** @file
 * VBox Qt GUI - UIDockIconPreview class declaration.
 */

/*
 * Copyright (C) 2009-2026 Oracle and/or its affiliates.
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

#ifndef FEQT_INCLUDED_SRC_platform_darwin_UIDockIconPreview_h
#define FEQT_INCLUDED_SRC_platform_darwin_UIDockIconPreview_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

/* Other VBox includes: */
#include <VBox/VBoxCocoa.h>

/* External includes: */
#include <ApplicationServices/ApplicationServices.h>

/* Forward declarations: */
class QPixmap;
class UIDockIconPreviewPrivate;
class UIFrameBuffer;
class UIMachine;

/** Class wrapping Cocoa dock icon preview interface. */
class UIDockIconPreview
{
public:

    /** Constructs Cocoa dock icon preview.
      * @param  pMachine      Brings the machine this preview being created for.
      * @param  overlayImage  Brings the overlayImage to use for a case when no preview available atm. */
    UIDockIconPreview(UIMachine *pMachine, const QPixmap &overlayImage);
    /** Destructs Cocoa dock icon preview. */
    virtual ~UIDockIconPreview();

    /** Updates dock overlay. */
    void updateDockOverlay();
    /** Updates dock preview with passed @a vmImage. */
    void updateDockPreview(CGImageRef vmImage);
    /** Updates dock preview with contents of passed @a pFrameBuffer. */
    void updateDockPreview(UIFrameBuffer *pFrameBuffer);

    /** Resize dock overlay to passed @a iWidth x @a iHeight size. */
    void setOriginalSize(int iWidth, int iHeight);

private:

    /** Holds the private data instance. */
    UIDockIconPreviewPrivate *d;
};

/** A class holding private data for Cocoa dock icon preview. */
class UIDockIconPreviewHelper
{
public:

    /** Constructs private data for Cocoa dock icon preview.
      * @param  pMachine      Brings the machine this preview being created for.
      * @param  overlayImage  Brings the overlayImage to use for a case when no preview available atm. */
    UIDockIconPreviewHelper(UIMachine *pMachine, const QPixmap &overlayImage);
    /** Destructs private data for Cocoa dock icon preview. */
    virtual ~UIDockIconPreviewHelper();

    /** Returns windows ID for current preview. */
    void *currentPreviewWindowId() const;

    /** Flips passed @a rect.
      * @note Flipping is necessary cause the drawing context in Mac OS X is flipped by 180 degree. */
    CGRect flipRect(CGRect rect) const;
    /** Centers passed @a rect according to @a m_dockIconRect. */
    CGRect centerRect(CGRect rect) const;
    /** Centers passed @a rect according to @a toRect. */
    CGRect centerRectTo(CGRect rect, const CGRect& toRect) const;

    /** Performs preview image initialization. */
    void initPreviewImages();
    /** Draws overlay icons within passed @a context. */
    void drawOverlayIcons(CGContextRef context);

    /** Holds the machine reference this preview being created for. */
    UIMachine *m_pMachine;

    /** Holds the default dock icon rectangle. */
    const CGRect  m_dockIconRect;

    /** Holds the overlay image instance. */
    CGImageRef  m_overlayImage;
    /** Holds the dock monitor instance. */
    CGImageRef  m_dockMonitor;
    /** Holds the glossy dock monitor instance. */
    CGImageRef  m_dockMonitorGlossy;

    /** Holds the update rectangle. */
    CGRect  m_updateRect;
    /** Holds the monitor rectangle. */
    CGRect  m_monitorRect;
};

#endif /* !FEQT_INCLUDED_SRC_platform_darwin_UIDockIconPreview_h */
