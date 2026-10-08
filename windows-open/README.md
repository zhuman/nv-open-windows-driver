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

For PCI-enumerated IddCx devices, hardware preparation retains authoritative
segment, bus, device, and function properties, validates NVIDIA PCI identity,
and catalogs the first three valid memory BARs in the same register,
framebuffer, and instance-memory order as KMDOD and upstream. It matches the
register BAR to the translated memory resource and maps it noncached through
UMDF before creating DMA and interrupt objects. Framework-mediated reads
collect `PMC_BOOT_0` and `PMC_BOOT_42`; an all-ones response fails preparation
as a missing device. Release and every failure path unwind interrupt, DMA,
MMIO, and bus-interface state in reverse ownership order. The root-enumerated
development device retains its resource-free software path.

The first D0 entry creates one device-owned IddCx adapter asynchronously with
conservative endpoint capabilities. Adapter completion and mode-commit
callbacks resolve through an adapter-object context, while ordinary D0 exits
only mark the adapter unpowered so a resume cannot create a duplicate WDDM
adapter. Parent teardown clears the callback owner before deleting the adapter.
No monitor is reported until RM/NVKMS provides a real connector and mode
snapshot.

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
by every adapter. The RM-facing entry point accepts bounded ASCII parameter
names and imports DWORD values from the service `Parameters` subkey; RM runtime
writes remain in its own in-memory registry and do not modify service settings.
The RM OS-services library exports both `osReadRegistryDwordBase` and the public
`osReadRegistryDword` entry point with their upstream signatures. They keep
`OBJGPU` opaque and translate Windows query results through a WDK-free
`NV_STATUS` ABI.
The RM OS library also exports wall-clock seconds/microseconds and Windows
performance-counter timestamps with their decoding frequency. The latter uses
`KeQueryPerformanceCounter`, matching the upstream requirement that RM and
DirectX timestamps share a clock domain.
Monotonic and high-resolution counters are exposed in nanoseconds. Millisecond
delays yield the thread when the current IRQL permits and otherwise use bounded
processor stalls; microsecond and nanosecond delays stall directly. As on
Linux, hard-interrupt waits longer than 20 ms are rejected.
RM CPU discovery now has exact CPUID, frequency, possible-processor-count, and
current-processor hooks. Processor counts span every Windows processor group,
and the current processor is flattened through preceding groups using maximum
group sizes so RM thread-state indices remain stable.
RM thread-state initialization now has exact maximum-core, current-thread,
current-process-flags, and scheduler-yield hooks. Kernel and terminating thread
flags come from WDK thread state, and scheduling fails explicitly when the
current IRQL cannot yield instead of silently continuing.
The first RM core target compiles the upstream `locks.c`, `locks_common.c`, and
`thread_state.c` sources unchanged with ClangCL. Windows supplies only the
NvPort profile, generated-header customization boundary, and required upstream
include roots; warning exceptions are scoped to known generated/upstream C
constructs.
The RM core target now also owns the upstream system constructor, RM API cache
and stub initialization, resource descriptors, event log, binary-data registry,
and generated HAL registration path. The Windows `osRmInitRm` export registers
those HALs and applies the upstream thread-state defaults; registry lifetime
remains driver-global and is initialized before RM construction.
The first generated NVOC object graph now includes the upstream runtime,
`Object`, `OBJTRACEABLE`, `OBJSYS`, `OBJHALMGR`, `OBJHAL`, and `OBJOS` class
bodies. Windows constructs `OBJOS` with generated property defaults instead of
inheriting Unix reset, ACPI-cache, or vblank policy.
The generated resource, GPU, memory, and FINN foundations are now followed by
the next mandatory system child, `OBJPFM`. Its generated class, common platform
implementation, and NBSI table owner compile unchanged, preserving per-system
NBSI state and the later ACPI/VBIOS discovery path without introducing a
Windows-only data-layout fork.
The following mandatory child, `OBJHYPERVISOR`, also compiles from its generated
class and common Xen, KVM, Hyper-V, and VMware backends. x64 detection uses the
Windows `osNv_cpuid` export; ARM64 retains the upstream `osGetSmbiosTable`
dependency as an explicit OS-service boundary to close before RM is linked into
KMDOD.
`OBJCL` is the next compiled mandatory system child. Its generated class,
common chipset policy, PCIe topology/configuration implementation, and chipset
information table compile unchanged for x64 and ARM64. The independent PCI PBI
helper remains deferred until a caller enters the startup graph.
The already-present `OBJGPUMGR` is followed by `OBJGSYNCMGR`, whose generated
class and common manager now compile unchanged. Construction initializes each
slot with the common null provider; hardware-specific G-Sync providers remain
deferred until GPU/device discovery requires them.
The generated `GpuAccounting` class and common accounting implementation now
compile unchanged as the next mandatory child. Construction initializes the
per-GPU live and retired process stores through the existing intrusive map and
list layer; sampling and timer activity remain deferred until GPU startup.
`PlatformRequestHandler` now includes its generated class, common lifecycle,
and control/sensor implementation. Both architectures compile the upstream
sources unchanged; target-local forced inclusion supplies the Unix control
parameter declarations exposed by the shared generated client-resource header,
without importing Unix control behavior.
The generated `OBJRCDB` class and common robust-error journal now compile
unchanged. Construction owns its journal, assertion, debugger, and diagnostic
ring buffers; the project supplies the generated `NV_VERSION_STRING` header
using the same target-local version metadata pattern as other RM sources.
The data-only `OBJVMM` child was already present. `KernelVgpuMgr` now follows it
with generated metadata, common profile helpers, the kernel manager, and its
lazy embedded `OBJEHEAP` implementation. Construction initializes driver-global
vGPU version ranges and list ownership without creating per-GPU state.
`OBJGPUBOOSTMGR` now compiles with its generated manager class and the generated
`SyncGpuBoost` RM resource that shares the common implementation unit. The
driver-global manager starts empty and uses the existing btree layer to track
GPU membership only when clients create boost groups.
The generated `Fabric` system class and common fabric cache implementation now
compile unchanged. Construction initializes driver-global event/cache containers
and NvPort RW locks with an invalid node ID; fabric memory resources and GPU
probe behavior remain deferred until their startup callers are imported.
The generated `GpuDb` class now closes the already-compiled common GPU database
implementation. It constructs an empty driver-global GPU list protected by an
NvPort mutex; per-GPU registration remains deferred to device startup.
Generated RM configuration predicates and the shared `OBJENGSTATE` and
`IntrService` NVOC ancestors now compile unchanged. A direct `coreInitializeRm`
link probe resolves those families and is being used to drive the remaining
resource-catalog and GPU HAL closure without weakening the production path.
The Windows RM core now compiles NVIDIA's complete generated NVOC and protobuf
catalog unchanged. Existing per-file metadata remains authoritative, while the
remaining generated units share their upstream control, GSP metadata, CrashCat,
and NVLink declaration headers. This makes every registered class contract
visible to the standalone probe so missing implementation owners cannot remain
hidden behind absent class definitions.
The matching portable implementation catalog is now complete for the upstream
GPU, memory manager, RMAPI, diagnostics, core, compute, display, platform,
Hyper-V virtualization, vGPU, NVLink, libspdm, CrashCat, CPER, MMU, and support
library families. Recursive project items preserve unique object paths and
exclude files already carrying per-source build metadata. Unix, DCE-only,
NvSwitch, Xen, KVM, and VMware policy implementations remain excluded. With
portable owners and the NVLink OS adapter present, the standalone probe has no
remaining active portable implementation symbol; its remaining 282 unique
contracts are Windows OS and cryptographic providers or deliberately excluded
platform controls.
Windows NVPort user-buffer copies validate arguments and delegate to guarded
WDK `ProbeForRead`/`ProbeForWrite` operations in the platform layer, returning
an address error on access exceptions. Test-only IOMMU SVA allocation reports
unsupported without publishing a handle or PASID, and Windows identifies
itself as neither a VGX hypervisor nor a provider of guest-MSI injection. These
are explicit capability boundaries rather than successful placeholder paths.
The generated `GpuManagementApi` and complete `GSyncApi` resource layers now
close their catalog entries. GSync notification traversal intentionally leaves
Windows event delivery as an explicit OS-service boundary rather than using a
placeholder notification implementation.
That boundary now has a native provider: user event handles are type- and
access-checked with `ObReferenceObjectByHandle`, held for the RM event lifetime,
signaled with `KeSetEvent`, and released through the matching object-reference
ABI. Kernel callbacks, index filtering, and subdevice filtering remain in the
thin RM sidecar while WDK types stay in the platform layer.
The generated `MpsApi` resource and its upstream constructor/destructor now
close the MPS catalog entry unchanged. The class adds no Windows OS boundary or
hardware dependency; its policy remains in the existing resource-server access
and parent-class checks.
The generated `KernelWatchdog` resource and its upstream implementation also
compile unchanged. Registry overrides use the existing Windows registry ABI;
its remaining MIG predicate is shared with the wider pending MIG manager
closure rather than introducing a new startup dependency.
RM's allocation-free system semaphore ABI now uses a driver-global Windows
kernel semaphore initialized with the platform-system lifetime. Blocking
acquisition is limited to wait-safe IRQL, conditional acquisition is
nonblocking, and teardown remains ordered after RM has released its locks.
The complete upstream `Intr` owner and vector container now provide RM's
interrupt state and mask bookkeeping. Windows lock hooks preserve the upstream
PCI-GPU behavior by disabling and restoring hardware interrupts through the
`Intr` HAL; unsupported Tegra SoC display IRQ control is asserted explicitly.
PCI configuration access now has an allocation-free platform registry keyed by
the complete domain, bus, device, and function tuple. Stable handles retain the
owning adapter's configuration callbacks, while a WDK-free sidecar supplies
RM's byte, word, and dword PCI ABI without introducing global-adapter policy.
KMDOD registers each callback set during adapter start using the PDO's packed
PCI segment/bus and slot properties, and unregisters it before clearing the
DXGK interface during stop. The stored location is available to construct RM's
eventual attach argument without assuming segment zero.
Kernel PCI mappings now preserve RM's requested cache and protection modes
through `MmMapIoSpaceEx`. Nonpaged platform metadata retains each aligned
Windows mapping so both generic kernel-space and PCI-specific RM unmaps release
the exact region even when the physical address begins within a page.
The unchanged upstream GPU mapping and OS register owners now select BAR0 from
`OBJGPU`, retain vGPU PCI-mirror routing, and perform bounded Windows
`READ_REGISTER_*`/`WRITE_REGISTER_*` accesses. Complete upstream NBSI and FNV
owners preserve firmware-backed registry override lookup on this path.
Upstream GPU register access and its generated `IoAperture` and
`RegisterAperture` classes now construct the per-GPU MMIO access object during
`gpuPostConstruct`. The standalone RM probe reaches the deeper user-register
permission and GPU-loss policy boundaries without substituting Windows policy
or weakening register checks.
The complete upstream user-register permission bitmap owner now enforces the
per-register allowlist. Windows evaluates administrator access against the
effective subject token, captures assertion stacks without allocation, maps RM
fatal codes to a deliberate kernel bugcheck, and handles lost GPUs by emitting
Xid 79, notifying RM subdevices, setting upstream disconnected properties, and
latching loss in the adapter-owned context.
Kernel-privileged GPU mappings now return bounded aliases into that existing
BAR0 mapping, while lower-privilege requests remain delegated to the explicit
per-process PCI user-mapping boundary. Kernel unmaps therefore preserve the
adapter's BAR ownership rather than releasing device memory prematurely.
KMDOD now owns a nonpaged opaque RM GPU context from successful BAR validation
until adapter stop. RM's DPC attach reserves the context, normal attach publishes
the `OBJGPU`, and detach withdraws it before GPU destruction; failed publication
can roll back the reservation. Creating this context does not invoke RM or wire
interrupt dispatch into KMDOD.
That context now retains full BDF identity, the first three valid PCI memory
BARs in upstream register/framebuffer/instance order, translated aperture
lengths, BAR0's volatile kernel mapping, and interrupt-vector metadata. A
WDK-free RM wrapper converts it to `GPUATTACHARG` and transactionally performs
GPU-ID registration, instance allocation, attach, and failure unwind under the
RMAPI lock. The wrapper is probe-link complete but remains disconnected from
KMDOD while the wider RM probe is unresolved.
RM execution-context queries now distinguish wait-safe, DPC, and interrupt
IRQLs through the Windows kernel. Spin polling uses the architecture's processor
pause primitive, write-combined register ordering uses `KeFlushWriteBuffer`,
and whole-cache invalidation reports unsupported where the WDK has no portable
ARM64 operation instead of silently claiming completion.
Cross-process unmapping now references the target `EPROCESS`, retains a
nonpaged `KAPC_STATE` for the synchronous attach, and releases both in reverse
order after unmapping. Direct RM parameter buffers are classified with
overflow-safe `MmSystemRangeStart` bounds rather than probing arbitrary kernel
addresses or treating user pointers as trusted.
RM wait queues use nonpaged Windows synchronization events. A wake remains
pending until one waiter consumes it, DPC-level producers can signal safely,
and RM's interruptible and uninterruptible waits map to alertable and
nonalertable kernel waits at wait-safe IRQL.
Upstream `platform/cpu.c` now supplies `RmInitCpuInfo` unchanged on x64 and
ARM64. Its x64 capability detection reads CR4 and XCR0 through architecture-
guarded platform intrinsics, while the existing CPUID, frequency, and processor
count services remain shared with the RM OS sidecar.
Static system configuration queries Windows power capabilities and marks a
system as a notebook when a system battery is present, matching one of the
upstream notebook criteria. Confidential-compute flags remain clear until a
Windows guest-isolation provider can report the active technology explicitly.
The optional service `Parameters\BugCheckOnTimeout` DWORD controls RM's timeout
bugcheck property. Missing, invalid, and zero values leave bugchecks disabled;
any nonzero DWORD enables the existing RM timeout policy.
RM system callbacks use driver-object-owned `IO_WORKITEM` storage and execute on
the delayed system worker queue. Rundown protection rejects new callbacks once
RM teardown starts and makes `osSyncWithRmDestroy` wait for every accepted
callback before global state is released. Work storage is nonpaged and retains
the platform's zero-initialization policy.
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

