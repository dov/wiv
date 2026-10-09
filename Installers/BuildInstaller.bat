@echo off
rem Builds the wiv Windows installer with NSIS (makensis).
rem
rem   BuildInstaller.bat [build-dir]
rem
rem build-dir defaults to build\windows-release (cmake --preset windows-release).
rem Steps: build -> cmake --install into Installers\tmp -> makensis.
setlocal

set "ROOT=%~dp0.."
set "BUILD=%~1"
if "%BUILD%"=="" set "BUILD=%ROOT%\build\windows-release"

set "VS_VCVARS=C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat"
set "VS_NINJA=C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
set "MAKENSIS=C:\Program Files (x86)\NSIS\makensis.exe"

if not exist "%MAKENSIS%" (
  echo makensis not found at "%MAKENSIS%"
  exit /b 1
)
call "%VS_VCVARS%" >nul || exit /b 1
set "PATH=%VS_NINJA%;%PATH%"

cd /d "%~dp0"

for /f "delims=" %%v in ('powershell -nologo -noprofile -command "(Get-Content '%ROOT%\vcpkg.json' | ConvertFrom-Json).version"') do set "VERSION=%%v"
for /f "delims=" %%s in ('git rev-parse --short^=6 HEAD') do set "SHORTSHA1=%%s"
echo wiv %VERSION% (%SHORTSHA1%) from %BUILD%

echo === Building ===
cmake --build "%BUILD%" || exit /b 1

echo === Staging ===
if exist tmp rmdir /s /q tmp
cmake --install "%BUILD%" --prefix "%~dp0tmp" || exit /b 1

echo === makensis ===
"%MAKENSIS%" /V2 /DVERSION=%VERSION% /DSHORTSHA1=%SHORTSHA1% /DSTAGING=tmp wiv.nsi || exit /b 1

echo.
echo Built: %~dp0Installwiv-v%VERSION%-%SHORTSHA1%.exe
