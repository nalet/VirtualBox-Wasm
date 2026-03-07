/* $Id: UIDockIconPreview-cocoa.mm 112818 2026-02-04 13:57:23Z sergey.dubov@oracle.com $ */
/** @file
 * VBox Qt GUI - UIDockIconPreview class implementation.
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

/* GUI includes: */
#include "UIDockIconPreview.h"
#include "VBoxUtils-darwin.h"

/* Cocoa imports: */
#import <Cocoa/Cocoa.h>


@interface UIDockTileMonitor: NSView
{
    UIDockIconPreviewPrivate *p;

    NSImageView *mScreenContent;
    NSImageView *mMonitorGlossy;
}
- (id)initWithFrame:(NSRect)frame parent:(UIDockIconPreviewPrivate*)parent;
- (NSImageView*)screenContent;
- (void)resize:(NSSize)size;
@end


@interface UIDockTileOverlay: NSView
{
    UIDockIconPreviewPrivate *p;
}
- (id)initWithFrame:(NSRect)frame parent:(UIDockIconPreviewPrivate*)parent;
@end


@interface UIDockTile: NSView
{
    UIDockIconPreviewPrivate *p;

    UIDockTileMonitor *mMonitor;
    NSImageView       *mAppIcon;

    UIDockTileOverlay *mOverlay;
}
- (id)initWithParent:(UIDockIconPreviewPrivate*)parent;
- (void)destroy;
- (NSView*)screenContentWithParentView:(NSView*)parentView;
- (void)cleanup;
- (void)restoreAppIcon;
- (void)updateAppIcon;
- (void)restoreMonitor;
- (void)updateMonitorWithImage:(CGImageRef)image;
- (void)resizeMonitor:(NSSize)size;
@end


/*
 * Helper class which allow us to access all members/methods of DockIconPreviewHelper from Cocoa class.
 */
class UIDockIconPreviewPrivate : public UIDockIconPreviewHelper
{
public:

    /** Constructs private data for Cocoa dock icon preview.
      * @param  pMachine      Brings the machine this preview being created for.
      * @param  overlayImage  Brings the overlayImage to use for a case when no preview available atm. */
    inline UIDockIconPreviewPrivate(UIMachine *pMachine, const QPixmap &overlayImage)
        : UIDockIconPreviewHelper(pMachine, overlayImage)
    {
        m_pUIDockTile = [[UIDockTile alloc] initWithParent:this];
    }

    /** Destructs private data for Cocoa dock icon preview. */
    inline ~UIDockIconPreviewPrivate()
    {

        [m_pUIDockTile destroy];
        [m_pUIDockTile release];
    }

    /** Holds the dock tile object reference. */
    UIDockTile *m_pUIDockTile;
};


/*
 * Class wrapping Cocoa dock icon preview interface.
 */
UIDockIconPreview::UIDockIconPreview(UIMachine *pMachine, const QPixmap& overlayImage)
{
    CocoaAutoreleasePool pool;

    d = new UIDockIconPreviewPrivate(pMachine, overlayImage);
}

UIDockIconPreview::~UIDockIconPreview()
{
    CocoaAutoreleasePool pool;

    delete d;
}

void UIDockIconPreview::updateDockOverlay()
{
    CocoaAutoreleasePool pool;

    [d->m_pUIDockTile updateAppIcon];
}

void UIDockIconPreview::updateDockPreview(CGImageRef vmImage)
{
    CocoaAutoreleasePool pool;

    [d->m_pUIDockTile updateMonitorWithImage:vmImage];
}

void UIDockIconPreview::setOriginalSize(int width, int height)
{
    CocoaAutoreleasePool pool;

    [d->m_pUIDockTile resizeMonitor:NSMakeSize(width, height)];
}


/*
 * Class for arranging/updating the layers for the glossy monitor preview.
 */
@implementation UIDockTileMonitor
- (id)initWithFrame:(NSRect)frame parent:(UIDockIconPreviewPrivate*)parent
{
    self = [super initWithFrame:frame];

    if (self != nil)
    {
        p = parent;
        /* The screen content view */
        mScreenContent = [[NSImageView alloc] initWithFrame:NSRectFromCGRect(p->flipRect(p->m_updateRect))];
//        [mScreenContent setImageAlignment: NSImageAlignCenter];
        [mScreenContent setImageAlignment: NSImageAlignTopLeft];
        [mScreenContent setImageScaling: NSImageScaleAxesIndependently];
        [self addSubview: mScreenContent];
        /* The state view */
        mMonitorGlossy = [[NSImageView alloc] initWithFrame:NSRectFromCGRect(p->flipRect(p->m_monitorRect))];
        [mMonitorGlossy setImage: ::darwinToNSImageRef(p->m_dockMonitorGlossy)];
        [self addSubview: mMonitorGlossy];
    }

    return self;
}

- (void)drawRect:(NSRect)aRect
{
    NSImage *dockMonitor = ::darwinToNSImageRef(p->m_dockMonitor);
#if MAC_OS_X_VERSION_MIN_REQUIRED >= 101200
    [dockMonitor drawInRect:NSRectFromCGRect(p->flipRect(p->m_monitorRect)) fromRect:aRect operation:NSCompositingOperationSourceOver fraction:1.0];
#else
    [dockMonitor drawInRect:NSRectFromCGRect(p->flipRect(p->m_monitorRect)) fromRect:aRect operation:NSCompositeSourceOver fraction:1.0];
#endif
    [dockMonitor release];
}

