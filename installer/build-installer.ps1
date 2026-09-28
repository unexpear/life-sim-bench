param(
    [string]$BuildDirectory,
    [string]$InnoCompiler,
    [string]$MsysRoot = 'C:\msys64',
    [string]$SourceCache,
    [switch]$SkipBuild,
    [switch]$StageToolchainOnly
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (!$BuildDirectory) { $BuildDirectory = Join-Path $repo 'native\build-runner' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
if (!$InnoCompiler) { $InnoCompiler = Join-Path $BuildDirectory 'installer-tools\inno\ISCC.exe' }
$stage = Join-Path $BuildDirectory 'installer-stage'
$compilerPrefix = Join-Path $MsysRoot 'ucrt64'
$pacman = Join-Path $MsysRoot 'usr\bin\pacman.exe'
if (!$SourceCache) { $SourceCache = Join-Path $BuildDirectory 'license-sources' }
$lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'toolchain-sources.json') -Raw | ConvertFrom-Json
if ($lock.schema -ne 1) { throw 'Unsupported toolchain source lock.' }
$packages = @($lock.packages.name)
$installed = @(& $pacman -Q @packages)
if ($LASTEXITCODE -ne 0) { throw 'Could not inspect compiler package versions.' }
$expected = @($lock.packages | ForEach-Object { $_.name + ' ' + $_.version })
if (Compare-Object ($expected | Sort-Object) ($installed | Sort-Object)) {
    throw 'Installed compiler packages differ from the reviewed source lock. Update and audit the lock before packaging.'
}
& (Join-Path $PSScriptRoot 'prepare-toolchain-sources.ps1') -CacheDirectory $SourceCache
if (!$StageToolchainOnly) {
    $dirty = @(git -C $repo status --porcelain --untracked-files=normal)
    if ($LASTEXITCODE -ne 0 -or $dirty.Count) { throw 'Commit the reviewed source before building an installer.' }
    $revision = git -C $repo rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw 'A source commit is required for binary distribution.' }
}
# A fresh stage prevents old dependency versions or removed assets leaking into
# a new distribution. Only remove this script's fixed staging child directory.
$stage = [IO.Path]::GetFullPath($stage)
if ($stage -ne [IO.Path]::GetFullPath((Join-Path $BuildDirectory 'installer-stage'))) {
    throw 'Unexpected installer staging directory.'
}
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null

# Copy the installed C/C++ toolchain by its package manifests, including its
# runtime dependencies and licence files. Do not copy unrelated MSYS software.
$manifest = & $pacman -Q -l -q @packages
if ($LASTEXITCODE -ne 0) { throw 'Could not read the compiler package manifests.' }
$copied = 0
foreach ($entry in $manifest) {
    if (!$entry.StartsWith('/ucrt64/') -or $entry.EndsWith('/')) { continue }
    $relative = $entry.Substring('/ucrt64/'.Length)
    $source = Join-Path $compilerPrefix $relative
    if (!(Test-Path -LiteralPath $source -PathType Leaf)) { throw "Compiler package file is missing: $source" }
    $destination = Join-Path (Join-Path $stage 'toolchain') $relative
    New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
    if (!(Test-Path -LiteralPath $destination) -or (Get-Item -LiteralPath $destination).Length -ne (Get-Item -LiteralPath $source).Length -or (Get-Item -LiteralPath $destination).LastWriteTimeUtc -ne (Get-Item -LiteralPath $source).LastWriteTimeUtc) {
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }
    ++$copied
}
(& $pacman -Q @packages) | Set-Content -LiteralPath (Join-Path $stage 'toolchain\PACKAGES.txt') -Encoding utf8
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'TOOLCHAIN-NOTICES.md') -Destination (Join-Path $stage 'toolchain\TOOLCHAIN-NOTICES.md') -Force
$sourceStage = Join-Path $stage 'toolchain\sources'
New-Item -ItemType Directory -Force -Path $sourceStage | Out-Null
foreach ($source in $lock.sources) {
    Copy-Item -LiteralPath (Join-Path $SourceCache $source.file) -Destination (Join-Path $sourceStage $source.file) -Force
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'toolchain-sources.json') -Destination (Join-Path $sourceStage 'toolchain-sources.json') -Force
Write-Output "Staged $copied compiler package files."
if ($StageToolchainOnly) { return }

