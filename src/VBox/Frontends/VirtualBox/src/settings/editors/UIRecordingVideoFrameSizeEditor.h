/* $Id: UIRecordingVideoFrameSizeEditor.h 112403 2026-01-11 19:29:08Z knut.osmundsen@oracle.com $ */
/** @file
 * VBox Qt GUI - UIRecordingVideoFrameSizeEditor class declaration.
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

#ifndef FEQT_INCLUDED_SRC_settings_editors_UIRecordingVideoFrameSizeEditor_h
#define FEQT_INCLUDED_SRC_settings_editors_UIRecordingVideoFrameSizeEditor_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

/* GUI includes: */
#include "UIEditor.h"

/* Forward declarations: */
class QComboBox;
class QGridLayout;
class QLabel;
class QSpinBox;

/** UIEditor sub-class used as a recording video frame size editor. */
class SHARED_LIBRARY_STUFF UIRecordingVideoFrameSizeEditor : public UIEditor
{
    Q_OBJECT;

signals:

    /** Notifies listeners about video frame size change. */
    void sigFrameSizeChanged();

public:

    /** Constructs editor passing @a pParent to the base-class. */
    UIRecordingVideoFrameSizeEditor(QWidget *pParent = 0);

    /** Defines frame @a iWidth. */
    void setFrameWidth(int iWidth);
    /** Returns frame width. */
    int frameWidth() const;
    /** Defines frame @a iHeight. */
    void setFrameHeight(int iHeight);
    /** Returns frame height. */
    int frameHeight() const;

    /** Returns minimum layout hint. */
    int minimumLabelHorizontalHint() const;
    /** Defines minimum layout @a iIndent. */
    void setMinimumLayoutIndent(int iIndent);

private slots:

    /** Handles translation event. */
    virtual void sltRetranslateUI() RT_OVERRIDE RT_FINAL;

    /** Handles frame size change. */
    void sltHandleFrameSizeComboChange();
    /** Handles frame width change. */
    void sltHandleFrameWidthChange();
    /** Handles frame height change. */
    void sltHandleFrameHeightChange();

private:

    /** Prepares all. */
    void prepare();
    /** Prepares widgets. */
    void prepareWidgets();
    /** Prepares connections. */
    void prepareConnections();

    /** Searches for corresponding frame size preset. */
    void lookForCorrespondingFrameSizePreset();

    /** @name Values
     * @{ */
        /** Holds the frame width. */
        int  m_iFrameWidth;
        /** Holds the frame height. */
        int  m_iFrameHeight;
    /** @} */

    /** @name Widgets
     * @{ */
        /** Holds the layout instance. */
        QGridLayout *m_pLayout;
        /** Holds the label instance. */
        QLabel      *m_pLabel;
        /** Holds the combo instance. */
        QComboBox   *m_pCombo;
        /** Holds the width spinbox instance. */
        QSpinBox    *m_pSpinboxWidth;
        /** Holds the height spinbox instance. */
        QSpinBox    *m_pSpinboxHeight;
    /** @} */
};

#endif /* !FEQT_INCLUDED_SRC_settings_editors_UIRecordingVideoFrameSizeEditor_h */
