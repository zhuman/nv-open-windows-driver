# NvIddCx Display Lab

A compact, dark/mint **WinUI 3 C# NativeAOT** control application. Unpackaged, read-only on startup, with editable swapchain and MPO demo drafts. No driver source, service, global solution, deployment script, or Desktop location is changed by this project.

## Build and run

Requirements: Windows 10 2004+; .NET **10** SDK; Visual Studio C++ x64 build tools and Windows SDK. ARM64 hosts need the ARM64-hosted x64 cross tools (the helper selects `arm64_x64`).

From this folder:

```powershell
.\Publish.ps1 -Smoke
# Or just the production-model tests:
dotnet run --project .\Tests\NvIddCx.Control.Tests.csproj -c Release
# Start the published application:
.\artifacts\publish\win-x64\NvIddCxControl.exe
```

Pinned NuGet packages: **Microsoft.WindowsAppSDK.WinUI 1.8.260224000** and Windows SDK BuildTools **10.0.26100.7705**. The WinUI component replaces the aggregate Windows App SDK package, retaining the previously resolved WinUI version without pulling in unrelated AI, ML, Widgets, and DWrite components. Checked-in lockfiles make the helper's restores repeatable. This standalone subtree has its own build properties; all products stay under this folder.

The helper runs the real model tests, publishes `Release / win-x64 / PublishAot=true / WindowsPackageType=None / SelfContained=true / WindowsAppSDKSelfContained=true`, and verifies the native PE and required runtime assets. `EnableMsixTooling=true` is necessary to generate the app's PRI even though the app is **not packaged**. Disabling it builds successfully but leaves XAML styles unavailable at launch.
Each publication replaces only this tool's `artifacts\publish\win-x64` output so removed package assets cannot survive from an earlier build. Close any local app using that output before publishing; a locked-file failure is reported rather than ignored.

The tests also compile the **production Apply broker** and exercise its request creator/reader using local files only, without invoking the registry transaction or requesting elevation. They cover user-owned private ACLs, the Administrators-owned descriptor that caused elevated-GUI Apply failures, trusted names/paths, directory junctions, exclusive sharing, size bounds, JSON/age validation, expected snapshots, and distinct rejection messages. When run from an already-elevated local shell, the same suite additionally verifies the old plain-file creation really uses the token's non-user default owner and is rejected, while the production creator succeeds. Test fixtures live under this folder's `artifacts` when invoked by `Publish.ps1` and are removed.

**Deliver the entire `artifacts\publish\win-x64` folder**, not just the executable. WinUI DLLs, app PRI, and runtime assets must remain beside it. This is not a verified single-file executable. The `.pdb` is an optional debugging artifact, not required to run.

The clean October 12 WinUI-only publication contains 240 files and approximately
96.0 MB, including debugging/support files, versus 287 files/146.9 MB with the
aggregate SDK (about 35% fewer bytes). Native-PE verification, 442 model/transport
assertions including 38 request-file checks, and the local three-tab UI smoke
passed. This package-only build has not replaced the already deployed fixed app
and does not establish interactive UAC acceptance.

Publishing also bundles this README and `Test-ApplyHelper.ps1`; it never executes the opt-in Apply helper smoke.

`artifacts\publish.log`, `native-aot-proof.txt`, `smoke.txt`, and `local-ui.png` contain local validation evidence. `Smoke.ps1` uses Windows PowerShell's UI Automation, exercises all three tabs, checks a bounded window response and the visible telemetry state, and closes only the app it launched. It never invokes Apply. `-ProcessId <pid>` can inspect an already-running local instance without closing it.

### Executable arguments and authorized target smoke

The exact basename is **`NvIddCxControl.exe`**:

| Arguments | Meaning |
|---|---|
| none | Normal GUI; registry/pipe reads only until an explicit Apply action. |
| `--apply-request <owned GUID.json path>` | Short-lived guarded helper; requires an elevated token, known values and exact expected snapshot. No GUI. |
| any other arguments | Rejected with exit 64. **No `--self-test` or `--read-only` switch exists.** |

Production self-tests are the separate `Tests` project, not an executable argument. `Smoke.ps1` is read-only local GUI validation; it does not test Apply.

`Test-ApplyHelper.ps1` is an **opt-in target support script** bundled beside the published executable. It is **never invoked by publishing or the local tests**. Only run it inside the user's expressly authorized **exclusive NVIDIA window**, never while AMD or another configuration writer owns the target. In an **already elevated SSH session**, use Windows PowerShell:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Test-ApplyHelper.ps1 `
    -Exe .\NvIddCxControl.exe -ExclusiveNvidiaWindowAuthorized
