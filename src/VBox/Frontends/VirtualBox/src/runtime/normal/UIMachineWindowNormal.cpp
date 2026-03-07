/* $Id: UIMachineWindowNormal.cpp 112954 2026-02-11 14:42:55Z sergey.dubov@oracle.com $ */
/** @file
 * VBox Qt GUI - UIMachineWindowNormal class implementation.
 */

/*
 * Copyright (C) 2010-2026 Oracle and/or its affiliates.
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
#include <QMenuBar>
#include <QTimerEvent>
#include <QContextMenuEvent>
#include <QResizeEvent>
#include <QScrollBar>
#ifdef VBOX_WS_NIX
# include <QTimer>
#endif

/* GUI includes: */
#include "UICommon.h"
#include "UIDesktopWidgetWatchdog.h"
#include "UIMachineWindowNormal.h"
#include "UIActionPoolRuntime.h"
#include "UIExtraDataManager.h"
#include "UIIndicatorsPool.h"
#include "UIKeyboardHandler.h"
#include "UILoggingDefs.h"
#include "UIMachine.h"
#include "UIMouseHandler.h"
#include "UIMachineLogic.h"
#include "UIMachineView.h"
#include "UINotificationCenter.h"
#include "UIIconPool.h"
#include "QIStatusBar.h"
#include "QIStatusBarIndicator.h"
#ifndef VBOX_WS_MAC
# include "UIMenuBar.h"
#else  /* VBOX_WS_MAC */
# include "UIImageTools.h"
# include "UICocoaApplication.h"
# include "UIVersion.h"
#endif /* VBOX_WS_MAC */

/* COM includes: */
#include "CConsole.h"
#include "CMediumAttachment.h"
#include "CUSBController.h"
#include "CUSBDeviceFilters.h"


UIMachineWindowNormal::UIMachineWindowNormal(UIMachineLogic *pMachineLogic, ulong uScreenId)
    : UIMachineWindow(pMachineLogic, uScreenId)
    , m_pIndicatorsPool(0)
    , m_iGeometrySaveTimerId(-1)
{
}

void UIMachineWindowNormal::sltMachineStateChanged()
{
    /* Call to base-class: */
    UIMachineWindow::sltMachineStateChanged();

    /* Update indicator-pool and virtualization stuff: */
    updateAppearanceOf(UIVisualElement_IndicatorPool);
}

#ifndef RT_OS_DARWIN
void UIMachineWindowNormal::sltHandleMenuBarConfigurationChange(const QUuid &uMachineID)
{
    /* Skip unrelated machine IDs: */
    if (uiCommon().managedVMUuid() != uMachineID)
        return;

    /* Check whether menu-bar is enabled: */
    const bool fEnabled = gEDataManager->menuBarEnabled(uiCommon().managedVMUuid());
    /* Update settings action 'enable' state: */
    QAction *pActionMenuBarSettings = actionPool()->action(UIActionIndexRT_M_View_M_MenuBar_S_Settings);
    pActionMenuBarSettings->setEnabled(fEnabled);
    /* Update switch action 'checked' state: */
    QAction *pActionMenuBarSwitch = actionPool()->action(UIActionIndexRT_M_View_M_MenuBar_T_Visibility);
    pActionMenuBarSwitch->blockSignals(true);
    pActionMenuBarSwitch->setChecked(fEnabled);
    pActionMenuBarSwitch->blockSignals(false);

    /* Update menu-bar visibility: */
    menuBar()->setVisible(pActionMenuBarSwitch->isChecked());
    /* Update menu-bar: */
    updateMenu();

    /* Normalize geometry without moving: */
    normalizeGeometry(false /* adjust position */, shouldResizeToGuestDisplay());
}

void UIMachineWindowNormal::sltHandleMenuBarContextMenuRequest(const QPoint &position)
{
    /* Raise action's context-menu: */
    if (gEDataManager->menuBarContextMenuEnabled(uiCommon().managedVMUuid()))
        actionPool()->action(UIActionIndexRT_M_View_M_MenuBar)->menu()->exec(menuBar()->mapToGlobal(position));
}
#endif /* !RT_OS_DARWIN */

