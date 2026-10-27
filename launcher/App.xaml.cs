using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace YeeHawLauncher;

/// <summary>
/// Starts the launcher window. With "--screenshot &lt;file.png&gt; [tab]" it instead draws
/// the window to a picture and exits, so the look can be checked without clicking around.
/// With "--logo &lt;file.png&gt;" it draws the project's 1280x640 title image (TitleArt), and with
/// "--icon &lt;file.png&gt;" its 512x512 square icon (IconArt), and exits.
/// </summary>
public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        foreach (var (flag, make) in new (string, Func<FrameworkElement>)[] { ("--logo", () => new TitleArt()), ("--icon", () => new IconArt()) })
        {
            int at = Array.IndexOf(e.Args, flag);
            if (at < 0 || at + 1 >= e.Args.Length) continue;
            var art = make();
            art.Measure(new Size(art.Width, art.Height));
            art.Arrange(new Rect(0, 0, art.Width, art.Height));
            art.UpdateLayout();
            SavePng(art, art.Width, art.Height, 96, e.Args[at + 1]);
            Shutdown();
            return;
        }
        var window = new MainWindow();
        int shot = Array.IndexOf(e.Args, "--screenshot");
        if (shot >= 0 && shot + 1 < e.Args.Length)
        {
            string file = e.Args[shot + 1];
            if (shot + 2 < e.Args.Length && int.TryParse(e.Args[shot + 2], out int tab)) window.ShowTab(tab);
            window.ContentRendered += (_, _) =>
            {
                SaveScreenshot(window, file);
                Shutdown();
            };
        }
        window.Show();
    }

    /// <summary>Renders the window's content into a PNG file.</summary>
    private static void SaveScreenshot(Window window, string file)
    {
        var content = (FrameworkElement)window.Content;
        SavePng(content, content.ActualWidth, content.ActualHeight, VisualTreeHelper.GetDpi(content).PixelsPerInchX, file);
    }

    /// <summary>Renders a laid-out visual of the given size (in 1/96 inch units) into a PNG file.</summary>
    private static void SavePng(Visual visual, double width, double height, double dpi, string file)
    {
        var bitmap = new RenderTargetBitmap((int)(width * dpi / 96), (int)(height * dpi / 96), dpi, dpi, PixelFormats.Pbgra32);
        bitmap.Render(visual);
        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using var stream = File.Create(file);
        encoder.Save(stream);
    }
}
