// SPDX-License-Identifier: GPL-2.0-or-later
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.System;
using Windows.UI.Core;

namespace MediaViewer.Chrome;

/// <summary>
/// Settings as a full-client screen in the command-bar island: view defaults
/// and a live remap of the same table <c>?</c> and the router use.
/// </summary>
public static partial class IslandHost
{
    private static Grid? _chromeRoot;
    private static Grid? _settingsHost;
    private static StackPanel? _keyList;
    private static TextBox? _keySearch;
    private static TextBlock? _keyEmpty;
    private static ToggleSwitch? _stripFolder;
    private static ToggleSwitch? _stripImage;
    private static ToggleSwitch? _wrap;
    private static ToggleSwitch? _sticky;
    private static ComboBox? _background;
    private static int _capturingRow = -1;
    private static bool _updatingSettingsUi;
    private static bool _settingsVisible;
    private static Button? _captureButton;
    private static Button? _cancelCapture;
    private static TextBlock? _captureHint;
    private static VirtualKey? _consumedCaptureKey;
    private static readonly Dictionary<int, Button> KeyButtons = new();
    private const string CaptureInstructions = "Choose a shortcut, then press its replacement. Esc cancels. Conflicts swap shortcuts.";

    private static Button SettingsButton(string text, Action action)
    {
        Button button = TextButton(text, action);
        button.AllowFocusOnInteraction = true;
        return button;
    }

    private static Grid BuildSettingsScreen()
    {
        KeyButtons.Clear();
        var view = new StackPanel { Spacing = 12, Padding = new Thickness(20, 16, 20, 16), Width = 320 };
        view.Children.Add(Heading("View"));
        _stripFolder = SettingsToggle("Filmstrip when opening a folder", SettingFlag.FilmstripForFolder);
        _stripImage = SettingsToggle("Filmstrip when opening an image", SettingFlag.FilmstripForImage);
        _wrap = SettingsToggle("Wrap at the end of the folder", SettingFlag.Wrap);
        _sticky = SettingsToggle("Sticky zoom (keep pan and zoom on next)", SettingFlag.StickyZoom);
        view.Children.Add(_stripFolder);
        view.Children.Add(_stripImage);
        view.Children.Add(_wrap);
        view.Children.Add(_sticky);
        view.Children.Add(Label("Canvas background"));
        _background = new ComboBox
        {
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Title),
            MinWidth = 200,
        };
        _background.Items.Add("Dark");
        _background.Items.Add("Gray");
        _background.Items.Add("White");
        _background.Items.Add("Checkerboard");
        _background.SelectedIndex = (_settingFlags & SettingFlag.BackgroundMask) >> SettingFlag.BackgroundShift;
        _background.SelectionChanged += (_, _) =>
        {
            if (_updatingSettingsUi || _background.SelectedIndex < 0) return;
            int next = (_settingFlags & ~SettingFlag.BackgroundMask) |
                       ((_background.SelectedIndex & 3) << SettingFlag.BackgroundShift);
            Send(Command.SetSettings, next);
        };
        view.Children.Add(_background);

        var keysHeader = new Grid { Margin = new Thickness(0, 0, 0, 8) };
        keysHeader.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        keysHeader.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var keysTitle = Heading("Keyboard");
        Grid.SetColumn(keysTitle, 0);
        keysHeader.Children.Add(keysTitle);
        Button reset = SettingsButton("Reset to default", () =>
        {
            CancelKeyCapture(restoreFocus: false);
            Send(Command.ResetKeys);
            SetCaptureHint("Default shortcuts restored.");
        });
        Grid.SetColumn(reset, 1);
        keysHeader.Children.Add(reset);