Both INFs retain root-enumerated development hardware IDs and also match every
NVIDIA PCI device listed in the repository's top-level compatible-GPU table.
Generic rows generate `PCI\VEN_10DE&DEV_xxxx` matches; subsystem-specific rows
generate the more specific Windows `SUBSYS_<device><vendor>` form. Device
Manager therefore displays the corresponding product name from that table.

The generated model and string blocks are authoritative output. Regenerate
both INFs after the compatible-GPU table changes instead of editing those
blocks by hand:

```powershell
& .\Generate-SupportedDeviceInfs.ps1
```

KMDOD and IddCx are alternative function drivers for the same PCI function.
Install only one package on a given GPU at a time.

The next NvPort expansion should follow the first thread and allocator call
sites reached while importing the selected RM core. Keep using sidecar Windows
backends and explicit module definitions; do not modify the published NvPort
headers or compile Unix kernel backend files.

## Monitor control (I2C/DDC/CI)

IddCx now registers monitor I2C transmit, receive, and availability-gated
combined transmit/receive callbacks. The shared NVKMS facade resolves the
current display/connector on every request and permits only the DDC/CI endpoint
`0x37`. Windows' directional `0x6E`/`0x6F` addresses are normalized at the
IddCx boundary; arbitrary board, EDID, and HDCP bus access is not exposed.

