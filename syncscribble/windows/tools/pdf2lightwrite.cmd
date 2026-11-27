@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0pdf2lightwrite.ps1" %*
exit /b %ERRORLEVEL%
