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
OutFile "Installwiv-v${VERSION}-${SHORTSHA1}.exe"
BrandingText "wiv Installer"

SetCompress force
CRCCheck on
XPStyle on

# Per-user install: no admin rights needed, everything goes under HKCU.
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\wiv"
InstallDirRegKey HKCU "Software\wiv" ""

!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\wiv"

!define MUI_FINISHPAGE_NOAUTOCLOSE
!define MUI_UNFINISHPAGE_NOAUTOCLOSE
!define MUI_FINISHPAGE_RUN "$INSTDIR\wiv.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start wiv"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "English"

Section "Install"
  SetOutPath "$INSTDIR"
  # wiv.exe, runtime DLLs, and the shaders, plugins and platforms folders
  File /r "${STAGING}\*"

  CreateDirectory "$SMPROGRAMS\wiv"
  CreateShortCut "$SMPROGRAMS\wiv\wiv.lnk" "$INSTDIR\wiv.exe" "" "$INSTDIR\wiv.exe" 0
  CreateShortCut "$SMPROGRAMS\wiv\Uninstall wiv.lnk" "$INSTDIR\uninst.exe"

  WriteRegStr HKCU "Software\wiv" "" "$INSTDIR"

  # Add/Remove Programs entry
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "wiv"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninst.exe"'
  WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninst.exe" /S'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" "$0"

  # Associate .giv files with wiv (per-user)
  WriteRegStr HKCU "Software\Classes\.giv" "" "wiv.GivFile"
  WriteRegStr HKCU "Software\Classes\wiv.GivFile" "" "giv File"
  WriteRegStr HKCU "Software\Classes\wiv.GivFile\DefaultIcon" "" "$INSTDIR\wiv.exe,0"
  WriteRegStr HKCU "Software\Classes\wiv.GivFile\shell" "" "open"
  WriteRegStr HKCU "Software\Classes\wiv.GivFile\shell\open\command" "" '"$INSTDIR\wiv.exe" "%1"'
  System::Call 'Shell32::SHChangeNotify(i 0x8000000, i 0, i 0, i 0)'

  # Pre-build the font atlas cache (~3 s per face) so the first scene opens fast
  DetailPrint "Building font cache..."
  ExecWait '"$INSTDIR\wiv.exe" --warm-fonts'

  WriteUninstaller "$INSTDIR\uninst.exe"
SectionEnd

Section "Uninstall"
  DeleteRegKey HKCU "Software\Classes\.giv"
  DeleteRegKey HKCU "Software\Classes\wiv.GivFile"
  DeleteRegKey HKCU "${UNINST_KEY}"
  DeleteRegKey HKCU "Software\wiv"
  System::Call 'Shell32::SHChangeNotify(i 0x8000000, i 0, i 0, i 0)'

  RMDir /r "$SMPROGRAMS\wiv"
  RMDir /r "$INSTDIR"
SectionEnd
