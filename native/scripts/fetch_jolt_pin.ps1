# Fetch the pinned JoltPhysics v5.6.0 sources into native/third_party/jolt/JoltPhysics.
# Optional local groundwork only — not linked by default (BENCH_WITH_JOLT=OFF).
# Usage (from repo root):
#   powershell -ExecutionPolicy Bypass -File native\scripts\fetch_jolt_pin.ps1
param(
    [string]$DestParent = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
if (-not $DestParent) { $DestParent = Join-Path $RepoRoot "native\third_party\jolt" }
$Dest = Join-Path $DestParent "JoltPhysics"
$PinCommit = "e77f175595e64cb44218cc9d9d56fc365ad0e36a"
$Url = "https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.6.0.zip"
$Zip = Join-Path $DestParent "jolt-v5.6.0.zip"

New-Item -ItemType Directory -Force -Path $DestParent | Out-Null
if (Test-Path $Dest) {
    Write-Host "Already present: $Dest"
    Write-Host "Remove it first to re-fetch."
    exit 0
}

Write-Host "Downloading $Url ..."
Invoke-WebRequest -Uri $Url -OutFile $Zip -UseBasicParsing
Write-Host "Extracting..."
Expand-Archive -Path $Zip -DestinationPath $DestParent -Force
$extracted = Get-ChildItem $DestParent -Directory | Where-Object { $_.Name -like "JoltPhysics-*" } | Select-Object -First 1
if (-not $extracted) { throw "Extracted JoltPhysics folder not found" }
Rename-Item $extracted.FullName $Dest
Remove-Item $Zip -Force
Set-Content -Path (Join-Path $DestParent "FETCHED.txt") -Value "tag=v5.6.0`nexpected_commit=$PinCommit`nfetched_from=$Url`nnote=Verify commit inside the tree if you need bit-exact pin matching.`n" -Encoding utf8
Write-Host "Fetched to $Dest"
Write-Host "Default build still uses the stub only. See native/third_party/jolt/PIN.md"
