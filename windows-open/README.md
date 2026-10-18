# Windows Driver Harness

This directory contains the Windows sidecar projects and platform adapters.
It does not replace NVIDIA's upstream Linux build system. The functional
display path hosts RM, GSP host code, and NVKMS in a **UMDF 2 / IddCx**
driver; the **KMDOD** miniport remains an experimental scaffold with production
RM activation gated off. They are alternative function drivers: install only
one on a given GPU.

This is an independent, experimental **display-only** port, not an NVIDIA
Windows rendering driver. Windows renders on another adapter and supplies
frames through IddCx. The default native D3D12 path copies into imported WDF
common-buffer memory, then NVIDIA's copy engine (CE) uploads to VRAM for
NVKMS scanout. D3D11 staging and D3D12 reference paths are also available.
There is no NVIDIA rendering UMD, CUDA support, or custom kernel transport
companion for the IddCx path.

See the [root README](../README.md) for the feature overview, tested hardware,
compatible-GPU table, and installation/recovery instructions. An INF match or
successful build is not evidence that a GPU or feature has been validated.

## Projects and ownership

| Area | Role |
| --- | --- |
| [iddcx](iddcx/nv-iddcx.vcxproj) | UMDF device lifecycle, PCI resources, IddCx adapter/monitor callbacks, frame acquisition, presentation, hardware cursor, and DDC/CI. |
| [modeset](modeset/nvkms-user.vcxproj) | User-mode NVKMS and the Windows KAPI facade for connectors, modesets, surfaces, color, and flips. |
| [common/kernel](common/kernel/nvcommon-kernel.vcxproj), [common/user](common/user/nvcommon-user.vcxproj) | Shared upstream status/SHA support and a versioned host-services interface for allocation, clocks, context safety, and logging. |
| [nvport/kernel](nvport/kernel/nvport-kernel.vcxproj), [nvport/user](nvport/user/nvport-user.vcxproj) | NvPort source selections and Windows memory, synchronization, thread, time, CPU, and utility backends. |
| [platform/kernel](platform/kernel/nvplatform-kernel.vcxproj), [platform/user](platform/user/nvplatform-user.vcxproj) | OS-specific services, PCI/MMIO, allocation, synchronization, interrupts, and execution-context contracts. |
| [rm/core](rm/core), [rm/kernel](rm/kernel) | Upstream RM/generated sources and WDK-free RM OS-service adapters, with separate user/kernel targets. |
| [rm/diagnostic](rm/diagnostic/nvrm-diagnostic.vcxproj) | Separate kernel RM diagnostic harness; not the production IddCx path. |
| [kmdod](kmdod/nv-kmdod.vcxproj) | Experimental WDDM display-only miniport and kernel resource ownership. |
| [tests](tests) | Standalone native host regressions, not driver binaries. |
| [tools](tools) | Control application and monitor diagnostics. |

Windows adaptations live at explicit sidecar boundaries. Upstream source
selections are recorded in [manifests](manifests) and project items; generated
Windows RM customizations are owned by
[Generate-WindowsRmProfile.ps1](rm/Generate-WindowsRmProfile.ps1).
Do not import Unix kernel backends or hand-edit generated output to satisfy
unresolved contracts. Unsupported services must fail explicitly rather than
publish success, handles, or capabilities they cannot provide. In particular,
privileged CPU facilities, IOMMU SVA, guest-MSI injection, and per-process PCI
user mappings are not general Windows capabilities of this harness.

Each device owns its PCI identity, BARs, DMA, interrupt, RM/NVKMS state, and
outputs. Initialization failures unwind in reverse ownership order. IddCx
creates a device-owned adapter asynchronously; ordinary D0 transitions do not
create duplicate adapters, and no monitor is announced before a real
RM/NVKMS connector/mode snapshot exists. Parent teardown clears callback
ownership before deleting the adapter.

## Prerequisites and build

- Visual Studio 2026 (18.x), Desktop development with C++, WDK integration,
  and the bundled ClangCL toolset.
