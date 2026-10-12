using System.Diagnostics;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text.Json;
using NvIddCx.Control.Core;

namespace NvIddCx.Control;

internal static class ApplyBroker
{
    internal const int MaximumRequestBytes = 65536;
    private static string RequestDirectory => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NvIddCxControl", "Requests");
    private static SecurityIdentifier UserSid => WindowsIdentity.GetCurrent().User ??
        throw new UnauthorizedAccessException("No Windows user SID.");

    internal static DirectorySecurity PrivateDirectorySecurity()
    {
        var security = new DirectorySecurity();
        security.SetOwner(UserSid);
        security.SetAccessRuleProtection(true, false);
        foreach (var sid in new[] { UserSid,
            new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null),
            new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null) })
            security.AddAccessRule(new FileSystemAccessRule(sid, FileSystemRights.FullControl,
                InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None,
                AccessControlType.Allow));
        return security;
    }

    internal static FileSecurity PrivateFileSecurity()
    {
        var security = new FileSecurity();
        // An elevated token's default owner can be Administrators, not its user SID.
        security.SetOwner(UserSid);
        security.SetAccessRuleProtection(true, false);
        foreach (var sid in new[] { UserSid,
            new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null),
            new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null) })
            security.AddAccessRule(new FileSystemAccessRule(sid, FileSystemRights.FullControl,
                AccessControlType.Allow));
        return security;
    }

    internal static void CheckPrivateSecurity(ObjectSecurity security, bool directory)
    {
        int ownerCode = directory ? 67 : 69;
        int accessCode = directory ? 68 : 72;
        string item = directory ? "Request directory" : "Request file";
        if (!UserSid.Equals(security.GetOwner(typeof(SecurityIdentifier))))
            throw new RequestRejectedException(ownerCode, $"{item} is not owned by the current user SID.");
        var rules = ((CommonObjectSecurity)security).GetAccessRules(true, true, typeof(SecurityIdentifier));
        foreach (FileSystemAccessRule rule in rules)
        {
            if (rule.AccessControlType != AccessControlType.Allow) continue;
            var sid = (SecurityIdentifier)rule.IdentityReference;
            if (sid.Equals(UserSid) || sid.IsWellKnown(WellKnownSidType.BuiltinAdministratorsSid) ||
                sid.IsWellKnown(WellKnownSidType.LocalSystemSid)) continue;
            const FileSystemRights write = FileSystemRights.Write | FileSystemRights.Delete |
                FileSystemRights.DeleteSubdirectoriesAndFiles | FileSystemRights.ChangePermissions |
                FileSystemRights.TakeOwnership;
            if ((rule.FileSystemRights & write) != 0)
                throw new RequestRejectedException(accessCode, $"{item} allows another identity to write.");
        }
    }

    private static void CheckOwnedPath(string path)
    {
        var current = new DirectoryInfo(path);
        for (DirectoryInfo? ancestor = current; ancestor is not null; ancestor = ancestor.Parent)
            if (ancestor.Exists && (ancestor.Attributes & FileAttributes.ReparsePoint) != 0)
                throw new RequestRejectedException(66, "Request path cannot traverse a reparse point.");
        CheckPrivateSecurity(current.GetAccessControl(), directory: true);
    }

    internal static async Task<string> CreateRequestAsync(ApplyRequest request, string requestDirectory)
    {
        request.Validate();
        var parent = new DirectoryInfo(Path.GetDirectoryName(requestDirectory)!);
        if (!parent.Exists) parent.Create(PrivateDirectorySecurity());
        CheckOwnedPath(parent.FullName);
        var directory = new DirectoryInfo(requestDirectory);
        if (!directory.Exists) directory.Create(PrivateDirectorySecurity());
        CheckOwnedPath(directory.FullName);
        string path = Path.Combine(requestDirectory, $"{Guid.NewGuid():N}.json");
        byte[] bytes = JsonSerializer.SerializeToUtf8Bytes(request, ControlJsonContext.Default.ApplyRequest);
        if (bytes.Length > MaximumRequestBytes)
            throw new RequestRejectedException(76, "Request exceeds 64 KiB.");
        bool created = false;
        try
        {
            await using (var file = new FileInfo(path).Create(FileMode.CreateNew, FileSystemRights.FullControl,
                FileShare.None, 4096, FileOptions.Asynchronous, PrivateFileSecurity()))
            {
                created = true;
                CheckPrivateSecurity(file.GetAccessControl(), directory: false);
                await file.WriteAsync(bytes);
            }
            return path;
        }
        catch
        {
            if (created) File.Delete(path);
            throw;
        }
    }

    public static async Task<int> SubmitAsync(ApplyRequest request)
    {
        string path = await CreateRequestAsync(request, RequestDirectory);
        try
        {
            var start = new ProcessStartInfo(Environment.ProcessPath ??
                throw new IOException("Cannot locate this executable."))
            {
                UseShellExecute = true,
                Verb = "runas",
                WorkingDirectory = AppContext.BaseDirectory
            };
            start.ArgumentList.Add("--apply-request");
            start.ArgumentList.Add(path);
            using var process = Process.Start(start) ?? throw new IOException("Elevated helper did not start.");
            await process.WaitForExitAsync();
            return process.ExitCode;
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }

    internal static ApplyRequest ReadRequest(string suppliedPath, string requestDirectory)
    {
        string path = Path.GetFullPath(suppliedPath);
        if (!string.Equals(Path.GetDirectoryName(path), requestDirectory, StringComparison.OrdinalIgnoreCase) ||
            !string.Equals(Path.GetExtension(path), ".json", StringComparison.OrdinalIgnoreCase) ||
            !Guid.TryParseExact(Path.GetFileNameWithoutExtension(path), "N", out _))
            throw new RequestRejectedException(65, "Request must be a GUID-named JSON file in the current user's private Requests directory.");
        CheckOwnedPath(requestDirectory);
        var info = new FileInfo(path);
        if ((info.Attributes & FileAttributes.ReparsePoint) != 0)
            throw new RequestRejectedException(66, "Request file cannot be a reparse point.");
        // Inspect the owner/ACL and read through the same exclusively held handle.
        using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.None);
        CheckPrivateSecurity(file.GetAccessControl(), directory: false);
        if (file.Length is <= 0 or > MaximumRequestBytes)
            throw new RequestRejectedException(76, "Request file must contain 1–65536 bytes.");
        byte[] bytes = new byte[(int)file.Length];
        file.ReadExactly(bytes);
        var request = JsonSerializer.Deserialize(bytes, ControlJsonContext.Default.ApplyRequest) ??
            throw new InvalidDataException("Empty request.");
        request.Validate();
        return request;
    }

    internal static string DescribeExitCode(int result) => result switch
    {
        65 => "Request path rejected: expected GUID.json in this user's private Requests directory. If UAC used a different account, use same-user elevation.",
        66 => "Request rejected: its file or an ancestor directory is a reparse point. Use a local, non-redirected request directory.",
        67 => "Request directory owner rejected: it must be the current user SID. Existing directory permissions were not changed.",
        68 => "Request directory permissions rejected: another identity can write. Existing permissions were not changed.",
        69 => "Request file owner rejected: it must be the current user SID, not Administrators or a different UAC account. No write performed.",
        72 => "Request file permissions rejected: another identity can write. No write performed.",
        73 => "Configuration conflict/interference. Nothing is assumed applied; reload and review.",
        75 => "Another control writer is busy. Try again after reloading.",
        76 => "Request size rejected: JSON must contain 1–65536 bytes. No write performed.",
        77 => "Helper was not elevated; no write performed.",
        _ => $"Helper exit {result}. Registry failure may leave an odd revision; reload and explicitly review recovery."
    };

    public static int RunHelper(string suppliedPath)
    {
        try
        {
            if (!new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator))
                return 77;
            var request = ReadRequest(suppliedPath, RequestDirectory);
            using var mutex = new Mutex(false, Schema.WriterMutex);
            bool held;
            try { held = mutex.WaitOne(TimeSpan.FromSeconds(5)); }
            catch (AbandonedMutexException) { held = true; }
            if (!held) return 75;
            try
            {
                using var store = new RegistryStore(writable: true);
                ConfigTransaction.Execute(store, request);
            }
            finally { mutex.ReleaseMutex(); }
            return 0;
        }
        catch (RequestRejectedException e) { return e.ExitCode; }
        catch (InvalidOperationException) { return 73; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or
            InvalidDataException or JsonException or ArgumentException or System.Security.SecurityException)
        { return 74; }
    }
}

internal sealed class RequestRejectedException(int exitCode, string message) : IOException(message)
{
    public int ExitCode { get; } = exitCode;
}
