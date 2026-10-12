#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [switch]$ExclusiveNvidiaWindowAuthorized
)

$ErrorActionPreference = 'Stop'
if (!$ExclusiveNvidiaWindowAuthorized) {
    throw 'Refusing writes: explicitly authorize an exclusive NVIDIA test window first.'
}
if ($PSVersionTable.PSEdition -ne 'Desktop') { throw 'Run this script with Windows PowerShell (powershell.exe), not pwsh.' }
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'This helper smoke requires an already elevated token. It does NOT test interactive UAC consent.'
}
$sid = $identity.User
$exePath = (Resolve-Path -LiteralPath $Exe).Path
if ([IO.Path]::GetFileName($exePath) -ne 'NvIddCxControl.exe') { throw 'Expected NvIddCxControl.exe in its complete published folder.' }
$clock = [Diagnostics.Stopwatch]::StartNew()
$requestPaths = New-Object 'System.Collections.Generic.List[string]'
$restorationPending = $false
$schema = [ordered]@{
    SwapChainApi = @(0, 11, 12); D3D12Mode = @(0, 1, 2, 3)
    D3D11ReadbackMode = @(0, 1, 2); SurfaceCachePolicy = @(0, 1, 2)
    DirtyRects = @(0, 1); D3D12Debug = @(0, 1)
    RealtimeGpuPriority = @(0, 1); MmcssDisplayPostProcessing = @(0, 1)
    VrrPreference = @(0, 1); FlipMode = @(0, 1); FramePipeline = @(0, 1)
    ScanoutBufferCount = @(2, 3, 4); MpoDemoMode = @(0, 1, 2, 3, 4, 5, 6)
    MpoDemoPlaneCount = @(1, 2, 3, 4, 5, 6, 7); MpoDemoConnectorIndex = @(0..15)
    MpoDemoCadenceHz = @(15, 30, 60); MpoDemoSpeedPercent = @(25..200)
}

function New-PrivateDirectory($path) {
    for ($ancestor = New-Object IO.DirectoryInfo($path); $ancestor; $ancestor = $ancestor.Parent) {
        if ($ancestor.Exists -and ($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Request path traverses a reparse point.'
        }
    }
    if (!(Test-Path -LiteralPath $path)) {
        $acl = New-Object Security.AccessControl.DirectorySecurity
        $acl.SetOwner($sid)
        $acl.SetAccessRuleProtection($true, $false)
        foreach ($owner in @($sid,
            (New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')),
            (New-Object Security.Principal.SecurityIdentifier('S-1-5-18')))) {
            $rule = New-Object Security.AccessControl.FileSystemAccessRule(
                $owner, [Security.AccessControl.FileSystemRights]::FullControl,
                ([Security.AccessControl.InheritanceFlags]::ContainerInherit -bor [Security.AccessControl.InheritanceFlags]::ObjectInherit),
                [Security.AccessControl.PropagationFlags]::None, [Security.AccessControl.AccessControlType]::Allow)
            $acl.AddAccessRule($rule)
        }
        [IO.Directory]::CreateDirectory($path, $acl) | Out-Null
    }
    $existing = (New-Object IO.DirectoryInfo($path)).GetAccessControl()
    if (!$sid.Equals($existing.GetOwner([Security.Principal.SecurityIdentifier]))) {
        throw 'Existing request directory is not owned by this user; it will not be changed.'
    }
    foreach ($rule in $existing.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])) {
        if ($rule.AccessControlType -ne [Security.AccessControl.AccessControlType]::Allow) { continue }
        $owner = $rule.IdentityReference
        if ($sid.Equals($owner) -or $owner.Value -in @('S-1-5-32-544', 'S-1-5-18')) { continue }
        $write = [Security.AccessControl.FileSystemRights]::Write -bor [Security.AccessControl.FileSystemRights]::Delete -bor
            [Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles -bor
            [Security.AccessControl.FileSystemRights]::ChangePermissions -bor [Security.AccessControl.FileSystemRights]::TakeOwnership
        if ($rule.FileSystemRights -band $write) { throw 'Existing request directory allows untrusted writes; it will not be changed.' }
    }
}

function Hash-Bytes([byte[]]$bytes) {
    $hash = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hash.ComputeHash($bytes)).Replace('-', '') }
    finally { $hash.Dispose() }
}