- Matching Windows SDK and WDK **10.0.28000.0**.
- A target with the required **Windows 11 Insider build 28000** runtime:
  UMDF **2.35**, IddCx **1.11** with a **1.10** minimum, and the native
  D3D12/common-buffer APIs. Installing the SDK does not provide these runtime
  features on an older retail OS.
- Matching NVIDIA GSP firmware for [version.mk](../version.mk). The current
  loader expects `%ProgramData%\nv-iddcx\gsp_ga10x.bin`; the build does not
  download or install it.
- A test-signing environment and a separate rendering-capable adapter with
  a recovery display. Driver replacement can interrupt output.

From the repository root, build IddCx and its RM/NVKMS dependencies:

```powershell
& .\build-windows-iddcx.ps1 -Configuration Debug -Platform x64
```

The [helper](../build-windows-iddcx.ps1) discovers 64-bit MSBuild, restores
packages, and rebuilds the driver. Use `-MSBuildPath` to select an installation.
The RM/NVKMS backend is intrinsic to the project, not an optional build switch.

Alternatively, open [nv-windows-drivers.slnx](nv-windows-drivers.slnx).
From this directory, a full solution build with 64-bit MSBuild is:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  .\nv-windows-drivers.slnx /t:Build /restore `
  /p:Configuration=Debug /p:Platform=x64 /m
```

Projects expose Debug/Release and x64/ARM64 configurations. These are build
targets, not equivalent hardware-validation claims; full ARM64 driver linking
and hardware operation remain unvalidated. Driver binaries retain WDK
toolsets for linking, INF stamping, catalog generation, signing, and packaging;
the portability/RM/NVKMS libraries use ClangCL. The NVKMS x64 shader-pack
custom build explicitly selects `x86_64-pc-windows-msvc`; custom commands must
not rely on the compiler's default architecture.

WIL uses a pinned native NuGet `PackageReference` in the IddCx project, with
`EnableNativePackageReferenceSupport=true`. Use `/restore` for command-line
builds instead of hand-written package-cache paths. Prefer MSBuild for
automated restores/builds; IDE background NuGet errors are not a substitute
for the command's exit status.

WDK builds normally test-sign the DLL and catalog. If changing project
settings, retain **Driver Signing > Sign Mode = Test Sign** and
**Inf2Cat > Run Inf2Cat = Yes**. Follow the root README's certificate-trust,
test-mode, and installation instructions; do not weaken system security
settings without understanding the recovery implications. Copy the whole
generated driver package, not just the DLL, and do not modify signed files or
the INF after catalog generation.

Both INFs retain root-enumerated development IDs and generated NVIDIA PCI
matches from the root compatible-GPU table. Regenerate their model/string
blocks after changing that table. From this directory:

```powershell
& .\Generate-SupportedDeviceInfs.ps1
```

### Build macro policy

| Macro | Purpose | Default |
| --- | --- | --- |
| `NV_WINDOWS_IDDCX` | Windows IddCx integration and Windows-only common-code adaptations. | Defined by IddCx, user-mode RM, and NVKMS projects. |
| `NV_DOWNSTREAM_BUGFIXES` | Portable downstream correctness fixes, including preserving the owning VA reservation after a failed reserved unmap. | On through `NvDownstreamBugfixes=true` in Windows builds; non-Windows builds explicitly opt in. |
| `NV_RM_VERBOSE_DIAGNOSTICS` | Detailed RM engine/FIFO and VA allocation/lifetime breadcrumbs. Windows trace calls also require `NV_WINDOWS_IDDCX`. | Off; set `NvRmVerboseDiagnostics=true` or use the helper's `-VerboseRmDiagnostics`. |

These policies are independent of **Debug/Release** and of `NV_USER_MODE`,
which identifies the execution environment. Assertions, error reporting, and
physical-removal cancellation diagnostics do not require verbose tracing.
Verbose diagnostics never select correctness or recovery behavior.

