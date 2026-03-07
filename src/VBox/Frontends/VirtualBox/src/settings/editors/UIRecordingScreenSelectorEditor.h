/* $Id: UIRecordingScreenSelectorEditor.h 112403 2026-01-11 19:29:08Z knut.osmundsen@oracle.com $ */
/** @file
 * VBox Qt GUI - UIRecordingScreenSelectorEditor class declaration.
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

#ifndef FEQT_INCLUDED_SRC_settings_editors_UIRecordingScreenSelectorEditor_h
#define FEQT_INCLUDED_SRC_settings_editors_UIRecordingScreenSelectorEditor_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

/* GUI includes: */
#include "UIEditor.h"

/* Forward declarations: */
class QGridLayout;
class QLabel;
class UIFilmContainer;

/** UIEditor sub-class used as a recording screen selection editor. */
class SHARED_LIBRARY_STUFF UIRecordingScreenSelectorEditor : public UIEditor
{
    Q_OBJECT;

public:

    /** Constructs editor passing @a pParent to the base-class. */
    UIRecordingScreenSelectorEditor(QWidget *pParent = 0);

    /** Defines enabled @a screens. */
    void setScreens(const QVector<bool> &screens);
    /** Returns enabled screens. */
    QVector<bool> screens() const;

    /** Returns minimum layout hint. */
    int minimumLabelHorizontalHint() const;
    /** Defines minimum layout @a iIndent. */
    void setMinimumLayoutIndent(int iIndent);

private slots:

    /** Handles translation event. */
    virtual void sltRetranslateUI() RT_OVERRIDE RT_FINAL;

private:

    /** Prepares all. */
    void prepare();
    /** Prepares widgets. */
    void prepareWidgets();

    /** @name Values
     * @{ */
        /** Holds the screens. */
        QVector<bool>  m_screens;
    /** @} */

    /** @name Widgets
     * @{ */
        /** Holds the main layout instance. */
        QGridLayout     *m_pLayout;
        /** Holds the label instance. */
        QLabel          *m_pLabel;
        /** Holds the scroller instance. */
        UIFilmContainer *m_pScroller;
    /** @} */
};

#endif /* !FEQT_INCLUDED_SRC_settings_editors_UIRecordingScreenSelectorEditor_h */
