[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64',

    [ValidateSet('Build', 'Rebuild')]
    [string]$Target = 'Rebuild',

    [string]$MSBuildPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = $PSScriptRoot
$projectPath = Join-Path $repoRoot 'windows-open\iddcx\nv-iddcx.vcxproj'

if (-not (Test-Path -LiteralPath $projectPath -PathType Leaf)) {
    throw "IddCx project not found: $projectPath"
}

function Find-MSBuild {
    if ($MSBuildPath) {
        $resolvedPath = (Resolve-Path -LiteralPath $MSBuildPath).Path
        if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf)) {
            throw "MSBuild executable not found: $MSBuildPath"
        }
        return $resolvedPath
    }

    $vswherePath = Join-Path ${env:ProgramFiles(x86)} `
        'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswherePath -PathType Leaf) {
        $discoveredPath = & $vswherePath -latest -products * `
            -requires Microsoft.Component.MSBuild `
            -find 'MSBuild\**\Bin\amd64\MSBuild.exe' |
            Select-Object -First 1
        if ($discoveredPath -and (Test-Path -LiteralPath $discoveredPath)) {
            return $discoveredPath
        }
    }

    $installRoots = @(
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio'),
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio')
    ) | Where-Object { Test-Path -LiteralPath $_ -PathType Container }

    $discoveredPath = Get-ChildItem -Path $installRoots `
        -Filter MSBuild.exe -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\Bin\\amd64\\MSBuild\.exe$' } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1 -ExpandProperty FullName
    if ($discoveredPath) {
        return $discoveredPath
    }

    $command = Get-Command MSBuild.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    throw 'MSBuild.exe was not found. Install Visual Studio Build Tools or pass -MSBuildPath.'
}

$msbuild = Find-MSBuild
$arguments = @(
    $projectPath,
    "/t:$Target",
    '/restore',
    '/m',
    "/p:Configuration=$Configuration",
    "/p:Platform=$Platform",
    '/p:NvRmUserLinkProbe=true',
    '/v:minimal'
)

Write-Host "Repository: $repoRoot"
Write-Host "MSBuild:   $msbuild"
Write-Host "Project:   $projectPath"
Write-Host "Build:     $Configuration|$Platform ($Target)"

& $msbuild @arguments
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE."
}

$outputDirectories = @(
    (Join-Path $repoRoot "windows-open\iddcx\$Platform\$Configuration\nv-iddcx"),
    (Join-Path $repoRoot "windows-open\out\$Platform\$Configuration\nv-iddcx")
)
$driver = $outputDirectories |
    ForEach-Object { Join-Path $_ 'nv-iddcx.dll' } |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
    Sort-Object { (Get-Item -LiteralPath $_).LastWriteTimeUtc } -Descending |
    Select-Object -First 1

if (-not $driver) {
    throw "Build succeeded, but nv-iddcx.dll was not found under the expected output directories."
}

$driverFile = Get-Item -LiteralPath $driver
$driverHash = (Get-FileHash -LiteralPath $driver -Algorithm SHA256).Hash
Write-Host "Driver:    $($driverFile.FullName)"
Write-Host "Timestamp: $($driverFile.LastWriteTime.ToString('o'))"
Write-Host "SHA-256:   $driverHash"