function Read-Datum($key, $name) {
    $absent = [ordered]@{ present = $false; kind = 'Absent'; dword = $null; raw = $null }
    if (!$key) { return $absent }
    $value = $key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    if ($null -eq $value) { return $absent }
    $kind = $key.GetValueKind($name).ToString()
    if ($kind -eq 'DWord' -and $value -is [int]) {
        return [ordered]@{ present = $true; kind = 'DWord'; dword = [uint32]([int64]$value -band 0xffffffffL); raw = $null }
    }
    $raw = if ($value -is [byte[]]) { Hash-Bytes $value }
    elseif ($value -is [string[]]) { Hash-Bytes ([Text.Encoding]::UTF8.GetBytes([string]::Join([string][char]0, $value))) }
    elseif ($value -is [string]) { Hash-Bytes ([Text.Encoding]::UTF8.GetBytes($value)) }
    elseif ($value -is [long]) { $value.ToString([Globalization.CultureInfo]::InvariantCulture) }
    else { 'Unsupported' }
    return [ordered]@{ present = $true; kind = $kind; dword = $null; raw = $raw }
}

function Read-Snapshot {
    $hive = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
    $key = $null
    $legacy = $null
    try {
        $key = $hive.OpenSubKey('SOFTWARE\Microsoft\Windows NT\CurrentVersion\WUDF\Services\nv-iddcx\Parameters', $false)
        if (!$key) { throw 'Existing framework Parameters key is absent; no key will be created.' }
        $legacy = $hive.OpenSubKey('SOFTWARE\NVIDIA Corporation\nv-iddcx', $false)
        for ($attempt = 0; $attempt -lt 4; $attempt++) {
            $revision = Read-Datum $key 'ConfigRevision'
            $fallback = Read-Datum $legacy 'ReadbackMode'
            $values = [ordered]@{}
            foreach ($name in $schema.Keys) { $values[$name] = Read-Datum $key $name }
            $revisionAfter = Read-Datum $key 'ConfigRevision'
            $fallbackAfter = Read-Datum $legacy 'ReadbackMode'
            if (($revision | ConvertTo-Json -Compress) -eq ($revisionAfter | ConvertTo-Json -Compress) -and
                ($fallback | ConvertTo-Json -Compress) -eq ($fallbackAfter | ConvertTo-Json -Compress)) {
                return [ordered]@{ keyExists = $true; revision = $revision; legacyReadback = $fallback; values = $values }
            }
        }
        throw 'Revision/fallback changed during bounded snapshot reads; aborting.'
    } finally {
        if ($legacy) { $legacy.Dispose() }
        if ($key) { $key.Dispose() }
        $hive.Dispose()
    }
}

function Desired-Values($snapshot) {
    $desired = [ordered]@{}
    foreach ($name in $schema.Keys) {
        $datum = $snapshot.values[$name]
        if ($datum.present -and ($datum.kind -ne 'DWord' -or $schema[$name] -notcontains $datum.dword)) {
            throw "Invalid current $name; no default will be substituted."
        }
        $desired[$name] = if ($datum.present) { $datum.dword } else { $null }
    }
    if (!$snapshot.values.D3D11ReadbackMode.present -and $snapshot.legacyReadback.present -and
        ($snapshot.legacyReadback.kind -ne 'DWord' -or $schema.D3D11ReadbackMode -notcontains $snapshot.legacyReadback.dword)) {
        throw 'Invalid effective legacy ReadbackMode; aborting.'
    }
    if ($desired.MpoDemoMode -gt 0 -and $null -eq $desired.MpoDemoConnectorIndex) { throw 'Enabled demo lacks explicit connector index.' }
    return $desired
}

