/* $Id: VBoxUtils-darwin.cpp 112804 2026-02-03 11:46:00Z sergey.dubov@oracle.com $ */
/** @file
 * VBox Qt GUI - Utility Classes and Functions specific to Darwin.
 */

/*
 * Copyright (C) 2006-2026 Oracle and/or its affiliates.
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

/* Qt includes: */
#include <QApplication>
#include <QPixmap>
#include <QToolBar>
#include <QWidget>

/* GUI includes: */
#include "VBoxUtils-darwin.h"
#include "UICocoaApplication.h"

/* Other VBox includes: */
#include <iprt/mem.h>
#include <iprt/assert.h>

/* Carbon includes: */
#include <Carbon/Carbon.h>


NativeNSViewRef darwinToNativeView(QWidget *pWidget)
{
    if (pWidget)
        return reinterpret_cast<NativeNSViewRef>(pWidget->winId());
    return nil;
}

NativeNSWindowRef darwinToNativeWindow(QWidget *pWidget)
{
    if (pWidget)
        return ::darwinToNativeWindowImpl(::darwinToNativeView(pWidget));
    return nil;
}

NativeNSViewRef darwinToNativeView(NativeNSWindowRef aWindow)
{
    return ::darwinToNativeViewImpl(aWindow);
}

NativeNSWindowRef darwinToNativeWindow(NativeNSViewRef aView)
{
    return ::darwinToNativeWindowImpl(aView);
}

NativeNSWindowRef darwinNativeButtonOfWindow(QWidget *pWidget, StandardWindowButtonType enmButtonType)
{
    return ::darwinNativeButtonOfWindowImpl(::darwinToNativeWindow(pWidget), enmButtonType);
}


bool darwinSetFrontMostProcess()
{
    ProcessSerialNumber psn = { 0, kCurrentProcess };
    return ::SetFrontProcess(&psn) == 0;
}

void darwinSetHidesAllTitleButtons(QWidget *pWidget)
{
    /* Currently only necessary in the Cocoa version */
    ::darwinSetHidesAllTitleButtonsImpl(::darwinToNativeWindow(pWidget));
}

void darwinSetShowsToolbarButton(QToolBar *aToolBar, bool fEnabled)
{
    QWidget *parent = aToolBar->parentWidget();
    if (parent)
        ::darwinSetShowsToolbarButtonImpl(::darwinToNativeWindow(parent), fEnabled);
}

void darwinSetWindowLabel(QWidget *pWidget, QPixmap *pPixmap)
{
    ::darwinSetWindowLabelImpl(::darwinToNativeWindow(pWidget), ::darwinToNSImageRef(pPixmap), pPixmap->devicePixelRatio());
}

void darwinSetWindowHasShadow(QWidget *pWidget, bool fEnabled)
{
    ::darwinSetWindowHasShadowImpl(::darwinToNativeWindow(pWidget), fEnabled);
}

void darwinDisableIconsInMenus()
{
    /* No icons in the menu of a mac application. */
    QApplication::instance()->setAttribute(Qt::AA_DontShowIconsInMenus, true);
}


uint64_t darwinGetCurrentProcessId()
{
    uint64_t processId = 0;
    ProcessSerialNumber psn = { 0, kCurrentProcess };
    if (::GetCurrentProcess(&psn) == 0)
        processId = RT_MAKE_U64(psn.lowLongOfPSN, psn.highLongOfPSN);
    return processId;
}

QString darwinResolveAlias(const QString &strFile)
{
    FSRef fileRef;
    QString strTarget;
    do
    {
        OSErr err;
        Boolean fDir;
        if ((err = FSPathMakeRef((const UInt8*)strFile.toUtf8().constData(), &fileRef, &fDir)) != noErr)
            break;
        Boolean fAlias = FALSE;
        if ((err = FSIsAliasFile(&fileRef, &fAlias, &fDir)) != noErr)
            break;
        if (fAlias == TRUE)
        {
            if ((err = FSResolveAliasFile(&fileRef, TRUE, &fAlias, &fDir)) != noErr)
                break;
            char pszPath[1024];
            if ((err = FSRefMakePath(&fileRef, (UInt8*)pszPath, 1024)) != noErr)
                break;
            strTarget = QString::fromUtf8(pszPath);
        }
        else
            strTarget = strFile;
    }while(0);

    return strTarget;
}

