@echo off
rem ===========================================================================
rem patch.bat - build the Beyond loader shim and patch the game APK.
rem
rem   patch.bat                 patch the newest APK in "..\Android APK"
rem   patch.bat --install       ...and push it to a connected device
rem   patch.bat --no-build      reuse the shim already in shim\out
rem
rem All arguments are forwarded to patch_apk.py; see README.md.
rem ===========================================================================
setlocal

where python >nul 2>&1
if errorlevel 1 (
    echo ERROR: python was not found on PATH.
    pause
    exit /b 1
)

python "%~dp0patch_apk.py" %*
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" pause
exit /b %RC%
