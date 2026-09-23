param([string]$CacheDirectory)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (!$CacheDirectory) { $CacheDirectory = Join-Path $repo 'native/build-runner/license-sources' }
$lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'toolchain-sources.json') -Raw | ConvertFrom-Json
if ($lock.schema -ne 1) { throw 'Unsupported toolchain source lock.' }
New-Item -ItemType Directory -Force -Path $CacheDirectory | Out-Null
foreach ($source in $lock.sources) {
    if ($source.file -notmatch '^mingw-w64-[A-Za-z0-9._+-]+\.src\.tar\.zst$' -or
        $source.url -ne ('https://repo.msys2.org/mingw/sources/' + $source.file) -or
        $source.sha256 -notmatch '^[0-9a-f]{64}$') { throw 'Invalid source lock entry.' }
    $target = Join-Path $CacheDirectory $source.file
    if (!(Test-Path -LiteralPath $target)) {
        Write-Output "Downloading corresponding source: $($source.file)"
        Invoke-WebRequest -Uri $source.url -OutFile ($target + '.partial') -TimeoutSec 180
        if ((Get-FileHash -LiteralPath ($target + '.partial') -Algorithm SHA256).Hash -ne $source.sha256) {
            throw "Downloaded source hash mismatch: $($source.file)"
        }
        Move-Item -LiteralPath ($target + '.partial') -Destination $target -Force
    }
    if ((Get-Item -LiteralPath $target).Length -ne $source.bytes -or
        (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $source.sha256) {
        throw "Cached source does not match the reviewed package: $($source.file)"
    }
}
Write-Output "Verified $($lock.sources.Count) corresponding-source archives."
