; VNC Helper Agent — NSIS Installer
; Installs vnc-helper.exe as a system tray app with firewall rule and user startup

!include "MUI2.nsh"
!include "nsDialogs.nsh"
!include "LogicLib.nsh"
!include "WinMessages.nsh"

; --- Build-time defines (can be overridden with -D on makensis command line) ---
!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef OUTFILE
  !define OUTFILE "vnc-helper-setup.exe"
!endif
!ifndef VI_VERSION
  !define VI_VERSION "${VERSION}"
!endif

; --- General ---
Name "VNC Helper Agent"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\VNC Helper"
InstallDirRegKey HKLM "Software\VNC Helper" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

; --- Version info embedded in the installer exe ---
VIProductVersion "${VI_VERSION}.0"
VIAddVersionKey "ProductName" "VNC Helper Agent"
VIAddVersionKey "FileDescription" "VNC Helper Agent Installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "BSD-2-Clause"
VIAddVersionKey "CompanyName" "The Daniel Morante Company, Inc."

; --- MUI settings ---
!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"
!define MUI_ABORTWARNING

; --- Pages ---
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
Page custom PortPageCreate PortPageLeave
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; --- Variables ---
Var PortInput
Var Port

; --- Custom port page ---
Function PortPageCreate
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}

  ${NSD_CreateLabel} 0 0 100% 24u "TCP listen port for the helper agent.$\nDefault: 9800. Change only if you have a conflict."
  Pop $0

  ${NSD_CreateNumber} 0 30u 80u 14u "9800"
  Pop $PortInput

  nsDialogs::Show
FunctionEnd

Function PortPageLeave
  ${NSD_GetText} $PortInput $Port
  ${If} $Port == ""
    StrCpy $Port "9800"
  ${EndIf}
FunctionEnd

; --- Installer section ---
Section "Install"
  SetOutPath "$INSTDIR"

  ; Kill any running tray instance (silent — may not be running on fresh install)
  nsExec::Exec 'taskkill /F /IM vnc-helper.exe'
  Sleep 500

  ; Copy files
  File "vnc-helper.exe"
  File "vnc-ocr.ps1"
  File "vnc-uia.ps1"

  ; Register in user startup (runs as tray app on login)
  nsExec::ExecToLog '"$INSTDIR\vnc-helper.exe" -port $Port install'

  ; Add firewall rule
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="VNC Helper Agent"'
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="VNC Helper Agent" dir=in action=allow protocol=TCP localport=$Port program="$INSTDIR\vnc-helper.exe" enable=yes'

  ; Start the tray app now
  Exec '"$INSTDIR\vnc-helper.exe" -port $Port'

  ; Write uninstaller
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; Registry for Add/Remove Programs
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VncHelper" \
    "DisplayName" "VNC Helper Agent"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VncHelper" \
    "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VncHelper" \
    "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VncHelper" \
    "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VncHelper" \
    "Publisher" "The Daniel Morante Company, Inc."
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VncHelper" \
    "DisplayIcon" '"$INSTDIR\vnc-helper.exe",0'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VncHelper" \
    "URLInfoAbout" "https://pacyworld.dev/pacyworld/vnc-mcp-server"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VncHelper" \
    "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VncHelper" \
    "NoRepair" 1

  ; Create Start Menu shortcut
  CreateDirectory "$SMPROGRAMS\VNC Helper"
  CreateShortcut "$SMPROGRAMS\VNC Helper\VNC Helper Agent.lnk" "$INSTDIR\vnc-helper.exe"
  CreateShortcut "$SMPROGRAMS\VNC Helper\Uninstall.lnk" "$INSTDIR\uninstall.exe"

  ; Remember install dir
  WriteRegStr HKLM "Software\VNC Helper" "InstallDir" "$INSTDIR"
SectionEnd

; --- Uninstaller section ---
Section "Uninstall"
  ; Kill running tray app
  nsExec::ExecToLog 'taskkill /F /IM vnc-helper.exe'
  Sleep 500

  ; Remove from startup registry
  nsExec::ExecToLog '"$INSTDIR\vnc-helper.exe" uninstall'

  ; Remove firewall rule
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="VNC Helper Agent"'

  ; Remove files
  Delete "$INSTDIR\vnc-helper.exe"
  Delete "$INSTDIR\vnc-ocr.ps1"
  Delete "$INSTDIR\vnc-uia.ps1"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"

  ; Remove Start Menu shortcuts
  Delete "$SMPROGRAMS\VNC Helper\VNC Helper Agent.lnk"
  Delete "$SMPROGRAMS\VNC Helper\Uninstall.lnk"
  RMDir "$SMPROGRAMS\VNC Helper"

  ; Clean registry
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VncHelper"
  DeleteRegKey HKLM "Software\VNC Helper"
SectionEnd

; --- Silent install defaults ---
Function .onInit
  StrCpy $Port "9800"
FunctionEnd
