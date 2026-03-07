/* $Id: UIRecordingSettingsEditor.cpp 112403 2026-01-11 19:29:08Z knut.osmundsen@oracle.com $ */
/** @file
 * VBox Qt GUI - UIRecordingSettingsEditor class implementation.
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
#include <QCheckBox>
#include <QGridLayout>

/* GUI includes: */
#include "UIRecordingAudioProfileEditor.h"
#include "UIRecordingScreenSelectorEditor.h"
#include "UIRecordingSettingsEditor.h"
#include "UIRecordingFilePathEditor.h"
#include "UIRecordingModeEditor.h"
#include "UIRecordingVideoBitrateEditor.h"
#include "UIRecordingVideoFrameRateEditor.h"
#include "UIRecordingVideoFrameSizeEditor.h"


UIRecordingSettingsEditor::UIRecordingSettingsEditor(QWidget *pParent /* = 0 */)
    : UIEditor(pParent, true /* show in basic mode */)
    , m_fFeatureEnabled(false)
    , m_fOptionsAvailable(false)
    , m_pCheckboxFeature(0)
    , m_pLayoutSettings(0)
    , m_pEditorMode(0)
    , m_pEditorFilePath(0)
    , m_pEditorFrameSize(0)
    , m_pEditorFrameRate(0)
    , m_pEditorBitrate(0)
    , m_pEditorAudioProfile(0)
    , m_pEditorScreenSelector(0)
{
    prepare();
}

void UIRecordingSettingsEditor::setFeatureEnabled(bool fEnabled)
{
    /* Update cached value and
     * check-box if value has changed: */
    if (m_fFeatureEnabled != fEnabled)
    {
        m_fFeatureEnabled = fEnabled;
        if (m_pCheckboxFeature)
        {
            m_pCheckboxFeature->setChecked(m_fFeatureEnabled);
            sltHandleFeatureToggle();
        }
    }
}

bool UIRecordingSettingsEditor::isFeatureEnabled() const
{
    return m_pCheckboxFeature ? m_pCheckboxFeature->isChecked() : m_fFeatureEnabled;
}

void UIRecordingSettingsEditor::setOptionsAvailable(bool fAvailable)
{
    /* Update cached value and
     * widget availability if value has changed: */
    if (m_fOptionsAvailable != fAvailable)
    {
        m_fOptionsAvailable = fAvailable;
        updateWidgetAvailability();
    }
}

void UIRecordingSettingsEditor::setMode(UISettingsDefs::RecordingMode enmMode)
{
    if (m_pEditorMode)
        m_pEditorMode->setMode(enmMode);
}

UISettingsDefs::RecordingMode UIRecordingSettingsEditor::mode() const
{
    return m_pEditorMode ? m_pEditorMode->mode() : UISettingsDefs::RecordingMode_None;
}

void UIRecordingSettingsEditor::setFolder(const QString &strFolder)
{
    if (m_pEditorFilePath)
        m_pEditorFilePath->setFolder(strFolder);
}

QString UIRecordingSettingsEditor::folder() const
{
    return m_pEditorFilePath ? m_pEditorFilePath->folder() : QString();
}

void UIRecordingSettingsEditor::setFilePath(const QString &strFilePath)
{
    if (m_pEditorFilePath)
        m_pEditorFilePath->setFilePath(strFilePath);
}

QString UIRecordingSettingsEditor::filePath() const
{
    return m_pEditorFilePath ? m_pEditorFilePath->filePath() : QString();
}

void UIRecordingSettingsEditor::setFrameWidth(int iWidth)
{
    if (m_pEditorFrameSize)
        m_pEditorFrameSize->setFrameWidth(iWidth);
}

int UIRecordingSettingsEditor::frameWidth() const
{
    return m_pEditorFrameSize ? m_pEditorFrameSize->frameWidth() : 0;
}

void UIRecordingSettingsEditor::setFrameHeight(int iHeight)
{
    if (m_pEditorFrameSize)
        m_pEditorFrameSize->setFrameHeight(iHeight);
}

int UIRecordingSettingsEditor::frameHeight() const
{
    return m_pEditorFrameSize ? m_pEditorFrameSize->frameHeight() : 0;
}

void UIRecordingSettingsEditor::setFrameRate(int iRate)
{
    if (m_pEditorFrameRate)
        m_pEditorFrameRate->setFrameRate(iRate);
}

int UIRecordingSettingsEditor::frameRate() const
{
    return m_pEditorFrameRate ? m_pEditorFrameRate->frameRate() : 0;
}

