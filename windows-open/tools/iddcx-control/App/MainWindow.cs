using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using NvIddCx.Control.Core;
using Windows.Graphics;
using Windows.UI;

namespace NvIddCx.Control;

public sealed partial class MainWindow : Window
{
    private static readonly Color Accent = Color.FromArgb(255, 117, 229, 187);
    private static readonly Color Muted = Color.FromArgb(255, 151, 166, 180);
    private static SolidColorBrush Brush(Color color) => new(color);
    private readonly Grid root = new() { RequestedTheme = ElementTheme.Dark, Background = Brush(Color.FromArgb(255, 13, 20, 29)) };
    private readonly Dictionary<string, ComboBox> editors = new(StringComparer.Ordinal);
    private readonly Dictionary<string, TextBlock> sources = new(StringComparer.Ordinal);
    private readonly Dictionary<string, uint?> draft = Schema.Defaults();
    private readonly StackPanel live = new() { Spacing = 12 };
    private readonly StackPanel controls = new() { Spacing = 8 };
    private readonly TextBlock connection = Text("Not connected", 14);
    private readonly TextBlock appliedSummary = Text("No driver status received.", 12, Muted);
    private readonly TextBlock configSummary = Text("Loading configuration…", 12, Muted);
    private readonly TextBlock effective = Text("", 12, Muted);
    private readonly TextBlock capabilities = Text("Caps unavailable · driver decides unsupported combinations.", 12, Muted);
    private readonly TextBlock preview = Text("MPO / OFF", 20, Accent);
    private readonly InfoBar notice = new() { IsOpen = true, IsClosable = false };
    private readonly Button apply = new() { Content = "Apply configuration", MinWidth = 170 };
    private readonly Button recover = new() { Content = "Recover incomplete write", Visibility = Visibility.Collapsed };
    private readonly Button reload = new() { Content = "Reload", MinWidth = 80 };
    private readonly Button defaults = new() { Content = "Driver defaults" };
    private readonly ComboBox presets = new() { PlaceholderText = "Quick preset", Width = 195 };
    private readonly List<Button> tabs = [];
    private readonly CancellationTokenSource lifetime = new();
    private readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromSeconds(3) };
    private ConfigSnapshot? snapshot;
    private DriverStatus? status;
    private string snapshotError = "";
    private int section;
    private bool suppressChanges;
    private bool busy;
    private bool polling;
    private DateTimeOffset? lastStatusAt;

    public MainWindow()
    {
        Title = "NvIddCx · Display Lab";
        AutomationProperties.SetAutomationId(connection, "TelemetryConnection");
        Content = root;
        IntPtr hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        uint dpi = GetDpiForWindow(hwnd);
        AppWindow.Resize(new SizeInt32((int)(1040 * dpi / 96), (int)(790 * dpi / 96)));
        AppWindow.TitleBar.BackgroundColor = Color.FromArgb(255, 13, 20, 29);
        AppWindow.TitleBar.ForegroundColor = Colors.White;
        AppWindow.TitleBar.ButtonBackgroundColor = Color.FromArgb(255, 13, 20, 29);
        AppWindow.TitleBar.ButtonForegroundColor = Colors.White;
        root.RowDefinitions.Add(new() { Height = GridLength.Auto });
        root.RowDefinitions.Add(new() { Height = new(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new() { Height = GridLength.Auto });
        root.Children.Add(BuildHeader());
        var body = BuildBody();
        Grid.SetRow(body, 1);
        root.Children.Add(body);
        var footer = BuildFooter();
        Grid.SetRow(footer, 2);
        root.Children.Add(footer);
        apply.Click += async (_, _) => await ApplyAsync(false);
        recover.Click += async (_, _) => await ApplyAsync(true);
        reload.Click += async (_, _) =>
        {
            if (HasChanges() && !await ConfirmAsync("Discard unsaved edits?", "Reload will replace your draft with the current registry values.", "Reload"))
                return;
            LoadConfig();
            await PollAsync();
        };
        defaults.Click += (_, _) => SetDraft(Schema.Defaults());
        presets.ItemsSource = new[] { "Native D3D12", "D3D11 reference", "Live showcase" };
        presets.SelectionChanged += (_, _) =>
        {
            if (presets.SelectedItem is string name)
            {
                SetDraft(Schema.Preset(draft, name));
                presets.SelectedIndex = -1;
            }
        };
        timer.Tick += async (_, _) => await PollAsync();
        root.Loaded += async (_, _) =>
        {
            LoadConfig();
            await PollAsync();
            timer.Start();
        };
        Closed += (_, _) => { timer.Stop(); lifetime.Cancel(); };
    }

    private static TextBlock Text(string value, double size = 14, Color? color = null) => new()
    {
        Text = value, FontSize = size, Foreground = Brush(color ?? Colors.White),
        TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center
    };

    private static Border Card(UIElement content, Thickness? padding = null) => new()
    {
        Child = content, Padding = padding ?? new Thickness(18), CornerRadius = new CornerRadius(16),
        Background = Brush(Color.FromArgb(255, 22, 31, 43)),
        BorderBrush = Brush(Color.FromArgb(255, 38, 51, 65)), BorderThickness = new Thickness(1)
    };

    private FrameworkElement BuildHeader()
    {
        var header = new Grid { Margin = new Thickness(24, 14, 24, 18), ColumnSpacing = 16 };
        header.ColumnDefinitions.Add(new() { Width = new(1, GridUnitType.Star) });
        header.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        var title = new StackPanel { Spacing = 4 };
        title.Children.Add(Text("NV / DISPLAY LAB", 12, Accent));
        title.Children.Add(Text("A clearer path to scanout.", 28));
        title.Children.Add(Text("Swapchain tuning & live MPO experiments", 13, Muted));
        header.Children.Add(title);
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, VerticalAlignment = VerticalAlignment.Center };
        actions.Children.Add(presets);
        actions.Children.Add(reload);
        Grid.SetColumn(actions, 1);
        header.Children.Add(actions);
        return header;
    }

    private FrameworkElement BuildBody()
    {
        var body = new Grid { Margin = new Thickness(24, 0, 24, 0), ColumnSpacing = 16 };
        body.ColumnDefinitions.Add(new() { Width = new(232) });
        body.ColumnDefinitions.Add(new() { Width = new(1, GridUnitType.Star) });
        var rail = new StackPanel { Spacing = 14 };
        rail.Children.Add(Text("LIVE TELEMETRY", 11, Accent));
        rail.Children.Add(connection);
        rail.Children.Add(appliedSummary);
        rail.Children.Add(new Border { Height = 1, Background = Brush(Color.FromArgb(255, 45, 60, 75)), Margin = new Thickness(0, 4, 0, 4) });
        rail.Children.Add(Text("ACTUAL OUTPUTS", 11, Muted));
        rail.Children.Add(live);
        rail.Children.Add(Text("CAPABILITIES", 11, Muted));
        rail.Children.Add(capabilities);
        rail.Children.Add(Text("REGISTRY SNAPSHOT", 11, Muted));
        rail.Children.Add(configSummary);
        rail.Children.Add(Text("Requests and actual output are independent. A successful write does not prove the driver applied it.", 12, Muted));
        body.Children.Add(Card(new ScrollViewer { Content = rail, VerticalScrollBarVisibility = ScrollBarVisibility.Auto }));

        var editor = new Grid { RowSpacing = 12 };
        editor.RowDefinitions.Add(new() { Height = GridLength.Auto });
        editor.RowDefinitions.Add(new() { Height = GridLength.Auto });
        editor.RowDefinitions.Add(new() { Height = new(1, GridUnitType.Star) });
        var tabBar = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        foreach (var (label, index) in new[] { ("Swapchain", 0), ("Demo studio", 1), ("Advanced", 2) })
        {
            var button = new Button { Content = label, CornerRadius = new CornerRadius(8), Padding = new Thickness(14, 9, 14, 9) };
            button.Click += (_, _) => { section = index; ShowSection(); };
            tabs.Add(button);
            tabBar.Children.Add(button);
        }
        editor.Children.Add(tabBar);
        Grid.SetRow(effective, 1);
        editor.Children.Add(effective);
        var scroll = new ScrollViewer { Content = controls, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        Grid.SetRow(scroll, 2);
        editor.Children.Add(scroll);
        var panel = Card(editor);
        Grid.SetColumn(panel, 1);
        body.Children.Add(panel);
        foreach (var spec in Schema.Settings)
        {
            var combo = new ComboBox { Width = 220, HorizontalAlignment = HorizontalAlignment.Right,
                ItemsSource = spec.EditorChoices().ToArray() };
            AutomationProperties.SetName(combo, spec.Label);
            ToolTipService.SetToolTip(combo, $"{spec.Name}\n{spec.Description}");
            combo.SelectionChanged += (_, _) =>
            {
                if (suppressChanges) return;
                if (combo.SelectedItem is Choice choice) draft[spec.Name] = choice.Value;
                RefreshValidation();
            };
            editors[spec.Name] = combo;
            sources[spec.Name] = Text("", 11, Muted);
        }
        ShowSection();
        return body;
    }

    private FrameworkElement BuildFooter()
    {
        var footer = new Grid { Margin = new Thickness(24, 16, 24, 20), RowSpacing = 10 };
        footer.RowDefinitions.Add(new() { Height = GridLength.Auto });
        footer.RowDefinitions.Add(new() { Height = GridLength.Auto });
        footer.Children.Add(notice);
        var buttons = new Grid { ColumnSpacing = 8 };
        buttons.ColumnDefinitions.Add(new() { Width = new(1, GridUnitType.Star) });
        buttons.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        buttons.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        buttons.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        buttons.Children.Add(Text("Read-only startup  /  UAC only for one guarded write", 12, Muted));
        Grid.SetColumn(defaults, 1); buttons.Children.Add(defaults);
        Grid.SetColumn(recover, 2); buttons.Children.Add(recover);
        Grid.SetColumn(apply, 3); buttons.Children.Add(apply);
        apply.Background = Brush(Accent);
        apply.Foreground = Brush(Color.FromArgb(255, 11, 23, 24));
        Grid.SetRow(buttons, 1);
        footer.Children.Add(buttons);
        return footer;
    }

    private void ShowSection()
    {
        // Detached WinUI trees can report Parent=null while retaining logical children.
        // Remove reused controls from their owning collections before disconnecting the tree.
        foreach (var child in controls.Children)
        {
            if (child is Grid row)
            {
                foreach (var label in row.Children.OfType<StackPanel>()) label.Children.Clear();
                row.Children.Clear();
            }
            else if (child is Border { Child: StackPanel studio }) studio.Children.Clear();
        }
        controls.Children.Clear();
        for (int i = 0; i < tabs.Count; i++)
        {
            tabs[i].Background = Brush(i == section ? Color.FromArgb(255, 39, 74, 67) : Color.FromArgb(255, 30, 42, 55));
            tabs[i].Foreground = Brush(i == section ? Accent : Muted);
        }
        if (section == 1)
        {
            var studio = new StackPanel { Spacing = 7 };
            studio.Children.Add(Text("MPO / MOTION & COLOR", 11, Muted));
            studio.Children.Add(preview);
            studio.Children.Add(Text("A hardware demo request, not a preview of applied scanout.", 12, Muted));
            controls.Children.Add(new Border { Child = studio, Padding = new Thickness(16),
                CornerRadius = new CornerRadius(12), Background = Brush(Color.FromArgb(255, 24, 53, 51)) });
        }
        string[] names = section switch
        {
            0 => ["SwapChainApi", "D3D12Mode", "D3D11ReadbackMode", "SurfaceCachePolicy", "DirtyRects", "ScanoutBufferCount"],
            1 => ["MpoDemoMode", "MpoDemoConnectorIndex", "MpoDemoPlaneCount", "MpoDemoCadenceHz", "MpoDemoSpeedPercent"],
            _ => ["FramePipeline", "FlipMode", "VrrPreference", "RealtimeGpuPriority", "MmcssDisplayPostProcessing", "D3D12Debug"]
        };
        foreach (string name in names)
        {
            var spec = Schema.ByName[name];
            var combo = editors[name];
            var source = sources[name];
            var row = new Grid { Padding = new Thickness(8, 11, 8, 11), ColumnSpacing = 12 };
            row.ColumnDefinitions.Add(new() { Width = new(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
            var label = new StackPanel { Spacing = 4 };
            label.Children.Add(Text(spec.Label, 14));
            label.Children.Add(source);
            row.Children.Add(label);
            Grid.SetColumn(combo, 1);
            row.Children.Add(combo);
            controls.Children.Add(row);
        }
    }

    private void LoadConfig()
    {
        if (busy) return;
        try
        {
            using var registry = new RegistryStore();
            snapshot = registry.ReadConsistent();
            snapshotError = "";
            SetDraft(snapshot.EditableValues());
            configSummary.Text = snapshot.KeyExists
                ? $"Registry64 · revision {(snapshot.Revision.Present ? snapshot.RevisionNumber.ToString() : "absent (0)")}" +
                    (snapshot.Incomplete ? "\nINCOMPLETE · odd revision" : "\nValues are requested configuration.")
                : "Framework-managed key not found.\nDriver installation is required.";
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException)
        {
            snapshot = null;
            snapshotError = e.Message;
            configSummary.Text = $"Unavailable · {e.Message}";
            RefreshValidation();
        }
    }

    private void SetDraft(IReadOnlyDictionary<string, uint?> values)
    {
        suppressChanges = true;
        foreach (var spec in Schema.Settings)
        {
            draft[spec.Name] = values[spec.Name];
            editors[spec.Name].SelectedItem = ((Choice[])editors[spec.Name].ItemsSource)
                .FirstOrDefault(c => c.Value == values[spec.Name]);
        }
        suppressChanges = false;
        RefreshValidation();
    }

    private bool HasChanges() => snapshot is not null && Schema.Settings.Any(s =>
        draft[s.Name] != (snapshot.Values.TryGetValue(s.Name, out var datum) && datum.IsDword ? datum.Dword : null));

    private List<string> CapabilityErrors()
    {
        var errors = new List<string>();
        var caps = status?.Caps;
        if (caps?.SupportedApis is { } apis && draft["SwapChainApi"] is uint api && api != 0 && !apis.Contains(api))
            errors.Add("The selected API is explicitly unsupported by driver capabilities.");
        if (caps?.SupportedDemos is { } demos && Schema.Effective(draft, "MpoDemoMode") is > 0 and var demo && !demos.Contains(demo))
            errors.Add("The selected demo is explicitly unsupported by driver capabilities.");
        if (caps?.MaxDemoPlanes is uint max && Schema.Effective(draft, "MpoDemoPlaneCount") > max)
            errors.Add($"The driver reports a maximum of {max} demo planes.");
        return errors;
    }

    private void RefreshValidation(bool keepNotice = false)
    {
        var priorNotice = (notice.Severity, notice.Title, notice.Message);
        var errors = snapshot?.Errors() ?? new List<string> { string.IsNullOrEmpty(snapshotError) ? "Registry snapshot unavailable." : snapshotError };
        errors.AddRange(Schema.Validate(draft));
        errors.AddRange(CapabilityErrors());
        apply.IsEnabled = !busy && errors.Count == 0 && HasChanges();
        recover.Visibility = snapshot?.Incomplete == true ? Visibility.Visible : Visibility.Collapsed;
        recover.IsEnabled = !busy && snapshot?.Incomplete == true &&
            snapshot.Errors(true).Count == 0 && Schema.Validate(snapshot.EditableValues()).Count == 0;
        reload.IsEnabled = defaults.IsEnabled = presets.IsEnabled = !busy;
        // Draft editing is safe without a driver installation. Apply still requires a valid existing key.
        bool editable = !busy && snapshot?.Incomplete != true;
        foreach (var spec in Schema.Settings)
        {
            var datum = snapshot?.Values.GetValueOrDefault(spec.Name);
            editors[spec.Name].IsEnabled = editable &&
                (datum?.Present != true || datum.IsDword && spec.Accepts(datum.Dword!.Value)) &&
                (spec.Name != "D3D12Mode" || Schema.Effective(draft, "SwapChainApi") != 11);
            sources[spec.Name].Text = datum?.Present == true && !datum.IsDword
                ? $"INVALID {datum.Kind} · not replaced" : draft[spec.Name] is uint n
                ? $"Explicit DWORD {n}" : $"Absent · {spec.Description}";
            if (spec.Name == "D3D11ReadbackMode" && draft[spec.Name] is null && snapshot?.LegacyReadback.Present == true)
                sources[spec.Name].Text = snapshot.LegacyReadback.IsDword && spec.Accepts(snapshot.LegacyReadback.Dword!.Value)
                    ? $"Absent · legacy ReadbackMode {snapshot.LegacyReadback.Dword} (effective)"
                    : "Absent · INVALID legacy ReadbackMode fallback";
            foreach (var item in editors[spec.Name].Items)
            {
                var container = editors[spec.Name].ContainerFromItem(item) as ComboBoxItem;
                if (container is not null && item is Choice choice) container.IsEnabled = ChoiceSupported(spec.Name, choice.Value);
            }
            editors[spec.Name].DropDownOpened -= UpdateChoiceContainers;
            editors[spec.Name].DropDownOpened += UpdateChoiceContainers;
        }
        effective.Text = $"Effective D3D12 mode: {Schema.ByName["D3D12Mode"].LabelFor(Schema.EffectiveD3D12Mode(draft))}" +
            (Schema.Effective(draft, "SwapChainApi") == 11 ? " · D3D11 selected (stored mode preserved)" : " · requested, not applied");
        preview.Text = Schema.ByName["MpoDemoMode"].LabelFor(Schema.Effective(draft, "MpoDemoMode")).ToUpperInvariant() +
            $"  /  {Schema.Effective(draft, "MpoDemoPlaneCount")} PLANE";
        notice.Severity = errors.Count != 0 ? InfoBarSeverity.Warning : InfoBarSeverity.Informational;
        notice.Title = errors.Count != 0 ? "Review required" : HasChanges() ? "Unsaved draft" : "Configuration loaded";
        notice.Message = errors.Count != 0 ? string.Join(" ", errors.Take(3)) : HasChanges()
            ? "Apply writes only changed known values as one revision group. Driver adoption is shown separately."
            : "Driver defaults preserves absent values. Presets edit this draft only; Apply is a separate action.";
        if (keepNotice && errors.Count == 0)
            (notice.Severity, notice.Title, notice.Message) = priorNotice;
    }

    private bool ChoiceSupported(string name, uint? value)
    {
        if (value is null or 0) return true;
        return name switch
        {
            "SwapChainApi" => status?.Caps?.SupportedApis?.Contains(value.Value) ?? true,
            "MpoDemoMode" => status?.Caps?.SupportedDemos?.Contains(value.Value) ?? true,
            "MpoDemoPlaneCount" => status?.Caps?.MaxDemoPlanes is not uint max || value <= max,
            _ => true
        };
    }

    private void UpdateChoiceContainers(object? sender, object args)
    {
        if (sender is not ComboBox combo) return;
        string name = editors.First(p => ReferenceEquals(p.Value, combo)).Key;
        foreach (var item in combo.Items)
            if (item is Choice choice && combo.ContainerFromItem(item) is ComboBoxItem container)
                container.IsEnabled = ChoiceSupported(name, choice.Value);
    }

    private async Task<bool> ConfirmAsync(string title, string message, string action)
    {
        var dialog = new ContentDialog { XamlRoot = root.XamlRoot, Title = title, Content = message,
            PrimaryButtonText = action, CloseButtonText = "Cancel", DefaultButton = ContentDialogButton.Close,
            RequestedTheme = ElementTheme.Dark };
        return await dialog.ShowAsync() == ContentDialogResult.Primary;
    }

    private async Task ApplyAsync(bool recovery)
    {
        if (busy || snapshot is null) return;
        var expected = snapshot;
        var desired = new Dictionary<string, uint?>(draft, StringComparer.Ordinal);
        string summary = recovery
            ? "Keep the currently stored values and complete the odd revision. This does NOT roll back a partial write. Review all current values before continuing."
            : string.Join("\n", Schema.Settings.Where(s => desired[s.Name] !=
                (expected.Values[s.Name].IsDword ? expected.Values[s.Name].Dword : null)).Select(s =>
                $"{s.Label} → {(desired[s.Name] is uint n ? s.LabelFor(n) :
                    s.Name == "D3D11ReadbackMode" ? "remove value / legacy fallback" : "remove value / driver default")}"));
        if (!await ConfirmAsync(recovery ? "Recover incomplete write?" : "Apply this configuration?",
            summary + "\n\nOne short-lived helper will request administrator permission. No service restart is performed.", recovery ? "Recover" : "Apply"))
            return;
        busy = true;
        RefreshValidation();
        try
        {
            var request = new ApplyRequest { Operation = recovery ? "recover" : "apply", Expected = expected,
                Desired = recovery ? new(StringComparer.Ordinal) : desired };
            int result = await ApplyBroker.SubmitAsync(request);
            busy = false;
            LoadConfig();
            await PollAsync();
            bool verified = result == 0 && snapshot is not null &&
                snapshot.RevisionNumber == RevisionPlan.Create(expected.RevisionNumber, recovery).Complete &&
                snapshot.LegacyReadback == expected.LegacyReadback && !snapshot.Incomplete && (recovery
                    ? Schema.Settings.All(s => snapshot.Values[s.Name] == expected.Values[s.Name])
                    : Schema.Settings.All(s => snapshot.Values[s.Name] ==
                        (desired[s.Name] is uint n ? RegistryDatum.FromDword(n) : RegistryDatum.Absent())));
            notice.Severity = verified ? InfoBarSeverity.Success : InfoBarSeverity.Error;
            notice.Title = verified ? "Registry write verified" : "Apply was not verified";
            notice.Message = verified ? $"Revision {snapshot!.RevisionNumber} committed. Actual driver adoption is independent; see live telemetry."
                : ApplyBroker.DescribeExitCode(result);
        }
        catch (Win32Exception e) when (e.NativeErrorCode == 1223)
        { notice.Severity = InfoBarSeverity.Informational; notice.Title = "Apply cancelled"; notice.Message = "UAC permission was not granted. No helper write was performed."; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException or Win32Exception or InvalidOperationException)
        { notice.Severity = InfoBarSeverity.Error; notice.Title = "Apply failed"; notice.Message = e.Message; }
        finally
        {
            busy = false;
            RefreshValidation(keepNotice: true);
        }
    }

    private async Task PollAsync()
    {
        if (polling) return;
        polling = true;
        try
        {
            var result = await StatusClient.ReadAsync(lifetime.Token);
            if (lifetime.IsCancellationRequested) return;
            status = result.Value;
            live.Children.Clear();
            bool rejected = string.Equals(result.Value?.Status, "rejected", StringComparison.OrdinalIgnoreCase);
            connection.Text = result.Connected ? rejected ? "●  Pipe connected / REJECTED" : "●  Pipe connected" : "○  Not connected";
            connection.Foreground = Brush(rejected ? Color.FromArgb(255, 255, 150, 151) : result.Connected ? Accent : Muted);
            if (result.Value is not DriverStatus current)
            {
                appliedSummary.Text = result.Error + (lastStatusAt is not null
                    ? $"\nLast successful sample {lastStatusAt.Value.ToLocalTime():HH:mm:ss} (stale; not displayed)." : "");
                live.Children.Add(Text("Actual output is unavailable.", 12, Muted));
                capabilities.Text = "Caps unavailable · driver decides unsupported combinations.";
            }
            else
            {
                lastStatusAt = DateTimeOffset.UtcNow;
                appliedSummary.Text = $"{current.Status ?? "Status unavailable"}\n" +
                    $"Generation {Number(current.Generation)} · revision {Number(current.Revision)}\n" +
                    $"{current.Message ?? "Driver message unavailable."}\nSample {lastStatusAt.Value.ToLocalTime():HH:mm:ss}";
                foreach (var output in current.Outputs ?? [])
                {
                    var info = new StackPanel { Spacing = 5 };
                    info.Children.Add(Text($"Connector {Number(output.ConnectorIndex)}  /  {Flag(output.Active, "active", "inactive")}", 13));
                    info.Children.Add(Text($"Applied generation {Number(output.AppliedGeneration)}\n" +
                        $"API {Number(output.Api)} · demo {Number(output.Demo)} · layers {Number(output.Layers)}\n" +
                        $"Rotation {Flag(output.Rotation)} / LUT {Flag(output.Lut)} / gain {Flag(output.Gain)}\n" +
                        output.VrrDiagnostic, 12, Muted));
                    live.Children.Add(Card(info, new Thickness(10)));
                }
                if (current.Outputs is null or { Length: 0 }) live.Children.Add(Text("No output records available.", 12, Muted));
                capabilities.Text = current.Caps is null ? "No explicit caps in status.\nDriver decides unsupported combinations."
                    : $"APIs: {List(current.Caps.SupportedApis)}\nDemos: {List(current.Caps.SupportedDemos)}\nMax demo planes: {Number(current.Caps.MaxDemoPlanes)}";
            }
            RefreshValidation(keepNotice: true);
        }
        finally { polling = false; }
    }

    private static string Number(uint? value) => value?.ToString() ?? "unavailable";
    private static string Flag(bool? value, string yes = "on", string no = "off") => value is null ? "unavailable" : value.Value ? yes : no;
    private static string List(uint[]? values) => values is null ? "unavailable" : values.Length == 0 ? "none" : string.Join(", ", values);

    [LibraryImport("user32.dll")]
    private static partial uint GetDpiForWindow(IntPtr hwnd);
}
