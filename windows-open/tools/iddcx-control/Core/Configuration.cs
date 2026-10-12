using System.Globalization;

namespace NvIddCx.Control.Core;

public sealed record Choice(uint? Value, string Label)
{
    public override string ToString() => Label;
}

public sealed record SettingSpec(string Name, string Label, string Description, uint? Default,
    uint Minimum, uint Maximum, Choice[]? Choices = null)
{
    public bool Accepts(uint value) => value >= Minimum && value <= Maximum &&
        (Choices is null || Choices.Any(c => c.Value == value));

    public IEnumerable<Choice> EditorChoices()
    {
        yield return new(null, Name == "D3D11ReadbackMode" ? "Driver default · legacy fallback" :
            Default is uint d ? $"Driver default · {LabelFor(d)}" : "Not set · required for demos");
        if (Choices is not null)
            foreach (var choice in Choices) yield return choice;
        else
            for (uint value = Minimum; value <= Maximum; value++) yield return new(value, value.ToString(CultureInfo.InvariantCulture));
    }

    public string LabelFor(uint value) => Choices?.FirstOrDefault(c => c.Value == value)?.Label ??
        value.ToString(CultureInfo.InvariantCulture);
}

public static class Schema
{
    public const string RegistryPath = @"SOFTWARE\Microsoft\Windows NT\CurrentVersion\WUDF\Services\nv-iddcx\Parameters";
    public const string LegacyRegistryPath = @"SOFTWARE\NVIDIA Corporation\nv-iddcx";
    public const string LegacyReadbackName = "ReadbackMode";
    public const string RevisionName = "ConfigRevision";
    public const string WriterMutex = @"Global\NvIddCx.Control.ConfigWriter.v1";
    private static Choice[] Bool => [new(0, "Off"), new(1, "On")];
    public static readonly SettingSpec[] Settings =
    [
        new("SwapChainApi", "Swapchain API", "Auto lets the driver select. Explicit D3D11 disables D3D12 mode.", 0, 0, 12,
            [new(0, "Auto"), new(11, "Direct3D 11"), new(12, "Direct3D 12")]),
        new("D3D12Mode", "D3D12 path", "Explicit D3D12 + Off resolves to Direct WDF. Alternating is experimental.", 1, 0, 3,
            [new(0, "Off"), new(1, "Direct WDF"), new(2, "Readback reference"), new(3, "Alternating experiment")]),
        new("D3D11ReadbackMode", "D3D11 readback", "Driver modes 0–2; mode 0 is the default path.", 0, 0, 2,
            [new(0, "Default path"), new(1, "Mode 1"), new(2, "Mode 2")]),
        new("SurfaceCachePolicy", "Surface cache", "Driver policies 0–2; policy 0 is the default.", 0, 0, 2,
            [new(0, "Default policy"), new(1, "Policy 1"), new(2, "Policy 2")]),
        new("DirtyRects", "Dirty rectangles", "Use driver damage tracking.", 1, 0, 1, Bool),
        new("D3D12Debug", "D3D12 debug", "Diagnostic mode; can affect performance.", 0, 0, 1, Bool),
        new("RealtimeGpuPriority", "Realtime GPU priority", "Request the driver's realtime scheduling preference.", 1, 0, 1, Bool),
        new("MmcssDisplayPostProcessing", "MMCSS display processing", "Use display post-processing scheduling.", 1, 0, 1, Bool),
        new("VrrPreference", "Prefer VRR", "Best-effort preference, not effective VRR. See actual output diagnostics.", 1, 0, 1, Bool),
        new("FlipMode", "Flip mode", "Request the driver's flip path.", 1, 0, 1, Bool),
        new("FramePipeline", "Frame pipeline", "Enable driver frame pipelining.", 1, 0, 1, Bool),
        new("ScanoutBufferCount", "Scanout buffers", "Double, triple or quadruple buffering.", 3, 2, 4),
        new("MpoDemoMode", "MPO demonstration", "Requested demo, never a claim that hardware applied it.", 0, 0, 6,
            [new(0, "Off"), new(1, "Alpha Orbit"), new(2, "Geometry Compass"), new(3, "CSC Color Lab"),
             new(4, "Luminance Elevator"), new(5, "LUT Gallery"), new(6, "Live Showcase")]),
        new("MpoDemoPlaneCount", "Demo planes", "Runtime capability gate may limit the requested plane count.", 1, 1, 7),
        new("MpoDemoConnectorIndex", "Connector index", "Driver connector index 0–15, NOT a GDI display number.", null, 0, 15),
        new("MpoDemoCadenceHz", "Demo cadence", "Requested update cadence in Hz.", 30, 15, 60,
            [new(15, "15 Hz"), new(30, "30 Hz"), new(60, "60 Hz")]),
        new("MpoDemoSpeedPercent", "Demo speed", "Animation speed in percent (25–200).", 100, 25, 200)
    ];
    public static readonly IReadOnlyDictionary<string, SettingSpec> ByName =
        Settings.ToDictionary(s => s.Name, StringComparer.Ordinal);

    public static Dictionary<string, uint?> Defaults() =>
        Settings.ToDictionary(s => s.Name, _ => (uint?)null, StringComparer.Ordinal);

