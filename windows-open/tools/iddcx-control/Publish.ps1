[CmdletBinding()]
param([switch]$Smoke)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Push-Location $root
try {
    $sdk = & dotnet --version
    if ($LASTEXITCODE -ne 0 -or $sdk -notmatch '^10\.') { throw 'Install the .NET 10 SDK.' }
    & dotnet restore 'Tests\NvIddCx.Control.Tests.csproj' --locked-mode --verbosity quiet
    if ($LASTEXITCODE -ne 0) { throw 'Locked test dependency restore failed.' }
    & dotnet run --project 'Tests\NvIddCx.Control.Tests.csproj' -c Release --no-restore
    if ($LASTEXITCODE -ne 0) { throw 'Production-model tests failed.' }
    & dotnet restore 'App\NvIddCx.Control.csproj' -p:Platform=x64 --locked-mode --verbosity quiet
    if ($LASTEXITCODE -ne 0) { throw 'Locked dependency restore failed.' }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path $vswhere)) { throw 'Visual Studio C++ tools / Windows SDK are required.' }
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual Studio with C++ x64 tools was not found.' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
    $hostArchitecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
    $arch = if ($hostArchitecture -eq 'Arm64') { 'arm64_x64' } else { 'x64' }
    New-Item -ItemType Directory -Path 'artifacts' -Force | Out-Null
    $publishFolder = Join-Path $root 'artifacts\publish\win-x64'
    if (Test-Path -LiteralPath $publishFolder) {
        Remove-Item -LiteralPath $publishFolder -Recurse -Force
    }
    $command = 'call "{0}" {1} >nul && dotnet publish App\NvIddCx.Control.csproj -c Release -r win-x64 -p:Platform=x64 -p:PublishAot=true --no-restore -o artifacts\publish\win-x64 --verbosity minimal' -f $vcvars, $arch
    & $env:ComSpec /d /c $command 2>&1 | Tee-Object -FilePath 'artifacts\publish.log'
    if ($LASTEXITCODE -ne 0) { throw 'NativeAOT publish failed; see artifacts\publish.log.' }
    Copy-Item -LiteralPath 'README.md', 'Test-ApplyHelper.ps1' -Destination 'artifacts\publish\win-x64' -Force
    & '.\Verify-NativeAot.ps1'
    if ($Smoke) {
        & "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File '.\Smoke.ps1'
        if ($LASTEXITCODE -ne 0) { throw 'Local UI smoke failed.' }
    }
    Write-Host "Published folder: $root\artifacts\publish\win-x64"
} finally { Pop-Location }
