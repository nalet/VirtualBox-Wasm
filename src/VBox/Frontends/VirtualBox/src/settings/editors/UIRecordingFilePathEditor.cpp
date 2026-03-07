/* $Id: UIRecordingFilePathEditor.cpp 112403 2026-01-11 19:29:08Z knut.osmundsen@oracle.com $ */
/** @file
 * VBox Qt GUI - UIRecordingFilePathEditor class implementation.
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

/* Qt includes: */
#include <QGridLayout>
#include <QLabel>

/* GUI includes: */
#include "UIFilePathSelector.h"
#include "UIRecordingFilePathEditor.h"

UIRecordingFilePathEditor::UIRecordingFilePathEditor(QWidget *pParent /* = 0 */)
    : UIEditor(pParent)
    , m_pLabel(0)
    , m_pSelector(0)
{
    prepare();
}

void UIRecordingFilePathEditor::setFolder(const QString &strFolder)
{
    /* Update cached value and
     * file editor if value has changed: */
    if (m_strFolder != strFolder)
    {
        m_strFolder = strFolder;
        if (m_pSelector)
            m_pSelector->setInitialPath(m_strFolder);
    }
}

QString UIRecordingFilePathEditor::folder() const
{
    return m_pSelector ? m_pSelector->initialPath() : m_strFolder;
}

void UIRecordingFilePathEditor::setFilePath(const QString &strFilePath)
{
    /* Update cached value and
     * file editor if value has changed: */
    if (m_strFilePath != strFilePath)
    {
        m_strFilePath = strFilePath;
        if (m_pSelector)
            m_pSelector->setPath(m_strFilePath);
    }
}

QString UIRecordingFilePathEditor::filePath() const
{
    return m_pSelector ? m_pSelector->path() : m_strFilePath;
}

int UIRecordingFilePathEditor::minimumLabelHorizontalHint() const
{
    return m_pLabel ? m_pLabel->minimumSizeHint().width() : 0;
}

void UIRecordingFilePathEditor::setMinimumLayoutIndent(int iIndent)
{
    if (m_pLayout)
        m_pLayout->setColumnMinimumWidth(0, iIndent + m_pLayout->spacing());
}

void UIRecordingFilePathEditor::sltRetranslateUI()
{
    m_pLabel->setText(tr("File &Path"));
    m_pSelector->setToolTip(tr("The filename VirtualBox uses to save the recorded content"));
}

void UIRecordingFilePathEditor::prepare()
{
    /* Prepare everything: */
    prepareWidgets();

    /* Apply language settings: */
    sltRetranslateUI();
}

void UIRecordingFilePathEditor::prepareWidgets()
{
    /* Prepare main layout: */
    m_pLayout = new QGridLayout(this);
    if (m_pLayout)
    {
        m_pLayout->setContentsMargins(0, 0, 0, 0);

        /* Prepare recording label: */
        m_pLabel = new QLabel(this);
        if (m_pLabel)
        {
            m_pLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_pLayout->addWidget(m_pLabel, 0, 0);
        }

        /* Prepare recording selector: */
        m_pSelector = new UIFilePathSelector(this);
        if (m_pSelector)
        {
            if (m_pLabel)
                m_pLabel->setBuddy(m_pSelector);
            m_pSelector->setEditable(false);
            m_pSelector->setMode(UIFilePathSelector::Mode_File_Save);
            m_pSelector->setSizePolicy(QSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed));
            m_pLayout->addWidget(m_pSelector, 0, 1);
        }
    }
}
