/* $Id: VBoxUtils-darwin-cocoa.mm 112827 2026-02-04 19:02:33Z sergey.dubov@oracle.com $ */
/** @file
 * VBox Qt GUI -  Declarations of utility classes and functions for handling Darwin Cocoa specific tasks.
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

/* Qt incoudes: */
#include <QImage>

/* GUI includes: */
#include "VBoxUtils-darwin.h"

/* Carbon includes: */
#include <Carbon/Carbon.h>

/* Cocoa imports: */
#import <AppKit/NSImage.h>
#import <AppKit/NSImageView.h>
#import <AppKit/NSScreen.h>
#import <AppKit/NSScroller.h>
#import <AppKit/NSWorkspace.h>


bool darwinCreateMachineShortcut(NativeNSStringRef pstrSrcFile, NativeNSStringRef pstrDstPath, NativeNSStringRef pstrName, NativeNSStringRef /* pstrUuid */)
{
    RT_NOREF(pstrName);
    if (!pstrSrcFile || !pstrDstPath)
        return false;

    NSError  *pErr        = nil;
    NSURL    *pSrcUrl     = [NSURL fileURLWithPath:pstrSrcFile];

    NSString *pVmFileName = [pSrcUrl lastPathComponent];
    NSString *pSrcPath    = [NSString stringWithFormat:@"%@/%@", pstrDstPath, [pVmFileName stringByDeletingPathExtension]];
    NSURL    *pDstUrl     = [NSURL fileURLWithPath:pSrcPath];

    bool rc = false;

    if (!pSrcUrl || !pDstUrl)
        return false;

    NSData *pBookmark = [pSrcUrl bookmarkDataWithOptions:NSURLBookmarkCreationSuitableForBookmarkFile
                                 includingResourceValuesForKeys:nil
                                 relativeToURL:nil
                                 error:&pErr];

    if (pBookmark)
    {
        rc = [NSURL writeBookmarkData:pBookmark
                    toURL:pDstUrl
                    options:0
                    error:&pErr];
    }

    return rc;
}

bool darwinOpenInFileManager(NativeNSStringRef pstrFile)
{
    return [[NSWorkspace sharedWorkspace] selectFile:pstrFile inFileViewerRootedAtPath:@""];
}

NativeNSWindowRef darwinToNativeWindowImpl(NativeNSViewRef pView)
{
    NativeNSWindowRef window = NULL;
    if (pView)
        window = [pView window];

    return window;
}

NativeNSViewRef darwinToNativeViewImpl(NativeNSWindowRef pWindow)
{
    NativeNSViewRef view = NULL;
    if (pWindow)
        view = [pWindow contentView];

    return view;
}


void darwinSetHidesAllTitleButtonsImpl(NativeNSWindowRef pWindow)
{
    /* Remove all title buttons by changing the style mask. This method is
       available from 10.6 on only. */
    if ([pWindow respondsToSelector: @selector(setStyleMask:)])
        [pWindow performSelector: @selector(setStyleMask:) withObject: (id)NSTitledWindowMask];
    else
    {
        /* On pre 10.6 disable all the buttons currently displayed. Don't use
           setHidden cause this remove the buttons, but didn't release the
           place used for the buttons. */
        NSButton *pButton = [pWindow standardWindowButton:NSWindowCloseButton];
        if (pButton != Nil)
            [pButton setEnabled: NO];
        pButton = [pWindow standardWindowButton:NSWindowMiniaturizeButton];
        if (pButton != Nil)
            [pButton setEnabled: NO];
        pButton = [pWindow standardWindowButton:NSWindowZoomButton];
        if (pButton != Nil)
            [pButton setEnabled: NO];
        pButton = [pWindow standardWindowButton:NSWindowDocumentIconButton];
        if (pButton != Nil)
            [pButton setEnabled: NO];
    }
}

void darwinSetShowsToolbarButtonImpl(NativeNSWindowRef pWindow, bool fEnabled)
{
    [pWindow setShowsToolbarButton:fEnabled];
}

void darwinSetWindowLabelImpl(NativeNSWindowRef pWindow, NativeNSImageRef pImage, double dDpr)
{
    /* Get the parent view of the close button. */
    NSView *wv = [[pWindow standardWindowButton:NSWindowCloseButton] superview];
    if (wv)
    {
        /* We have to calculate the size of the title bar for the center case. */
        NSSize s = [pImage size];
        NSSize s1 = [wv frame].size;
        /* Correctly position the label. */
        NSImageView *iv = [[NSImageView alloc] initWithFrame:NSMakeRect(s1.width - s.width / dDpr,
                                                                        s1.height - s.height / dDpr - 1,
                                                                        s.width / dDpr, s.height / dDpr)];
        /* Configure the NSImageView for auto moving. */
        [iv setImage:pImage];
        [iv setAutoresizesSubviews:true];
        [iv setAutoresizingMask:NSViewMinXMargin | NSViewMinYMargin];
        /* Add it to the parent of the close button. */
        [wv addSubview:iv positioned:NSWindowBelow relativeTo:nil];
    }
}

