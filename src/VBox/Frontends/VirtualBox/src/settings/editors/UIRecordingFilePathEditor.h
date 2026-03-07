/* $Id: UIRecordingFilePathEditor.h 112403 2026-01-11 19:29:08Z knut.osmundsen@oracle.com $ */
/** @file
 * VBox Qt GUI - UIRecordingFilePathEditor class declaration.
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

#ifndef FEQT_INCLUDED_SRC_settings_editors_UIRecordingFilePathEditor_h
#define FEQT_INCLUDED_SRC_settings_editors_UIRecordingFilePathEditor_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

/* GUI includes: */
#include "UIEditor.h"

/* Forward declarations: */
class QGridLayout;
class QLabel;
class UIFilePathSelector;

/** UIEditor sub-class used as a recording file-path editor. */
class SHARED_LIBRARY_STUFF UIRecordingFilePathEditor : public UIEditor
{
    Q_OBJECT;

public:

    /** Constructs editor passing @a pParent to the base-class. */
    UIRecordingFilePathEditor(QWidget *pParent = 0);

    /** Defines @a strFolder. */
    void setFolder(const QString &strFolder);
    /** Returns folder. */
    QString folder() const;
    /** Defines @a strFilePath. */
    void setFilePath(const QString &strFilePath);
    /** Returns file path. */
    QString filePath() const;

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
        /** Holds the folder. */
        QString  m_strFolder;
        /** Holds the file path. */
        QString  m_strFilePath;
    /** @} */

    /** @name Widgets
     * @{ */
        /** Holds the main layout instance. */
        QGridLayout        *m_pLayout;
        /** Holds the label instance. */
        QLabel             *m_pLabel;
        /** Holds the selector instance. */
        UIFilePathSelector *m_pSelector;
    /** @} */
};

#endif /* !FEQT_INCLUDED_SRC_settings_editors_UIRecordingFilePathEditor_h */
