using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace NvIddCx.Control.Core;

public sealed class DriverStatus
{
    public uint? Generation { get; init; }
    public uint? Revision { get; init; }
    public string? Status { get; init; }
    public string? Message { get; init; }
    public OutputStatus[]? Outputs { get; init; }
    public DriverCaps? Caps { get; init; }
}

public sealed class OutputStatus
{
    public uint? ConnectorIndex { get; init; }
    public bool? Active { get; init; }
    public uint? AppliedGeneration { get; init; }
    public uint? Api { get; init; }
    public uint? Demo { get; init; }
    public uint? Layers { get; init; }
    public bool? Rotation { get; init; }
    public bool? Lut { get; init; }
    public bool? Gain { get; init; }
    public bool? VrrEnabled { get; init; }
    [JsonIgnore]
    public string VrrDiagnostic => VrrEnabled switch { true => "VRR on", false => "VRR off", null => "VRR unreported" };
}

// Optional explicit capability extension. Applied flags are not capabilities.
public sealed class DriverCaps
{
    public uint[]? SupportedApis { get; init; }
    public uint[]? SupportedDemos { get; init; }
    public uint? MaxDemoPlanes { get; init; }
}

public sealed record StatusRead(DriverStatus? Value, string Error)
{
    public bool Connected => Value is not null;
}

[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
    GenerationMode = JsonSourceGenerationMode.Metadata, WriteIndented = true,
    UnmappedMemberHandling = JsonUnmappedMemberHandling.Skip)]
[JsonSerializable(typeof(ApplyRequest))]
[JsonSerializable(typeof(DriverStatus))]
public partial class ControlJsonContext : JsonSerializerContext;

public static class StatusClient
{
    public const string PipeName = "NvIddCx.Status";
    public const int MaximumBytes = 65536;
    public static DriverStatus Parse(ReadOnlySpan<byte> bytes)
    {
        var status = JsonSerializer.Deserialize(bytes, ControlJsonContext.Default.DriverStatus)
            ?? throw new InvalidDataException("Empty driver status.");
        if (status.Outputs is { Length: > 16 } || status.Status?.Length > 256 || status.Message?.Length > 4096)
            throw new InvalidDataException("Driver status exceeds bounds.");
        if (status.Outputs?.Any(o => o is null || o.ConnectorIndex > 15 ||
            o.Api is not (null or 0 or 11 or 12) || o.Demo > 6 || o.Layers > 7) == true)
            throw new InvalidDataException("Invalid driver output record.");
        if (status.Caps?.SupportedApis is { Length: > 16 } || status.Caps?.SupportedDemos is { Length: > 16 } ||
            status.Caps?.MaxDemoPlanes is 0 or > 7)
            throw new InvalidDataException("Invalid driver capabilities.");
        return status;
    }

    public static Task<StatusRead> ReadAsync(CancellationToken token = default) =>
        ReadFromPipeAsync(PipeName, token);

    internal static async Task<StatusRead> ReadFromPipeAsync(string pipeName, CancellationToken token = default)
    {
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(token);
        deadline.CancelAfter(TimeSpan.FromMilliseconds(1800));
        try
        {
            using var pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.In, PipeOptions.Asynchronous);
            await pipe.ConnectAsync(deadline.Token).ConfigureAwait(false);
            using var bytes = new MemoryStream();
            byte[] buffer = new byte[1024];
            while (bytes.Length < MaximumBytes)
            {
                int read = await pipe.ReadAsync(buffer.AsMemory(0, Math.Min(buffer.Length, MaximumBytes - (int)bytes.Length)),
                    deadline.Token).ConfigureAwait(false);
                if (read == 0) throw new InvalidDataException("Driver disconnected before a JSON newline.");
                int newline = Array.IndexOf(buffer, (byte)'\n', 0, read);
                bytes.Write(buffer, 0, newline >= 0 ? newline : read);
                if (newline >= 0) return new(Parse(bytes.ToArray()), "");
            }
            throw new InvalidDataException("Status line exceeds 64 KiB.");
        }
        catch (OperationCanceledException) { return new(null, "Not connected · connect/read deadline elapsed."); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or InvalidDataException)
        { return new(null, $"Not connected · {e.Message}"); }
    }
}
