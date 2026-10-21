@echo off
title Super Paper Mario recomp build
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-spm.ps1" %*
echo.
echo Finished with exit code %ERRORLEVEL%. Logs are in the build-logs folder.
pause
