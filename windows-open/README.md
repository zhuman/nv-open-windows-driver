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
device-specific resources and tears them down with the adapter or WDF object
lifecycle.

KMDOD binds one driver-owned common host context to NvPort before registering
with Dxgkrnl and unbinds it from the driver unload callback. Adapter start and
stop only manage device-owned PCI, MMIO, interrupt, and display state, so
multiple GPU instances share NvPort without reinitializing or tearing down its
global services. The current NvPort profile
enables core, atomic, CPU discovery, debug, memory, pseudorandom generation,
safe arithmetic, string, synchronization, thread, time, and utility. CPU
identity and performance-counter time are implemented; privileged MSR, PMU,
and integrated-memory-controller operations report that they are unsupported.
The xorshift generator is available for RM jitter and identifiers but is not a
cryptographic entropy source.
The kernel platform layer retains the driver service registry key for the
driver lifetime and provides typed DWORD reads from the root or a relative
subkey. This gives global RM initialization one immutable settings source shared
by every adapter.
Its raw allocation header retains the pool type needed by the host free callback;
kernel allocations remain zero-initialized through the default `ExAllocatePool2`
behavior. Upstream counter-only memory tracking reports aggregate and
per-allocator usage without allocation staging overhead. Clang C11 atomics are
used unchanged. Debug strings route through the
common host logger, formatted output uses WDK `vDbgPrintEx`, and opaque
synchronization handles keep WDK types out of the Clang library. Utility
intrinsics come from upstream, with sidecar interrupt-context, spin, and delay
operations. Thread, CPU, and crypto modules remain disabled until their Windows
backends or portable source selections are added.

The thread sidecar implements invalid-handle constants, current thread/process
identity, ID lookup and comparison, scheduler yield, and kernel thread create,
exit, join, and destroy over a narrow WDK platform contract. Created threads
retain their kernel object and completion status in a registry owned by the
NvPort binding lifecycle. Priority and processor-group affinity operations are
also implemented. Process create/exit notification registration preserves the
NvPort callback replacement contract over the WDK process-notify routine.

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

The next NvPort expansion should follow the first thread and allocator call
sites reached while importing the selected RM core. Keep using sidecar Windows
backends and explicit module definitions; do not modify the published NvPort
headers or compile Unix kernel backend files.