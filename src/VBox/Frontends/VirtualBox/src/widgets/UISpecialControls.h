/* $Id: UISpecialControls.h 112807 2026-02-03 13:54:18Z sergey.dubov@oracle.com $ */
/** @file
 * VBox Qt GUI - UISpecialControls declarations.
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

#ifndef FEQT_INCLUDED_SRC_widgets_UISpecialControls_h
#define FEQT_INCLUDED_SRC_widgets_UISpecialControls_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

/* Qt includes: */
#include <QPushButton>

/* GUI includes: */
#include "QIToolButton.h"

/* Forward declarations: */
class QImage;
class QPixmap;

/** QIToolButton subclass, used as mini cancel button. */
class UIMiniCancelButton : public QIToolButton
{
    Q_OBJECT;

public:

    /** Constructs mini cancel-button passing @a pParent to the base-class. */
    UIMiniCancelButton(QWidget *pParent = 0);
};

/** QPushButton subclass, used as help button. */
class UIHelpButton : public QPushButton
{
    Q_OBJECT;

public:

    /** Constructs help-button passing @a pParent to the base-class. */
    UIHelpButton(QWidget *pParent = 0);

# ifdef VBOX_WS_MAC
    /** Destructs help-button. */
    virtual ~UIHelpButton() RT_OVERRIDE;

    /** Returns size-hint. */
    virtual QSize sizeHint() const RT_OVERRIDE;
# endif /* VBOX_WS_MAC */

    /** Inits this button from pOther. */
    void initFrom(QPushButton *pOther);

# ifdef VBOX_WS_MAC
protected:

    /** Handles paint @a pEvent. */
    virtual void paintEvent(QPaintEvent *pEvent) RT_OVERRIDE;

    /** Handles button hit as certain @a position. */
    virtual bool hitButton(const QPoint &position) const RT_OVERRIDE;
    /** Handles mouse-press @a pEvent. */
    virtual void mousePressEvent(QMouseEvent *pEvent) RT_OVERRIDE;
    /** Handles mouse-release @a pEvent. */
    virtual void mouseReleaseEvent(QMouseEvent *pEvent) RT_OVERRIDE;
    /** Handles mouse-leave @a pEvent. */
    virtual void leaveEvent(QEvent *pEvent) RT_OVERRIDE;
# endif /* VBOX_WS_MAC */

private slots:

    /** Handles translation event. */
    void sltRetranslateUI();

# ifdef VBOX_WS_MAC
private:

    /** Holds the pressed button instance. */
    bool  m_pButtonPressed;

    /** Holds the button size. */
    QSize  m_size;

    /** Holds the normal pixmap instance. */
    QPixmap *m_pNormalPixmap;
    /** Holds the pressed pixmap instance. */
    QPixmap *m_pPressedPixmap;

    /** Holds the button mask instance. */
    QImage *m_pMask;

    /** Holds the button rect. */
    QRect  m_BRect;
# endif /* VBOX_WS_MAC */
};

#endif /* !FEQT_INCLUDED_SRC_widgets_UISpecialControls_h */