The Windows profile retains its GSP thread-state timeout adaptation, global
CeUtils suppression, BAR2 shutdown cleanup, and legacy CE timeout fallback.
These are not advertised as portable Linux fixes. The legacy timeout path
can advance completion and invoke callbacks; confirmed physical removal uses
separate cancellation without claiming completion.

`NvDownstreamBugfixes=false` is for upstream-policy comparison tests, not
normal operation: it restores the known reserved-unmap double-free behavior.
Native-policy host tests do not constitute a Linux build or runtime test.

## Live IddCx parameters and hardware-plane experiments

The driver watches DWORD values under its **UMDF framework** key, not the
PnP `WUDFRd` service key:

```text
HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\WUDF\Services\nv-iddcx\Parameters
```

Missing values use defaults. Malformed, incomplete, or unsupported candidates
leave the last running configuration intact. The current schema is defined in
[driver_parameters.hpp](iddcx/driver_parameters.hpp).

| DWORD | Values | Default |
| --- | --- | --- |
| `SwapChainApi` | 0 automatic; 11 D3D11; 12 D3D12 | 0 |
| `D3D12Mode` | 0 off; 1 direct WDF; 2 readback reference; 3 alternating experiment | 1 |
| `D3D11ReadbackMode` | 0 staging/default; 1 experimental `ReadFromSubresource`; 2 alternating experiment | 0 |
| `SurfaceCachePolicy` | 0 automatic (honor for D3D12, not D3D11); 1 ignore; 2 honor | 0 |
| `DirtyRects` | 0 off; 1 damage tracking | 1 |
| `D3D12Debug` | 0 off; 1 D3D12 debug layer | 0 |
| `RealtimeGpuPriority` | 0 off; 1 request realtime scheduling | 1 |
| `MmcssDisplayPostProcessing` | 0 off; 1 display post-processing scheduling | 1 |
| `VrrPreference` | 0 off; 1 prefer VRR when available | 1 |
| `FlipMode` | 0 synchronized/non-tearing; 1 interval-zero/non-tearing, latest ready | 1 |
| `FramePipeline` | 0 serial; 1 overlap producer and CE consumer | 1 |
| `ScanoutBufferCount` | 2-4 | 3 |
| `MpoDemoMode` | 0 Off; 1 Alpha Orbit; 2 Geometry Compass; 3 CSC Color Lab; 4 Luminance Elevator; 5 LUT Gallery; 6 Live Showcase | 0 |
| `MpoDemoConnectorIndex` | Physical connector index 0-15, required while a demo is enabled; not a GDI display number | Absent |
| `MpoDemoPlaneCount` | 1-7 additional planes, subject to the active head's layer budget | 1 |
| `MpoDemoCadenceHz` | 15, 30, or 60, not above the active refresh rate | 30 |
| `MpoDemoSpeedPercent` | 25-200 | 100 |
| `ConfigRevision` | Odd while editing; stable even value commits | 0 |

Explicit D3D11 overrides `D3D12Mode`. Explicit D3D12 with mode 0 resolves to
direct WDF; automatic API with mode 0 selects D3D11. The legacy
`HKLM\SOFTWARE\NVIDIA Corporation\nv-iddcx\ReadbackMode` is a fallback only
when the framework `D3D11ReadbackMode` value is absent.

For grouped writes, set `ConfigRevision` odd, write the complete candidate,
then commit with the next even revision. Odd or changing revisions are never
applied. Unversioned edits require two matching reads separated by a settling
interval. Normalized equivalent settings and settings for an inactive source
API do not recreate the source. An explicitly committed new even revision can
retry a latched native-source failure once; healthy equivalent commits remain
no-ops.

Accepted changes retire/recreate the source swapchain through IddCx without
restarting the device. Buffer-count or cache-policy changes stage a replacement
scanout pool, retaining the old front allocation until its replacement latches.
Acceptance is not proof of application: the
[IddCx Control application](tools/iddcx-control/README.md) reads accepted and
per-output applied generations from the local `NvIddCx.Status` pipe.
It starts read-only; Apply uses a guarded, elevated registry transaction.
Publish/copy the complete NativeAOT output folder, not only the executable.

