# Windows Driver Scaffold

This directory is the sidecar build and platform root for the experimental
Windows port. It does not replace or modify the NVIDIA source build system.

## Projects

- `common/kernel/nvcommon-kernel.vcxproj` builds the initial NVIDIA common-code
  slice for kernel mode with Clang-CL.
- `common/user/nvcommon-user.vcxproj` builds the same slice for user mode with
  Clang-CL.
- `platform/kernel/nvplatform-kernel.vcxproj` provides Windows kernel platform
  primitives for the future RM host layer. Its first contract owns physical
  MMIO mappings, checked register access, and adapter-scoped PCI configuration
  access. Its public headers retain C linkage and C-compatible declarations so
  upstream RM C translation units can consume them later.
- `kmdod/nv-kmdod.vcxproj` is the WDDM display-only miniport scaffold.
- `iddcx/nv-iddcx.vcxproj` is the UMDF 2 and IddCx scaffold.

The common projects initially compile only `nvstatus.c`. Add upstream files to
the manifests under `manifests/` after they compile in both address spaces or
after their mode-specific dependencies are understood.

## Prerequisites

- Visual Studio 2026 Insiders (18.x) with Desktop development with C++
- The bundled `ClangCL` platform toolset (currently LLVM 22)
- Windows Driver Kit 10.0.28000.0
- A test-signing and deployment environment for loading either driver

Open `nv-windows-drivers.slnx` and build `Debug|x64`. The driver projects retain
the WDK platform toolsets because those targets provide driver linking, INF
stamping, signing, and packaging. The common libraries use `ClangCL` now.
Clang-CL integration for the driver binaries will be enabled separately after
the installed WDK toolset path is validated.

For command-line builds, use the 64-bit MSBuild from Visual Studio 2026
Insiders. WDK 28000 installs the INF verification task for x64 MSBuild:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  .\nv-windows-drivers.slnx /t:Build /p:Configuration=Debug /p:Platform=x64 /m
```

Both INFs use root-enumerated development hardware IDs. They deliberately do
not match NVIDIA PCI devices yet.

The next RM library boundary is NvPort. Its portable core is reusable, but the
published headers identify this source profile as Unix and do not enable MSVC.
Build it as a separate Clang-CL project with a Windows forced-include profile
and Windows allocator/synchronization/time backends; do not modify the
published NvPort headers or compile its Unix kernel backend files.