function Invoke-Group($expected, $desired) {
    $request = [ordered]@{
        protocol = 1; operation = 'apply'; createdUtc = [DateTimeOffset]::UtcNow.ToString('o')
        expected = $expected; desired = $desired
    }
    $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes(($request | ConvertTo-Json -Depth 12 -Compress))
    if ($bytes.Length -gt 65536) { throw 'Request exceeds 64 KiB.' }
    $path = Join-Path $requestDirectory ([Guid]::NewGuid().ToString('N') + '.json')
    $requestPaths.Add($path)
    $file = New-Object IO.FileStream($path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $file.Write($bytes, 0, $bytes.Length) } finally { $file.Dispose() }
    # Elevated tokens may otherwise create an Administrators-owned file.
    $info = New-Object IO.FileInfo($path)
    $acl = $info.GetAccessControl()
    $acl.SetOwner($sid)
    $info.SetAccessControl($acl)
    $remaining = 30000 - [int]$clock.ElapsedMilliseconds
    if ($remaining -le 0) { throw 'Thirty-second smoke deadline expired before helper launch.' }
    $process = Start-Process -FilePath $exePath -ArgumentList @('--apply-request', ('"' + $path + '"')) -PassThru
    try {
        if (!$process.WaitForExit([Math]::Min(10000, $remaining))) {
            Stop-Process -Id $process.Id
            throw 'Helper timed out; an odd revision may remain. Review explicitly; no automatic recovery is performed.'
        }
        if ($process.ExitCode -ne 0) { throw "Helper exit $($process.ExitCode); no success/rollback is assumed." }
    } finally { $process.Dispose() }
}

function Assert-Group($actual, $desired, [uint32]$revision, $legacy) {
    if (!$actual.revision.present -or $actual.revision.kind -ne 'DWord' -or $actual.revision.dword -ne $revision -or ($revision -band 1)) {
        throw 'Committed even revision was not verified.'
    }
    foreach ($name in $schema.Keys) {
        $datum = $actual.values[$name]
        if ($null -eq $desired[$name]) {
            if ($datum.present) { throw "Expected absent $name was not restored." }
        } elseif (!$datum.present -or $datum.kind -ne 'DWord' -or $datum.dword -ne $desired[$name]) {
            throw "Expected DWORD $name was not verified."
        }
    }
    if (($actual.legacyReadback | ConvertTo-Json -Compress) -ne ($legacy | ConvertTo-Json -Compress)) {
        throw 'Legacy fallback changed; exclusive ownership or expected snapshot no longer holds.'
    }
}

try {
    $before = Read-Snapshot
    $original = Desired-Values $before
    if ($before.revision.present -and $before.revision.kind -ne 'DWord') { throw 'Invalid ConfigRevision.' }
    $revision = if ($before.revision.present) { [uint32]$before.revision.dword } else { [uint32]0 }
    if (($revision -band 1) -or $revision -gt 4294967290L) { throw 'Incomplete/exhausted revision; explicit review is required.' }
    $parent = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'NvIddCxControl'
    $requestDirectory = Join-Path $parent 'Requests'
    New-PrivateDirectory $parent
    New-PrivateDirectory $requestDirectory
    $test = [ordered]@{}
    foreach ($name in $schema.Keys) { $test[$name] = $original[$name] }
    $speed = if ($null -eq $original.MpoDemoSpeedPercent) { 100 } else { [int]$original.MpoDemoSpeedPercent }
    $test.MpoDemoSpeedPercent = if ($speed -eq 200) { 199 } else { $speed + 1 }
    Write-Host 'Already elevated helper test: no runas/UAC consent is exercised. No API/demo/connector/service change.'
    Invoke-Group $before $test
    $restorationPending = $true
    $after = Read-Snapshot
    Assert-Group $after $test ([uint32]($revision + 2)) $before.legacyReadback
    Invoke-Group $after $original
    Assert-Group (Read-Snapshot) $original ([uint32]($revision + 4)) $before.legacyReadback
    $restorationPending = $false
    Write-Host "PASS helper registry round-trip in $($clock.ElapsedMilliseconds) ms: speed restored including absent-vs-present shape; revision advanced $revision -> $($revision + 4)."
    Write-Host 'This proves neither interactive UAC consent nor driver/hardware adoption. Inspect real pipe status separately.'
} catch {
    if ($restorationPending) { Write-Warning 'Restoration was NOT verified. Review current values/revision; no automatic recovery or blind overwrite will be attempted.' }
    throw
} finally {
    foreach ($path in $requestPaths) {
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    }
}