# Keep the legacy switch accepted, but always let the incremental build check
# freshness so the binaries and corresponding source cannot silently diverge.
$cache = Get-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt')
$configuredSource = ($cache | Where-Object { $_.StartsWith('CMAKE_HOME_DIRECTORY:INTERNAL=') }).Split('=',2)[1]
$configuredCompiler = ($cache | Where-Object { $_.StartsWith('CMAKE_CXX_COMPILER:FILEPATH=') }).Split('=',2)[1]
if ([IO.Path]::GetFullPath($configuredSource) -ne (Join-Path $repo 'native') -or
    [IO.Path]::GetFullPath((Split-Path $configuredCompiler)) -ne (Join-Path $compilerPrefix 'bin')) {
    throw 'The build must use this source checkout and the reviewed UCRT64 compiler.'
}
& cmake --build $BuildDirectory --target workbench bench_run --parallel 1
if ($LASTEXITCODE -ne 0) { throw 'Workbench build failed.' }
foreach ($directory in @('native','native\src','native\third_party','assets','templates','source')) {
    New-Item -ItemType Directory -Path (Join-Path $stage $directory) -Force | Out-Null
}
foreach ($name in @('LICENSE','LICENSING.md','THIRD_PARTY_NOTICES.md')) {
    Copy-Item -LiteralPath (Join-Path $repo $name) -Destination (Join-Path $stage $name) -Force
}
Copy-Item -LiteralPath (Join-Path $repo 'licenses') -Destination $stage -Recurse -Force
$sourceZip = Join-Path $stage 'source\life-sim-workbench-source.zip'
git -C $repo archive --format=zip --output=$sourceZip HEAD
if ($LASTEXITCODE -ne 0) { throw 'Could not package the corresponding workbench source.' }
$revision | Set-Content -LiteralPath (Join-Path $stage 'source\REVISION.txt') -Encoding utf8
foreach ($name in @('workbench.exe','bench_run.exe')) {
    Copy-Item -LiteralPath (Join-Path $BuildDirectory $name) -Destination (Join-Path $stage "native\$name") -Force
}
'Life-sim Workbench installed layout v1' | Set-Content -LiteralPath (Join-Path $stage 'native\workbench.install') -Encoding utf8
$sourceRoot = Join-Path $repo 'native\src'
Get-ChildItem -LiteralPath $sourceRoot -Recurse -File | Where-Object { $_.Extension -in @('.hpp','.h') } | ForEach-Object {
    $relative = [IO.Path]::GetRelativePath($sourceRoot, $_.FullName)
    $target = Join-Path (Join-Path $stage 'native\src') $relative
    New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination $target -Force
}
# Corresponding source for vendored Box2D (MIT), linked into the collision lab.
$box2dSrc = Join-Path $repo 'native\third_party\box2d'
$box2dDst = Join-Path $stage 'native\third_party\box2d'
if (Test-Path -LiteralPath $box2dSrc) {
    New-Item -ItemType Directory -Path $box2dDst -Force | Out-Null
    Copy-Item -LiteralPath $box2dSrc -Destination (Join-Path $stage 'native\third_party') -Recurse -Force
    Write-Output 'Staged native/third_party/box2d for corresponding source.'
}

Copy-Item -LiteralPath (Join-Path $repo 'plugins\example_rule.cpp') -Destination (Join-Path $stage 'assets\example_rule.cpp') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'WELCOME.txt') -Destination (Join-Path $stage 'WELCOME.txt') -Force
& (Join-Path $BuildDirectory 'bench_run.exe') --export-templates (Join-Path $stage 'templates')
if ($LASTEXITCODE -ne 0) { throw 'Template export failed.' }

$components = [Collections.Generic.List[string]]::new()
$files = [Collections.Generic.List[string]]::new()
$deletes = [Collections.Generic.List[string]]::new()
foreach ($template in Get-ChildItem -LiteralPath (Join-Path $stage 'templates') -Filter '*.benchsim' -File | Sort-Object Name) {
    $nameLine = Get-Content -LiteralPath $template.FullName | Where-Object { $_.StartsWith('name ') } | Select-Object -First 1
    $title = $nameLine.Substring(6, $nameLine.Length - 7).Replace('"','""')
    $id = $template.BaseName
    if ($id -notmatch '^[a-z0-9]+$') { throw "Unsafe template id: $id" }
    $components.Add('Name: "templates\' + $id + '"; Description: "' + $title + '"; Types: full')
    $files.Add('Source: "{#StageDir}\templates\' + $id + '.benchsim"; DestDir: "{app}\templates"; Flags: ignoreversion; Components: templates\' + $id)
    $deletes.Add('Type: files; Name: "{app}\templates\' + $id + '.benchsim"; Check: not WizardIsComponentSelected(''templates\' + $id + ''')')
}
$generated = "[Components]`r`n" + ($components -join "`r`n") + "`r`n[Files]`r`n" + ($files -join "`r`n") + "`r`n[InstallDelete]`r`n" + ($deletes -join "`r`n")
[IO.File]::WriteAllText((Join-Path $stage 'templates.iss'), $generated, [Text.UTF8Encoding]::new($true))
$output = Join-Path $repo 'dist'
New-Item -ItemType Directory -Path $output -Force | Out-Null
& $InnoCompiler "/DStageDir=$stage" "/DOutputFolder=$output" (Join-Path $PSScriptRoot 'workbench.iss')
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }
Get-Item -LiteralPath (Join-Path $output 'LifeSimWorkbench-Setup.exe') | Select-Object FullName,Length