int darwinWindowTitleHeight(QWidget *pWidget)
{
    return ::darwinWindowTitleHeightImpl(::darwinToNativeWindow(pWidget));
}

bool darwinIsWindowMaximized(QWidget *pWidget)
{
    return ::darwinIsWindowMaximizedImpl(::darwinToNativeWindow(pWidget));
}

void darwinEnableFullscreenSupport(QWidget *pWidget)
{
    return ::darwinEnableFullscreenSupportImpl(::darwinToNativeWindow(pWidget));
}

void darwinEnableTransienceSupport(QWidget *pWidget)
{
    return ::darwinEnableTransienceSupportImpl(::darwinToNativeWindow(pWidget));
}

void darwinToggleFullscreenMode(QWidget *pWidget)
{
    return ::darwinToggleFullscreenModeImpl(::darwinToNativeWindow(pWidget));
}

void darwinToggleWindowZoom(QWidget *pWidget)
{
    return ::darwinToggleWindowZoomImpl(::darwinToNativeWindow(pWidget));
}

bool darwinIsInFullscreenMode(QWidget *pWidget)
{
    return ::darwinIsInFullscreenModeImpl(::darwinToNativeWindow(pWidget));
}

bool darwinIsOnActiveSpace(QWidget *pWidget)
{
    return ::darwinIsOnActiveSpaceImpl(::darwinToNativeWindow(pWidget));
}

int darwinWindowToolBarHeight(QWidget *pWidget)
{
    NOREF(pWidget);
    return 0;
}

QString darwinSystemLanguage(void)
{
    /* Get the locales supported by our bundle */
    CFArrayRef supportedLocales = ::CFBundleCopyBundleLocalizations(::CFBundleGetMainBundle());
    /* Check them against the languages currently selected by the user */
    CFArrayRef preferredLocales = ::CFBundleCopyPreferredLocalizationsFromArray(supportedLocales);
    /* Get the one which is on top */
    CFStringRef localeId = (CFStringRef)::CFArrayGetValueAtIndex(preferredLocales, 0);
    /* Convert them to a C-string */
    char localeName[20];
    ::CFStringGetCString(localeId, localeName, sizeof(localeName), kCFStringEncodingUTF8);
    /* Some cleanup */
    ::CFRelease(supportedLocales);
    ::CFRelease(preferredLocales);
    QString id(localeName);
    /* Check for some misbehavior */
    if (id.isEmpty() ||
        id.toLower() == "english")
        id = "en";
    return id;
}

void darwinMouseGrab(QWidget *pWidget)
{
    CGAssociateMouseAndMouseCursorPosition(false);
    UICocoaApplication::instance()->registerForNativeEvents(RT_BIT_32(1)  | /* NSLeftMouseDown */
                                                            RT_BIT_32(2)  | /* NSLeftMouseUp */
                                                            RT_BIT_32(3)  | /* NSRightMouseDown */
                                                            RT_BIT_32(4)  | /* NSRightMouseUp */
                                                            RT_BIT_32(5)  | /* NSMouseMoved */
                                                            RT_BIT_32(6)  | /* NSLeftMouseDragged */
                                                            RT_BIT_32(7)  | /* NSRightMouseDragged */
                                                            RT_BIT_32(25) | /* NSOtherMouseDown */
                                                            RT_BIT_32(26) | /* NSOtherMouseUp */
                                                            RT_BIT_32(27) | /* NSOtherMouseDragged */
                                                            RT_BIT_32(22),  /* NSScrollWheel */
                                                            ::darwinMouseGrabEvents, pWidget);
}

