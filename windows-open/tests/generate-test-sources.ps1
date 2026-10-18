[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Project,
    [Parameter(Mandatory)][string]$Profile,
    [Parameter(Mandatory)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$fixtures = Join-Path $repo 'windows-open\iddcx\tests'
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

function Get-Section([string]$Source, [string]$Start, [string]$End) {
    $first = $Source.IndexOf($Start, [StringComparison]::Ordinal)
    if ($first -lt 0) { throw "Production test boundary not found: $Start" }
    $last = $Source.IndexOf($End, $first, [StringComparison]::Ordinal)
    if ($last -le $first) { throw "Production test boundary not found after ${Start}: $End" }
    return $Source.Substring($first, $last - $first)
}

function Get-Body([string]$Function) {
    $first = $Function.IndexOf('{')
    $last = $Function.LastIndexOf('}')
    if ($first -lt 0 -or $last -le $first) { throw 'Production function body boundaries not found' }
    return $Function.Substring($first + 1, $last - $first - 1)
}

switch ($Project) {
    'monitor-departure-tests' {
        $source = Get-Content -LiteralPath (Join-Path $fixtures '..\adapter.cpp') -Raw
        $loop = Get-Section $source '    for (NvU32 index = outputCount_; index-- > 0;)' '    return status;'
        $removal = Get-Body (Get-Section $source 'void Adapter::OnOutputDestroyed(Output* output) noexcept' 'NTSTATUS Adapter::CommitModes(')
        $template = Get-Content -LiteralPath (Join-Path $fixtures 'monitor_departure_test.cpp') -Raw
        $template.Replace('/* PRODUCTION_DEPARTURE_LOOP */', $loop).
            Replace('/* PRODUCTION_OUTPUT_REMOVAL */', $removal) |
            Set-Content -LiteralPath (Join-Path $OutputDirectory 'monitor_departure_test.cpp') -Encoding ascii
    }
    'mpo-retirement-tests' {
        $source = Get-Content -LiteralPath (Join-Path $fixtures '..\mpo_scene.cpp') -Raw
        $body = Get-Body (Get-Section $source 'HRESULT MpoScene::Retire(bool* pending) noexcept' 'bool MpoScene::Due() const noexcept')
        $template = Get-Content -LiteralPath (Join-Path $fixtures 'mpo_retirement_test.cpp') -Raw
        $template.Replace('/* PRODUCTION_RETIREMENT */', $body) |
            Set-Content -LiteralPath (Join-Path $OutputDirectory 'mpo_retirement_test.cpp') -Encoding ascii
    }
    'mpo-request-tests' {
        $source = Get-Content -LiteralPath (Join-Path $repo 'src\nvidia-modeset\src\nvkms-hw-flip.c') -Raw
        (Get-Section $source 'inline static NvBool ValidateFlipChannelEvoHwState(' 'static NvBool ValidateSurfaceSize(').
            Replace('const NVFlipChannelEvoHwState *pState', 'const TestFlipState *pState') |
            Set-Content -LiteralPath (Join-Path $OutputDirectory 'flip_channel_validation.hpp') -Encoding ascii
    }
    'gsp-timeout-tests' {
        $source = (Get-Content -LiteralPath (Join-Path $repo 'src\nvidia\src\kernel\gpu\gsp\kernel_gsp.c') -Raw).Replace("`r`n", "`n")
        Get-Section $source "static NV_STATUS`n_kgspCheckIfMemsysError" '/*' |
            Set-Content -LiteralPath (Join-Path $OutputDirectory 'gsp_timeout_probe.inc') -Encoding ascii
    }
    default {
        $fixture = switch ($Project) {
            'device-removal-tests' { 'device' }
            'scrubber-removal-tests' { 'scrubber' }
            'pma-scrub-removal-tests' { 'pma' }
            default { throw "No source generator for project: $Project" }
        }
        & (Join-Path $fixtures 'generate-removal-sources.ps1') -Fixture $fixture -Profile $Profile -OutputDirectory $OutputDirectory
    }
}
