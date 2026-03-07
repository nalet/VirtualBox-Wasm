/* $Id: UIRecordingVideoBitrateEditor.cpp 112403 2026-01-11 19:29:08Z knut.osmundsen@oracle.com $ */
/** @file
 * VBox Qt GUI - UIRecordingVideoBitrateEditor class implementation.
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
#include "UIRecordingVideoBitrateEditor.h"

/* Defines: */
#define VIDEO_CAPTURE_BIT_RATE_MIN 32
#define VIDEO_CAPTURE_BIT_RATE_MAX 2048


UIRecordingVideoBitrateEditor::UIRecordingVideoBitrateEditor(QWidget *pParent /* = 0 */)
    : UIEditor(pParent, true /* show in basic mode? */)
    , m_iQuality(0)
    , m_iBitrate(0)
    , m_pLayout(0)
    , m_pLabel(0)
    , m_pSlider(0)
    , m_pSpinbox(0)
    , m_pLabelMin(0)
    , m_pLabelMed(0)
    , m_pLabelMax(0)
{
    prepare();
}

void UIRecordingVideoBitrateEditor::setQuality(int iQuality)
{
    /* Update cached value and
     * spin-box if value has changed: */
    if (m_iQuality != iQuality)
    {
        m_iQuality = iQuality;
        if (m_pSlider)
            m_pSlider->setValue(m_iQuality);
    }
}

int UIRecordingVideoBitrateEditor::quality() const
{
    return m_pSlider ? m_pSlider->value() : m_iQuality;
}

void UIRecordingVideoBitrateEditor::setBitrate(int iBitrate)
{
    /* Update cached value and
     * spin-box if value has changed: */
    if (m_iBitrate != iBitrate)
    {
        m_iBitrate = iBitrate;
        if (m_pSpinbox)
            m_pSpinbox->setValue(m_iBitrate);
    }
}

int UIRecordingVideoBitrateEditor::bitrate() const
{
    return m_pSpinbox ? m_pSpinbox->value() : m_iBitrate;
}

int UIRecordingVideoBitrateEditor::minimumLabelHorizontalHint() const
{
    return m_pLabel ? m_pLabel->minimumSizeHint().width() : 0;
}

void UIRecordingVideoBitrateEditor::setMinimumLayoutIndent(int iIndent)
{
    if (m_pLayout)
        m_pLayout->setColumnMinimumWidth(0, iIndent + m_pLayout->spacing());
}

void UIRecordingVideoBitrateEditor::sltRetranslateUI()
{
    m_pLabel->setText(tr("&Bitrate"));
    m_pSlider->setToolTip(tr("Bitrate. Increasing this value will make the video "
                             "look better at the cost of an increased file size."));
    m_pSpinbox->setSuffix(QString(" %1").arg(tr("kbps")));
    m_pSpinbox->setToolTip(tr("Bitrate in kilobits per second. Increasing this value "
                              "will make the video look better at the cost of an increased file size."));
    m_pLabelMin->setText(tr("low", "bitrate"));
    m_pLabelMed->setText(tr("medium", "bitrate"));
    m_pLabelMax->setText(tr("high", "bitrate"));
}

void UIRecordingVideoBitrateEditor::sltHandleBitrateSliderChange()
{
    emit sigVideoQualityChanged(m_pSlider->value());
}

void UIRecordingVideoBitrateEditor::sltHandleBitrateSpinboxChange()
{
    emit sigVideoBitrateChanged(m_pSpinbox->value());
}

void UIRecordingVideoBitrateEditor::prepare()
{
    /* Prepare everything: */
    prepareWidgets();
    prepareConnections();

    /* Apply language settings: */
    sltRetranslateUI();
}

void UIRecordingVideoBitrateEditor::prepareWidgets()
{
    /* Prepare main layout: */
    m_pLayout = new QGridLayout(this);
    if (m_pLayout)
    {
        m_pLayout->setContentsMargins(0, 0, 0, 0);
        m_pLayout->setColumnStretch(2, 1); // stretch between min and med labels
        m_pLayout->setColumnStretch(4, 1); // stretch between med and max labels

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
            m_pSlider->setMaximum(10);
            m_pSlider->setPageStep(1);
            m_pSlider->setSingleStep(1);
            m_pSlider->setTickInterval(1);
            m_pSlider->setSnappingEnabled(true);
            m_pSlider->setOptimalHint(1, 5);
            m_pSlider->setWarningHint(5, 9);
            m_pSlider->setErrorHint(9, 10);
            m_pLayout->addWidget(m_pSlider, 0, 1, 1, 5);
        }

        /* Prepare min label: */
        m_pLabelMin = new QLabel(this);
        if (m_pLabelMin)
            m_pLayout->addWidget(m_pLabelMin, 1, 1);
        /* Prepare med label: */
        m_pLabelMed = new QLabel(this);
        if (m_pLabelMed)
            m_pLayout->addWidget(m_pLabelMed, 1, 3);
        /* Prepare max label: */
        m_pLabelMax = new QLabel(this);
        if (m_pLabelMax)
            m_pLayout->addWidget(m_pLabelMax, 1, 5);

        /* Prepare spinbox: */
        m_pSpinbox = new QSpinBox(this);
        if (m_pSpinbox)
        {
            if (m_pLabel)
                m_pLabel->setBuddy(m_pSpinbox);
            uiCommon().setMinimumWidthAccordingSymbolCount(m_pSpinbox, 5);
            m_pSpinbox->setMinimum(VIDEO_CAPTURE_BIT_RATE_MIN);
            m_pSpinbox->setMaximum(VIDEO_CAPTURE_BIT_RATE_MAX);
            m_pLayout->addWidget(m_pSpinbox, 0, 6);
        }
    }
}

void UIRecordingVideoBitrateEditor::prepareConnections()
{
    connect(m_pSlider, &QIAdvancedSlider::valueChanged,
            this, &UIRecordingVideoBitrateEditor::sltHandleBitrateSliderChange);
    connect(m_pSpinbox, &QSpinBox::valueChanged,
            this, &UIRecordingVideoBitrateEditor::sltHandleBitrateSpinboxChange);
}
