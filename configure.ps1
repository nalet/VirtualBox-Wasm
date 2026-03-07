# $Id: configure.ps1 112403 2026-01-11 19:29:08Z knut.osmundsen@oracle.com $
# Thin PowerShell wrapper to call configure.py with passed arguments.

#
# Copyright (C) 2025-2026 Oracle and/or its affiliates.
#
# This file is part of VirtualBox base platform packages, as
# available from https://www.virtualbox.org.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation, in version 3 of the
# License.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <https://www.gnu.org/licenses>.
#
# SPDX-License-Identifier: GPL-3.0-only
#

$python = Get-Command python3 -ErrorAction SilentlyContinue
if (-not $python)
{
    $python = Get-Command python -ErrorAction SilentlyContinue
}

if (-not $python)
{
    Write-Host "Python 3 is required in order to build VirtualBox." -ForegroundColor Red
    Write-Host "Please install Python 3 and ensure it is in your PATH." -ForegroundColor Red
    exit 1
}

$version = & $python.Path --version 2>&1
if ($version -notmatch "Python 3")
{
    Write-Host "Python 3 is required. Found: $version" -ForegroundColor Red
    exit 1
}

& $python.Path configure.py @args
exit $LASTEXITCODE
