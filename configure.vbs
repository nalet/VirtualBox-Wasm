' $Id: configure.vbs 112403 2026-01-11 19:29:08Z knut.osmundsen@oracle.com $
' Thin VBScript wrapper to call configure.py with passed arguments.

'
' Copyright (C) 2025-2026 Oracle and/or its affiliates.
'
' This file is part of VirtualBox base platform packages, as
' available from https://www.virtualbox.org.
'
' This program is free software; you can redistribute it and/or
' modify it under the terms of the GNU General Public License
' as published by the Free Software Foundation, in version 3 of the
' License.
'
' This program is distributed in the hope that it will be useful, but
' WITHOUT ANY WARRANTY; without even the implied warranty of
' MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
' General Public License for more details.
'
' You should have received a copy of the GNU General Public License
' along with this program; if not, see <https://www.gnu.org/licenses>.
'
' SPDX-License-Identifier: GPL-3.0-only
'

WScript.Echo "Deprecation notice: VBScript has been marked as being deprecated and will be removed in the future."
WScript.Echo "                    Please either invoke configure.py via Python or use the Powershell script via configure.ps1"
WScript.Echo ""
WScript.Sleep 3000 ' Make it painful to use.

Set objShell = CreateObject("WScript.Shell")
Set objFSO = CreateObject("Scripting.FileSystemObject")
Set objArgs = WScript.Arguments

' Combine passed arguments.
strArgs = ""
For i = 1 To WScript.Arguments.Count
    strArgs = strArgs & " " & Chr(34) & WScript.Arguments(i-1) & Chr(34)
Next

strPythonPath = ""
strPythonBin  = ""

' Returns the value of a given command line argument.
Function GetArgValue(sArgName)
    Dim i, sArg, sTemp, sValue, sArgNext, idxSep
    GetArgValue = ""
    uArgLen = Len(sArgName)
    i = 0
    Do While i < objArgs.Count
        sArg = Trim(objArgs(i))
        If LCase(Left(sArg, uArgLen)) = LCase(sArgName) Then
            sTemp = Trim(Mid(sArg, uArgLen + 1)) ' Everything after the flag.
            ' Remove all spaces between flag and =
            If sTemp <> "" Then
                sTemp = Replace(sTemp, " ", "") ' Remove ALL spaces from temp.
            End If
            idxSep = InStr(sTemp, "=")
            If idxSep > 0 Then
                ' --arg=value, --arg =value, --arg=   value, --arg   =value
                sValue = Trim(Mid(sTemp, idxSep + 1))
                GetArgValue = sValue
                Exit Function
            ElseIf sTemp = "" Then
               ' Handle space after flag: --arg value or --arg = value or --arg      = value
                If i + 1 < objArgs.Count Then
                    sArgNext = Trim(objArgs(i + 1))
                    If Left(sArgNext, 1) = "=" Then
                        sValue = Trim(Mid(sArgNext, 2)) ' Remove any spaces after equal sign.
                        GetArgValue = sValue
                    Else
                        GetArgValue = sArgNext
                    End If
                    Exit Function
                End If
            End If
        End If
        i = i + 1
    Loop
End Function

strPythonPath = GetArgValue("--with-python-path")
if strPythonPath = "" Then
   strPythonPath = GetArgValue("--with-python")
End If

' Python binaries to search for. Sorted by likely-ness.
arrBin = Array("python.exe", "python3.exe")

if strPythonPath <> "" Then

   For Each strCurBin In arrBin
      strCurBinPath = objFSO.BuildPath(strPythonPath, strCurBin)
      If objFSO.FileExists(strCurBinPath) Then
         strPythonBin  = strCurBinPath
         Exit For
      End If
   Next
   if strPythonBin = "" Then
       WScript.Echo "Error: Not valid Python installation found at: " & strPythonPath
       WScript.Quit 1
   End if
End If

' Not specified above? Try python3, then just python.
If strPythonBin = "" Then
   strOutput = ""
   For Each strCurBin In arrBin
      Set exec = objShell.Exec("cmd /c where " & strCurBin)
      Do While Not exec.StdOut.AtEndOfStream
         strLine = exec.StdOut.ReadLine()
         If strOutput = "" Then ' Expects the path at the first line.
            strOutput = strLine
            Exit Do
         End If
      Loop
      If strOutput <> "" Then
         Exit For
      End If
   Next
   strPythonBin = strOutput
End If

If strPythonBin = "" Then
    WScript.Echo "Error: Python is not found."
    WScript.Echo ""
    WScript.Echo "Python 3 is required in order to build VirtualBox."
    WScript.Echo "Please install Python 3 and ensure it is in your PATH."
    WScript.Quit 1
End If

WScript.Echo "Using Python at: " & strPythonBin

' Execute configure.py with arguments.
strCmd = "cmd /c " & strPythonBin & " configure.py" & strArgs & " 2>&1"
Set oProc = objShell.Exec(strCmd)
Do While Not oProc.StdOut.AtEndOfStream
    WScript.StdOut.WriteLine oProc.StdOut.ReadLine
Loop

' Pass back exit code from configure.py.
WScript.Quit oProc.ExitCode