void darwinSetWindowHasShadowImpl(NativeNSWindowRef pWindow, bool fEnabled)
{
    if (fEnabled)
        [pWindow setHasShadow :YES];
    else
        [pWindow setHasShadow :NO];
}


NativeNSButtonRef darwinNativeButtonOfWindowImpl(NativeNSWindowRef pWindow, StandardWindowButtonType enmButtonType)
{
    /* Return corresponding button: */
    switch (enmButtonType)
    {
        case StandardWindowButtonType_Close:            return [pWindow standardWindowButton:NSWindowCloseButton];
        case StandardWindowButtonType_Miniaturize:      return [pWindow standardWindowButton:NSWindowMiniaturizeButton];
        case StandardWindowButtonType_Zoom:             return [pWindow standardWindowButton:NSWindowZoomButton];
        case StandardWindowButtonType_Toolbar:          return [pWindow standardWindowButton:NSWindowToolbarButton];
        case StandardWindowButtonType_DocumentIcon:     return [pWindow standardWindowButton:NSWindowDocumentIconButton];
        case StandardWindowButtonType_DocumentVersions: /*return [pWindow standardWindowButton:NSWindowDocumentVersionsButton];*/ break;
        case StandardWindowButtonType_FullScreen:       /*return [pWindow standardWindowButton:NSWindowFullScreenButton];*/ break;
    }
    /* Return Nul by default: */
    return Nil;
}

int darwinWindowTitleHeightImpl(NativeNSWindowRef pWindow)
{
    NSView *pSuperview = [[pWindow standardWindowButton:NSWindowCloseButton] superview];
    NSSize sz = [pSuperview frame].size;
    return sz.height;
}

bool darwinIsWindowMaximizedImpl(NativeNSWindowRef pWindow)
{
    /* Mac OS X API NSWindow isZoomed returns true even for almost maximized windows,
     * So implementing this by ourseleves by comparing visible screen-frame & window-frame: */
    NSRect windowFrame = [pWindow frame];
    NSRect screenFrame = [[NSScreen mainScreen] visibleFrame];

    return (windowFrame.origin.x == screenFrame.origin.x) &&
           (windowFrame.origin.y == screenFrame.origin.y) &&
           (windowFrame.size.width == screenFrame.size.width) &&
           (windowFrame.size.height == screenFrame.size.height);
}

void darwinEnableFullscreenSupportImpl(NativeNSWindowRef pWindow)
{
    [pWindow setCollectionBehavior :NSWindowCollectionBehaviorFullScreenPrimary];
}

void darwinEnableTransienceSupportImpl(NativeNSWindowRef pWindow)
{
    [pWindow setCollectionBehavior :NSWindowCollectionBehaviorTransient];
}

void darwinToggleFullscreenModeImpl(NativeNSWindowRef pWindow)
{
    /* Toggle native fullscreen mode for passed pWindow. This method is available since 10.7 only. */
    if ([pWindow respondsToSelector: @selector(toggleFullScreen:)])
        [pWindow performSelector: @selector(toggleFullScreen:) withObject: (id)nil];
}

void darwinToggleWindowZoomImpl(NativeNSWindowRef pWindow)
{
    /* Toggle native window zoom for passed pWindow. This method is available since 10.0. */
    if ([pWindow respondsToSelector: @selector(zoom:)])
        [pWindow performSelector: @selector(zoom:)];
}

bool darwinIsInFullscreenModeImpl(NativeNSWindowRef pWindow)
{
    /* Check whether passed pWindow is in native fullscreen mode. */
    return [pWindow styleMask] & NSFullScreenWindowMask;
}

bool darwinIsOnActiveSpaceImpl(NativeNSWindowRef pWindow)
{
    /* Check whether passed pWindow is on active space. */
    return [pWindow isOnActiveSpace];
}


