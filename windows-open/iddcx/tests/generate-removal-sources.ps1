[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('device', 'scrubber', 'pma')][string]$Fixture,
    [Parameter(Mandatory)][string]$Profile,
    [Parameter(Mandatory)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
# Keep the pre-fix reproductions stable after the removal fix is committed.
$baselineRef = '955ba86ce7fdbeb3d616dd7597daa8779b088f38'
$output = $OutputDirectory
New-Item -ItemType Directory -Path $output -Force | Out-Null

function Get-Function([string]$Source, [string]$Signature) {
    $Source = $Source.Replace("`r`n", "`n")
    $start = $Source.IndexOf($Signature, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Production function not found: $Signature" }
    $opening = $Source.IndexOf('{', $start)
    $depth = 1
    $end = $opening + 1
    while ($depth -ne 0 -and $end -lt $Source.Length) {
        if ($Source[$end] -eq '{') { ++$depth }
        if ($Source[$end] -eq '}') { --$depth }
        ++$end
    }
    if ($depth -ne 0) { throw "Unbalanced production function: $Signature" }
    return $Source.Substring($start, $end - $start)
}

& {
    $device = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\device.cpp') -Raw
    if ($device -notmatch 'pnpCallbacks\.EvtDeviceSurpriseRemoval\s*=\s*SurpriseRemoval;') {
        throw 'Surprise-removal callback is not registered'
    }
    $header = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\device.hpp') -Raw
    $hardware = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\hardware_services.cpp') -Raw
    $rm = Get-Content -LiteralPath (Join-Path $repo 'windows-open\rm\kernel\nvrm_gpu_attach.c') -Raw
    $pipeline = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\swapchain_pipeline.cpp') -Raw
    $adapter = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\adapter.cpp') -Raw
    $modeset = Get-Content -LiteralPath (Join-Path $repo 'windows-open\modeset\nvkms_windows.cpp') -Raw
    $dma = Get-Content -LiteralPath (Join-Path $repo 'src\nvidia-modeset\src\nvkms-dma.c') -Raw
    $server = Get-Content -LiteralPath (Join-Path $repo 'src\nvidia\src\libraries\resserv\src\rs_server.c') -Raw
    $client = Get-Content -LiteralPath (Join-Path $repo 'src\nvidia\src\libraries\resserv\src\rs_client.c') -Raw
    $memory = Get-Content -LiteralPath (Join-Path $repo 'src\nvidia\src\kernel\mem_mgr\mem.c') -Raw
    $virtualMemory = Get-Content -LiteralPath (Join-Path $repo 'src\nvidia\src\kernel\mem_mgr\virtual_mem.c') -Raw
    $gpuVaSpace = Get-Content -LiteralPath (Join-Path $repo 'src\nvidia\src\kernel\mem_mgr\gpu_vaspace.c') -Raw
    $dmaAllocator = Get-Content -LiteralPath (Join-Path $repo 'src\nvidia\src\kernel\gpu\mem_mgr\arch\maxwell\virt_mem_allocator_gm107.c') -Raw
    $functions = @(
        'NTSTATUS DeviceResetInterface::ResetDevice(',
        'NTSTATUS DeviceResetInterface::ResetDeviceInPlace(',
        'void ConfigSpace::Reset(', 'NTSTATUS ConfigSpace::Read(', 'NTSTATUS ConfigSpace::Write(',
        'bool ConfigSpace::IsAvailable(', 'UCHAR RegisterAperture::ReadRegister8(',
        'USHORT RegisterAperture::ReadRegister16(', 'ULONG RegisterAperture::ReadRegister32(',
        'void RegisterAperture::ReadRegisterBuffer32(', 'void RegisterAperture::WriteRegister8(',
        'void RegisterAperture::WriteRegister16(', 'void RegisterAperture::WriteRegister32(',
        'NTSTATUS RegisterAperture::Read32(', 'NTSTATUS Interrupt::Enable(',
        'NTSTATUS Interrupt::Disable(', 'BOOLEAN Interrupt::Isr(', 'void Interrupt::Dpc('
    )
    $modes = if ($Fixture -eq 'device') { @($Profile) } else { @() }
    foreach ($mode in $modes) {
        $source = $hardware
        if ($mode -eq 'baseline') {
            $source = (& git -C $repo --no-pager show "${baselineRef}:windows-open/iddcx/hardware_services.cpp") -join "`n"
            if ($LASTEXITCODE -ne 0) { throw 'Cannot read the committed baseline' }
        }
        $production = Get-Function $header 'inline bool IsHardwareRemoved('
        foreach ($signature in $functions) { $production += "`n" + (Get-Function $source $signature) }
        if ($mode -ne 'baseline') {
            $production += "`n" + (Get-Function $rm 'NV_STATUS NvRmMarkGpuLost(')
            $production += "`n" + (Get-Function $pipeline 'HRESULT SwapChainUploadPipeline::DisableDemo(')
            $production += "`n" + (Get-Function $pipeline 'HRESULT SwapChainUploadPipeline::Stop(')
            $production += "`n" + (Get-Function $adapter 'void Adapter::ResetOutputs(')
            $production += "`n" + (Get-Function $device 'NvBool IsNvKmsDeviceRemoved(')
            $production += "`n" + (Get-Function $modeset 'NV_STATUS NvKmsWindowsRegisterGpu(')
            $production += "`n" + (Get-Function $modeset 'void NvKmsWindowsUnregisterGpu(')
            $production += "`n" + (Get-Function $modeset 'NvBool NvKmsWindowsIsGpuRemoved(')
            $production += "`n" + (Get-Function $modeset 'NvBool NvKmsWindowsIsGpuSurpriseRemoved(')
            $production += "`n" + (Get-Function $server 'NvBool serverIsFreeRpcStatusExpected(')
            $production += "`nnamespace ResourceFree {`n" +
                (Get-Function $client "NV_STATUS`nclientFreeResource_IMPL") + "`n" +
                (Get-Function $memory "void`nmemDestruct_IMPL") + "`n}"
            $production += "`nnamespace VaOwnership {`n"
            $production += "`n#if defined(NV_WINDOWS_IDDCX) && defined(NV_RM_VERBOSE_DIAGNOSTICS)`n" +
                (Get-Function $gpuVaSpace "static void`n_gvaspaceTraceAllocation") + "`n" +
                (Get-Function $virtualMemory "static void`n_virtmemTraceLifetime") + "`n#endif`n"
            $production += (Get-Function $gpuVaSpace "NV_STATUS`ngvaspaceFree_IMPL") + "`n" +
                (Get-Function $gpuVaSpace "NV_STATUS`ngvaspaceIncAllocRefCnt_IMPL") + "`n" +
                (Get-Function $virtualMemory "void`nvirtmemDestruct_IMPL") + "`n"
            $unmapSource = $dmaAllocator
            if ($mode -eq 'dma-baseline') {
                $unmapSource = (& git -C $repo --no-pager show "${baselineRef}:src/nvidia/src/kernel/gpu/mem_mgr/arch/maxwell/virt_mem_allocator_gm107.c") -join "`n"
                if ($LASTEXITCODE -ne 0) { throw 'Cannot read the committed DMA unmap baseline' }
            }
            # MSVC cannot infer that SLI p2p implies pCliMapInfo initialized subDevIdSrc.
            $production += "`n#pragma warning(push)`n#pragma warning(disable:4701)`n" +
                (Get-Function $unmapSource "NV_STATUS`ndmaFreeMapping_GM107") +
                "`n#pragma warning(pop)`n}"
            $production += "`n" + (Get-Function $device 'void DestroyRmGpuContext(').Replace(
                'void DestroyRmGpuContext(', 'void DestroyRmGpuContextForTest(')
            $production += "`n" + (Get-Function $dma 'static inline void EvoWriteNotifier(')
            $production += "`n" + (Get-Function $dma 'static NvBool EvoCheckNotifier(')
            foreach ($signature in @('void SurpriseRemoval(', 'NTSTATUS ReleaseHardware(', 'NTSTATUS EnterD0(')) {
                $production += "`n" + (Get-Function $device $signature)
            }
        }
        $production | Set-Content -LiteralPath (Join-Path $output 'device_removal_production.inc') -Encoding ascii
    }
    $scrubberModes = if ($Fixture -eq 'scrubber') { @($Profile) } else { @() }
    foreach ($mode in $scrubberModes) {
        $sources = @{}
        foreach ($name in @('ce_utils', 'channel_utils', 'mem_scrub', 'intr')) {
            $relative = if ($name -eq 'intr') {
                'src/nvidia/src/kernel/gpu/intr/intr.c'
            } else {
                "src/nvidia/src/kernel/gpu/mem_mgr/$name.c"
            }
            if ($mode -eq 'baseline') {
                $sources[$name] = (& git -C $repo --no-pager show "${baselineRef}:$relative") -join "`n"
                if ($LASTEXITCODE -ne 0) { throw "Cannot read committed scrubber source: $name" }
            } else {
                $sources[$name] = Get-Content -LiteralPath (Join-Path $repo $relative.Replace('/', '\')) -Raw
            }
        }
        $production = ''
        if ($mode -ne 'baseline') {
            $production += (Get-Function $sources.channel_utils "static NvBool`n_channelGpuIsSurpriseRemoved") + "`n"
            $production += (Get-Function $sources.intr "static NvBool`n_intrGpuIsSurpriseRemoved") + "`n"
        }
        foreach ($signature in @("void`nintrServiceStallList_IMPL", "void`nintrServiceStallSingle_IMPL")) {
            $production += (Get-Function $sources.intr $signature) + "`n"
        }
        foreach ($signature in @("void`nchannelServiceScrubberInterrupts",
            "NV_STATUS`nchannelWaitForFinishPayload", "NV_STATUS`nchannelWaitForFreeEntry")) {
            $production += (Get-Function $sources.channel_utils $signature) + "`n"
        }
        foreach ($signature in @("NvU64`nceutilsUpdateProgress_IMPL",
            'static void _ceutilsProcessCompletionCallbacksThrough',
            "static void _ceutilsProcessCompletionCallbacks`n", "void`nceutilsDestruct_IMPL")) {
            $production += (Get-Function $sources.ce_utils $signature) + "`n"
        }
        $production += (Get-Function $sources.mem_scrub "void`nscrubberDestruct") + "`n"
        $production | Set-Content -LiteralPath (Join-Path $output 'scrubber_removal_production.inc') -Encoding ascii
    }
    $pmaModes = if ($Fixture -eq 'pma') { @($Profile) } else { @() }
    foreach ($mode in $pmaModes) {
        $sources = @{}
        foreach ($name in @('mem_scrub', 'phys_mem_allocator', 'kernel_graphics')) {
            $relative = if ($name -eq 'kernel_graphics') {
                'src/nvidia/src/kernel/gpu/gr/kernel_graphics.c'
            } else {
                'src/nvidia/src/kernel/gpu/mem_mgr/' +
                    $(if ($name -eq 'phys_mem_allocator') { 'phys_mem_allocator/' } else { '' }) + "$name.c"
            }
            if ($mode -eq 'baseline') {
                $sources[$name] = (& git -C $repo --no-pager show "${baselineRef}:$relative") -join "`n"
                if ($LASTEXITCODE -ne 0) { throw "Cannot read committed PMA/scrub source: $name" }
            } else {
                $sources[$name] = Get-Content -LiteralPath (Join-Path $repo $relative.Replace('/', '\')) -Raw
            }
        }
        $production = ''
        if ($mode -ne 'baseline') {
            $production += (Get-Function $sources.mem_scrub "static NvBool`n_scrubGpuIsSurpriseRemoved") + "`n"
        }
        foreach ($signature in @("static NvU64`n_scrubGetFreeEntries", "static void`n_scrubCopyListItems",
            "static NV_STATUS`n_scrubWaitAndSave", "static void`n_scrubAddWorkToList",
            "static NV_STATUS`n_scrubMemory", "static NvU64`n_scrubCheckAndSubmit",
            "static NV_STATUS`n_scrubCombinePages", "NV_STATUS`nscrubSubmitPages")) {
            if ($signature -eq "static NV_STATUS`n_scrubMemory") {
                # Native CE/SEC2 chooses the payload; the legacy payload parameter is unused.
                $production += "#pragma warning(push)`n#pragma warning(disable:4100)`n" +
                    (Get-Function $sources.mem_scrub $signature) + "`n#pragma warning(pop)`n"
            } else {
                $production += (Get-Function $sources.mem_scrub $signature) + "`n"
            }
        }
        if ($mode -ne 'baseline') {
            foreach ($signature in @("static NvU64`n_searchScrubList", "static NV_STATUS`n_waitForPayload")) {
                $production += (Get-Function $sources.mem_scrub $signature) + "`n"
            }
        }
        $production += (Get-Function $sources.phys_mem_allocator "NV_STATUS`npmaQueryConfigs") + "`n"
        $production += (Get-Function $sources.phys_mem_allocator "void`npmaFreePages") + "`n"
        $production += "`nnamespace GraphicsCleanup {`n" +
            (Get-Function $sources.kernel_graphics 'void kgraphicsFreeGlobalCtxBuffers_IMPL') + "`n}`n"
        $production | Set-Content -LiteralPath (Join-Path $output 'pma_scrub_removal_production.inc') -Encoding ascii
    }
}
