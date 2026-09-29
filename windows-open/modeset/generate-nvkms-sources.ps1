param(
    [string]$RepositoryRoot = (Resolve-Path "$PSScriptRoot\..\..")
)

$manifest = Join-Path $RepositoryRoot "src\nvidia-modeset\srcs.mk"
$output = Join-Path $PSScriptRoot "nvkms-sources.props"
$items = foreach ($line in Get-Content $manifest) {
    if ($line -notmatch '^SRCS(_CXX)? \+= (.+)$') {
        continue
    }

    $language = if ($matches[1] -eq '_CXX') { 'CompileAsCpp' } else { 'CompileAsC' }
    $relativePath = $matches[2].Replace('/', '\')
    $sourcePath = "`$(RepoRoot)src\nvidia-modeset\$relativePath"
    $objectGroup = if ($language -eq 'CompileAsCpp') { 'cxx' } else { 'c' }

@"
    <ClCompile Include="$sourcePath">
      <CompileAs>$language</CompileAs>
      <ObjectFileName>`$(IntDir)upstream\$objectGroup\%(Filename).obj</ObjectFileName>
    </ClCompile>
"@
}

$content = @"
<?xml version="1.0" encoding="utf-8"?>
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup>
$($items -join '')  </ItemGroup>
</Project>
"@

Set-Content -Path $output -Value $content -Encoding utf8NoBOM
