/* $Id: QIMainWindow.h 112750 2026-01-29 16:19:14Z sergey.dubov@oracle.com $ */
/** @file
 * VBox Qt GUI - QIMainWindow class declaration.
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

#ifndef FEQT_INCLUDED_SRC_extensions_QIMainWindow_h
#define FEQT_INCLUDED_SRC_extensions_QIMainWindow_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

/* Qt includes: */
#include <QMainWindow>
#include <QRect>

/* GUI includes: */
#include "UILibraryDefs.h"

/* Forward declarations: */
class QMoveEvent;
class QResizeEvent;
class QWidget;

/** QIMainWindow extension with geometry saving/restoring capabilities. */
class SHARED_LIBRARY_STUFF QIMainWindow : public QMainWindow
{
    Q_OBJECT;

public:

    /** Constructs main window passing @a pParent and @a enmFlags to base-class. */
    QIMainWindow(QWidget *pParent = 0, Qt::WindowFlags enmFlags = Qt::WindowFlags());

protected:

    /** Handles move @a pEvent. */
    virtual void moveEvent(QMoveEvent *pEvent) RT_OVERRIDE;
    /** Handles resize @a pEvent. */
    virtual void resizeEvent(QResizeEvent *pEvent) RT_OVERRIDE;

    /** Returns whether the window should be maximized when geometry being restored. */
    virtual bool shouldBeMaximized() const { return false; }

    /** Restores the window geometry to passed @a rect. */
    void restoreGeometry(const QRect &rect);
    /** Returns current window geometry. */
    QRect currentGeometry() const { return m_geometry; }

    /** Returns whether the window is currently maximized. */
    bool isCurrentlyMaximized();

private:

    /** Holds the cached window geometry. */
    QRect  m_geometry;
};

#endif /* !FEQT_INCLUDED_SRC_extensions_QIMainWindow_h */
