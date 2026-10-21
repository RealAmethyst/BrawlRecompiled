@echo off
rem Diagnostic: same sweep with GX lighting disabled (SPM_NOLIGHT). Compare ground/walls with the normal run.
set SPM_NOLIGHT=1
call "%~dp0Sweep suspect maps.cmd"