        _keySearch = new TextBox
        {
            PlaceholderText = "Search commands or keys",
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Title),
            Margin = new Thickness(12, 0, 20, 8),
        };
        _keySearch.TextChanged += (_, _) => FilterSettingsKeys();
        _keySearch.KeyDown += (_, e) =>
        {
            if (e.Key != VirtualKey.Escape || string.IsNullOrEmpty(_keySearch.Text)) return;
            _keySearch.Text = "";
            e.Handled = true;
        };
        _keyList = new StackPanel { Spacing = 2 };
        _keyEmpty = new TextBlock
        {
            Text = "No matching shortcuts",
            Foreground = Brush(Body),
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Margin = new Thickness(12, 8, 20, 8),
            Visibility = Visibility.Collapsed,
        };
        var keyStack = new StackPanel();
        keyStack.Children.Add(_keyList);
        keyStack.Children.Add(_keyEmpty);
        var keyScroll = new ScrollViewer
        {
            Content = keyStack,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
        };
        var keysCol = new Grid();
        keysCol.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        keysCol.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        keysCol.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        Grid.SetRow(keysHeader, 0);
        keysCol.Children.Add(keysHeader);
        Grid.SetRow(_keySearch, 1);
        keysCol.Children.Add(_keySearch);
        Grid.SetRow(keyScroll, 2);
        keysCol.Children.Add(keyScroll);

        var split = new Grid();
        split.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        split.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1) });
        split.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        Grid.SetColumn(view, 0);
        split.Children.Add(view);
        var rule = new Border { Background = Brush(Hairline), Width = 1, Margin = new Thickness(0, 8, 0, 8) };
        Grid.SetColumn(rule, 1);
        split.Children.Add(rule);
        Grid.SetColumn(keysCol, 2);
        split.Children.Add(keysCol);

        var footer = new Grid
        {
            ColumnSpacing = 12,
            Padding = new Thickness(20, 8, 20, 12),
        };
        footer.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        footer.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        footer.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        footer.Children.Add(SettingsButton("Close  Esc", () => Send(Command.OpenSettings)));
        _captureHint = new TextBlock
        {
            Text = CaptureInstructions,
            Foreground = Brush(Body),
            FontFamily = UiFont,
            FontSize = UiFontSize,
            VerticalAlignment = VerticalAlignment.Center,
            TextWrapping = TextWrapping.Wrap,
        };
        Grid.SetColumn(_captureHint, 1);
        footer.Children.Add(_captureHint);
        _cancelCapture = SettingsButton("Cancel change", () => CancelKeyCapture());
        _cancelCapture.Visibility = Visibility.Collapsed;
        Grid.SetColumn(_cancelCapture, 2);
        footer.Children.Add(_cancelCapture);

        var root = new Grid { Background = Brush(Canvas) };
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        Grid.SetRow(split, 0);
        root.Children.Add(split);
        var footRule = new Border { Background = Brush(Hairline), Height = 1, VerticalAlignment = VerticalAlignment.Top };
        Grid.SetRow(footRule, 1);
        root.Children.Add(footRule);
        Grid.SetRow(footer, 1);
        root.Children.Add(footer);
        root.IsTabStop = true;
        return root;
    }

    private static TextBlock Heading(string text) => new()
    {
        Text = text,
        Foreground = Brush(Title),
        FontFamily = UiFont,
        FontSize = UiFontSize + 2,
        Margin = new Thickness(0, 0, 0, 4),
    };

    private static TextBlock Label(string text) => new()
    {
        Text = text,
        Foreground = Brush(Body),
        FontFamily = UiFont,
        FontSize = UiFontSize,
    };

    private static ToggleSwitch SettingsToggle(string header, int flag)
    {
        var toggle = new ToggleSwitch
        {
            Header = header,
            IsOn = HasFlag(flag),
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Title),
        };
        toggle.Toggled += (_, _) =>
        {
            if (_updatingSettingsUi) return;
            SetFlag(flag, toggle.IsOn);
        };
        return toggle;
    }

    private static void ShowSettingsScreen()
    {
        if (_chromeRoot is null || _settingsHost is null) return;
        _settingsVisible = true;
        _consumedCaptureKey = null;
        if (_keySearch is not null) _keySearch.Text = "";
        RefreshSettingsScreen();
        _settingsHost.Visibility = Visibility.Visible;
        _chromeRoot.UpdateLayout();
        FocusSettings();
        _dispatcher?.DispatcherQueue.TryEnqueue(() =>
        {
            if (_settingsVisible) FocusSettings();
        });
    }

    private static void HideSettingsScreen()
    {
        _settingsVisible = false;
        CancelKeyCapture(restoreFocus: false);
        _consumedCaptureKey = null;
        if (_chromeRoot is null || _settingsHost is null) return;
        _settingsHost.Visibility = Visibility.Collapsed;
    }

    private static void FocusSettings()
    {
        if (!_settingsVisible) return;
        if (_capturingRow >= 0) _captureButton?.Focus(FocusState.Keyboard);
        else _stripFolder?.Focus(FocusState.Programmatic);
    }

    private static void OnSettingsKeyUp(object sender, KeyRoutedEventArgs e)
    {
        if (!_settingsVisible) return;
        if (_capturingRow >= 0) e.Handled = true;
        if (_consumedCaptureKey != e.OriginalKey) return;
        e.Handled = true;
        _consumedCaptureKey = null;
    }

    private static void OnSettingsNavigationKeyDown(object sender, KeyRoutedEventArgs e)
    {
        _ = sender;
        if (!_settingsVisible || e.Handled) return;
        if (e.Key == VirtualKey.Escape && _capturingRow < 0)
        {
            Send(Command.OpenSettings);
            e.Handled = true;
        }
    }

    private static void RefreshSettingsScreen()
    {
        _updatingSettingsUi = true;
        try
        {
            if (_stripFolder is not null) _stripFolder.IsOn = HasFlag(SettingFlag.FilmstripForFolder);
            if (_stripImage is not null) _stripImage.IsOn = HasFlag(SettingFlag.FilmstripForImage);
            if (_wrap is not null) _wrap.IsOn = HasFlag(SettingFlag.Wrap);
            if (_sticky is not null) _sticky.IsOn = HasFlag(SettingFlag.StickyZoom);
            if (_background is not null)
            {
                _background.SelectedIndex =
                    (_settingFlags & SettingFlag.BackgroundMask) >> SettingFlag.BackgroundShift;
            }
        }
        finally
        {
            _updatingSettingsUi = false;
        }
        RefreshSettingsKeys();
    }

    private static void RefreshSettingsKeys()
    {
        if (_keyList is null) return;
        var seen = new HashSet<int>();
        foreach (CommandRow row in CommandRows)
        {
            if (row.Row < 0 || !seen.Add(row.Row)) continue;
            if (KeyButtons.TryGetValue(row.Row, out Button? existing))
            {
                SetButtonText(existing, _capturingRow == row.Row ? "Press shortcut…" : KeysForRow(row.Row));
                continue;
            }
            int captured = row.Row;
            var line = new Grid { Margin = new Thickness(12, 2, 20, 2) };
            line.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            line.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(180) });
            var name = new TextBlock
            {
                Text = row.Name,
                Foreground = Brush(Title),
                FontFamily = UiFont,
                FontSize = UiFontSize,
                VerticalAlignment = VerticalAlignment.Center,
            };
            Grid.SetColumn(name, 0);
            line.Children.Add(name);
            Button bind = SettingsButton(KeysForRow(captured), () =>
            {
                CancelKeyCapture(restoreFocus: false);
                _capturingRow = captured;
                _captureButton = KeyButtons[captured];
                SetButtonText(_captureButton, "Press shortcut…");
                SetCaptureHint($"Changing {row.Name}. Press a new combination, or Esc to cancel.");
                if (_cancelCapture is not null) _cancelCapture.Visibility = Visibility.Visible;
                _captureButton.Focus(FocusState.Keyboard);
            });
            bind.HorizontalAlignment = HorizontalAlignment.Stretch;
            bind.BorderThickness = new Thickness(1);
            bind.BorderBrush = Brush(Hairline);
            bind.LostFocus += (_, _) =>
            {
                if (_capturingRow == captured) CancelKeyCapture(restoreFocus: false);
            };
            KeyButtons.Add(captured, bind);
            Grid.SetColumn(bind, 1);
            line.Children.Add(bind);
            _keyList.Children.Add(line);
        }
        FilterSettingsKeys();
    }

    private static void FilterSettingsKeys()
    {
        if (_keyList is null) return;
        string q = (_keySearch?.Text ?? "").Trim();
        int shown = 0;
        foreach (UIElement child in _keyList.Children)
        {
            if (child is not Grid line) continue;
            string hay = "";
            if (line.Children.Count > 0 && line.Children[0] is TextBlock name) hay += name.Text;
            if (line.Children.Count > 1 && line.Children[1] is Button bind &&
                bind.Content is TextBlock keys) hay += " " + keys.Text;
            bool match = q.Length == 0 ||
                         hay.Contains(q, StringComparison.OrdinalIgnoreCase);
            line.Visibility = match ? Visibility.Visible : Visibility.Collapsed;
            if (match) shown++;
        }
        if (_keyEmpty is not null)
            _keyEmpty.Visibility = shown == 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private static void OnSettingsKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (!_settingsVisible) return;
        if (_consumedCaptureKey == e.OriginalKey) { e.Handled = true; return; }
        if (_capturingRow < 0) return;
        e.Handled = true;  // includes modifiers and unsupported keys
        if (e.Key == VirtualKey.Escape)
        {
            _consumedCaptureKey = e.OriginalKey;
            CancelKeyCapture();
            return;
        }
        bool shift = Down(VirtualKey.Shift);
        int k = EncodeCaptureKey(e, shift);
        if (k <= 0) return;
        int mods = 0;
        if (Down(VirtualKey.Control)) mods |= 1;
        if (shift) mods |= 2;
        if (Down(VirtualKey.Menu)) mods |= 4;
        // Shift that produced a symbol is not a modifier on the table (`?`, `+`).
        if (k >= 0x21 && k <= 0x7E && (k < '0' || k > '9') && (k < 'A' || k > 'Z'))
            mods &= ~2;
        int packed = (_capturingRow & 0xFF) | (k << 8) | ((mods & 7) << 20);
        int row = _capturingRow;
        _consumedCaptureKey = e.OriginalKey;
        _capturingRow = -1;
        _captureButton = null;
        if (_cancelCapture is not null) _cancelCapture.Visibility = Visibility.Collapsed;
        Send(Command.Rebind, packed);
        RefreshSettingsKeys();
        SetCaptureHint($"Shortcut updated to {KeysForRow(row)}.");
    }

    private static string KeysForRow(int index) => string.Join("  /  ",
        CommandRows.Where(row => row.Row == index).Select(row => row.Keys).Distinct());

    private static void SetButtonText(Button button, string text)
    {
        if (button.Content is TextBlock label) label.Text = text;
    }

    private static void SetCaptureHint(string text)
    {
        if (_captureHint is not null) _captureHint.Text = text;
    }

    private static void CancelKeyCapture(bool restoreFocus = true)
    {
        Button? previous = _captureButton;
        int row = _capturingRow;
        _capturingRow = -1;
        _captureButton = null;
        if (previous is not null) SetButtonText(previous, KeysForRow(row));
        if (_cancelCapture is not null) _cancelCapture.Visibility = Visibility.Collapsed;
        SetCaptureHint(CaptureInstructions);
        if (restoreFocus) previous?.Focus(FocusState.Keyboard);
    }

    private static bool Down(VirtualKey vk) =>
        InputKeyboardSource.GetKeyStateForCurrentThread(vk).HasFlag(CoreVirtualKeyStates.Down);

    // Host key codes (commands.h): letters are the character, named keys live
    // at 0x100. Same translation the native edge uses, so a remap round-trips.
    private static int EncodeCaptureKey(KeyRoutedEventArgs e, bool shift)
    {
        VirtualKey vk = e.OriginalKey;
        int v = (int)vk;
        if (v >= (int)VirtualKey.A && v <= (int)VirtualKey.Z) return v;
        if (v >= (int)VirtualKey.Number0 && v <= (int)VirtualKey.Number9) return v;
        return vk switch
        {
            VirtualKey.Space => 0x100,
            VirtualKey.Back => 0x101,
            VirtualKey.Enter => 0x102,
            VirtualKey.Escape => 0x103,
            VirtualKey.Tab => 0x104,
            VirtualKey.Insert => 0x105,
            VirtualKey.Delete => 0x106,
            VirtualKey.Home => 0x107,
            VirtualKey.End => 0x108,
            VirtualKey.PageUp => 0x109,
            VirtualKey.PageDown => 0x10A,
            VirtualKey.Left => 0x10B,
            VirtualKey.Right => 0x10C,
            VirtualKey.Up => 0x10D,
            VirtualKey.Down => 0x10E,
            VirtualKey.F1 => 0x10F,
            VirtualKey.F2 => 0x110,
            VirtualKey.F3 => 0x111,
            VirtualKey.F4 => 0x112,
            VirtualKey.F5 => 0x113,
            VirtualKey.F6 => 0x114,
            VirtualKey.F7 => 0x115,
            VirtualKey.F8 => 0x116,
            VirtualKey.F9 => 0x117,
            VirtualKey.F10 => 0x118,
            VirtualKey.F11 => 0x119,
            VirtualKey.F12 => 0x11A,
            VirtualKey.Add => (int)'+',
            VirtualKey.Subtract => (int)'-',
            _ => v switch
            {
                187 => shift ? (int)'+' : (int)'=',  // VK_OEM_PLUS
                189 => (int)'-',                     // VK_OEM_MINUS
                188 => (int)',',                     // VK_OEM_COMMA
                190 => (int)'.',                     // VK_OEM_PERIOD
                191 => shift ? (int)'?' : (int)'/',  // VK_OEM_2
                220 => (int)'\\',                    // VK_OEM_5
                _ => 0,
            },
        };
    }
}