void UIMachineWindowNormal::sltHandleStatusBarConfigurationChange(const QUuid &uMachineID)
{
    /* Skip unrelated machine IDs: */
    if (uiCommon().managedVMUuid() != uMachineID)
        return;

    /* Check whether status-bar is enabled: */
    const bool fEnabled = gEDataManager->statusBarEnabled(uiCommon().managedVMUuid());
    /* Update settings action 'enable' state: */
    QAction *pActionStatusBarSettings = actionPool()->action(UIActionIndexRT_M_View_M_StatusBar_S_Settings);
    pActionStatusBarSettings->setEnabled(fEnabled);
    /* Update switch action 'checked' state: */
    QAction *pActionStatusBarSwitch = actionPool()->action(UIActionIndexRT_M_View_M_StatusBar_T_Visibility);
    pActionStatusBarSwitch->blockSignals(true);
    pActionStatusBarSwitch->setChecked(fEnabled);
    pActionStatusBarSwitch->blockSignals(false);

    /* Update status-bar visibility: */
    statusBar()->setVisible(pActionStatusBarSwitch->isChecked());
    /* Update status-bar indicators-pool: */
    if (m_pIndicatorsPool)
        m_pIndicatorsPool->setAutoUpdateIndicatorStates(statusBar()->isVisible() && uimachine()->isRunning());

    /* Normalize geometry without moving: */
    normalizeGeometry(false /* adjust position */, shouldResizeToGuestDisplay());
}

void UIMachineWindowNormal::sltHandleStatusBarContextMenuRequest(const QPoint &position)
{
    /* Raise action's context-menu: */
    if (gEDataManager->statusBarContextMenuEnabled(uiCommon().managedVMUuid()))
        actionPool()->action(UIActionIndexRT_M_View_M_StatusBar)->menu()->exec(statusBar()->mapToGlobal(position));
}

void UIMachineWindowNormal::sltHandleIndicatorContextMenuRequest(IndicatorType enmIndicatorType, const QPoint &indicatorPosition)
{
    /* Sanity check, this slot should be called if m_pIndicatorsPool present anyway: */
    AssertPtrReturnVoid(m_pIndicatorsPool);
    /* Determine action depending on indicator-type: */
    UIAction *pAction = 0;
    switch (enmIndicatorType)
    {
        case IndicatorType_HardDisks:     pAction = actionPool()->action(UIActionIndexRT_M_Devices_M_HardDrives);     break;
        case IndicatorType_OpticalDisks:  pAction = actionPool()->action(UIActionIndexRT_M_Devices_M_OpticalDevices); break;
        case IndicatorType_FloppyDisks:   pAction = actionPool()->action(UIActionIndexRT_M_Devices_M_FloppyDevices);  break;
        case IndicatorType_Audio:         pAction = actionPool()->action(UIActionIndexRT_M_Devices_M_Audio);          break;
        case IndicatorType_Network:       pAction = actionPool()->action(UIActionIndexRT_M_Devices_M_Network);        break;
        case IndicatorType_USB:           pAction = actionPool()->action(UIActionIndexRT_M_Devices_M_USBDevices);     break;
        case IndicatorType_SharedFolders: pAction = actionPool()->action(UIActionIndexRT_M_Devices_M_SharedFolders);  break;
        case IndicatorType_Display:       pAction = actionPool()->action(UIActionIndexRT_M_ViewPopup);                break;
        case IndicatorType_Recording:     pAction = actionPool()->action(UIActionIndexRT_M_View_M_Recording);         break;
        case IndicatorType_Mouse:         pAction = actionPool()->action(UIActionIndexRT_M_Input_M_Mouse);            break;
        case IndicatorType_Keyboard:      pAction = actionPool()->action(UIActionIndexRT_M_Input_M_Keyboard);         break;
        default: break;
    }
    /* Raise action's context-menu: */
    if (pAction && pAction->isEnabled())
        pAction->menu()->exec(m_pIndicatorsPool->mapIndicatorPositionToGlobal(enmIndicatorType, indicatorPosition));
}

#ifdef VBOX_WS_MAC
void UIMachineWindowNormal::sltActionHovered(UIAction *pAction)
{
    /* Show the action message for a ten seconds: */
    statusBar()->showMessage(pAction->statusTip(), 10000);
}
#endif /* VBOX_WS_MAC */