- (NSImageView*)screenContent
{
    return mScreenContent;
}

- (void)resize:(NSSize)size
{
    /* Calculate the new size based on the aspect ratio of the original screen
       size */
    float w, h;
    if (size.width > size.height)
    {
        w = p->m_updateRect.size.width;
        h = ((float)size.height / size.width * p->m_updateRect.size.height);
    }
    else
    {
        w = ((float)size.width / size.height * p->m_updateRect.size.width);
        h = p->m_updateRect.size.height;
    }
    CGRect r = (p->flipRect (p->centerRectTo (CGRectMake (0, 0, (int)w, (int)h), p->m_updateRect)));
    r.origin.x = (int)r.origin.x;
    r.origin.y = (int)r.origin.y;
    r.size.width = (int)r.size.width;
    r.size.height = (int)r.size.height;
//    printf("gui %f %f %f %f\n", r.origin.x, r.origin.y, r.size.width, r.size.height);
    /* Center within the update rect */
    [mScreenContent setFrame:NSRectFromCGRect (r)];
}
@end


/*
 * Simple implementation for the overlay of the OS & the state icon. Is used both
 * in the application icon & preview mode.
 */
@implementation UIDockTileOverlay
- (id)initWithFrame:(NSRect)frame parent:(UIDockIconPreviewPrivate*)parent
{
    self = [super initWithFrame:frame];

    if (self != nil)
        p = parent;

    return self;
}

- (void)drawRect:(NSRect)aRect
{
    RT_NOREF(aRect);
    NSGraphicsContext *nsContext = [NSGraphicsContext currentContext];
    CGContextRef pCGContext = (CGContextRef)[nsContext graphicsPort];
    p->drawOverlayIcons (pCGContext);
}
@end


/*
 * VirtualBox Dock Tile implementation. Manage the switching between the icon
 * and preview mode & forwards all update request to the appropriate methods.
 */
@implementation UIDockTile
- (id)initWithParent:(UIDockIconPreviewPrivate*)parent
{
    self = [super init];

    if (self != nil)
    {
        p = parent;
        /* Add self as the content view of the dock tile */
        NSDockTile *dock = [[NSApplication sharedApplication] dockTile];
        [dock setContentView: self];
        /* App icon is default */
        [self restoreAppIcon];
        /* The overlay */
        mOverlay = [[UIDockTileOverlay alloc] initWithFrame:NSRectFromCGRect(p->flipRect (p->m_dockIconRect)) parent:p];
        [self addSubview: mOverlay];
    }

    return self;
}

- (void)destroy
{
    /* Remove all content from the application dock tile. */
    [mOverlay removeFromSuperview];
    [mOverlay release];
    mOverlay = nil;
    NSDockTile *dock = [[NSApplication sharedApplication] dockTile];
    [dock setContentView: nil];
    /* Cleanup all other resources */
    [self cleanup];
}

- (NSView*)screenContentWithParentView:(NSView*)parentView
{
    if (mMonitor != nil)
    {
        void *pId = p->currentPreviewWindowId();
        if (parentView == pId)
            return [mMonitor screenContent];
    }
    return nil;
}

- (void)cleanup
{
    if (mAppIcon != nil)
    {
        [mAppIcon removeFromSuperview];
        [mAppIcon release];
        mAppIcon = nil;
    }
    if (mMonitor != nil)
    {
        [mMonitor removeFromSuperview];
        [mMonitor release];
        mMonitor = nil;
    }
}

- (void)restoreAppIcon
{
    if (mAppIcon == nil)
    {
        [self cleanup];
        mAppIcon = [[NSImageView alloc] initWithFrame:NSRectFromCGRect (p->flipRect (p->m_dockIconRect))];
        [mAppIcon setImage: [NSImage imageNamed:@"NSApplicationIcon"]];
        [self addSubview: mAppIcon positioned:NSWindowBelow relativeTo:mOverlay];
    }
}

- (void)updateAppIcon
{
    [self restoreAppIcon];
    [[[NSApplication sharedApplication] dockTile] display];
}

- (void)restoreMonitor
{
    if (mMonitor == nil)
    {
        p->initPreviewImages();
        [self cleanup];
        mMonitor = [[UIDockTileMonitor alloc] initWithFrame:NSRectFromCGRect (p->flipRect (p->m_dockIconRect)) parent:p];
        [self addSubview: mMonitor positioned:NSWindowBelow relativeTo:mOverlay];
    }
}

- (void)updateMonitorWithImage:(CGImageRef)image
{
    [self restoreMonitor];
    NSImage *nsimage = ::darwinToNSImageRef(image);
    [[mMonitor screenContent] setImage: nsimage];
    [nsimage release];
    [[[NSApplication sharedApplication] dockTile] display];
}

- (void)resizeMonitor:(NSSize)size
{
    [self restoreMonitor];
    [mMonitor resize:size];
}
@end