bool darwinMouseGrabEvents(const void *pvCocoaEvent, const void *pvCarbonEvent, void *pvUser)
{
    NSEvent *pEvent = (NSEvent*)pvCocoaEvent;
    NSEventType EvtType = [pEvent type];
    NSWindow *pWin = ::darwinToNativeWindow((QWidget*)pvUser);
    if (   pWin == [pEvent window]
        && (   EvtType == NSLeftMouseDown
            || EvtType == NSLeftMouseUp
            || EvtType == NSRightMouseDown
            || EvtType == NSRightMouseUp
            || EvtType == NSOtherMouseDown
            || EvtType == NSOtherMouseUp
            || EvtType == NSLeftMouseDragged
            || EvtType == NSRightMouseDragged
            || EvtType == NSOtherMouseDragged
            || EvtType == NSMouseMoved
            || EvtType == NSScrollWheel))
    {
        /* When the mouse position is not associated to the mouse cursor, the x
           and y values are reported as delta values. */
        float x = [pEvent deltaX];
        float y = [pEvent deltaY];
        if (EvtType == NSScrollWheel)
        {
            /* In the scroll wheel case we have to do some magic, cause a
               normal scroll wheel on a mouse behaves different to a trackpad.
               The following is used within Qt. We use the same to get a
               similar behavior. */
            if ([pEvent respondsToSelector:@selector(deviceDeltaX:)])
                x = (float)(intptr_t)[pEvent performSelector:@selector(deviceDeltaX)] * 2;
            else
                x = qBound(-120, (int)(x * 10000), 120);
            if ([pEvent respondsToSelector:@selector(deviceDeltaY:)])
                y = (float)(intptr_t)[pEvent performSelector:@selector(deviceDeltaY)] * 2;
            else
                y = qBound(-120, (int)(y * 10000), 120);
        }
        /* Get the buttons which where pressed when this event occurs. We have
           to use Carbon here, cause the Cocoa method [NSEvent pressedMouseButtons]
           is >= 10.6. */
        uint32 buttonMask = 0;
        GetEventParameter((EventRef)pvCarbonEvent, kEventParamMouseChord, typeUInt32, 0,
                          sizeof(buttonMask), 0, &buttonMask);
        /* Produce a Qt event out of our info. */
        ::darwinSendMouseGrabEvents((QWidget*)pvUser, EvtType, [pEvent buttonNumber], buttonMask, x, y);
        return true;
    }
    return false;
}


void *darwinCocoaToCarbonEvent(void *pvCocoaEvent)
{
    NSEvent *pEvent = (NSEvent*)pvCocoaEvent;
    return (void*)[pEvent eventRef];
}

NativeNSStringRef darwinToNativeString(const char* pcszString)
{
    return [NSString stringWithUTF8String: pcszString];
}

QString darwinFromNativeString(NativeNSStringRef pString)
{
    return [pString cStringUsingEncoding :NSASCIIStringEncoding];
}


/**
 * Calls the + (void)setMouseCoalescingEnabled:(BOOL)flag class method.
 *
 * @param   fEnabled    Whether to enable or disable coalescing.
 */
void darwinSetMouseCoalescingEnabled(bool fEnabled)
{
    [NSEvent setMouseCoalescingEnabled:fEnabled];
}


void darwinForceActiveFocus()
{
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
}

bool darwinScreensHaveSeparateSpaces()
{
    /* Check whether screens have separate spaces.
     * This method is available since 10.9 only. */
    if ([NSScreen respondsToSelector: @selector(screensHaveSeparateSpaces)])
        return [NSScreen performSelector: @selector(screensHaveSeparateSpaces)];
    else
        return false;
}

bool darwinIsScrollerStyleOverlay()
{
    /* Check whether scrollers by default have legacy style.
     * This method is available since 10.7 only. */
    if ([NSScroller respondsToSelector: @selector(preferredScrollerStyle)])
    {
        const int enmType = (int)(intptr_t)[NSScroller performSelector: @selector(preferredScrollerStyle)];
        return enmType == NSScrollerStyleOverlay;
    }
    else
        return false;
}

/**
 * Check for some default application key combinations a Mac user expect, like
 * CMD+Q or CMD+H.
 *
 * @returns true if such a key combo was hit, false otherwise.
 * @param   pEvent          The Cocoa event.
 */
