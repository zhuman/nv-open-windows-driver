[CmdletBinding()]
param([string]$OutputDirectory, [string]$VcVarsPath)
$ErrorActionPreference = 'Stop'
if (-not $VcVarsPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'Visual C++ build tools were not found' }
    $VcVarsPath = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
}
if (-not (Test-Path -LiteralPath $VcVarsPath)) { throw 'vcvars64.bat was not found' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $PSScriptRoot '..\out\x64\monitor-ddc' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$output = (Resolve-Path -LiteralPath $OutputDirectory).Path
$source = Join-Path $PSScriptRoot 'monitor-ddc.cpp'
& $env:ComSpec /c "call `"$VcVarsPath`" >nul && cd /d `"$output`" && cl /nologo /std:c++20 /EHsc /W4 /WX /MT /Fe:monitor-ddc.exe `"$source`" /link Dxva2.lib User32.lib"
if ($LASTEXITCODE -ne 0) { throw "Monitor DDC tool build failed: $LASTEXITCODE" }
Get-Item -LiteralPath (Join-Path $output 'monitor-ddc.exe')
