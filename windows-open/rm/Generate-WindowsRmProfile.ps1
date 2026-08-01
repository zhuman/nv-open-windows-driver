[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

$architectureSources = @(
    'ada\kern_gpu_ad102.c'
    'ampere\kern_gpu_ga100.c'
    'blackwell\kern_gpu_gb100.c'
    'blackwell\kern_gpu_gb10b.c'
    'blackwell\kern_gpu_gb202.c'
    'blackwell\kern_gpu_gb20b.c'
    'hopper\kern_gpu_gh100.c'
    'maxwell\kern_gpu_gm107.c'
    'pascal\kern_gpu_gp100.c'
    'rubin\kern_gpu_gr100.c'
    'rubin\kern_gpu_gr102.c'
    't23x\kern_gpu_t234d.c'
    'turing\kern_gpu_tu102.c'
)

function Write-GeneratedFile
{
    param(
        [string]$Path,
        [string]$Content
    )

    $directory = Split-Path -Parent $Path
    [System.IO.Directory]::CreateDirectory($directory) | Out-Null

    if ((Test-Path -LiteralPath $Path) -and
        [System.IO.File]::ReadAllText($Path) -ceq $Content)
    {
        return
    }

    [System.IO.File]::WriteAllText($Path, $Content, $utf8NoBom)
}

$architectureRoot = Join-Path $repositoryRoot 'src\nvidia\src\kernel\gpu\arch'
$filteredArchitectureRoot = Join-Path $OutputRoot 'gpu\arch'
$removedArchitectureEntries = 0

foreach ($relativePath in $architectureSources)
{
    $sourcePath = Join-Path $architectureRoot $relativePath
    $content = [System.IO.File]::ReadAllText($sourcePath)
    $newLine = if ($content.Contains("`r`n")) { "`r`n" } else { "`n" }
    $lines = $content -split '\r?\n'
    $filteredLines = @()

    foreach ($line in $lines)
    {
        if ($line -match 'GPU_CHILD_PRESENT\((OBJGRIDDISPLAYLESS|OBJDCECLIENTRM),\s*1\),' -or
            $line -match '\{classId\((OBJGRIDDISPLAYLESS|OBJDCECLIENTRM)\),\s*GCO_ALL\},')
        {
            $removedArchitectureEntries++
            continue
        }

        $filteredLines += $line
    }

    Write-GeneratedFile `
        -Path (Join-Path $filteredArchitectureRoot $relativePath) `
        -Content ($filteredLines -join $newLine)
}

if ($removedArchitectureEntries -ne 22)
{
    throw "Expected to remove 22 GRID/DCE architecture entries, removed $removedArchitectureEntries."
}

$resourceRoot = Join-Path $repositoryRoot 'src\nvidia\src\kernel\rmapi'
$resourceListPath = Join-Path $resourceRoot 'resource_list.h'
$resourceListContent = [System.IO.File]::ReadAllText($resourceListPath)
$resourceNewLine = if ($resourceListContent.Contains("`r`n")) { "`r`n" } else { "`n" }
$resourceLines = [System.Collections.Generic.List[string]]::new()
$resourceLines.AddRange([string[]]($resourceListContent -split '\r?\n'))
$resourceMarkerIndexes = @(
    0..($resourceLines.Count - 1) |
        Where-Object { $resourceLines[$_] -match 'NVA083_GRID_DISPLAYLESS' }
)

if ($resourceMarkerIndexes.Count -ne 1)
{
    throw "Expected one GRID displayless resource entry, found $($resourceMarkerIndexes.Count)."
}

$resourceStart = $resourceMarkerIndexes[0]
while ($resourceStart -ge 0 -and $resourceLines[$resourceStart].Trim() -ne 'RS_ENTRY(')
{
    $resourceStart--
}

$resourceEnd = $resourceMarkerIndexes[0]
while ($resourceEnd -lt $resourceLines.Count -and $resourceLines[$resourceEnd].Trim() -ne ')')
{
    $resourceEnd++
}

if ($resourceStart -lt 0 -or $resourceEnd -ge $resourceLines.Count)
{
    throw 'Could not isolate the GRID displayless resource entry.'
}

$resourceLines.RemoveRange($resourceStart, $resourceEnd - $resourceStart + 1)
$filteredResourceRoot = Join-Path $OutputRoot 'rmapi'
Write-GeneratedFile `
    -Path (Join-Path $filteredResourceRoot 'resource_list.h') `
    -Content ($resourceLines -join $resourceNewLine)

$resourceDescPath = Join-Path $resourceRoot 'resource_desc.c'
Write-GeneratedFile `
    -Path (Join-Path $filteredResourceRoot 'resource_desc.c') `
    -Content ([System.IO.File]::ReadAllText($resourceDescPath))

$rpcPath = Join-Path $repositoryRoot 'src\nvidia\src\kernel\vgpu\rpc.c'
$rpcContent = [System.IO.File]::ReadAllText($rpcPath)
$rpcNewLine = if ($rpcContent.Contains("`r`n")) { "`r`n" } else { "`n" }
$rpcLines = [System.Collections.Generic.List[string]]::new()
$rpcLines.AddRange([string[]]($rpcContent -split '\r?\n'))

$gridDeclarationIndexes = @(
    0..($rpcLines.Count - 1) |
        Where-Object { $rpcLines[$_] -match 'OBJGRIDDISPLAYLESS \*pGridDisplayless = GPU_GET_GRIDDISPLAYLESS' }
)
if ($gridDeclarationIndexes.Count -ne 1)
{
    throw "Expected one GRID RPC object declaration, found $($gridDeclarationIndexes.Count)."
}
$rpcLines.RemoveAt($gridDeclarationIndexes[0])

$gridUpdateStartIndexes = @(
    0..($rpcLines.Count - 1) |
        Where-Object { $rpcLines[$_] -match 'status = griddisplaylessUpdateHeadNumInfo' }
)
$commonRpcStepIndexes = @(
    0..($rpcLines.Count - 1) |
        Where-Object { $rpcLines[$_] -match 'NV_RM_RPC_GET_CONSOLIDATED_GR_STATIC_INFO\(pGpu, status\);' }
)
if ($gridUpdateStartIndexes.Count -ne 1)
{
    throw "Expected one GRID RPC update block, found $($gridUpdateStartIndexes.Count)."
}
$gridUpdateStart = $gridUpdateStartIndexes[0]
$gridUpdateEnd = @($commonRpcStepIndexes | Where-Object { $_ -gt $gridUpdateStart } | Select-Object -First 1)
if ($gridUpdateEnd.Count -ne 1)
{
    throw 'Could not find the common RPC step after the GRID update block.'
}
$rpcLines.RemoveRange($gridUpdateStart, $gridUpdateEnd[0] - $gridUpdateStart)

$filteredVgpuRoot = Join-Path $OutputRoot 'vgpu'
Write-GeneratedFile `
    -Path (Join-Path $filteredVgpuRoot 'rpc.c') `
    -Content ($rpcLines -join $rpcNewLine)

$rpcCommonPath = Join-Path $resourceRoot 'rpc_common.c'
$rpcCommonContent = [System.IO.File]::ReadAllText($rpcCommonPath)
$rpcCommonNewLine = if ($rpcCommonContent.Contains("`r`n")) { "`r`n" } else { "`n" }
$rpcCommonLines = [System.Collections.Generic.List[string]]::new()
$rpcCommonLines.AddRange([string[]]($rpcCommonContent -split '\r?\n'))
$dceBranchIndexes = @(
    0..($rpcCommonLines.Count - 1) |
        Where-Object { $rpcCommonLines[$_].Trim() -eq 'else if (IS_DCE_CLIENT(pGpu))' }
)
if ($dceBranchIndexes.Count -ne 1)
{
    throw "Expected one DCE RMAPI setup branch, found $($dceBranchIndexes.Count)."
}

$dceBranchStart = $dceBranchIndexes[0]
$dceBranchEnd = $dceBranchStart
$braceDepth = 0
$foundOpeningBrace = $false
while ($dceBranchEnd -lt $rpcCommonLines.Count)
{
    $braceDepth += ([regex]::Matches($rpcCommonLines[$dceBranchEnd], '\{')).Count
    if ($braceDepth -gt 0)
    {
        $foundOpeningBrace = $true
    }
    $braceDepth -= ([regex]::Matches($rpcCommonLines[$dceBranchEnd], '\}')).Count
    if ($foundOpeningBrace -and $braceDepth -eq 0)
    {
        break
    }
    $dceBranchEnd++
}
if (-not $foundOpeningBrace -or $dceBranchEnd -ge $rpcCommonLines.Count)
{
    throw 'Could not isolate the DCE RMAPI setup branch.'
}
$rpcCommonLines.RemoveRange($dceBranchStart, $dceBranchEnd - $dceBranchStart + 1)

Write-GeneratedFile `
    -Path (Join-Path $filteredResourceRoot 'rpc_common.c') `
    -Content ($rpcCommonLines -join $rpcCommonNewLine)

$nvspdmRsaPath = Join-Path $repositoryRoot 'src\nvidia\src\libraries\libspdm\nvidia\nvspdm_rsa.c'
$nvspdmRsaContent = [System.IO.File]::ReadAllText($nvspdmRsaPath)
$signingGuard = '#if LIBSPDM_RSA_PSS_SUPPORT'
$signingGuardMatches = ([regex]::Matches(
    $nvspdmRsaContent,
    [regex]::Escape($signingGuard))).Count
if ($signingGuardMatches -ne 1)
{
    throw "Expected one requester RSA-PSS signing guard, found $signingGuardMatches."
}
$filteredNvspdmRsa = $nvspdmRsaContent.Replace(
    $signingGuard,
    '#if 0 // Windows RM has no N/D/E private-key import provider.')
$filteredLibspdmRoot = Join-Path $OutputRoot 'libspdm\nvidia'
Write-GeneratedFile `
    -Path (Join-Path $filteredLibspdmRoot 'nvspdm_rsa.c') `
    -Content $filteredNvspdmRsa

$cryptAsymPath = Join-Path $repositoryRoot 'src\nvidia\src\libraries\libspdm\3.5.0\library\spdm_crypt_lib\libspdm_crypt_asym.c'
$cryptAsymContent = [System.IO.File]::ReadAllText($cryptAsymPath)
$rsaPssCallPattern = '(?s)return libspdm_rsa_pss_sign \(context, hash_nid,\s*message, message_size, signature, sig_size\);'
$rsaPssCallMatches = ([regex]::Matches($cryptAsymContent, $rsaPssCallPattern)).Count
if ($rsaPssCallMatches -ne 1)
{
    throw "Expected one RSA-PSS signing wrapper call, found $rsaPssCallMatches."
}
$filteredCryptAsym = [regex]::Replace(
    $cryptAsymContent,
    $rsaPssCallPattern,
    'return false;')
Write-GeneratedFile `
    -Path (Join-Path $filteredLibspdmRoot 'libspdm_crypt_asym.c') `
    -Content $filteredCryptAsym

$pmaSourceRoot = Join-Path $repositoryRoot 'src\nvidia\src\kernel\gpu\mem_mgr\phys_mem_allocator'
$filteredPmaRoot = Join-Path $OutputRoot 'gpu\mem_mgr\phys_mem_allocator'

$pmaPath = Join-Path $pmaSourceRoot 'phys_mem_allocator.c'
$pmaContent = [System.IO.File]::ReadAllText($pmaPath)
$numaEnabledAssignment = '        pPma->bNuma = !!(initFlags & PMA_INIT_NUMA);'
$numaAutoOnlineAssignment = '        pPma->bNumaAutoOnline = !!(initFlags & PMA_INIT_NUMA_AUTO_ONLINE);'
if (([regex]::Matches(
        $pmaContent,
        [regex]::Escape($numaEnabledAssignment))).Count -ne 1 -or
    ([regex]::Matches(
        $pmaContent,
        [regex]::Escape($numaAutoOnlineAssignment))).Count -ne 1)
{
    throw 'Expected one PMA NUMA and one NUMA auto-online assignment.'
}
$filteredPma = $pmaContent.Replace(
    $numaEnabledAssignment,
    '        pPma->bNuma = NV_FALSE;').Replace(
    $numaAutoOnlineAssignment,
    '        pPma->bNumaAutoOnline = NV_FALSE;')

$freeMemoryNumaPattern = '(?s)#if !defined\(SRT_BUILD\)\s+NvU64 val;\s+portSyncSpinlockAcquire\(pPma->pPmaLock\);\s+NvBool nodeOnlined = pPma->nodeOnlined;\s+portSyncSpinlockRelease\(pPma->pPmaLock\);\s+if \(nodeOnlined\)\s+\{\s+osGetNumaMemoryUsage\(pPma->numaNodeId, pBytesFree, &val\);\s+return;\s+\}.*?#endif'
$totalMemoryNumaPattern = '(?s)#if !defined\(SRT_BUILD\)\s+NvU64 val;\s+portSyncSpinlockAcquire\(pPma->pPmaLock\);\s+NvBool nodeOnlined = pPma->nodeOnlined;\s+portSyncSpinlockRelease\(pPma->pPmaLock\);\s+if \(nodeOnlined\)\s+\{\s+osGetNumaMemoryUsage\(pPma->numaNodeId, &val, pBytesTotal\);\s+return;\s+\}.*?#endif'
if (([regex]::Matches($filteredPma, $freeMemoryNumaPattern)).Count -ne 1 -or
    ([regex]::Matches($filteredPma, $totalMemoryNumaPattern)).Count -ne 1)
{
    throw 'Expected one free-memory and one total-memory NUMA query block.'
}
$filteredPma = [regex]::Replace($filteredPma, $freeMemoryNumaPattern, '')
$filteredPma = [regex]::Replace($filteredPma, $totalMemoryNumaPattern, '')
Write-GeneratedFile `
    -Path (Join-Path $filteredPmaRoot 'phys_mem_allocator.c') `
    -Content $filteredPma

$pmaUtilPath = Join-Path $pmaSourceRoot 'phys_mem_allocator_util.c'
$pmaUtilContent = [System.IO.File]::ReadAllText($pmaUtilPath)
$numaCleanupPattern = '(?s)static NV_STATUS\s+_pmaCleanupNumaReusePages\s*\(.*?\n\}\s*(?=/\*!)'
if (([regex]::Matches($pmaUtilContent, $numaCleanupPattern)).Count -ne 1)
{
    throw 'Expected one PMA NUMA reuse cleanup function.'
}
$numaCleanupReplacement = @'
static NV_STATUS
_pmaCleanupNumaReusePages
(
    PMA         *pPma,
    NvU64        evictStart,
    NvU64        numFrames,
    NvBool       bEvictionSucceeded
)
{
    (void)pPma;
    (void)evictStart;
    (void)numFrames;
    (void)bEvictionSucceeded;
    return NV_ERR_NOT_SUPPORTED;
}

'@
$filteredPmaUtil = [regex]::Replace(
    $pmaUtilContent,
    $numaCleanupPattern,
    $numaCleanupReplacement)
Write-GeneratedFile `
    -Path (Join-Path $filteredPmaRoot 'phys_mem_allocator_util.c') `
    -Content $filteredPmaUtil

$numaPath = Join-Path $pmaSourceRoot 'numa.c'
$numaContent = [System.IO.File]::ReadAllText($numaPath)
$numaExportPatterns = @{
    pmaNumaAllocate = '(?m)^NV_STATUS pmaNumaAllocate\s*$'
    pmaNumaFreeInternal = '(?m)^void pmaNumaFreeInternal\s*$'
    pmaNumaSetReclaimSkipThreshold = '(?m)^void pmaNumaSetReclaimSkipThreshold\s*\('
}
foreach ($exportedFunction in $numaExportPatterns.Keys)
{
    if (([regex]::Matches(
            $numaContent,
            $numaExportPatterns[$exportedFunction])).Count -ne 1)
    {
        throw "Expected one exported $exportedFunction implementation."
    }
}
$filteredNuma = @'
#include "gpu/mem_mgr/phys_mem_allocator/numa.h"

NV_STATUS pmaNumaAllocate(
    PMA* pPma,
    NvLength allocationCount,
    NvU64 pageSize,
    PMA_ALLOCATION_OPTIONS* allocationOptions,
    NvU64* pages)
{
    (void)pPma;
    (void)allocationCount;
    (void)pageSize;
    (void)allocationOptions;
    (void)pages;
    return NV_ERR_NOT_SUPPORTED;
}

void pmaNumaFreeInternal(
    PMA* pPma,
    NvU64* pages,
    NvU64 pageCount,
    NvU64 size,
    NvU32 flags)
{
    (void)pPma;
    (void)pages;
    (void)pageCount;
    (void)size;
    (void)flags;
}

void pmaNumaSetReclaimSkipThreshold(PMA* pPma, NvU32 skipReclaimPercent)
{
    (void)pPma;
    (void)skipReclaimPercent;
}
'@
Write-GeneratedFile `
    -Path (Join-Path $filteredPmaRoot 'numa.c') `
    -Content $filteredNuma
