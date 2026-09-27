// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;

namespace MediaViewer.Chrome;

/// <summary>
/// plan/16 `?`, go-to and find: XAML flyouts on the command bar, built from
/// the same static table the native router dispatches from (native pushes it
/// once, via SetCommandTable). Nothing here composites onto the swapchain.
/// </summary>
public static partial class IslandHost
{
    internal const int PopupArgsSize = 8;
    internal const int TableArgsSize = 16;

    // Mirrors mv::shell::chrome_popup.
    internal static class PopupKind
    {
        public const int Close = 0;
        public const int Help = 1;
        public const int Palette = 2;
        public const int GoTo = 3;
        public const int Find = 4;
        public const int Settings = 5;
        public const int Export = 6;  // PR 10; ModeMask carries the last packed choice
        public const int ClipTools = 7;  // PR 14; ModeMask carries trim_state.h kClipFlag*
    }

    private sealed class CommandRow
    {
        public int Id;
        public int Modes;
        public string Name = "";
        public string Keys = "";
        public bool Runnable;  // false: needs a key-up (hold Z, hold Q), so `?` only
        public int Row = -1;   // live-table index; Settings remaps this row
    }

    private sealed class CommandEntry
    {
        public int Id;
        public string Name = "";
        public string Keys = "";
        public override string ToString() => $"{Name}    {Keys}";
    }

    private static readonly List<CommandRow> CommandRows = new();
    private static Flyout? _popup;
    // Go-to / find type onto a label. Report text focus so the router yields
    // keys (plan/16) without putting a TextBox in the Flyout.
    private static bool _popupTakesText;

    // Explorer-style typeahead in the strip and the gallery (plan/16,
    // plan/12 2026-09-13): what was typed within 300 ms of the last key.
    private static string _typed = "";
    private static DateTime _typedAt = DateTime.MinValue;

    public static int SetCommandTable(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < TableArgsSize) return unchecked((int)0x80070057);
            ChromeTableArgs args = Marshal.PtrToStructure<ChromeTableArgs>(arg);
            string text = args.Utf8 == 0 || args.Length <= 0
                ? ""
                : Marshal.PtrToStringUTF8(checked((IntPtr)args.Utf8), args.Length) ?? "";
            CommandRows.Clear();
            foreach (string line in text.Split('\n', StringSplitOptions.RemoveEmptyEntries))
            {
                string[] f = line.Split('\t');
                if (f.Length < 5) continue;
                if (!int.TryParse(f[0], out int id) || !int.TryParse(f[1], out int modes)) continue;
                int row = -1;
                if (f.Length >= 6) int.TryParse(f[5], out row);
                CommandRows.Add(new CommandRow
                {
                    Id = id, Modes = modes, Name = f[2], Keys = f[3], Runnable = f[4] == "1",
                    Row = row,
                });
            }
            RefreshSettingsKeys();
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    public static int ShowPopup(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < PopupArgsSize) return unchecked((int)0x80070057);
            ChromePopupArgs args = Marshal.PtrToStructure<ChromePopupArgs>(arg);
            ClosePopup();
            if (args.Kind == PopupKind.Close)
            {
                HideSettingsScreen();
                return 0;
            }
            if (args.Kind == PopupKind.Settings)
            {
                ShowSettingsScreen();
                return 0;
            }

            if (_source?.Content is not FrameworkElement anchor)
            {
                Send(Command.Popup, 0);
                return 1;
            }

            UIElement? focusTarget = null;
            UIElement? content = args.Kind switch
            {
                PopupKind.Help => BuildHelp(args.ModeMask),
                PopupKind.GoTo => BuildGoTo(out focusTarget),
                PopupKind.Find => BuildFind(out focusTarget),
                PopupKind.Export => BuildExport(args.ModeMask, out focusTarget),
                PopupKind.ClipTools => BuildClipTools(args.ModeMask, out focusTarget),
                _ => null,
            };
            if (content is null)
            {
                Send(Command.Popup, 0);
                return 1;
            }

