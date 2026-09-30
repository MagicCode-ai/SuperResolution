@echo off
setlocal enableextensions

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "ANDROID_DIR=%ROOT%\android"
set "GRADLEW=%ANDROID_DIR%\gradlew.bat"
set "ASSETS_MODEL_DIR=%ANDROID_DIR%\app\src\main\assets\model"
set "MODEL_DIR=%ROOT%\model"
set "ANDROID_LIB_DIR=%ROOT%\..\..\lib\android"
set "IOS_LIB_DIR=%ROOT%\..\..\lib\ios"
set "INTERFACE_DIR=%ROOT%\..\..\interface"

echo [INFO] Root: %ROOT%

if not exist "%GRADLEW%" (
  echo [ERROR] gradlew.bat not found: %GRADLEW%
  exit /b 1
)

call :require_file "%INTERFACE_DIR%\mc_interface.h"
call :require_file "%ANDROID_LIB_DIR%\libmagic_sr.a"
call :require_file "%IOS_LIB_DIR%\libmagic_sr.a"

call :require_file "%MODEL_DIR%\magic_gles_speed_gpu_params.bin"
call :require_file "%MODEL_DIR%\magic_gles_balanced_gpu_params.bin"
call :require_file "%MODEL_DIR%\magic_metal_speed_gpu_params.bin"
call :require_file "%MODEL_DIR%\magic_metal_balanced_gpu_params.bin"

if not exist "%ASSETS_MODEL_DIR%" (
  echo [INFO] Creating Android assets model directory...
  mkdir "%ASSETS_MODEL_DIR%"
  if errorlevel 1 (
    echo [ERROR] Failed to create directory: %ASSETS_MODEL_DIR%
    exit /b 1
  )
)

echo [INFO] Copying Android model files...
copy /Y "%MODEL_DIR%\magic_gles_speed_gpu_params.bin" "%ASSETS_MODEL_DIR%\" >nul
if errorlevel 1 (
  echo [ERROR] Failed to copy magic_gles_speed_gpu_params.bin
  exit /b 1
)
copy /Y "%MODEL_DIR%\magic_gles_balanced_gpu_params.bin" "%ASSETS_MODEL_DIR%\" >nul
if errorlevel 1 (
  echo [ERROR] Failed to copy magic_gles_balanced_gpu_params.bin
  exit /b 1
)

echo [INFO] Building Android Debug APK...
pushd "%ANDROID_DIR%"
call "%GRADLEW%" :app:assembleDebug
if errorlevel 1 (
  popd
  echo [ERROR] Android build failed.
  exit /b 1
)
popd

echo.
echo [SUCCESS] Android build complete.
echo [SUCCESS] APK: %ANDROID_DIR%\app\build\outputs\apk\debug\app-debug.apk
echo [INFO] iOS build should be done on macOS via Xcode (see README.md).
exit /b 0

:require_file
if not exist "%~1" (
  echo [ERROR] Required file missing: %~1
  exit /b 1
)
exit /b 0