`VrrPreference` is best effort. Live changes use a same-head modeset and
check the actual NVKMS reply. An unavailable enable preference keeps VRR off
and reports a warning while other valid settings can apply. Failed rollback
blocks presentation and retains scanout leases for recovery. Requested and
effective VRR are reported separately.

### Demo capability and retirement rules

Both source APIs share the presentation coordinator. Off retains normal
presentation; demos are opt-in and require actual capabilities and test-only
NVKMS validation on the selected active output.

- Software generates source cards; NVKMS performs the real plane composition,
  supported transforms, CSC/gain, and independent input LUT. Unsupported
  features do not fall back to software composition or head-wide gamma.
- The desktop consumes one hardware layer. Each extra layer has independent
  buffer/LUT ownership and uses its own pending/timestamp queries for
  retirement. Packed layers use a neutral input LUT unless a custom one is
  requested.
- Disabling a layer submits a null surface and the active primary together in
  an atomic non-tearing update. Actual primary completion witnesses retirement;
  leases remain held until then. Overlay-only updates reuse desktop content
  without inventing an IddCx frame completion.
- Live Showcase skips unsupported chapters and reports omissions. Unsupported
  standalone modes reject the candidate. Geometry Compass adapts to supported
  transforms and finite test-only-validated scales, reporting an adaptation
  warning and omitted states. `rotation=false` means full quarter-turn support
  is unavailable, not that every rotation is unavailable.
- Cropping reduces source extents from the origin; nonzero source offsets are
  rejected. Combined CSC/gain chapters keep native size when the scaling
  pipeline is occupied; geometric scaling is demonstrated separately.
- QPC absolute deadlines and a high-resolution waitable timer skip missed slots
  rather than burst. Requested cadence is not a guaranteed displayed rate.

## Hardware cursor and frame-upload synchronization

[`PresentationLocks`](modeset/presentation_locks.hpp) separates device
lifetime, CE transfers, display programming, and presentation transactions:

- Uploads, cursor updates, presentations, buffer lookups, and notifier reads
  hold shared lifetime protection. Initialization, shutdown, external surface
  allocation/destruction, modesets, and monitor control use exclusive
  protection.
- CE transfers serialize because KAPI caches one copy-channel handle.
  Position-only cursor updates do not acquire that lock.
- Cursor programming, flip programming, and notifier reads exclude each other,
  but programming exclusion is not held across ordinary frame uploads.
  Cursor-image uploads finish before programming exclusion is acquired.
- Presentations serialize across preflight, upload, and flip; RAM/VRAM leases
  prevent writing front or pending storage. Cursor programming may overlap
  the upload phase.

Acquire lifetime first, then the presentation transaction when needed, then
programming or copy exclusion. LUT/gamma operations may take copy exclusion
while holding programming exclusion; the reverse nesting is prohibited.
Common NVKMS ioctl/timer locking is unchanged. Cursor-image uploads still
share CE bandwidth, and allocation/modeset work remains exclusive; these
operations are not made asynchronous by the lock split.

## BAR1 resources and bounded mappings

`ConfigSpace` owns PCI configuration/address discovery; `PciBar`/`PciBars`
resolve assigned resources and own mappings; `RegisterAperture` supplies WDF
mapping/access callbacks. The first three valid memory BARs are indexed in
register/framebuffer/instance-memory order, not PCI configuration-DWORD order.

- Ordinary memory and `MEMORY_LARGE` 40/48/64-bit lengths use checked 64-bit
  decoding. PCI addresses match raw resources and the corresponding translated
  resource; absent, inconsistent, or ambiguous assignments fail explicitly.
- BAR0 is eagerly mapped noncached; BAR2 is eagerly mapped write-combined.
  BAR1 retains its full physical address/length and is demand-mapped
  write-combined.
