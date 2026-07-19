[CmdletBinding()]
param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$readmePath = Join-Path $RepositoryRoot 'README.md'
$rowPattern = '^\|\s*(?<name>[^|]+?)\s*\|\s*(?<device>[0-9A-Fa-f]{4})(?:\s+(?<subVendor>[0-9A-Fa-f]{4})\s+(?<subDevice>[0-9A-Fa-f]{4}))?\s*\|$'
$modelBegin = '; BEGIN GENERATED NVIDIA SUPPORTED GPU MODELS'
$modelEnd = '; END GENERATED NVIDIA SUPPORTED GPU MODELS'
$namesBegin = '; BEGIN GENERATED NVIDIA SUPPORTED GPU NAMES'
$namesEnd = '; END GENERATED NVIDIA SUPPORTED GPU NAMES'

$devices = @(foreach ($line in Get-Content -LiteralPath $readmePath)
{
    if ($line -notmatch $rowPattern)
    {
        continue
    }

    $name = $Matches.name.Trim()
    if ($name -match '[^\x20-\x7E]' -or $name -match '["%]')
    {
        throw "Product name cannot be represented safely in an INF string: $name"
    }

    $device = $Matches.device.ToUpperInvariant()
    $subVendor = ([string]$Matches['subVendor']).ToUpperInvariant()
    $subDevice = ([string]$Matches['subDevice']).ToUpperInvariant()
    $suffix = $device
    $hardwareId = "PCI\VEN_10DE&DEV_$device"

    if ($subVendor.Length -ne 0)
    {
        $suffix = "$device.$subVendor.$subDevice"
        $hardwareId += "&SUBSYS_$subDevice$subVendor"
    }

    [pscustomobject]@{
        Name = $name
        Token = "NVIDIA.DeviceDesc.$suffix"
        HardwareId = $hardwareId
    }
})

if ($devices.Count -eq 0)
{
    throw "No supported GPU rows were found in $readmePath"
}

$duplicateHardwareIds = @(
    $devices | Group-Object HardwareId | Where-Object Count -gt 1
)
if ($duplicateHardwareIds.Count -ne 0)
{
    throw "The README contains duplicate Windows hardware IDs: $($duplicateHardwareIds.Name -join ', ')"
}

function Update-GeneratedBlock
{
    param(
        [string]$Content,
        [string]$BeginMarker,
        [string]$EndMarker,
        [string[]]$Lines,
        [int]$ExpectedCount,
        [string]$NewLine
    )

    $pattern = '(?ms)^' + [regex]::Escape($BeginMarker) + '\r?\n.*?^' +
        [regex]::Escape($EndMarker) + '\r?$'
    $matches = [regex]::Matches($Content, $pattern)
    if ($matches.Count -ne $ExpectedCount)
    {
        throw "Expected $ExpectedCount '$BeginMarker' blocks, found $($matches.Count)"
    }

    $replacement = $BeginMarker + $NewLine + ($Lines -join $NewLine) +
        $NewLine + $EndMarker
    return [regex]::Replace($Content, $pattern, $replacement)
}

$targets = @(
    [pscustomobject]@{
        Path = Join-Path $RepositoryRoot 'windows-open\kmdod\nv-kmdod.inf'
        InstallSection = 'NvKmdod_Install'
    },
    [pscustomobject]@{
        Path = Join-Path $RepositoryRoot 'windows-open\iddcx\nv-iddcx.inf'
        InstallSection = 'NvIddCx_Install'
    }
)

$encoding = [System.Text.UTF8Encoding]::new($false)
foreach ($target in $targets)
{
    $content = [System.IO.File]::ReadAllText($target.Path)
    $newLine = if ($content.Contains("`r`n")) { "`r`n" } else { "`n" }
    $modelLines = @($devices | ForEach-Object {
        "%$($_.Token)%=$($target.InstallSection),$($_.HardwareId)"
    })
    $nameLines = @($devices | ForEach-Object {
        "$($_.Token)=`"$($_.Name)`""
    })

    $content = Update-GeneratedBlock $content $modelBegin $modelEnd `
        $modelLines 2 $newLine
    $content = Update-GeneratedBlock $content $namesBegin $namesEnd `
        $nameLines 1 $newLine
    [System.IO.File]::WriteAllText($target.Path, $content, $encoding)
}

Write-Host "Generated $($devices.Count) NVIDIA GPU models in each INF."