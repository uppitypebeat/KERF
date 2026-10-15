; KERF installer for Windows (Inno Setup 6). Built by GitHub Actions (.github/workflows/build.yml):
;   ISCC /DAppVersion=0.9.1 "/DAppLabel=0.9.1 beta" /DSourceDir=<folder with KERF.vst3 and KERF.exe>
;        /DOutputDir=<where the setup goes> installer\windows\KERF.iss
;
; Installs the VST3 plugin into the standard folder every Windows DAW scans, the standalone
; app into Program Files (with a Start menu entry), and an uninstaller.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef AppLabel
  #define AppLabel AppVersion
#endif
#ifndef SourceDir
  #define SourceDir "..\..\build\Kerf_artefacts\Release"
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
; The same id for every version: a new version replaces the old one.
AppId={{6F3C2B7E-4A1D-4C8E-9B5F-2D7A1E0C9B3F}
AppName=KERF
AppVersion={#AppVersion}
AppVerName=KERF {#AppLabel}
AppPublisher=Julien Upmeier
AppPublisherURL=https://github.com/uppitypebeat/KERF
AppSupportURL=https://github.com/uppitypebeat/KERF/issues
AppUpdatesURL=https://github.com/uppitypebeat/KERF/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\KERF
DefaultGroupName=KERF
DisableProgramGroupPage=yes
DisableDirPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=KERF-{#AppVersion}-Windows-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=KERF {#AppLabel}
UninstallDisplayIcon={app}\KERF.exe
CloseApplications=yes

[Types]
Name: "full"; Description: "VST3 plugin and standalone app"
Name: "plugin"; Description: "VST3 plugin only"
Name: "custom"; Description: "Choose"; Flags: iscustom

[Components]
Name: "vst3"; Description: "KERF VST3 plugin (FL Studio, Ableton, Reaper, Bitwig, Cubase ...)"; Types: full plugin custom; Flags: fixed
Name: "app"; Description: "KERF standalone app (runs without a DAW)"; Types: full

[InstallDelete]
; A clean plugin bundle (no files left over from an older version).
Type: filesandordirs; Name: "{commoncf64}\VST3\KERF.vst3"

[Files]
Source: "{#SourceDir}\KERF.vst3\*"; DestDir: "{commoncf64}\VST3\KERF.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\KERF.exe"; DestDir: "{app}"; Components: app; Flags: ignoreversion

[Icons]
Name: "{group}\KERF"; Filename: "{app}\KERF.exe"; Components: app
Name: "{group}\Uninstall KERF"; Filename: "{uninstallexe}"

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\KERF.vst3"

[Run]
Filename: "{app}\KERF.exe"; Description: "Open the KERF standalone app now"; Components: app; Flags: nowait postinstall skipifsilent unchecked

[Messages]
FinishedLabel=KERF is installed.%n%nIn your DAW, rescan your plugins. In FL Studio: Options > Manage plugins > Find installed plugins, then add KERF from Generators.
