@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0pdf2write.ps1" %*
exit /b %ERRORLEVEL%
