/* $Id: UIDetailsItem.cpp 112701 2026-01-26 15:33:51Z sergey.dubov@oracle.com $ */
/** @file
 * VBox Qt GUI - UIDetailsItem class definition.
 */

/*
 * Copyright (C) 2012-2026 Oracle and/or its affiliates.
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
#include <QAccessibleObject>
#include <QApplication>
#include <QGraphicsScene>
#include <QPainter>
#include <QStyleOptionGraphicsItem>

/* GUI includes: */
#include "UIGraphicsTextPane.h"
#include "UIDetails.h"
#include "UIDetailsElement.h"
#include "UIDetailsGroup.h"
#include "UIDetailsModel.h"
#include "UIDetailsSet.h"
#include "UIDetailsView.h"


/** QAccessibleObject extension used as an accessibility interface for Details-view items. */
class UIAccessibilityInterfaceForUIDetailsItem : public QAccessibleObject
{
public:

    /** Returns an accessibility interface for passed @a strClassname and @a pObject. */
    static QAccessibleInterface *pFactory(const QString &strClassname, QObject *pObject)
    {
        /* Creating Details-view accessibility interface: */
        if (pObject && strClassname == QLatin1String("UIDetailsItem"))
            return new UIAccessibilityInterfaceForUIDetailsItem(pObject);

        /* Null by default: */
        return 0;
    }

    /** Constructs an accessibility interface passing @a pObject to the base-class. */
    UIAccessibilityInterfaceForUIDetailsItem(QObject *pObject)
        : QAccessibleObject(pObject)
    {}

    /** Returns the role. */
    virtual QAccessible::Role role() const RT_OVERRIDE
    {
        return QAccessible::ListItem;
    }

    /** Returns the parent. */
    virtual QAccessibleInterface *parent() const RT_OVERRIDE
    {
        /* Sanity check: */
        AssertPtrReturn(item(), 0);
        AssertPtrReturn(item()->model(), 0);
        AssertPtrReturn(item()->model()->view(), 0);

        /* Always return parent view: */
        return QAccessible::queryAccessibleInterface(item()->model()->view());
    }

    /** Returns the rect. */
    virtual QRect rect() const RT_OVERRIDE
    {
        /* Sanity check: */
        AssertPtrReturn(item(), QRect());
        AssertPtrReturn(item()->model(), QRect());
        AssertPtrReturn(item()->model()->view(), QRect());

        /* Now goes the mapping: */
        const QSize   itemSize         = item()->size().toSize();
        const QPointF itemPosInScene   = item()->mapToScene(QPointF(0, 0));
        const QPoint  itemPosInView    = item()->model()->view()->mapFromScene(itemPosInScene);
        const QPoint  itemPosInScreen  = item()->model()->view()->mapToGlobal(itemPosInView);
        const QRect   itemRectInScreen = QRect(itemPosInScreen, itemSize);
        return itemRectInScreen;
    }

    /** Returns the number of children. */
    virtual int childCount() const RT_OVERRIDE
    {
        /* Sanity check: */
        AssertPtrReturn(item(), 0);

        /* Zero by default: */
        return 0;
    }

    /** Returns the child with the passed @a iIndex. */
    virtual QAccessibleInterface *child(int iIndex) const RT_OVERRIDE
    {
        /* Sanity check: */
        AssertReturn(iIndex >= 0 && iIndex < childCount(), 0);
        AssertPtrReturn(item(), 0);

        /* Null be default: */
        return 0;
    }

    /** Returns the index of the passed @a pChild. */
    virtual int indexOfChild(const QAccessibleInterface *pChild) const RT_OVERRIDE
    {
        /* Sanity check: */
        AssertPtrReturn(pChild, -1);

        /* -1 by default: */
        return -1;
    }

    /** Returns the state. */
    virtual QAccessible::State state() const RT_OVERRIDE
    {
        /* Sanity check: */
        AssertPtrReturn(item(), QAccessible::State());
        AssertPtrReturn(item()->model(), QAccessible::State());

        /* Compose the state: */
        QAccessible::State myState;
        myState.focusable = true;
        myState.selectable = true;
        if (item()->model()->currentItem() == item())
        {
            myState.focused = true;
            myState.selected = true;
        }

        /* Return the state: */
        return myState;
    }

