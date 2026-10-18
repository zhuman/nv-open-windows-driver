[CmdletBinding()]
param(
    [string]$VcVarsPath,
    [string]$WdfIncludePath,
    [string]$WilIncludePath,
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [ValidateSet('x64', 'ARM64')][string]$Platform = 'x64',
    [switch]$BuildOnly,
    [string]$Filter = '*'
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot '..\..\tests\run-host-tests.ps1') -Suite Presentation @PSBoundParameters
