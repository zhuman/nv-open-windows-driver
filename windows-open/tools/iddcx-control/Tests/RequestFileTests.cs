using System.Diagnostics;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text.Json;
using NvIddCx.Control;
using NvIddCx.Control.Core;

internal static class RequestFileTests
{
    public static async Task<int> RunAsync(ApplyRequest request)
    {
        int assertions = 0;
        void Check(bool condition, string name)
        {
            assertions++;
            if (!condition) throw new Exception($"FAIL: request files: {name}");
        }
        void Rejected(Action action, int code, string name)
        {
            try { action(); }
            catch (RequestRejectedException e)
            {
                Check(e.ExitCode == code && !string.IsNullOrWhiteSpace(e.Message), name);
                return;
            }
            throw new Exception($"FAIL: request files: expected rejection: {name}");
        }
        void Throws<T>(Action action, string name) where T : Exception
        {
            try { action(); }
            catch (T) { assertions++; return; }
            throw new Exception($"FAIL: request files: expected {typeof(T).Name}: {name}");
        }

        using var identity = WindowsIdentity.GetCurrent();
        var sid = identity.User!;
        var administrators = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
        var everyone = new SecurityIdentifier(WellKnownSidType.WorldSid, null);
        var oldElevatedFile = ApplyBroker.PrivateFileSecurity();
        oldElevatedFile.SetOwner(administrators);
        Rejected(() => ApplyBroker.CheckPrivateSecurity(oldElevatedFile, directory: false), 69,
            "old elevated default-owner descriptor reproduces helper rejection");
        var wrongDirectoryOwner = ApplyBroker.PrivateDirectorySecurity();
        wrongDirectoryOwner.SetOwner(administrators);
        Rejected(() => ApplyBroker.CheckPrivateSecurity(wrongDirectoryOwner, directory: true), 67,
            "Administrators directory owner still rejected");
        foreach (bool directory in new[] { false, true })
        {
            FileSystemSecurity acl = directory ? ApplyBroker.PrivateDirectorySecurity() : ApplyBroker.PrivateFileSecurity();
            acl.AddAccessRule(new FileSystemAccessRule(everyone, FileSystemRights.Write, AccessControlType.Allow));
            Rejected(() => ApplyBroker.CheckPrivateSecurity(acl, directory), directory ? 68 : 72,
                "untrusted writes still rejected");
        }

        string root = Path.GetFullPath(Path.Combine("artifacts", $"request-files-{Guid.NewGuid():N}"));
        string requestDirectory = Path.Combine(root, "Session", "Requests");
        string junction = Path.Combine(root, "Junction");
        Directory.CreateDirectory(root);
        try
        {
            string path = await ApplyBroker.CreateRequestAsync(request, requestDirectory);
            var acl = new FileInfo(path).GetAccessControl();
            Check(sid.Equals(acl.GetOwner(typeof(SecurityIdentifier))), "production file explicitly owned by current user");
            Check(acl.AreAccessRulesProtected, "production file does not inherit broad ACLs");
            Check(acl.GetAccessRules(true, true, typeof(SecurityIdentifier)).Count == 3,
                "production file grants only user, Administrators and SYSTEM");
            Check(new FileInfo(path).Length is > 0 and <= ApplyBroker.MaximumRequestBytes, "production file size bounded");
            Check(Guid.TryParseExact(Path.GetFileNameWithoutExtension(path), "N", out _), "production GUID filename");
            var read = ApplyBroker.ReadRequest(path, requestDirectory);
            Check(read.Expected.Equivalent(request.Expected) && read.Desired.SequenceEqual(request.Desired),
                "production creator/helper reader agree without registry access");
            Check(read.CreatedUtc == request.CreatedUtc && read.Operation == request.Operation,
                "request age and operation preserved");
            string recoveryPath = await ApplyBroker.CreateRequestAsync(new ApplyRequest
            {
                Operation = "recover",
                Expected = new ConfigSnapshot
                {
                    KeyExists = true, Revision = RegistryDatum.FromDword(3),
                    LegacyReadback = request.Expected.LegacyReadback, Values = new(request.Expected.Values)
                }
            }, requestDirectory);
            Check(ApplyBroker.ReadRequest(recoveryPath, requestDirectory).Operation == "recover",
                "recovery uses the same protected transport");

            string legacyPath = Path.Combine(requestDirectory, $"{Guid.NewGuid():N}.json");
            await File.WriteAllTextAsync(legacyPath, "{}");
            var legacyOwner = new FileInfo(legacyPath).GetAccessControl().GetOwner(typeof(SecurityIdentifier));
            Console.WriteLine($"Request ownership: user={sid}; token default owner={identity.Owner}; plain file owner={legacyOwner}; production owner={acl.GetOwner(typeof(SecurityIdentifier))}.");
            if (identity.Owner is { } tokenOwner && !sid.Equals(tokenOwner))
            {
                Check(tokenOwner.Equals(legacyOwner), "elevated plain FileStream uses token default owner");
                Rejected(() => ApplyBroker.ReadRequest(legacyPath, requestDirectory), 69,
                    "real elevated legacy file reproduces owner rejection");
            }

            foreach (string badPath in new[]
            {
                Path.Combine(root, Path.GetFileName(path)),
                Path.Combine(requestDirectory, "request.json"),
                Path.ChangeExtension(path, ".txt"),
                Path.Combine(requestDirectory, $"{Guid.NewGuid():D}.json")
            })
                Rejected(() => ApplyBroker.ReadRequest(badPath, requestDirectory), 65, "trusted path and filename checks");

            var writableFile = ApplyBroker.PrivateFileSecurity();
            writableFile.AddAccessRule(new FileSystemAccessRule(everyone, FileSystemRights.Write, AccessControlType.Allow));
            new FileInfo(path).SetAccessControl(writableFile);
            Rejected(() => ApplyBroker.ReadRequest(path, requestDirectory), 72, "actual request file untrusted writes rejected");
            new FileInfo(path).SetAccessControl(ApplyBroker.PrivateFileSecurity());

            var writableDirectory = ApplyBroker.PrivateDirectorySecurity();
            writableDirectory.AddAccessRule(new FileSystemAccessRule(everyone, FileSystemRights.Write, AccessControlType.Allow));
            new DirectoryInfo(requestDirectory).SetAccessControl(writableDirectory);
            string existingAcl = new DirectoryInfo(requestDirectory).GetAccessControl()
                .GetSecurityDescriptorSddlForm(AccessControlSections.All);
            int before = Directory.GetFiles(requestDirectory).Length;
            Rejected(() => ApplyBroker.CreateRequestAsync(request, requestDirectory).GetAwaiter().GetResult(), 68,
                "creator rejects existing writable request directory");
            Rejected(() => ApplyBroker.ReadRequest(path, requestDirectory), 68, "reader rejects existing writable request directory");
            Check(Directory.GetFiles(requestDirectory).Length == before, "rejected directory gets no new request");
            Check(new DirectoryInfo(requestDirectory).GetAccessControl().GetSecurityDescriptorSddlForm(AccessControlSections.All) ==
                existingAcl, "existing insecure ACL is not silently repaired");
            new DirectoryInfo(requestDirectory).SetAccessControl(ApplyBroker.PrivateDirectorySecurity());

            var start = new ProcessStartInfo(Environment.GetEnvironmentVariable("ComSpec")!)
            {
                UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true,
                Arguments = $"/d /c mklink /J \"{junction}\" \"{Path.Combine(root, "Session")}\""
            };
            using (var process = Process.Start(start)!)
            {
                await process.WaitForExitAsync();
                Check(process.ExitCode == 0, $"created local reparse regression fixture: {await process.StandardError.ReadToEndAsync()}");
            }
            string redirectedRequests = Path.Combine(junction, "Requests");
            Rejected(() => ApplyBroker.ReadRequest(Path.Combine(redirectedRequests, Path.GetFileName(path)),
                redirectedRequests), 66, "ancestor junction rejected");
            Rejected(() => ApplyBroker.ReadRequest(Path.Combine(junction, Path.GetFileName(path)), junction),
                66, "request directory junction rejected");
            Directory.Delete(junction);

            using (var held = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.None))
                Throws<IOException>(() => ApplyBroker.ReadRequest(path, requestDirectory), "exclusive sharing enforced");
            Check(ApplyBroker.ReadRequest(path, requestDirectory).Expected.Equivalent(request.Expected),
                "exclusive reader releases its handle");

            foreach (int length in new[] { 0, ApplyBroker.MaximumRequestBytes + 1 })
            {
                await File.WriteAllBytesAsync(path, new byte[length]);
                Rejected(() => ApplyBroker.ReadRequest(path, requestDirectory), 76, "empty/oversized request rejected");
            }
            await File.WriteAllBytesAsync(path, new byte[ApplyBroker.MaximumRequestBytes]);
            Throws<JsonException>(() => ApplyBroker.ReadRequest(path, requestDirectory),
                "64 KiB accepted by size guard but malformed JSON still rejected");
            await File.WriteAllTextAsync(path, "not-json");
            Throws<JsonException>(() => ApplyBroker.ReadRequest(path, requestDirectory), "malformed JSON rejected");
            var stale = new ApplyRequest { CreatedUtc = DateTimeOffset.UtcNow.AddMinutes(-10),
                Expected = request.Expected, Desired = request.Desired };
            await File.WriteAllBytesAsync(path, JsonSerializer.SerializeToUtf8Bytes(stale, ControlJsonContext.Default.ApplyRequest));
            Throws<InvalidDataException>(() => ApplyBroker.ReadRequest(path, requestDirectory), "stale request still rejected");

            foreach (var (code, word) in new[] { (65, "path"), (66, "reparse"), (67, "directory owner"),
                (68, "directory permissions"), (69, "file owner"), (72, "file permissions"), (76, "size") })
                Check(ApplyBroker.DescribeExitCode(code).Contains(word, StringComparison.OrdinalIgnoreCase),
                    $"explicit rejection message for exit {code}");
            Console.WriteLine($"PASS: {assertions} production request-file assertions (local files only, no registry access or elevation requested).");
            return assertions;
        }
        finally
        {
            if (Directory.Exists(junction)) Directory.Delete(junction);
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }
}
