param(
    [string]$Date = [TimeZoneInfo]::ConvertTimeBySystemTimeZoneId([datetime]::UtcNow, 'Korea Standard Time').ToString('yyyy-MM-dd'),
    [string]$OutputRoot = 'F:\Codex\outputs'
)
$ErrorActionPreference = 'Stop'
$taskWorkspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskBuild = Join-Path $taskWorkspace 'build'
$taskQt = 'C:\dev\Qt\6.10.3\msvc2022_64'
$taskFFmpeg = 'C:\dev\ffmpeg'
$taskName = 'charonos-2.2.0-' + [guid]::NewGuid().ToString('N').Substring(0,8)
$taskOutput = Join-Path (Join-Path $OutputRoot $Date) $taskName
$taskPackage = Join-Path $taskOutput 'CHARONOS-2.2.0-win64'
$taskExe = Join-Path $taskPackage 'CHARONOS.exe'
if (!(Test-Path -LiteralPath (Join-Path $taskBuild 'CHARONOS.exe'))) { throw 'Build CHARONOS first with build.bat.' }
New-Item -ItemType Directory -Path $taskPackage | Out-Null
Copy-Item -LiteralPath (Join-Path $taskBuild 'CHARONOS.exe') -Destination $taskExe
& (Join-Path $taskQt 'bin\windeployqt.exe') --release --no-translations --no-compiler-runtime --no-system-d3d-compiler --no-opengl-sw $taskExe
if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed.' }
foreach ($taskShaderDll in @('dxcompiler.dll','dxil.dll')) {
    $taskShaderPath = Join-Path $taskBuild $taskShaderDll
    if (Test-Path -LiteralPath $taskShaderPath) { Copy-Item -LiteralPath $taskShaderPath -Destination $taskPackage }
}
foreach ($taskDll in @('avcodec-62.dll','avformat-62.dll','avutil-60.dll','swscale-9.dll','swresample-6.dll')) {
    $taskFrom = Join-Path $taskFFmpeg ('bin\' + $taskDll)
    $taskTo = Join-Path $taskPackage $taskDll
    Copy-Item -LiteralPath $taskFrom -Destination $taskTo
    if ((Get-FileHash -LiteralPath $taskFrom).Hash -ne (Get-FileHash -LiteralPath $taskTo).Hash) { throw "Copy verification failed: $taskDll" }
}
$taskRedistBase = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC'
$taskCrt = Get-ChildItem -LiteralPath $taskRedistBase -Directory | Where-Object Name -Match '^\d+\.' |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$taskCrtPath = Join-Path $taskCrt.FullName 'x64\Microsoft.VC143.CRT'
Get-ChildItem -LiteralPath $taskCrtPath -Filter '*.dll' | Copy-Item -Destination $taskPackage
Copy-Item -LiteralPath (Join-Path $taskWorkspace 'docs\CHARONOS-2.2-guide.md') -Destination $taskPackage
Copy-Item -LiteralPath (Join-Path $taskBuild 'app.png') -Destination $taskPackage
Copy-Item -LiteralPath (Join-Path $taskBuild 'translations') -Destination $taskPackage -Recurse
$taskLicenseDir = Join-Path $taskPackage 'LICENSES'
New-Item -ItemType Directory -Path $taskLicenseDir | Out-Null
Copy-Item -LiteralPath (Join-Path $taskFFmpeg 'LICENSE.txt') -Destination (Join-Path $taskLicenseDir 'FFmpeg-GPL-3.0.txt')
@'
CHARONOS 2.2.0 - Windows x64 portable package

Extract the entire ZIP and run CHARONOS.exe from this folder.
Keep the DLLs and plugin subfolders beside the executable.
Qt, FFmpeg and Microsoft C++ runtime DLLs are included.

Controls:
  V = select (startup mode)
  One Pen button: right-click for Pencil / Pen / Highlighter (P / N / B)
  Hold Shift while drawing to create a straight stroke with mouse or tablet
  Ctrl+Alt+K = HotKey Manager (persistent assignments, conflict checks, defaults)
  Ctrl+Alt+N = New 2D Animation (no video required)
  Ctrl+Shift+I = Import Folder (recursive; keeps numbered image sequences together)
  Sources folder button = import; right-click = create a project bin
  Export Presets: built-in and saved custom settings; Canvas Border includes outside notes
  Ctrl+` = Console (Qt/FFmpeg logs and named commands; type help)
  Endless Scrubbing: Alt+drag in viewer or timeline, middle drag on timeline
  Screen-edge cursor wrap, sequence or In/Out wrap; Shift = fine scrubbing
  Tools > Endless Scrubbing disables cursor/sequence wrap
  Side handles = width/height; corners = free resize; Shift = keep proportions
  Select annotations, Shift-click a tracking marker, Ctrl+J = join
  Ctrl+Shift+J = detach at the current position
  G = grid on A and B; Shift+O = Onion Skin
  Drop video files into Project View, Sources, or the viewer

Runtime components:
  Qt 6.10.3: https://www.qt.io/  https://doc.qt.io/qt-6/licensing.html
  FFmpeg 8.1 shared GPL build: https://ffmpeg.org/  (license in LICENSES)
  Microsoft Visual C++ 2022 runtime: Microsoft redistributable runtime files

Feature checks cover live Qt windows, video URL drops, independent resizing,
pressure events for all styles, A/B grid and Onion Skin pixels, tracking joins,
undo/redo, project save/load and rendered export placement.
Physical tablet hardware was not available for a device-specific pressure test.
'@ | Set-Content -LiteralPath (Join-Path $taskPackage 'README.txt') -Encoding utf8
$taskQa = Join-Path $taskOutput 'QA'
New-Item -ItemType Directory -Path $taskQa | Out-Null
Copy-Item -LiteralPath (Join-Path $taskWorkspace 'build-test\feature-results.txt') -Destination $taskQa
Copy-Item -LiteralPath (Join-Path $taskWorkspace 'docs\QA-2.2.md') -Destination $taskQa
Get-ChildItem -LiteralPath (Join-Path $taskWorkspace 'build-test') -Filter 'qa-*.png' | Copy-Item -Destination $taskQa
$taskHashRows = Get-ChildItem -LiteralPath $taskPackage -File -Recurse | Sort-Object FullName | ForEach-Object {
    (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' +
    $_.FullName.Substring($taskPackage.Length+1)
}
$taskHashRows | Set-Content -LiteralPath (Join-Path $taskPackage 'SHA256SUMS.txt') -Encoding ascii
if ((Get-FileHash -LiteralPath (Join-Path $taskBuild 'CHARONOS.exe')).Hash -ne (Get-FileHash -LiteralPath $taskExe).Hash) { throw 'EXE copy differs.' }
$taskZip = Join-Path $taskOutput 'CHARONOS-2.2.0-win64.zip'
Compress-Archive -LiteralPath $taskPackage -DestinationPath $taskZip -CompressionLevel Optimal
Add-Type -AssemblyName System.IO.Compression.FileSystem
$taskArchive = [IO.Compression.ZipFile]::OpenRead($taskZip)
try {
    $taskExpectedFiles = (Get-ChildItem -LiteralPath $taskPackage -File -Recurse).Count
    $taskArchivedFiles = @($taskArchive.Entries | Where-Object { $_.Name -ne '' }).Count
    if ($taskArchivedFiles -ne $taskExpectedFiles) { throw 'ZIP file count differs.' }
    foreach ($taskEntry in $taskArchive.Entries) {
        if ($taskEntry.Name -eq '') { continue }
        $taskEntryPath = $taskEntry.FullName.Replace('\','/')
        $taskRelative = $taskEntryPath.Substring($taskEntryPath.IndexOf('/')+1)
        $taskLocalFile = Join-Path $taskPackage $taskRelative
        $taskStream = $taskEntry.Open()
        try {
            $taskSha = [Security.Cryptography.SHA256]::Create()
            $taskArchivedHash = [BitConverter]::ToString($taskSha.ComputeHash($taskStream)).Replace('-','')
            if ($taskArchivedHash -ne (Get-FileHash -LiteralPath $taskLocalFile).Hash) { throw "ZIP hash mismatch: $taskRelative" }
        } finally { $taskStream.Dispose() }
    }
} finally { $taskArchive.Dispose() }
$taskSourceZip = Join-Path $taskOutput 'CHARONOS-2.2.0-source.zip'
$taskSourcePaths = @('src','tools','res','docs','README.md','CMakeLists.txt','build.bat','build-test.bat') | ForEach-Object { Join-Path $taskWorkspace $_ }
Compress-Archive -LiteralPath $taskSourcePaths -DestinationPath $taskSourceZip -CompressionLevel Optimal
Get-FileHash -LiteralPath $taskZip,$taskSourceZip -Algorithm SHA256 | Format-Table -AutoSize
Write-Output "OUTPUT=$taskOutput"
Write-Output "EXE=$taskExe"
Write-Output "ZIP=$taskZip"
Write-Output "Verified $taskExpectedFiles runtime files and ZIP contents."
