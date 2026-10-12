param(
    [string]$VisualStudio = 'C:\Program Files\Microsoft Visual Studio\18\Community'
)

$ErrorActionPreference = 'Stop'
$repo = (Get-Location).Path
if (-not (Test-Path 'windows-open\modeset\nvkms-user.vcxproj')) {
    throw 'Run this test script from the repository root.'
}
$output = 'windows-open\out\tests\nvkms-mpo'
New-Item -ItemType Directory -Force $output | Out-Null
$flipSource = Get-Content -LiteralPath 'src\nvidia-modeset\src\nvkms-hw-flip.c' -Raw
$validationStart = $flipSource.IndexOf('inline static NvBool ValidateFlipChannelEvoHwState(')
$validationEnd = $flipSource.IndexOf('static NvBool ValidateSurfaceSize(', $validationStart)
if ($validationStart -lt 0 -or $validationEnd -le $validationStart) {
    throw 'Production flip-channel validation boundaries were not found.'
}
$flipSource.Substring($validationStart, $validationEnd - $validationStart).
    Replace('const NVFlipChannelEvoHwState *pState', 'const TestFlipState *pState') |
    Set-Content -LiteralPath "$output\flip_channel_validation.hpp" -Encoding ascii
[xml]$project = Get-Content 'windows-open\modeset\nvkms-user.vcxproj'
$options = $project.Project.ItemDefinitionGroup.ClCompile
$includes = $options.AdditionalIncludeDirectories.Replace(
    '$(WindowsOpenRoot)', "$repo\windows-open\").Replace('$(RepoRoot)', "$repo\").
    Split(';') | Where-Object { $_ -notlike '%(*' }
$defines = $options.PreprocessorDefinitions.Replace('$(NvKmsDebugDefinition)', '').
    Split(';') | Where-Object { $_ -notlike '%(*' }
$arguments = @('/nologo', '/std:c++17', '/O2', '/Gy', '/MT',
    '/clang:-fno-operator-names', '/D_CRT_SECURE_NO_WARNINGS')
$arguments += $includes | ForEach-Object { "/I`"$_`"" }
$arguments += $defines | ForEach-Object { "/D$_" }
$arguments += '/I"windows-open\common\include"'
$arguments += "/I`"$output`""
$arguments += @('/Fo"' + "$output\mpo_request_tests.obj" + '"',
    '/Fe"' + "$output\mpo_request_tests.exe" + '"',
    '"windows-open\modeset\tests\mpo_request_tests.cpp"',
    '/link', '/OPT:REF', '/OPT:ICF')
$response = "$output\compile.rsp"
$arguments | Set-Content -Encoding ascii $response
$compiler = "$VisualStudio\VC\Tools\Llvm\x64\bin\clang-cl.exe"
$setup = "$VisualStudio\VC\Auxiliary\Build\vcvars64.bat"
& $env:ComSpec /c "call `"$setup`" >nul && `"$compiler`" @`"$response`""
if ($LASTEXITCODE -ne 0) { throw "Test compilation failed: $LASTEXITCODE" }
& "$output\mpo_request_tests.exe"
if ($LASTEXITCODE -ne 0) { throw "MPO request tests failed: $LASTEXITCODE" }
