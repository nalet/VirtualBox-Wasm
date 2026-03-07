# $Id: makefile 112403 2026-01-11 19:29:08Z knut.osmundsen@oracle.com $
## @file
# wmake makefile for the VGA BIOS.
#

#
# Copyright (C) 2012-2026 Oracle and/or its affiliates.
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

# Compiler flags:
#  -q   no banner
#  -wx  max warnings
#  -0   generate 8086 code
#  -ms  force small model (default)
#  -s   remove runtime stack checks (required)
#  -os  optimize for size
#  -zu  assume that SS != DS (required)
#  -zl  do not emit default library information
#
!ifdef __UNIX__
Q=\"
!else
Q="
!endif
CFLAGS = -q -3 -wx -zu -zl -s -oas -d1 -ms
DEFS   = -DVBOX -DVBE -DVBOX_VERSION_STRING=$(Q)0.9.90$(Q) -DVBOX_VENDOR=$(Q)Oracle_Corporation$(Q) -DVBOX_PRODUCT=$(Q)Oracle_VirtualBox$(Q) -DVBOX_BIOS_CPU=80386
#DEFS   = -DVBOX -DVBOX_VERSION_STRING=$(Q)0.9$(Q) -DVBOX_VENDOR=$(Q)Oracle$(Q) -DVBOX_PRODUCT=$(Q)VirtualBox$(Q)

AFLAGS = -q -0 -wx

INCLS  = -I$(Q)../../../../../include$(Q) -I$(Q)../../../../../include/VBox/Graphics$(Q)
AINCS  = -I$(Q)../../BiosCommonCode$(Q)

.c.obj : .autodepend
	wcc -fo=.obj $(CFLAGS) $(DEFS) $(INCLS) $<

.asm.obj : .autodepend
	wasm -fo=.obj $(AFLAGS) $(DEFS) $(AINCS) $<

OBJS = 	vgarom.obj vberom.obj vgabios.obj vbe.obj

vgabios.rom : vgabios.bin
	biossums $< $@

vgabios.bin : $(OBJS) $(__MAKEFILES__)
	wlink name $@ system dos debug all option quiet &
	 option nofarcalls, map, verbose, statics, symfile &
	 output raw offset=0xC0000 order &
	 clname CODE segment VGAROM segaddr=0xC000 &
	 segment _TEXT segaddr=0xC000 offset=0xA00 &
	 segment VBE32 segaddr=0xC000 offset=0x4600 &
	 clname DATA segment _DATA segaddr=0xC000 offset=0x4800 &
	 file { $(OBJS) } disable 1014, 1023, 2120

clean : .symbolic
	@rm -f *.obj *.err
	@rm -f vgabios.bin vgabios.rom vgabios.map vgabios.sym
