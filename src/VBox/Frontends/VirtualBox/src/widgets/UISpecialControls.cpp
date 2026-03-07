/* $Id: UISpecialControls.cpp 112807 2026-02-03 13:54:18Z sergey.dubov@oracle.com $ */
/** @file
 * VBox Qt GUI - UISpecialControls implementation.
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

/* Qt includes: */
#include <QMouseEvent>
#include <QPainter>

/* GUI includes: */
#include "UIIconPool.h"
#include "UIShortcutPool.h"
#include "UISpecialControls.h"
#include "UITranslationEventListener.h"


/*********************************************************************************************************************************
*   Class UIMiniCancelButton implementation.                                                                                     *
*********************************************************************************************************************************/

UIMiniCancelButton::UIMiniCancelButton(QWidget *pParent /* = 0 */)
    : QIToolButton(pParent)
{
    setAutoRaise(true);
    setFocusPolicy(Qt::TabFocus);
    setShortcut(QKeySequence(Qt::Key_Escape));
    setIcon(UIIconPool::defaultIcon(UIIconPool::UIDefaultIconType_DialogCancel));
}


/*********************************************************************************************************************************
*   Class UIHelpButton implementation.                                                                                           *
*********************************************************************************************************************************/

#ifdef VBOX_WS_MAC
/* From: src/gui/styles/qmacstyle_mac.cpp */
static const int PushButtonLeftOffset = 6;
static const int PushButtonTopOffset = 4;
static const int PushButtonRightOffset = 12;
static const int PushButtonBottomOffset = 4;
#endif /* VBOX_WS_MAC */

UIHelpButton::UIHelpButton(QWidget *pParent /* = 0 */)
    : QPushButton(pParent)
#ifdef VBOX_WS_MAC
    , m_pButtonPressed(false)
    , m_pNormalPixmap(0)
    , m_pPressedPixmap(0)
    , m_pMask(0)
#endif /* VBOX_WS_MAC */
{
#ifdef VBOX_WS_MAC
    m_pButtonPressed = false;
    m_pNormalPixmap = new QPixmap(":/help_button_normal_mac_24px.png");
    m_pPressedPixmap = new QPixmap(":/help_button_pressed_mac_24px.png");
    m_size = m_pNormalPixmap->size();
    m_pMask = new QImage(m_pNormalPixmap->mask().toImage());
    m_BRect = QRect(PushButtonLeftOffset,
                    PushButtonTopOffset,
                    m_size.width(),
                    m_size.height());
#endif /* VBOX_WS_MAC */

    /* Apply language settings: */
    sltRetranslateUI();
    connect(&translationEventListener(), &UITranslationEventListener::sigRetranslateUI,
        this, &UIHelpButton::sltRetranslateUI);
}

#ifdef VBOX_WS_MAC
UIHelpButton::~UIHelpButton()
{
    delete m_pNormalPixmap;
    delete m_pPressedPixmap;
    delete m_pMask;
}

QSize UIHelpButton::sizeHint() const
{
    return QSize(m_size.width() + PushButtonLeftOffset + PushButtonRightOffset,
                 m_size.height() + PushButtonTopOffset + PushButtonBottomOffset);
}
#endif /* VBOX_WS_MAC */

void UIHelpButton::initFrom(QPushButton *pOther)
{
    /* Copy settings from pOther: */
    setIcon(pOther->icon());
    setText(pOther->text());
    setShortcut(pOther->shortcut());
    setFlat(pOther->isFlat());
    setAutoDefault(pOther->autoDefault());
    setDefault(pOther->isDefault());

    /* Apply language settings: */
    sltRetranslateUI();
}

#ifdef VBOX_WS_MAC
void UIHelpButton::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.drawPixmap(PushButtonLeftOffset, PushButtonTopOffset, m_pButtonPressed ? *m_pPressedPixmap: *m_pNormalPixmap);
}

bool UIHelpButton::hitButton(const QPoint &position) const
{
    if (m_BRect.contains(position))
        return m_pMask->pixel(position.x() - PushButtonLeftOffset,
                              position.y() - PushButtonTopOffset) == 0xff000000;
    else
        return false;
}

void UIHelpButton::mousePressEvent(QMouseEvent *pEvent)
{
    if (hitButton(pEvent->position().toPoint()))
        m_pButtonPressed = true;
    QPushButton::mousePressEvent(pEvent);
    update();
}

void UIHelpButton::mouseReleaseEvent(QMouseEvent *pEvent)
{
    QPushButton::mouseReleaseEvent(pEvent);
    m_pButtonPressed = false;
    update();
}

void UIHelpButton::leaveEvent(QEvent *pEvent)
{
    QPushButton::leaveEvent(pEvent);
    m_pButtonPressed = false;
    update();
}
#endif /* VBOX_WS_MAC */

void UIHelpButton::sltRetranslateUI()
{
    setText(tr("&Help"));
    if (shortcut().isEmpty())
        setShortcut(UIShortcutPool::standardSequence(QKeySequence::HelpContents));
}
