@echo off
rem Diagnostic: logs every draw (projection, cull, depth, scissor...) for 2 frames of he4_08 into the sweep launch log.
set SPM_SWEEP_DUMP=he4_08
call "%~dp0Sweep whole game.cmd" --seconds 10 --maps he4_08
