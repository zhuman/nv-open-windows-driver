# Windows Driver Scaffold

This directory is the sidecar build and platform root for the experimental
Windows port. It does not replace or modify the NVIDIA source build system.

## Live IddCx parameters and hardware-plane experiments

**Validation status (updated October 12, 2026):** implemented and locally
tested; Release x64 build/signing, ARM64 compile checks, and NativeAOT app
smoke checks passed. Coordinated October 12 hardware tests applied all six
scenes and Off on both D3D11 and native D3D12, with repeated live transitions
in one unchanged driver host. ShadowCast 2 Pro recordings verified animated
Geometry Compass and complete supported Showcase cycles. High-resolution
deadline scheduling reached measured layer-retirement rates of approximately
15.0/30.0/58.8 Hz for requests of 15/30/60 Hz. Long-run resource,
hotplug/power recovery, and interactive UAC acceptance remain open.
The monitor-departure double deletion and elevated GUI request-ownership issue
have production regressions and target validation.

The IddCx driver watches DWORD values under its UMDF framework key:

```text
HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\WUDF\Services\nv-iddcx\Parameters
```

This is **not** the PnP `WUDFRd` service key. Missing values retain their
documented defaults. Malformed, incomplete, or unsupported candidates do not
replace the last running configuration. Accepted changes retire the source
swapchain through IddCx and recreate it without restarting the device. Buffer
count or cache-policy changes also stage a replacement scanout pool. The old
front allocation stays owned until its replacement is latched.

For grouped writes, set `ConfigRevision` to an odd DWORD, write the complete
candidate, then commit with the next even revision. Odd or changing revisions
are never applied. Unversioned edits are accepted only after two matching
reads separated by a short settling interval. Normalized equivalent settings
and settings belonging to an inactive source API do not recreate a swapchain.
An explicitly committed new even revision can retry a latched native-source
failure once; healthy equivalent configurations remain no-ops.
The legacy D3D11 `ReadbackMode` key is watched as a fallback only when the
framework `D3D11ReadbackMode` value is absent.

The new demonstration controls are:

| DWORD | Values | Default |
| --- | --- | --- |
| `MpoDemoMode` | 0 Off; 1 Alpha Orbit; 2 Geometry Compass; 3 CSC Color Lab; 4 Luminance Elevator; 5 LUT Gallery; 6 Live Showcase | 0 |
| `MpoDemoConnectorIndex` | Physical connector index 0-15; required while enabled | Absent |
| `MpoDemoPlaneCount` | 1-7 additional planes, subject to the active head's layer budget | 1 |
| `MpoDemoCadenceHz` | 15, 30, or 60, not above the active refresh rate | 30 |
| `MpoDemoSpeedPercent` | 25-200 | 100 |

The software renderer produces animated source cards; NVKMS performs the
actual plane composition, supported rotation/reflection, CSC/color gain,
and independent input LUT. No software composition or head-wide gamma is
substituted for an unsupported plane feature. The desktop counts as one
hardware layer. Each additional layer retains separate buffer and LUT
ownership and is retired using that layer's own pending/timestamp queries.
Disabling a layer uses a null surface and resubmits the active primary in
the same atomic non-tearing update. Its actual primary completion witnesses
disabled-layer retirement; old leases remain held until that completion.
Overlay-only animation explicitly reuses unchanged desktop content and does
not invent an IddCx frame completion.

Live Showcase includes only capability-supported chapters and a supported
combination. Skipped chapters are reported explicitly; selecting an
unsupported standalone mode still rejects the candidate.
Geometry Compass is an approved exception: it cycles only supported
transformations and scales that pass finite test-only validation, with
omissions reported in the trace and an adaptation warning in status.
On the tested RTX 4070, mask `0x55` supports 0/180-degree rotations and
their reflected-X variants, not 90/270 degrees. Only unity scale passed;
0.5x and 1.5x were explicitly omitted. `rotation=false` means full
quarter-turn support is unavailable, not that every rotation is unavailable.
Cropping reduces source extents from the origin: the overlay KAPI does not
translate source offsets, and nonzero offsets are rejected. The combined
Showcase chapter keeps native size when CSC/gain uses the scaling pipeline;
validated geometric scaling is demonstrated separately where available.

