@echo off
rem Diagnostic: checks the game's recompiled matrix routines against plain math; see [mtxtest] lines in the launch log.
set SPM_MTX_SELFTEST=1
call "%~dp0Sweep whole game.cmd" --seconds 2 --maps he4_08