bool darwinIsApplicationCommand(const void *pvCocoaEvent)
{
    NSEvent *pEvent = (NSEvent*)pvCocoaEvent;
    NSEventType  eEvtType = [pEvent type];
    bool         fGlobalHotkey = false;
//
//    if (   (eEvtType == NSKeyDown || eEvtType == NSKeyUp)
//        && [[NSApp mainMenu] performKeyEquivalent:pEvent])
//        return true;
//    return false;
//        && [[[NSApp mainMenu] delegate] menuHasKeyEquivalent:[NSApp mainMenu] forEvent:pEvent target:b action:a])

    switch (eEvtType)
    {
        case NSKeyDown:
        case NSKeyUp:
        {
            NSUInteger fEvtMask = [pEvent modifierFlags];
            unsigned short KeyCode = [pEvent keyCode];
            if (   ((fEvtMask & (NX_NONCOALSESCEDMASK | NX_COMMANDMASK | NX_DEVICELCMDKEYMASK)) == (NX_NONCOALSESCEDMASK | NX_COMMANDMASK | NX_DEVICELCMDKEYMASK))  /* L+CMD */
                || ((fEvtMask & (NX_NONCOALSESCEDMASK | NX_COMMANDMASK | NX_DEVICERCMDKEYMASK)) == (NX_NONCOALSESCEDMASK | NX_COMMANDMASK | NX_DEVICERCMDKEYMASK))) /* R+CMD */
            {
                if (   KeyCode == 0x0c  /* CMD+Q (Quit) */
                    || KeyCode == 0x04) /* CMD+H (Hide) */
                    fGlobalHotkey = true;
            }
            else if (   ((fEvtMask & (NX_NONCOALSESCEDMASK | NX_ALTERNATEMASK | NX_DEVICELALTKEYMASK | NX_COMMANDMASK | NX_DEVICELCMDKEYMASK)) == (NX_NONCOALSESCEDMASK | NX_ALTERNATEMASK | NX_DEVICELALTKEYMASK | NX_COMMANDMASK | NX_DEVICELCMDKEYMASK)) /* L+ALT+CMD */
                     || ((fEvtMask & (NX_NONCOALSESCEDMASK | NX_ALTERNATEMASK | NX_DEVICERCMDKEYMASK | NX_COMMANDMASK | NX_DEVICERCMDKEYMASK)) == (NX_NONCOALSESCEDMASK | NX_ALTERNATEMASK | NX_DEVICERCMDKEYMASK | NX_COMMANDMASK | NX_DEVICERCMDKEYMASK))) /* R+ALT+CMD */
            {
                if (KeyCode == 0x04)    /* ALT+CMD+H (Hide-Others) */
                    fGlobalHotkey = true;
            }
            break;
        }
        default: break;
    }
    return fGlobalHotkey;
}

int darwinWindowToolBarHeight(NativeNSWindowRef pWindow)
{
    NSToolbar *toolbar = [pWindow toolbar];
    NSRect windowFrame = [pWindow frame];
    int toolbarHeight = 0;
    int theight = (NSHeight([NSWindow contentRectForFrameRect:[pWindow frame] styleMask:[pWindow styleMask]]) - NSHeight([[pWindow contentView] frame]));
    /* toolbar height: */
    if(toolbar && [toolbar isVisible])
        /* title bar height: */
        toolbarHeight = NSHeight(windowFrame) - NSHeight([[pWindow contentView] frame]) - theight;

    return toolbarHeight;
}

void darwinRetranslateAppMenu()
{
    /* This is purely Qt internal. If the Trolls change something here, it will
       not work anymore, but at least it will not be a burning man. */
    if ([NSApp respondsToSelector:@selector(qt_qcocoamenuLoader)])
    {
        id loader = [NSApp performSelector:@selector(qt_qcocoamenuLoader)];
        if ([loader respondsToSelector:@selector(qtTranslateApplicationMenu)])
            [loader performSelector:@selector(qtTranslateApplicationMenu)];
    }
}


NativeNSImageRef darwinToNSImageRef(const CGImageRef pImage)
{
    /* Create a bitmap rep from the image. */
    NSBitmapImageRep *bitmapRep = [[[NSBitmapImageRep alloc] initWithCGImage:pImage] autorelease];
    /* Create an NSImage and add the bitmap rep to it */
    NSImage *image = [[NSImage alloc] init];
    [image addRepresentation:bitmapRep];
    return image;
}

NativeNSImageRef darwinToNSImageRef(const QImage *pImage)
{
    /* Create CGImage on the basis of passed QImage: */
    CGImageRef pCGImage = ::darwinToCGImageRef(pImage);
    NativeNSImageRef pNSImage = ::darwinToNSImageRef(pCGImage);
    CGImageRelease(pCGImage);
    /* Apply device pixel ratio: */
    double dScaleFactor = pImage->devicePixelRatio();
    NSSize imageSize = { (CGFloat)pImage->width() / dScaleFactor,
                         (CGFloat)pImage->height() / dScaleFactor };
    [pNSImage setSize:imageSize];
    /* Return result: */
    return pNSImage;
}

NativeNSImageRef darwinToNSImageRef(const QPixmap *pPixmap)
{
   CGImageRef pCGImage = ::darwinToCGImageRef(pPixmap);
   NativeNSImageRef pNSImage = ::darwinToNSImageRef(pCGImage);
   CGImageRelease(pCGImage);
   return pNSImage;
}

NativeNSImageRef darwinToNSImageRef(const char *pczSource)
{
   CGImageRef pCGImage = ::darwinToCGImageRef(pczSource);
   NativeNSImageRef pNSImage = ::darwinToNSImageRef(pCGImage);
   CGImageRelease(pCGImage);
   return pNSImage;
}
