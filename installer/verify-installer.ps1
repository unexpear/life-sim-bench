param([string]$InstallerPath, [string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (!$BuildDirectory) { $BuildDirectory = Join-Path $repo 'native\build-runner' }
if (!$InstallerPath) { $InstallerPath = Join-Path $repo 'dist\LifeSimWorkbench-Setup.exe' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$InstallerPath = [IO.Path]::GetFullPath($InstallerPath)
$registration = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{E4BB3429-C471-4EE3-83A6-02A74E32D850}_is1'
if (Test-Path -LiteralPath $registration) { throw 'An installed Workbench is already registered. Do not replace a user installation with this QA fixture.' }
$qa = Join-Path $BuildDirectory ('installer-qa-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$app = Join-Path $qa 'Installed workbench'
$data = Join-Path $qa ('Saved simulations ' + [char]0x03A9)
New-Item -ItemType Directory -Path $qa -Force | Out-Null
$previousData = $env:LIFESIM_USER_DATA
$previousPath = $env:PATH
function Install-Choice([string]$name, [string]$choice, [int]$count) {
    $log = Join-Path $qa ($name + '-setup.log')
    $arguments = '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /NOICONS /TASKS="" /NOCLOSEAPPLICATIONS /DIR="{0}" /LOG="{1}" {2}' -f $app,$log,$choice
    $process = Start-Process -FilePath $InstallerPath -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "$name install failed: $($process.ExitCode). See $log" }
    Copy-Item -LiteralPath (Join-Path $BuildDirectory 'bench_ui_shot.exe') -Destination (Join-Path $app 'native\bench_ui_shot.exe') -Force
    & (Join-Path $app 'native\bench_ui_shot.exe') --verify-library $count *> (Join-Path $qa ($name + '-library.log'))
    if ($LASTEXITCODE -ne 0) { throw "$name library check failed." }
    Write-Output "PASS $name install: exactly $count starter templates."
}
function Verify-Saves([string]$phase, [string]$name) {
    & (Join-Path $app 'native\bench_ui_shot.exe') $phase *> (Join-Path $qa ($name + '.log'))
    if ($LASTEXITCODE -ne 0) { throw "$name failed. See $qa" }
    Write-Output "PASS $name."
}
function Verify-Licensing([bool]$withTools) {
    foreach ($relative in @('LICENSE','LICENSING.md','THIRD_PARTY_NOTICES.md',
        'licenses\Golly.txt','licenses\GCC-Runtime-Exception-3.1.txt',
        'licenses\Inno-Setup.txt','licenses\runtime\crt\COPYING.MinGW-w64-runtime.txt')) {
        $installedFile = Join-Path $app $relative
        if (!(Test-Path -LiteralPath $installedFile) -or
            (Get-FileHash -LiteralPath $installedFile).Hash -ne (Get-FileHash -LiteralPath (Join-Path $repo $relative)).Hash) {
            throw "Missing or altered license notice: $relative"
        }
    }
    $zip = [IO.Compression.ZipFile]::OpenRead((Join-Path $app 'source\life-sim-workbench-source.zip'))
    try {
        foreach ($required in @('LICENSE','LICENSING.md','THIRD_PARTY_NOTICES.md',
            'native/CMakeLists.txt','native/src/workbench.cpp','native/src/sims/nesart.hpp',
            'plugins/example_rule.cpp','installer/build-installer.ps1','installer/toolchain-sources.json')) {
            if (!$zip.GetEntry($required)) { throw "Corresponding source is missing $required" }
        }
        if ($zip.Entries.FullName -match '(^|/)(userdata|backups|\.git)/|\.(exe|dll)$') {
            throw 'Source archive contains unexpected generated or private files.'
        }
    } finally { $zip.Dispose() }
    $revision = (Get-Content -LiteralPath (Join-Path $app 'source\REVISION.txt') -Raw).Trim()
    if ($revision -notmatch '^[0-9a-f]{40}$') { throw 'Missing source revision.' }
    if ($withTools) {
        $manifest = Join-Path $app 'toolchain\sources\toolchain-sources.json'
        if ((Get-FileHash -LiteralPath $manifest).Hash -ne
            (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'toolchain-sources.json')).Hash) {
            throw 'Installed compiler source manifest differs from the reviewed lock.'
        }
        $lock = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
        foreach ($source in $lock.sources) {
            $archive = Join-Path $app ('toolchain\sources\' + $source.file)
            if (!(Test-Path -LiteralPath $archive) -or
                (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $source.sha256) {
                throw "Missing or altered compiler source: $($source.file)"
            }
        }
        Write-Output "PASS licenses, workbench source and all $($lock.sources.Count) compiler sources."
    } else {
        if (Test-Path -LiteralPath (Join-Path $app 'toolchain')) { throw 'Core-only install unexpectedly installed build tools.' }
        Write-Output 'PASS core-only licenses and complete workbench source without build tools.'
    }
}
try {
    $env:LIFESIM_USER_DATA = $data
    # Prove that the app cannot accidentally find this machine's compiler.
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    Install-Choice 'core-only' '/COMPONENTS="core"' 0
    Verify-Licensing $false
    Install-Choice 'empty' '/TYPE=empty' 0
    Verify-Licensing $true
    & (Join-Path $app 'native\bench_ui_shot.exe') library 0 1280 800 (Join-Path $qa 'empty-library.png') *> (Join-Path $qa 'empty-screenshot.log')
    if ($LASTEXITCODE -ne 0) { throw 'Empty library rendering failed.' }
    Verify-Saves '--verify-saved-write' 'saved-write'
    Verify-Saves '--verify-saved-read' 'saved-reopen'
    Install-Choice 'selected' '/COMPONENTS="core,tools,templates\sorting2d,templates\locomotion"' 2
    Install-Choice 'full' '/TYPE=full' 38
    Install-Choice 'empty-again' '/TYPE=empty' 0
    Verify-Saves '--verify-saved-read' 'saved-after-template-removal'
    $before = @(Get-ChildItem -LiteralPath $data -Recurse -File | Get-FileHash -Algorithm SHA256)
    $uninstaller = [IO.Path]::GetFullPath((Join-Path $app 'unins000.exe'))
    if (!$uninstaller.StartsWith([IO.Path]::GetFullPath($qa) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'QA uninstall target is outside its fixture.' }
    $arguments = '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG="{0}"' -f (Join-Path $qa 'uninstall.log')
    $process = Start-Process -FilePath $uninstaller -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Uninstall failed: $($process.ExitCode)." }
    foreach ($file in $before) {
        if (!(Test-Path -LiteralPath $file.Path) -or (Get-FileHash -LiteralPath $file.Path -Algorithm SHA256).Hash -ne $file.Hash) { throw "Uninstall changed a saved user file: $($file.Path)" }
    }
    if (Test-Path -LiteralPath $registration) { throw 'QA uninstall registration remains.' }
    Write-Output "PASS uninstall preserved all $($before.Count) user files byte for byte."
    Write-Output "Installer verification passed. Logs and preview: $qa"
} finally {
    $env:LIFESIM_USER_DATA = $previousData
    $env:PATH = $previousPath
}
