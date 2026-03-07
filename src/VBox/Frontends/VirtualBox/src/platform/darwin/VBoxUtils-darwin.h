/* $Id: VBoxUtils-darwin.h 112827 2026-02-04 19:02:33Z sergey.dubov@oracle.com $ */
/** @file
 * VBox Qt GUI - Declarations of utility classes and functions for handling Darwin specific tasks.
 */

/*
 * Copyright (C) 2010-2026 Oracle and/or its affiliates.
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

#ifndef FEQT_INCLUDED_SRC_platform_darwin_VBoxUtils_darwin_h
#define FEQT_INCLUDED_SRC_platform_darwin_VBoxUtils_darwin_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

/* Qt includes: */
#include <QEvent>
#include <QString>

/* GUI includes: */
#include "UILibraryDefs.h"

/* Other VBox includes: */
#include <VBox/VBoxCocoa.h>

/* External includes: */
#define UInt UInt_not_needed // libkern/OSTypes.h defines it without any code needing it, causing trouble with Qt's use of this type for different purpose!!
#include <ApplicationServices/ApplicationServices.h>
#undef UInt
#undef PVM // Stupid, stupid apple headers (sys/param.h)!!

/* Forward declarations: */
class QImage;
class QToolBar;
class QPixmap;
class QWidget;

/* Cocoa declarations: */
ADD_COCOA_NATIVE_REF(NSButton);
ADD_COCOA_NATIVE_REF(NSImage);
ADD_COCOA_NATIVE_REF(NSString);
ADD_COCOA_NATIVE_REF(NSView);
ADD_COCOA_NATIVE_REF(NSWindow);

/** Standard window button types. */
enum StandardWindowButtonType
{
    StandardWindowButtonType_Close,            // Since OS X 10.2
    StandardWindowButtonType_Miniaturize,      // Since OS X 10.2
    StandardWindowButtonType_Zoom,             // Since OS X 10.2
    StandardWindowButtonType_Toolbar,          // Since OS X 10.2
    StandardWindowButtonType_DocumentIcon,     // Since OS X 10.2
    StandardWindowButtonType_DocumentVersions, // Since OS X 10.7
    StandardWindowButtonType_FullScreen        // Since OS X 10.7
};

/** Special mouse grab even for native macOS mouse handler. */
class UIGrabMouseEvent : public QEvent
{
public:

    enum { GrabMouseEvent = QEvent::User + 200 };

    UIGrabMouseEvent(QEvent::Type type,
                     Qt::MouseButton button,
                     Qt::MouseButtons buttons,
                     int x,
                     int y,
                     int wheelDelta,
                     Qt::Orientation o)
      : QEvent((QEvent::Type)GrabMouseEvent)
      , m_type(type)
      , m_button(button)
      , m_buttons(buttons)
      , m_x(x)
      , m_y(y)
      , m_wheelDelta(wheelDelta)
      , m_orientation(o)
    {}

    QEvent::Type mouseEventType() const { return m_type; }
    Qt::MouseButton button() const { return m_button; }
    Qt::MouseButtons buttons() const { return m_buttons; }
    int xDelta() const { return m_x; }
    int yDelta() const { return m_y; }
    int wheelDelta() const { return m_wheelDelta; }
    Qt::Orientation orientation() const { return m_orientation; }

private:

    QEvent::Type      m_type;
    Qt::MouseButton   m_button;
    Qt::MouseButtons  m_buttons;
    int               m_x;
    int               m_y;
    int               m_wheelDelta;
    Qt::Orientation   m_orientation;
};


/*********************************************************************************************************************************
 *
 * Internal .h/.mm functions which are used by the external .h/.cpp public API
 *
 ********************************************************************************************************************************/

RT_C_DECLS_BEGIN
// Here we'll add only impl .h/.mm functions which have public .h/.cpp API

/********************************************************************************
 * General functionality (OS System native)
 ********************************************************************************/
SHARED_LIBRARY_STUFF bool darwinCreateMachineShortcut(NativeNSStringRef pstrSrcFile,
                                                      NativeNSStringRef pstrDstPath,
                                                      NativeNSStringRef pstrName,
                                                      NativeNSStringRef pstrUuid);
