using System.Text;
using System.Text.Json;
using System.IO.Pipes;
using System.Diagnostics;
using NvIddCx.Control.Core;

int assertions = 0;
void Check(bool condition, string name)
{
    assertions++;
    if (!condition) throw new Exception($"FAIL: {name}");
}
void Throws(Action action, string name)
{
    try { action(); }
    catch (Exception e) when (e is InvalidDataException or InvalidOperationException or IOException)
    { assertions++; return; }
    throw new Exception($"FAIL: expected exception: {name}");
}
ConfigSnapshot Snapshot(uint? revision = null, Dictionary<string, uint?>? values = null, RegistryDatum? legacy = null)
{
    values ??= Schema.Defaults();
    return new()
    {
        KeyExists = true,
        Revision = revision is uint n ? RegistryDatum.FromDword(n) : RegistryDatum.Absent(),
        LegacyReadback = legacy ?? RegistryDatum.Absent(),
        Values = values.ToDictionary(p => p.Key,
            p => p.Value is uint v ? RegistryDatum.FromDword(v) : RegistryDatum.Absent())
    };
}

var defaults = Schema.Defaults();
Check(Schema.Validate(defaults).Count == 0, "all absent defaults are valid");
Check(defaults.Values.All(v => v is null), "defaults do not materialize DWORDs");
Check(Schema.Effective(defaults, "ScanoutBufferCount") == 3, "triple buffer default");
Check(Schema.Effective(defaults, "MpoDemoCadenceHz") == 30, "cadence default");
Check(Schema.Effective(defaults, "DirtyRects") == 1, "dirty rect default");
foreach (var spec in Schema.Settings)
{
    foreach (var choice in spec.EditorChoices().Where(c => c.Value is not null))
        Check(Schema.TryParse(spec, choice.Value!.Value.ToString(), out _), $"{spec.Name} valid option");
    Check(!Schema.TryParse(spec, "-1", out _), $"{spec.Name} rejects negative");
    Check(!Schema.TryParse(spec, "4294967296", out _), $"{spec.Name} rejects overflow");
    Check(!Schema.TryParse(spec, "1.5", out _), $"{spec.Name} rejects fractions");
    Check(!Schema.TryParse(spec, " 1", out _), $"{spec.Name} rejects whitespace");
    Check(!Schema.TryParse(spec, "0x1", out _), $"{spec.Name} rejects hex text");
}
Check(!Schema.TryParse(Schema.ByName["SwapChainApi"], "1", out _), "API sparse validation");
Check(!Schema.TryParse(Schema.ByName["MpoDemoCadenceHz"], "45", out _), "cadence sparse validation");
Check(Schema.EffectiveD3D11ReadbackMode(defaults, RegistryDatum.FromDword(2)) == 2, "absent framework reads legacy mode");
Check(Schema.EffectiveD3D11ReadbackMode(defaults, RegistryDatum.Absent()) == 0, "absent legacy reads mode zero");
var frameworkWins = new Dictionary<string, uint?>(defaults) { ["D3D11ReadbackMode"] = 0 };
Check(Schema.EffectiveD3D11ReadbackMode(frameworkWins, RegistryDatum.FromDword(2)) == 0, "present framework zero wins over legacy");
var badLegacy = RegistryDatum.FromDword(3);
Throws(() => Schema.EffectiveD3D11ReadbackMode(defaults, badLegacy), "invalid legacy fallback rejects");
Check(Snapshot(legacy: badLegacy).Errors().Any(e => e.Contains("Legacy")), "invalid effective fallback is visible");
Check(Snapshot(values: frameworkWins, legacy: badLegacy).Errors().Count == 0, "irrelevant invalid legacy does not override framework");
var expectedLegacy = Snapshot(legacy: RegistryDatum.FromDword(1));
var changedLegacy = new MemoryStore(Snapshot(legacy: RegistryDatum.FromDword(2)));
Throws(() => ConfigTransaction.Execute(changedLegacy, new() { Expected = expectedLegacy, Desired = Schema.Defaults() }),
    "stale legacy fallback rejected");
Check(changedLegacy.Writes.Count == 0, "legacy conflict writes nothing");
var legacyRoundtrip = JsonSerializer.Deserialize(JsonSerializer.SerializeToUtf8Bytes(
    new ApplyRequest { Expected = expectedLegacy, Desired = Schema.Preset(defaults, "Native D3D12") },
    ControlJsonContext.Default.ApplyRequest), ControlJsonContext.Default.ApplyRequest)!;
