[CmdletBinding()]
param(
    [string]$Stamp = "20260829T062326Z",
    [int]$Parallel = 4
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = "G:\MGIF"
$DonorRoot = "G:\Temp\mgif-p6-00-msvc-full-20260828"
$EvidenceRoot = "G:\Temp\mgif-p6-90-evidence-$Stamp"
$VsDevCmd = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
$SlangRoot = Join-Path $DonorRoot "_deps\Slang-windows-x86_64-2026.3.1"
$DependencyNames = @(
    "glfw",
    "glm",
    "imgui",
    "meshoptimizer",
    "tinygltf",
    "volk",
    "vulkanmemoryallocator"
)
$BuildTargets = @(
    "scene_selection_tests",
    "material_clipboard_codec_tests",
    "editor_viewport_metrics_tests",
    "scene_render_occurrence_mapping_tests",
    "editor_picking_rhi_tests",
    "editor_selection_contract_tests",
    "editor_picking_contract_tests",
    "editor_input_router_tests",
    "editor_selection_picking_integration_tests",
    "editor_controller_animation_tests",
    "editor_controller_transaction_tests",
    "editor_rhi_contract_tests",
    "editor_transaction_smoke_tests",
    "editor_transaction_path_tests",
    "p2_projection_cpu_tests",
    "renderer_scene_publication_tests",
    "scene_runtime_mapping_tests",
    "scene_change_capture_tests",
    "scene_delta_fake_rhi_tests",
    "editor_history_core_tests",
    "editor_history_structural_tests",
    "editor_history_property_tests",
    "editor_continuous_edit_tests",
    "rhi_public_contract_tests",
    "vulkan_conversion_tests",
    "vulkan_command_buffer_handle_tests",
    "vulkan_allocation_failure_tests",
    "rhi_backend_factory_tests",
    "d3d12_device_smoke_tests",
    "d3d12_clear_present_tests",
    "Demo"
)
$CTestSelection = "^(scene_editor\.(scene_selection|material_clipboard_codec|viewport_metrics|render_occurrence_mapping|picking_rhi|selection_contract|picking_contract|input_router|selection_picking_integration|editor_controller_animation|editor_controller_transaction|rhi_contract|editor_transaction_smoke|editor_transaction_path|scene_projection|renderer_scene_publication|scene_runtime_mapping|scene_change_capture|scene_delta_fake_rhi|p4\.(core_execute|capacity_merge|branch_clear|structural|properties|continuous_lifecycle)|android_viewer_contract)|rhi_public_contract|vulkan_conversion|vulkan_command_buffer_handles|vulkan_allocation_failures|rhi_backend_factory|d3d12_device_smoke|d3d12_clear_present)$"

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$File,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    Write-Host ("> " + $File + " " + ($Arguments -join " "))
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$File exited with code $LASTEXITCODE"
    }
}