- UMDF allows **one active mapping per assigned CM resource**, not one per
  disjoint subrange. BAR1 aliases share a page-aligned CPU window covering the
  initial request, capped at `0xFFFFF000` bytes and rounded down for the system
  page size. Contained requests reuse it.
- A request outside a leased window returns `NV_ERR_STATE_IN_USE`. After the
  last alias is released, another window may be selected, including at high
  offsets or across a 4-GiB boundary. Oversized, out-of-resource, incompatible
  cache requests and exhausted registry slots fail explicitly. The mapping
  registry has 32 slots shared with fixed mappings.
- Noncopyable owners, smart-owned mappings, and RAII registration handles keep
  registrations alive only while their backing resources exist. Hardware
  release stops RM/NVKMS before draining mappings and releasing BARs.
  Startup recovery suspends/restores demand providers without rebuilding fixed
  mappings, WDF DMA, or interrupt objects.

The WDF backend retains framework pseudo addresses and mapped process
addresses; register access remains framework-mediated. Demand maps are
permitted only while the assigned-resource provider is registered, at
`PASSIVE_LEVEL`; see the WDF
[map](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfdevice/nf-wdfdevice-wdfdevicemapiospace)
and [unmap](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfdevice/nf-wdfdevice-wdfdeviceunmapiospace)
contracts.

This is **not direct CPU-to-VRAM presentation**. `osMapPciMemoryUser` and
`osMapPciMemoryAreaUser` remain unsupported. BAR1 is not a permanently linear
view of VRAM: never derive an allocation pointer as aperture base plus VRAM
offset. Allocation-aware translation, WC ordering, and image validation remain
required. The common-buffer/CE presentation path introduces no per-frame BAR1
mapping.

## PCI reset and startup recovery

The driver queries the versioned Windows
[device reset interface](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_device_reset_interface_standard)
and attempts only advertised reset capabilities.

A firmware-startup `NV_ERR_TIMEOUT` on a present device tears down failed RM
state and BAR1 leases, requests an in-place function reset with `KeepStackReset`,
and reattaches/reinitializes RM **once**, only after that reset completes.
Fixed BAR0/BAR2 mappings, the DMA enabler, and WDF interrupt object remain
intact; RM-owned allocations/context are rebuilt. If in-place reset is
unsupported, standard function-level reset is a stack-changing fallback,
not successful in-process recovery. Reset, reattach, and retry failures remain
explicit, and removed devices are never reset.

This covers startup timeout recovery, not arbitrary runtime GPU hangs or
uninterrupted output. Restart can remove/reannounce monitors. Other reset
paths and repeated-cycle reliability require hardware-specific validation.

## USB4 enclosure surprise removal

`EvtDeviceSurpriseRemoval` latches terminal per-device removal and cancels
initialization/subsequent D0 entry. It does not wait for workers, take the
lifecycle lock, or dereference hardware owners: WDF may call it concurrently
with preparation/release or after resources have been released.

- PCI requests return `STATUS_DEVICE_REMOVED`; register reads return all ones
  and writes are suppressed. Interrupt disable/callbacks no longer call RM
  for absent hardware. Interface references, mappings, DMA, and workers still
  follow normal software cleanup.
- Release marks RM disconnected under its normal locks before output cleanup.
  Plane-disable/retirement waits, all-heads-off modesets, cursor hides, and
  native notifier waits cancel only with explicit physical-removal proof.
  The predicate stays registered through RM detach and is cleared before
  its device context is released.
- Resource-tree, memory, and VA cleanup may accept firmware-free
  `NV_ERR_GPU_IS_LOST` on confirmed removal while releasing local objects and
  preserving the error. A fault on a present GPU, an unavailable predicate,
  unrelated errors, and software-ownership assertions remain strict.
- Failed reserved unmaps preserve the owning virtual-memory reservation for
  its destructor. Cleanup must not double-free client-owned VA.
- CE/channel teardown discards pending callbacks without invoking them or
  advancing completion. PMA invalidates the scrubber but preserves scrub-on-free
  policy: abandoned work is not certified clean and no clean-page list is
  published. Local descriptors, lists, and locks are still released.