Check(legacyRoundtrip.Expected.Equivalent(expectedLegacy), "AOT JSON preserves legacy expected snapshot");
var legacyStore = new MemoryStore(expectedLegacy);
ConfigTransaction.Execute(legacyStore, legacyRoundtrip);
Check(legacyStore.ReadConsistent().LegacyReadback == RegistryDatum.FromDword(1), "grouped apply never writes legacy fallback");
var demo = Schema.Preset(defaults, "Live showcase");
Check(Schema.Validate(demo).Any(e => e.Contains("connector")), "demo requires explicit connector");
demo["MpoDemoConnectorIndex"] = 15;
Check(Schema.Validate(demo).Count == 0, "connector 15 valid");
demo["MpoDemoConnectorIndex"] = 16;
Check(Schema.Validate(demo).Count != 0, "connector 16 invalid");
var native = Schema.Preset(defaults, "Native D3D12");
Check(native["SwapChainApi"] == 12 && native["D3D12Mode"] == 1, "native preset explicit shape");
Check(native.Where(p => p.Key is not ("SwapChainApi" or "D3D12Mode")).All(p => p.Value is null),
    "preset preserves absent unrelated values");
native["DirtyRects"] = 1;
Check(Schema.Preset(native, "D3D11 reference")["DirtyRects"] == 1, "preset preserves present defaults");
native["D3D12Mode"] = 0;
Check(Schema.EffectiveD3D12Mode(native) == 1, "explicit D3D12 mode 0 resolves to 1");
native["SwapChainApi"] = 11;
native["D3D12Mode"] = 3;
Check(Schema.EffectiveD3D12Mode(native) == 0, "D3D11 mode effective zero");
Check(RevisionPlan.Create(0, false) == new RevisionPlan(1, 2), "absent revision grouping");
Check(RevisionPlan.Create(42, false) == new RevisionPlan(43, 44), "even revision grouping");
Check(RevisionPlan.Create(43, true) == new RevisionPlan(43, 44), "odd recovery");
Throws(() => RevisionPlan.Create(3, false), "odd apply rejected");
Throws(() => RevisionPlan.Create(uint.MaxValue - 1, false), "overflow rejected");
Throws(() => RevisionPlan.Create(uint.MaxValue, true), "recovery overflow rejected");

var expected = Snapshot();
var request = new ApplyRequest { Expected = expected, Desired = Schema.Preset(defaults, "Native D3D12") };
var roundtrip = JsonSerializer.Deserialize(
    JsonSerializer.SerializeToUtf8Bytes(request, ControlJsonContext.Default.ApplyRequest),
    ControlJsonContext.Default.ApplyRequest)!;
Check(roundtrip.Expected.Equivalent(expected), "AOT request snapshot roundtrip");
roundtrip.Validate();
var store = new MemoryStore(expected);
Check(ConfigTransaction.Execute(store, roundtrip) == 2, "transaction completed revision");
Check(store.Writes.SequenceEqual(["revision:1", "flush", "SwapChainApi:12", "D3D12Mode:1", "flush", "revision:2", "flush"]),
    "odd then grouped fields then even with flush ordering");
Check(store.ReadConsistent().Values["DirtyRects"] == RegistryDatum.Absent(), "transaction preserves absent defaults");
Check(store.Unrelated == "untouched", "unrelated values untouched");
var changed = Snapshot(2);
Throws(() => ConfigTransaction.Execute(new MemoryStore(changed), request), "stale revision rejected");
var altered = Schema.Defaults(); altered["DirtyRects"] = 0;
var changedStore = new MemoryStore(Snapshot(null, altered));
Throws(() => ConfigTransaction.Execute(changedStore, request), "stale values rejected even without revision");
Check(changedStore.Writes.Count == 0, "stale snapshot writes nothing");
var failing = new MemoryStore(expected) { FailWrites = true };
Throws(() => ConfigTransaction.Execute(failing, request), "partial write fails");
Check(failing.ReadConsistent().Incomplete, "failed grouped write remains odd");
var odd = Snapshot(3);
var recovery = new ApplyRequest { Expected = odd, Operation = "recover" };
var recoveryStore = new MemoryStore(odd);
Check(ConfigTransaction.Execute(recoveryStore, recovery) == 4, "explicit recovery completes current values");
Check(recoveryStore.Writes.SequenceEqual(["revision:4", "flush"]), "recovery cannot rewrite values");
var presentDefaults = Schema.Defaults();
presentDefaults["DirtyRects"] = 1;
var presentSnapshot = Snapshot(8, presentDefaults);
var deleteStore = new MemoryStore(presentSnapshot);
ConfigTransaction.Execute(deleteStore, new() { Expected = presentSnapshot, Desired = Schema.Defaults() });
Check(deleteStore.Writes.Contains("DirtyRects:"), "explicit restore-default deletes a present default");
Check(deleteStore.ReadConsistent().Values["DirtyRects"] == RegistryDatum.Absent(), "absent default restored");
Check(!Snapshot().Equivalent(Snapshot(0)), "absent revision differs from present zero");
var interfered = new MemoryStore(expected) { Interfere = true };
Throws(() => ConfigTransaction.Execute(interfered, request), "multiwriter value interference detected");
Check(interfered.ReadConsistent().Incomplete, "interference never commits an even revision");
var unsafeRecovery = Schema.Preset(defaults, "Live showcase");
Throws(() => new ApplyRequest { Expected = Snapshot(5, unsafeRecovery), Operation = "recover" }.Validate(),
    "recovery cannot accept an incomplete demo without a connector");