- **DisplayPort/USB-C:** RM I2C-over-AUX with 16-byte chunks, repeated-start/MOT
  handling, terminal STOP, bounded DEFER/write-status retries, and staged
  receive-buffer publication. Length-prefixed replies are supported.
- **HDMI/DVI/VGA:** published RM indexed I2C, fixed-length transmit/receive,
  and combined requests with at most four transmit/index bytes. Length-prefixed
  native receives and larger combined prefixes return `NOT_SUPPORTED` before
  bus traffic. This path is software-tested, not hardware-validated.
- **Limits:** 64 transmit bytes and 256 receive bytes per callback. Internal
  displays and DP MST are unsupported. KMDOD's OS-facing I2C interface is
  still pending.

Transactions retain a WDF power reference and serialize with NVKMS EDID,
hotplug, link training, and other NVKMS users through the existing device and
recursive NVKMS locks. This is conservative global serialization, not
independent per-connector concurrency. Lifecycle/lock acquisition and AUX
software retries are bounded; synchronous RM calls still use RM's own hardware
timeouts. Disconnect, power loss, and teardown return explicit errors.

### Build and run the Win32 test tool

From the repository root:

```powershell
& .\windows-open\tools\build-monitor-ddc.ps1
$ddc = '.\windows-open\out\x64\monitor-ddc\monitor-ddc.exe'
& $ddc --list
& $ddc --display '\\.\DISPLAYn' --capabilities-string
```

