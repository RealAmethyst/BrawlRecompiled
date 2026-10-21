@echo off
title SPM diagnostic: texture cache off
cd /d "%~dp0.."
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-spm.ps1
if errorlevel 1 goto end
set SPM_TEX_NOCACHE=1
py tools\sweep-spm-maps.py --seconds 8 --maps he1_01,he4_08,gn3_11,gn4_15,an2_04,ta2_02,aa1_02
:end
echo Done: newest autotest\map-sweep-* folder = texture cache off.
pause
