@echo off
rem Builds the wiv Windows installer with NSIS (makensis).
rem
rem   BuildInstaller.bat [build-dir] [--copy-w]
rem
rem build-dir defaults to build\windows-RelWithDebInfo (see config-win.bat), so
rem that crash dumps from the installed exe can be matched to its wiv.pdb.
rem --copy-w also copies the installer to w:\users\Dov\WinInstall.
rem Steps: build -> cmake --install into Installers\tmp -> makensis.
setlocal

set "ROOT=%~dp0.."
set "BUILD="
set "COPY_W="
:parseargs
if "%~1"=="" goto argsdone
if /i "%~1"=="--copy-w" (set "COPY_W=1") else set "BUILD=%~1"
shift
goto parseargs
:argsdone
if "%BUILD%"=="" set "BUILD=%ROOT%\build\windows-RelWithDebInfo"

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

rem Keep the PDB matching the staged exe, named after the installer, for
rem debugging crash dumps (it is not shipped in the installer).
if exist "%BUILD%\wiv.pdb" copy /y "%BUILD%\wiv.pdb" "%~dp0InstallWiv-v%VERSION%-%SHORTSHA1%.pdb" >nul

echo.
echo Built: %~dp0InstallWiv-v%VERSION%-%SHORTSHA1%.exe

set "DEST=w:\users\Dov\WinInstall"
if defined COPY_W (
  if not exist "%DEST%" mkdir "%DEST%"
  copy /y "%~dp0InstallWiv-v%VERSION%-%SHORTSHA1%.exe" "%DEST%\" || exit /b 1
  if exist "%~dp0InstallWiv-v%VERSION%-%SHORTSHA1%.pdb" copy /y "%~dp0InstallWiv-v%VERSION%-%SHORTSHA1%.pdb" "%DEST%\" || exit /b 1
  echo Copied to %DEST%
)
