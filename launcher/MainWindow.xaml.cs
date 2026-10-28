using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace YeeHawLauncher;

/// <summary>
/// The launcher window. Every tab is built from <see cref="SettingsCatalog"/>: each
/// setting becomes a row with its name, a short explanation and a control. Nothing is
/// written until Save or "Saddle up!" is pressed.
/// </summary>
public partial class MainWindow : Window
{
    /// <summary>Steam's app ID for Toy Story 3 Complete Edition (as the one-PC test script uses).</summary>
    private const string SteamAppId = "4049180";
    private const string GameExe = "Toy Story 3 Complete Edition.exe";

    private readonly IniFile? _ini;
    /// <summary>Reads each control's current value back into the ini when saving.</summary>
    private readonly List<Action<IniFile>> _writers = new();
    /// <summary>Sets controls from ini values (used by presets and on start).</summary>
    private readonly List<Action> _refreshers = new();
    /// <summary>Each tab's settings panel, built once.</summary>
    private readonly List<StackPanel> _tabPanels = new();
    private readonly List<RadioButton> _tabButtons = new();

    public MainWindow()
    {
        InitializeComponent();
        string? gameDir = FindGameFolder();
        if (gameDir == null)
        {
            StatusText.Text = $"Can't find \"{GameExe}\". Put the launcher in the game folder.";
            SaveButton.IsEnabled = LaunchButton.IsEnabled = false;
        }
        else
        {
            _ini = new IniFile(Path.Combine(gameDir, "xinput_harness.ini"));
            StatusText.Text = File.Exists(_ini.Path) ? "Settings loaded. Howdy!" : "No settings file yet: Save makes one.";
        }
        VersionText.Text = "v" + typeof(MainWindow).Assembly.GetName().Version?.ToString(3);
        BuildTabs();
        ShowTab(0);
    }

    /// <summary>Opens a tab by its position (0 = the first).</summary>
    public void ShowTab(int index)
    {
        if (index < 0 || index >= _tabPanels.Count) return;
        _tabButtons[index].IsChecked = true;
        TabContent.Children.Clear();
        TabContent.Children.Add(_tabPanels[index]);
    }

