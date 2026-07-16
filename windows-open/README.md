# Windows Driver Scaffold

This directory is the sidecar build and platform root for the experimental
Windows port. It does not replace or modify the NVIDIA source build system.

## Projects

- `common/kernel/nvcommon-kernel.vcxproj` builds the initial NVIDIA common-code
  slice for kernel mode with Clang-CL.
- `common/user/nvcommon-user.vcxproj` builds the same slice for user mode with
  Clang-CL.
- `nvport/kernel/nvport-kernel.vcxproj` builds the first kernel NvPort slice
  with Clang-CL. It compiles upstream core and generic time conversion code
  with upstream string support and sidecar Windows memory, debug,
  synchronization, and time backends.
- `platform/kernel/nvplatform-kernel.vcxproj` provides Windows kernel platform
  primitives for the future RM host layer. Its first contract owns physical
  MMIO mappings, checked register access, and adapter-scoped PCI configuration
  access. It also owns opaque WDK spinlock, mutex, semaphore, reader/writer
  lock, and execution-level operations for NvPort. Its public headers retain C
  linkage and C-compatible declarations so upstream RM C translation units can
  consume them later.
- `kmdod/nv-kmdod.vcxproj` is the WDDM display-only miniport scaffold.
- `iddcx/nv-iddcx.vcxproj` is the UMDF 2 and IddCx scaffold.

The common projects compile upstream `nvstatus.c` and `nvSha256.c` in both
address spaces. They also compile the sidecar `nvcommon_host.c`, which validates
and wraps a versioned host-services table for allocation, Unix-epoch time,
monotonic uptime, execution-context allocation safety, and logging. KMDOD
supplies pool, kernel time, IRQL, and debugger callbacks; IddCx supplies
process-heap, user-mode time, and debugger callbacks. Each device owns its
common context and tears it down with the adapter or WDF object lifecycle.

KMDOD binds its common host context to NvPort before hardware discovery and
unbinds NvPort before destroying that context. The current NvPort profile
enables core, atomic, debug, memory, string, synchronization, and time. Its
allocation header retains the pool type needed by the host free callback; kernel
allocations remain zero-initialized through the default `ExAllocatePool2`
behavior. Clang C11 atomics are used unchanged. Debug strings route through the
common host logger, formatted output uses WDK `vDbgPrintEx`, and opaque
synchronization handles keep WDK types out of the Clang library. Thread, CPU,
crypto, safe, and utility modules remain disabled until their Windows backends
or portable source selections are added.

Add upstream files to the manifests under `manifests/` only after they compile
in both address spaces or after their mode-specific dependencies are understood.
Sidecar import implementations remain explicit project items rather than
upstream manifest entries.

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

The next NvPort expansion is memory tracking, followed by the safe, utility,
and thread functions reached by the selected RM core. Keep using sidecar
Windows backends and explicit module definitions; do not modify the published
NvPort headers or compile Unix kernel backend files.