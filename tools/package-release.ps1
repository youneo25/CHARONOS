# Packages a CHARONOS release: Full and Lite for Windows (installer + portable ZIP each), the
# Android Lite APK when it was built, the source ZIP and SHA256SUMS.
#
#   tools\package-release.ps1 -OutputDir D:\release -Compiler C:\dev\InnoSetup\ISCC.exe
#
# Builds first: build-test (Full, Release), build-lite (-DCHARONOS_LITE=ON) and optionally
# build-android (see docs/BUILD-ANDROID.md); -Apk points at a signed APK.
param(
    [Parameter(Mandatory = $true)][string]$OutputDir,
    [string]$Compiler = 'C:\dev\InnoSetup\ISCC.exe',
    [string]$FullBuild = 'build-test',
    [string]$LiteBuild = 'build-lite',
    [string]$Apk = '',
    [string]$Qt = 'C:\dev\Qt\6.10.3\msvc2022_64',
    [string]$FFmpeg = 'C:\dev\ffmpeg'
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$version = [regex]::Match((Get-Content (Join-Path $root 'CMakeLists.txt') -Raw), 'project\(CHRONOS VERSION ([\d.]+)').Groups[1].Value
if (!$version) { throw 'Cannot read the version from CMakeLists.txt.' }
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$OutputDir = (Resolve-Path $OutputDir).Path
$artifacts = @()

function Stage-Edition([string]$name, [string]$build, [string]$exe, [bool]$lite) {
    $buildDir = Join-Path $root $build
    $exePath = Join-Path $buildDir $exe
    if (!(Test-Path -LiteralPath $exePath)) { throw "$exe is missing in $build; build it first." }
    if ((Get-Item -LiteralPath $exePath).VersionInfo.ProductVersion -ne $version) { throw "$exe is not version $version; rebuild it." }
    $stage = Join-Path $OutputDir "$name-$version-win64"
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    New-Item -ItemType Directory -Path $stage | Out-Null
    Copy-Item -LiteralPath $exePath -Destination $stage
    if (!$lite) { Copy-Item -LiteralPath (Join-Path $buildDir 'charonos-mcp.exe') -Destination $stage }
    & (Join-Path $Qt 'bin\windeployqt.exe') --release --no-translations --no-compiler-runtime --no-system-d3d-compiler --no-opengl-sw (Join-Path $stage $exe) | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Qt deployment failed for $exe." }
    foreach ($dll in @('avcodec-62.dll','avdevice-62.dll','avfilter-11.dll','avformat-62.dll','avutil-60.dll','swscale-9.dll','swresample-6.dll')) {
        Copy-Item -LiteralPath (Join-Path $FFmpeg ('bin\' + $dll)) -Destination $stage
    }
    Copy-Item -LiteralPath (Join-Path $FFmpeg 'bin\ffmpeg.exe') -Destination $stage
    $crt = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC' -Directory |
        Where-Object Name -Match '^\d+\.' | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    Get-ChildItem (Join-Path $crt.FullName 'x64\Microsoft.VC143.CRT') -Filter '*.dll' | Copy-Item -Destination $stage
    Copy-Item -LiteralPath (Join-Path $root 'res\app.png') -Destination $stage
    New-Item -ItemType Directory -Path (Join-Path $stage 'translations') | Out-Null
    Copy-Item -LiteralPath (Join-Path $Qt 'translations\qtbase_ko.qm') -Destination (Join-Path $stage 'translations')
    New-Item -ItemType Directory -Path (Join-Path $stage 'docs') | Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'docs\CHARONOS-User-Guide.html') -Destination (Join-Path $stage 'docs')
    Copy-Item -LiteralPath (Join-Path $root "docs\CHANGES-$version.md") -Destination (Join-Path $stage 'docs')
    New-Item -ItemType Directory -Path (Join-Path $stage 'LICENSES') | Out-Null
    Copy-Item -LiteralPath (Join-Path $FFmpeg 'LICENSE.txt') -Destination (Join-Path $stage 'LICENSES\FFmpeg-GPL-3.0.txt')
    return $stage
}

foreach ($edition in @(@{ Name = 'CHARONOS'; Build = $FullBuild; Exe = 'CHARONOS.exe'; Lite = $false },
                       @{ Name = 'CHARONOS-Lite'; Build = $LiteBuild; Exe = 'CHARONOS-Lite.exe'; Lite = $true })) {
    $stage = Stage-Edition $edition.Name $edition.Build $edition.Exe $edition.Lite
    $defines = @("/DPackageDir=$stage", "/DVersion=$version", "/DOutputDir=$OutputDir")
    if ($edition.Lite) { $defines += '/DLite' }
    & $Compiler @defines (Join-Path $PSScriptRoot 'charonos.iss') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Installer compilation failed for $($edition.Name)." }
    $setup = Join-Path $OutputDir "$($edition.Name)-$version-Setup-x64.exe"
    if (!(Test-Path -LiteralPath $setup)) { throw "Installer not found: $setup" }
    $zip = Join-Path $OutputDir "$($edition.Name)-$version-win64-portable.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
    Compress-Archive -LiteralPath $stage -DestinationPath $zip -CompressionLevel Optimal
    $artifacts += $setup, $zip
}

if ($Apk) {
    $apkOut = Join-Path $OutputDir "CHARONOS-Lite-$version-android-arm64.apk"
    Copy-Item -LiteralPath $Apk -Destination $apkOut -Force
    $artifacts += $apkOut
}

$source = Join-Path $OutputDir "CHARONOS-$version-source.zip"
if (Test-Path -LiteralPath $source) { Remove-Item -LiteralPath $source }
git -C $root archive --format=zip -o $source HEAD
if ($LASTEXITCODE -ne 0) { throw 'git archive failed.' }
$artifacts += $source

Get-FileHash -LiteralPath $artifacts | ForEach-Object { $_.Hash.ToLowerInvariant() + '  ' + [IO.Path]::GetFileName($_.Path) } |
    Set-Content -LiteralPath (Join-Path $OutputDir "SHA256SUMS-$version.txt") -Encoding ascii
$artifacts | ForEach-Object { Write-Output ("ARTIFACT=" + $_) }
