[CmdletBinding()]
param([int]$ProcessId = 0)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ControlSmokeNative {
    [DllImport("user32.dll", SetLastError=true)]
    public static extern IntPtr SendMessageTimeout(IntPtr hwnd, uint msg, UIntPtr wp, IntPtr lp, uint flags, uint timeout, out UIntPtr result);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hwnd, IntPtr dc, uint flags);
    public struct RECT { public int Left, Top, Right, Bottom; }
}
'@
$startedHere = $ProcessId -eq 0
$process = if ($startedHere) {
    Start-Process (Join-Path $PSScriptRoot 'artifacts\publish\win-x64\NvIddCxControl.exe') -PassThru
} else { Get-Process -Id $ProcessId }
$log = New-Object 'System.Collections.Generic.List[string]'
try {
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 300
        $process.Refresh()
        if ($process.HasExited) { throw "App exited with code $($process.ExitCode)." }
        if ($process.MainWindowHandle -ne [IntPtr]::Zero) { break }
    }
    if ($process.MainWindowHandle -eq [IntPtr]::Zero) { throw 'No top-level window appeared.' }
    Start-Sleep -Seconds 2
    $hwnd = $process.MainWindowHandle
    $result = [UIntPtr]::Zero
    if ([ControlSmokeNative]::SendMessageTimeout($hwnd, 0, [UIntPtr]::Zero, [IntPtr]::Zero, 2, 3000, [ref]$result) -eq [IntPtr]::Zero) {
        throw 'Window did not respond within three seconds.'
    }
    $element = [System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
    $log.Add("Window: $($element.Current.Name); PID=$($process.Id); HWND=$hwnd; WM_NULL responsive.")
    function Find-Named($name) {
        $condition = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::NameProperty, $name)
        return $element.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
    }
    foreach ($tab in @('Demo studio', 'Advanced', 'Swapchain', 'Demo studio', 'Advanced', 'Swapchain')) {
        $button = Find-Named $tab
        if (!$button) { throw "Missing tab button: $tab" }
        $invoke = $button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
        $invoke.Invoke()
        Start-Sleep -Milliseconds 500
        $expectedControl = switch ($tab) { 'Demo studio' { 'MPO demonstration' } 'Advanced' { 'Frame pipeline' } default { 'Swapchain API' } }
        if (!(Find-Named $expectedControl)) {
            $element.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                [System.Windows.Automation.Condition]::TrueCondition) |
                ForEach-Object { "$($_.Current.ControlType.ProgrammaticName): $($_.Current.Name)" } |
                Set-Content (Join-Path $PSScriptRoot 'artifacts\smoke-ui-tree.log')
            $log | ForEach-Object { Write-Host $_ }
            throw "Tab did not expose expected controls: $tab"
        }
        $log.Add("Invoked tab: $tab")
    }
    Start-Sleep -Seconds 3
    $tree = $element.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition)
    $connection = @($tree | Where-Object { $_.Current.Name -match 'Not connected|Pipe connected' })
    if ($connection.Count -eq 0) {
        $tree | ForEach-Object { "$($_.Current.ControlType.ProgrammaticName): $($_.Current.Name)" } |
            Set-Content (Join-Path $PSScriptRoot 'artifacts\smoke-ui-tree.log')
        throw 'Connection state is not exposed in the accessibility tree.'
    }
    $apply = Find-Named 'Apply configuration'
    if (!$apply) { throw 'Apply button is missing.' }
    if ($apply.Current.IsEnabled) { throw 'Apply is enabled before any draft edits.' }
    $log.Add("Startup Apply disabled; telemetry state visible. No Apply/UAC/registry writes performed.")
    $rect = New-Object ControlSmokeNative+RECT
    if ([ControlSmokeNative]::GetWindowRect($hwnd, [ref]$rect)) {
        $bitmap = New-Object System.Drawing.Bitmap(($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top))
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $dc = $graphics.GetHdc()
        try { $captured = [ControlSmokeNative]::PrintWindow($hwnd, $dc, 2) }
        finally { $graphics.ReleaseHdc($dc); $graphics.Dispose() }
        if ($captured) { $bitmap.Save((Join-Path $PSScriptRoot 'artifacts\local-ui.png')) }
        $bitmap.Dispose()
    }
    $process.Refresh()
    if (!$process.Responding) { throw 'App stopped responding after tab interaction.' }
    $log.Add('PASS: published NativeAOT window remained responsive after accessible tab interaction and status polling.')
    $log | Set-Content (Join-Path $PSScriptRoot 'artifacts\smoke.txt')
    $log | ForEach-Object { Write-Host $_ }
} finally {
    if ($startedHere -and !$process.HasExited) {
        $process.CloseMainWindow() | Out-Null
        if (!$process.WaitForExit(5000)) { Stop-Process -Id $process.Id }
    }
    $process.Dispose()
}