void UIRecordingSettingsEditor::setBitrate(int iBitrate)
{
    if (m_pEditorBitrate)
        m_pEditorBitrate->setBitrate(iBitrate);
}

int UIRecordingSettingsEditor::bitrate() const
{
    return m_pEditorBitrate ? m_pEditorBitrate->bitrate() : 0;
}

void UIRecordingSettingsEditor::setAudioProfile(const QString &strProfile)
{
    if (m_pEditorAudioProfile)
        m_pEditorAudioProfile->setAudioProfile(strProfile);
}

QString UIRecordingSettingsEditor::audioProfile() const
{
    return m_pEditorAudioProfile ? m_pEditorAudioProfile->audioProfile() : QString();
}

void UIRecordingSettingsEditor::setScreens(const QVector<bool> &screens)
{
    if (m_pEditorScreenSelector)
        m_pEditorScreenSelector->setScreens(screens);
}

QVector<bool> UIRecordingSettingsEditor::screens() const
{
    return m_pEditorScreenSelector ? m_pEditorScreenSelector->screens() : QVector<bool>();
}

void UIRecordingSettingsEditor::handleFilterChange()
{
    updateMinimumLayoutHint();
}

void UIRecordingSettingsEditor::sltRetranslateUI()
{
    m_pCheckboxFeature->setText(tr("&Enable Recording"));
    m_pCheckboxFeature->setToolTip(tr("VirtualBox will record the virtual machine session as a video file"));

    updateMinimumLayoutHint();
}

void UIRecordingSettingsEditor::sltHandleFeatureToggle()
{
    /* Update widget availability: */
    updateWidgetAvailability();
}

void UIRecordingSettingsEditor::sltHandleModeChange()
{
    /* Update widget availability: */
    updateWidgetAvailability();
}

void UIRecordingSettingsEditor::sltHandleVideoQualityChange()
{
    /* Calculate/apply proposed bitrate: */
    m_pEditorBitrate->blockSignals(true);
    m_pEditorBitrate->setBitrate(calculateBitrate(m_pEditorFrameSize->frameWidth(),
                                                  m_pEditorFrameSize->frameHeight(),
                                                  m_pEditorFrameRate->frameRate(),
                                                  m_pEditorBitrate->quality()));
    m_pEditorBitrate->blockSignals(false);
}

void UIRecordingSettingsEditor::sltHandleVideoBitrateChange(int iBitrate)
{
    /* Calculate/apply proposed quality: */
    m_pEditorBitrate->blockSignals(true);
    m_pEditorBitrate->setQuality(calculateQuality(m_pEditorFrameSize->frameWidth(),
                                                  m_pEditorFrameSize->frameHeight(),
                                                  m_pEditorFrameRate->frameRate(),
                                                  iBitrate));
    m_pEditorBitrate->blockSignals(false);
}

void UIRecordingSettingsEditor::prepare()
{
    /* Prepare everything: */
    prepareWidgets();
    prepareConnections();

    /* Apply language settings: */
    sltRetranslateUI();
}

