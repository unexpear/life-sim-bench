# Create or refresh the portable Brian2 venv for the Shiu neural reference on Windows.
# Usage (from repo root):
#   powershell -ExecutionPolicy Bypass -File native\scripts\setup_brian2_windows.ps1
# Optional: -VenvDir path  -SkipCythonCheck
param(
    [string]$VenvDir = "",
    [switch]$SkipCythonCheck
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
if (-not $VenvDir) { $VenvDir = Join-Path $RepoRoot "userdata\brian2-venv" }
$Req = Join-Path $PSScriptRoot "requirements-brian2.txt"
$Verify = Join-Path $PSScriptRoot "verify_brian2.py"

Write-Host "Repo: $RepoRoot"
Write-Host "Venv: $VenvDir"

function Find-Uv {
    $u = Get-Command uv -ErrorAction SilentlyContinue
    if ($u) { return $u.Source }
    return $null
}

$uv = Find-Uv
if (-not $uv) { throw "uv not found on PATH. Install uv or create the venv manually with Python 3.11." }

if (-not (Test-Path (Join-Path $VenvDir "Scripts\python.exe"))) {
    Write-Host "Creating Python 3.11 venv..."
    & $uv venv --python 3.11 $VenvDir
} else {
    Write-Host "Existing venv found."
}

$Py = Join-Path $VenvDir "Scripts\python.exe"
Write-Host "Installing pinned requirements..."
& $uv pip install --python $Py -r $Req

Write-Host "Import check (numpy codegen)..."
& $Py $Verify --numpy-only
if ($LASTEXITCODE -ne 0) { throw "Brian2 numpy verify failed" }

$Vcvars = @(
    "${env:ProgramFiles}\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if ($SkipCythonCheck) {
    Write-Host "Skipping Cython/MSVC check (-SkipCythonCheck)."
} elseif ($Vcvars) {
    Write-Host "MSVC vcvars found: $Vcvars"
    Write-Host "Cython codegen check..."
    $cmd = "call `"$Vcvars`" >nul && `"$Py`" `"$Verify`" --cython"
    cmd.exe /c $cmd
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "Cython codegen failed. Brian2 still imports; use --numpy-only or fix MSVC."
    } else {
        Write-Host "Cython + MSVC codegen OK."
    }
} else {
    Write-Warning "No vcvars64.bat found. Cython codegen needs MSVC Build Tools; numpy target still works."
}

Write-Host ""
Write-Host "STATUS: Brian2 venv ready at $VenvDir"
Write-Host "Activate:  $VenvDir\Scripts\Activate.ps1"
Write-Host "Point fly arena / actor tooling at: $Py"
Write-Host "FlyWire optional pack: userdata\packs\flywire-nc\ (see fetch_flywire_nc.ps1)"
