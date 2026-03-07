/* $Id: UIRecordingVideoFrameSizeEditor.cpp 112403 2026-01-11 19:29:08Z knut.osmundsen@oracle.com $ */
/** @file
 * VBox Qt GUI - UIRecordingVideoFrameSizeEditor class implementation.
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
#include <QSpinBox>

/* GUI includes: */
#include "UICommon.h"
#include "UIRecordingVideoFrameSizeEditor.h"


UIRecordingVideoFrameSizeEditor::UIRecordingVideoFrameSizeEditor(QWidget *pParent /* = 0 */)
    : UIEditor(pParent)
    , m_iFrameWidth(0)
    , m_iFrameHeight(0)
    , m_pLayout(0)
    , m_pLabel(0)
    , m_pCombo(0)
    , m_pSpinboxWidth(0)
    , m_pSpinboxHeight(0)
{
    prepare();
}

void UIRecordingVideoFrameSizeEditor::setFrameWidth(int iWidth)
{
    /* Update cached value and
     * spin-box if value has changed: */
    if (m_iFrameWidth != iWidth)
    {
        m_iFrameWidth = iWidth;
        if (m_pSpinboxWidth)
            m_pSpinboxWidth->setValue(m_iFrameWidth);
    }
}

int UIRecordingVideoFrameSizeEditor::frameWidth() const
{
    return m_pSpinboxWidth ? m_pSpinboxWidth->value() : m_iFrameWidth;
}

void UIRecordingVideoFrameSizeEditor::setFrameHeight(int iHeight)
{
    /* Update cached value and
     * spin-box if value has changed: */
    if (m_iFrameHeight != iHeight)
    {
        m_iFrameHeight = iHeight;
        if (m_pSpinboxHeight)
            m_pSpinboxHeight->setValue(m_iFrameHeight);
    }
}

int UIRecordingVideoFrameSizeEditor::frameHeight() const
{
    return m_pSpinboxHeight ? m_pSpinboxHeight->value() : m_iFrameHeight;
}

int UIRecordingVideoFrameSizeEditor::minimumLabelHorizontalHint() const
{
    return m_pLabel ? m_pLabel->minimumSizeHint().width() : 0;
}

void UIRecordingVideoFrameSizeEditor::setMinimumLayoutIndent(int iIndent)
{
    if (m_pLayout)
        m_pLayout->setColumnMinimumWidth(0, iIndent + m_pLayout->spacing());
}

void UIRecordingVideoFrameSizeEditor::sltRetranslateUI()
{
    m_pLabel->setText(tr("Frame Si&ze"));
    m_pCombo->setItemText(0, tr("User Defined"));
    m_pCombo->setToolTip(tr("Resolution (frame size) of the recorded video"));
    m_pSpinboxWidth->setToolTip(tr("Horizontal resolution (frame width) of the recorded video"));
    m_pSpinboxHeight->setToolTip(tr("Vertical resolution (frame height) of the recorded video"));
}

void UIRecordingVideoFrameSizeEditor::sltHandleFrameSizeComboChange()
{
    /* Get the proposed size: */
    const QSize frameSize = m_pCombo->currentData().toSize();
    if (!frameSize.isValid())
        return;

    /* Apply proposed size: */
    m_pSpinboxWidth->setValue(frameSize.width());
    m_pSpinboxHeight->setValue(frameSize.height());
}

void UIRecordingVideoFrameSizeEditor::sltHandleFrameWidthChange()
{
    /* Look for preset: */
    lookForCorrespondingFrameSizePreset();
    /* Update quality and bit rate: */
    emit sigFrameSizeChanged();
}

void UIRecordingVideoFrameSizeEditor::sltHandleFrameHeightChange()
{
    /* Look for preset: */
    lookForCorrespondingFrameSizePreset();
    /* Update quality and bit rate: */
    emit sigFrameSizeChanged();
}

void UIRecordingVideoFrameSizeEditor::prepare()
{
    /* Prepare everything: */
    prepareWidgets();
    prepareConnections();

    /* Apply language settings: */
    sltRetranslateUI();
}