void UIRecordingSettingsEditor::prepareWidgets()
{
    /* Prepare main layout: */
    QGridLayout *pLayout = new QGridLayout(this);
    if (pLayout)
    {
        pLayout->setContentsMargins(0, 0, 0, 0);
        pLayout->setColumnStretch(1, 1);

        /* Prepare 'feature' check-box: */
        m_pCheckboxFeature = new QCheckBox(this);
        if (m_pCheckboxFeature)
        {
            // this name is used from outside, have a look at UIMachineLogic..
            m_pCheckboxFeature->setObjectName("m_pCheckboxVideoCapture");
            pLayout->addWidget(m_pCheckboxFeature, 0, 0, 1, 2);
        }

        /* Prepare 20-px shifting spacer: */
        QSpacerItem *pSpacerItem = new QSpacerItem(20, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
        if (pSpacerItem)
            pLayout->addItem(pSpacerItem, 1, 0);

        /* Prepare 'settings' widget: */
        QWidget *pWidgetSettings = new QWidget(this);
        if (pWidgetSettings)
        {
            /* Prepare recording settings widget layout: */
            m_pLayoutSettings = new QGridLayout(pWidgetSettings);
            if (m_pLayoutSettings)
            {
                m_pLayoutSettings->setContentsMargins(0, 0, 0, 0);
                int iLayoutSettingsRow = 0;

                /* Prepare recording mode editor: */
                m_pEditorMode = new UIRecordingModeEditor(pWidgetSettings);
                if (m_pEditorMode)
                {
                    addEditor(m_pEditorMode);
                    m_pLayoutSettings->addWidget(m_pEditorMode, ++iLayoutSettingsRow, 0);
                }
                /* Prepare recording file path editor: */
                m_pEditorFilePath = new UIRecordingFilePathEditor(pWidgetSettings);
                if (m_pEditorFilePath)
                {
                    addEditor(m_pEditorFilePath);
                    m_pLayoutSettings->addWidget(m_pEditorFilePath, ++iLayoutSettingsRow, 0);
                }
                /* Prepare recording frame size editor: */
                m_pEditorFrameSize = new UIRecordingVideoFrameSizeEditor(pWidgetSettings);
                if (m_pEditorFrameSize)
                {
                    addEditor(m_pEditorFrameSize);
                    m_pLayoutSettings->addWidget(m_pEditorFrameSize, ++iLayoutSettingsRow, 0);
                }
                /* Prepare recording frame rate editor: */
                m_pEditorFrameRate = new UIRecordingVideoFrameRateEditor(pWidgetSettings);
                if (m_pEditorFrameRate)
                {
                    addEditor(m_pEditorFrameRate);
                    m_pLayoutSettings->addWidget(m_pEditorFrameRate, ++iLayoutSettingsRow, 0);
                }
                /* Prepare recording bitrate editor: */
                m_pEditorBitrate = new UIRecordingVideoBitrateEditor(pWidgetSettings);
                if (m_pEditorBitrate)
                {
                    addEditor(m_pEditorBitrate);
                    m_pLayoutSettings->addWidget(m_pEditorBitrate, ++iLayoutSettingsRow, 0);
                }
                /* Prepare recording audio profile editor: */
                m_pEditorAudioProfile = new UIRecordingAudioProfileEditor(pWidgetSettings);
                if (m_pEditorAudioProfile)
                {
                    addEditor(m_pEditorAudioProfile);
                    m_pLayoutSettings->addWidget(m_pEditorAudioProfile, ++iLayoutSettingsRow, 0);
                }
                /* Prepare screen selector editor: */
                m_pEditorScreenSelector = new UIRecordingScreenSelectorEditor(this);
                if (m_pEditorScreenSelector)
                {
                    addEditor(m_pEditorScreenSelector);
                    m_pLayoutSettings->addWidget(m_pEditorScreenSelector, ++iLayoutSettingsRow, 0);
                }
            }
            pLayout->addWidget(pWidgetSettings, 1, 1, 1, 2);
        }
    }

    /* Update widget availability: */
    updateWidgetAvailability();
}

void UIRecordingSettingsEditor::prepareConnections()
{
    connect(m_pCheckboxFeature, &QCheckBox::toggled,
            this, &UIRecordingSettingsEditor::sltHandleFeatureToggle);
    connect(m_pEditorMode, &UIRecordingModeEditor::sigModeChange,
            this, &UIRecordingSettingsEditor::sltHandleModeChange);
    connect(m_pEditorFrameSize, &UIRecordingVideoFrameSizeEditor::sigFrameSizeChanged,
            this, &UIRecordingSettingsEditor::sltHandleVideoQualityChange);
    connect(m_pEditorFrameRate, &UIRecordingVideoFrameRateEditor::sigFrameRateChanged,
            this, &UIRecordingSettingsEditor::sltHandleVideoQualityChange);
    connect(m_pEditorBitrate, &UIRecordingVideoBitrateEditor::sigVideoQualityChanged,
            this, &UIRecordingSettingsEditor::sltHandleVideoQualityChange);
    connect(m_pEditorBitrate, &UIRecordingVideoBitrateEditor::sigVideoBitrateChanged,
            this, &UIRecordingSettingsEditor::sltHandleVideoBitrateChange);
}

void UIRecordingSettingsEditor::updateWidgetAvailability()
{
    const bool fFeatureEnabled = m_pCheckboxFeature->isChecked();
    const UISettingsDefs::RecordingMode enmRecordingMode = m_pEditorMode->mode();
    const bool fRecordVideo =    enmRecordingMode == UISettingsDefs::RecordingMode_VideoOnly
                              || enmRecordingMode == UISettingsDefs::RecordingMode_VideoAudio;
    const bool fRecordAudio =    enmRecordingMode == UISettingsDefs::RecordingMode_AudioOnly
                              || enmRecordingMode == UISettingsDefs::RecordingMode_VideoAudio;

    m_pEditorMode->setEnabled(fFeatureEnabled && m_fOptionsAvailable);
    m_pEditorFilePath->setEnabled(fFeatureEnabled && m_fOptionsAvailable);
    m_pEditorFrameSize->setEnabled(fFeatureEnabled && m_fOptionsAvailable && fRecordVideo);
    m_pEditorFrameRate->setEnabled(fFeatureEnabled && m_fOptionsAvailable && fRecordVideo);
    m_pEditorBitrate->setEnabled(fFeatureEnabled && m_fOptionsAvailable && fRecordVideo);
    m_pEditorAudioProfile->setEnabled(fFeatureEnabled && m_fOptionsAvailable && fRecordAudio);
    m_pEditorScreenSelector->setEnabled(fFeatureEnabled && m_fOptionsAvailable && fRecordVideo);
}

void UIRecordingSettingsEditor::updateMinimumLayoutHint()
{
    /* Layout all the editors: */
    int iMinimumLayoutHint = 0;
    /* The following editors have own labels, but we want them to be properly layouted according to rest of stuff: */
    if (m_pEditorMode && !m_pEditorMode->isHidden())
        iMinimumLayoutHint = qMax(iMinimumLayoutHint, m_pEditorMode->minimumLabelHorizontalHint());
    if (m_pEditorFilePath && !m_pEditorFilePath->isHidden())
        iMinimumLayoutHint = qMax(iMinimumLayoutHint, m_pEditorFilePath->minimumLabelHorizontalHint());
    if (m_pEditorFrameSize && !m_pEditorFrameSize->isHidden())
        iMinimumLayoutHint = qMax(iMinimumLayoutHint, m_pEditorFrameSize->minimumLabelHorizontalHint());
    if (m_pEditorFrameRate && !m_pEditorFrameRate->isHidden())
        iMinimumLayoutHint = qMax(iMinimumLayoutHint, m_pEditorFrameRate->minimumLabelHorizontalHint());
    if (m_pEditorBitrate && !m_pEditorBitrate->isHidden())
        iMinimumLayoutHint = qMax(iMinimumLayoutHint, m_pEditorBitrate->minimumLabelHorizontalHint());
    if (m_pEditorAudioProfile && !m_pEditorAudioProfile->isHidden())
        iMinimumLayoutHint = qMax(iMinimumLayoutHint, m_pEditorAudioProfile->minimumLabelHorizontalHint());
    if (m_pEditorScreenSelector && !m_pEditorScreenSelector->isHidden())
        iMinimumLayoutHint = qMax(iMinimumLayoutHint, m_pEditorScreenSelector->minimumLabelHorizontalHint());
    if (m_pEditorMode)
        m_pEditorMode->setMinimumLayoutIndent(iMinimumLayoutHint);
    if (m_pEditorFilePath)
        m_pEditorFilePath->setMinimumLayoutIndent(iMinimumLayoutHint);
    if (m_pEditorFrameRate)
        m_pEditorFrameRate->setMinimumLayoutIndent(iMinimumLayoutHint);
    if (m_pEditorFrameSize)
        m_pEditorFrameSize->setMinimumLayoutIndent(iMinimumLayoutHint);
    if (m_pEditorBitrate)
        m_pEditorBitrate->setMinimumLayoutIndent(iMinimumLayoutHint);
    if (m_pEditorAudioProfile)
        m_pEditorAudioProfile->setMinimumLayoutIndent(iMinimumLayoutHint);
    if (m_pEditorScreenSelector)
        m_pEditorScreenSelector->setMinimumLayoutIndent(iMinimumLayoutHint);
    if (m_pLayoutSettings)
        m_pLayoutSettings->setColumnMinimumWidth(0, iMinimumLayoutHint);
}

/* static */
int UIRecordingSettingsEditor::calculateBitrate(int iFrameWidth, int iFrameHeight, int iFrameRate, int iQuality)
{
    /* Linear quality<=>bitrate scale-factor: */
    const double dResult = (double)iQuality
                         * (double)iFrameWidth * (double)iFrameHeight * (double)iFrameRate
                         / (double)10 /* translate quality to [%] */
                         / (double)1024 /* translate bitrate to [kbps] */
                         / (double)18.75 /* linear scale factor */;
    return (int)dResult;
}

/* static */
int UIRecordingSettingsEditor::calculateQuality(int iFrameWidth, int iFrameHeight, int iFrameRate, int iBitRate)
{
    /* Linear bitrate<=>quality scale-factor: */
    const double dResult = (double)iBitRate
                         / (double)iFrameWidth / (double)iFrameHeight / (double)iFrameRate
                         * (double)10 /* translate quality to [%] */
                         * (double)1024 /* translate bitrate to [kbps] */
                         * (double)18.75 /* linear scale factor */;
    return (int)dResult;
}