void UIMachineWindowNormal::sltHandleCommitData()
{
    /* Shutting down resize timer early, that is
     * necessary to prevent touching e-data after
     * e-data manager was decomissioned already. */
    shutdownGeometrySaveTimer();
}

#ifndef VBOX_WS_MAC
void UIMachineWindowNormal::prepareMenu()
{
    /* Create menu-bar: */
    setMenuBar(new UIMenuBar);
    AssertPtrReturnVoid(menuBar());
    {
        /* Configure menu-bar: */
        menuBar()->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(menuBar(), &UIMenuBar::customContextMenuRequested,
                this, &UIMachineWindowNormal::sltHandleMenuBarContextMenuRequest);
        connect(gEDataManager, &UIExtraDataManager::sigMenuBarConfigurationChange,
                this, &UIMachineWindowNormal::sltHandleMenuBarConfigurationChange);
        /* Update menu-bar: */
        updateMenu();
    }
}
#endif /* !VBOX_WS_MAC */

void UIMachineWindowNormal::prepareStatusBar()
{
    /* Call to base-class: */
    UIMachineWindow::prepareStatusBar();

    /* Create status-bar: */
    setStatusBar(new QIStatusBar);
    AssertPtrReturnVoid(statusBar());
    {
        /* Configure status-bar: */
        statusBar()->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(statusBar(), &QIStatusBar::customContextMenuRequested,
                this, &UIMachineWindowNormal::sltHandleStatusBarContextMenuRequest);
        /* Create indicator-pool: */
        m_pIndicatorsPool = new UIIndicatorsPool(machineLogic()->uimachine());
        AssertPtrReturnVoid(m_pIndicatorsPool);
        {
            /* Configure indicator-pool: */
            connect(m_pIndicatorsPool, &UIIndicatorsPool::sigContextMenuRequest,
                    this, &UIMachineWindowNormal::sltHandleIndicatorContextMenuRequest);
            /* Add indicator-pool into status-bar: */
            statusBar()->addPermanentWidget(m_pIndicatorsPool, 0);
        }
        /* Post-configure status-bar: */
        connect(gEDataManager, &UIExtraDataManager::sigStatusBarConfigurationChange,
                this, &UIMachineWindowNormal::sltHandleStatusBarConfigurationChange);
#ifdef VBOX_WS_MAC
        /* Make sure the status-bar is aware of action hovering: */
        connect(actionPool(), &UIActionPool::sigActionHovered,
                this, &UIMachineWindowNormal::sltActionHovered);
#endif /* VBOX_WS_MAC */
    }

#ifdef VBOX_WS_MAC
    /* For the status-bar on Cocoa: */
    setUnifiedTitleAndToolBarOnMac(true);
#endif /* VBOX_WS_MAC */
}

void UIMachineWindowNormal::prepareNotificationCenter()
{
    if (gpNotificationCenter && (m_uScreenId == 0))
        gpNotificationCenter->setParent(centralWidget());
}

void UIMachineWindowNormal::prepareOtherConnections()
{
    /* UICommon connections: */
    connect(&uiCommon(), &UICommon::sigAskToCommitData,
            this, &UIMachineWindowNormal::sltHandleCommitData);
}

void UIMachineWindowNormal::prepareVisualState()
{
    /* Call to base-class: */
    UIMachineWindow::prepareVisualState();

#ifdef VBOX_GUI_WITH_CUSTOMIZATIONS1
    /* Customer request: The background has to go black: */
    QPalette palette(centralWidget()->palette());
    palette.setColor(centralWidget()->backgroundRole(), Qt::black);
    centralWidget()->setPalette(palette);
    centralWidget()->setAutoFillBackground(true);
    setAutoFillBackground(true);
#endif /* VBOX_GUI_WITH_CUSTOMIZATIONS1 */

#ifdef VBOX_WS_MAC
    /* Beta label? */
    if (UIVersionInfo::showBetaLabel())
    {
        QPixmap betaLabel = ::betaLabel(QSize(74, darwinWindowTitleHeight(this) - 1));
        darwinSetWindowLabel(this, &betaLabel);
    }

    /* Enable fullscreen support for every screen which requires it: */
    if (darwinScreensHaveSeparateSpaces() || m_uScreenId == 0)
        darwinEnableFullscreenSupport(this);
    /* Register 'Zoom' button to use our full-screen: */
    UICocoaApplication::instance()->registerCallbackForStandardWindowButton(this, StandardWindowButtonType_Zoom,
                                                                            UIMachineWindow::handleStandardWindowButtonCallback);
#endif /* VBOX_WS_MAC */
}

