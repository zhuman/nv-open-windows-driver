namespace NvIddCx.Control.Core;

public interface IConfigStore
{
    ConfigSnapshot ReadConsistent();
    void WriteRevision(uint revision);
    void WriteValue(string name, uint? value);
    void Flush();
}

public sealed class ApplyRequest
{
    public int Protocol { get; init; } = 1;
    public string Operation { get; init; } = "apply";
    public DateTimeOffset CreatedUtc { get; init; } = DateTimeOffset.UtcNow;
    public ConfigSnapshot Expected { get; init; } = new();
    public Dictionary<string, uint?> Desired { get; init; } = new(StringComparer.Ordinal);

    public void Validate()
    {
        if (Expected is null || Expected.Revision is null || Expected.LegacyReadback is null || Expected.Values is null || Desired is null ||
            Expected.Values.Values.Any(v => v is null))
            throw new InvalidDataException("Request contains null configuration data.");
        if (Protocol != 1 || Operation is not ("apply" or "recover"))
            throw new InvalidDataException("Unknown request protocol or operation.");
        if (CreatedUtc > DateTimeOffset.UtcNow.AddMinutes(1) || CreatedUtc < DateTimeOffset.UtcNow.AddMinutes(-10))
            throw new InvalidDataException("Request expired or has an invalid creation time.");
        var errors = Expected.Errors(allowIncomplete: Operation == "recover");
        if (Operation == "apply") errors.AddRange(Schema.Validate(Desired));
        else
        {
            if (!Expected.Incomplete) errors.Add("Recovery requires an odd DWORD revision.");
            if (Desired.Count != 0) errors.Add("Recovery cannot change values.");
            errors.AddRange(Schema.Validate(Expected.EditableValues()));
        }
        if (errors.Count != 0) throw new InvalidDataException(string.Join("\n", errors));
        RevisionPlan.Create(Expected.RevisionNumber, Operation == "recover");
    }
}

public readonly record struct RevisionPlan(uint Begin, uint Complete)
{
    public static RevisionPlan Create(uint current, bool recover)
    {
        if (recover)
        {
            if ((current & 1) == 0 || current == uint.MaxValue)
                throw new InvalidDataException("Odd revision cannot be recovered without revision overflow.");
            return new(current, current + 1);
        }
        if ((current & 1) != 0 || current >= uint.MaxValue - 1)
            throw new InvalidDataException("Revision is incomplete or exhausted; explicit recovery is required.");
        return new(current + 1, current + 2);
    }
}

public static class ConfigTransaction
{
    // Caller holds the global writer mutex throughout comparison, writes and flushes.
    public static uint Execute(IConfigStore store, ApplyRequest request)
    {
        request.Validate();
        var current = store.ReadConsistent();
        if (!current.Equivalent(request.Expected))
            throw new InvalidOperationException("Configuration changed since it was loaded. Reload and review; nothing was written.");
        var plan = RevisionPlan.Create(current.RevisionNumber, request.Operation == "recover");
        if (request.Operation == "recover")
        {
            store.WriteRevision(plan.Complete);
            store.Flush();
            return plan.Complete;
        }
        store.WriteRevision(plan.Begin);
        store.Flush();
        foreach (var spec in Schema.Settings)
        {
            uint? before = current.Values[spec.Name].IsDword ? current.Values[spec.Name].Dword : null;
            if (before != request.Desired[spec.Name]) store.WriteValue(spec.Name, request.Desired[spec.Name]);
        }
        store.Flush();
        var written = store.ReadConsistent();
        if (!written.Revision.IsDword || written.RevisionNumber != plan.Begin || written.LegacyReadback != current.LegacyReadback ||
            Schema.Settings.Any(s => written.Values[s.Name] !=
                (request.Desired[s.Name] is uint n ? RegistryDatum.FromDword(n) : RegistryDatum.Absent())))
            throw new InvalidOperationException("Another writer interfered with the grouped write. Do not assume it applied; reload and review.");
        store.WriteRevision(plan.Complete);
        store.Flush();
        return plan.Complete;
    }
}