void UIRecordingVideoFrameSizeEditor::prepareWidgets()
{
    /* Prepare main layout: */
    m_pLayout = new QGridLayout(this);
    if (m_pLayout)
    {
        m_pLayout->setContentsMargins(0, 0, 0, 0);

        /* Prepare recording frame size label: */
        m_pLabel = new QLabel(this);
        if (m_pLabel)
        {
            m_pLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_pLayout->addWidget(m_pLabel, 0, 0);
        }

        /* Prepare recording frame size combo: */
        m_pCombo = new QComboBox(this);
        if (m_pCombo)
        {
            if (m_pLabel)
                m_pLabel->setBuddy(m_pCombo);
            m_pCombo->setSizePolicy(QSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed));
            m_pCombo->addItem(""); /* User Defined */
            m_pCombo->addItem("320 x 200 (16:10)",   QSize(320, 200));
            m_pCombo->addItem("640 x 480 (4:3)",     QSize(640, 480));
            m_pCombo->addItem("720 x 400 (9:5)",     QSize(720, 400));
            m_pCombo->addItem("720 x 480 (3:2)",     QSize(720, 480));
            m_pCombo->addItem("800 x 600 (4:3)",     QSize(800, 600));
            m_pCombo->addItem("1024 x 768 (4:3)",    QSize(1024, 768));
            m_pCombo->addItem("1152 x 864 (4:3)",    QSize(1152, 864));
            m_pCombo->addItem("1280 x 720 (16:9)",   QSize(1280, 720));
            m_pCombo->addItem("1280 x 800 (16:10)",  QSize(1280, 800));
            m_pCombo->addItem("1280 x 960 (4:3)",    QSize(1280, 960));
            m_pCombo->addItem("1280 x 1024 (5:4)",   QSize(1280, 1024));
            m_pCombo->addItem("1366 x 768 (16:9)",   QSize(1366, 768));
            m_pCombo->addItem("1440 x 900 (16:10)",  QSize(1440, 900));
            m_pCombo->addItem("1440 x 1080 (4:3)",   QSize(1440, 1080));
            m_pCombo->addItem("1600 x 900 (16:9)",   QSize(1600, 900));
            m_pCombo->addItem("1680 x 1050 (16:10)", QSize(1680, 1050));
            m_pCombo->addItem("1600 x 1200 (4:3)",   QSize(1600, 1200));
            m_pCombo->addItem("1920 x 1080 (16:9)",  QSize(1920, 1080));
            m_pCombo->addItem("1920 x 1200 (16:10)", QSize(1920, 1200));
            m_pCombo->addItem("1920 x 1440 (4:3)",   QSize(1920, 1440));
            m_pCombo->addItem("2880 x 1800 (16:10)", QSize(2880, 1800));

            m_pLayout->addWidget(m_pCombo, 0, 1);
        }

        /* Prepare recording frame width spinbox: */
        m_pSpinboxWidth = new QSpinBox(this);
        if (m_pSpinboxWidth)
        {
            uiCommon().setMinimumWidthAccordingSymbolCount(m_pSpinboxWidth, 5);
            m_pSpinboxWidth->setMinimum(16);
            m_pSpinboxWidth->setMaximum(2880);

            m_pLayout->addWidget(m_pSpinboxWidth, 0, 2);
        }

        /* Prepare recording frame height spinbox: */
        m_pSpinboxHeight = new QSpinBox(this);
        if (m_pSpinboxHeight)
        {
            uiCommon().setMinimumWidthAccordingSymbolCount(m_pSpinboxHeight, 5);
            m_pSpinboxHeight->setMinimum(16);
            m_pSpinboxHeight->setMaximum(1800);

            m_pLayout->addWidget(m_pSpinboxHeight, 0, 3);
        }
    }
}

void UIRecordingVideoFrameSizeEditor::prepareConnections()
{
    connect(m_pCombo, &QComboBox:: currentIndexChanged,
            this, &UIRecordingVideoFrameSizeEditor::sltHandleFrameSizeComboChange);
    connect(m_pSpinboxWidth, &QSpinBox::valueChanged,
            this, &UIRecordingVideoFrameSizeEditor::sltHandleFrameWidthChange);
    connect(m_pSpinboxHeight, &QSpinBox::valueChanged,
            this, &UIRecordingVideoFrameSizeEditor::sltHandleFrameHeightChange);
}

void UIRecordingVideoFrameSizeEditor::lookForCorrespondingFrameSizePreset()
{
    /* Use passed iterator to look for corresponding preset of passed combo-box: */
    const int iLookupResult = m_pCombo->findData(QSize(m_pSpinboxWidth->value(), m_pSpinboxHeight->value()));
    if (iLookupResult != -1 && m_pCombo->currentIndex() != iLookupResult)
        m_pCombo->setCurrentIndex(iLookupResult);
    else if (iLookupResult == -1 && m_pCombo->currentIndex() != 0)
        m_pCombo->setCurrentIndex(0);
}