`VrrPreference` remains best effort, not proof that VRR is active. Live changes
use a same-head modeset and verify the actual NVKMS reply. An unavailable
enable preference leaves VRR off and reports a warning while other valid
settings can apply. Failed rollback blocks further presentation and retains
scanout leases for recovery. The control app displays effective VRR separately
from the requested preference.

Both D3D11 and D3D12 use the shared presentation coordinator. Mode 0 retains
normal presentation; demonstrations are experimental and require actual
capability and test-only NVKMS validation on the selected active output.
**Current experimental hardware status (October 12, 2026): all six modes
activate and retire live on the tested two-layer RTX 4070 head.** The desktop
uses one layer, leaving one demo plane; requesting two demo planes rejects
the candidate without disturbing the current scene. Invalid types, odd
revisions, and equivalent valid commits also passed retention/no-op checks.
The earlier coarse-clock/full-interval scheduling under-delivered requested
cadences. QPC-based absolute deadlines and a high-resolution waitable timer
now exclude rendering time from the next frame budget, skip missed slots
instead of bursting, and preserve immediate stop/source wakeups. Three
36-second Showcase windows measured unique layer retirements at
15.000/29.9997/58.804 Hz for requests of 15/30/60 Hz, with median intervals
67/33/17 ms. These are observed rates, not a guarantee of exact 60 Hz.
Private memory returned near the native baseline, but handle-class audits
identified three additional `DxgkSharedSyncObject` handles per source
recreation: 3 to 45 across 14 changes. Read-only status polls did not explain
the growth. No borrowed/opaque GPU handles are closed speculatively; allocation
ownership and long-run behavior remain open before claiming reliability.
Packed layers now use an explicit neutral input LUT unless a custom LUT is
requested. This prevents the earlier default-LUT activation stall on this
target without claiming the underlying upstream allocation defect is proven.
The six-mode trace decoded 643,930 events with zero losses, format errors,
or unknowns; its 36 software flip rejections matched intentional unsupported
scale probes. The later deadline trace decoded 446,120 events with the same
zero-loss/error result. Neither showed Xid, retirement timeout, assertion,
fail-fast, or the guarded nested-RPC warning. That is not runtime acceptance
of the unexercised GSP timeout guard. The final deployed deadline candidate
was OEM45/DLL `D6B437A94DD3D29D3AE36FD46327E60ED123A1CB88B101A63D7D000BF5E43211`.
Defaults and native D3D12/Off were restored at October 12 05:25:36 PDT before
the verified AMD coordination release; demos remain opt-in. This is a
timestamped result, not a claim about later runtime state.
See [the approved experiment plan](../PLAN-WINDOWS.md#11a-live-parameters-internal-mpo-demos-and-a-control-app)
for the visual designs and validation requirements.

The compact [IddCx Control application](tools/iddcx-control/README.md) exposes
these settings and reads driver-reported accepted/applied generations from
the local `NvIddCx.Status` pipe. A registry write is not proof of application.
Publish and copy the complete NativeAOT output folder, not just its executable.

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
register BAR through corresponding raw/translated resources and maps it
noncached through UMDF before creating DMA and interrupt objects. Ordinary
and `MEMORY_LARGE` descriptors share checked 64-bit decoding. BAR0 and BAR2
remain eagerly mapped; BAR1 retains its full translated resource and provides
bounded on-demand RM mappings (see below). Framework-mediated reads
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
  .\nv-windows-drivers.slnx /t:Build /restore `
  /p:Configuration=Debug /p:Platform=x64 /m
```

The IddCx and user-mode RM projects default `NvRmUserLinkProbe` to `true`.
Despite its historical name, this enables the real RM/NVKMS GPU backend used
by the driver; the IDE therefore links the same backend as the build helper.
Explicit property overrides remain available, and kernel-mode RM activation
gates are unchanged.

WIL is pinned as a `PackageReference` in
[iddcx/nv-iddcx.vcxproj](iddcx/nv-iddcx.vcxproj). The project's
`EnableNativePackageReferenceSupport=true` property enables native C++ support
in Visual Studio and MSBuild. Command-line builds should include `/restore`;
NuGet supplies the package's native build imports without hand-written paths
or a solution-local `packages` directory.

For automated builds, use the 64-bit MSBuild `/restore` command above or the
[repository build helper](../build-windows-iddcx.ps1). Visual Studio 18.10.3
can report `ProjectNotNominatedException` from its background NuGet service
when starting `devenv /Build` on a WDK project, even though compilation and
linking succeed. This was also reproduced with an isolated WDK project.
MSBuild restores native `PackageReference` dependencies directly and reports
restore failures through its exit code; changing to `packages.config` is not
required.

Both INFs retain root-enumerated development hardware IDs and also match every
NVIDIA PCI device listed in the repository's top-level compatible-GPU table.
Generic rows generate `PCI\VEN_10DE&DEV_xxxx` matches; subsystem-specific rows
generate the more specific Windows `SUBSYS_<device><vendor>` form. Device
Manager therefore displays the corresponding product name from that table.

The NVKMS x64 shader-pack custom build explicitly selects Clang's
`x86_64-pc-windows-msvc` target. Custom commands do not inherit the normal
`ClCompile` architecture selection; using the compiler's default target can
produce an ARM64 shader object inside an x64 intermediate directory. If an
older build left that object behind, rebuild `nvkms-user` in the selected
x64 configuration rather than disabling optimization or forcing the
librarian to accept mixed architectures.

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

## Hardware cursor and frame-upload synchronization

The Windows NVKMS backend uses
[`PresentationLocks`](modeset/presentation_locks.hpp) to separate device lifetime,
CE transfers, display programming, and complete presentation transactions:

- Uploads, cursor updates, presentations, scanout-buffer lookups, and flip
  notifier reads take shared device-lifetime protection. Initialization,
  shutdown, external surface allocation/destruction, modesets, and monitor
  control retain exclusive protection.
- All backend CE transfers serialize separately because common KAPI caches one
  copy-channel handle. A position-only cursor update does not acquire this lock.
- Cursor programming, flip programming, and flip-notifier reads exclude one
  another, but the display-programming lock is not held across ordinary frame
  uploads. Cursor image uploads finish before acquiring programming exclusion.
- Complete presentations serialize across preflight, upload, and flip phases,
  without excluding cursor programming during the upload phase. The pipeline's
  existing RAM/VRAM leases still prevent uploads into front/pending storage.

Acquire device lifetime first, then the presentation transaction when needed,
then programming or copy exclusion. Infrequent LUT/gamma operations may take
copy exclusion while holding programming exclusion; the reverse nesting is
not permitted. Common NVKMS ioctl/timer synchronization remains unchanged.
The hot paths read the allocated KAPI device's immutable head/layer counts
instead of issuing a redundant RM resource query on every cursor update/flip.

Cursor shape changes still share the CE channel with frame uploads, and surface
allocation or modeset work still requires exclusive lifetime protection. This
change does not make those infrequent operations asynchronous.

The retained cursor API wraps a timed entry point. Verbose `Hardware cursor
timing` events report the attempt's status, QPC begin, lifetime/programming lock
wait ticks, image upload ticks, and programming start/end. These use the same
clock as pipeline upload intervals; failures/retries are traced too.

Run the presentation regressions with:

```powershell
& .\windows-open\iddcx\tests\run-dirty-regions-tests.ps1
```

The runner includes 24,054 production-header checks for cursor/copy overlap,
cursor/flip exclusion, CE/presenter serialization, lifecycle teardown, moved
guard ownership, and 12,000 concurrent copy/program operations.

Additional production backend and platform diagnostic checks are available:

```powershell
& .\windows-open\modeset\tests\run_mpo_request_tests.ps1
& .\windows-open\platform\tests\run_debug_log_tests.ps1
```

The IDDCX WPP provider mirrors RM platform debugger output, including Xid
messages, assertion frame addresses, and fail-fast bug codes. Enable the
`TRACE_DRIVER` flag at informational level or higher to capture these messages;
their original platform level is included in the payload. Resolve stack
addresses with the exact deployed DLL's PDB. These diagnostics do not bypass
hardware completion, interlocks, or recovery checks.

**RTX 4070 assessment (2026-10-08):** signed x64 package `oem200.inf`, loaded DLL
SHA-256 `8BA7FC24C991FCF2C526AE0742A098BDD6FD04EC11513C4FF2EB16B0A112333C`,
initialized and passed a deliberate warm restart. The restarted host remained
healthy through the subsequent tests. Immediate post-restart desktop enumeration
temporarily differed and a physical-monitor lookup failed; a later recheck
returned the baseline layout with the AMD-connected BenQ primary, without
this session changing display settings. The coordinated AMD session separately
reported an AMD-only install/rollback, so the transient is not attributed to
the NVIDIA changes. Read-only brightness 80 and MCCS capabilities-string
retrieval passed; the known MCCS 2.2 `GetMonitorCapabilities` error remains.

The user's 60-second workload reproduction decoded with zero lost events,
unknowns, or format errors. Its 58.668-second analysis interval contained 1,149
uploads/latches, including 589 complete 66,355,200-byte 3840x2160 FP16 uploads.
Of 2,554 successful position-only cursor attempts, **759 programming intervals
were entirely inside complete-frame upload intervals**. Cursor lock-wait p95
was 0.0011 ms; total backend cursor duration median/p95 was 0.0699/0.2094 ms.
Complete-frame upload median/p95 was 18.2555/18.3834 ms. No cursor failures or
retries were recorded. Cursor event gaps include periods without mouse movement
and are not an input-latency measurement.

Earlier automated window tests confirmed full-frame transfers but did not
produce sustained hardware-cursor updates, so they are not cursor-concurrency
proof or a valid before/after cursor benchmark. The reproduction verifies backend
overlap, not physical-panel input latency or camera-measured smoothness.
The final NVIDIA check retained problem code 0 and the verified loaded DLL,
with no new WER reports or owned test tasks/loggers left running.

## BAR1 resources and bounded mappings

The earlier UMDF code filtered out `CmResourceTypeMemoryLarge` in both BAR
mapping and RM attach discovery. Its descriptor-order fallback could then
misidentify BAR2 as BAR1; an "unassigned BAR1" trace was not reliable evidence
that Windows had left the framebuffer aperture unassigned.

The source now:

- Owns the RM-visible memory BARs in an indexed `PciBars` collection, using
  the same `PciBar` owner and a mapping/cache policy table for each entry.
  These indices identify the first three memory BARs (register/framebuffer/
  instance-memory), not PCI configuration DWORD positions; 64-bit BAR pairs
  are consumed by `ConfigSpace` during address discovery.
- Keeps PCI configuration access/address discovery in `ConfigSpace`, assigned
  resource resolution and registration in `PciBar`, and generic WDF mapping/
  access callbacks in `RegisterAperture`. The latter no longer probes BAR0
  identity or resolves CM resources; `PciBars` performs the GPU identity check.
- Uses noncopyable/nonmovable owners, `std::unique_ptr` mappings and WIL RAII
  registration handles. Provider unmap receives its owner context, so mappings
  remain smart-owned instead of transferring raw allocation ownership through
  the callback ABI. Registrations are removed before their mappings are freed.
  Preparation failures use one scope-exit hardware cleanup path, stopping
  RM/NVKMS before releasing BARs. Startup recovery suspends/restores only
  demand providers; fixed mappings, DMA, and WDF interrupts stay intact.
- Decodes ordinary memory and all `MEMORY_LARGE` 40/48/64-bit length encodings
  without truncating the complete BAR size.
- Matches each PCI BAR address against the raw resource list, then uses the
  corresponding translated address and length for WDF and RM. Missing,
  inconsistent, or ambiguous resources fail explicitly; there is no
  next-memory-descriptor fallback.
- Supplies RM with the complete BAR1 physical address and 64-bit length,
  independently of the size of any individual CPU mapping.
- Registers BAR1 as a write-combined, demand-mapped resource. Existing RM
  physical-mapping hooks obtain aliases into a single shared CPU window.
  The first request maps a page-aligned window covering the request, up to
  `0xFFFFF000` bytes (4 GiB minus 4 KiB, rounded down for the system page size).
  Later contained requests reuse that window without another WDF mapping.
- Enforces UMDF's **one active mapping per assigned CM resource** restriction.
  A request outside a leased window returns `NV_ERR_STATE_IN_USE`, rather than
  triggering the framework verifier with a second mapping. After the last
  lease ends, another window can be selected, including above 4 GiB or across
  a 4-GiB boundary. Simultaneous windows spanning the full large BAR are not
  supported. Oversized, out-of-resource, and cache-incompatible requests also
  fail explicitly.
- Reference-counts aliases, releases the backing window after the final unmap,
  and drains remaining mappings after RM/NVKMS teardown in
  hardware release. The existing global registry has 32 mapped-aperture slots,
  shared with eager BAR0/BAR2 mappings; exhaustion is an explicit error.

The WDF backend retains both the framework pseudo address and the process
address from `WdfDeviceGetHardwareRegisterMappedAddress`. Register access stays
framework-mediated. BAR0 remains noncached and BAR2 remains write-combined.
No new per-frame BAR1 mapping is introduced in the existing common-buffer/CE
presentation path. Future CPU-copy clients should retain an allocation-lifetime
mapping rather than map/unmap every frame; an idle mapping cache is not added.

Microsoft's [map](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfdevice/nf-wdfdevice-wdfdevicemapiospace)
and [unmap](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfdevice/nf-wdfdevice-wdfdeviceunmapiospace)
references specify `PASSIVE_LEVEL` and describe startup/release usage. They do
not list a startup-only DDI restriction or document the user-mode 4-GiB limit.
This backend permits demand maps only while its assigned-resource provider is
registered between hardware preparation and release. Actual runtime mapping of
the target's large BAR has now been tested. The
[framework implementation](https://github.com/microsoft/Windows-Driver-Frameworks/blob/master/src/framework/shared/support/um/fxresourcecollectionum.cpp)
explicitly rejects multiple active mappings within one assigned resource;
disjoint subranges are not an exception.

Local validation covers **over 200,000 resource/mapping checks and 8,000 concurrent
map/unmap cycles**, using the production decoder and mapping registry:

```powershell
& .\windows-open\iddcx\tests\run-bar-resource-tests.ps1
```

The same runner executes **333 production `PciBar`/`PciBars` ownership/lifecycle
checks** with mock PCI config and WDF mapping backends. These cover partial
initialization cleanup, destruction with live aliases, demand-provider
suspension/restoration while fixed mappings stay unchanged, reinitialization,
and rejection of invalid restoration without draining existing leases.

The x64 IddCx and experimental KMDOD compatibility builds pass, as do DDC and
presentation regressions. ARM64 kernel/user platform libraries also build;
the IddCx sources compile for ARM64, but full ARM64 driver linking and ARM64
hardware remain untested. Unit tests use a mock
backend, now enforcing one active mapping, not the real WDF mapping APIs.

**Ownership-refactor reassessment (2026-10-07):** signed x64 package
`oem199.inf`, DLL SHA-256
`AF01BA5A96C82177E850D19D7D336E43F79477F95D66AF4BBFF4AC8AB06C83EC`,
initialized and passed a deliberate warm restart. Each startup hit a real GSP
`NV_ERR_TIMEOUT` and recovered within its own original host after one in-place
reset/retry, without external recovery commands. The restarted host remained
stable for 60 seconds with problem code 0; the loaded DLL hash was verified.
Mode/layout matched the baseline, including the unchanged AMD primary.
Read-only DDC returned brightness 80 and the capabilities string. A 12-second
animation produced 374 application paints; ETW recorded 547 uploads and 547
latches. No new UMDF host/verifier WER reports or owned test tasks/loggers
remained. These are pipeline/runtime observations, not camera verification
or proof of uninterrupted physical output.

**RTX 4070 deployment assessment (2026-10-07):**

- Windows assigned BAR1 at `0x2C00000000`, length `0x400000000` (**16 GiB**).
  RM receives the full address and size.
- The initial candidate successfully mapped 64 KiB, but its second submapping
  caused `WUDFVerifierFailure` in `ValidateRegisterPhysicalAddressRange` and
  repeated host restarts. The shared-window correction fixes that violation
  without disabling verification or bypassing framework register access.
- Corrected package `oem193.inf` (DLL SHA-256 prefix `27E5BDFE`) successfully
  maps the window at `0x2C00000000`, length `0xFFFFF000`. The host stayed stable
  through initial activation, animation, DDC tests, and recovered activation.
- The AMD display remains primary. NVIDIA's reported 3840x2160, 32-bpp,
  integer-60-Hz mode, orientation, and desktop layout match the baseline.
  Logical bounds remain 2560x1440; do not equate these DPI-virtualized bounds
  with physical timing. Exact fractional refresh and HDR state were not queried.
- Twelve-second animation tests produced 370 and 375 application paints
  (baseline: 372). ETW confirms D3D12 common-buffer pixel verification, CE
  uploads, and scanout latch observations. Application paints are not displayed
  frames; the baseline trace lacks frame events, so no before/after FPS claim
  is made. No capture-device or physical-panel verification was available.
- DDC capabilities-string retrieval and brightness **80 -> 105 -> 80** pass,
  with exact restoration. `GetMonitorCapabilities` retains the known MCCS 2.2
  `0xC02625DC` error; it is not reported as a passing API.
- A scoped warm restart released the BAR1 window, then hit GSP
  `NV_ERR_TIMEOUT` (`0x65`) before another BAR1 mapping. Disable/enable recovered
  normal operation in that initial assessment. The later reset/startup retry
  below removes that manual recovery requirement for the tested timeout.

Only the tested window has hardware evidence; high-offset windows and
cross-4-GiB requests remain software-tested.

This is **not yet direct CPU-to-VRAM presentation**. `osMapPciMemoryUser` and
`osMapPciMemoryAreaUser` remain unsupported, and allocation-aware RM aperture
translation, WC visibility ordering, and readback/image validation remain
necessary. Do not derive a CPU pointer as `BAR1 base + VRAM allocation offset`
or assume the aperture is a permanently linear view of all VRAM.

## PCI reset and startup recovery

The reset-only startup path previously returned a failure even when Windows
successfully reset the device. A deliberate RTX 4070 restart reproduced
**Code 10 / `CM_PROB_FAILED_START`**, no UMDF host, and no NVIDIA desktop monitor
for 60 seconds without external intervention. The GSP result was
`NV_ERR_TIMEOUT` (`0x65`), not `NV_ERR_RESET_REQUIRED`.

The driver now queries the versioned
[device reset interface](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_device_reset_interface_standard),
including bus-specific capabilities and reset status. On this USB4 RTX 4070:

- Standard reset mask `0x1`: function-level reset only.
- PCI bus-specific mask `0x31`: function-level reset, no-op, and function-level
  reset in place. Platform-level, secondary-bus, and power-controller resets
  are not advertised and are not attempted.

After a firmware startup timeout, the driver tears down failed RM state and
its BAR1 leases, requests the advertised in-place function reset with
`KeepStackReset`, and **reattaches/reinitializes RM once**. WDF's interrupt
object, DMA enabler, and fixed BAR0/BAR2 mappings remain intact; RM-owned DMA
allocations and context are rebuilt. Recreating the WDF interrupt during the
same preparation callback caused a verifier failure in an earlier experiment;
the corrected retry avoids that invalid lifecycle.

The retry requires a completed in-place reset, does not loop indefinitely,
and reports reset/reattach/firmware failures explicitly. If in-place reset is
unavailable, the existing standard function-level reset remains the fallback;
that stack-changing path is not treated as an in-process recovery.

**Hardware verification (2026-10-07):** package `oem197.inf`, DLL SHA-256
`BA95F22DF1A898737E4DD7C9DA9FF49A2DAC47BE5523ADE619EA23C52E5E5AC1`,
recovered three actual GSP startup timeouts (activation and two deliberate
restarts). Each trace records timeout, successful in-place reset, RM rebuild,
and successful firmware retry in the same host, without external recovery.
Both restart assessments retained a healthy host for 60 seconds; no new WER
host/verifier reports occurred with the corrected candidate.

The final desktop layout/mode, animation uploads/latches, read-only brightness
(80), and DDC capabilities string pass. This is automatic recovery, **not
uninterrupted display output**: a deliberate restart still removes/reannounces
the monitor and takes several seconds. No camera-based blanking measurement
was available. This recovery covers firmware startup timeouts, not arbitrary
runtime GPU hangs; other GPUs, unsupported reset paths, and a second failed
firmware attempt remain hardware-unverified.

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