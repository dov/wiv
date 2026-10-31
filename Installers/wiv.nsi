# Build a Unicode installer (ANSI targets are deprecated since NSIS 3.07).
Unicode true

#----- Include Modern UI -----#
!include "MUI2.nsh"
!include "FileFunc.nsh"

# VERSION and SHORTSHA1 are passed in by BuildInstaller.bat (/DVERSION=...).
!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef SHORTSHA1
  !define SHORTSHA1 "local"
!endif
# Directory holding the `cmake --install` output to package.
!ifndef STAGING
  !define STAGING "tmp"
!endif

Name "wiv"
OutFile "InstallWiv-v${VERSION}-${SHORTSHA1}.exe"
BrandingText "wiv Installer"
Icon "giv-logo.ico"
UninstallIcon "giv-logo-install.ico"

SetCompress force
CRCCheck on
XPStyle on

# Machine-wide install: UAC asks for permission, files go under
# C:\Program Files\wiv and registry entries under HKLM (64-bit view).
RequestExecutionLevel admin
InstallDir "$PROGRAMFILES64\wiv"
InstallDirRegKey HKLM "Software\wiv" ""

!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\wiv"

!define MUI_FINISHPAGE_NOAUTOCLOSE
!define MUI_UNFINISHPAGE_NOAUTOCLOSE

!define MUI_LICENSEPAGE_TEXT_TOP "wiv is free software. Here is its license."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "COPYING.dos"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Section "Install"
  SetOutPath "$INSTDIR"
  # bin (wiv.exe, DLLs, Qt plugins), lib/wiv/plugins, share/wiv/shaders, examples
  File /r "${STAGING}\*"
  File "..\README.md"
  File "..\COPYING"

  CreateDirectory "$SMPROGRAMS\wiv"
  CreateShortCut "$SMPROGRAMS\wiv\wiv.lnk" "$INSTDIR\bin\wiv.exe" "" "$INSTDIR\bin\wiv.exe" 0
  CreateShortCut "$SMPROGRAMS\wiv\Examples.lnk" "$INSTDIR\examples"
  CreateShortCut "$SMPROGRAMS\wiv\Uninstall wiv.lnk" "$INSTDIR\uninst.exe"

  WriteRegStr HKLM "Software\wiv" "" "$INSTDIR"

  # Add/Remove Programs entry
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "wiv"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninst.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninst.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" "$0"

  # Associate .giv files with wiv (all users). As giv's installer does,
  # drop a stale per-user choice first, or it would override our association.
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.giv"
  DeleteRegKey HKLM "Software\Classes\wiv.GivFile"
  WriteRegStr HKLM "Software\Classes\.giv" "" "wiv.GivFile"
  WriteRegStr HKLM "Software\Classes\wiv.GivFile" "" "giv File"
  WriteRegStr HKLM "Software\Classes\wiv.GivFile\DefaultIcon" "" "$INSTDIR\bin\wiv.exe,1"
  WriteRegStr HKLM "Software\Classes\wiv.GivFile\shell" "" "open"
  WriteRegStr HKLM "Software\Classes\wiv.GivFile\shell\open\command" "" '"$INSTDIR\bin\wiv.exe" "%1"'
  System::Call 'Shell32::SHChangeNotify(i 0x8000000, i 0, i 0, i 0)'

  # Pre-build the font atlas cache (~3 s per face) so the first scene opens fast
  DetailPrint "Building font cache..."
  ExecWait '"$INSTDIR\bin\wiv.exe" --warm-fonts'

  WriteUninstaller "$INSTDIR\uninst.exe"
SectionEnd

Section "Uninstall"
  DeleteRegKey HKLM "Software\Classes\.giv"
  DeleteRegKey HKLM "Software\Classes\wiv.GivFile"
  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegKey HKLM "Software\wiv"
  System::Call 'Shell32::SHChangeNotify(i 0x8000000, i 0, i 0, i 0)'

  # Leave $INSTDIR first so the directory isn't locked by the uninstaller.
  SetOutPath "$TEMP"
  RMDir /r "$SMPROGRAMS\wiv"
  RMDir /r "$INSTDIR"
SectionEnd