function Import-VsEnvironment {
    if (-not (Test-Path -LiteralPath $VsDevCmd -PathType Leaf)) {
        throw "VS 2022 developer environment is unavailable: $VsDevCmd"
    }
    $Command = "call `"$VsDevCmd`" -no_logo -arch=amd64 -host_arch=amd64 >nul && set"
    $EnvironmentLines = & $env:ComSpec /d /c $Command
    if ($LASTEXITCODE -ne 0) {
        throw "VsDevCmd failed with code $LASTEXITCODE"
    }
    foreach ($Line in $EnvironmentLines) {
        $Separator = $Line.IndexOf("=")
        if ($Separator -le 0) {
            continue
        }
        $Name = $Line.Substring(0, $Separator)
        $Value = $Line.Substring($Separator + 1)
        Set-Item -LiteralPath "Env:$Name" -Value $Value
    }
}

function Initialize-FreshBuildRoot {
    param([Parameter(Mandatory = $true)][string]$BuildRoot)

    if (-not (Test-Path -LiteralPath $BuildRoot -PathType Container)) {
        New-Item -ItemType Directory -Path $BuildRoot | Out-Null
    }
    $Unexpected = @(
        Get-ChildItem -LiteralPath $BuildRoot -Force |
            Where-Object { $_.Name -ne "_deps" }
    )
    if ($Unexpected.Count -ne 0) {
        throw "Build root is not fresh: $BuildRoot"
    }

    $BuildDeps = Join-Path $BuildRoot "_deps"
    if (-not (Test-Path -LiteralPath $BuildDeps -PathType Container)) {
        New-Item -ItemType Directory -Path $BuildDeps | Out-Null
    }
    foreach ($Name in $DependencyNames) {
        $Source = Join-Path $DonorRoot "_deps\$Name-src"
        $Link = Join-Path $BuildDeps "$Name-src"
        if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
            throw "Dependency donor is unavailable: $Source"
        }
        if (Test-Path -LiteralPath $Link) {
            throw "Fresh dependency path already exists: $Link"
        }
        Copy-Item -LiteralPath $Source -Destination $Link -Recurse -Force
    }
}

function Write-AuthorityManifest {
    $RebindPath = Join-Path $RepoRoot "docs\editor-parity\p6-90-write-set-rebind.json"
    $Rebind = Get-Content -LiteralPath $RebindPath -Raw | ConvertFrom-Json
    $Records = foreach ($Entry in $Rebind.newWriteSet) {
        $Path = Join-Path $RepoRoot ([string]$Entry.path).Replace("/", "\")
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
            throw "Authority path is missing: $($Entry.path)"
        }
        $Item = Get-Item -LiteralPath $Path
        [ordered]@{
            path = [string]$Entry.path
            expectedDisposition = [string]$Entry.expectedDisposition
            bytes = [int64]$Item.Length
            sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
            preimageSha256 = if ($null -eq $Entry.sha256) { $null } else { [string]$Entry.sha256 }
        }
    }
    $Manifest = [ordered]@{
        generatedAtUtc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ")
        repository = $RepoRoot
        rebindSha256 = (Get-FileHash -LiteralPath $RebindPath -Algorithm SHA256).Hash.ToLowerInvariant()
        revisionSha256 = [string]$Rebind.revisionSha256
        paths = @($Records)
    }
    $Manifest | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath (Join-Path $EvidenceRoot "authority-source-manifest.json") -Encoding UTF8

}

if (-not (Test-Path -LiteralPath $EvidenceRoot -PathType Container)) {
    New-Item -ItemType Directory -Path $EvidenceRoot | Out-Null
}
$TranscriptPath = Join-Path $EvidenceRoot "windows-msvc-build-test.log"
Start-Transcript -LiteralPath $TranscriptPath -Force | Out-Null

$Succeeded = $false
try {
    Import-VsEnvironment
    $CMake = (Get-Command cmake.exe -ErrorAction Stop).Source
    $CTest = (Get-Command ctest.exe -ErrorAction Stop).Source
    $Ninja = (Get-Command ninja.exe -ErrorAction Stop).Source
    $Cl = (Get-Command cl.exe -ErrorAction Stop).Source

    Write-Host "MSVC=$Cl"
    Invoke-Native -File $CMake -Arguments @("--version")
    Invoke-Native -File $Ninja -Arguments @("--version")
    $ClBanner = & $env:ComSpec /d /c "`"$Cl`" 2>&1"
    $ClBanner | Select-Object -First 3 | Write-Host
    Write-AuthorityManifest

    foreach ($Configuration in @("Debug", "Release")) {
        $BuildRoot = "G:\Temp\mgif-p6-90-msvc-$Stamp-$Configuration"
        Initialize-FreshBuildRoot -BuildRoot $BuildRoot

        Invoke-Native -File $CMake -Arguments @(
            "-S", $RepoRoot,
            "-B", $BuildRoot,
            "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=$Configuration",
            "-DBUILD_TESTING=ON",
            "-DUSE_SLANG=ON",
            "-DDEMO_ENABLE_D3D12=ON",
            "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
            "-DSlang_ROOT=$SlangRoot"
        )

        Invoke-Native -File $CMake -Arguments @(
            "--build", $BuildRoot,
            "--target", "scene_editor_dependency_preflight",
            "--parallel", "$Parallel"
        )
        $BuildArguments = @("--build", $BuildRoot, "--target")
        $BuildArguments += $BuildTargets
        $BuildArguments += @("--parallel", "$Parallel")
        Invoke-Native -File $CMake -Arguments $BuildArguments
        Invoke-Native -File $CTest -Arguments @(
            "--test-dir", $BuildRoot,
            "-C", $Configuration,
            "-R", $CTestSelection,
            "--output-on-failure",
            "--parallel", "$Parallel",
            "--output-junit", (Join-Path $EvidenceRoot "ctest-$Configuration.xml")
        )
    }
    $Succeeded = $true
}
catch {
    Write-Error $_
}
finally {
    [ordered]@{
        completedAtUtc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ")
        succeeded = $Succeeded
        stamp = $Stamp
        debugRoot = "G:\Temp\mgif-p6-90-msvc-$Stamp-Debug"
        releaseRoot = "G:\Temp\mgif-p6-90-msvc-$Stamp-Release"
        evidenceRoot = $EvidenceRoot
    } | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $EvidenceRoot "windows-msvc-build-test-result.json") -Encoding UTF8
    Stop-Transcript | Out-Null
}

if (-not $Succeeded) {
    exit 1
}
