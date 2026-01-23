; =============================================================================
; NSIS Installer Script for GNostr Signer
; =============================================================================
;
; Build Requirements:
;   - NSIS 3.x (https://nsis.sourceforge.io/)
;   - NSIS plugins: nsProcess, AccessControl, ShellLink
;
; Build Command:
;   makensis /DVERSION=1.0.0 /DARCH=x64 gnostr-signer.nsi
;
; Environment Variables:
;   BUILD_DIR    - Path to build output (default: ..\..\..\..\build)
;   DEPS_DIR     - Path to bundled dependencies (default: deps)
;
; =============================================================================

!include "MUI2.nsh"
!include "x64.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "nsDialogs.nsh"
!include "WinVer.nsh"

; -----------------------------------------------------------------------------
; Configuration
; -----------------------------------------------------------------------------

!ifndef VERSION
  !define VERSION "1.0.0"
!endif

!ifndef ARCH
  !define ARCH "x64"
!endif

!define PRODUCT_NAME "GNostr Signer"
!define PRODUCT_PUBLISHER "GNostr Contributors"
!define PRODUCT_WEB_SITE "https://github.com/gnostr/gnostr-signer"
!define PRODUCT_DIR_REGKEY "Software\Microsoft\Windows\CurrentVersion\App Paths\gnostr-signer.exe"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"
!define PRODUCT_UNINST_ROOT_KEY "HKLM"

!define APP_ID "org.gnostr.Signer"
!define EXECUTABLE "gnostr-signer.exe"
!define DAEMON_EXECUTABLE "gnostr-signer-daemon.exe"

; Installer attributes
Name "${PRODUCT_NAME} ${VERSION}"
OutFile "GNostrSigner-${VERSION}-${ARCH}-setup.exe"
InstallDir "$PROGRAMFILES64\${PRODUCT_NAME}"
InstallDirRegKey HKLM "${PRODUCT_DIR_REGKEY}" ""
RequestExecutionLevel admin
ShowInstDetails show
ShowUnInstDetails show

; Compression
SetCompressor /SOLID lzma
SetCompressorDictSize 64

; Modern UI configuration
!define MUI_ABORTWARNING
!define MUI_ICON "..\..\data\icons\gnostr-signer.ico"
!define MUI_UNICON "..\..\data\icons\gnostr-signer.ico"
!define MUI_WELCOMEFINISHPAGE_BITMAP "installer-welcome.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "installer-welcome.bmp"

; Branding text
BrandingText "${PRODUCT_NAME} ${VERSION}"

; -----------------------------------------------------------------------------
; Pages
; -----------------------------------------------------------------------------

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\..\..\..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXECUTABLE}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${PRODUCT_NAME}"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"

; -----------------------------------------------------------------------------
; Installer Sections
; -----------------------------------------------------------------------------

