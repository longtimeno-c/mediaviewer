// SPDX-License-Identifier: GPL-2.0-or-later
using Microsoft.UI;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
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
    private static FakeInput? _keyFilterInput;
    private static TextBlock? _keyEmpty;
    private static string _keyFilter = "";
    private static ToggleSwitch? _stripFolder;
    private static ToggleSwitch? _stripImage;
    private static ToggleSwitch? _wrap;
    private static ToggleSwitch? _sticky;
    private static ComboBox? _background;
    private static ComboBox? _sortKey;
    private static ToggleSwitch? _sortDescending;
    private static int _capturingRow = -1;
    private static bool _updatingSettingsUi;
    private static bool _settingsVisible;
    private static bool _settingsKeyboard;
    private static Button? _captureButton;
    private static Button? _cancelCapture;
    private static TextBlock? _captureHint;
    private static VirtualKey? _consumedCaptureKey;
    private static readonly Dictionary<int, Button> KeyButtons = new();
    private const string CaptureInstructions = "Choose a shortcut, then press its replacement. Esc cancels. Conflicts swap shortcuts.";
    private const string FilterPrompt = "Search commands or keys";

    private static Button SettingsButton(string text, Action action)
    {
        Button button = TextButton(text, action);
        button.AllowFocusOnInteraction = true;
        return button;
    }

    private static Grid BuildSettingsScreen()
    {
        KeyButtons.Clear();
        var view = new StackPanel { Spacing = 8, Padding = new Thickness(24, 12, 24, 24), MaxWidth = 800,
            HorizontalAlignment = HorizontalAlignment.Stretch };
        view.Children.Add(SettingsSection("Filmstrip"));
        _stripFolder = SettingsToggle("When opening a folder", SettingFlag.FilmstripForFolder);
        _stripImage = SettingsToggle("When opening an image", SettingFlag.FilmstripForImage);
        _wrap = SettingsToggle("Wrap at the end", SettingFlag.Wrap);
        _sticky = SettingsToggle("Keep pan and zoom", SettingFlag.StickyZoom);
        view.Children.Add(SettingsRow("When opening a folder", "Show thumbnails below the viewer.", _stripFolder));
        view.Children.Add(SettingsRow("When opening an image", "Show nearby images from the same folder.", _stripImage));
        view.Children.Add(SettingsSection("Browsing"));
        view.Children.Add(SettingsRow("Wrap at the end", "Continue from the last item to the first.", _wrap));
        view.Children.Add(SettingsRow("Keep pan and zoom", "Keep your view position when moving to the next item.", _sticky));
        _background = new ComboBox
        {
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Title),
            Width = 180,
        };
        _background.Items.Add("Dark");
        _background.Items.Add("Grey");
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

        _sortKey = new ComboBox
        {
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Title),
            Width = 180,
        };
        foreach (string name in SortNames) _sortKey.Items.Add(name);
        _sortKey.SelectedIndex = Math.Clamp(_sortPacked & 7, 0, SortNames.Length - 1);
        _sortKey.SelectionChanged += (_, _) =>
        {
            if (_updatingSettingsUi || _sortKey.SelectedIndex < 0) return;
            Send(Command.SetSort, (_sortPacked & 8) | _sortKey.SelectedIndex);
        };
        view.Children.Add(SettingsRow("Sort folder by", "Applies to the gallery and filmstrip.", _sortKey));
        _sortDescending = new ToggleSwitch
        {
            FontFamily = UiFont,
            FontSize = UiFontSize,
            IsOn = (_sortPacked & 8) != 0,
        };
        _sortDescending.Toggled += (_, _) =>
        {
            if (_updatingSettingsUi) return;
            Send(Command.SetSort, _sortDescending.IsOn ? _sortPacked | 8 : _sortPacked & ~8);
        };
        view.Children.Add(SettingsRow("Descending order", "Reverse the selected sort order.", _sortDescending));
        view.Children.Add(SettingsSection("Appearance"));
        view.Children.Add(SettingsRow("Canvas background", "The area behind your photos and videos.", _background));
        view.Children.Add(SettingsSection("Updates and privacy"));
        AddUpdateSettingsRow(view);
        AddTelemetrySettingsRow(view);
        var addons = new StackPanel { Spacing = 8, Margin = new Thickness(0, 20, 0, 0) };
        AddAddonsSettingsRow(addons);
        view.Children.Add(addons);

        var keysHeader = new Grid { Margin = new Thickness(0, 0, 0, 8) };
        keysHeader.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        keysHeader.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var keysTitle = Heading("Keyboard shortcuts");
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

        // No TextBox: that control fail-fasts in this island (0xC000027B).
        FakeInput filter = new FakeInput(FilterPrompt)
        {
            Margin = new Thickness(0, 0, 0, 8),
        };
        filter.Changed += () => SetKeyFilter(filter.Text);
        filter.MoveDown += FocusFirstVisibleKey;
        _keyFilterInput = filter;
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
        var keysCol = new Grid { Padding = new Thickness(24), MaxWidth = 900 };
        keysCol.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        keysCol.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        keysCol.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        Grid.SetRow(keysHeader, 0);
        keysCol.Children.Add(keysHeader);
        Grid.SetRow(_keyFilterInput, 1);
        keysCol.Children.Add(_keyFilterInput);
        Grid.SetRow(keyScroll, 2);
        keysCol.Children.Add(keyScroll);

        var viewScroll = new ScrollViewer
        {
            Content = view,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
        };
        var content = new Grid();
        content.Children.Add(viewScroll);
        content.Children.Add(keysCol);

        var header = new Grid { Padding = new Thickness(24, 16, 24, 16), ColumnSpacing = 24 };
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var title = Heading("Settings");
        title.FontSize = 24;
        header.Children.Add(title);
        var tabs = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        var generalTab = new ToggleButton { Content = "General", FontFamily = UiFont, FontSize = UiFontSize };
        var keysTab = new ToggleButton { Content = "Keyboard shortcuts", FontFamily = UiFont, FontSize = UiFontSize };
        tabs.Children.Add(generalTab);
        tabs.Children.Add(keysTab);
        Grid.SetColumn(tabs, 1);
        header.Children.Add(tabs);
        void SelectCategory(bool keyboard, bool focus)
        {
            CancelKeyCapture(restoreFocus: false);
            _settingsKeyboard = keyboard;
            generalTab.IsChecked = !keyboard;
            keysTab.IsChecked = keyboard;
            viewScroll.Visibility = keyboard ? Visibility.Collapsed : Visibility.Visible;
            keysCol.Visibility = keyboard ? Visibility.Visible : Visibility.Collapsed;
            SetCaptureHint(keyboard ? CaptureInstructions : "Changes are saved automatically.");
            if (focus) FocusSettings();
        }
        generalTab.Click += (_, _) => SelectCategory(false, true);
        keysTab.Click += (_, _) => SelectCategory(true, true);

        var footer = new Grid
        {
            ColumnSpacing = 12,
            Padding = new Thickness(20, 8, 20, 12),
        };
        footer.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        footer.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        footer.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        footer.Children.Add(SettingsButton("Done", () => Send(Command.OpenSettings)));
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

        var root = new Grid
        {
            Background = Brush(Canvas),
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Stretch,
        };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.Children.Add(header);
        Grid.SetRow(content, 1);
        root.Children.Add(content);
        var footRule = new Border { Background = Brush(Hairline), Height = 1, VerticalAlignment = VerticalAlignment.Top };
        Grid.SetRow(footRule, 2);
        root.Children.Add(footRule);
        Grid.SetRow(footer, 2);
        root.Children.Add(footer);
        root.IsTabStop = true;
        SelectCategory(_settingsKeyboard, false);
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

    private static TextBlock SettingsSection(string title)
    {
        var heading = Heading(title);
        heading.Margin = new Thickness(0, 16, 0, 4);
        return heading;
    }

    private static FrameworkElement SettingsRow(string title, string detail, FrameworkElement control)
    {
        var row = new Grid { Padding = new Thickness(14), ColumnSpacing = 24,
            Background = Brush(ChromeColour.Surface), CornerRadius = new CornerRadius(8) };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var labels = new StackPanel { Spacing = 4, VerticalAlignment = VerticalAlignment.Center };
        var name = Label(title);
        name.Foreground = Brush(Title);
        name.TextWrapping = TextWrapping.Wrap;
        var description = Label(detail);
        description.FontSize = 12;
        description.TextWrapping = TextWrapping.Wrap;
        labels.Children.Add(name);
        labels.Children.Add(description);
        row.Children.Add(labels);
        if (control is ToggleSwitch toggle)
        {
            toggle.Header = null;
            toggle.OnContent = "";
            toggle.OffContent = "";
            toggle.MinWidth = 0;
            toggle.Width = 48;
        }
        control.VerticalAlignment = VerticalAlignment.Center;
        AutomationProperties.SetName(control, title);
        AutomationProperties.SetHelpText(control, detail);
        Grid.SetColumn(control, 1);
        row.Children.Add(control);
        return row;
    }

    private static ToggleSwitch SettingsToggle(string header, int flag)
    {
        var toggle = new ToggleSwitch
        {
            IsOn = HasFlag(flag),
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Title),
        };
        AutomationProperties.SetName(toggle, header);
        toggle.Toggled += (_, _) =>
        {
            if (_updatingSettingsUi) return;
            SetFlag(flag, toggle.IsOn);
        };
        return toggle;
    }

    private static void ShowSettingsScreen()
    {
        _settingsVisible = true;
        _consumedCaptureKey = null;
        SetKeyFilter("");
        // Native resizes the island to the full client, then calls us. Wait one
        // tick so that MoveAndResize has been applied; building into the 48 DIP
        // bar left this screen blank (star row height 0).
        if (_dispatcher is not null)
            _dispatcher.DispatcherQueue.TryEnqueue(PresentSettings);
        else
            PresentSettings();
    }

    private static void PresentSettings()
    {
        if (!_settingsVisible || _chromeRoot is null) return;
        try
        {
            if (_settingsHost is null)
            {
                _settingsHost = BuildSettingsScreen();
                _settingsHost.HorizontalAlignment = HorizontalAlignment.Stretch;
                _settingsHost.VerticalAlignment = VerticalAlignment.Stretch;
                Grid.SetRow(_settingsHost, 0);
                Grid.SetRowSpan(_settingsHost, 4);
                _chromeRoot.Children.Add(_settingsHost);
            }
            RefreshSettingsScreen();
            _settingsHost.Visibility = Visibility.Visible;
            _chromeRoot.UpdateLayout();
            FocusSettings();
            // Island focus can miss on the first try after MoveAndResize.
            if (_dispatcher is not null)
                _dispatcher.DispatcherQueue.TryEnqueue(() => { if (_settingsVisible) FocusSettings(); });
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
        }
    }

    private static void HideSettingsScreen()
    {
        _settingsVisible = false;
        CancelKeyCapture(restoreFocus: false);
        _consumedCaptureKey = null;
        SetKeyFilter("");
        if (_chromeRoot is null || _settingsHost is null) return;
        _settingsHost.Visibility = Visibility.Collapsed;
    }

    private static void FocusSettings()
    {
        if (!_settingsVisible) return;
        // Ctrl+, opens this from the canvas HWND. Steal focus into the island
        // so ContentPreTranslateMessage delivers keys to the filter field.
        if (_source?.SiteBridge is not null)
        {
            IntPtr hwnd = Win32Interop.GetWindowFromWindowId(_source.SiteBridge.WindowId);
            if (hwnd != IntPtr.Zero) SetFocus(hwnd);
        }
        if (_capturingRow >= 0) _captureButton?.Focus(FocusState.Keyboard);
        else if (_settingsKeyboard && _keyFilterInput is not null) _keyFilterInput.Focus(FocusState.Keyboard);
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
        if (_capturingRow >= 0) return;
        if (e.Key == VirtualKey.Escape)
        {
            if (_settingsKeyboard && _keyFilter.Length > 0)
            {
                SetKeyFilter("");
                e.Handled = true;
                return;
            }
            Send(Command.OpenSettings);
            e.Handled = true;
            return;
        }
        if (e.Key == VirtualKey.Down && ReferenceEquals(e.OriginalSource, _keyFilterInput))
        {
            FocusFirstVisibleKey();
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
            RefreshUpdateSettingsRow();
            RefreshTelemetrySettingsRow();
            if (_background is not null)
            {
                _background.SelectedIndex =
                    (_settingFlags & SettingFlag.BackgroundMask) >> SettingFlag.BackgroundShift;
            }
            if (_sortKey is not null) _sortKey.SelectedIndex = Math.Clamp(_sortPacked & 7, 0, SortNames.Length - 1);
            if (_sortDescending is not null) _sortDescending.IsOn = (_sortPacked & 8) != 0;
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
                if (existing.Parent is Grid existingLine) existingLine.Tag = row.Name + " " + KeysForRow(row.Row);
                continue;
            }
            int captured = row.Row;
            var line = new Grid { Margin = new Thickness(0, 4, 0, 4), ColumnSpacing = 16 };
            line.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            line.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(180) });
            var name = new TextBlock
            {
                Text = row.Name,
                TextWrapping = TextWrapping.Wrap,
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
            AutomationProperties.SetName(bind, "Change shortcut for " + row.Name);
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
            line.Tag = row.Name + " " + KeysForRow(captured);
            _keyList.Children.Add(line);
        }
        FilterSettingsKeys();
    }

    private static void SetKeyFilter(string value)
    {
        _keyFilter = value ?? "";
        _keyFilterInput?.SetText(_keyFilter);
        FilterSettingsKeys();
    }

    private static void FocusFirstVisibleKey()
    {
        if (_keyList is null) return;
        foreach (UIElement child in _keyList.Children)
        {
            if (child is not Grid line || line.Visibility != Visibility.Visible) continue;
            if (line.Children.Count > 1 && line.Children[1] is Button bind)
            {
                bind.Focus(FocusState.Keyboard);
                return;
            }
        }
    }

    private static void FilterSettingsKeys()
    {
        if (_keyList is null) return;
        string q = _keyFilter.Trim();
        int shown = 0;
        foreach (UIElement child in _keyList.Children)
        {
            if (child is not Grid line) continue;
            string hay = line.Tag as string ?? "";
            bool match = q.Length == 0 || hay.Contains(q, StringComparison.OrdinalIgnoreCase);
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

    private static string KeysForRow(int index)
    {
        string keys = string.Join("  /  ",
            CommandRows.Where(row => row.Row == index && row.Keys.Length > 0)
                       .Select(row => row.Keys).Distinct());
        return keys.Length == 0 ? "Unbound" : keys;
    }

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
        SetCaptureHint(_settingsKeyboard ? CaptureInstructions : "Changes are saved automatically.");
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
