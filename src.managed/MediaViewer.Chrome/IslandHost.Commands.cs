// SPDX-License-Identifier: GPL-2.0-or-later
using System.Runtime.InteropServices;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;

namespace MediaViewer.Chrome;

/// <summary>
/// plan/16 "Command palette and `?`": XAML flyouts on the command bar, built
/// from the same static table the native router dispatches from (native pushes
/// it once, via SetCommandTable). Running a palette entry sends its command id
/// back; native runs it through the same switch as the key. Nothing here
/// composites onto the swapchain.
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

            Control? focusTarget = null;
            UIElement? content = args.Kind switch
            {
                PopupKind.Help => BuildHelp(args.ModeMask),
                PopupKind.Palette => BuildPalette(out focusTarget),
                PopupKind.GoTo => BuildGoTo(out focusTarget),
                PopupKind.Find => BuildFind(out focusTarget),
                _ => null,
            };
            if (content is null)
            {
                Send(Command.Popup, 0);
                return 1;
            }

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
                focusTarget?.Focus(FocusState.Keyboard);
            };
            flyout.Closed += (_, _) =>
            {
                // Only the flyout that is still current reports closing; a flyout
                // replaced by the next one must not clear its state or steal
                // the replacement's text focus (palette filter, go-to, find).
                if (_popup is null || ReferenceEquals(_popup, flyout))
                {
                    _popup = null;
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
    // The palette asks for runnable ones only (review note 39).
    private static List<CommandEntry> Entries(int modeMask, bool runnableOnly = false)
    {
        var entries = new List<CommandEntry>();
        var byId = new Dictionary<int, CommandEntry>();
        foreach (CommandRow row in CommandRows)
        {
            if (modeMask != 0 && (row.Modes & modeMask) == 0) continue;
            if (runnableOnly && !row.Runnable) continue;
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
    // drift from what the keys do.
    private static UIElement BuildHelp(int modeMask)
    {
        var list = new StackPanel { Spacing = 2, Margin = new Thickness(12, 8, 12, 10) };
        list.Children.Add(Label("Keyboard shortcuts", UiFontSize + 2));
        list.Children.Add(Label("Esc closes. Type to filter, or Ctrl+K for the palette.", UiFontSize, mute: true));
        var search = new TextBox
        {
            PlaceholderText = "Search commands or keys",
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Title),
            Margin = new Thickness(0, 6, 0, 8),
        };
        list.Children.Add(search);
        var rows = new StackPanel { Spacing = 2 };
        var empty = Label("No matching shortcuts", UiFontSize, mute: true);
        empty.Visibility = Visibility.Collapsed;
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
            row.Tag = entry.Keys + " " + entry.Name;
            rows.Children.Add(row);
        }
        list.Children.Add(rows);
        list.Children.Add(empty);
        search.KeyDown += (_, e) =>
        {
            if (e.Key != Windows.System.VirtualKey.Escape || string.IsNullOrEmpty(search.Text)) return;
            search.Text = "";
            e.Handled = true;
        };
        search.TextChanged += (_, _) =>
        {
            string q = search.Text.Trim();
            int shown = 0;
            foreach (UIElement child in rows.Children)
            {
                if (child is not Grid row) continue;
                string hay = row.Tag as string ?? "";
                bool match = q.Length == 0 || hay.Contains(q, StringComparison.OrdinalIgnoreCase);
                row.Visibility = match ? Visibility.Visible : Visibility.Collapsed;
                if (match) shown++;
            }
            empty.Visibility = shown == 0 ? Visibility.Visible : Visibility.Collapsed;
        };
        return new ScrollViewer
        {
            Content = list,
            MaxHeight = 560,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            IsTabStop = true,  // so the flyout holds focus and does not light-dismiss
        };
    }

    private static TextBox Input(string placeholder, double width) => new()
    {
        PlaceholderText = placeholder,
        FontFamily = UiFont,
        FontSize = UiFontSize,
        Width = width,
    };

    // Ctrl+K: every command, filtered as you type; Enter runs the top one (or
    // the one chosen with the arrows). Same dispatch as the key.
    private static UIElement BuildPalette(out Control focusTarget)
    {
        List<CommandEntry> all = Entries(0, runnableOnly: true);
        TextBox filter = Input("Type a command", 420);
        var list = new ListView
        {
            MaxHeight = 420,
            Width = 420,
            SelectionMode = ListViewSelectionMode.Single,
            IsItemClickEnabled = true,
            ItemsSource = all,
        };

        void Run(CommandEntry? entry)
        {
            if (entry is null) return;
            ClosePopup();
            Send(entry.Id);
        }

        filter.TextChanged += (_, _) =>
        {
            string q = filter.Text.Trim();
            var shown = new List<CommandEntry>();
            foreach (CommandEntry e in all)
            {
                if (q.Length == 0 ||
                    e.Name.Contains(q, StringComparison.OrdinalIgnoreCase) ||
                    e.Keys.Contains(q, StringComparison.OrdinalIgnoreCase))
                {
                    shown.Add(e);
                }
            }
            list.ItemsSource = shown;
            if (shown.Count > 0) list.SelectedIndex = 0;
        };
        filter.KeyDown += (_, e) =>
        {
            if (e.Key == Windows.System.VirtualKey.Enter)
            {
                Run(list.SelectedItem as CommandEntry ??
                    (list.ItemsSource is List<CommandEntry> l && l.Count > 0 ? l[0] : null));
                e.Handled = true;
            }
            else if (e.Key == Windows.System.VirtualKey.Down && list.Items.Count > 0)
            {
                if (list.SelectedIndex < 0) list.SelectedIndex = 0;
                list.Focus(FocusState.Keyboard);
                e.Handled = true;
            }
        };
        list.ItemClick += (_, e) => Run(e.ClickedItem as CommandEntry);
        list.KeyDown += (_, e) =>
        {
            if (e.Key == Windows.System.VirtualKey.Enter)
            {
                Run(list.SelectedItem as CommandEntry);
                e.Handled = true;
            }
        };
        focusTarget = filter;
        var panel = new StackPanel { Spacing = 6, Margin = new Thickness(10) };
        panel.Children.Add(filter);
        panel.Children.Add(list);
        return panel;
    }

    // Ctrl+G: go to an item by its position in the folder.
    private static UIElement BuildGoTo(out Control focusTarget)
    {
        TextBox box = Input(Items.Count > 0 ? $"Go to 1 – {Items.Count}" : "Nothing open", 220);
        box.KeyDown += (_, e) =>
        {
            if (e.Key != Windows.System.VirtualKey.Enter) return;
            e.Handled = true;
            if (int.TryParse(box.Text.Trim(), out int n) && n >= 1 && n <= Items.Count)
            {
                ClosePopup();
                Send(Command.SelectItem, n - 1);
            }
        };
        focusTarget = box;
        var panel = new StackPanel { Spacing = 4, Margin = new Thickness(10) };
        panel.Children.Add(box);
        return panel;
    }

    // `/` from the canvas: find by name in the already-loaded listing.
    private static UIElement BuildFind(out Control focusTarget)
    {
        TextBox box = Input("Find by name", 320);
        TextBlock match = Label("", UiFontSize, mute: true);
        int found = -1;
        box.TextChanged += (_, _) =>
        {
            found = FindByName(box.Text.Trim(), prefixOnly: false);
            match.Text = found >= 0 ? $"{Items[found].Name}   ({found + 1} / {Items.Count})"
                       : box.Text.Length > 0 ? "No match" : "";
        };
        box.KeyDown += (_, e) =>
        {
            if (e.Key != Windows.System.VirtualKey.Enter) return;
            e.Handled = true;
            if (found < 0) return;
            ClosePopup();
            Send(Command.SelectItem, found);
        };
        focusTarget = box;
        var panel = new StackPanel { Spacing = 6, Margin = new Thickness(10) };
        panel.Children.Add(box);
        panel.Children.Add(match);
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
