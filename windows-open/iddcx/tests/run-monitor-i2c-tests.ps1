[CmdletBinding()]
param([string]$VcVarsPath)
$ErrorActionPreference = 'Stop'
if (-not $VcVarsPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'Visual C++ build tools were not found' }
    $VcVarsPath = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
}
if (-not (Test-Path -LiteralPath $VcVarsPath)) { throw 'vcvars64.bat was not found' }
$clang = Join-Path (Get-Item -LiteralPath $VcVarsPath).Directory.Parent.Parent.FullName 'Tools\Llvm\x64\bin\clang-cl.exe'
if (-not (Test-Path -LiteralPath $clang)) { throw 'The NVKMS Clang-CL toolchain was not found' }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$include = Join-Path $repo 'src\common\sdk\nvidia\inc'
$source = Join-Path $PSScriptRoot 'monitor_i2c_test.cpp'
$output = Join-Path ([IO.Path]::GetTempPath()) ("nv-monitor-i2c-tests-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output | Out-Null
try {
    & $env:ComSpec /c "call `"$VcVarsPath`" >nul && cd /d `"$output`" && `"$clang`" /nologo /std:c++20 /EHsc /W4 /WX /MT /external:W0 /external:I`"$include`" /Fe:monitor_i2c_test.exe `"$source`" && monitor_i2c_test.exe"
    if ($LASTEXITCODE -ne 0) { throw "Monitor I2C tests failed: $LASTEXITCODE" }
} finally {
    foreach ($name in @('monitor_i2c_test.obj','monitor_i2c_test.exe')) {
        $path = Join-Path $output $name
        if (Test-Path -LiteralPath $path) {
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