SHARED_LIBRARY_STUFF bool darwinOpenInFileManager(NativeNSStringRef pstrFile);

/********************************************************************************
 * Window/View management (OS System native)
 ********************************************************************************/
NativeNSWindowRef darwinToNativeWindowImpl(NativeNSViewRef pView);
NativeNSViewRef darwinToNativeViewImpl(NativeNSWindowRef pWindow);

/********************************************************************************
 * Simple setter methods (OS System native)
 ********************************************************************************/
void darwinSetHidesAllTitleButtonsImpl(NativeNSWindowRef pWindow);
void darwinSetShowsToolbarButtonImpl(NativeNSWindowRef pWindow, bool fEnabled);
void darwinSetWindowLabelImpl(NativeNSWindowRef pWindow, NativeNSImageRef pImage, double dDpr);
void darwinSetWindowHasShadowImpl(NativeNSWindowRef pWindow, bool fEnabled);

/********************************************************************************
 * Simple helper methods (OS System native)
 ********************************************************************************/
NativeNSButtonRef darwinNativeButtonOfWindowImpl(NativeNSWindowRef pWindow, StandardWindowButtonType enmButtonType);
int darwinWindowTitleHeightImpl(NativeNSWindowRef pWindow);
bool darwinIsWindowMaximizedImpl(NativeNSWindowRef pWindow);
void darwinEnableFullscreenSupportImpl(NativeNSWindowRef pWindow);
void darwinEnableTransienceSupportImpl(NativeNSWindowRef pWindow);
void darwinToggleFullscreenModeImpl(NativeNSWindowRef pWindow);
void darwinToggleWindowZoomImpl(NativeNSWindowRef pWindow);
bool darwinIsInFullscreenModeImpl(NativeNSWindowRef pWindow);
bool darwinIsOnActiveSpaceImpl(NativeNSWindowRef pWindow);

// Finishing adding impl .h/.mm functions which have public .h/.cpp API
RT_C_DECLS_END

/********************************************************************************
 * Simple helper methods (OS System native)
 ********************************************************************************/
bool darwinMouseGrabEvents(const void *pvCocoaEvent, const void *pvCarbonEvent, void *pvUser);


/*********************************************************************************************************************************
 *
 * External .h/.mm functions which are used by the external code directly
 *
 ********************************************************************************************************************************/

/********************************************************************************
 * General functionality (OS System native)
 ********************************************************************************/
SHARED_LIBRARY_STUFF void *darwinCocoaToCarbonEvent(void *pvCocoaEvent);
SHARED_LIBRARY_STUFF NativeNSStringRef darwinToNativeString(const char* pcszString);
QString darwinFromNativeString(NativeNSStringRef pString);

/********************************************************************************
 * Simple setter methods (OS System native)
 ********************************************************************************/
SHARED_LIBRARY_STUFF void darwinSetMouseCoalescingEnabled(bool fEnabled);

/********************************************************************************
 * Simple helper methods (OS System native)
 ********************************************************************************/
SHARED_LIBRARY_STUFF void darwinForceActiveFocus();
SHARED_LIBRARY_STUFF bool darwinScreensHaveSeparateSpaces();
SHARED_LIBRARY_STUFF bool darwinIsScrollerStyleOverlay();
SHARED_LIBRARY_STUFF bool darwinIsApplicationCommand(const void *pvCocoaEvent);
int darwinWindowToolBarHeight(NativeNSWindowRef pWindow);
void darwinRetranslateAppMenu();

/********************************************************************************
 * Graphics stuff (OS System native)
 ********************************************************************************/
SHARED_LIBRARY_STUFF NativeNSImageRef darwinToNSImageRef(const CGImageRef pImage);
SHARED_LIBRARY_STUFF NativeNSImageRef darwinToNSImageRef(const QImage *pImage);
SHARED_LIBRARY_STUFF NativeNSImageRef darwinToNSImageRef(const QPixmap *pPixmap);
SHARED_LIBRARY_STUFF NativeNSImageRef darwinToNSImageRef(const char *pczSource);


/*********************************************************************************************************************************
 *
 * External .h/.cpp functions which are used by the external code directly
 *
 ********************************************************************************************************************************/