void darwinMouseRelease(QWidget *pWidget)
{
    UICocoaApplication::instance()->unregisterForNativeEvents(RT_BIT_32(1)  | /* NSLeftMouseDown */
                                                              RT_BIT_32(2)  | /* NSLeftMouseUp */
                                                              RT_BIT_32(3)  | /* NSRightMouseDown */
                                                              RT_BIT_32(4)  | /* NSRightMouseUp */
                                                              RT_BIT_32(5)  | /* NSMouseMoved */
                                                              RT_BIT_32(6)  | /* NSLeftMouseDragged */
                                                              RT_BIT_32(7)  | /* NSRightMouseDragged */
                                                              RT_BIT_32(25) | /* NSOtherMouseDown */
                                                              RT_BIT_32(26) | /* NSOtherMouseUp */
                                                              RT_BIT_32(27) | /* NSOtherMouseDragged */
                                                              RT_BIT_32(22),  /* NSScrollWheel */
                                                              ::darwinMouseGrabEvents, pWidget);
    CGAssociateMouseAndMouseCursorPosition(true);
}

void darwinSendMouseGrabEvents(QWidget *pWidget, int type, int button, int buttons, int x, int y)
{
    QEvent::Type qtType = QEvent::None;
    Qt::MouseButtons qtButtons = Qt::NoButton;
    Qt::MouseButton qtButton = Qt::NoButton;
    Qt::MouseButton qtExtraButton = Qt::NoButton;
    Qt::Orientation qtOrientation = Qt::Horizontal;
    int wheelDelta = 0;
    /* Which button is used in the NSOtherMouse... cases? */
    if (button == 0)
        qtExtraButton = Qt::LeftButton;
    else if (button == 1)
        qtExtraButton = Qt::RightButton;
    else if (button == 2)
        qtExtraButton = Qt::MiddleButton;
    else if (button == 3)
        qtExtraButton = Qt::XButton1;
    else if (button == 4)
        qtExtraButton = Qt::XButton2;
    /* Map the NSEvent to a QEvent and define the Qt::Buttons when necessary. */
    switch(type)
    {
        case 1: /* NSLeftMouseDown */
        {
            qtType = QEvent::MouseButtonPress;
            qtButton = Qt::LeftButton;
            break;
        }
        case 2: /* NSLeftMouseUp */
        {
            qtType = QEvent::MouseButtonRelease;
            qtButton = Qt::LeftButton;
            break;
        }
        case 3: /* NSRightMouseDown */
        {
            qtType = QEvent::MouseButtonPress;
            qtButton = Qt::RightButton;
            break;
        }
        case 4: /* NSRightMouseUp */
        {
            qtType = QEvent::MouseButtonRelease;
            qtButton = Qt::RightButton;
            break;
        }
        case 5: /* NSMouseMoved */
        {
            qtType = QEvent::MouseMove;
            break;
        }
        case 6: /* NSLeftMouseDragged */
        {
            qtType = QEvent::MouseMove;
            qtButton = Qt::LeftButton;
            break;
        }
        case 7: /* NSRightMouseDragged */
        {
            qtType = QEvent::MouseMove;
            qtButton = Qt::RightButton;
            break;
        }
        case 22: /* NSScrollWheel */
        {
            qtType = QEvent::Wheel;
            if (y != 0)
            {
                wheelDelta = y;
                qtOrientation = Qt::Vertical;
            }
            else if (x != 0)
            {
                wheelDelta = x;
                qtOrientation = Qt::Horizontal;
            }
            x = y = 0;
            break;
        }
        case 25: /* NSOtherMouseDown */
        {
            qtType = QEvent::MouseButtonPress;
            qtButton = qtExtraButton;
            break;
        }
        case 26: /* NSOtherMouseUp */
        {
            qtType = QEvent::MouseButtonRelease;
            qtButton = qtExtraButton;
            break;
        }
        case 27: /* NSOtherMouseDragged */
        {
            qtType = QEvent::MouseMove;
            qtButton = qtExtraButton;
            break;
        }
        default: return;
    }
    /* Create a Qt::MouseButtons Mask. */
    if ((buttons & RT_BIT_32(0)) == RT_BIT_32(0))
        qtButtons |= Qt::LeftButton;
    if ((buttons & RT_BIT_32(1)) == RT_BIT_32(1))
        qtButtons |= Qt::RightButton;
    if ((buttons & RT_BIT_32(2)) == RT_BIT_32(2))
        qtButtons |= Qt::MiddleButton;
    if ((buttons & RT_BIT_32(3)) == RT_BIT_32(3))
        qtButtons |= Qt::XButton1;
    if ((buttons & RT_BIT_32(4)) == RT_BIT_32(4))
        qtButtons |= Qt::XButton2;
    /* Create a new mouse delta event and send it to the widget. */
    UIGrabMouseEvent *pEvent = new UIGrabMouseEvent(qtType, qtButton, qtButtons, x, y, wheelDelta, qtOrientation);
    qApp->sendEvent(pWidget, pEvent);
}


