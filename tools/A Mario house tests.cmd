@echo off
title SPM diagnostic: Mario house, culling off / depth off / lights off
cd /d "%~dp0.."
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-spm.ps1
if errorlevel 1 goto end
set SPM_CULL_TEST=1
py tools\sweep-spm-maps.py --seconds 10 --maps he1_01,aa1_02
set SPM_CULL_TEST=2
py tools\sweep-spm-maps.py --seconds 10 --maps he1_01,aa1_02
set SPM_CULL_TEST=
set SPM_NOLIGHT=1
py tools\sweep-spm-maps.py --seconds 10 --maps he1_01,aa1_02
:end
echo Done: 3 newest autotest\map-sweep-* folders = culling off, depth off, lights off.
pause
