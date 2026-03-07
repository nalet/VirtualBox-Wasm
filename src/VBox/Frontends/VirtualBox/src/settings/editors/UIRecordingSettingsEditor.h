/* $Id: UIRecordingSettingsEditor.h 112403 2026-01-11 19:29:08Z knut.osmundsen@oracle.com $ */
/** @file
 * VBox Qt GUI - UIRecordingSettingsEditor class declaration.
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

#ifndef FEQT_INCLUDED_SRC_settings_editors_UIRecordingSettingsEditor_h
#define FEQT_INCLUDED_SRC_settings_editors_UIRecordingSettingsEditor_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

/* GUI includes: */
#include "UIEditor.h"
#include "UISettingsDefs.h"

/* Forward declarations: */
class QCheckBox;
class QGridLayout;
class QWidget;
class UIRecordingAudioProfileEditor;
class UIRecordingFilePathEditor;
class UIRecordingModeEditor;
class UIRecordingScreenSelectorEditor;
class UIRecordingVideoBitrateEditor;
class UIRecordingVideoFrameRateEditor;
class UIRecordingVideoFrameSizeEditor;

/** UIEditor sub-class used as a recording settings editor. */
class SHARED_LIBRARY_STUFF UIRecordingSettingsEditor : public UIEditor
{
    Q_OBJECT;

public:

    /** Constructs editor passing @a pParent to the base-class. */
    UIRecordingSettingsEditor(QWidget *pParent = 0);

    /** Defines whether feature is @a fEnabled. */
    void setFeatureEnabled(bool fEnabled);
    /** Returns whether feature is enabled. */
    bool isFeatureEnabled() const;

    /** Defines whether options are @a fAvailable. */
    void setOptionsAvailable(bool fAvailable);

    /** Defines @a enmMode. */
    void setMode(UISettingsDefs::RecordingMode enmMode);
    /** Return mode. */
    UISettingsDefs::RecordingMode mode() const;

    /** Defines @a strFolder. */
    void setFolder(const QString &strFolder);
    /** Returns folder. */
    QString folder() const;
    /** Defines @a strFilePath. */
    void setFilePath(const QString &strFilePath);
    /** Returns file path. */
    QString filePath() const;

    /** Defines frame @a iWidth. */
    void setFrameWidth(int iWidth);
    /** Returns frame width. */
    int frameWidth() const;
    /** Defines frame @a iHeight. */
    void setFrameHeight(int iHeight);
    /** Returns frame height. */
    int frameHeight() const;

    /** Defines frame @a iRate. */
    void setFrameRate(int iRate);
    /** Returns frame rate. */
    int frameRate() const;

    /** Defines @a iBitrate. */
    void setBitrate(int iBitrate);
    /** Returns bitrate. */
    int bitrate() const;

    /** Defines audio @a strProfile. */
    void setAudioProfile(const QString &strProfile);
    /** Returns audio profile. */
    QString audioProfile() const;

    /** Defines enabled @a screens. */
    void setScreens(const QVector<bool> &screens);
    /** Returns enabled screens. */
    QVector<bool> screens() const;

protected:

    /** Handles filter change. */
    virtual void handleFilterChange() RT_OVERRIDE;

private slots:

    /** Handles translation event. */
    virtual void sltRetranslateUI() RT_OVERRIDE RT_FINAL;

    /** Handles feature toggling. */
    void sltHandleFeatureToggle();
    /** Handles mode change. */
    void sltHandleModeChange();
    /** Handles video quality change. */
    void sltHandleVideoQualityChange();
    /** Handles video bitrate change. */
    void sltHandleVideoBitrateChange(int iBitrate);

private:

    /** Prepares all. */
    void prepare();
    /** Prepares widgets. */
    void prepareWidgets();
    /** Prepares connections. */
    void prepareConnections();

    /** Updates widget availability. */
    void updateWidgetAvailability();
    /** Updates minimum layout hint. */
    void updateMinimumLayoutHint();

    /** Calculates recording bitrate for passed @a iFrameWidth, @a iFrameHeight, @a iFrameRate and @a iQuality. */
    static int calculateBitrate(int iFrameWidth, int iFrameHeight, int iFrameRate, int iQuality);
    /** Calculates recording quality for passed @a iFrameWidth, @a iFrameHeight, @a iFrameRate and @a iBitrate. */
    static int calculateQuality(int iFrameWidth, int iFrameHeight, int iFrameRate, int iBitRate);

    /** @name Values
     * @{ */
        /** Holds whether feature is enabled. */
        bool  m_fFeatureEnabled;
        /** Holds whether options are available. */
        bool  m_fOptionsAvailable;
    /** @} */

    /** @name Widgets
     * @{ */
        /** Holds the feature check-box instance. */
        QCheckBox                       *m_pCheckboxFeature;
        /** Holds the settings layout instance. */
        QGridLayout                     *m_pLayoutSettings;
        /** Holds the mode editor instance. */
        UIRecordingModeEditor           *m_pEditorMode;
        /** Holds the file path editor instance. */
        UIRecordingFilePathEditor       *m_pEditorFilePath;
        /** Holds the frame size editor instance. */
        UIRecordingVideoFrameSizeEditor *m_pEditorFrameSize;
        /** Holds the frame rate editor instance. */
        UIRecordingVideoFrameRateEditor *m_pEditorFrameRate;
        /** Holds the bitrate editor instance. */
        UIRecordingVideoBitrateEditor   *m_pEditorBitrate;
        /** Holds the audio profile editor instance. */
        UIRecordingAudioProfileEditor   *m_pEditorAudioProfile;
        /** Holds the screen selector editor instance. */
        UIRecordingScreenSelectorEditor *m_pEditorScreenSelector;
    /** @} */
};

#endif /* !FEQT_INCLUDED_SRC_settings_editors_UIRecordingSettingsEditor_h */