- Graphics teardown releases local buffers without cache eviction on absent
  hardware. Healthy eviction and non-removal error checks remain unchanged.

Cancellation is not successful hardware completion. These rules do not relax
live-device retirement, timeout, or assertion policy, and do not establish
general hotplug/power or soak reliability.

## Monitor control (I2C/DDC/CI)

IddCx exposes transmit, receive, and availability-gated combined callbacks.
NVKMS resolves the current display/connector per request and permits only
DDC/CI endpoint `0x37`; Windows directional `0x6E`/`0x6F` addresses normalize
at the IddCx boundary. Arbitrary board, EDID, and HDCP bus access is excluded.

- **DisplayPort/USB-C:** RM I2C-over-AUX, 16-byte chunks, repeated-start/MOT,
  terminal STOP, bounded DEFER/write-status retries, staged receive publication,
  and length-prefixed replies.
- **HDMI/DVI/VGA:** RM indexed I2C, fixed-length transmit/receive, and combined
  requests with at most four transmit/index bytes. Unsupported length-prefixed
  receives or larger prefixes fail before bus traffic.
- **Limits:** 64 transmit and 256 receive bytes per callback. Internal displays
  and DP MST are unsupported; KMDOD's OS-facing I2C interface is not implemented.

Transactions retain a WDF power reference and serialize with EDID, hotplug,
link training, and other NVKMS operations through existing device/recursive
locks. This is global, not per-connector concurrency. Lock acquisition and
software retries are bounded; synchronous RM calls retain RM hardware timeouts.
Disconnect, power loss, and teardown return explicit errors.

From the repository root, build and run the Win32 diagnostic tool:

```powershell
& .\windows-open\tools\build-monitor-ddc.ps1
$ddc = '.\windows-open\out\x64\monitor-ddc\monitor-ddc.exe'
& $ddc --list
& $ddc --display '\\.\DISPLAYn' --capabilities-string
```

Replace `DISPLAYn` with the intended NVIDIA-connected display from `--list`;
numbers may change after restart. Run in the logged-in interactive desktop,
not session 0, SSH, or RDP. Default operation is read-only. Windows may reject
an advertised MCCS version through `GetMonitorCapabilities` even when brightness
and capabilities-string operations work; do not rewrite monitor data or report
that API as passing.

An explicit write exercise for manual OSD observation is:

```powershell
& $ddc --display '\\.\DISPLAYn' --brightness-only --exercise --hold-ms 10000
```

It changes brightness by about 5% of the reported range, checks readback, then
restores/verifies the original. Killing the tool or unplugging can prevent
restoration. Writes require an explicit display; `--physical N` selects the
physical-monitor index (default 0), and hold time is capped at 60 seconds.
`--brightness-only` explicitly skips the capabilities API. Exit codes are
0 success, 1 API/verification failure, and 2 invalid arguments.

## Native host tests

The 16 standalone console `.vcxproj` projects under [tests](tests) appear in
the solution's **Host tests** group. They do not link into drivers or require
a GPU. A normal solution `Build` builds default profiles but never runs tests,
installs drivers, or performs hardware operations.

[host-test.props](tests/host-test.props) pins the official
`Microsoft.googletest.v140.windesktop.msvcstl.static.rt-dyn` **1.8.1.8** native
`PackageReference`. Despite its historical name, it supports MSVC **v145** and
x64/ARM64 Debug/Release libraries; ClangCL projects use its force-enable
property. NuGet supplies headers, `gtest`, and `gtest_main`, with no vendored
sources or hard-coded package-cache paths. Fixtures using WDF/WIL restore
the pinned WIL package as needed.

From the repository root:

```powershell
& .\windows-open\tests\run-host-tests.ps1 -Configuration Debug -VerifyBaseline
& .\windows-open\tests\run-host-tests.ps1 -Configuration Release -VerifyBaseline
```