Throws(() => new ApplyRequest { Expected = expected, Desired = null! }.Validate(), "null request data rejected");
Throws(() => new ApplyRequest { Expected = odd, Desired = defaults }.Validate(), "no silent odd apply");
Throws(() => new ApplyRequest { Expected = expected, Desired = new() { ["ArbitraryKey"] = 1 } }.Validate(),
    "unknown key rejected");
var invalidValues = new Dictionary<string, RegistryDatum>(expected.Values)
{
    ["DirtyRects"] = new() { Present = true, Kind = "String", Raw = "hash" }
};
var invalid = new ConfigSnapshot { KeyExists = true, Values = invalidValues };
Check(invalid.Errors().Any(e => e.Contains("DirtyRects")), "invalid registry type visible");
Throws(() => new ApplyRequest { Expected = invalid, Desired = defaults }.Validate(), "invalid type never silently overwritten");
Throws(() => new ApplyRequest { Expected = expected, Desired = defaults, CreatedUtc = DateTimeOffset.UtcNow.AddMinutes(-11) }.Validate(),
    "expired request rejected");
var status = StatusClient.Parse(Encoding.UTF8.GetBytes(
    """{"generation":4,"revision":8,"status":"Ready","outputs":[{"connectorIndex":0,"active":true,"appliedGeneration":3,"api":12,"demo":6,"layers":2,"rotation":false,"lut":true,"gain":false}],"future":true}"""));
Check(status.Generation == 4 && status.Outputs![0].AppliedGeneration == 3, "requested generation distinct from applied");
Check(status.Caps is null, "missing capabilities unavailable");
Check(status.Outputs![0].VrrEnabled is null && status.Outputs[0].VrrDiagnostic == "VRR unreported",
    "legacy status never infers VRR from preference or active state");
Check(Schema.ByName["VrrPreference"].Label == "Prefer VRR" && Schema.Effective(defaults, "VrrPreference") == 1,
    "best-effort preference label and default preserved");
foreach (bool enabled in new[] { false, true })
{
    var vrrStatus = StatusClient.Parse(Encoding.UTF8.GetBytes(
        "{\"outputs\":[{\"vrrEnabled\":" + (enabled ? "true" : "false") + "}]}"));
    Check(vrrStatus.Outputs![0].VrrEnabled == enabled &&
        vrrStatus.Outputs[0].VrrDiagnostic == (enabled ? "VRR on" : "VRR off"), "actual VRR bool diagnostic");
    byte[] encoded = JsonSerializer.SerializeToUtf8Bytes(vrrStatus, ControlJsonContext.Default.DriverStatus);
    Check(StatusClient.Parse(encoded).Outputs![0].VrrEnabled == enabled, "AOT VRR bool roundtrip");
    Check(!Encoding.UTF8.GetString(encoded).Contains("vrrDiagnostic"), "display-only VRR diagnostic not added to wire schema");
}
Check(StatusClient.Parse("""{"outputs":[{"vrrEnabled":null}]}"""u8).Outputs![0].VrrDiagnostic == "VRR unreported",
    "explicit null VRR remains unknown");
