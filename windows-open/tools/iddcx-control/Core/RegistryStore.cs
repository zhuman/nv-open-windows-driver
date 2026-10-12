using Microsoft.Win32;
using System.Security.Cryptography;
using System.Text;

namespace NvIddCx.Control.Core;

public sealed class RegistryStore : IConfigStore, IDisposable
{
    private readonly RegistryKey hive;
    private readonly RegistryKey? key;
    private readonly RegistryKey? legacyKey;
    public RegistryStore(bool writable = false)
    {
        hive = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
        key = hive.OpenSubKey(Schema.RegistryPath, writable);
        legacyKey = hive.OpenSubKey(Schema.LegacyRegistryPath, writable: false);
    }

    public ConfigSnapshot ReadConsistent()
    {
        for (int attempt = 0; attempt < 4; attempt++)
        {
            var before = ReadDatum(key, Schema.RevisionName);
            var legacyBefore = ReadDatum(legacyKey, Schema.LegacyReadbackName);
            var values = Schema.Settings.ToDictionary(s => s.Name, s => ReadDatum(key, s.Name), StringComparer.Ordinal);
            var legacyAfter = ReadDatum(legacyKey, Schema.LegacyReadbackName);
            var after = ReadDatum(key, Schema.RevisionName);
            if (before == after && legacyBefore == legacyAfter)
                return new() { KeyExists = key is not null, Revision = after, Values = values, LegacyReadback = legacyAfter };
        }
        throw new IOException("Configuration is changing. Wait for the writer, then reload.");
    }

    private static RegistryDatum ReadDatum(RegistryKey? source, string name)
    {
        if (source is null) return RegistryDatum.Absent();
        object? value = source.GetValue(name, null, RegistryValueOptions.DoNotExpandEnvironmentNames);
        if (value is null) return RegistryDatum.Absent();
        var kind = source.GetValueKind(name);
        if (kind == RegistryValueKind.DWord && value is int integer)
            return RegistryDatum.FromDword(unchecked((uint)integer));
        string raw = value switch
        {
            byte[] bytes => Convert.ToHexString(SHA256.HashData(bytes)),
            string[] strings => Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(string.Join("\0", strings)))),
            string text => Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(text))),
            long number => number.ToString(System.Globalization.CultureInfo.InvariantCulture),
            _ => "Unsupported"
        };
        return new() { Present = true, Kind = kind.ToString(), Raw = raw };
    }

    public void WriteRevision(uint revision) =>
        RequireKey().SetValue(Schema.RevisionName, unchecked((int)revision), RegistryValueKind.DWord);
    public void WriteValue(string name, uint? value)
    {
        if (!Schema.ByName.ContainsKey(name)) throw new InvalidDataException("Unknown setting name.");
        if (value is uint n) RequireKey().SetValue(name, unchecked((int)n), RegistryValueKind.DWord);
        else RequireKey().DeleteValue(name, false);
    }
    private RegistryKey RequireKey() => key ?? throw new IOException("Parameters key is absent; no key was created.");
    public void Flush() => RequireKey().Flush();
    public void Dispose() { legacyKey?.Dispose(); key?.Dispose(); hive.Dispose(); }
}
