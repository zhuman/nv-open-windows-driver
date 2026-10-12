[CmdletBinding()]
param([string]$VcVarsPath, [string]$WdfIncludePath, [string]$WilIncludePath)
$ErrorActionPreference = 'Stop'
if (-not $VcVarsPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'Visual C++ build tools were not found' }
    $VcVarsPath = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
}
if (-not (Test-Path -LiteralPath $VcVarsPath)) { throw 'vcvars64.bat was not found' }
if (-not $WdfIncludePath) {
    $WdfIncludePath = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include\wdf\umdf\2.35'
}
if (-not $WilIncludePath) {
    $packages = if ($env:NUGET_PACKAGES) { $env:NUGET_PACKAGES } else { Join-Path $env:USERPROFILE '.nuget\packages' }
    $WilIncludePath = Join-Path $packages 'microsoft.windows.implementationlibrary\1.0.260126.7\include'
}
if (-not (Test-Path (Join-Path $WdfIncludePath 'wdf.h'))) { throw 'UMDF headers were not found' }
if (-not (Test-Path (Join-Path $WilIncludePath 'wil\resource.h'))) { throw 'Restored WIL headers were not found' }
$output = Join-Path ([IO.Path]::GetTempPath()) ("nv-iddcx-dirty-tests-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output | Out-Null
$source = Join-Path $PSScriptRoot '..\dirty_regions.cpp'
$tests = Join-Path $PSScriptRoot 'dirty_regions_test.cpp'
$parametersTests = Join-Path $PSScriptRoot 'driver_parameters_test.cpp'
$ownershipTests = Join-Path $PSScriptRoot 'frame_slots_test.cpp'
$presentationTests = Join-Path $PSScriptRoot 'presentation_locks_test.cpp'
$patternTests = Join-Path $PSScriptRoot 'mpo_patterns_test.cpp'
$departureTests = Join-Path $output 'monitor_departure_test.cpp'
$retirementTests = Join-Path $output 'mpo_retirement_test.cpp'
try {
    $adapterSource = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\adapter.cpp') -Raw
    $departureStart = $adapterSource.IndexOf('    for (NvU32 index = outputCount_; index-- > 0;)')
    $departureEnd = $adapterSource.IndexOf("    return status;", $departureStart)
    $removalStart = $adapterSource.IndexOf("void Adapter::OnOutputDestroyed(Output* output) noexcept")
    $removalEnd = $adapterSource.IndexOf("NTSTATUS Adapter::CommitModes(", $removalStart)
    if ($departureStart -lt 0 -or $departureEnd -le $departureStart -or
        $removalStart -lt 0 -or $removalEnd -le $removalStart) {
        throw 'Production monitor departure test boundaries were not found'
    }
    $departureLoop = $adapterSource.Substring($departureStart, $departureEnd - $departureStart)
    $removal = $adapterSource.Substring($removalStart, $removalEnd - $removalStart)
    $removalBody = $removal.Substring($removal.IndexOf('{') + 1)
    $removalBody = $removalBody.Substring(0, $removalBody.LastIndexOf('}'))
    $departureTemplate = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'monitor_departure_test.cpp') -Raw
    $departureTemplate.Replace('/* PRODUCTION_DEPARTURE_LOOP */', $departureLoop).
        Replace('/* PRODUCTION_OUTPUT_REMOVAL */', $removalBody) |
        Set-Content -LiteralPath $departureTests -Encoding ascii
    & $env:ComSpec /c "call `"$VcVarsPath`" >nul && cd /d `"$output`" && cl /nologo /std:c++20 /EHsc /W4 /WX /MT /Fe:monitor_departure_test.exe `"$departureTests`" && monitor_departure_test.exe"
    if ($LASTEXITCODE -ne 0) { throw "Monitor departure tests failed: $LASTEXITCODE" }
    $sceneSource = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\mpo_scene.cpp') -Raw
    $retireStart = $sceneSource.IndexOf('HRESULT MpoScene::Retire(bool* pending) noexcept')
    $retireEnd = $sceneSource.IndexOf('bool MpoScene::Due() const noexcept', $retireStart)
    if ($retireStart -lt 0 -or $retireEnd -le $retireStart) {
        throw 'Production MPO retirement test boundaries were not found'
    }
    $retirement = $sceneSource.Substring($retireStart, $retireEnd - $retireStart)
    $retirement = $retirement.Substring($retirement.IndexOf('{') + 1)
    $retirement = $retirement.Substring(0, $retirement.LastIndexOf('}'))
    $retireTemplate = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'mpo_retirement_test.cpp') -Raw
    $retireTemplate.Replace('/* PRODUCTION_RETIREMENT */', $retirement).
        Replace('#include "../frame_slots.hpp"', '#include "' + (Join-Path $PSScriptRoot '..\frame_slots.hpp') + '"') |
        Set-Content -LiteralPath $retirementTests -Encoding ascii
    & $env:ComSpec /c "call `"$VcVarsPath`" >nul && cd /d `"$output`" && cl /nologo /std:c++20 /EHsc /W4 /WX /MT /Fe:mpo_retirement_test.exe `"$retirementTests`" && mpo_retirement_test.exe"
    if ($LASTEXITCODE -ne 0) { throw "MPO retirement tests failed: $LASTEXITCODE" }
    & $env:ComSpec /c "call `"$VcVarsPath`" >nul && cd /d `"$output`" && cl /nologo /std:c++20 /EHsc /W4 /WX /MT /Fe:dirty_regions_test.exe `"$source`" `"$tests`" && dirty_regions_test.exe && cl /nologo /std:c++20 /EHsc /W4 /WX /MT /DUMDF_DRIVER /external:W0 /external:I`"$WdfIncludePath`" /external:I`"$WilIncludePath`" /Fe:driver_parameters_test.exe `"$parametersTests`" && driver_parameters_test.exe && cl /nologo /std:c++20 /EHsc /W4 /WX /MT /Fe:frame_slots_test.exe `"$ownershipTests`" dirty_regions.obj && frame_slots_test.exe"
    if ($LASTEXITCODE -ne 0) { throw "Dirty-region tests failed: $LASTEXITCODE" }
    & $env:ComSpec /c "call `"$VcVarsPath`" >nul && cd /d `"$output`" && cl /nologo /std:c++20 /EHsc /W4 /WX /MT /Fe:presentation_locks_test.exe `"$presentationTests`" && presentation_locks_test.exe"
    if ($LASTEXITCODE -ne 0) { throw "Presentation lock tests failed: $LASTEXITCODE" }
    & $env:ComSpec /c "call `"$VcVarsPath`" >nul && cd /d `"$output`" && cl /nologo /std:c++20 /EHsc /W4 /WX /MT /Fe:mpo_patterns_test.exe `"$patternTests`" && mpo_patterns_test.exe"
    if ($LASTEXITCODE -ne 0) { throw "MPO pattern tests failed: $LASTEXITCODE" }
} finally {
    foreach ($name in @('dirty_regions.obj','dirty_regions_test.obj','dirty_regions_test.exe',
        'driver_parameters_test.obj','driver_parameters_test.exe','frame_slots_test.obj','frame_slots_test.exe',
        'presentation_locks_test.obj','presentation_locks_test.exe',
        'mpo_patterns_test.obj','mpo_patterns_test.exe',
        'monitor_departure_test.cpp','monitor_departure_test.obj','monitor_departure_test.exe',
        'mpo_retirement_test.cpp','mpo_retirement_test.obj','mpo_retirement_test.exe')) {
        $path = Join-Path $output $name
        if (Test-Path -LiteralPath $path) {
            # Windows can briefly retain the executable after process exit.
            for ($attempt = 0; ; ++$attempt) {
                try { Remove-Item -LiteralPath $path; break }
                catch [System.UnauthorizedAccessException] {
                    if ($attempt -eq 9) { throw }
                    Start-Sleep -Milliseconds 200
                }
                catch [System.IO.IOException] {
                    if ($attempt -eq 9) { throw }
                    Start-Sleep -Milliseconds 200
                }
            }
        }
    }
    if (@(Get-ChildItem -LiteralPath $output -Force).Count -eq 0) { Remove-Item -LiteralPath $output }
}
