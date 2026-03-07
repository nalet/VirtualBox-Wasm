/* $Id: UITextTable.cpp 112717 2026-01-27 16:35:59Z sergey.dubov@oracle.com $ */
/** @file
 * VBox Qt GUI - UITextTable class implementation.
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

/* GUI includes: */
#include "UITextTable.h"


UITextTableLine::UITextTableLine(const QString &str1, const QString &str2, QObject *pParent /* = 0 */)
    : QObject(pParent)
    , m_str1(str1)
    , m_str2(str2)
{
}

UITextTableLine::UITextTableLine(const UITextTableLine &other)
    : QObject(other.parent())
    , m_str1(other.string1())
    , m_str2(other.string2())
{
}

UITextTableLine &UITextTableLine::operator=(const UITextTableLine &other)
{
    setParent(other.parent());
    set1(other.string1());
    set2(other.string2());
    return *this;
}

bool UITextTableLine::operator==(const UITextTableLine &other) const
{
    return    string1() == other.string1()
           && string2() == other.string2();
}