Section "Core Application" SEC_CORE
  SectionIn RO  ; Required section

  SetOutPath "$INSTDIR"

  ; Main executables
  File "${BUILD_DIR}\apps\gnostr-signer\${EXECUTABLE}"
  File "${BUILD_DIR}\apps\gnostr-signer\${DAEMON_EXECUTABLE}"

  ; GSettings schema
  SetOutPath "$INSTDIR\share\glib-2.0\schemas"
  File "..\..\data\org.gnostr.Signer.gschema.xml"

  ; Compile GSettings schema
  nsExec::ExecToLog '"$INSTDIR\bin\glib-compile-schemas.exe" "$INSTDIR\share\glib-2.0\schemas"'

  ; Icons
  SetOutPath "$INSTDIR\share\icons\hicolor\scalable\apps"
  File "..\..\data\icons\hicolor\scalable\apps\org.gnostr.Signer.svg"

  ; Create application data directory
  CreateDirectory "$LOCALAPPDATA\gnostr-signer"

  ; Write registry keys
  WriteRegStr HKLM "${PRODUCT_DIR_REGKEY}" "" "$INSTDIR\${EXECUTABLE}"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayName" "${PRODUCT_NAME}"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "URLInfoAbout" "${PRODUCT_WEB_SITE}"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayIcon" "$INSTDIR\${EXECUTABLE}"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "UninstallString" "$INSTDIR\uninstall.exe"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "NoModify" 1
  WriteRegDWORD ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "NoRepair" 1

  ; Calculate installed size
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "EstimatedSize" "$0"

  ; Create uninstaller
  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "GTK4 Runtime" SEC_GTK4
  SectionIn RO  ; Required section

  SetOutPath "$INSTDIR\bin"

  ; GTK4 and dependencies (from MSYS2 or vcpkg bundle)
  ; These files are copied by the bundle-deps.ps1 script
  File /r "${DEPS_DIR}\bin\*.dll"

  ; GLib binaries
  File "${DEPS_DIR}\bin\glib-compile-schemas.exe"
  File "${DEPS_DIR}\bin\gspawn-win64-helper.exe"
  File "${DEPS_DIR}\bin\gspawn-win64-helper-console.exe"
  File "${DEPS_DIR}\bin\gdbus.exe"

  ; GTK4 loaders and modules
  SetOutPath "$INSTDIR\lib\gdk-pixbuf-2.0\2.10.0\loaders"
  File /r "${DEPS_DIR}\lib\gdk-pixbuf-2.0\2.10.0\loaders\*.dll"
  File "${DEPS_DIR}\lib\gdk-pixbuf-2.0\2.10.0\loaders.cache"

  ; GTK4 modules
  SetOutPath "$INSTDIR\lib\gtk-4.0\4.0.0\media"
  File /nonfatal /r "${DEPS_DIR}\lib\gtk-4.0\4.0.0\media\*.dll"

  SetOutPath "$INSTDIR\lib\gtk-4.0\4.0.0\printbackends"
  File /nonfatal /r "${DEPS_DIR}\lib\gtk-4.0\4.0.0\printbackends\*.dll"

  ; GLib schemas (GTK defaults)
  SetOutPath "$INSTDIR\share\glib-2.0\schemas"
  File /nonfatal "${DEPS_DIR}\share\glib-2.0\schemas\*.xml"
  File /nonfatal "${DEPS_DIR}\share\glib-2.0\schemas\*.override"

  ; GTK settings and icons
  SetOutPath "$INSTDIR\share\gtk-4.0"
  File /nonfatal /r "${DEPS_DIR}\share\gtk-4.0\*.*"

  ; Icon themes (Adwaita)
  SetOutPath "$INSTDIR\share\icons"
  File /nonfatal /r "${DEPS_DIR}\share\icons\Adwaita"
  File /nonfatal /r "${DEPS_DIR}\share\icons\hicolor"

  ; Locale data
  SetOutPath "$INSTDIR\share\locale"
  File /nonfatal /r "${DEPS_DIR}\share\locale\*.*"
SectionEnd

Section "Libadwaita" SEC_ADWAITA
  SectionIn RO  ; Required section

  ; libadwaita is included in GTK4 section DLLs
  ; Additional Adwaita resources
  SetOutPath "$INSTDIR\share\libadwaita-1"
  File /nonfatal /r "${DEPS_DIR}\share\libadwaita-1\*.*"
SectionEnd

Section "Start Menu Shortcuts" SEC_SHORTCUTS
  SetOutPath "$INSTDIR"

  ; Create Start Menu folder
  CreateDirectory "$SMPROGRAMS\${PRODUCT_NAME}"

  ; Main application shortcut
  CreateShortcut "$SMPROGRAMS\${PRODUCT_NAME}\${PRODUCT_NAME}.lnk" \
    "$INSTDIR\${EXECUTABLE}" \
    "" \
    "$INSTDIR\${EXECUTABLE}" \
    0 \
    SW_SHOWNORMAL \
    "" \
    "Authorize Nostr app requests securely"

  ; Uninstaller shortcut
  CreateShortcut "$SMPROGRAMS\${PRODUCT_NAME}\Uninstall.lnk" \
    "$INSTDIR\uninstall.exe"

  ; Desktop shortcut (optional)
  CreateShortcut "$DESKTOP\${PRODUCT_NAME}.lnk" \
    "$INSTDIR\${EXECUTABLE}" \
    "" \
    "$INSTDIR\${EXECUTABLE}" \
    0 \
    SW_SHOWNORMAL \
    "" \
    "Authorize Nostr app requests securely"
SectionEnd

Section "Named Pipes Service" SEC_IPC
  ; Windows equivalent of D-Bus - Named Pipes for IPC
  ; Register the daemon for autostart (optional)

  ; Create a scheduled task or service entry for background daemon
  ; Using Task Scheduler for user-level autostart
  nsExec::ExecToLog 'schtasks /Create /TN "GNostr Signer Daemon" /TR "\"$INSTDIR\${DAEMON_EXECUTABLE}\"" /SC ONLOGON /RL LIMITED /F'

  ; Alternative: Windows Service registration (requires admin and service wrapper)
  ; This is commented out as Named Pipes work better for desktop apps
  ; nsExec::ExecToLog 'sc create "GNostrSignerDaemon" binPath= "$INSTDIR\${DAEMON_EXECUTABLE}" start= auto'
SectionEnd