    /// <summary>
    /// The game folder: the first folder, from the launcher's own upwards, that holds the
    /// game's exe. So the launcher works both next to the game and from harness\launcher\bin.
    /// </summary>
    private static string? FindGameFolder()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
            if (File.Exists(Path.Combine(dir.FullName, GameExe))) return dir.FullName;
        return null;
    }

    private void BuildTabs()
    {
        foreach (var (tab, i) in SettingsCatalog.Tabs.Select((t, i) => (t, i)))
        {
            // Sidebar button: the tab's icon over its name.
            var face = new StackPanel();
            face.Children.Add(new TextBlock { Text = tab.Icon, FontFamily = (FontFamily)FindResource("IconFont"), FontSize = 22,
                                              HorizontalAlignment = HorizontalAlignment.Center });
            face.Children.Add(new TextBlock { Text = tab.Title, FontFamily = (FontFamily)FindResource("TitleFont"), FontSize = 12,
                                              TextAlignment = TextAlignment.Center, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 6, 0, 0) });
            var button = new RadioButton { Content = face, Style = (Style)FindResource("NavButton"), GroupName = "Tabs" };
            int index = i;
            button.Checked += (_, _) => ShowTab(index);
            _tabButtons.Add(button);
            NavBar.Children.Add(button);

            // Page: its name and subtitle, then the presets and groups as cards.
            var panel = new StackPanel();
            panel.Children.Add(new TextBlock { Text = tab.Title, FontFamily = (FontFamily)FindResource("TitleFont"), FontSize = 26, Margin = new Thickness(2, 0, 0, 0) });
            panel.Children.Add(new TextBlock { Text = tab.Subtitle, FontSize = 14, Foreground = (Brush)FindResource("MutedBrush"), Margin = new Thickness(2, 2, 0, 14) });
            if (tab.Presets != null) panel.Children.Add(BuildPresets(tab.Presets));
            foreach (var group in tab.Groups) panel.Children.Add(BuildGroup(group));
            _tabPanels.Add(panel);
        }
        foreach (var refresh in _refreshers) refresh();
    }

    /// <summary>
    /// A row of one-click preset buttons; each writes its values into the controls (not the file).
    /// A preset only changes its own settings: every other control keeps what the player chose,
    /// saved or not, on any page.
    /// </summary>
    private UIElement BuildPresets(Preset[] presets)
    {
        var row = new WrapPanel();
        row.Children.Add(new TextBlock { Text = "Quick picks", FontWeight = FontWeights.SemiBold, FontSize = 15,
                                         VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 16, 0) });
        foreach (var preset in presets)
        {
            var button = new Button { Content = preset.Name, ToolTip = preset.Help, Style = (Style)FindResource("QuietButton"),
                                      FontSize = 14, Padding = new Thickness(18, 6, 18, 6), Margin = new Thickness(0, 0, 8, 0) };
            button.Click += (_, _) =>
            {
                if (_ini == null) return;
                // The controls are reloaded from the in-memory settings below, so first copy every
                // control's current value into them. Otherwise an unsaved change on any page (say
                // Online co-op on Partner Up) would snap back to the last saved value. This only
                // touches memory: the file is still written by Save or Saddle up!.
                foreach (var write in _writers) write(_ini);
                foreach (var (section, key, value) in preset.Values) _ini.Set(section, key, value);
                foreach (var refresh in _refreshers) refresh();
                StatusText.Text = $"\"{preset.Name}\" picked. Press Save or Saddle up! to keep it.";
            };
            row.Children.Add(button);
        }
        return Card(row, new Thickness(20, 14, 20, 14));
    }

    /// <summary>A soft rounded card with a shadow instead of a border.</summary>
    private Border Card(UIElement child, Thickness padding) => new()
    {
        Background = (Brush)FindResource("CardBrush"), CornerRadius = new CornerRadius(14),
        Effect = (System.Windows.Media.Effects.Effect)FindResource("CardShadow"),
        Padding = padding, Margin = new Thickness(0, 0, 0, 14), Child = child,
    };

    /// <summary>A card with a red rope-lettered title (like the stitching on Jessie's shirt) and one row per setting.</summary>
    private UIElement BuildGroup(Group group)
    {
        var rows = new StackPanel();
        var heading = new TextBlock { Text = group.Title, FontSize = 30, Foreground = (Brush)FindResource("RedBrush"), Margin = new Thickness(0, -2, 0, 4) };
        heading.SetResourceReference(TextBlock.FontFamilyProperty, "RopeFont");
        rows.Children.Add(heading);
        foreach (var (setting, i) in group.Items.Select((s, i) => (s, i)))
        {
            if (i > 0) rows.Children.Add(new Border { Height = 1, Background = (Brush)FindResource("DividerBrush") });
            rows.Children.Add(BuildRow(setting));
        }
        return Card(rows, new Thickness(22, 16, 22, 10));
    }

    /// <summary>One setting: name and explanation on the left, its control on the right.</summary>
    private UIElement BuildRow(Setting setting)
    {
        var grid = new Grid { Margin = new Thickness(0, 10, 0, 10) };
        grid.ColumnDefinitions.Add(new ColumnDefinition());
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(220) });

        var text = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
        var name = new StackPanel { Orientation = Orientation.Horizontal };
        name.Children.Add(new TextBlock { Text = setting.Label, FontWeight = FontWeights.SemiBold, FontSize = 15 });
        if (setting.MustMatch)
            name.Children.Add(new Border
            {
                Background = (Brush)FindResource("YellowSoftBrush"), CornerRadius = new CornerRadius(9),
                Padding = new Thickness(8, 1, 8, 2), Margin = new Thickness(10, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center,
                Child = new TextBlock { Text = "match your partner", FontSize = 11, Foreground = (Brush)FindResource("OnYellowBrush") },
            });
        text.Children.Add(name);
        text.Children.Add(new TextBlock { Text = setting.Help, FontSize = 13, TextWrapping = TextWrapping.Wrap,
                                          Foreground = (Brush)FindResource("MutedBrush"), Margin = new Thickness(0, 3, 20, 0) });
        grid.Children.Add(text);

        UIElement control = setting switch
        {
            Toggle t => BuildToggle(t),
            Choice c => BuildChoice(c),
            TextSetting s => BuildText(s),
            _ => new TextBlock(),
        };
        Grid.SetColumn(control, 1);
        grid.Children.Add(control);
        return grid;
    }

    private UIElement BuildToggle(Toggle toggle)
    {
        var box = new CheckBox { Style = (Style)FindResource("Switch"), HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Center };
        // On when every key it writes already holds its "on" value; a missing key counts as the default.
        _refreshers.Add(() => box.IsChecked = toggle.Writes.All(w => (_ini?.Get(w.Section, w.Key) ?? (toggle.Default ? w.On : w.Off)).Trim() == w.On));
        _writers.Add(ini => { foreach (var w in toggle.Writes) ini.Set(w.Section, w.Key, box.IsChecked == true ? w.On : w.Off); });
        return box;
    }

    private UIElement BuildChoice(Choice choice)
    {
        var combo = new ComboBox { VerticalAlignment = VerticalAlignment.Center };
        foreach (var (name, _) in choice.Options) combo.Items.Add(name);
        _refreshers.Add(() =>
        {
            string value = (_ini?.Get(choice.Section, choice.Key) ?? choice.Default).Trim();
            int index = Array.FindIndex(choice.Options, o => string.Equals(o.Value, value, StringComparison.OrdinalIgnoreCase));
            combo.SelectedIndex = index >= 0 ? index : Array.FindIndex(choice.Options, o => o.Value == choice.Default);
        });
        _writers.Add(ini => { if (combo.SelectedIndex >= 0) ini.Set(choice.Section, choice.Key, choice.Options[combo.SelectedIndex].Value); });
        return combo;
    }

    private UIElement BuildText(TextSetting setting)
    {
        var box = new TextBox { VerticalAlignment = VerticalAlignment.Center };
        _refreshers.Add(() => box.Text = _ini?.Get(setting.Section, setting.Key) ?? setting.Default);
        _writers.Add(ini => ini.Set(setting.Section, setting.Key, box.Text.Trim()));
        return box;
    }

    /// <summary>Writes every control's value to the ini. Returns false if there's no game folder.</summary>
    private bool SaveSettings()
    {
        if (_ini == null) return false;
        foreach (var write in _writers) write(_ini);
        _ini.Save();
        return true;
    }

    private void OnSave(object sender, RoutedEventArgs e)
    {
        try
        {
            if (SaveSettings()) StatusText.Text = "Saved! Your settings are in xinput_harness.ini.";
        }
        catch (Exception ex) { StatusText.Text = $"Couldn't save: {ex.Message}"; }
    }

    /// <summary>Saves, then asks Steam to start the game.</summary>
    private void OnLaunch(object sender, RoutedEventArgs e)
    {
        try
        {
            if (!SaveSettings()) return;
            Process.Start(new ProcessStartInfo($"steam://rungameid/{SteamAppId}") { UseShellExecute = true });
            StatusText.Text = "Yee haw! Starting the game through Steam...";
        }
        catch (Exception ex) { StatusText.Text = $"Couldn't start the game: {ex.Message}"; }
    }
}
