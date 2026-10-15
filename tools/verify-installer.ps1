param([Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference='Stop'
$taskOutput=[IO.Path]::GetFullPath($OutputPath)
$taskSetup=Get-ChildItem -LiteralPath $taskOutput -Filter 'CHARONOS-*-Setup-x64.exe' | Select-Object -First 1
$taskPackage=Get-ChildItem -LiteralPath $taskOutput -Directory -Filter 'CHARONOS-*-win64' | Select-Object -First 1
if (!$taskSetup -or !$taskPackage) { throw 'Installer or runtime package not found.' }
$taskQa=Join-Path $taskOutput 'QA'
$taskInstall=[IO.Path]::GetFullPath((Join-Path $taskQa 'install-smoke'))
if (!$taskInstall.StartsWith($taskOutput+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Smoke path must be inside the output folder.' }
$taskRegistry='HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{541A5B3B-408C-478D-AD80-2AE20787183E}_is1'
if (Test-Path -LiteralPath $taskRegistry) { throw 'An existing CHARONOS install is registered. Use an isolated Windows account to verify this installer.' }
if (Test-Path -LiteralPath $taskInstall) { throw 'Smoke directory already exists. Refusing to overwrite it.' }
$taskInstallLog=Join-Path $taskQa 'installer-install.log'
$taskProcess=Start-Process -FilePath $taskSetup.FullName -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/TASKS=""',('/DIR="'+$taskInstall+'"'),('/LOG="'+$taskInstallLog+'"')) -WindowStyle Hidden -Wait -PassThru
if ($taskProcess.ExitCode -ne 0) { throw "Installer failed: $($taskProcess.ExitCode)" }
$taskFiles=Get-ChildItem -LiteralPath $taskPackage.FullName -Recurse -File
foreach($taskFile in $taskFiles) {
    $taskRelative=$taskFile.FullName.Substring($taskPackage.FullName.Length+1)
    $taskInstalled=Join-Path $taskInstall $taskRelative
    if (!(Test-Path -LiteralPath $taskInstalled) -or (Get-FileHash -LiteralPath $taskInstalled).Hash -ne (Get-FileHash -LiteralPath $taskFile.FullName).Hash) { throw "Installed file mismatch: $taskRelative" }
}
$taskPriorPath=$env:PATH
$taskPriorAssoc=$env:CHRONOS_NO_ASSOC
$taskPriorPlugin=$env:QT_PLUGIN_PATH
try {
    $env:PATH=$env:SystemRoot+'\System32;'+$env:SystemRoot
    $env:CHRONOS_NO_ASSOC='1'
    $env:QT_PLUGIN_PATH=''
    $taskApp=Start-Process -FilePath (Join-Path $taskInstall 'CHARONOS.exe') -WorkingDirectory $taskInstall -WindowStyle Hidden -PassThru
    if (!$taskApp.WaitForInputIdle(15000)) { throw 'Installed application did not become ready.' }
    Start-Sleep -Milliseconds 800
    $taskApp.Refresh()
    if ($taskApp.HasExited) { throw 'Installed application exited during startup.' }
    # Hidden windows do not expose a MainWindowHandle to CloseMainWindow.
    # End only the exact smoke-test process after startup readiness is confirmed.
    Stop-Process -Id $taskApp.Id
    if (!$taskApp.WaitForExit(10000)) { throw 'Smoke-test process did not stop.' }
} finally {
    $env:PATH=$taskPriorPath
    $env:CHRONOS_NO_ASSOC=$taskPriorAssoc
    $env:QT_PLUGIN_PATH=$taskPriorPlugin
}
$taskSentinel=Join-Path $taskInstall 'user-content-preservation.txt'
'User-created file used only by the installer QA.' | Set-Content -LiteralPath $taskSentinel
$taskUninstallLog=Join-Path $taskQa 'installer-uninstall.log'
$taskUninstall=Start-Process -FilePath (Join-Path $taskInstall 'unins000.exe') -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/LOG="'+$taskUninstallLog+'"')) -WindowStyle Hidden -Wait -PassThru
if ($taskUninstall.ExitCode -ne 0) { throw "Uninstaller failed: $($taskUninstall.ExitCode)" }
for($taskAttempt=0;$taskAttempt -lt 40 -and (Test-Path -LiteralPath (Join-Path $taskInstall 'unins000.exe'));++$taskAttempt) { Start-Sleep -Milliseconds 250 }
if ((Test-Path -LiteralPath (Join-Path $taskInstall 'CHARONOS.exe')) -or (Test-Path -LiteralPath $taskRegistry)) { throw 'Application or uninstall registration remains after uninstall.' }
if (!(Test-Path -LiteralPath $taskSentinel)) { throw 'Uninstall removed user-created content.' }
$taskResults=@(
    'PASS: Silent install exited 0.',
    "PASS: $($taskFiles.Count) installed files match deployed runtime SHA-256 hashes.",
    'PASS: Installed application became ready and remained running with development folders removed from PATH; exact test process then stopped.',
    'PASS: Silent uninstall exited 0; application and uninstall registration removed.',
    'PASS: User-created content in the installation folder was preserved.',
    'File associations and shortcuts were disabled for this isolated smoke test.'
)
$taskResults | Set-Content -LiteralPath (Join-Path $taskQa 'installer-verification.txt') -Encoding utf8
$taskResults