            // The export dialog takes the arrows and Enter itself, like go-to's
            // digits, so it reports text focus and the router yields.
            _popupTakesText = args.Kind is PopupKind.GoTo or PopupKind.Find or PopupKind.Export
                or PopupKind.ClipTools;
            var flyout = new Flyout
            {
                ShouldConstrainToRootBounds = false,
                FlyoutPresenterStyle = FlyoutPresenterStyle(),
                Placement = FlyoutPlacementMode.BottomEdgeAlignedLeft,
                Content = content,
            };
            flyout.Opened += (_, _) =>
            {
                Send(Command.Popup, 1);
                if (_popupTakesText) Send(Command.FocusChanged, FocusKind.Text);
                focusTarget?.Focus(FocusState.Keyboard);
            };
            flyout.Closed += (_, _) =>
            {
                // Only the flyout that is still current reports closing; a flyout
                // replaced by the next one must not clear its state or steal
                // the replacement's text focus (go-to, find).
                if (_popup is null || ReferenceEquals(_popup, flyout))
                {
                    _popup = null;
                    _popupTakesText = false;
                    Send(Command.Popup, 0);
                    RestoreCanvasFocus();
                }
            };
            _popup = flyout;
            flyout.ShowAt(anchor);
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            Send(Command.Popup, 0);
            return unchecked((int)0x80004005);
        }
    }

    private static void ClosePopup()
    {
        Flyout? open = _popup;
        _popup = null;
        open?.Hide();
    }

    // Bindings grouped by command, in table order; `modeMask` 0 means all modes.
    private static List<CommandEntry> Entries(int modeMask)
    {
        var entries = new List<CommandEntry>();
        var byId = new Dictionary<int, CommandEntry>();
        foreach (CommandRow row in CommandRows)
        {
            if (modeMask != 0 && (row.Modes & modeMask) == 0) continue;
            // An unbound row (Open RAW / Open JPEG by default) is Settings-only:
            // `?` lists what a key does.
            if (row.Keys.Length == 0) continue;
            if (!byId.TryGetValue(row.Id, out CommandEntry? entry))
            {
                entry = new CommandEntry { Id = row.Id, Name = row.Name, Keys = row.Keys };
                byId[row.Id] = entry;
                entries.Add(entry);
            }
            else if (!entry.Keys.Split("  /  ").Contains(row.Keys))
            {
                entry.Keys += "  /  " + row.Keys;
            }
        }
        return entries;
    }

    private static TextBlock Label(string text, double size, bool mute = false) => new()
    {
        Text = text,
        FontFamily = UiFont,
        FontSize = size,
        Foreground = Brush(mute ? Body : Title),
        TextWrapping = TextWrapping.NoWrap,
    };

    // `?`: a mode-sensitive cheat sheet, generated from the table so it cannot
    // drift from what the keys do. No TextBox: a text control in a Flyout
    // hanging off a DesktopWindowXamlSource is a Microsoft.UI.Xaml fail-fast
    // (0xC000027B).
    private static UIElement BuildHelp(int modeMask)
    {
        var list = new StackPanel { Spacing = 2, Margin = new Thickness(12, 8, 12, 10) };
        list.Children.Add(Label("Keyboard shortcuts", UiFontSize + 2));
        list.Children.Add(Label("Esc closes.", UiFontSize, mute: true));
        foreach (CommandEntry entry in Entries(modeMask))
        {
            var row = new Grid { ColumnSpacing = 16 };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(220) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            TextBlock keys = Label(entry.Keys, UiFontSize);
            TextBlock name = Label(entry.Name, UiFontSize, mute: true);
            Grid.SetColumn(keys, 0);
            Grid.SetColumn(name, 1);
            row.Children.Add(keys);
            row.Children.Add(name);
            list.Children.Add(row);
        }
        return new ScrollViewer
        {
            Content = list,
            MaxHeight = 560,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            IsTabStop = true,  // so the flyout holds focus and does not light-dismiss
        };
    }

    // Type-in field with no WinUI TextBox: that control in a Flyout or this
    // island is a Microsoft.UI.Xaml fail-fast (0xC000027B). Settings filter
    // uses the same stand-in.
    private sealed class FakeInput : ContentControl
    {
        private readonly TextBlock _label;
        private readonly Rectangle _caret;
        private readonly Border _inner;
        private readonly string _placeholder;
        private Microsoft.UI.Dispatching.DispatcherQueueTimer? _blink;
        private bool _selectAll;
        private bool _tookChar;

        public string Text { get; private set; } = "";
        public event Action? Changed;
        public event Action? Submitted;
        public event Action? MoveDown;

        public FakeInput(string placeholder, double width = 0)
        {
            _placeholder = placeholder;
            ProtectedCursor = InputSystemCursor.Create(InputSystemCursorShape.IBeam);
            if (width > 0) Width = width;
            else HorizontalAlignment = HorizontalAlignment.Stretch;
            IsTabStop = true;
            AllowFocusOnInteraction = true;
            UseSystemFocusVisuals = true;
            _label = new TextBlock
            {
                FontFamily = UiFont,
                FontSize = UiFontSize,
                Foreground = Brush(Body),
                VerticalAlignment = VerticalAlignment.Center,
                IsHitTestVisible = false,
            };
            _caret = new Rectangle
            {
                Width = 1,
                Height = UiFontSize + 4,
                Fill = Brush(Title),
                Margin = new Thickness(1, 0, 0, 0),
                VerticalAlignment = VerticalAlignment.Center,
                Visibility = Visibility.Collapsed,
                IsHitTestVisible = false,
            };
            var row = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                VerticalAlignment = VerticalAlignment.Center,
            };
            row.Children.Add(_label);
            row.Children.Add(_caret);
            _inner = new Border
            {
                Child = row,
                Background = Brush(Canvas),
                BorderBrush = Brush(Hairline),
                BorderThickness = new Thickness(1),
                Padding = new Thickness(10, 6, 10, 6),
                MinHeight = 32,
                HorizontalAlignment = HorizontalAlignment.Stretch,
            };
            Content = _inner;
            HorizontalContentAlignment = HorizontalAlignment.Stretch;
            Paint();
            GotFocus += (_, _) => { StartCaret(); Paint(); };
            LostFocus += (_, _) =>
            {
                StopCaret();
                _selectAll = false;
                Paint();
            };
            PointerPressed += (_, e) =>
            {
                Focus(FocusState.Pointer);
                _selectAll = Text.Length > 0;
                Paint();
                e.Handled = true;
            };
            CharacterReceived += (_, e) =>
            {
                char c = e.Character;
                if (char.IsControl(c)) return;
                _tookChar = true;
                Append(c);
                e.Handled = true;
            };
            KeyDown += (_, e) =>
            {
                _tookChar = false;
                if (e.Key == Windows.System.VirtualKey.Enter)
                {
                    Submitted?.Invoke();
                    e.Handled = true;
                }
                else if (e.Key == Windows.System.VirtualKey.Down)
                {
                    MoveDown?.Invoke();
                    e.Handled = true;
                }
                else if (e.Key == Windows.System.VirtualKey.Back && Text.Length > 0)
                {
                    Text = _selectAll ? "" : Text[..^1];
                    _selectAll = false;
                    Changed?.Invoke();
                    Paint();
                    e.Handled = true;
                }
                else if (e.Key == Windows.System.VirtualKey.A && Down(Windows.System.VirtualKey.Control)
                         && Text.Length > 0)
                {
                    _selectAll = true;
                    Paint();
                    e.Handled = true;
                }
            };
            KeyUp += (_, e) =>
            {
                // This island sometimes never raises CharacterReceived on a
                // ContentControl; letters still have to reach the filter.
                if (_tookChar) return;
                if (!TryCharFromKey(e, out char c)) return;
                Append(c);
                e.Handled = true;
            };
        }

        private void Append(char c)
        {
            Text = _selectAll ? c.ToString() : Text + c;
            _selectAll = false;
            Changed?.Invoke();
            Paint();
        }

        private static bool TryCharFromKey(KeyRoutedEventArgs e, out char c)
        {
            c = '\0';
            if (Down(Windows.System.VirtualKey.Control) || Down(Windows.System.VirtualKey.Menu))
                return false;
            int v = (int)e.OriginalKey;
            if (v >= (int)Windows.System.VirtualKey.A && v <= (int)Windows.System.VirtualKey.Z)
            {
                c = (char)v;
                if (!Down(Windows.System.VirtualKey.Shift)) c = char.ToLowerInvariant(c);
                return true;
            }
            if (v >= (int)Windows.System.VirtualKey.Number0 &&
                v <= (int)Windows.System.VirtualKey.Number9)
            {
                c = (char)v;
                return true;
            }
            return false;
        }

        public void SetText(string value)
        {
            string next = value ?? "";
            if (next == Text) return;
            Text = next;
            _selectAll = false;
            Paint();
        }

        private void Paint()
        {
            bool empty = Text.Length == 0;
            _label.Text = empty ? (FocusState != FocusState.Unfocused ? "" : _placeholder) : Text;
            _label.Foreground = Brush(empty ? Body : Title);
            _caret.Visibility = FocusState == FocusState.Unfocused
                ? Visibility.Collapsed : Visibility.Visible;
            _caret.Opacity = 1;
            _inner.BorderBrush = Brush(FocusState == FocusState.Unfocused ? Hairline : Title);
            _inner.BorderThickness = new Thickness(FocusState == FocusState.Unfocused ? 1 : 2);
        }

        private void StartCaret()
        {
            if (_dispatcher is null) return;
            _blink ??= _dispatcher.DispatcherQueue.CreateTimer();
            _blink.Interval = TimeSpan.FromMilliseconds(530);
            _blink.IsRepeating = true;
            _blink.Tick -= OnBlink;
            _blink.Tick += OnBlink;
            _caret.Visibility = Visibility.Visible;
            _caret.Opacity = 1;
            _blink.Start();
        }

        private void StopCaret()
        {
            _blink?.Stop();
            _caret.Opacity = 1;
        }

        private void OnBlink(Microsoft.UI.Dispatching.DispatcherQueueTimer sender, object args)
        {
            if (FocusState == FocusState.Unfocused) return;
            _caret.Opacity = _caret.Opacity > 0.5 ? 0 : 1;
        }
    }

    private static TextBlock QueryLabel(string prompt) => new()
    {
        Text = prompt,
        FontFamily = UiFont,
        FontSize = UiFontSize,
        Foreground = Brush(Body),
        TextWrapping = TextWrapping.NoWrap,
    };

    private static void SetQuery(TextBlock label, string prompt, string text)
    {
        bool empty = text.Length == 0;
        label.Text = empty ? prompt : text;
        label.Foreground = Brush(empty ? Body : Title);
    }

    // Ctrl+G: go to an item by its position in the folder. No TextBox: same
    // fail-fast as Settings (0xC000027B). Type onto this label; Enter jumps.
    private static UIElement BuildGoTo(out UIElement focusTarget)
    {
        string query = "";
        string prompt = Items.Count > 0 ? $"Go to 1 – {Items.Count}" : "Nothing open";
        TextBlock label = QueryLabel(prompt);
        var panel = new StackPanel { Spacing = 4, Margin = new Thickness(10), IsTabStop = true };
        panel.Children.Add(label);
        panel.CharacterReceived += (_, e) =>
        {
            char c = e.Character;
            if (char.IsControl(c)) return;
            query += c;
            SetQuery(label, prompt, query);
            e.Handled = true;
        };
        panel.KeyDown += (_, e) =>
        {
            if (e.Key == Windows.System.VirtualKey.Enter)
            {
                if (int.TryParse(query.Trim(), out int n) && n >= 1 && n <= Items.Count)
                {
                    ClosePopup();
                    Send(Command.SelectItem, n - 1);
                }
                e.Handled = true;
            }
            else if (e.Key == Windows.System.VirtualKey.Back && query.Length > 0)
            {
                query = query[..^1];
                SetQuery(label, prompt, query);
                e.Handled = true;
            }
        };
        focusTarget = panel;
        return panel;
    }

    // `/` from the canvas: find by name in the already-loaded listing.
    private static UIElement BuildFind(out UIElement focusTarget)
    {
        string query = "";
        const string prompt = "Find by name";
        TextBlock label = QueryLabel(prompt);
        TextBlock match = Label("", UiFontSize, mute: true);
        int found = -1;

        void Apply()
        {
            SetQuery(label, prompt, query);
            found = FindByName(query.Trim(), prefixOnly: false);
            match.Text = found >= 0 ? $"{Items[found].Name}   ({found + 1} / {Items.Count})"
                       : query.Length > 0 ? "No match" : "";
        }

        var panel = new StackPanel { Spacing = 6, Margin = new Thickness(10), IsTabStop = true };
        panel.Children.Add(label);
        panel.Children.Add(match);
        panel.CharacterReceived += (_, e) =>
        {
            char c = e.Character;
            if (char.IsControl(c)) return;
            query += c;
            Apply();
            e.Handled = true;
        };
        panel.KeyDown += (_, e) =>
        {
            if (e.Key == Windows.System.VirtualKey.Enter)
            {
                if (found >= 0)
                {
                    ClosePopup();
                    Send(Command.SelectItem, found);
                }
                e.Handled = true;
            }
            else if (e.Key == Windows.System.VirtualKey.Back && query.Length > 0)
            {
                query = query[..^1];
                Apply();
                e.Handled = true;
            }
        };
        focusTarget = panel;
        return panel;
    }

    // PR 10 export dialog (plan/10: "export dialog in WinUI"). Keyboard-complete
    // and TextBox-free (the 0xC000027B fail-fast): ↑ ↓ pick a row, ← → change
    // it, Enter exports, Esc cancels. Clicking a value steps it. The answer goes
    // back as one integer, the same packing the Mac sheet uses
    // (shell/edit_session.h pack_export); native runs the export on its pool.
    private static readonly string[] ExportFormats = { "JPEG", "PNG" };
    private static readonly int[] ExportQualities = { 100, 95, 92, 85, 75, 60 };
    private static readonly string[] ExportSizes = { "Full size", "3840 px", "2560 px", "2048 px", "1600 px", "1080 px" };
    private static readonly string[] ExportPolicies = { "All metadata", "All but location (GPS)", "None" };

    private static UIElement BuildExport(int lastChoice, out UIElement focusTarget)
    {
        int quality = lastChoice & 0x7F;
        int format = (lastChoice >> 7) & 1;
        int policy = Math.Clamp((lastChoice >> 8) & 3, 0, 2);
        int size = Math.Clamp((lastChoice >> 10) & 7, 0, ExportSizes.Length - 1);
        int qualityIndex = Array.IndexOf(ExportQualities, quality);
        if (qualityIndex < 0) qualityIndex = 2;  // 92
        int row = 0;
        const int Rows = 4;

        var panel = new StackPanel { Spacing = 6, Margin = new Thickness(12), IsTabStop = true, MinWidth = 320 };
        panel.Children.Add(Label("Export a copy", UiFontSize + 2));
        panel.Children.Add(Label("Beside the original as name-edit; never overwrites.", UiFontSize, mute: true));
        var rowLabels = new TextBlock[Rows];
        string[] names = { "Format", "Quality", "Size", "Metadata" };
        var grid = new Grid { ColumnSpacing = 16, RowSpacing = 4, Margin = new Thickness(0, 6, 0, 6) };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        string ValueOf(int r) => r switch
        {
            0 => ExportFormats[format],
            1 => format == 1 ? "(PNG is lossless)" : $"{ExportQualities[qualityIndex]}",
            2 => ExportSizes[size],
            _ => ExportPolicies[policy],
        };
        void Refresh()
        {
            for (int r = 0; r < Rows; ++r)
            {
                rowLabels[r].Text = (r == row ? "\u25C0  " : "    ") + ValueOf(r) + (r == row ? "  \u25B6" : "");
                rowLabels[r].Foreground = Brush(r == row ? Title : Body);
            }
        }
        void Step(int r, int delta)
        {
            switch (r)
            {
                case 0: format = (format + delta + 2) % 2; break;
                case 1: if (format == 0) qualityIndex = Math.Clamp(qualityIndex - delta, 0, ExportQualities.Length - 1); break;
                case 2: size = (size + delta + ExportSizes.Length) % ExportSizes.Length; break;
                default: policy = (policy + delta + ExportPolicies.Length) % ExportPolicies.Length; break;
            }
            Refresh();
        }
        void Confirm()
        {
            int packed = ExportQualities[qualityIndex] | (format << 7) | (policy << 8) | (size << 10);
            ClosePopup();
            Send(Command.Export, packed);
        }

        for (int r = 0; r < Rows; ++r)
        {
            grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            TextBlock name = Label(names[r], UiFontSize, mute: true);
            Grid.SetRow(name, r);
            grid.Children.Add(name);
            rowLabels[r] = Label("", UiFontSize);
            int captured = r;
            var value = FlatButton(rowLabels[r], new Thickness(0));
            value.Click += (_, _) => { row = captured; Step(captured, 1); };
            Grid.SetRow(value, r);
            Grid.SetColumn(value, 1);
            grid.Children.Add(value);
        }
        panel.Children.Add(grid);
        panel.Children.Add(Label("\u2191 \u2193 choose   \u2190 \u2192 change   Enter export   Esc cancel", UiFontSize, mute: true));
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, HorizontalAlignment = HorizontalAlignment.Right };
        var export = new Button { Content = "Export", IsTabStop = false };
        export.Click += (_, _) => Confirm();
        var cancel = new Button { Content = "Cancel", IsTabStop = false };
        cancel.Click += (_, _) => ClosePopup();
        buttons.Children.Add(cancel);
        buttons.Children.Add(export);
        panel.Children.Add(buttons);
        Refresh();

        panel.KeyDown += (_, e) =>
        {
            switch (e.Key)
            {
                case Windows.System.VirtualKey.Up: row = (row + Rows - 1) % Rows; Refresh(); e.Handled = true; break;
                case Windows.System.VirtualKey.Down: row = (row + 1) % Rows; Refresh(); e.Handled = true; break;
                case Windows.System.VirtualKey.Left: Step(row, -1); e.Handled = true; break;
                case Windows.System.VirtualKey.Right: Step(row, 1); e.Handled = true; break;
                case Windows.System.VirtualKey.Enter: Confirm(); e.Handled = true; break;
            }
        };
        focusTarget = panel;
        return panel;
    }

    // First item whose name starts with `text`; unless prefixOnly, then the
    // first that contains it. Case-insensitive. -1 if none.
    private static int FindByName(string text, bool prefixOnly)
    {
        if (text.Length == 0) return -1;
        for (int i = 0; i < Items.Count; ++i)
        {
            if (Items[i].Name.StartsWith(text, StringComparison.OrdinalIgnoreCase)) return i;
        }
        if (prefixOnly) return -1;
        for (int i = 0; i < Items.Count; ++i)
        {
            if (Items[i].Name.Contains(text, StringComparison.OrdinalIgnoreCase)) return i;
        }
        return -1;
    }

    // Typing with the strip or the gallery focused jumps by name.
    private static void OnTypeahead(UIElement sender, CharacterReceivedRoutedEventArgs e)
    {
        char c = e.Character;
        if (char.IsControl(c)) return;
        DateTime now = DateTime.UtcNow;
        if ((now - _typedAt).TotalMilliseconds > 300) _typed = "";
        _typedAt = now;
        _typed += c;
        int index = FindByName(_typed, prefixOnly: true);
        if (index >= 0) Send(Command.SelectItem, index);
        e.Handled = true;
    }
}

[StructLayout(LayoutKind.Sequential)]
internal struct ChromePopupArgs
{
    public int Kind;
    public int ModeMask;
}

[StructLayout(LayoutKind.Sequential)]
internal struct ChromeTableArgs
{
    public ulong Utf8;
    public int Length;
    public int Reserved;
}
