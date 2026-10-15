param(
    [string]$BuildDir = 'build',
    [string]$OutputRoot = 'F:\Codex\outputs',
    [string]$Compiler = ''
)
$ErrorActionPreference = 'Stop'
$taskWorkspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskBuild = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $taskWorkspace $BuildDir }
$taskVersion = [regex]::Match((Get-Content (Join-Path $taskWorkspace 'CMakeLists.txt') -Raw), 'project\(CHRONOS VERSION ([\d.]+)').Groups[1].Value
if (!$taskVersion) { throw 'Cannot read application version.' }
$taskDate = [TimeZoneInfo]::ConvertTimeBySystemTimeZoneId([datetime]::UtcNow,'Korea Standard Time').ToString('yyyy-MM-dd')
$taskOutput = Join-Path (Join-Path $OutputRoot $taskDate) ('charonos-' + $taskVersion + '-installer-' + [guid]::NewGuid().ToString('N').Substring(0,8))
$taskPackage = Join-Path $taskOutput ('CHARONOS-' + $taskVersion + '-win64')
$taskQt = 'C:\dev\Qt\6.10.3\msvc2022_64'
$taskExe = Join-Path $taskBuild 'CHARONOS.exe'
if (!(Test-Path -LiteralPath $taskExe)) { throw 'Build CHARONOS first.' }
if (!$Compiler) { $Compiler = Join-Path $taskWorkspace 'build-tools\InnoSetup\ISCC.exe' }
if (!(Test-Path -LiteralPath $Compiler)) { throw 'Pass -Compiler with the path to Inno Setup ISCC.exe.' }
New-Item -ItemType Directory -Path $taskPackage | Out-Null
Copy-Item -LiteralPath $taskExe -Destination $taskPackage
& (Join-Path $taskQt 'bin\windeployqt.exe') --release --no-translations --no-compiler-runtime --no-system-d3d-compiler --no-opengl-sw (Join-Path $taskPackage 'CHARONOS.exe')
if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed.' }
foreach ($taskDll in @('avcodec-62.dll','avformat-62.dll','avutil-60.dll','swscale-9.dll','swresample-6.dll')) {
    Copy-Item -LiteralPath (Join-Path 'C:\dev\ffmpeg\bin' $taskDll) -Destination $taskPackage
}
foreach ($taskDll in @('dxcompiler.dll','dxil.dll')) {
    if (Test-Path -LiteralPath (Join-Path $taskBuild $taskDll)) { Copy-Item -LiteralPath (Join-Path $taskBuild $taskDll) -Destination $taskPackage }
}
$taskCrt = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC' -Directory |
    Where-Object Name -Match '^\d+\.' | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
Get-ChildItem (Join-Path $taskCrt.FullName 'x64\Microsoft.VC143.CRT') -Filter '*.dll' | Copy-Item -Destination $taskPackage
Copy-Item -LiteralPath (Join-Path $taskBuild 'app.png') -Destination $taskPackage
Copy-Item -LiteralPath (Join-Path $taskBuild 'translations') -Destination $taskPackage -Recurse
Copy-Item -LiteralPath (Join-Path $taskWorkspace 'docs\CHARONOS-2.3-guide.md') -Destination $taskPackage
Copy-Item -LiteralPath (Join-Path $taskWorkspace 'docs\KEYFRAME-PRO-2-analysis.md') -Destination $taskPackage
Copy-Item -LiteralPath (Join-Path $taskWorkspace 'docs\QA-2.4.0.md') -Destination $taskPackage
Copy-Item -LiteralPath (Join-Path $taskWorkspace 'docs\QA-2.4.1.md') -Destination $taskPackage
Copy-Item -LiteralPath (Join-Path $taskWorkspace 'docs\QA-2.4.2.md') -Destination $taskPackage
Copy-Item -LiteralPath (Join-Path $taskWorkspace 'docs\QA-2.4.3.md') -Destination $taskPackage
New-Item -ItemType Directory -Path (Join-Path $taskPackage 'LICENSES') | Out-Null
Copy-Item -LiteralPath 'C:\dev\ffmpeg\LICENSE.txt' -Destination (Join-Path $taskPackage 'LICENSES\FFmpeg-GPL-3.0.txt')
if ((Get-FileHash $taskExe).Hash -ne (Get-FileHash (Join-Path $taskPackage 'CHARONOS.exe')).Hash) { throw 'EXE copy verification failed.' }
& $Compiler "/DPackageDir=$taskPackage" "/DVersion=$taskVersion" "/DOutputDir=$taskOutput" (Join-Path $PSScriptRoot 'charonos.iss')
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }
$taskSourceZip = Join-Path $taskOutput ('CHARONOS-' + $taskVersion + '-source.zip')
$taskSourcePaths = @('src','tools','res','docs','README.md','CMakeLists.txt','build.bat','build-test.bat') | ForEach-Object { Join-Path $taskWorkspace $_ }
Compress-Archive -LiteralPath $taskSourcePaths -DestinationPath $taskSourceZip -CompressionLevel Optimal
$taskQa = Join-Path $taskOutput 'QA'; New-Item -ItemType Directory -Path $taskQa | Out-Null
Copy-Item -LiteralPath (Join-Path $taskWorkspace 'build-test\feature-results.txt') -Destination $taskQa
Copy-Item -LiteralPath (Join-Path $taskWorkspace 'docs\QA-2.3.md') -Destination $taskQa
Copy-Item -LiteralPath (Join-Path $taskWorkspace 'docs\QA-2.3.1.md') -Destination $taskQa
Copy-Item -LiteralPath (Join-Path $taskWorkspace 'docs\QA-2.3.2.md') -Destination $taskQa
Copy-Item -LiteralPath (Join-Path $taskWorkspace 'docs\QA-2.4.0.md') -Destination $taskQa
Copy-Item -LiteralPath (Join-Path $taskWorkspace 'docs\QA-2.4.1.md') -Destination $taskQa
Copy-Item -LiteralPath (Join-Path $taskWorkspace 'docs\QA-2.4.2.md') -Destination $taskQa
Copy-Item -LiteralPath (Join-Path $taskWorkspace 'docs\QA-2.4.3.md') -Destination $taskQa
Get-ChildItem -LiteralPath (Join-Path $taskWorkspace 'build-test') -Filter 'qa-startup-*.txt' | Copy-Item -Destination $taskQa
Copy-Item -LiteralPath (Join-Path $taskWorkspace 'docs\KEYFRAME-PRO-2-analysis.md') -Destination $taskQa
Get-ChildItem -LiteralPath (Join-Path $taskWorkspace 'build-test') -Filter 'qa-*.png' | Copy-Item -Destination $taskQa
Get-ChildItem -LiteralPath (Join-Path $taskWorkspace 'build-test') -Filter '*performance*.txt' | Copy-Item -Destination $taskQa
$taskSetup = Join-Path $taskOutput ('CHARONOS-' + $taskVersion + '-Setup-x64.exe')
Get-FileHash -LiteralPath $taskSetup,$taskSourceZip | ForEach-Object { $_.Hash.ToLowerInvariant() + '  ' + [IO.Path]::GetFileName($_.Path) } |
    Set-Content -LiteralPath (Join-Path $taskOutput 'SHA256SUMS.txt') -Encoding ascii
Write-Output "OUTPUT=$taskOutput"
Write-Output "INSTALLER=$taskSetup"
