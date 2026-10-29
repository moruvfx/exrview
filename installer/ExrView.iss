; ExrView — file cài đặt (Inno Setup 6). Build: iscc installer\ExrView.iss (sau khi đã có thư mục dist\)
#define AppName "ExrView"
#define AppVersion "1.0.0"
#define ThumbClsid "{{2222F408-4C02-4EF2-8893-520179C9792B}"
#define PreviewClsid "{{BA6AEF84-B730-460B-8AE3-ABDD08DB62E4}"
#define ThumbKey "{{e357fccd-a995-4576-b01f-234630154e96}"
#define PreviewKey "{{8895b1c6-b41f-4c1c-a562-0d564250836f}"

[Setup]
AppId={{F7A7E865-0D21-4005-935D-B52D53D45F31}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=ExrView
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
ChangesAssociations=yes
CloseApplications=yes
RestartApplications=no
OutputDir=Output
OutputBaseFilename=ExrView-Setup-{#AppVersion}
SetupIconFile=..\viewer\resources\exrview.ico
UninstallDisplayIcon={app}\ExrView.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "assoc"; Description: "Mở file .exr bằng ExrView khi double-click"; GroupDescription: "Liên kết file:"
Name: "desktopicon"; Description: "Tạo shortcut trên Desktop"; GroupDescription: "Shortcut:"; Flags: unchecked

[Files]
Source: "..\dist\*"; DestDir: "{app}"; Excludes: "ExrViewShell.dll"; Flags: ignoreversion recursesubdirs createallsubdirs
; DLL thumbnail/preview: tự gọi DllRegisterServer khi cài, DllUnregisterServer khi gỡ.
; Explorer có thể đang giữ DLL -> thay thế sau khi khởi động lại nếu cần.
Source: "..\dist\ExrViewShell.dll"; DestDir: "{app}"; Flags: ignoreversion regserver restartreplace uninsrestartdelete

[Registry]
; ProgID cho .exr / .sxr
Root: HKA; Subkey: "Software\Classes\ExrView.exr"; ValueType: string; ValueName: ""; ValueData: "OpenEXR Image"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\ExrView.exr\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\ExrView.exe,0"
Root: HKA; Subkey: "Software\Classes\ExrView.exr\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\ExrView.exe"" ""%1"""
Root: HKA; Subkey: "Software\Classes\ExrView.exr\ShellEx\{#ThumbKey}"; ValueType: string; ValueName: ""; ValueData: "{#ThumbClsid}"
Root: HKA; Subkey: "Software\Classes\ExrView.exr\ShellEx\{#PreviewKey}"; ValueType: string; ValueName: ""; ValueData: "{#PreviewClsid}"
; Hiện ExrView trong "Open with"
Root: HKA; Subkey: "Software\Classes\.exr\OpenWithProgids"; ValueType: string; ValueName: "ExrView.exr"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.sxr\OpenWithProgids"; ValueType: string; ValueName: "ExrView.exr"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\Applications\ExrView.exe\SupportedTypes"; ValueType: string; ValueName: ".exr"; ValueData: ""; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Applications\ExrView.exe\SupportedTypes"; ValueType: string; ValueName: ".sxr"; ValueData: ""
; App mặc định (Windows có thể vẫn hỏi lại nếu bạn đã chọn app khác trước đó)
Root: HKA; Subkey: "Software\Classes\.exr"; ValueType: string; ValueName: ""; ValueData: "ExrView.exr"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.sxr"; ValueType: string; ValueName: ""; ValueData: "ExrView.exr"; Flags: uninsdeletevalue; Tasks: assoc

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\ExrView.exe"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\ExrView.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\ExrView.exe"; Description: "Chạy ExrView"; Flags: nowait postinstall skipifsilent