    /** Returns a text for the passed @a enmTextRole. */
    virtual QString text(QAccessible::Text enmTextRole) const RT_OVERRIDE
    {
        /* Sanity check: */
        AssertPtrReturn(item(), QString());
        UIDetailsElement *pElement = item()->toElement();
        AssertPtrReturn(pElement, QString());

        /* Text for known roles: */
        switch (enmTextRole)
        {
            case QAccessible::Name:
            {
                const QString strName = UIDetailsItem::tr("%1 details", "like 'General details' or 'Storage details'")
                                                          .arg(pElement->name());
                return QString("%1, ").arg(strName);
            }
            case QAccessible::Description:
            {
                QStringList result;
                foreach (const UITextTableLine &guiTextLine, pElement->text())
                {
                    const QString str1 = guiTextLine.string1();
                    QString str2 = guiTextLine.string2();
                    if (!str2.isEmpty())
                        str2.remove(QRegularExpression("<a[^>]*>|</a>"));
                    const QString strLine = str2.isEmpty() ? str1 : QString("%1: %2").arg(str1, str2);
                    result << strLine;
                }
                return result.join(", ");
            }
            default:
                break;
        }

        /* Null-string by default: */
        return QString();
    }

private:

    /** Returns corresponding Details-view item. */
    UIDetailsItem *item() const { return qobject_cast<UIDetailsItem*>(object()); }
};


/*********************************************************************************************************************************
*   Class UIDetailsItem implementation.                                                                                          *
*********************************************************************************************************************************/

UIDetailsItem::UIDetailsItem(UIDetailsItem *pParent)
    : QIGraphicsWidget(pParent)
    , m_pParent(pParent)
{
    /* Install Details-view item accessibility interface factory: */
    QAccessible::installFactory(UIAccessibilityInterfaceForUIDetailsItem::pFactory);

    /* Basic item setup: */
    setOwnedByLayout(false);
    setFocusPolicy(Qt::NoFocus);
    setFlag(QGraphicsItem::ItemIsSelectable, false);

    /* Non-root item? */
    if (parentItem())
    {
        /* Non-root item setup: */
        setAcceptHoverEvents(true);
    }

    /* Setup connections: */
    connect(this, &UIDetailsItem::sigBuildStep,
            this, &UIDetailsItem::sltBuildStep,
            Qt::QueuedConnection);
}

UIDetailsGroup *UIDetailsItem::toGroup()
{
    UIDetailsGroup *pItem = qgraphicsitem_cast<UIDetailsGroup*>(this);
    AssertMsg(pItem, ("Trying to cast invalid item type to UIDetailsGroup!"));
    return pItem;
}

UIDetailsSet *UIDetailsItem::toSet()
{
    UIDetailsSet *pItem = qgraphicsitem_cast<UIDetailsSet*>(this);
    AssertMsg(pItem, ("Trying to cast invalid item type to UIDetailsSet!"));
    return pItem;
}

UIDetailsElement *UIDetailsItem::toElement()
{
    UIDetailsElement *pItem = qgraphicsitem_cast<UIDetailsElement*>(this);
    AssertMsg(pItem, ("Trying to cast invalid item type to UIDetailsElement!"));
    return pItem;
}

UIDetailsModel *UIDetailsItem::model() const
{
    UIDetailsModel *pModel = qobject_cast<UIDetailsModel*>(QIGraphicsWidget::scene()->parent());
    AssertMsg(pModel, ("Incorrect graphics scene parent set!"));
    return pModel;
}

void UIDetailsItem::updateGeometry()
{
    /* Call to base-class: */
    QIGraphicsWidget::updateGeometry();

    /* Do the same for the parent: */
    if (parentItem())
        parentItem()->updateGeometry();
}

QSizeF UIDetailsItem::sizeHint(Qt::SizeHint enmWhich, const QSizeF &constraint /* = QSizeF() */) const
{
    /* If Qt::MinimumSize or Qt::PreferredSize requested: */
    if (enmWhich == Qt::MinimumSize || enmWhich == Qt::PreferredSize)
        /* Return wrappers: */
        return QSizeF(minimumWidthHint(), minimumHeightHint());
    /* Call to base-class: */
    return QIGraphicsWidget::sizeHint(enmWhich, constraint);
}

void UIDetailsItem::sltBuildStep(const QUuid &, int)
{
    AssertMsgFailed(("This item doesn't support building!"));
}


/*********************************************************************************************************************************
*   Class UIPrepareStep implementation.                                                                                          *
*********************************************************************************************************************************/

UIPrepareStep::UIPrepareStep(QObject *pParent, QObject *pBuildObject, const QUuid &uStepId, int iStepNumber)
    : QObject(pParent)
    , m_uStepId(uStepId)
    , m_iStepNumber(iStepNumber)
{
    /* Prepare connections: */
    connect(qobject_cast<UIDetailsItem*>(pBuildObject), &UIDetailsItem::sigBuildDone,
            this, &UIPrepareStep::sltStepDone,
            Qt::QueuedConnection);

    UIDetailsItem *pDetailsItem = qobject_cast<UIDetailsItem*>(pParent);
    AssertPtrReturnVoid(pDetailsItem);
    {
        connect(this, &UIPrepareStep::sigStepDone,
                pDetailsItem, &UIDetailsItem::sltBuildStep,
                Qt::QueuedConnection);
    }
}

void UIPrepareStep::sltStepDone()
{
    emit sigStepDone(m_uStepId, m_iStepNumber);
}
