/* $Id: UIRecordingVideoFrameRateEditor.cpp 112403 2026-01-11 19:29:08Z knut.osmundsen@oracle.com $ */
/** @file
 * VBox Qt GUI - UIRecordingVideoFrameRateEditor class implementation.
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
#include <QSpinBox>

/* GUI includes: */
#include "QIAdvancedSlider.h"
#include "UICommon.h"
#include "UIRecordingVideoFrameRateEditor.h"

UIRecordingVideoFrameRateEditor::UIRecordingVideoFrameRateEditor(QWidget *pParent /* = 0 */)
: UIEditor(pParent)
    , m_pLabel(0)
    , m_pSlider(0)
    , m_pSpinbox(0)
    , m_pLabelMin(0)
    , m_pLabelMax(0)
{
    prepare();
}

void UIRecordingVideoFrameRateEditor::setFrameRate(int iRate)
{
    if (!m_pSpinbox || m_pSpinbox->value() == iRate)
        return;
    m_pSpinbox->setValue(iRate);
}

int UIRecordingVideoFrameRateEditor::frameRate() const
{
    return m_pSpinbox ? m_pSpinbox->value() : 0;
}

int UIRecordingVideoFrameRateEditor::minimumLabelHorizontalHint() const
{
    return m_pLabel ? m_pLabel->minimumSizeHint().width() : 0;
}

void UIRecordingVideoFrameRateEditor::setMinimumLayoutIndent(int iIndent)
{
    if (m_pLayout)
        m_pLayout->setColumnMinimumWidth(0, iIndent + m_pLayout->spacing());
}

void UIRecordingVideoFrameRateEditor::sltRetranslateUI()
{
    m_pLabel->setText(tr("Frame R&ate"));
    m_pSlider->setToolTip(tr("Maximum number of frames per second. Additional frames "
                             "will be skipped. Reducing this value will increase the number of skipped "
                             "frames and reduce the file size."));
    m_pSpinbox->setSuffix(QString(" %1").arg(tr("fps")));
    m_pSpinbox->setToolTip(tr("Maximum number of frames per second. Additional frames "
                              "will be skipped. Reducing this value will increase the number of skipped "
                              "frames and reduce the file size."));
    m_pLabelMin->setText(tr("%1 fps").arg(m_pSlider->minimum()));
    m_pLabelMin->setToolTip(tr("Minimum recording frame rate"));
    m_pLabelMax->setText(tr("%1 fps").arg(m_pSlider->maximum()));
    m_pLabelMax->setToolTip(tr("Maximum recording frame rate"));
}

void UIRecordingVideoFrameRateEditor::sltHandleFrameRateSliderChange()
{
    /* Apply proposed frame-rate: */
    m_pSpinbox->blockSignals(true);
    m_pSpinbox->setValue(m_pSlider->value());
    m_pSpinbox->blockSignals(false);
    emit sigFrameRateChanged(m_pSlider->value());
}

void UIRecordingVideoFrameRateEditor::sltHandleFrameRateSpinboxChange()
{
    /* Apply proposed frame-rate: */
    m_pSlider->blockSignals(true);
    m_pSlider->setValue(m_pSpinbox->value());
    m_pSlider->blockSignals(false);
    emit sigFrameRateChanged(m_pSpinbox->value());
}

void UIRecordingVideoFrameRateEditor::prepare()
{
    /* Prepare everything: */
    prepareWidgets();
    prepareConnections();

    /* Apply language settings: */
    sltRetranslateUI();
}

void UIRecordingVideoFrameRateEditor::prepareWidgets()
{
    /* Prepare main layout: */
    m_pLayout = new QGridLayout(this);
    if (m_pLayout)
    {
        m_pLayout->setContentsMargins(0, 0, 0, 0);
        m_pLayout->setColumnStretch(2, 1); // stretch between min and max labels

        /* Prepare label: */
        m_pLabel = new QLabel(this);
        if (m_pLabel)
        {
            m_pLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_pLayout->addWidget(m_pLabel, 0, 0);
        }

        /* Prepare slider: */
        m_pSlider = new QIAdvancedSlider(this);
        if (m_pSlider)
        {
            m_pSlider->setOrientation(Qt::Horizontal);
            m_pSlider->setMinimum(1);
            m_pSlider->setMaximum(30);
            m_pSlider->setPageStep(1);
            m_pSlider->setSingleStep(1);
            m_pSlider->setTickInterval(1);
            m_pSlider->setSnappingEnabled(true);
            m_pSlider->setOptimalHint(1, 25);
            m_pSlider->setWarningHint(25, 30);
            m_pLayout->addWidget(m_pSlider, 0, 1, 1, 3);
        }

        /* Prepare min label: */
        m_pLabelMin = new QLabel(this);
        if (m_pLabelMin)
            m_pLayout->addWidget(m_pLabelMin, 1, 1);
        /* Prepare max label: */
        m_pLabelMax = new QLabel(this);
        if (m_pLabelMax)
            m_pLayout->addWidget(m_pLabelMax, 1, 3);

        /* Prepare spinbox: */
        m_pSpinbox = new QSpinBox(this);
        if (m_pSpinbox)
        {
            if (m_pLabel)
                m_pLabel->setBuddy(m_pSpinbox);
            uiCommon().setMinimumWidthAccordingSymbolCount(m_pSpinbox, 3);
            m_pSpinbox->setMinimum(1);
            m_pSpinbox->setMaximum(30);
            m_pLayout->addWidget(m_pSpinbox, 0, 4);
        }
    }
}

void UIRecordingVideoFrameRateEditor::prepareConnections()
{
    connect(m_pSlider, &QIAdvancedSlider::valueChanged,
            this, &UIRecordingVideoFrameRateEditor::sltHandleFrameRateSliderChange);
    connect(m_pSpinbox, &QSpinBox::valueChanged,
            this, &UIRecordingVideoFrameRateEditor::sltHandleFrameRateSpinboxChange);
}