```

It refuses absent/invalid/incomplete configuration, builds private owned requests, invokes the real helper **without `runas`**, changes only `MpoDemoSpeedPercent` by one valid step, verifies the even revision and exact values, then restores the original absent/present shape through a second guarded helper transaction. It preserves legacy data and leaves `ConfigRevision` monotonically advanced by **4**, not reset to its old value. There are two maximum-ten-second helper waits under a thirty-second overall helper-launch/wait budget; a timed-out helper is stopped by its specific PID and may leave an odd revision. Failure does **not** trigger automatic recovery or blind restoration. Review any unverified restoration explicitly. No API, demo mode, connector, service, Desktop, or driver files are changed by this smoke.

**An elevated SSH helper round-trip does not exercise interactive UAC consent.** To test that separately, launch the no-argument app from the user's **unelevated interactive Desktop session**, edit one field, click Apply, approve the same-user UAC prompt, verify the registry-write result, then restore the prior choice using another Apply. Observe actual pipe status separately: a registry success is not driver adoption, `accepted` is not `applied`, and active output with `appliedGeneration=0` may still await its first frame.

The UI displays reported `layers/rotation/lut/gain` and optional `vrrEnabled` as actual state. There are no explicit `csc` or `pixelAlpha` fields in the agreed JSON schema; those capabilities are not inferred. Unsupported combinations must be communicated through the driver's visible rejection status/message.

### Hardware demo acceptance (2026-10-12)

All six modes and Off applied on both source APIs in one unchanged driver
host on the tested RTX 4070. Geometry Compass now adapts to supported
transforms and finite test-only-validated scales, as explicitly approved.
This head has one demo plane, supports 0/180 degrees and reflected-X variants,
and validated unity scale only. The adaptation is reported in driver status;
exact omitted transforms/scales are logged in the trace. `rotation=false`
means the complete quarter-turn set is unavailable, not that 180-degree
rotation cannot be used. Source cropping reduces origin-anchored extents;
nonzero overlay fetch offsets are not supported.

Live scene-to-scene and scene-to-Off retirement passed without PnP, and an
over-budget plane request retained the running scene. These tests used
scripted transactions, not an unelevated interactive Apply/UAC round trip.
Requested cadence acceptance does not establish achieved cadence. Subsequent
high-resolution deadline tests measured approximately 15.0/30.0/58.8 Hz
unique layer retirements for the 15/30/60 Hz requests, over 36 seconds each.
Source-recreation handle growth was narrowed to three shared GPU
synchronization handles per recreation; ownership remains under
investigation. The final coordination handoff restored defaults/Off; the
modes remain opt-in.

## Configuration contract

The only writable location is **Registry64 HKLM**:

```text
SOFTWARE\Microsoft\Windows NT\CurrentVersion\WUDF\Services\nv-iddcx\Parameters
```

This existing key is framework managed. The app **does not create it**, does not use `WUDFRd`, and never starts/stops/restarts a service. If the key is absent, drafts remain editable, but Apply is disabled.

Each setting has a **Driver default** choice that means *absent*, distinct from an explicit DWORD equal to the default. Merely loading, selecting a tab, choosing a preset, or editing a draft writes nothing. “Driver defaults” explicitly drafts removal of known values; the Apply confirmation lists those removals. Presets patch only their own fields and preserve every other present/absent value.

| DWORD | Accepted values | Absent default |
|---|---|---|
| SwapChainApi | 0 Auto, 11 D3D11, 12 D3D12 | 0 |
| D3D12Mode | 0 Off, 1 Direct WDF, 2 Readback reference, 3 Alternating experiment | 1 |
| D3D11ReadbackMode | 0–2 | Legacy ReadbackMode, then 0 |
| SurfaceCachePolicy | 0–2 | 0 |
| DirtyRects | 0/1 | 1 |
| D3D12Debug | 0/1 | 0 |
| RealtimeGpuPriority | 0/1 | 1 |
| MmcssDisplayPostProcessing | 0/1 | 1 |
| VrrPreference (“Prefer VRR”, best effort) | 0/1 | 1 |
| FlipMode | 0/1 | 1 |
| FramePipeline | 0/1 | 1 |
| ScanoutBufferCount | 2–4 | 3 |
| MpoDemoMode | 0 Off, 1 Alpha Orbit, 2 Geometry Compass, 3 CSC Color Lab, 4 Luminance Elevator, 5 LUT Gallery, 6 Live Showcase | 0 |
| MpoDemoPlaneCount | 1–7; driver runtime cap gate | 1 |
| MpoDemoConnectorIndex | 0–15; **required explicitly when demo > 0** | absent |
| MpoDemoCadenceHz | 15, 30, 60 | 30 |
| MpoDemoSpeedPercent | 25–200 | 100 |

Connector index is the **driver connector index**, not a GDI display number. Explicit API 11 makes D3D12 mode effectively 0 without deleting the stored selection. Explicit API 12 plus mode 0 resolves to effective mode 1. The UI labels effective configuration as *requested*, never as applied.

When framework `D3D11ReadbackMode` is absent, the reader consults **Registry64 HKLM\SOFTWARE\NVIDIA Corporation\nv-iddcx\ReadbackMode**, with the same 0–2 bounds, and then defaults to 0 if both are absent. A present framework value always wins, even an explicit 0. The legacy key is **read-only**; Apply can only set/delete the framework value. The snapshot and expected-value comparison include the legacy fallback so a concurrent legacy change causes a conflict rather than a silent override. Invalid effective legacy data is reported, not substituted.

Wrong registry kinds and out-of-range DWORDs are visible errors and block Apply. They are **not silently repaired or replaced with defaults**. Correct such pre-existing invalid data explicitly outside this tool. No friendly meanings are invented for the unspecified D3D11 readback/cache policy modes.

## Guarded Apply and recovery

The normal GUI never requests lasting elevation. Apply confirms the exact changed fields, creates a maximum **64 KiB**, GUID-named, UTF-8 JSON request in the current user's private `%LOCALAPPDATA%\NvIddCxControl\Requests` directory, then launches **this same executable** once with:

```text
--apply-request <owned request path>
```

The request file is created with an **explicit current-user SID owner and a protected ACL** granting only that user, Administrators, and SYSTEM access. This works even when the GUI is already elevated: a plain file creation can otherwise use the token's **Administrators default owner**, which the helper correctly rejects. Existing directory ownership/ACLs are never silently repaired, and Administrators ownership is not accepted as a substitute for user ownership.

The elevated helper does not create a window. It rejects other arguments, paths, reparse-point paths, non-owned request files, untrusted directory **or file** write ACLs, requests older than ten minutes, unknown keys, bad types/ranges, invalid revisions and missing required connectors. It opens the file once without sharing, checks the file owner/ACL through that same handle, then reads and validates the request. UAC under a *different administrator identity* is deliberately rejected because that identity does not own the request directory. Cancellation writes nothing. Requests are deleted by the GUI caller in `finally`.

Transport failures have distinct exit codes and GUI explanations: **65** trusted path/name, **66** reparse point, **67** directory owner, **68** directory write ACL, **69** file owner, **72** file write ACL, and **76** size (1–65536 bytes). **73** remains a configuration conflict, **75** writer contention, and **77** missing helper elevation. Do not bypass ownership, ACL, or reparse checks to resolve a rejection.

### Ownership-fix production evidence (2026-10-11)

The initial two parent-run **already-elevated interactive GUI** tests exercised the real Apply button and production request writer, not a manually prepared request. Both main-GUI tokens had **Administrators as their default owner**. Each first Apply changed only Demo speed to 101 while Demo remained Off, created a **user-SID-owned request with a protected, trusted write ACL**, and reported GUI success with the exact even revision advanced by **2**. This verifies the producer ownership fix under the formerly failing token condition.

The restore portions of those initial automation attempts failed in the helper; the parent reviewed and restored the original absent speed shape explicitly through its parameter journal/transaction. Those first-only results are separate from the successful final regression below.

The **third parent-run production-GUI regression passed in 4691 ms** (`live-mpo\gui-apply-result-3.json`). Its first Apply advanced revision **34 → 36**; a fresh owned GUI then selected the speed field's original **Driver default** option and performed a genuine restore Apply, advancing **36 → 38**. Both phases verified GUI success, exact registry snapshots, and user-SID-owned/private-ACL request files. The second Apply restored the original **absent** speed value, preserving the other known values and legacy fallback while Demo remained Off. The result recorded `restoredOriginalShape=true`, `parentJournalReviewRequired=false`, and task exit **0**. This is a verified production Apply plus **absence-preserving GUI restore** round trip, not merely a first-Apply ownership check.

**Interactive UAC consent remains unproven:** all of these test GUIs and their helpers were already elevated. The successful round trip does not establish an interactive UAC-prompt path.

During this deployment an older user-launched GUI (PID **6988**) remained loaded and blocked in-place executable replacement. It was left untouched. The parent deployed and hash-verified all **287** fixed published files to the separate `Desktop\NvIddCx-Control-Fixed` folder. Its executable SHA256 is `488F052879C2307E081ECE7556257FD1A22D0DA4EFAF7B58AA9011784648C9F7`; the older publication was `A296369CA1C56BBEA562C428418F30D8A85B7536EA095358BF0C7CDF0BD88178`. Do not overwrite a loaded user application or stop it merely to update a test copy.

**Cooperative multiwriter protocol:**

1. Hold `Global\NvIddCx.Control.ConfigWriter.v1` across the entire transaction (five-second bounded acquisition).
2. Re-read the revision and every known value. Require exact agreement with the GUI snapshot, including absence/type. A conflict before this point writes nothing.
3. Treat absent `ConfigRevision` as 0. Write current even + 1 (odd), flush, write/delete only changed known values, flush, verify all known values and the odd revision, write even + 2, flush.
4. Readers accept an unchanged even revision as committed. An unchanged odd revision is **incomplete**, not an applied configuration.

The named mutex prevents competing app helpers from tearing writes. Every other configuration writer must use this same mutex/revision protocol; registry APIs cannot atomically compare-and-swap a set of values against arbitrary non-cooperating writers. Revision/value rechecks detect observable interference and **never fabricate completion**. Unrelated values are never enumerated for writing, deleted, or reset.

Failure during a group leaves its odd revision visible. **Recover incomplete write** is a separate UAC-confirmed action: review the currently stored valid values, retain them, then advance odd + 1 to even. This is *not a rollback*. Recovery compares the complete expected snapshot under the mutex and refuses invalid data, missing demo connector, or revision overflow. There is no automatic recovery or wraparound.

A helper exit 0 is followed by another unelevated read to verify the committed revision and values. Even a verified registry commit is not proof of driver adoption.

## Live status and capabilities

Every three seconds, an inbound-only `NamedPipeClientStream` connects to `NvIddCx.Status`, reads **one UTF-8 JSON newline**, and disconnects. Connect and read share a **1.8-second deadline** and a **64 KiB bound**. Missing pipes, denied access, timeout, malformed/oversized data, and premature disconnect are displayed as **Not connected**. Failed polls clear current output/cap data; the last successful timestamp, if any, is explicitly stale. Nothing is fabricated. A driver `rejected` status is highlighted in red alongside its actual rejection message; `accepted` is never relabeled `applied`.

Supported driver payload:

```json
{
  "generation": 4,
  "revision": 8,
  "status": "Ready",
  "message": "Driver supplied text",
  "outputs": [
    {
      "connectorIndex": 0,
      "active": true,
      "appliedGeneration": 3,
      "api": 12,
      "demo": 6,
      "layers": 2,
      "rotation": false,
      "lut": true,
      "gain": false,
      "vrrEnabled": false
    }
  ]
}
```

The model uses nullable fields: missing fields render **unavailable**. Additional fields are tolerated. Generation and per-output appliedGeneration are displayed separately; `rotation/lut/gain` are **actual-state flags, not capabilities**.

Optional `outputs[].vrrEnabled` is a nullable actual-state boolean. Each output diagnostic shows **VRR on**, **VRR off**, or **VRR unreported** when absent/null. The **Prefer VRR** registry setting remains a best-effort request; its default or explicit On value never implies VRR is active. An `applied` driver message is displayed verbatim, including warnings that VRR is unavailable or unsupported Showcase chapters were skipped. Actual VRR reporting does not introduce a new capability gate or reject an otherwise valid preference.

**Capability limitation:** the agreed payload above does not contain capability fields. In that case, all selection options remain available and the UI says “Driver decides unsupported combinations.” It never infers capability support from applied flags. An optional additive `caps` object can be understood if a future driver explicitly supplies it:

```json
{"caps":{"supportedApis":[11,12],"supportedDemos":[0,1,2],"maxDemoPlanes":2}}
```

Only explicit caps disable unsupported dropdown entries and block incompatible drafts. No driver/schema change is required for the base payload.

All request/status JSON serialization uses **source-generated System.Text.Json metadata** with reflection serialization disabled. The UI is constructed directly with typed controls, with no reflection-based bindings.

## Validation scope

The executable model test project exercises production classes: valid/invalid strict unsigned parsing, defaults, sparse enum ranges, API effective rules, demo connector requirements, preset absence shape, typed JSON roundtrips, revision overflow/grouping, exact expected snapshots, deletions, stale/multiwriter conflicts, partial-write recovery, and missing/invalid status fields. Real local test pipes with randomized names also cover UTF-8 JSON newline reads, preserved rejection messages, actual API/head layers, malformed/oversized records, premature disconnect, and bounded stalled reads. Tests use an in-memory implementation of the production registry interface, never HKLM or the driver's live pipe.

Local verification uses .NET SDK **10.0.401**, VS **2026** / MSVC **14.51.36231**, and Windows **10.0.28000** on ARM64 with x64 emulation. Build/publish and a responsive native UI are local-only verification. Real driver adoption, hardware caps, UAC writes, target deployment and Desktop synchronization remain integration tasks; this tool does not perform them.

Official toolchain references:

- [Windows App SDK self-contained deployment](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/self-contained-deploy/deploy-self-contained-apps)
- [Microsoft WinUI Gallery project](https://github.com/microsoft/WinUI-Gallery/blob/main/WinUIGallery/WinUIGallery.csproj)
- [.NET NativeAOT deployment](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/)
