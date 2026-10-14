[CmdletBinding()]
param([string]$Stamp = "20260829T151147Z")

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = "G:\MGIF"
$EvidenceRoot = "G:\Temp\mgif-p6-90-evidence-$Stamp"
$OutputRoot = Join-Path $EvidenceRoot "shaders"
$SpirvVal = "C:\VulkanSDK\1.4.328.1\Bin\spirv-val.exe"
$SpirvDis = "C:\VulkanSDK\1.4.328.1\Bin\spirv-dis.exe"
$Slangc = "G:\Temp\mgif-p6-00-msvc-full-20260828\_deps\Slang-windows-x86_64-2026.3.1\bin\slangc.exe"
$Dxc = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe"

foreach ($Tool in @($SpirvVal, $SpirvDis, $Slangc, $Dxc)) {
    if (-not (Test-Path -LiteralPath $Tool -PathType Leaf)) {
        throw "Required shader tool is unavailable: $Tool"
    }
}
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null

$Records = [System.Collections.Generic.List[object]]::new()
foreach ($Configuration in @("Debug", "Release")) {
    $Autogen = "G:\Temp\mgif-p6-90-msvc-$Stamp-$Configuration\_autogen"
    foreach ($Name in @("editor_picking.slang", "editor_highlight.slang")) {
        $InputPath = Join-Path $Autogen "$Name.spv"
        if (-not (Test-Path -LiteralPath $InputPath -PathType Leaf)) {
            throw "Generated SPIR-V is missing: $InputPath"
        }
        & $SpirvVal --target-env vulkan1.3 $InputPath
        if ($LASTEXITCODE -ne 0) {
            throw "spirv-val rejected $InputPath"
        }
        $AssemblyPath = Join-Path $OutputRoot "$Configuration-$Name.spvasm"
        & $SpirvDis $InputPath -o $AssemblyPath
        if ($LASTEXITCODE -ne 0) {
            throw "spirv-dis rejected $InputPath"
        }
        $Records.Add([ordered]@{
            kind = "spirv"
            configuration = $Configuration
            source = $InputPath
            bytes = [int64](Get-Item -LiteralPath $InputPath).Length
            sha256 = (Get-FileHash -LiteralPath $InputPath -Algorithm SHA256).Hash.ToLowerInvariant()
            validation = "vulkan1.3-pass"
            disassembly = $AssemblyPath
            disassemblySha256 = (Get-FileHash -LiteralPath $AssemblyPath -Algorithm SHA256).Hash.ToLowerInvariant()
        })
    }
}

$Entries = @(
    [ordered]@{ source = "editor_picking.slang"; entry = "editorPickingVertexMain"; stage = "vertex"; suffix = "" },
    [ordered]@{ source = "editor_picking.slang"; entry = "editorPickingFragmentMain"; stage = "fragment"; suffix = "-alpha0"; define = "DEMO_SPECIALIZATION_0=0" },
    [ordered]@{ source = "editor_picking.slang"; entry = "editorPickingFragmentMain"; stage = "fragment"; suffix = "-alpha1"; define = "DEMO_SPECIALIZATION_0=1" },
    [ordered]@{ source = "editor_highlight.slang"; entry = "editorPickSampleMain"; stage = "compute"; suffix = "" },
    [ordered]@{ source = "editor_highlight.slang"; entry = "editorHighlightVertexMain"; stage = "vertex"; suffix = "" },
    [ordered]@{ source = "editor_highlight.slang"; entry = "editorHighlightFragmentMain"; stage = "fragment"; suffix = "" }
)

foreach ($Entry in $Entries) {
    $SourcePath = Join-Path $RepoRoot "shaders\$($Entry.source)"
    $Stem = "$($Entry.entry)$($Entry.suffix)"
    $DxilPath = Join-Path $OutputRoot "$Stem.dxil"
    $ReflectionPath = Join-Path $OutputRoot "$Stem.dx.json"
    $Arguments = @(
        $SourcePath,
        "-lang", "slang",
        "-matrix-layout-column-major",
        "-O0", "-g1",
        "-entry", [string]$Entry.entry,
        "-stage", [string]$Entry.stage,
        "-I", $RepoRoot,
        "-DDEMO_D3D12=1",
        "-target", "dxil",
        "-profile", "sm_6_6",
        "-reflection-json", $ReflectionPath,
        "-o", $DxilPath
    )
    if ($Entry.Contains("define")) {
        $Arguments += "-D$($Entry.define)"
    }
    & $Slangc @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "slangc DXIL compilation failed for $Stem"
    }
    $DumpPath = Join-Path $OutputRoot "$Stem.dxil.txt"
    & $Dxc -dumpbin $DxilPath | Set-Content -LiteralPath $DumpPath -Encoding UTF8
    if ($LASTEXITCODE -ne 0) {
        throw "DXC rejected generated DXIL for $Stem"
    }
    $Records.Add([ordered]@{
        kind = "dxil"
        source = $SourcePath
        entryPoint = [string]$Entry.entry
        stage = [string]$Entry.stage
        specialization = if ($Entry.Contains("define")) { [string]$Entry.define } else { $null }
        output = $DxilPath
        bytes = [int64](Get-Item -LiteralPath $DxilPath).Length
        sha256 = (Get-FileHash -LiteralPath $DxilPath -Algorithm SHA256).Hash.ToLowerInvariant()
        reflection = $ReflectionPath
        reflectionSha256 = (Get-FileHash -LiteralPath $ReflectionPath -Algorithm SHA256).Hash.ToLowerInvariant()
        dumpbin = $DumpPath
        dumpbinSha256 = (Get-FileHash -LiteralPath $DumpPath -Algorithm SHA256).Hash.ToLowerInvariant()
        validation = "dxc-dumpbin-pass"
    })
}

[ordered]@{
    completedAtUtc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ")
    stamp = $Stamp
    tools = [ordered]@{
        spirvVal = $SpirvVal
        spirvDis = $SpirvDis
        slangc = $Slangc
        dxc = $Dxc
    }
    records = $Records
} | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $OutputRoot "shader-validation-manifest.json") -Encoding UTF8