void UIMachineWindowNormal::loadSettings()
{
    /* Call to base-class: */
    UIMachineWindow::loadSettings();

    /* Load GUI customizations: */
    {
#ifndef VBOX_WS_MAC
        /* Update menu-bar visibility: */
        menuBar()->setVisible(actionPool()->action(UIActionIndexRT_M_View_M_MenuBar_T_Visibility)->isChecked());
#endif /* !VBOX_WS_MAC */
        /* Update status-bar visibility: */
        statusBar()->setVisible(actionPool()->action(UIActionIndexRT_M_View_M_StatusBar_T_Visibility)->isChecked());
        if (m_pIndicatorsPool)
            m_pIndicatorsPool->setAutoUpdateIndicatorStates(statusBar()->isVisible() && uimachine()->isRunning());
    }

#ifdef VBOX_GUI_WITH_CUSTOMIZATIONS1
    /* Just configure the window geometry for the 1st time,
     * required to make Qt windows work on tiling window managers. */
    UIDesktopWidgetWatchdog::setTopLevelGeometry(this, m_geometry);
#else
    /* Load window geometry: */
    {
        /* Load extra-data: */
        QRect geo = gEDataManager->machineWindowGeometry(machineLogic()->visualStateType(),
                                                         m_uScreenId, uiCommon().managedVMUuid());

        /* If we do have proper geometry: */
        if (!geo.isNull())
        {
            /* Restore window geometry: */
            m_geometry = geo;
            UIDesktopWidgetWatchdog::setTopLevelGeometry(this, m_geometry);

            /* If actual machine-state is NOT saved => normalize window to the optimal-size: */
            KMachineState enmActualState = KMachineState_Null;
            uimachine()->acquireLiveMachineState(enmActualState);
            if (enmActualState != KMachineState_Saved && enmActualState != KMachineState_AbortedSaved)
                normalizeGeometry(false /* adjust position */, shouldResizeToGuestDisplay());

            /* Maximize window (if necessary): */
            if (gEDataManager->machineWindowShouldBeMaximized(machineLogic()->visualStateType(),
                                                              m_uScreenId, uiCommon().managedVMUuid()))
                setWindowState(windowState() | Qt::WindowMaximized);
        }
        /* If we do NOT have proper geometry: */
        else
        {
            /* Normalize window to the optimal size: */
            normalizeGeometry(true /* adjust position */, shouldResizeToGuestDisplay());

            /* Move it to the screen-center: */
            m_geometry = geometry();
            m_geometry.moveCenter(gpDesktop->availableGeometry(this).center());
            UIDesktopWidgetWatchdog::setTopLevelGeometry(this, m_geometry);
        }

        /* Normalize to the optimal size: */
#ifdef VBOX_WS_NIX
        QTimer::singleShot(0, this, SLOT(sltNormalizeGeometry()));
#else /* !VBOX_WS_NIX */
        normalizeGeometry(true /* adjust position */, shouldResizeToGuestDisplay());
#endif /* !VBOX_WS_NIX */
    }
#endif /* VBOX_GUI_WITH_CUSTOMIZATIONS1 */
}

void UIMachineWindowNormal::cleanupVisualState()
{
#ifdef VBOX_WS_MAC
    /* Unregister 'Zoom' button from using our full-screen: */
    UICocoaApplication::instance()->unregisterCallbackForStandardWindowButton(this, StandardWindowButtonType_Zoom);
#endif /* VBOX_WS_MAC */
}

void UIMachineWindowNormal::cleanupNotificationCenter()
{
    if (gpNotificationCenter && (gpNotificationCenter->parent() == centralWidget()))
        gpNotificationCenter->setParent(0);
}

void UIMachineWindowNormal::cleanupStatusBar()
{
    delete m_pIndicatorsPool;
    m_pIndicatorsPool = 0;
}

