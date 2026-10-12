using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;

namespace NvIddCx.Control;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length != 0)
            return args.Length == 2 && args[0] == "--apply-request" ? ApplyBroker.RunHelper(args[1]) : 64;
        WinRT.ComWrappersSupport.InitializeComWrappers();
        Application.Start(parameters =>
        {
            try
            {
                SynchronizationContext.SetSynchronizationContext(
                    new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread()));
                _ = new App();
            }
            catch (Exception error)
            {
                RecordFailure(error);
                Environment.Exit(70);
            }
        });
        return 0;
    }

    internal static void RecordFailure(Exception error)
    {
        try { File.WriteAllText(Path.Combine(AppContext.BaseDirectory, "NvIddCxControl.error.log"), error.ToString()); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }
}
