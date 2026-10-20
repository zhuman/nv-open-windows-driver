[CmdletBinding()]
param(
    [ValidateSet('All', 'Presentation', 'BarResources', 'MonitorI2c', 'Removal', 'Modeset', 'Platform', 'DisplayPort')]
    [string]$Suite = 'All',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [ValidateSet('x64', 'ARM64')][string]$Platform = 'x64',
    [switch]$VerifyBaseline,
    [switch]$BuildOnly,
    [string]$Filter = '*',
    [string]$VisualStudio,
    [string]$VcVarsPath,
    [string]$WdfIncludePath,
    [string]$WilIncludePath
)

$ErrorActionPreference = 'Stop'
if ($VisualStudio -and $VcVarsPath) { throw 'Specify either VisualStudio or VcVarsPath, not both.' }
if ($VcVarsPath) {
    if (-not (Test-Path -LiteralPath $VcVarsPath)) { throw "vcvars path not found: $VcVarsPath" }
    $VisualStudio = (Resolve-Path (Join-Path (Split-Path $VcVarsPath -Parent) '..\..\..')).Path
}
if (-not $VisualStudio) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer/vswhere was not found.' }
    $VisualStudio = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $VisualStudio) { throw 'Visual Studio MSBuild was not found.' }
}
$msbuild = Join-Path $VisualStudio 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path -LiteralPath $msbuild)) { throw "MSBuild was not found: $msbuild" }
if (-not $BuildOnly -and $Platform -eq 'ARM64' -and
    [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne
        [System.Runtime.InteropServices.Architecture]::Arm64) {
    throw 'ARM64 tests must run on an ARM64 host. Use -BuildOnly to cross-compile.'
}
$groups = [ordered]@{
    Presentation = @('dirty-regions-tests', 'driver-parameters-tests', 'frame-slots-tests',
        'presentation-locks-tests', 'mpo-patterns-tests', 'monitor-departure-tests', 'mpo-retirement-tests')
    BarResources = @('bar-resources-tests', 'pci-bars-tests')
    MonitorI2c = @('monitor-i2c-tests')
    Removal = @('device-removal-tests', 'scrubber-removal-tests', 'pma-scrub-removal-tests')
    Modeset = @('mpo-request-tests')
    Platform = @('debug-log-tests', 'gsp-timeout-tests')
    DisplayPort = @('dp-trace-tests')
}
$projects = if ($Suite -eq 'All') { @($groups.Values | ForEach-Object { $_ }) } else { $groups[$Suite] }
$runs = 0
$testCount = 0
foreach ($project in $projects) {
    $profiles = @('default')
    if ($project -in $groups.Removal) {
        $profiles += @('diagnostics', 'native-policy', 'native-diagnostics')
        if ($project -eq 'device-removal-tests') {
            $profiles += @('downstream-disabled', 'native-downstream-disabled')
        }
        if ($VerifyBaseline) {
            $profiles += 'baseline'
            if ($project -eq 'device-removal-tests') { $profiles += 'dma-baseline' }
        }
    } elseif ($project -eq 'gsp-timeout-tests') {
        $profiles += 'native-policy'
    }
    foreach ($profile in $profiles) {
        $arguments = @((Join-Path $PSScriptRoot "$project.vcxproj"), '/restore', '/m', '/nologo', '/v:minimal',
            "/p:Configuration=$Configuration", "/p:Platform=$Platform", "/p:NvTestProfile=$profile")
        if ($WdfIncludePath) { $arguments += "/p:NvTestWdfIncludePath=$WdfIncludePath" }
        if ($WilIncludePath) { $arguments += "/p:NvTestWilIncludePath=$WilIncludePath" }
        Write-Host "Building $project [$Configuration|$Platform, $profile]"
        & $msbuild @arguments
        if ($LASTEXITCODE -ne 0) { throw "$project ($profile) build failed: $LASTEXITCODE" }
        if ($BuildOnly) { continue }
        $output = Join-Path $PSScriptRoot "..\out\$Platform\$Configuration\tests\$profile"
        $executable = Join-Path $output "$project.exe"
        $result = Join-Path $output "results\$project.xml"
        New-Item -ItemType Directory -Path (Split-Path $result -Parent) -Force | Out-Null
        if (Test-Path -LiteralPath $result) { Remove-Item -LiteralPath $result }
        & $executable "--gtest_filter=$Filter" "--gtest_output=xml:$result"
        if ($LASTEXITCODE -ne 0) { throw "$project ($profile) tests failed: $LASTEXITCODE" }
        if (-not (Test-Path -LiteralPath $result)) { throw "GoogleTest did not create its report: $result" }
        [xml]$report = Get-Content -LiteralPath $result -Raw
        $testCount += [int]$report.testsuites.tests
        ++$runs
    }
}
if ($BuildOnly) {
    Write-Host 'Host-test builds completed. No test executables or hardware operations were run.'
} else {
    if ($testCount -eq 0) { throw "No tests matched filter '$Filter' in suite '$Suite'." }
    Write-Host "Passed $testCount GoogleTest cases across $runs executable/profile runs."
}
