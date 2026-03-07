/* $Id: QIMainWindow.cpp 112750 2026-01-29 16:19:14Z sergey.dubov@oracle.com $ */
/** @file
 * VBox Qt GUI - QIMainWindow class implementation.
 */

/*
 * Copyright (C) 2016-2026 Oracle and/or its affiliates.
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
#include <QResizeEvent>

/* GUI includes: */
#include "QIMainWindow.h"
#ifdef VBOX_WS_MAC
# include "VBoxUtils-darwin.h"
#endif
#ifdef VBOX_WS_NIX
# include "UIDesktopWidgetWatchdog.h"
#endif


QIMainWindow::QIMainWindow(QWidget *pParent /* = 0 */, Qt::WindowFlags enmFlags /* = Qt::WindowFlags() */)
    : QMainWindow(pParent, enmFlags)
{
}

void QIMainWindow::moveEvent(QMoveEvent *pEvent)
{
    /* Call to base-class: */
    QMainWindow::moveEvent(pEvent);

#ifdef VBOX_WS_NIX
    /* Prevent further handling if fake screen detected: */
    if (UIDesktopWidgetWatchdog::isFakeScreenDetected())
        return;
#endif

    /* Prevent handling for yet/already invisible window or if window is in minimized state: */
    if (isVisible() && (windowState() & Qt::WindowMinimized) == 0)
    {
#if defined(VBOX_WS_MAC) || defined(VBOX_WS_WIN)
        /* Use the old approach for OSX/Win: */
        m_geometry.moveTo(frameGeometry().x(), frameGeometry().y());
#else
        /* Use the new approach otherwise: */
        m_geometry.moveTo(geometry().x(), geometry().y());
#endif
    }
}

void QIMainWindow::resizeEvent(QResizeEvent *pEvent)
{
    /* Call to base-class: */
    QMainWindow::resizeEvent(pEvent);

#ifdef VBOX_WS_NIX
    /* Prevent handling if fake screen detected: */
    if (UIDesktopWidgetWatchdog::isFakeScreenDetected())
        return;
#endif

    /* Prevent handling for yet/already invisible window or if window is in minimized state: */
    if (isVisible() && (windowState() & Qt::WindowMinimized) == 0)
    {
        QResizeEvent *pResizeEvent = static_cast<QResizeEvent*>(pEvent);
        m_geometry.setSize(pResizeEvent->size());
    }
}

void QIMainWindow::restoreGeometry(const QRect &rect)
{
    m_geometry = rect;
#if defined(VBOX_WS_MAC) || defined(VBOX_WS_WIN)
    /* Use the old approach for OSX/Win: */
    move(m_geometry.topLeft());
    resize(m_geometry.size());
#else
    /* Use the new approach otherwise: */
    UIDesktopWidgetWatchdog::setTopLevelGeometry(this, m_geometry);
#endif

    /* Maximize (if necessary): */
    if (shouldBeMaximized())
        showMaximized();
}

bool QIMainWindow::isCurrentlyMaximized()
{
#ifdef VBOX_WS_MAC
    return ::darwinIsWindowMaximized(this);
#else
    return isMaximized();
#endif
}
