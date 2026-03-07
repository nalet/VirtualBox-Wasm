/* $Id: UIRecordingModeEditor.cpp 112403 2026-01-11 19:29:08Z knut.osmundsen@oracle.com $ */
/** @file
 * VBox Qt GUI - UIRecordingModeEditor class implementation.
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
#include <QComboBox>
#include <QGridLayout>
#include <QLabel>

/* GUI includes: */
#include "UIConverter.h"
#include "UIGlobalSession.h"
#include "UIRecordingModeEditor.h"

/* COM includes: */
#include "KRecordingFeature.h"


UIRecordingModeEditor::UIRecordingModeEditor(QWidget *pParent /* = 0 */)
    : UIEditor(pParent)
    , m_enmMode(UISettingsDefs::RecordingMode_Max)
    , m_pLabel(0)
    , m_pCombo(0)
{
    prepare();
}

void UIRecordingModeEditor::setMode(UISettingsDefs::RecordingMode enmMode)
{
    /* Update cached value and
     * combo if value has changed: */
    if (m_enmMode != enmMode)
    {
        m_enmMode = enmMode;
        populateCombo();
    }
}

UISettingsDefs::RecordingMode UIRecordingModeEditor::mode() const
{
    return m_pCombo ? m_pCombo->currentData().value<UISettingsDefs::RecordingMode>() : m_enmMode;
}

int UIRecordingModeEditor::minimumLabelHorizontalHint() const
{
    return m_pLabel ? m_pLabel->minimumSizeHint().width() : 0;
}

void UIRecordingModeEditor::setMinimumLayoutIndent(int iIndent)
{
    if (m_pLayout)
        m_pLayout->setColumnMinimumWidth(0, iIndent + m_pLayout->spacing());
}

void UIRecordingModeEditor::sltRetranslateUI()
{
    m_pLabel->setText(tr("Recording &Mode"));
    for (int iIndex = 0; iIndex < m_pCombo->count(); ++iIndex)
    {
        const UISettingsDefs::RecordingMode enmType =
            m_pCombo->itemData(iIndex).value<UISettingsDefs::RecordingMode>();
        m_pCombo->setItemText(iIndex, gpConverter->toString(enmType));
    }
    m_pCombo->setToolTip(tr("Recording mode"));
}

void UIRecordingModeEditor::prepare()
{
    /* Prepare everything: */
    prepareWidgets();
    prepareConnections();

    /* Populate combo: */
    populateCombo();

    /* Apply language settings: */
    sltRetranslateUI();
}

void UIRecordingModeEditor::prepareWidgets()
{
    /* Prepare main layout: */
    m_pLayout = new QGridLayout(this);
    if (m_pLayout)
    {
        m_pLayout->setContentsMargins(0, 0, 0, 0);

        /* Prepare recording mode label: */
        m_pLabel = new QLabel(this);
        if (m_pLabel)
        {
            m_pLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_pLayout->addWidget(m_pLabel, 0, 0);
        }
        /* Prepare recording mode combo: */
        m_pCombo = new QComboBox(this);
        if (m_pCombo)
        {
            if (m_pLabel)
                m_pLabel->setBuddy(m_pCombo);
            m_pCombo->setSizePolicy(QSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed));
            m_pCombo->addItem(QString(), QVariant::fromValue(UISettingsDefs::RecordingMode_VideoAudio));
            m_pCombo->addItem(QString(), QVariant::fromValue(UISettingsDefs::RecordingMode_VideoOnly));
            m_pCombo->addItem(QString(), QVariant::fromValue(UISettingsDefs::RecordingMode_AudioOnly));

            m_pLayout->addWidget(m_pCombo, 0, 1);
        }
    }
}

void UIRecordingModeEditor::prepareConnections()
{
    connect(m_pCombo, &QComboBox::currentIndexChanged,
            this, &UIRecordingModeEditor::sigModeChange);
}

void UIRecordingModeEditor::populateCombo()
{
    if (m_pCombo)
    {
        /* Clear combo first of all: */
        m_pCombo->clear();

        /* Load currently supported recording features: */
        const int iSupportedFlag = gpGlobalSession->supportedRecordingFeatures();
        m_supportedValues.clear();
        if (!iSupportedFlag)
            m_supportedValues << UISettingsDefs::RecordingMode_None;
        else
        {
            if (   (iSupportedFlag & KRecordingFeature_Video)
                && (iSupportedFlag & KRecordingFeature_Audio))
                m_supportedValues << UISettingsDefs::RecordingMode_VideoAudio;
            if (iSupportedFlag & KRecordingFeature_Video)
                m_supportedValues << UISettingsDefs::RecordingMode_VideoOnly;
            if (iSupportedFlag & KRecordingFeature_Audio)
                m_supportedValues << UISettingsDefs::RecordingMode_AudioOnly;
        }

        /* Make sure requested value if sane is present as well: */
        if (   m_enmMode != UISettingsDefs::RecordingMode_Max
            && !m_supportedValues.contains(m_enmMode))
            m_supportedValues.prepend(m_enmMode);

        /* Update combo with all the supported values: */
        foreach (const UISettingsDefs::RecordingMode &enmType, m_supportedValues)
            m_pCombo->addItem(QString(), QVariant::fromValue(enmType));

        /* Look for proper index to choose: */
        const int iIndex = m_pCombo->findData(QVariant::fromValue(m_enmMode));
        if (iIndex != -1)
            m_pCombo->setCurrentIndex(iIndex);

        /* Retranslate finally: */
        sltRetranslateUI();
    }
}
