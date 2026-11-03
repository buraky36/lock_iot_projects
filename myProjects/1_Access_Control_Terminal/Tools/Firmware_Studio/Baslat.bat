@echo off
chcp 65001 >nul
title ONLOI Firmware Studio v1.0
echo ============================================================
echo   ONLOI FIRMWARE STUDIO v1.0
echo   Derleme, Paketleme, Flash Yükleme ve Seri Monitör
echo ============================================================
echo Uygulama başlatılıyor, lütfen bekleyin...
echo.

set PYTHON_EXE=C:\Users\gentek-ideapad\.platformio\penv\Scripts\python.exe
set SCRIPT_PATH=%~dp0onloi_firmware_studio.py

if exist "%PYTHON_EXE%" (
    "%PYTHON_EXE%" "%SCRIPT_PATH%"
) else (
    python "%SCRIPT_PATH%"
)

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [HATA] Uygulama sonlandı veya çalıştırılamadı. Hata kodu: %ERRORLEVEL%
    pause
)
