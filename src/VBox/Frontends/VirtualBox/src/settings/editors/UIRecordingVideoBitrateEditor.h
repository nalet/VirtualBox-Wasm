/* $Id: UIRecordingVideoBitrateEditor.h 112403 2026-01-11 19:29:08Z knut.osmundsen@oracle.com $ */
/** @file
 * VBox Qt GUI - UIRecordingVideoBitrateEditor class declaration.
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

#ifndef FEQT_INCLUDED_SRC_settings_editors_UIRecordingVideoBitrateEditor_h
#define FEQT_INCLUDED_SRC_settings_editors_UIRecordingVideoBitrateEditor_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

/* GUI includes: */
#include "UIEditor.h"

/* Forward declarations: */
class QGridLayout;
class QLabel;
class QSpinBox;
class QIAdvancedSlider;

/** UIEditor sub-class used as a recording video bitrate editor. */
class SHARED_LIBRARY_STUFF UIRecordingVideoBitrateEditor : public UIEditor
{
    Q_OBJECT;

signals:

    /** Notifies listeners about video @a iBitrate change. */
    void sigVideoBitrateChanged(int iBitrate);
    /** Notifies listeners about video @a iQuality change. */
    void sigVideoQualityChanged(int iQuality);

public:

    /** Constructs editor passing @a pParent to the base-class. */
    UIRecordingVideoBitrateEditor(QWidget *pParent = 0);

    /** Defines video @a iQuality. */
    void setQuality(int iQuality);
    /** Return video quality. */
    int quality() const;

    /** Defines video @a iBitrate. */
    void setBitrate(int iBitrate);
    /** Returns video bitrate. */
    int bitrate() const;

    /** Returns minimum layout hint. */
    int minimumLabelHorizontalHint() const;
    /** Defines minimum layout @a iIndent. */
    void setMinimumLayoutIndent(int iIndent);

private slots:

    /** Handles translation event. */
    virtual void sltRetranslateUI() RT_OVERRIDE RT_FINAL;

    /** Handles bitrate slider change. */
    void sltHandleBitrateSliderChange();
    /** Handles bitrate spinbox change. */
    void sltHandleBitrateSpinboxChange();

private:

    /** Prepares all. */
    void prepare();
    /** Prepares widgets. */
    void prepareWidgets();
    /** Prepares connections. */
    void prepareConnections();

    /** @name Values
     * @{ */
        /** Holds the quality. */
        int  m_iQuality;
        /** Holds the bitrate. */
        int  m_iBitrate;
    /** @} */

    /** @name Widgets
     * @{ */
        /** Holds the main layout instance. */
        QGridLayout      *m_pLayout;
        /** Holds the bitrate label instance. */
        QLabel           *m_pLabel;
        /** Holds the bitrate slider instance. */
        QIAdvancedSlider *m_pSlider;
        /** Holds the bitrate spinbox instance. */
        QSpinBox         *m_pSpinbox;
        /** Holds the bitrate min label instance. */
        QLabel           *m_pLabelMin;
        /** Holds the bitrate med label instance. */
        QLabel           *m_pLabelMed;
        /** Holds the bitrate max label instance. */
        QLabel           *m_pLabelMax;
    /** @} */
};

#endif /* !FEQT_INCLUDED_SRC_settings_editors_UIRecordingVideoBitrateEditor_h */