bool UIMachineWindowNormal::event(QEvent *pEvent)
{
    switch (pEvent->type())
    {
        case QEvent::Resize:
        {
#ifdef VBOX_WS_NIX
            /* Prevent handling if fake screen detected: */
            if (UIDesktopWidgetWatchdog::isFakeScreenDetected())
                break;
#endif /* VBOX_WS_NIX */

            QResizeEvent *pResizeEvent = static_cast<QResizeEvent*>(pEvent);
            if (!isMaximizedChecked())
            {
                m_geometry.setSize(pResizeEvent->size());
#ifdef VBOX_WITH_DEBUGGER_GUI
                /* Update debugger window position: */
                updateDbgWindows();
#endif /* VBOX_WITH_DEBUGGER_GUI */
            }

            /* Restart geometry save timer: */
            restartGeometrySaveTimer();

            /* Let listeners know about geometry changes: */
            emit sigGeometryChange(geometry());
            break;
        }
        case QEvent::Move:
        {
#ifdef VBOX_WS_NIX
            /* Prevent handling if fake screen detected: */
            if (UIDesktopWidgetWatchdog::isFakeScreenDetected())
                break;
#endif /* VBOX_WS_NIX */

            if (!isMaximizedChecked())
            {
                m_geometry.moveTo(geometry().x(), geometry().y());
#ifdef VBOX_WITH_DEBUGGER_GUI
                /* Update debugger window position: */
                updateDbgWindows();
#endif /* VBOX_WITH_DEBUGGER_GUI */
            }

            /* Restart geometry save timer: */
            restartGeometrySaveTimer();

            /* Let listeners know about geometry changes: */
            emit sigGeometryChange(geometry());
            break;
        }
        case QEvent::WindowActivate:
        {
            /* Let listeners know about geometry changes: */
            emit sigGeometryChange(geometry());
            break;
        }
        /* Handle timer event started above: */
        case QEvent::Timer:
        {
            QTimerEvent *pTimerEvent = static_cast<QTimerEvent*>(pEvent);
            if (pTimerEvent->timerId() == m_iGeometrySaveTimerId)
            {
                /* Shutdown geometry save timer: */
                shutdownGeometrySaveTimer();

                LogRel2(("GUI: UIMachineWindowNormal: Saving geometry as: Origin=%dx%d, Size=%dx%d\n",
                         m_geometry.x(), m_geometry.y(), m_geometry.width(), m_geometry.height()));
                gEDataManager->setMachineWindowGeometry(machineLogic()->visualStateType(),
                                                        m_uScreenId, m_geometry,
                                                        isMaximizedChecked(), uiCommon().managedVMUuid());
            }
            break;
        }
        default:
            break;
    }
    return UIMachineWindow::event(pEvent);
}

void UIMachineWindowNormal::showInNecessaryMode()
{
    /* Make sure this window should be shown at all: */
    if (!uimachine()->isScreenVisible(m_uScreenId))
        return hide();

    /* Make sure this window is not minimized: */
    if (isMinimized())
        return;

    /* Show in normal mode: */
    show();

    /* Normalize machine-window geometry: */
    normalizeGeometry(true /* adjust position */, shouldResizeToGuestDisplay());

    /* Make sure machine-view have focus: */
    m_pMachineView->setFocus();
}

void UIMachineWindowNormal::restoreCachedGeometry()
{
    /* Restore the geometry cached by the window: */
    resize(m_geometry.size());
    move(m_geometry.topLeft());

    /* Adjust machine-view accordingly: */
    adjustMachineViewSize();
}

