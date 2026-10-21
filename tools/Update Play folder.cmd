@echo off
set P=%~dp0..
copy /y "%P%\wiicompiled\native-build\WiiCompiled.exe" "%P%\Play\SuperPaperMario.exe"
pause
