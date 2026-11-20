@echo off
setlocal
set DIR=D:\Program Files\010 Editor

echo Closing 010 Editor...
taskkill /F /IM 010Editor.exe >nul 2>&1
ping -n 2 127.0.0.1 >nul

if not exist "%DIR%\quazip_orig.dll" (
    echo ERROR: quazip_orig.dll backup not found. Nothing to restore.
    pause
    exit /b 1
)

echo Restoring original quazip.dll...
del /Y "%DIR%\quazip.dll" >nul 2>&1
ren "%DIR%\quazip_orig.dll" quazip.dll
if errorlevel 1 (
    echo   FAILED. Run as Administrator.
    pause
    exit /b 1
)
echo Done. Original quazip.dll restored; hijack removed.
ping -n 3 127.0.0.1 >nul
