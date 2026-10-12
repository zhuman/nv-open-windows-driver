[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$folder = Join-Path $PSScriptRoot 'artifacts\publish\win-x64'
$exe = Join-Path $folder 'NvIddCxControl.exe'
$stream = [IO.File]::OpenRead($exe)
$reader = New-Object IO.BinaryReader($stream)
try {
    $stream.Position = 0x3c
    $pe = $reader.ReadUInt32()
    $stream.Position = $pe
    if ($reader.ReadUInt32() -ne 0x4550) { throw 'Invalid PE signature.' }
    if ($reader.ReadUInt16() -ne 0x8664) { throw 'Executable is not x64.' }
    $optionalHeader = $pe + 24
    $stream.Position = $optionalHeader
    if ($reader.ReadUInt16() -ne 0x20b) { throw 'Executable is not PE32+.' }
    $stream.Position = $optionalHeader + 68
    if ($reader.ReadUInt16() -ne 2) { throw 'Executable is not a GUI application.' }
    $stream.Position = $optionalHeader + 112 + 14 * 8
    if ($reader.ReadUInt32() -ne 0 -or $reader.ReadUInt32() -ne 0) {
        throw 'Managed CLR header is present; this is not a native executable.'
    }
} finally { $reader.Dispose(); $stream.Dispose() }
foreach ($asset in @('Microsoft.ui.xaml.dll', 'Microsoft.UI.Xaml.Controls.dll', 'NvIddCxControl.pri')) {
    if (!(Test-Path (Join-Path $folder $asset))) { throw "Required WinUI asset is absent: $asset" }
}
foreach ($managedRuntime in @('coreclr.dll', 'clrjit.dll', 'NvIddCxControl.dll')) {
    if (Test-Path (Join-Path $folder $managedRuntime)) { throw "Unexpected managed runtime/app asset: $managedRuntime" }
}
$files = Get-ChildItem $folder -Recurse -File
$proof = "PASS NativeAOT: x64 native GUI PE; CLR descriptor=0; no CoreCLR/JIT/app DLL; self-contained WinUI DLLs and app PRI present.`r`nFiles: $($files.Count); bytes: $(($files | Measure-Object Length -Sum).Sum)`r`nEXE SHA256: $((Get-FileHash $exe -Algorithm SHA256).Hash)"
$proof | Set-Content (Join-Path $PSScriptRoot 'artifacts\native-aot-proof.txt')
Write-Host $proof
