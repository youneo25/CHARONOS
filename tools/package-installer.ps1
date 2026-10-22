param(
    [string]$BuildDir = 'build',
    [string]$TestDir = 'build-test',
    [string]$OutputRoot = 'F:\Codex\outputs',
    [string]$Compiler = '',
    [switch]$NoInstaller   # portable ZIP + source ZIP only (no Inno Setup compiler needed)
)
$ErrorActionPreference = 'Stop'
$taskWorkspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskBuild = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $taskWorkspace $BuildDir }
$taskTests = if ([IO.Path]::IsPathRooted($TestDir)) { $TestDir } else { Join-Path $taskWorkspace $TestDir }
$taskVersion = [regex]::Match((Get-Content (Join-Path $taskWorkspace 'CMakeLists.txt') -Raw), 'project\(CHRONOS VERSION ([\d.]+)').Groups[1].Value
if (!$taskVersion) { throw 'Cannot read application version.' }
$taskDisplayVersion = $taskVersion
$taskPackageVersion = $taskVersion
$taskDate = [TimeZoneInfo]::ConvertTimeBySystemTimeZoneId([datetime]::UtcNow,'Korea Standard Time').ToString('yyyy-MM-dd')
$taskOutput = Join-Path (Join-Path $OutputRoot $taskDate) ('charonos-' + $taskPackageVersion + '-release-' + [guid]::NewGuid().ToString('N').Substring(0,8))
$taskPackage = Join-Path $taskOutput ('CHARONOS-' + $taskPackageVersion + '-win64')
$taskQt = 'C:\dev\Qt\6.10.3\msvc2022_64'
$taskExe = Join-Path $taskBuild 'CHARONOS.exe'
$taskBridge = Join-Path $taskBuild 'charonos-mcp.exe'
if (!(Test-Path -LiteralPath $taskExe)) { throw 'Build CHARONOS first.' }
if (!(Test-Path -LiteralPath $taskBridge)) { throw 'charonos-mcp.exe is missing; build the charonos_mcp target.' }
if ((Get-Item -LiteralPath $taskExe).VersionInfo.ProductVersion -ne $taskDisplayVersion) { throw 'Build version does not match the source version. Rebuild CHARONOS first.' }
if (!$NoInstaller) {
    if (!$Compiler) { $Compiler = Join-Path $taskWorkspace 'build-tools\InnoSetup\ISCC.exe' }
    if (!(Test-Path -LiteralPath $Compiler)) { throw 'Pass -Compiler with the path to Inno Setup ISCC.exe, or use -NoInstaller.' }
}
New-Item -ItemType Directory -Path $taskPackage | Out-Null
Copy-Item -LiteralPath $taskExe -Destination $taskPackage
Copy-Item -LiteralPath $taskBridge -Destination $taskPackage
& (Join-Path $taskQt 'bin\windeployqt.exe') --release --no-translations --no-compiler-runtime --no-system-d3d-compiler --no-opengl-sw (Join-Path $taskPackage 'CHARONOS.exe')
if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed.' }
foreach ($taskDll in @('Qt6Core.dll','Qt6Network.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $taskPackage $taskDll))) { throw "$taskDll was not deployed (needed by charonos-mcp.exe)." }
}
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
New-Item -ItemType Directory -Path (Join-Path $taskPackage 'docs') | Out-Null
Copy-Item -LiteralPath (Join-Path $taskWorkspace 'docs\CHARONOS-User-Guide.html') -Destination (Join-Path $taskPackage 'docs')
Copy-Item -LiteralPath (Join-Path $taskWorkspace ('docs\QA-' + $taskVersion + '.md')) -Destination (Join-Path $taskPackage 'docs')
New-Item -ItemType Directory -Path (Join-Path $taskPackage 'LICENSES') | Out-Null
Copy-Item -LiteralPath 'C:\dev\ffmpeg\LICENSE.txt' -Destination (Join-Path $taskPackage 'LICENSES\FFmpeg-GPL-3.0.txt')
if ((Get-FileHash $taskExe).Hash -ne (Get-FileHash (Join-Path $taskPackage 'CHARONOS.exe')).Hash) { throw 'EXE copy verification failed.' }

if (!$NoInstaller) {
    & $Compiler "/DPackageDir=$taskPackage" "/DVersion=$taskVersion" "/DDisplayVersion=$taskDisplayVersion" "/DPackageVersion=$taskPackageVersion" "/DOutputDir=$taskOutput" (Join-Path $PSScriptRoot 'charonos.iss')
    if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }
}
$taskPortableZip = Join-Path $taskOutput ('CHARONOS-' + $taskPackageVersion + '-win64-portable.zip')
Compress-Archive -LiteralPath $taskPackage -DestinationPath $taskPortableZip -CompressionLevel Optimal
$taskSourceZip = Join-Path $taskOutput ('CHARONOS-' + $taskPackageVersion + '-source.zip')
$taskSourcePaths = @('src','tools','res','docs','README.md','CMakeLists.txt','build.bat','build-test.bat','.gitignore') | ForEach-Object { Join-Path $taskWorkspace $_ }
Compress-Archive -LiteralPath $taskSourcePaths -DestinationPath $taskSourceZip -CompressionLevel Optimal
$taskQa = Join-Path $taskOutput 'QA'; New-Item -ItemType Directory -Path $taskQa | Out-Null
Copy-Item -LiteralPath (Join-Path $taskWorkspace ('docs\QA-' + $taskVersion + '.md')) -Destination $taskQa
if (Test-Path -LiteralPath (Join-Path $taskTests 'feature-results.txt')) { Copy-Item -LiteralPath (Join-Path $taskTests 'feature-results.txt') -Destination $taskQa }
Get-ChildItem -LiteralPath $taskTests -Filter 'qa-*.png' -ErrorAction SilentlyContinue | Copy-Item -Destination $taskQa
Get-ChildItem -LiteralPath $taskTests -Filter '*performance*.txt' -ErrorAction SilentlyContinue | Copy-Item -Destination $taskQa
$taskArtifacts = @($taskPortableZip, $taskSourceZip)
$taskSetup = Join-Path $taskOutput ('CHARONOS-' + $taskPackageVersion + '-Setup-x64.exe')
if (Test-Path -LiteralPath $taskSetup) { $taskArtifacts = @($taskSetup) + $taskArtifacts }
Get-FileHash -LiteralPath $taskArtifacts | ForEach-Object { $_.Hash.ToLowerInvariant() + '  ' + [IO.Path]::GetFileName($_.Path) } |
    Set-Content -LiteralPath (Join-Path $taskOutput 'SHA256SUMS.txt') -Encoding ascii
Write-Output "OUTPUT=$taskOutput"
if (Test-Path -LiteralPath $taskSetup) { Write-Output "INSTALLER=$taskSetup" } else { Write-Output 'INSTALLER=(not built: Inno Setup compiler not used)' }
