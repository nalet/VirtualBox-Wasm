/* $Id: UIRecordingModeEditor.h 112403 2026-01-11 19:29:08Z knut.osmundsen@oracle.com $ */
/** @file
 * VBox Qt GUI - UIRecordingModeEditor class declaration.
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

#ifndef FEQT_INCLUDED_SRC_settings_editors_UIRecordingModeEditor_h
#define FEQT_INCLUDED_SRC_settings_editors_UIRecordingModeEditor_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

/* GUI includes: */
#include "UIEditor.h"
#include "UISettingsDefs.h"

/* Forward declarations: */
class QComboBox;
class QGridLayout;
class QLabel;

/** UIEditor sub-class used as a recording mode editor. */
class SHARED_LIBRARY_STUFF UIRecordingModeEditor : public UIEditor
{
    Q_OBJECT;

signals:

    /** Notifies listeners about mode change. */
    void sigModeChange();

public:

    /** Constructs editor passing @a pParent to the base-class. */
    UIRecordingModeEditor(QWidget *pParent = 0);

    /** Defines @a enmMode. */
    void setMode(UISettingsDefs::RecordingMode enmMode);
    /** Returns mode. */
    UISettingsDefs::RecordingMode mode() const;

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
    /** Prepares connections. */
    void prepareConnections();

    /** Populates combo-box. */
    void populateCombo();

    /** @name Values
     * @{ */
        /** Holds the list of supported modes. */
        QVector<UISettingsDefs::RecordingMode>  m_supportedValues;
        /** Holds the mode. */
        UISettingsDefs::RecordingMode           m_enmMode;
    /** @} */

    /** @name Widgets
     * @{ */
        QGridLayout *m_pLayout;
        /** Holds the mode label instance. */
        QLabel      *m_pLabel;
        /** Holds the mode combo instance. */
        QComboBox   *m_pCombo;
    /** @} */
};

#endif /* !FEQT_INCLUDED_SRC_settings_editors_UIRecordingModeEditor_h */