Replace `DISPLAYn` with the NVIDIA-connected display shown by `--list`;
display numbers can change after a driver restart. Run the executable in the
logged-in **interactive desktop**, not directly through SSH/session 0 or RDP.
Listing shows the actual GPU and monitor paths to avoid testing another GPU.
The tool is read-only by default and exercises `GetMonitorCapabilities` and
`GetMonitorBrightness`; `--capabilities-string` also retrieves the MCCS text.

For manual monitor-OSD observation:

```powershell
& $ddc --display '\\.\DISPLAYn' --brightness-only --exercise --hold-ms 10000
```

This changes brightness by approximately 5% of its reported range, checks exact
readback, holds it for ten seconds, then restores and verifies the original.
Let the tool finish; killing it or unplugging the monitor can prevent
restoration. Writes require an explicit display; `--physical N` selects a
physical monitor index (default `0`). Hold time is limited to 60 seconds.
`--brightness-only` explicitly skips the capabilities API, rather than hiding
its failure. Exit codes are `0` for success, `1` for API/verification failure,
and `2` for invalid command-line arguments.

### Validation and remaining work

RTX 4070 / ROG PG27U DisplayPort testing successfully read brightness
`min=0, current=80, max=500`, changed **80 -> 105 -> 80**, verified restoration,
and retrieved the full MCCS capabilities string. The monitor advertises MCCS
2.2; Windows `GetMonitorCapabilities` returns
`0xC02625DC` (`ERROR_GRAPHICS_MCA_UNSUPPORTED_MCCS_VERSION`). Default tool mode
therefore reports failure even when the independent brightness operations
succeed; the monitor's version and capability data are not rewritten.

The final scoped `--brightness-only --exercise` test passed, as did a separate
post-restoration read. All 46 monitor-I2C callbacks in that final trace succeeded;
the maximum recorded callback time was 16 ms, below IddCx's five-second
deadline. There were no lost ETW events or runtime errors during those tests.
These observations do not certify the untested failure/power scenarios below.

The production framing, AUX, and native-indexed helpers have **67,411 counted
software cases** covering bounds, addresses, chunking, partial ACKs,
DEFER/write-status polling, cleanup, and error propagation:

```powershell
& .\windows-open\iddcx\tests\run-monitor-i2c-tests.ps1
```

This runner uses the production Clang-CL toolchain. Native HDMI/DVI/VGA,
positive length-prefixed/combined hardware requests, unplug during a
transaction, forced timeouts, power transitions, and HLK certification remain
unvalidated. No capture device was connected for these DDC tests; physical
brightness/OSD confirmation remains a manual step.