@echo off
setlocal EnableExtensions

set "SCRIPT_DIR=%~dp0"
set "VERSION="

for /f "tokens=2 delims='" %%V in ('findstr /b /c:"VERSION='" "%SCRIPT_DIR%build.py"') do set "VERSION=%%V"

if not defined VERSION (
    echo ERROR: Could not read VERSION from "%SCRIPT_DIR%build.py".
    exit /b 1
)

set "SOURCE_NRO=%SCRIPT_DIR%build\NXInputRelay.nro"
set "OUTPUT_ZIP=%SCRIPT_DIR%NXInputRelay-v%VERSION%.zip"
set "STAGE_DIR=%SCRIPT_DIR%release"
set "PACKAGE_DIR=%STAGE_DIR%\switch\NXInputRelay"

if not exist "%SOURCE_NRO%" (
    echo ERROR: "%SOURCE_NRO%" does not exist.
    echo Run: python build.py
    exit /b 1
)

if not exist "%PACKAGE_DIR%" mkdir "%PACKAGE_DIR%" >nul 2>&1
if not exist "%PACKAGE_DIR%" (
    echo ERROR: Could not create the package staging directory.
    exit /b 1
)

copy /y "%SOURCE_NRO%" "%PACKAGE_DIR%\NXInputRelay.nro" >nul
if errorlevel 1 goto :package_error

if exist "%OUTPUT_ZIP%" del /q "%OUTPUT_ZIP%"
"%SystemRoot%\System32\tar.exe" -a -c -f "%OUTPUT_ZIP%" -C "%STAGE_DIR%" switch
if errorlevel 1 goto :package_error

if not exist "%OUTPUT_ZIP%" goto :package_error

echo Created: "%OUTPUT_ZIP%"
echo Staged: "%PACKAGE_DIR%\NXInputRelay.nro"
echo Archive path: switch/NXInputRelay/NXInputRelay.nro
exit /b 0

:package_error
echo ERROR: Failed to create the release archive.
exit /b 1