    // Presets only set their own fields. All other present/absent values survive.
    public static Dictionary<string, uint?> Preset(IReadOnlyDictionary<string, uint?> current, string name)
    {
        var result = current.ToDictionary(p => p.Key, p => p.Value, StringComparer.Ordinal);
        var patch = name switch
        {
            "Native D3D12" => new Dictionary<string, uint?> { ["SwapChainApi"] = 12, ["D3D12Mode"] = 1 },
            "D3D11 reference" => new Dictionary<string, uint?> { ["SwapChainApi"] = 11, ["D3D11ReadbackMode"] = 0 },
            "Live showcase" => new Dictionary<string, uint?> { ["MpoDemoMode"] = 6, ["MpoDemoPlaneCount"] = 1 },
            _ => throw new ArgumentException("Unknown preset.")
        };
        foreach (var pair in patch) result[pair.Key] = pair.Value;
        return result;
    }

    public static uint Effective(IReadOnlyDictionary<string, uint?> values, string name) =>
        values[name] ?? ByName[name].Default ?? 0;

    public static uint EffectiveD3D12Mode(IReadOnlyDictionary<string, uint?> values)
    {
        uint api = Effective(values, "SwapChainApi"), mode = Effective(values, "D3D12Mode");
        return api == 11 ? 0 : api == 12 && mode == 0 ? 1 : mode;
    }

    public static uint EffectiveD3D11ReadbackMode(IReadOnlyDictionary<string, uint?> values, RegistryDatum legacy)
    {
        if (values["D3D11ReadbackMode"] is uint framework) return framework;
        if (!legacy.Present) return 0;
        if (!legacy.IsDword || !ByName["D3D11ReadbackMode"].Accepts(legacy.Dword!.Value))
            throw new InvalidDataException("Invalid legacy ReadbackMode; no default was substituted.");
        return legacy.Dword!.Value;
    }

    public static bool TryParse(SettingSpec spec, string text, out uint value) =>
        uint.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out value) && spec.Accepts(value);

    public static List<string> Validate(IReadOnlyDictionary<string, uint?> values)
    {
        var errors = new List<string>();
        if (values.Count != Settings.Length || values.Keys.Any(k => !ByName.ContainsKey(k)))
            errors.Add("Configuration must contain exactly the known setting names.");
        foreach (var spec in Settings)
        {
            if (!values.TryGetValue(spec.Name, out uint? value)) errors.Add($"{spec.Name} is missing from the request.");
            else if (value is uint n && !spec.Accepts(n)) errors.Add($"{spec.Name} has invalid DWORD value {n}.");
        }
        if (values.TryGetValue("MpoDemoMode", out uint? demo) && demo is > 0 &&
            (!values.TryGetValue("MpoDemoConnectorIndex", out uint? connector) || connector is null))
            errors.Add("Choose a driver connector index before enabling a demo.");
        return errors;
    }
}

public sealed record RegistryDatum
{
    public bool Present { get; init; }
    public string Kind { get; init; } = "Absent";
    public uint? Dword { get; init; }
    public string? Raw { get; init; }
    public static RegistryDatum Absent() => new();
    public static RegistryDatum FromDword(uint value) => new() { Present = true, Kind = "DWord", Dword = value };
    public bool IsDword => Present && Kind == "DWord" && Dword is not null && Raw is null;
}

public sealed class ConfigSnapshot
{
    public bool KeyExists { get; init; }
    public RegistryDatum Revision { get; init; } = RegistryDatum.Absent();
    public RegistryDatum LegacyReadback { get; init; } = RegistryDatum.Absent();
    public Dictionary<string, RegistryDatum> Values { get; init; } = new(StringComparer.Ordinal);
    public uint RevisionNumber => Revision.IsDword ? Revision.Dword!.Value : 0;
    public bool Incomplete => Revision.IsDword && (RevisionNumber & 1) != 0;
    public bool Equivalent(ConfigSnapshot other) => KeyExists == other.KeyExists && Revision == other.Revision &&
        LegacyReadback == other.LegacyReadback &&
        Values.Count == other.Values.Count && Values.All(p => other.Values.TryGetValue(p.Key, out var v) && v == p.Value);

    public Dictionary<string, uint?> EditableValues() => Schema.Settings.ToDictionary(s => s.Name,
        s => Values.TryGetValue(s.Name, out var datum) && datum.IsDword ? datum.Dword : null, StringComparer.Ordinal);

    public List<string> Errors(bool allowIncomplete = false)
    {
        var errors = new List<string>();
        if (!KeyExists) errors.Add("The framework-managed Parameters key is absent. The app will not create it.");
        if (Revision.Present && !Revision.IsDword) errors.Add("ConfigRevision is not a DWORD. Repair it explicitly outside this app.");
        if (!allowIncomplete && Incomplete) errors.Add("Incomplete grouped write (odd ConfigRevision). Review and explicitly recover before Apply.");
        if (Values.Count != Schema.Settings.Length || Values.Keys.Any(k => !Schema.ByName.ContainsKey(k)))
            errors.Add("Snapshot does not contain exactly the known fields.");
        foreach (var spec in Schema.Settings)
        {
            if (!Values.TryGetValue(spec.Name, out var value)) errors.Add($"Snapshot lacks {spec.Name}.");
            else if (value.Present && (!value.IsDword || !spec.Accepts(value.Dword!.Value)))
                errors.Add($"{spec.Name}: invalid registry {value.Kind} value; it has NOT been replaced with a default.");
        }
        if (Values.TryGetValue("D3D11ReadbackMode", out var frameworkReadback) && !frameworkReadback.Present &&
            LegacyReadback.Present && (!LegacyReadback.IsDword ||
                !Schema.ByName["D3D11ReadbackMode"].Accepts(LegacyReadback.Dword!.Value)))
            errors.Add("Legacy ReadbackMode: invalid fallback; it has NOT been replaced with a default.");
        return errors;
    }
}
