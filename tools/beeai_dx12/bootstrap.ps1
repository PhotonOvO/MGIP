[CmdletBinding()]
param(
    [string]$BeeAIRepository = 'F:\GitHub\beeai-framework',
    [string]$ExpectedCommit = '14882f0a16d08439dab919d9b05028464d304fc5'
)

$ErrorActionPreference = 'Stop'
$ToolRoot = $PSScriptRoot
$RepoRoot = (Resolve-Path -LiteralPath (Join-Path $ToolRoot '..\..')).Path
$RuntimeRoot = Join-Path $RepoRoot 'out\beeai_dx12'
$VenvRoot = Join-Path $RuntimeRoot 'venv'
$PythonExe = Join-Path $VenvRoot 'Scripts\python.exe'
$env:UV_CACHE_DIR = Join-Path $RuntimeRoot 'uv-cache'

if (-not (Test-Path -LiteralPath (Join-Path $BeeAIRepository 'python\pyproject.toml'))) {
    throw "BeeAI Python package was not found under: $BeeAIRepository"
}

$ActualCommit = (& git -c "safe.directory=$($BeeAIRepository.Replace('\', '/'))" -C $BeeAIRepository rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $ActualCommit -ne $ExpectedCommit) {
    throw "BeeAI commit mismatch. Expected $ExpectedCommit, found $ActualCommit"
}

if (-not (Test-Path -LiteralPath $PythonExe)) {
    uv venv $VenvRoot --python 3.13
    if ($LASTEXITCODE -ne 0) { throw 'uv venv failed' }
} else {
    $Version = (& $PythonExe -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")').Trim()
    if ($LASTEXITCODE -ne 0 -or $Version -ne '3.13') {
        throw "Existing BeeAI environment uses Python $Version; expected 3.13"
    }
    Write-Host "Reusing Python $Version environment at: $VenvRoot"
}

uv pip install --python $PythonExe --editable (Join-Path $BeeAIRepository 'python')
if ($LASTEXITCODE -ne 0) { throw 'BeeAI editable install failed' }

uv pip install --python $PythonExe --no-deps --editable $ToolRoot
if ($LASTEXITCODE -ne 0) { throw 'MGIF BeeAI tool install failed' }

& $PythonExe -c "import beeai_framework; import mgif_beeai_dx12; print('BeeAI DX12 environment ready')"