DECLINLINE(CGRect) darwinFlipCGRect(CGRect aRect, double aTargetHeight)
{
    aRect.origin.y = aTargetHeight - aRect.origin.y - aRect.size.height;
    return aRect;
}

DECLINLINE(CGRect) darwinFlipCGRect(CGRect aRect, const CGRect &aTarget)
{
    return darwinFlipCGRect(aRect, aTarget.size.height);
}

DECLINLINE(CGRect) darwinCenterRectTo(CGRect aRect, const CGRect& aToRect)
{
    aRect.origin.x = aToRect.origin.x + (aToRect.size.width  - aRect.size.width)  / 2.0;
    aRect.origin.y = aToRect.origin.y + (aToRect.size.height - aRect.size.height) / 2.0;
    return aRect;
}

/********************************************************************************
 * Window/View management (Qt Wrapper)
 ********************************************************************************/
NativeNSViewRef darwinToNativeView(QWidget *pWidget);
NativeNSWindowRef darwinToNativeWindow(QWidget *pWidget);
NativeNSViewRef darwinToNativeView(NativeNSWindowRef pWindow);
NativeNSWindowRef darwinToNativeWindow(NativeNSViewRef pView);
NativeNSButtonRef darwinNativeButtonOfWindow(QWidget *pWidget, StandardWindowButtonType enmButtonType);

/********************************************************************************
 * Simple setter methods (Qt Wrapper)
 ********************************************************************************/
SHARED_LIBRARY_STUFF bool darwinSetFrontMostProcess();
SHARED_LIBRARY_STUFF void darwinSetHidesAllTitleButtons(QWidget *pWidget);
void darwinSetShowsToolbarButton(QToolBar *aToolBar, bool fEnabled);
SHARED_LIBRARY_STUFF void darwinSetWindowLabel(QWidget *pWidget, QPixmap *pPixmap);
SHARED_LIBRARY_STUFF void darwinSetWindowHasShadow(QWidget *pWidget, bool fEnabled);
SHARED_LIBRARY_STUFF void darwinDisableIconsInMenus();

/********************************************************************************
 * Simple helper methods (Qt Wrapper)
 ********************************************************************************/
SHARED_LIBRARY_STUFF uint64_t darwinGetCurrentProcessId();
SHARED_LIBRARY_STUFF QString darwinResolveAlias(const QString &strFile);
SHARED_LIBRARY_STUFF int darwinWindowTitleHeight(QWidget *pWidget);
SHARED_LIBRARY_STUFF bool darwinIsWindowMaximized(QWidget *pWidget);
SHARED_LIBRARY_STUFF void darwinEnableFullscreenSupport(QWidget *pWidget);
SHARED_LIBRARY_STUFF void darwinEnableTransienceSupport(QWidget *pWidget);
SHARED_LIBRARY_STUFF void darwinToggleFullscreenMode(QWidget *pWidget);
SHARED_LIBRARY_STUFF void darwinToggleWindowZoom(QWidget *pWidget);
SHARED_LIBRARY_STUFF bool darwinIsInFullscreenMode(QWidget *pWidget);
SHARED_LIBRARY_STUFF bool darwinIsOnActiveSpace(QWidget *pWidget);
int darwinWindowToolBarHeight(QWidget *pWidget);
QString darwinSystemLanguage(void);
SHARED_LIBRARY_STUFF void darwinMouseGrab(QWidget *pWidget);
SHARED_LIBRARY_STUFF void darwinMouseRelease(QWidget *pWidget);
void darwinSendMouseGrabEvents(QWidget *pWidget, int type, int button, int buttons, int x, int y);

/********************************************************************************
 * Graphics stuff (Qt Wrapper)
 ********************************************************************************/
SHARED_LIBRARY_STUFF CGImageRef darwinToCGImageRef(const QImage *pImage);
SHARED_LIBRARY_STUFF CGImageRef darwinToCGImageRef(const QPixmap *pPixmap);
SHARED_LIBRARY_STUFF CGImageRef darwinToCGImageRef(const char *pczSource);

#endif /* !FEQT_INCLUDED_SRC_platform_darwin_VBoxUtils_darwin_h */
