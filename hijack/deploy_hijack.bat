@echo off
setlocal
set DIR=D:\Program Files\010 Editor
set SRC=%~dp0quazip.dll

echo [1/3] Closing 010 Editor...
taskkill /F /IM 010Editor.exe >nul 2>&1
ping -n 2 127.0.0.1 >nul

echo [2/3] Backing up original quazip.dll...
if not exist "%DIR%\quazip_orig.dll" (
    copy /Y "%DIR%\quazip.dll" "%DIR%\quazip_orig.dll" >nul
    echo   Backed up to quazip_orig.dll
) else (
    echo   Backup already exists, skipped
)

echo [3/3] Installing hijack proxy quazip.dll...
copy /Y "%SRC%" "%DIR%\quazip.dll" >nul
if errorlevel 1 (
    echo   FAILED to install. Run as Administrator.
    pause
    exit /b 1
)
echo   Installed.

echo.
echo Done. Now launching 010 Editor from ANY method (desktop shortcut,
echo file association, command line) will auto-load the Chinese patch.
echo.
echo To UNINSTALL (restore original):
echo   del "%DIR%\quazip.dll"
echo   ren "%DIR%\quazip_orig.dll" quazip.dll
echo.
ping -n 3 127.0.0.1 >nul
