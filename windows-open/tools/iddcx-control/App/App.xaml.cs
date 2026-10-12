using Microsoft.UI.Xaml;

namespace NvIddCx.Control;

public partial class App : Application
{
    private Window? window;
    public App()
    {
        UnhandledException += (_, args) => Program.RecordFailure(args.Exception);
        InitializeComponent();
    }
    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        window = new MainWindow();
        window.Activate();
    }
}
