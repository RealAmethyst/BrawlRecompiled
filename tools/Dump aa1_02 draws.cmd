@echo off
rem Diagnostic: logs every draw for 2 frames of aa1_02 (Mario house interior) into the sweep launch log.
set SPM_SWEEP_DUMP=aa1_02
call "%~dp0Sweep whole game.cmd" --seconds 10 --maps aa1_02
