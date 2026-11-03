@echo off
chcp 65001 >nul
title ONLOI DVT Test Asistani v1.0
echo ============================================================
echo   ONLOI DVT TEST ASISTANI v1.0 - Hedef Cihaz FW: v2.0.12
echo ============================================================
echo Uygulama baslatiliyor, lutfen bekleyin...
echo.

set PYTHON_EXE=C:\Users\gentek-ideapad\.platformio\penv\Scripts\python.exe
set SCRIPT_PATH=%~dp0onloi_dvt_assistant.py

if exist "%PYTHON_EXE%" (
    "%PYTHON_EXE%" "%SCRIPT_PATH%"
) else (
    python "%SCRIPT_PATH%"
)

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [HATA] Uygulama sonlandi veya calistirilamadi. Hata kodu: %ERRORLEVEL%
    pause
)