Section "URL Protocol Handler" SEC_URLHANDLER
  ; Register nostr: and bunker: URL schemes

  ; nostr: protocol
  WriteRegStr HKCR "nostr" "" "URL:Nostr Protocol"
  WriteRegStr HKCR "nostr" "URL Protocol" ""
  WriteRegStr HKCR "nostr\DefaultIcon" "" "$INSTDIR\${EXECUTABLE},0"
  WriteRegStr HKCR "nostr\shell\open\command" "" '"$INSTDIR\${EXECUTABLE}" "%1"'

  ; bunker: protocol
  WriteRegStr HKCR "bunker" "" "URL:Nostr Bunker Protocol"
  WriteRegStr HKCR "bunker" "URL Protocol" ""
  WriteRegStr HKCR "bunker\DefaultIcon" "" "$INSTDIR\${EXECUTABLE},0"
  WriteRegStr HKCR "bunker\shell\open\command" "" '"$INSTDIR\${EXECUTABLE}" "%1"'
SectionEnd

; Optional: Visual C++ Runtime (if not using MSYS2 builds)
Section /o "Visual C++ Runtime" SEC_VCRUNTIME
  ; Only needed if building with MSVC/vcpkg
  SetOutPath "$TEMP"
  File /nonfatal "vc_redist.x64.exe"
  ${If} ${FileExists} "$TEMP\vc_redist.x64.exe"
    nsExec::ExecToLog '"$TEMP\vc_redist.x64.exe" /install /quiet /norestart'
    Delete "$TEMP\vc_redist.x64.exe"
  ${EndIf}
SectionEnd

; -----------------------------------------------------------------------------
; Section Descriptions
; -----------------------------------------------------------------------------

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_CORE} "The main GNostr Signer application and daemon."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_GTK4} "GTK4 graphics toolkit runtime (required)."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_ADWAITA} "Libadwaita GNOME design patterns library (required)."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_SHORTCUTS} "Create Start Menu and Desktop shortcuts."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_IPC} "Background daemon for inter-process communication with Nostr apps."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_URLHANDLER} "Handle nostr: and bunker: URL links."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_VCRUNTIME} "Microsoft Visual C++ Runtime (only needed for MSVC builds)."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; -----------------------------------------------------------------------------
; Callbacks
; -----------------------------------------------------------------------------

Function .onInit
  ; Check for Windows 10 or later
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_OK|MB_ICONSTOP "GNostr Signer requires Windows 10 or later."
    Abort
  ${EndIf}

  ; Check for 64-bit Windows
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "GNostr Signer requires a 64-bit version of Windows."
    Abort
  ${EndIf}

  ; Check if already running
  FindWindow $0 "" "${PRODUCT_NAME}"
  ${If} $0 != 0
    MessageBox MB_OK|MB_ICONEXCLAMATION "GNostr Signer is currently running. Please close it before installing."
    Abort
  ${EndIf}

  ; Set 64-bit registry view
  SetRegView 64
FunctionEnd

Function .onInstSuccess
  ; Compile GSettings schemas after all files are installed
  nsExec::ExecToLog '"$INSTDIR\bin\glib-compile-schemas.exe" "$INSTDIR\share\glib-2.0\schemas"'

  ; Update icon cache
  nsExec::ExecToLog '"$INSTDIR\bin\gtk4-update-icon-cache.exe" "$INSTDIR\share\icons\hicolor"'
FunctionEnd

; -----------------------------------------------------------------------------
; Uninstaller
; -----------------------------------------------------------------------------

Section "Uninstall"
  ; Stop daemon if running
  nsExec::ExecToLog 'taskkill /F /IM ${DAEMON_EXECUTABLE}'

  ; Remove scheduled task
  nsExec::ExecToLog 'schtasks /Delete /TN "GNostr Signer Daemon" /F'

  ; Remove URL protocol handlers
  DeleteRegKey HKCR "nostr"
  DeleteRegKey HKCR "bunker"

  ; Remove Start Menu shortcuts
  RMDir /r "$SMPROGRAMS\${PRODUCT_NAME}"
  Delete "$DESKTOP\${PRODUCT_NAME}.lnk"

  ; Remove installation directory
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\lib"
  RMDir /r "$INSTDIR\share"
  Delete "$INSTDIR\${EXECUTABLE}"
  Delete "$INSTDIR\${DAEMON_EXECUTABLE}"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"

  ; Remove registry keys
  DeleteRegKey ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}"
  DeleteRegKey HKLM "${PRODUCT_DIR_REGKEY}"

  SetRegView 64
SectionEnd

Function un.onInit
  SetRegView 64
FunctionEnd