Check(StatusClient.Parse("{}"u8).Revision is null, "missing fields remain unavailable");
Throws(() => StatusClient.Parse(Encoding.UTF8.GetBytes("""{"caps":{"maxDemoPlanes":99}}""")), "invalid caps rejected");
Throws(() => StatusClient.Parse(Encoding.UTF8.GetBytes("""{"outputs":[{"connectorIndex":16}]}""")), "invalid connector status rejected");
Throws(() => StatusClient.Parse(Encoding.UTF8.GetBytes("""{"outputs":[null]}""")), "null output record rejected");
async Task<StatusRead> ReadTestPipe(string payload, bool stall = false)
{
    string pipeName = $"NvIddCx.Control.Tests.{Guid.NewGuid():N}";
    using var cancel = new CancellationTokenSource(TimeSpan.FromSeconds(8));
    using var server = new NamedPipeServerStream(pipeName, PipeDirection.Out, 1, PipeTransmissionMode.Byte,
        PipeOptions.Asynchronous);
    var send = Task.Run(async () =>
    {
        await server.WaitForConnectionAsync(cancel.Token);
        if (stall) await Task.Delay(TimeSpan.FromSeconds(6), cancel.Token);
        else
        {
            await server.WriteAsync(Encoding.UTF8.GetBytes(payload), cancel.Token);
            server.WaitForPipeDrain();
            server.Disconnect();
        }
    });
    var result = await StatusClient.ReadFromPipeAsync(pipeName);
    cancel.Cancel();
    try { await send; }
    catch (Exception e) when (e is OperationCanceledException or IOException) { }
    return result;
}
var pipeStatus = await ReadTestPipe("""{"status":"rejected","message":"LUT unsupported","outputs":[{"api":11,"layers":1}]}""" + "\n");
Check(pipeStatus.Connected && pipeStatus.Value!.Status == "rejected", "real inbound pipe preserves rejection");
Check(pipeStatus.Value!.Outputs![0].Api == 11 && pipeStatus.Value.Outputs[0].Layers == 1, "actual head API/layers not inferred from demo");
Check(pipeStatus.Value.Outputs[0].VrrDiagnostic == "VRR unreported", "real legacy pipe has no fabricated VRR");
var appliedWarning = await ReadTestPipe(
    """{"status":"applied","message":"VRR unavailable; unsupported Showcase chapters skipped","outputs":[{"active":true,"api":12,"vrrEnabled":false}]}""" + "\n");
Check(appliedWarning.Connected && appliedWarning.Value!.Status == "applied" &&
    appliedWarning.Value.Message == "VRR unavailable; unsupported Showcase chapters skipped", "applied warning preserved verbatim");
Check(appliedWarning.Value!.Outputs![0].VrrDiagnostic == "VRR off", "actual pipe VRR false stays off despite applied status");
Check(!(await ReadTestPipe("not-json\n")).Connected, "malformed pipe JSON is disconnected");
Check((await ReadTestPipe("{}")).Error.Contains("newline"), "EOF before newline is an explicit error");
var bounded = Stopwatch.StartNew();
Check(!(await ReadTestPipe("", stall: true)).Connected, "stalled pipe read times out");
Check(bounded.Elapsed < TimeSpan.FromSeconds(5), "connect and read share bounded deadline");
Check(!(await ReadTestPipe(new string('x', StatusClient.MaximumBytes))).Connected, "oversized pipe line rejected");
assertions += await RequestFileTests.RunAsync(request);
Console.WriteLine($"PASS: {assertions} production-model/transport assertions (no registry writes, no elevation requested).");

sealed class MemoryStore(ConfigSnapshot initial) : IConfigStore
{
    private ConfigSnapshot snapshot = initial;
    public List<string> Writes { get; } = [];
    public bool FailWrites { get; init; }
    public bool Interfere { get; init; }
    public string Unrelated { get; } = "untouched";
    public ConfigSnapshot ReadConsistent() => snapshot;
    public void WriteRevision(uint revision)
    {
        Writes.Add($"revision:{revision}");
        snapshot = new() { KeyExists = snapshot.KeyExists, Revision = RegistryDatum.FromDword(revision),
            Values = new(snapshot.Values), LegacyReadback = snapshot.LegacyReadback };
    }
    public void WriteValue(string name, uint? value)
    {
        if (FailWrites) throw new IOException("Simulated registry failure");
        Writes.Add($"{name}:{value}");
        snapshot.Values[name] = value is uint n ? RegistryDatum.FromDword(n) : RegistryDatum.Absent();
    }
    public void Flush()
    {
        Writes.Add("flush");
        if (Interfere && Writes.Any(w => w.StartsWith("SwapChainApi:")))
            snapshot.Values["DirtyRects"] = RegistryDatum.FromDword(0);
    }
}
