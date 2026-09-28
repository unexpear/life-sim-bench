# Download the optional FlyWire CC BY-NC local pack used by the Shiu/Brian2 reference.
# Writes under userdata/packs/flywire-nc/ (gitignored). Never stages into the installer.
# Usage (from repo root):
#   powershell -ExecutionPolicy Bypass -File native\scripts\fetch_flywire_nc.ps1
param(
    [string]$DestDir = "",
    [string]$SourceBase = "https://raw.githubusercontent.com/philshiu/Drosophila_brain_model/master"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
if (-not $DestDir) { $DestDir = Join-Path $RepoRoot "userdata\packs\flywire-nc" }
New-Item -ItemType Directory -Force -Path $DestDir | Out-Null

$files = @(
    @{ Name = "Connectivity_783.parquet"; Url = "$SourceBase/Connectivity_783.parquet" },
    @{ Name = "Completeness_783.csv";     Url = "$SourceBase/Completeness_783.csv" },
    @{ Name = "SHIU-MODEL-LICENSE.txt";   Url = "$SourceBase/LICENSE" }
)

foreach ($f in $files) {
    $out = Join-Path $DestDir $f.Name
    Write-Host "Fetching $($f.Name) ..."
    Invoke-WebRequest -Uri $f.Url -OutFile $out -UseBasicParsing
}

$Notice = @"
FlyWire public connectome data (local optional pack)
=====================================================

License: Creative Commons Attribution-NonCommercial 4.0 International (CC BY-NC 4.0)
Guidelines: https://flywire.ai/guidelines
License text: https://creativecommons.org/licenses/by-nc/4.0/

This folder holds connectivity/completeness tables downloaded for local research
with the Shiu/Brian2 reference. It is NOT part of the Life-sim Workbench GPL
installer and must not be staged into installer components or main shipped
templates.

Code licenses (GPL for this workbench; MIT for the Shiu model) do not relicense
this data. Commercial redistribution of this pack is not authorized by CC BY-NC.

See native/PACKS.md and native/RESEARCH-NEXT.md.
"@
Set-Content -Path (Join-Path $DestDir "NOTICE.txt") -Value $Notice -Encoding utf8

$PyCandidates = @(
    (Join-Path $RepoRoot "userdata\brian2-venv\Scripts\python.exe"),
    (Get-Command python -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)
) | Where-Object { $_ -and (Test-Path $_) }

$Py = $PyCandidates | Select-Object -First 1
if (-not $Py) { throw "No Python found to write MANIFEST.json" }

& $Py -c @"
import hashlib, json, pathlib
root = pathlib.Path(r'$DestDir')
files = {}
for name in ['Connectivity_783.parquet', 'Completeness_783.csv']:
    p = root / name
    h = hashlib.sha256()
    with p.open('rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    files[name] = {'sha256': h.hexdigest(), 'bytes': p.stat().st_size}
pack_hash = files['Connectivity_783.parquet']['sha256']
manifest = {
    'pack_id': 'flywire-nc',
    'license': 'CC BY-NC 4.0',
    'license_url': 'https://creativecommons.org/licenses/by-nc/4.0/',
    'source': 'https://flywire.ai/guidelines',
    'shiu_model_pin': 'philshiu/Drosophila_brain_model Connectivity_783 / Completeness_783',
    'dataset': 'FlyWire public release v783',
    'dataset_hash': pack_hash,
    'files': files,
    'not_for_installer': True,
    'fetched_local': True,
    'note': 'Optional local research pack. Never stage into the GPL installer or main shipped templates.'
}
(root / 'MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
print('dataset_hash', pack_hash)
print('bytes', sum(v['bytes'] for v in files.values()))
"@

Write-Host "Pack ready at $DestDir"
Write-Host "Point LIFESIM_FLYWIRE_PACK at this folder, or rely on userdata/packs/flywire-nc default."
Write-Host "DO NOT copy this into installer/ staging."
