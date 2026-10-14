[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$DebugBuild,
    [Parameter(Mandatory = $true)][string]$ReleaseBuild
)

$ErrorActionPreference = "Stop"
$cases = @(
    @{ Config = "Release"; Build = $ReleaseBuild; Width = 960; Height = 540; Dpi = 1.25; Token = "960x540-dpi-1p25" },
    @{ Config = "Debug";   Build = $DebugBuild;   Width = 960; Height = 540; Dpi = 1.25; Token = "960x540-dpi-1p25" },
    @{ Config = "Release"; Build = $ReleaseBuild; Width = 960; Height = 540; Dpi = 1.5;  Token = "960x540-dpi-1p5" },
    @{ Config = "Debug";   Build = $DebugBuild;   Width = 960; Height = 540; Dpi = 1.5;  Token = "960x540-dpi-1p5" },
    @{ Config = "Release"; Build = $ReleaseBuild; Width = 960; Height = 540; Dpi = 2.0;  Token = "960x540-dpi-2p0" },
    @{ Config = "Debug";   Build = $DebugBuild;   Width = 960; Height = 540; Dpi = 2.0;  Token = "960x540-dpi-2p0" },
    @{ Config = "Release"; Build = $ReleaseBuild; Width = 1280; Height = 720; Dpi = 1.0; Token = "1280x720-dpi-1" },
    @{ Config = "Debug";   Build = $DebugBuild;   Width = 1280; Height = 720; Dpi = 1.0; Token = "1280x720-dpi-1" },
    @{ Config = "Release"; Build = $ReleaseBuild; Width = 1920; Height = 1080; Dpi = 1.0; Token = "1920x1080-dpi-1" },
    @{ Config = "Debug";   Build = $DebugBuild;   Width = 1920; Height = 1080; Dpi = 1.0; Token = "1920x1080-dpi-1" },
    @{ Config = "Release"; Build = $ReleaseBuild; Width = 2560; Height = 1440; Dpi = 1.0; Token = "2560x1440-dpi-1" },
    @{ Config = "Debug";   Build = $DebugBuild;   Width = 2560; Height = 1440; Dpi = 1.0; Token = "2560x1440-dpi-1" },
    @{ Config = "Release"; Build = $ReleaseBuild; Width = 640; Height = 960; Dpi = 1.0; Token = "640x960-dpi-1" },
    @{ Config = "Debug";   Build = $DebugBuild;   Width = 640; Height = 960; Dpi = 1.0; Token = "640x960-dpi-1" }
)
foreach ($case in $cases) {
    $evidence = Join-Path $Root ("v07-{0}-final-{1}" -f $case.Config.ToLowerInvariant(), $case.Token)
    Write-Output "Capturing $evidence"
    & G:/MGIF/tools/editor_visual_capture.ps1 -BuildRoot $case.Build -EvidenceRoot $evidence `
        -Case V07-dpi-resize -Configuration $case.Config -Width $case.Width `
        -Height $case.Height -DpiScale $case.Dpi -TimeoutSeconds 300
    if (-not $?) { throw "V07 capture failed: $evidence" }
}
