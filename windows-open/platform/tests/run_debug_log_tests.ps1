param(
    [string]$VisualStudio = 'C:\Program Files\Microsoft Visual Studio\18\Community'
)

$ErrorActionPreference = 'Stop'
$repo = (Get-Location).Path
if (-not (Test-Path 'windows-open\modeset\nvkms-user.vcxproj')) {
    throw 'Run this test script from the repository root.'
}
$output = "$repo\windows-open\out\tests\platform-log"
New-Item -ItemType Directory -Force $output | Out-Null
[xml]$project = Get-Content 'windows-open\modeset\nvkms-user.vcxproj'
$options = $project.Project.ItemDefinitionGroup.ClCompile
$includes = $options.AdditionalIncludeDirectories.Replace(
    '$(WindowsOpenRoot)', "$repo\windows-open\").Replace('$(RepoRoot)', "$repo\").
    Split(';') | Where-Object { $_ -notlike '%(*' }
$arguments = @('/nologo', '/std:c++17', '/O2', '/Gy', '/MT',
    '/clang:-fno-operator-names', '/D_CRT_SECURE_NO_WARNINGS')
$arguments += $includes | ForEach-Object { "/I`"$_`"" }
$arguments += @('/I"windows-open\common\include"', '/I"windows-open\iddcx"',
    "/Fo`"$output\\`"", "/Fe`"$output\debug_log_tests.exe`"",
    '"windows-open\platform\tests\debug_log_tests.cpp"',
    '"windows-open\platform\user\nvplatform_common.cpp"',
    '"windows-open\platform\user\nvplatform_system_rm.cpp"',
    '"windows-open\iddcx\common_services.cpp"',
    '/link', 'ntdll.lib', 'advapi32.lib')
$response = "$output\compile.rsp"
$arguments | Set-Content -Encoding ascii $response
$compiler = "$VisualStudio\VC\Tools\Llvm\x64\bin\clang-cl.exe"
$setup = "$VisualStudio\VC\Auxiliary\Build\vcvars64.bat"
& $env:ComSpec /c "call `"$setup`" >nul && `"$compiler`" @`"$response`""
if ($LASTEXITCODE -ne 0) { throw "Test compilation failed: $LASTEXITCODE" }
& "$output\debug_log_tests.exe"
if ($LASTEXITCODE -ne 0) { throw "Debug log tests failed: $LASTEXITCODE" }

$gsp = Get-Content -LiteralPath 'src\nvidia\src\kernel\gpu\gsp\kernel_gsp.c' -Raw
$start = $gsp.IndexOf("static NV_STATUS`n_kgspCheckIfMemsysError")
if ($start -lt 0) { $start = $gsp.IndexOf("static NV_STATUS`r`n_kgspCheckIfMemsysError") }
$end = $gsp.IndexOf('/*', $start)
if ($start -lt 0 -or $end -le $start) { throw 'Production GSP timeout probe boundaries not found.' }
$gsp.Substring($start, $end - $start) |
    Set-Content -LiteralPath "$output\gsp_timeout_probe.inc" -Encoding ascii
$arguments = @('/nologo', '/std:c++17', '/O2', '/MT')
$arguments += $includes | ForEach-Object { "/I`"$_`"" }
$arguments += @("/I`"$output`"", "/Fo`"$output\gsp_timeout_probe_tests.obj`"",
    "/Fe`"$output\gsp_timeout_probe_tests.exe`"",
    '"windows-open\platform\tests\gsp_timeout_probe_tests.cpp"')
$arguments | Set-Content -LiteralPath $response -Encoding ascii
& $env:ComSpec /c "call `"$setup`" >nul && `"$compiler`" @`"$response`""
if ($LASTEXITCODE -ne 0) { throw "GSP probe test compilation failed: $LASTEXITCODE" }
& "$output\gsp_timeout_probe_tests.exe"
if ($LASTEXITCODE -ne 0) { throw "GSP probe tests failed: $LASTEXITCODE" }
$arguments += '/DNV_GSP_TIMEOUT_TEST_NONWINDOWS'
$arguments | Set-Content -LiteralPath $response -Encoding ascii
& $env:ComSpec /c "call `"$setup`" >nul && `"$compiler`" @`"$response`""
if ($LASTEXITCODE -ne 0) { throw "Non-Windows GSP probe test compilation failed: $LASTEXITCODE" }
& "$output\gsp_timeout_probe_tests.exe"
if ($LASTEXITCODE -ne 0) { throw "Non-Windows GSP probe tests failed: $LASTEXITCODE" }
