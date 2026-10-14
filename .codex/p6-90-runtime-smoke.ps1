[CmdletBinding()]
param(
    [string]$Stamp = "20260829T151147Z",
    [int]$TimeoutSeconds = 300
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$BuildRoot = "G:\Temp\mgif-p6-90-msvc-$Stamp-Debug"
$EvidenceRoot = "G:\Temp\mgif-p6-90-evidence-$Stamp\runtime"
$Executable = Join-Path $BuildRoot "Demo.exe"
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "Debug Demo executable is missing: $Executable"
}
New-Item -ItemType Directory -Path $EvidenceRoot -Force | Out-Null

$Records = [System.Collections.Generic.List[object]]::new()
foreach ($Backend in @("vulkan", "d3d12")) {
    $Stdout = Join-Path $EvidenceRoot "$Backend.stdout.log"
    $Stderr = Join-Path $EvidenceRoot "$Backend.stderr.log"
    $Metrics = Join-Path $EvidenceRoot "$Backend.metrics.json"
    if ($Backend -eq "vulkan") {
        $env:VK_INSTANCE_LAYERS = "VK_LAYER_KHRONOS_validation"
        $env:VK_KHRONOS_VALIDATION_VALIDATE_SYNC = "true"
        $env:VK_KHRONOS_VALIDATION_SYNCVAL_SUBMIT_TIME_VALIDATION = "true"
    } else {
        Remove-Item Env:VK_INSTANCE_LAYERS -ErrorAction SilentlyContinue
        Remove-Item Env:VK_KHRONOS_VALIDATION_VALIDATE_SYNC -ErrorAction SilentlyContinue
        Remove-Item Env:VK_KHRONOS_VALIDATION_SYNCVAL_SUBMIT_TIME_VALIDATION -ErrorAction SilentlyContinue
    }
    $Arguments = @(
        "--backend=$Backend",
        "--automation=csm-translate-stop",
        "--fixed-dt=0.016666667",
        "--warmup-frames=8",
        "--motion-frames=24",
        "--hold-frames=8",
        "--no-ui",
        "--no-post",
        "--no-ddgi",
        "--auto-exit",
        "--metrics-output=$Metrics"
    )
    $Process = Start-Process -FilePath $Executable -ArgumentList $Arguments `
        -WorkingDirectory $BuildRoot -PassThru `
        -RedirectStandardOutput $Stdout -RedirectStandardError $Stderr
    if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        throw "$Backend Demo smoke exceeded $TimeoutSeconds seconds"
    }
    $Process.WaitForExit()
    if ($Process.ExitCode -ne 0) {
        throw "$Backend Demo smoke exited with code $($Process.ExitCode)"
    }
    if (-not (Test-Path -LiteralPath $Metrics -PathType Leaf)) {
        throw "$Backend Demo smoke did not write metrics"
    }
    $Combined = (Get-Content -LiteralPath $Stdout -Raw -ErrorAction SilentlyContinue) +
                (Get-Content -LiteralPath $Stderr -Raw -ErrorAction SilentlyContinue)
    $Records.Add([ordered]@{
        backend = $Backend
        exitCode = $Process.ExitCode
        stdout = $Stdout
        stdoutSha256 = (Get-FileHash -LiteralPath $Stdout -Algorithm SHA256).Hash.ToLowerInvariant()
        stderr = $Stderr
        stderrSha256 = (Get-FileHash -LiteralPath $Stderr -Algorithm SHA256).Hash.ToLowerInvariant()
        metrics = $Metrics
        metricsSha256 = (Get-FileHash -LiteralPath $Metrics -Algorithm SHA256).Hash.ToLowerInvariant()
        vuidCount = ([regex]::Matches($Combined, "VUID-")).Count
        syncHazardCount = ([regex]::Matches($Combined, "SYNC-HAZARD")).Count
        validationErrorCount = ([regex]::Matches($Combined, "Validation Error", "IgnoreCase")).Count
        d3d12ErrorCount = ([regex]::Matches($Combined, "D3D12[^\r\n]*(ERROR|CORRUPTION)", "IgnoreCase")).Count
    })
}

$DxilCache = Join-Path $BuildRoot "_autogen\_dxil_cache"
$DxilRecords = @()
if (Test-Path -LiteralPath $DxilCache -PathType Container) {
    $DxilRecords = @(
        Get-ChildItem -LiteralPath $DxilCache -File |
            Where-Object { $_.Name -match '^editor_(picking|highlight)\.slang\.' } |
            Sort-Object Name |
            ForEach-Object {
                [ordered]@{
                    name = $_.Name
                    bytes = [int64]$_.Length
                    sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
                }
            }
    )
}

[ordered]@{
    completedAtUtc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ")
    stamp = $Stamp
    executable = $Executable
    executableSha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
    cases = $Records
    runtimeEditorDxilCache = $DxilRecords
    evidenceLimit = "CSM smoke exercises the production renderer but does not inject editor picking interactions"
} | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $EvidenceRoot "runtime-smoke-manifest.json") -Encoding UTF8
