@echo off
title SPM diagnostic: no-culling run, then no-depth-test run
cd /d "%~dp0.."
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-spm.ps1
if errorlevel 1 goto end
set SPM_CULL_TEST=1
py tools\sweep-spm-maps.py --seconds 8 --maps he1_01,he4_02,he4_08,gn3_11,gn3_13,gn4_15,an2_04,an2_05,ta2_02,aa1_02
set SPM_CULL_TEST=2
py tools\sweep-spm-maps.py --seconds 8 --maps he1_01,he4_02,he4_08,gn3_11,gn3_13,gn4_15,an2_04,an2_05,ta2_02,aa1_02
:end
echo Done: the two newest autotest\map-sweep-* folders (1 = culling off, 2 = depth test off).
pause
