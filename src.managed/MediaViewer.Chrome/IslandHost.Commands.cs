// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Windows.Foundation;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;

namespace MediaViewer.Chrome;

/// <summary>
/// docs/design/16 `?`, go-to and find: XAML flyouts on the command bar, built from
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
        public const int Rename = 8;     // issue #210; ModeMask carries the folder index
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
    // keys (docs/design/16) without putting a TextBox in the Flyout.
    private static bool _popupTakesText;

    // Explorer-style typeahead in the strip and the gallery (docs/design/16,
    // docs/design/12 2026-09-13): what was typed within 300 ms of the last key.
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
            // docs/design/25 PR 57: a script add-on that starts or stops changes the
            // table; Settings' From others re-reads the scripts' state (no file read).
            RefreshOpenScripts();
            RefreshKeyTips();
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    // The key bound to `commandId` now: its first row in the live table, as the
    // Mac reads it (SettingsStore.keyLabel). Null while it has none. Menus and
    // tooltips name keys through this, never a literal, so a remap shows.
    private static string? KeyOf(int commandId)
    {
        foreach (CommandRow row in CommandRows)
        {
            if (row.Id == commandId && row.Keys.Length > 0) return row.Keys;
        }
        return null;
    }

    // A tooltip's "text  key", or the text alone while the command is unbound.
    private static string WithKey(string text, int commandId) =>
        KeyOf(commandId) is string key ? $"{text}  {key}" : text;

    // " (key)" to follow a sentence, or nothing while the command is unbound.
    private static string KeyNote(int commandId) =>
        KeyOf(commandId) is string key ? $" ({key})" : "";

    // Native pushes the table after the bar is built and again on every remap:
    // tooltips that name a key read it again (the menus rebuild on Opening).
    private static void RefreshKeyTips()
    {
        RenderEditBarButton();  // and the Open in button beside it
        UpdatePathSearchButton();
        RefreshIndexingTip();
        if (_speed is not null) ToolTipService.SetToolTip(_speed, SpeedTip());
        if (_helpButton is not null)
        {
            ToolTipService.SetToolTip(_helpButton, WithKey("Keyboard shortcuts", Command.Help));
        }
        // The Edit pane names its keys as it is built: rebuild a visible one.
        if (_editPaneVisible)
        {
            _editShape = "";
            RenderEdit();
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
                PopupKind.Rename => BuildRename(args.ModeMask, out focusTarget),
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
                or PopupKind.ClipTools or PopupKind.Rename;
            // Export and clip tools are sheets (issue #199): a centred card,
            // radius 12, padding 20 (ExportView.swift, ClipToolsView.swift),
            // not a menu hanging off the bar's corner.
            bool sheet = args.Kind is PopupKind.Export or PopupKind.ClipTools or PopupKind.Rename;
            var flyout = new Flyout
            {
                ShouldConstrainToRootBounds = false,
                FlyoutPresenterStyle = sheet ? SheetPresenterStyle()
                    : args.Kind == PopupKind.Help ? HelpPresenterStyle() : FlyoutPresenterStyle(),
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
            if (sheet && SheetOrigin(anchor, content) is Point origin)
                flyout.ShowAt(anchor, new FlyoutShowOptions { Position = origin, Placement = FlyoutPlacementMode.Bottom });
            else
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

    private static Style SheetPresenterStyle()
    {
        Style style = FlyoutPresenterStyle();
        style.Setters.Add(new Setter(FlyoutPresenter.CornerRadiusProperty, Corner(RadiusSheet)));
        style.Setters.Add(new Setter(FlyoutPresenter.PaddingProperty, Pad(20)));
        // The stock presenter caps its size; a sheet sizes to its content.
        style.Setters.Add(new Setter(FrameworkElement.MaxWidthProperty, 640.0));
        style.Setters.Add(new Setter(FrameworkElement.MaxHeightProperty, 900.0));
        return style;
    }

    // Where a sheet's top-centre goes, relative to the bar (which sits at the
    // client origin): centred in the main window's client area. Null when the
    // window cannot be measured; the caller then shows it the old way.
    private static Point? SheetOrigin(FrameworkElement anchor, UIElement content)
    {
        if (_themeWindow == IntPtr.Zero || anchor.XamlRoot is null) return null;
        if (!GetClientRect(_themeWindow, out ClientRect rect)) return null;
        double scale = anchor.XamlRoot.RasterizationScale > 0 ? anchor.XamlRoot.RasterizationScale : 1;
        double width = (rect.Right - rect.Left) / scale;
        double height = (rect.Bottom - rect.Top) / scale;
        if (width <= 0 || height <= 0) return null;
        content.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        double sheet = content.DesiredSize.Height + 2 * 20 + 2;  // padding and hairline
        return new Point(width / 2, Math.Max(16, (height - sheet) / 2));
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ClientRect
    {
        public int Left, Top, Right, Bottom;
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetClientRect(IntPtr hWnd, out ClientRect rect);

    // A sheet's Cancel / default-button row, right-aligned (the Mac's HStack
    // with .cancelAction and .defaultAction).
    private static StackPanel SheetButtons(Button cancel, Button primary)
    {
        var buttons = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = Space(8),
            HorizontalAlignment = HorizontalAlignment.Right,
            Margin = Pad(0, 10, 0, 0),
        };
        buttons.Children.Add(cancel);
        buttons.Children.Add(primary);
        return buttons;
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
    //
    // Issue #201: the Mac's HelpView. Its adaptive grid (columns of at least
    // HelpColumn, as many as the window holds, filled row by row), body type,
    // and the footer hint. Sized from the window once, as it opens.
    private const double HelpColumn = 340;
    private const double HelpColumnGap = 24;
    private const double HelpKeysWidth = 160;

    private static UIElement BuildHelp(int modeMask)
    {
        double windowWidth = 0, windowHeight = 0;
        if (_themeWindow != IntPtr.Zero && GetClientRect(_themeWindow, out ClientRect rect)
            && _source?.Content is FrameworkElement { XamlRoot: { } root })
        {
            double scale = root.RasterizationScale > 0 ? root.RasterizationScale : 1;
            windowWidth = (rect.Right - rect.Left) / scale;
            windowHeight = (rect.Bottom - rect.Top) / scale;
        }
        double gap = Space(HelpColumnGap);
        // The room inside the flyout: the window less its margins and padding.
        double room = windowWidth > 0 ? windowWidth - 2 * 16 - 2 * Space(32) : HelpColumn;
        int columns = Math.Clamp((int)((room + gap) / (HelpColumn + gap)), 1, 4);
        List<CommandEntry> entries = Entries(modeMask);
        columns = Math.Max(1, Math.Min(columns, entries.Count));

        var grid = new Grid { ColumnSpacing = gap, RowSpacing = Space(6), Margin = Pad(32, 0, 32, 0) };
        for (int c = 0; c < columns; ++c)
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(HelpColumn) });
        int rows = (entries.Count + columns - 1) / columns;
        for (int r = 0; r < rows; ++r)
            grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        double type = TypeSize(TypeBody);
        for (int i = 0; i < entries.Count; ++i)
        {
            var row = new Grid { ColumnSpacing = Space(8) };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(HelpKeysWidth) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            TextBlock keys = Label(entries[i].Keys, type);
            keys.TextWrapping = TextWrapping.Wrap;
            TextBlock name = Label(entries[i].Name, type, mute: true);
            name.TextWrapping = TextWrapping.Wrap;
            Grid.SetColumn(name, 1);
            row.Children.Add(keys);
            row.Children.Add(name);
            Grid.SetRow(row, i / columns);
            Grid.SetColumn(row, i % columns);
            grid.Children.Add(row);
        }

        var scroll = new ScrollViewer
        {
            Content = grid,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            IsTabStop = true,  // so the flyout holds focus and does not light-dismiss
        };
        TextBlock title = Label("Keyboard shortcuts", TypeSize(TypeHeading));
        title.HorizontalAlignment = HorizontalAlignment.Center;
        string help = KeyOf(Command.Help) ?? "?";
        TextBlock footer = Label(
            $"Wheel zooms toward the cursor, drag pans. Keys change in Settings{KeyNote(Command.OpenSettings)}. "
            + $"{help} or Esc to close.", TypeSize(TypeCaption), mute: true);
        footer.HorizontalAlignment = HorizontalAlignment.Center;
        footer.TextWrapping = TextWrapping.Wrap;
        footer.TextAlignment = TextAlignment.Center;

        var page = new Grid { RowSpacing = Space(16), Padding = Pad(0, 16, 0, 12) };
        page.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        page.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        page.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        Grid.SetRow(scroll, 1);
        Grid.SetRow(footer, 2);
        page.Children.Add(title);
        page.Children.Add(scroll);
        page.Children.Add(footer);
        // Tall enough to show most of the table, short enough to stay on screen.
        page.MaxHeight = windowHeight > 0 ? Math.Max(240, windowHeight - 96) : 560;
        return page;
    }

    // The stock presenter caps its width (~456), which a grid of columns
    // overflows; the help sizes to its content.
    private static Style HelpPresenterStyle()
    {
        Style style = FlyoutPresenterStyle();
        style.Setters.Add(new Setter(FrameworkElement.MaxWidthProperty, double.PositiveInfinity));
        style.Setters.Add(new Setter(FrameworkElement.MaxHeightProperty, double.PositiveInfinity));
        return style;
    }

    // The chrome's type-in field: Shared\FakeInput.cs in the chrome's look (a
    // WinUI TextBox in this island is a fail-fast; see that file). It keeps
    // Enter and Down even with no listener, as it always has.
    private static readonly MediaViewer.Shared.FakeInputLook ChromeInputLook = new()
    {
        Font = () => UiFont,
        FontSize = () => UiFontSize,
        Title = () => Brush(Title),
        Body = () => Brush(Body),
        // The Mac's field: a surface fill, radius 6, a 1 px accent border when focused.
        Canvas = () => Brush(ChromeColour.Surface),
        Hairline = () => Brush(Hairline),
        Selection = () => Brush(TextSelection),
        SelectionInk = () => Brush(TextSelectionInk),
        Accent = () => Brush(ChromeColour.Accent),
    };

    private sealed class FakeInput : MediaViewer.Shared.FakeInput
    {
        // `bare`: no box of its own, for a field whose container draws one
        // (file search).
        public FakeInput(string placeholder, double width = 0, bool bare = false,
                         MediaViewer.Shared.FakeInputLook? look = null)
            : base(look ?? ChromeInputLook, placeholder, width, bare)
        {
            OwnsEnterAndDown = true;
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
        var panel = new StackPanel { Spacing = Space(4), Margin = Pad(10), IsTabStop = true };
        panel.Children.Add(label);
        // Typing lands on a label, not a field: name the panel and speak the
        // number as it is typed (issue #223).
        AutomationProperties_SetName(panel, "Go to item");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetHelpText(panel, prompt + ". Type a number, Enter to go");
        MarkLive(label);
        panel.CharacterReceived += (_, e) =>
        {
            char c = e.Character;
            if (char.IsControl(c)) return;
            query += c;
            SetQuery(label, prompt, query);
            RaiseLive(label);
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
                RaiseLive(label);
                e.Handled = true;
            }
        };
        focusTarget = panel;
        return panel;
    }

    // F2 (issue #210): rename the stop at `index`. The field holds the name without
    // its extension, selected, as Explorer does; Enter parks it for native, which
    // checks it and renames the file, its pair and its sidecars on the file
    // worker. Esc closes and renames nothing.
    private static UIElement? BuildRename(int index, out UIElement focusTarget)
    {
        focusTarget = null!;
        if (index < 0 || index >= Items.Count) return null;
        FolderItemVm item = Items[index];
        string name = item.Name;
        int dot = name.LastIndexOf('.');
        string stem = dot > 0 ? name[..dot] : name;
        string ext = dot > 0 ? name[dot..] : "";

        var panel = new StackPanel { Spacing = Space(6), Margin = Pad(12), MinWidth = 340 };
        panel.Children.Add(Label("Rename", UiFontSize + 2));
        string carried = item.PairPath.Length > 0
            ? $"Keeps {ext} and its pair; any .xmp sidecar follows. Never overwrites."
            : $"Keeps {ext}; any .xmp sidecar follows. Never overwrites.";
        panel.Children.Add(Label(carried, UiFontSize, mute: true));
        var field = new FakeInput("New name", 316);
        field.SetText(stem);
        field.SelectAll();
        field.Submitted += () =>
        {
            string typed = field.Text.Trim();
            ClosePopup();
            if (typed.Length == 0 || typed == stem) return;
            _treePending = typed;
            Send(Command.Rename);
        };
        AutomationProperties_SetName(field, "New name for " + name);
        panel.Children.Add(field);
        focusTarget = field;
        return panel;
    }

    // `/` from the canvas: find by name in the already-loaded listing, then
    // (#291) by tag, from the core's folder tag pass (memory, never a read).
    private static UIElement BuildFind(out UIElement focusTarget)
    {
        string query = "";
        const string prompt = "Find by name or tag";
        TextBlock label = QueryLabel(prompt);
        TextBlock match = Label("", UiFontSize, mute: true);
        int found = -1;

        void Apply()
        {
            SetQuery(label, prompt, query);
            found = FindByName(query.Trim(), prefixOnly: false);
            string why = "";
            string reading = "";
            if (found < 0 && query.Trim().Length > 0) (found, why, reading) = FindByTag(query.Trim());
            match.Text = found >= 0 ? $"{Items[found].Name}   ({found + 1} / {Items.Count})" + (why.Length > 0 ? $"   · {why}" : "")
                       : reading.Length > 0 ? reading
                       : query.Length > 0 ? "No match" : "";
            AutomationProperties_SetName(match, found >= 0 ? $"{Items[found].Name}, {found + 1} of {Items.Count}"
                                                : match.Text);
            RaiseLive(match);
        }

        var panel = new StackPanel { Spacing = Space(6), Margin = Pad(10), IsTabStop = true };
        panel.Children.Add(label);
        panel.Children.Add(match);
        // The result line is what Narrator reads as the name is typed (issue #223).
        AutomationProperties_SetName(panel, prompt);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetHelpText(panel, "Type part of a name, Enter to go");
        MarkLive(match);
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

    // #291: the first stop whose tags hold every word, its why, and the
    // "still reading" note while the tag pass runs.
    private static (int Index, string Why, string Reading) FindByTag(string query)
    {
        string json = _folderSession?.FolderTagSearch(query) ?? "";
        if (json.Length == 0) return (-1, "", "");
        try
        {
            using var doc = System.Text.Json.JsonDocument.Parse(json);
            var root = doc.RootElement;
            int total = root.GetProperty("total").GetInt32();
            string reading = root.GetProperty("reading").GetBoolean() && total > 0
                ? $"Still reading tags — {root.GetProperty("done").GetInt32():N0} of {total:N0}"
                : "";
            foreach (var hit in root.GetProperty("hits").EnumerateArray())
            {
                int index = hit[0].GetInt32();
                if ((uint)index < (uint)Items.Count) return (index, hit[1].GetString() ?? "", reading);
            }
            return (-1, "", reading);
        }
        catch (Exception ex) when (ex is System.Text.Json.JsonException or InvalidOperationException
                                       or KeyNotFoundException or IndexOutOfRangeException)
        {
            return (-1, "", "");
        }
    }

    // PR 10 export dialog (docs/design/10: "export dialog in WinUI"). Keyboard-complete
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

        var panel = new StackPanel { Spacing = Space(6), IsTabStop = true, Width = 340 };
        panel.Children.Add(Label("Export a copy", UiFontSize + 2));
        panel.Children.Add(Label("Beside the original as name-edit; never overwrites.", UiFontSize, mute: true));
        var rowLabels = new TextBlock[Rows];
        string[] names = { "Format", "Quality", "Size", "Metadata" };
        var grid = new Grid { ColumnSpacing = Space(16), RowSpacing = Space(4), Margin = Pad(0, 6, 0, 6) };
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
                AutomationProperties_SetName(rowLabels[r], $"{names[r]}: {ValueOf(r)}");
            }
        }
        // Rows are labels on a focused panel: speak the current one as ↑ ↓ ← →
        // move it (issue #223).
        void Speak()
        {
            Refresh();
            RaiseLive(rowLabels[row]);
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
            Speak();
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
            MarkLive(rowLabels[r]);
            int captured = r;
            var value = FlatButton(rowLabels[r], Pad(0));
            value.Click += (_, _) => { row = captured; Step(captured, 1); };
            Grid.SetRow(value, r);
            Grid.SetColumn(value, 1);
            grid.Children.Add(value);
        }
        panel.Children.Add(grid);
        panel.Children.Add(Label("\u2191 \u2193 choose   \u2190 \u2192 change   Enter export   Esc cancel", UiFontSize, mute: true));
        // Cancel (Esc) and the accent default, Export (Enter).
        Button cancel = TextButton("Cancel", ClosePopup);
        cancel.IsTabStop = false;
        ToolTipService.SetToolTip(cancel, "Esc");
        Button export = AccentButton("Export", Confirm);
        export.IsTabStop = false;
        ToolTipService.SetToolTip(export, "Enter");
        panel.Children.Add(SheetButtons(cancel, export));
        AutomationProperties_SetName(panel, "Export a copy");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetHelpText(
            panel, "Up and Down choose a setting, Left and Right change it, Enter exports, Escape cancels");
        Refresh();

        panel.KeyDown += (_, e) =>
        {
            switch (e.Key)
            {
                case Windows.System.VirtualKey.Up: row = (row + Rows - 1) % Rows; Speak(); e.Handled = true; break;
                case Windows.System.VirtualKey.Down: row = (row + 1) % Rows; Speak(); e.Handled = true; break;
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