/**
 * Callback for deleting the QImage object when CGImageCreate is done
 * with it (which is probably not until the returned CFGImageRef is released).
 *
 * @param   info        Pointer to the QImage.
 */
static void darwinDataProviderReleaseQImage(void *info, const void *, size_t)
{
    QImage *qimg = (QImage *)info;
    delete qimg;
}

/**
 * Converts a QImage to a CGImage.
 *
 * @returns CGImageRef for the new image. (Remember to release it when finished with it.)
 * @param  pImage  Pointer to the QImage instance to convert.
 */
CGImageRef darwinToCGImageRef(const QImage *pImage)
{
    QImage *imageCopy = new QImage(*pImage);
    /** @todo this code assumes 32-bit image input, the lazy bird convert image to 32-bit method is anything but optimal... */
    if (imageCopy->format() != QImage::Format_ARGB32)
        *imageCopy = imageCopy->convertToFormat(QImage::Format_ARGB32);
    Assert(!imageCopy->isNull());

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(imageCopy, pImage->bits(), pImage->sizeInBytes(),
                                                        darwinDataProviderReleaseQImage);

    CGBitmapInfo bmpInfo = kCGImageAlphaFirst | kCGBitmapByteOrder32Host;
    CGImageRef ir = CGImageCreate(imageCopy->width(), imageCopy->height(), 8, 32, imageCopy->bytesPerLine(), cs,
                                   bmpInfo, dp, 0 /*decode */, 0 /* shouldInterpolate */,
                                   kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(dp);

    Assert(ir);
    return ir;
}

/**
 * Converts a QPixmap to a CGImage.
 *
 * @returns CGImageRef for the new image. (Remember to release it when finished with it.)
 * @param  pPixmap  Pointer to the QPixmap instance to convert.
 */
CGImageRef darwinToCGImageRef(const QPixmap *pPixmap)
{
    /* It seems Qt releases the memory to an returned CGImageRef when the
     * associated QPixmap is destroyed. This shouldn't happen as long a
     * CGImageRef has a retrain count. As a workaround we make a real copy. */
    int bitmapBytesPerRow = pPixmap->width() * 4;
    int bitmapByteCount = (bitmapBytesPerRow * pPixmap->height());
    /* Create a memory block for the temporary image. It is initialized by zero
     * which means black & zero alpha. */
    void *pBitmapData = RTMemAllocZ(bitmapByteCount);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    /* Create a context to paint on */
    CGContextRef ctx = CGBitmapContextCreate(pBitmapData,
                                              pPixmap->width(),
                                              pPixmap->height(),
                                              8,
                                              bitmapBytesPerRow,
                                              cs,
                                              kCGImageAlphaPremultipliedFirst);
    /* Get the CGImageRef from Qt */
    CGImageRef qtPixmap = pPixmap->toImage().toCGImage();
    /* Draw the image from Qt & convert the context back to a new CGImageRef. */
    CGContextDrawImage(ctx, CGRectMake(0, 0, pPixmap->width(), pPixmap->height()), qtPixmap);
    CGImageRef newImage = CGBitmapContextCreateImage(ctx);
    /* Now release all used resources */
    CGImageRelease(qtPixmap);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    RTMemFree(pBitmapData);

    /* Return the new CGImageRef */
    return newImage;
}

/**
 * Loads an image using Qt and converts it to a CGImage.
 *
 * @returns CGImageRef for the new image. (Remember to release it when finished with it.)
 * @param  pczSource  The source name.
 */
CGImageRef darwinToCGImageRef(const char *pczSource)
{
    QPixmap qpm(QString(":/") + pczSource);
    Assert(!qpm.isNull());
    return ::darwinToCGImageRef(&qpm);
}