Select `-Suite Presentation`, `BarResources`, `MonitorI2c`, `Removal`, `Modeset`,
or `Platform`; `-Filter` accepts a GoogleTest filter. The six focused runner
scripts in the IddCx/modeset/platform test directories remain compatibility
entrypoints to this shared runner. Tool discovery supports `-VisualStudio`
or `-VcVarsPath`, plus `-WdfIncludePath` and `-WilIncludePath` overrides.

Each project has an explicit MSBuild `Test` target:

```powershell
MSBuild.exe .\windows-open\tests\dirty-regions-tests.vcxproj /restore /t:Test /p:Configuration=Release /p:Platform=x64
```

Executables and XML reports are under
`windows-open\out\<Platform>\<Configuration>\tests\<profile>\` (reports in
`results`). Per-project intermediates, generated snippets, and NuGet assets
are isolated under `windows-open\out\obj`. Executables accept
`--gtest_list_tests` and `--gtest_filter`; the MSBuild target also accepts
`/p:NvTestFilter=...`. Use `-Platform ARM64 -BuildOnly` to cross-compile
without executing ARM64 tests on an x64 host.

The removal matrix preserves Windows/native policy, verbose diagnostics
on/off, and downstream-fix on/off profiles. Applicable projects accept
`/p:NvTestProfile=diagnostics`, `native-policy`, `native-diagnostics`,
`downstream-disabled`, or `native-downstream-disabled`. GSP timeout tests
also retain a native-policy variant. `-VerifyBaseline` adds `baseline` and,
where applicable, `dma-baseline` profiles using the generator's pinned
pre-fix sources. They positively assert reproduction; a nonzero process exit
is never a successful baseline test. Profile outputs/assets do not share
incompatible objects.

These are white-box unit/component regressions, not GPU emulation. Production
headers/full files are used where practical; lifecycle/firmware body extraction
is an explicit generated step with checked boundaries. Deterministic loops
and fake-boundary fault injection do not establish hardware behavior, and
looped assertions are not independent GoogleTest case counts. Control-app
model tests remain separate; see its README.

## Diagnostics and limitations

The IddCx WPP provider mirrors RM platform output, including Xid, assertion
frames, and fail-fast codes. Enable `TRACE_DRIVER` at informational level or
higher; payloads retain the original platform level. Decode with matching
build symbols and resolve addresses with the exact binary's PDB. Optional
verbose RM tracing works in either build configuration and does not bypass
interlocks, hardware completion, or recovery checks.

Cursor timing events include QPC begin/end, lifetime/programming waits, image
upload time, and result. Compare them with pipeline upload/latch intervals;
event gaps without cursor motion are not an input-latency measurement.
For reports, include the source revision, build/profile, OS/kit, GPU/output
topology, requested settings and driver-reported applied/effective state,
reproduction steps, matching-symbol diagnostics, and trace loss/decode errors.
Separate API/readback observations from physical-output verification.

Important limits remain:

- Source recreation can grow shared synchronization-handle usage; allocation
  ownership and long-run reliability remain unresolved. Do not close borrowed
  or opaque GPU handles speculatively.
- Full-frame high-resolution/FP16 transfers remain bandwidth-sensitive.
  Upload/latch counts, application paints, and requested demo cadence do not
  prove displayed FPS, panel latency, or uninterrupted output.
- VRR is capability-gated, not generally hardware-certified. HDR absolute
  luminance/cursor calibration, broad GPU compatibility, sleep/resume,
  repeated hotplug, long-run reliability, and HLK certification remain open.
- High-offset/cross-4-GiB BAR1 mapping tests use software backends, not general
  hardware validation. Full-aperture simultaneous CPU windows and direct
  CPU-to-VRAM presentation are unsupported.
- Native HDMI/DVI/VGA DDC, positive combined/length-prefixed hardware requests,
  transaction-time unplug/timeout/power behavior, and physical OSD changes
  need separate hardware validation. Host tests do not certify these paths.
- KMDOD does not inherit IddCx functionality or acceptance, and native-policy
  tests on Windows are not Linux runtime validation.
