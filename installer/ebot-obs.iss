; Інсталятор плагіна «єБот для OBS»: кладе плагін у ProgramData — OBS сам
; підхоплює плагіни звідти: OBS 33+ — ebot-obs\ebot-obs.dll, OBS 31–32 — копію
; в bin\64bit (обидві готує CI, .github/workflows/windows.yml).
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{4E9B1F7A-2C51-4D7E-9A3B-8F0C6D2E1B44}
AppName=єБот для OBS
AppVersion={#AppVersion}
AppPublisher=єБот
AppPublisherURL=https://ebot.live
DefaultDirName={commonappdata}\obs-studio\plugins\ebot-obs
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
CloseApplications=yes
OutputDir=..\dist
OutputBaseFilename=ebot-obs-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=єБот для OBS
; Іконка єБота (frontend/src/app/icon.png основного репо, 16–256 px) — і в
; інсталятора, і в «Програмах»: стандартна іконка Inno поруч із попередженням
; SmartScreen про непідписаний файл виглядала як чужа програма.
SetupIconFile=ebot.ico
UninstallDisplayIcon={uninstallexe}

[Languages]
Name: "uk"; MessagesFile: "compiler:Languages\Ukrainian.isl"

[Files]
Source: "..\stage\ebot-obs\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Messages]
uk.FinishedLabel=Готово. Відкрийте OBS: у меню «Доки» з'явиться «єБот» — натисніть там «Підключити канал».
