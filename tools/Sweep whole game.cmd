@echo off
title New Super Paper Bros - whole-game render sweep
cd /d "%~dp0.."
echo Building, then loading every map with render auditing and screenshots.
echo Runs on a private copy of the game and saves; your keyboard and mouse are not used.
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-spm.ps1
if errorlevel 1 goto end
where py >nul 2>nul && (py tools\sweep-spm-maps.py %*) || (python tools\sweep-spm-maps.py %*)
:end
echo.
echo Report: newest autotest\map-sweep-* folder (REPORT.md, results.json, shots\).
pause
