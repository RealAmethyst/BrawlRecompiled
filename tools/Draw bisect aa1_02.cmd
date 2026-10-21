@echo off
title SPM diagnostic: draw bisect (Mario house)
cd /d "%~dp0.."
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-spm.ps1
if errorlevel 1 goto end
for %%N in (60 150 300 500 750 1000 1300) do (
  set "SPM_DRAW_LIMIT=%%N"
  py tools\sweep-spm-maps.py --seconds 6 --maps aa1_02
)
:end
echo Done: the 7 newest autotest\map-sweep-* folders = first 60,150,300,500,750,1000,1300 draws per frame.
pause