void UIMachineWindowNormal::normalizeGeometry(bool fAdjustPosition, bool fResizeToGuestDisplay)
{
#ifndef VBOX_GUI_WITH_CUSTOMIZATIONS1
    /* Skip if maximized: */
    if (isMaximized())
        return;

    /* Calculate client window offsets: */
    QRect frGeo = frameGeometry();
    const QRect geo = geometry();
    const int dl = geo.left() - frGeo.left();
    const int dt = geo.top() - frGeo.top();
    const int dr = frGeo.right() - geo.right();
    const int db = frGeo.bottom() - geo.bottom();

    /* Get the best size w/o scroll-bars: */
    if (fResizeToGuestDisplay)
    {
        /* Get widget size-hint: */
        QSize sh = sizeHint();

        /* If guest-screen auto-resize is not enabled
         * or guest-additions doesn't support graphics
         * we should deduce widget's size-hint on visible scroll-bar's hint: */
        if (   !machineView()->isGuestAutoresizeEnabled()
            || !uimachine()->isGuestSupportsGraphics())
        {
            if (machineView()->verticalScrollBar()->isVisible())
                sh -= QSize(machineView()->verticalScrollBar()->sizeHint().width(), 0);
            if (machineView()->horizontalScrollBar()->isVisible())
                sh -= QSize(0, machineView()->horizontalScrollBar()->sizeHint().height());
        }

        /* Resize the frame to fit the contents: */
        sh -= size();
        frGeo.setRight(frGeo.right() + sh.width());
        frGeo.setBottom(frGeo.bottom() + sh.height());
    }

    /* Adjust size/position if necessary: */
    QRect frGeoNew = fAdjustPosition
                   ? UIDesktopWidgetWatchdog::normalizeGeometry(frGeo, gpDesktop->overallAvailableRegion())
                   : frGeo;

    /* If guest-screen auto-resize is not enabled
     * or the guest-additions doesn't support graphics
     * we should take scroll-bars size-hints into account: */
    if (   frGeoNew != frGeo
        && (   !machineView()->isGuestAutoresizeEnabled()
            || !uimachine()->isGuestSupportsGraphics()))
    {
        /* Determine whether we need additional space for one or both scroll-bars: */
        QSize addition;
        if (frGeoNew.height() < frGeo.height())
            addition += QSize(machineView()->verticalScrollBar()->sizeHint().width() + 1, 0);
        if (frGeoNew.width() < frGeo.width())
            addition += QSize(0, machineView()->horizontalScrollBar()->sizeHint().height() + 1);

        /* Resize the frame to fit the contents: */
        frGeoNew.setRight(frGeoNew.right() + addition.width());
        frGeoNew.setBottom(frGeoNew.bottom() + addition.height());

        /* Adjust size/position again: */
        frGeoNew = UIDesktopWidgetWatchdog::normalizeGeometry(frGeoNew, gpDesktop->overallAvailableRegion());
    }

    /* Finally, set the frame geometry: */
    UIDesktopWidgetWatchdog::setTopLevelGeometry(this,
                                                 frGeoNew.left() + dl, frGeoNew.top() + dt,
                                                 frGeoNew.width() - dl - dr, frGeoNew.height() - dt - db);
#else /* VBOX_GUI_WITH_CUSTOMIZATIONS1 */
    /* Customer request: There should no be
     * machine-window resize/move on machine-view resize: */
    Q_UNUSED(fAdjustPosition);
    Q_UNUSED(fResizeToGuestDisplay);
#endif /* VBOX_GUI_WITH_CUSTOMIZATIONS1 */
}

void UIMachineWindowNormal::updateAppearanceOf(int iElement)
{
    /* Call to base-class: */
    UIMachineWindow::updateAppearanceOf(iElement);

    /* Set status-bar indicator-pool auto update timer: */
    if (   m_pIndicatorsPool
        && iElement & UIVisualElement_IndicatorPool)
        m_pIndicatorsPool->setAutoUpdateIndicatorStates(statusBar()->isVisible() && uimachine()->isRunning());
}

#ifndef VBOX_WS_MAC
void UIMachineWindowNormal::updateMenu()
{
    /* Rebuild menu-bar: */
    menuBar()->clear();
    foreach (QMenu *pMenu, actionPool()->menus())
        menuBar()->addMenu(pMenu);
}
#endif /* !VBOX_WS_MAC */

bool UIMachineWindowNormal::isMaximizedChecked()
{
#ifdef VBOX_WS_MAC
    /* On the Mac the WindowStateChange signal doesn't seems to be delivered
     * when the user get out of the maximized state. So check this ourself. */
    return ::darwinIsWindowMaximized(this);
#else /* VBOX_WS_MAC */
    return isMaximized();
#endif /* !VBOX_WS_MAC */
}

void UIMachineWindowNormal::restartGeometrySaveTimer()
{
    if (m_iGeometrySaveTimerId != -1)
        killTimer(m_iGeometrySaveTimerId);
    m_iGeometrySaveTimerId = startTimer(300);
}

void UIMachineWindowNormal::shutdownGeometrySaveTimer()
{
    if (m_iGeometrySaveTimerId != -1)
    {
        killTimer(m_iGeometrySaveTimerId);
        m_iGeometrySaveTimerId = -1;
    }
}
