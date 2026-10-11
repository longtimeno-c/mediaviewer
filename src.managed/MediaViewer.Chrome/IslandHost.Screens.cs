// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;

namespace MediaViewer.Chrome;

/// <summary>
/// Screens (docs/design/25 §10, PR 58): an add-on's view tree built from WinUI's
/// own controls. The Mac twin is ScreenView.swift.
/// </summary>
/// <remarks>
/// The tree arrives checked by the core (src/addon/screen.h): every control and
/// picture has a label, which becomes its Narrator name
/// (AutomationProperties.Name). Only controls that load in these islands are
/// used (tools/check-winui-controls.ps1): Button, ToggleSwitch, ComboBox,
/// Slider, TextBlock, Image, ScrollViewer, ItemsRepeater; a text field is
/// <see cref="FakeInput"/>, a number is − / + buttons, progress is a drawn bar,
/// lists and tables are rows of buttons. Arrow keys between rows are handled
/// here, since FocusManager's directional moves fail-fast in these islands.
/// A control the person changes reports to the core, which checks it against
/// the tree before the add-on's worker hears of it; a text field reports on
/// Enter and when it loses focus, never per key; a slider when it is let go.
/// </remarks>
public static partial class IslandHost
{
    private readonly record struct ScreenAt(string Addon, string Screen);

    private static readonly FontFamily SymbolFont = new("Segoe Fluent Icons,Segoe MDL2 Assets");

    /// <summary>The host's glyph for a contribution's symbol (contrib.h kSymbols).</summary>
    private static string SlotGlyph(string symbol) => symbol switch
    {
        "map" => "",
        "pin" => "",
        "star" => "",
        "tag" => "",
        "search" => "",
        "info" => "",
        "person" => "",
        "camera" => "",
        "folder" => "",
        "sparkle" => "",
        "check" => "",
        "flag" => "",
        "heart" => "",
        "clock" => "",
        "globe" => "",
        "photo" => "",
        "film" => "",
        "bolt" => "",
        "gear" => "",
        "plus" => "",
        "list" => "",
        "grid" => "",
        "share" => "",
        "warning" => "",
        _ => "",
    };

    private static string Str(JsonElement n, string key) =>
        n.TryGetProperty(key, out JsonElement v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";

    private static double Num(JsonElement n, string key, double fallback) =>
        n.TryGetProperty(key, out JsonElement v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : fallback;

    private static bool Flag(JsonElement n, string key) =>
        n.TryGetProperty(key, out JsonElement v) && v.ValueKind == JsonValueKind.True;

    private static IEnumerable<JsonElement> Arr(JsonElement n, string key) =>
        n.TryGetProperty(key, out JsonElement v) && v.ValueKind == JsonValueKind.Array
            ? v.EnumerateArray() : Enumerable.Empty<JsonElement>();

    private static HashSet<string> SelectedSet(JsonElement n) =>
        new(Arr(n, "selection").Where(e => e.ValueKind == JsonValueKind.String).Select(e => e.GetString() ?? ""),
            StringComparer.Ordinal);

    private static void SendScreenEvent(ScreenAt at, string node, uint kind, string valueJson) =>
        SlotNative.Event(at.Addon, at.Screen, node, kind, valueJson);

    private static string JsonString(string s) => JsonSerializer.Serialize(s);

    private static string JsonList(IEnumerable<string> list) => JsonSerializer.Serialize(list.ToArray());

    // ---- the renderer ---------------------------------------------------------------

    private static UIElement BuildScreenNode(JsonElement n, ScreenAt at)
    {
        UIElement e = BuildScreenNodeInner(n, at);
        string label = Str(n, "label");
        if (label.Length > 0) AutomationProperties.SetName(e, label);
        string help = Str(n, "help");
        if (help.Length > 0) ToolTipService.SetToolTip(e, help);
        if (Flag(n, "hidden")) e.Visibility = Visibility.Collapsed;
        if (Flag(n, "disabled") && e is Control c) c.IsEnabled = false;
        return e;
    }

    private static UIElement BuildScreenNodeInner(JsonElement n, ScreenAt at)
    {
        string id = Str(n, "id");
        switch (Str(n, "type"))
        {
            case "stack":
            {
                var panel = new StackPanel
                {
                    Orientation = Str(n, "axis") == "horizontal" ? Orientation.Horizontal : Orientation.Vertical,
                    Spacing = Num(n, "spacing", 8),
                };
                foreach (JsonElement c in Arr(n, "children"))
                {
                    UIElement child = BuildScreenNode(c, at);
                    if (child is FrameworkElement fe && panel.Orientation == Orientation.Vertical)
                    {
                        fe.HorizontalAlignment = Str(n, "align") switch
                        {
                            "center" => HorizontalAlignment.Center,
                            "end" => HorizontalAlignment.Right,
                            "fill" => HorizontalAlignment.Stretch,
                            _ => fe.HorizontalAlignment,
                        };
                    }
                    panel.Children.Add(child);
                }
                return panel;
            }
            case "grid":
            {
                int columns = Math.Max(1, (int)Num(n, "columns", 2));
                double spacing = Num(n, "spacing", 8);
                var grid = new Grid { ColumnSpacing = spacing, RowSpacing = spacing };
                for (int i = 0; i < columns; ++i) grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                int k = 0;
                foreach (JsonElement c in Arr(n, "children"))
                {
                    if (k % columns == 0) grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
                    var child = (FrameworkElement)BuildScreenNode(c, at);
                    Grid.SetColumn(child, k % columns);
                    Grid.SetRow(child, k / columns);
                    grid.Children.Add(child);
                    ++k;
                }
                return grid;
            }
            case "scroll":
            {
                var inner = new StackPanel { Spacing = 8 };
                foreach (JsonElement c in Arr(n, "children")) inner.Children.Add(BuildScreenNode(c, at));
                bool horizontal = Str(n, "axis") == "horizontal";
                if (horizontal) inner.Orientation = Orientation.Horizontal;
                var scroll = new ScrollViewer
                {
                    Content = inner,
                    HorizontalScrollBarVisibility = horizontal ? ScrollBarVisibility.Auto : ScrollBarVisibility.Disabled,
                    VerticalScrollBarVisibility = horizontal ? ScrollBarVisibility.Disabled : ScrollBarVisibility.Auto,
                };
                double h = Num(n, "height", 0);
                if (h > 0) scroll.MaxHeight = h;
                return scroll;
            }
            case "split":
            {
                double f = Num(n, "fraction", 0.5);
                bool vertical = Str(n, "axis") == "vertical";
                var grid = new Grid();
                JsonElement[] parts = Arr(n, "children").ToArray();
                if (vertical)
                {
                    grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(f, GridUnitType.Star) });
                    grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1) });
                    grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1 - f, GridUnitType.Star) });
                }
                else
                {
                    grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(f, GridUnitType.Star) });
                    grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1) });
                    grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1 - f, GridUnitType.Star) });
                }
                var rule = new Border { Background = Brush(Hairline) };
                if (vertical) Grid.SetRow(rule, 1); else Grid.SetColumn(rule, 1);
                grid.Children.Add(rule);
                for (int i = 0; i < Math.Min(2, parts.Length); ++i)
                {
                    var child = (FrameworkElement)BuildScreenNode(parts[i], at);
                    child.Margin = new Thickness(8);
                    if (vertical) Grid.SetRow(child, i * 2); else Grid.SetColumn(child, i * 2);
                    grid.Children.Add(child);
                }
                return grid;
            }
            case "text":
            {
                string style = Str(n, "style");
                TextBlock t = Text(Str(n, "text"),
                    style is "secondary" or "caption" ? Body : Title,
                    style switch
                    {
                        "title" => UiFontSize - 1,
                        "heading" => UiFontSize + 1,
                        "secondary" => UiFontSize - 3,
                        "caption" => UiFontSize - 4,
                        _ => UiFontSize - 2,
                    },
                    bold: style is "title" or "heading",
                    maxLines: (int)Num(n, "lines", 0));
                if (style == "mono") t.FontFamily = new FontFamily("Cascadia Mono,Consolas");
                if (Flag(n, "copyable")) t.IsTextSelectionEnabled = true;
                return t;
            }
            case "image":
            {
                var img = new Image { Stretch = Stretch.Uniform };
                double w = Num(n, "width", 0), h = Num(n, "height", 0);
                if (w > 0) img.Width = w;
                if (h > 0) img.Height = h;
                string? file = SlotImagePath(at.Addon, Str(n, "src"));
                if (file is not null)
                {
                    try { img.Source = new BitmapImage(new Uri(file)); }
                    catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
                }
                return img;
            }
            case "thumbnail":
                return SlotThumb(Str(n, "path"), Num(n, "size", 96), Str(n, "label"));
            case "thumbnail_grid":
                return BuildThumbnailGrid(n, at, id);
            case "list":
                return BuildRows(n, at, id, table: false);
            case "table":
                return BuildRows(n, at, id, table: true);
            case "button":
            {
                string label = Str(n, "label");
                Button b = EditButton(label, () => SendScreenEvent(at, id, SlotNative.EventClick, ""));
                string symbol = Str(n, "symbol");
                if (symbol.Length > 0)
                {
                    var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
                    row.Children.Add(new FontIcon { Glyph = SlotGlyph(symbol), FontFamily = SymbolFont, FontSize = 12, Foreground = Brush(Title) });
                    row.Children.Add(Text(label, Title, UiFontSize - 2));
                    b.Content = row;
                }
                if (Str(n, "style") == "primary") b.Background = Brush(Hairline);
                // Reached by Tab like every control here (EditButton's default keeps
                // focus off the canvas; a screen is a window of its own).
                b.AllowFocusOnInteraction = true;
                b.IsTabStop = true;
                return b;
            }
            case "toggle":
            {
                var t = new ToggleSwitch
                {
                    Header = Text(Str(n, "label"), Title, UiFontSize - 2),
                    IsOn = Flag(n, "value"),
                    OnContent = "",
                    OffContent = "",
                };
                t.Toggled += (_, _) => SendScreenEvent(at, id, SlotNative.EventChange, t.IsOn ? "true" : "false");
                return t;
            }
            case "choice":
            {
                var box = new ComboBox { Header = Text(Str(n, "label"), Title, UiFontSize - 2), MinWidth = 160 };
                string value = Str(n, "value");
                var ids = new List<string>();
                foreach (JsonElement o in Arr(n, "options"))
                {
                    ids.Add(Str(o, "id"));
                    box.Items.Add(new ComboBoxItem { Content = Str(o, "text") });
                }
                box.SelectedIndex = ids.IndexOf(value);
                box.SelectionChanged += (_, _) =>
                {
                    if (box.SelectedIndex >= 0 && box.SelectedIndex < ids.Count)
                        SendScreenEvent(at, id, SlotNative.EventChange, JsonString(ids[box.SelectedIndex]));
                };
                return box;
            }
            case "slider":
            {
                double lo = Num(n, "min", 0), hi = Num(n, "max", 1);
                var s = new Slider
                {
                    Header = Str(n, "label"),
                    Minimum = lo,
                    Maximum = hi,
                    StepFrequency = Num(n, "step", 0) > 0 ? Num(n, "step", 0) : (hi - lo) / 100,
                    Value = Num(n, "value", lo),
                    MinWidth = 160,
                };
                // Reported when let go (pointer or key), not on every step of a drag.
                void Report() => SendScreenEvent(at, id, SlotNative.EventChange, NumberJson(s.Value));
                s.PointerCaptureLost += (_, _) => Report();
                s.KeyUp += (_, e) =>
                {
                    if (e.Key is Windows.System.VirtualKey.Left or Windows.System.VirtualKey.Right
                        or Windows.System.VirtualKey.Up or Windows.System.VirtualKey.Down
                        or Windows.System.VirtualKey.Home or Windows.System.VirtualKey.End) Report();
                };
                return s;
            }
            case "number":
                return BuildNumber(n, at, id);
            case "text_field":
            {
                var field = new FakeInput(Str(n, "placeholder").Length > 0 ? Str(n, "placeholder") : Str(n, "label"), 220)
                {
                    Text = Str(n, "value"),
                };
                string last = field.Text;
                field.Submitted += () =>
                {
                    last = field.Text;
                    SendScreenEvent(at, id, SlotNative.EventSubmit, JsonString(field.Text));
                };
                field.LostFocus += (_, _) =>
                {
                    if (field.Text == last) return;
                    last = field.Text;
                    SendScreenEvent(at, id, SlotNative.EventChange, JsonString(field.Text));
                };
                return field;
            }
            case "progress":
            {
                double v = Num(n, "value", -1);
                var track = new Grid { Height = 6, MinWidth = 120, Background = Brush(Hairline), CornerRadius = new CornerRadius(3) };
                var fill = new Border
                {
                    Background = Brush(TrimAccent),
                    CornerRadius = new CornerRadius(3),
                    HorizontalAlignment = HorizontalAlignment.Left,
                };
                track.Children.Add(fill);
                // Indeterminate: a still third (no animation that would tick).
                track.SizeChanged += (_, _) => fill.Width = track.ActualWidth * (v < 0 ? 0.33 : Math.Clamp(v, 0, 1));
                AutomationProperties.SetHelpText(track, v < 0 ? "In progress" : $"{Math.Round(v * 100)} percent");
                return track;
            }
            case "separator":
                return new Border { Height = 1, Background = Brush(Hairline), Margin = new Thickness(0, 4, 0, 4) };
            case "surface":
                return BuildSurface(n, at);
            default:
                return new Border();
        }
    }

    private static string NumberJson(double v) =>
        v == Math.Round(v) && Math.Abs(v) < 1e15
            ? ((long)v).ToString(System.Globalization.CultureInfo.InvariantCulture)
            : v.ToString("R", System.Globalization.CultureInfo.InvariantCulture);

    private static UIElement BuildNumber(JsonElement n, ScreenAt at, string id)
    {
        double lo = Num(n, "min", -1e12), hi = Num(n, "max", 1e12), step = Num(n, "step", 1);
        double value = Num(n, "value", Math.Max(lo, 0));
        string label = Str(n, "label");
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        TextBlock shown = Text($"{label}: {NumberJson(value)}", Title, UiFontSize - 2);
        shown.VerticalAlignment = VerticalAlignment.Center;
        void Step(double by)
        {
            value = Math.Clamp(value + by, lo, hi);
            shown.Text = $"{label}: {NumberJson(value)}";
            SendScreenEvent(at, id, SlotNative.EventChange, NumberJson(value));
        }
        Button minus = EditButton("−", () => Step(-step));
        Button plus = EditButton("+", () => Step(step));
        foreach (Button b in new[] { minus, plus })
        {
            b.AllowFocusOnInteraction = true;
            b.IsTabStop = true;
        }
        AutomationProperties.SetName(minus, $"Decrease {label}");
        AutomationProperties.SetName(plus, $"Increase {label}");
        row.Children.Add(shown);
        row.Children.Add(minus);
        row.Children.Add(plus);
        // Up / Down on either button steps too.
        row.KeyDown += (_, e) =>
        {
            if (e.Key == Windows.System.VirtualKey.Up) { Step(step); e.Handled = true; }
            else if (e.Key == Windows.System.VirtualKey.Down) { Step(-step); e.Handled = true; }
        };
        return row;
    }

    // A list or table: one button per row, so each takes Tab, Space and Enter and
    // is named for Narrator. Up / Down move between rows.
    private static UIElement BuildRows(JsonElement n, ScreenAt at, string id, bool table)
    {
        HashSet<string> selected = SelectedSet(n);
        bool multi = Flag(n, "multi");
        var rows = new StackPanel { Spacing = 0 };
        var buttons = new List<Button>();
        JsonElement[] columns = table ? Arr(n, "columns").ToArray() : Array.Empty<JsonElement>();
        if (table)
        {
            var header = new Grid { Padding = new Thickness(8, 4, 8, 4) };
            for (int c = 0; c < columns.Length; ++c)
            {
                header.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                TextBlock h = Text(Str(columns[c], "title"), Body, UiFontSize - 4, bold: true);
                Grid.SetColumn(h, c);
                header.Children.Add(h);
            }
            rows.Children.Add(header);
            rows.Children.Add(new Border { Height = 1, Background = Brush(Hairline) });
        }
        foreach (JsonElement r in Arr(n, table ? "rows" : "items"))
        {
            string rowId = Str(r, "id");
            UIElement content;
            string name;
            if (table)
            {
                var g = new Grid();
                string[] cells = Arr(r, "cells").Select(c => c.GetString() ?? "").ToArray();
                for (int c = 0; c < columns.Length; ++c)
                {
                    g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                    TextBlock cell = Text(c < cells.Length ? cells[c] : "", Title, UiFontSize - 3, maxLines: 1);
                    Grid.SetColumn(cell, c);
                    g.Children.Add(cell);
                }
                content = g;
                name = string.Join(", ", cells);
            }
            else
            {
                var col = new StackPanel { Spacing = 1 };
                col.Children.Add(Text(Str(r, "text"), Title, UiFontSize - 2));
                string detail = Str(r, "detail");
                if (detail.Length > 0) col.Children.Add(Text(detail, Body, UiFontSize - 4));
                content = col;
                name = detail.Length > 0 ? $"{Str(r, "text")}, {detail}" : Str(r, "text");
            }
            Button b = EditButton("", () => { }, stretch: true);
            b.Content = content;
            b.HorizontalContentAlignment = HorizontalAlignment.Stretch;
            b.AllowFocusOnInteraction = true;
            b.IsTabStop = true;
            AutomationProperties.SetName(b, name);
            int index = buttons.Count;
            void Paint() => b.Background = selected.Contains(rowId) ? Brush(ChromeColour.Selection) : Brush(Colors.Transparent);
            Paint();
            b.Click += (_, _) =>
            {
                if (multi)
                {
                    if (!selected.Remove(rowId)) selected.Add(rowId);
                }
                else
                {
                    selected.Clear();
                    selected.Add(rowId);
                }
                RepaintRows();
                SendScreenEvent(at, id, SlotNative.EventSelect, JsonList(selected.OrderBy(s => s, StringComparer.Ordinal)));
            };
            b.DoubleTapped += (_, _) => SendScreenEvent(at, id, SlotNative.EventActivate, JsonString(rowId));
            b.KeyDown += (_, e) =>
            {
                if (e.Key == Windows.System.VirtualKey.Enter)
                {
                    SendScreenEvent(at, id, SlotNative.EventActivate, JsonString(rowId));
                    e.Handled = true;
                }
                else if (e.Key == Windows.System.VirtualKey.Down && index + 1 < buttons.Count)
                {
                    buttons[index + 1].Focus(FocusState.Keyboard);
                    e.Handled = true;
                }
                else if (e.Key == Windows.System.VirtualKey.Up && index > 0)
                {
                    buttons[index - 1].Focus(FocusState.Keyboard);
                    e.Handled = true;
                }
            };
            b.DataContext = (Action)Paint;
            buttons.Add(b);
            rows.Children.Add(b);
        }
        void RepaintRows()
        {
            foreach (Button other in buttons) (other.DataContext as Action)?.Invoke();
        }
        return new ScrollViewer
        {
            Content = rows,
            MaxHeight = 320,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
        };
    }

    // ---- thumbnails by path (the viewer's JPEG-512 cache) ------------------------------

    private static readonly SemaphoreSlim SlotThumbGate = new(2);
    private static readonly Dictionary<string, string?> SlotThumbCache = new(StringComparer.Ordinal);

    private static FrameworkElement SlotThumb(string path, double size, string label)
    {
        var img = new Image { Width = size, Height = size, Stretch = Stretch.Uniform };
        var box = new Border
        {
            Width = size,
            Height = size,
            CornerRadius = new CornerRadius(6),
            Background = Brush(ChromeColour.Surface),
            Child = img,
        };
        AutomationProperties.SetName(box, label.Length > 0 ? label : Path.GetFileName(path));
        LoadSlotThumb(path, img, size);
        return box;
    }

    // Asked as the tile is realised, made on a worker, two at a time; a tile
    // recycled meanwhile is not painted (its Tag no longer names the path).
    private static void LoadSlotThumb(string path, Image img, double size)
    {
        img.Tag = path;
        if (SlotThumbCache.TryGetValue(path, out string? known))
        {
            SetSlotThumb(img, known, size);
            return;
        }
        _ = Task.Run(async () =>
        {
            await SlotThumbGate.WaitAsync().ConfigureAwait(false);
            string? jpeg = null;
            try
            {
                jpeg = SlotNative.ThumbnailPath(path);
            }
            finally
            {
                SlotThumbGate.Release();
            }
            DispatcherQueueControllerTryEnqueue(() =>
            {
                if (SlotThumbCache.Count > 4000) SlotThumbCache.Clear();
                SlotThumbCache[path] = jpeg;
                if (img.Tag is string p && p == path) SetSlotThumb(img, jpeg, size);
            });
        });
    }

    private static void SetSlotThumb(Image img, string? jpeg, double size)
    {
        if (jpeg is null)
        {
            img.Source = null;
            return;
        }
        try { img.Source = new BitmapImage(new Uri(jpeg)) { DecodePixelWidth = (int)Math.Min(512, size * 2) }; }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
    }

    // A virtualised grid: ItemsRepeater realises only what is in view, and each
    // tile asks for its thumbnail as it is realised. Arrow keys move between
    // realised tiles; Space selects, Enter activates.
    private sealed record SlotTileVm(string Path, string Label, int Index);

    private static UIElement BuildThumbnailGrid(JsonElement n, ScreenAt at, string id)
    {
        double tile = Num(n, "tile", 128);
        bool multi = Flag(n, "multi");
        HashSet<string> selected = SelectedSet(n);
        var vms = new List<SlotTileVm>();
        foreach (JsonElement item in Arr(n, "items"))
        {
            string path = Str(item, "path");
            string label = Str(item, "label");
            vms.Add(new SlotTileVm(path, label.Length > 0 ? label : Path.GetFileName(path), vms.Count));
        }
        var realised = new Dictionary<int, Button>();
        ScrollViewer? scroller = null;
        var repeater = new ItemsRepeater
        {
            Layout = new UniformGridLayout { MinItemWidth = tile, MinItemHeight = tile, MinColumnSpacing = 8, MinRowSpacing = 8 },
            ItemsSource = vms,
        };
        repeater.ItemTemplate = new SlotTileFactory(tile, vm =>
        {
            if (multi)
            {
                if (!selected.Remove(vm.Path)) selected.Add(vm.Path);
            }
            else
            {
                selected.Clear();
                selected.Add(vm.Path);
            }
            foreach (Button b in realised.Values) b.BorderThickness = new Thickness(selected.Contains(((SlotTileVm)b.DataContext).Path) ? 2 : 0);
            SendScreenEvent(at, id, SlotNative.EventSelect, JsonList(selected.OrderBy(s => s, StringComparer.Ordinal)));
        }, vm => SendScreenEvent(at, id, SlotNative.EventActivate, JsonString(vm.Path)), selected, realised,
        (from, delta) =>
        {
            int to = Math.Clamp(from + delta, 0, vms.Count - 1);
            UIElement? target = repeater.GetOrCreateElement(to);
            target?.StartBringIntoView();
            if (target is Button b) b.Focus(FocusState.Keyboard);
        }, () => scroller is null ? 1 : Math.Max(1, (int)((scroller.ViewportWidth + 8) / (tile + 8))));
        scroller = new ScrollViewer
        {
            Content = repeater,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            MinHeight = tile + 16,
        };
        return scroller;
    }

    private sealed class SlotTileFactory(double tile, Action<SlotTileVm> select, Action<SlotTileVm> activate,
        HashSet<string> selected, Dictionary<int, Button> realised, Action<int, int> move, Func<int> columns)
        : IElementFactory
    {
        public UIElement GetElement(ElementFactoryGetArgs args)
        {
            var vm = (SlotTileVm)args.Data;
            FrameworkElement thumb = SlotThumb(vm.Path, tile, vm.Label);
            var b = new Button
            {
                Content = thumb,
                Padding = new Thickness(0),
                Background = Brush(Colors.Transparent),
                BorderBrush = Brush(TrimAccent),
                BorderThickness = new Thickness(selected.Contains(vm.Path) ? 2 : 0),
                DataContext = vm,
                IsTabStop = true,
            };
            FlattenButton(b);
            AutomationProperties.SetName(b, vm.Label);
            b.Click += (_, _) => select(vm);
            b.DoubleTapped += (_, _) => activate(vm);
            b.KeyDown += (_, e) =>
            {
                int step = e.Key switch
                {
                    Windows.System.VirtualKey.Left => -1,
                    Windows.System.VirtualKey.Right => 1,
                    Windows.System.VirtualKey.Up => -columns(),
                    Windows.System.VirtualKey.Down => columns(),
                    _ => 0,
                };
                if (step != 0)
                {
                    move(vm.Index, step);
                    e.Handled = true;
                }
                else if (e.Key == Windows.System.VirtualKey.Enter)
                {
                    activate(vm);
                    e.Handled = true;
                }
            };
            realised[vm.Index] = b;
            return b;
        }

        public void RecycleElement(ElementFactoryRecycleArgs args)
        {
            if (args.Element is Button { DataContext: SlotTileVm vm } b)
            {
                realised.Remove(vm.Index);
                if (b.Content is Border { Child: Image img }) img.Tag = null;  // a late thumbnail paints nothing
            }
        }
    }

    // The add-on's pixels with the chrome's pin over them, in the accent colour
    // (docs/plans/metadata-search-and-place.md slice 3). One element for
    // Narrator, named by its label.
    private static UIElement BuildSurface(JsonElement n, ScreenAt at)
    {
        double height = Num(n, "height", 120);
        double width = Num(n, "width", 0);
        var grid = new Grid
        {
            Height = height,
            CornerRadius = new CornerRadius(6),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(1),
            Background = Brush(ChromeColour.Surface),
        };
        if (width > 0) grid.Width = width;
        var px = SlotNative.Surface(at.Addon, at.Screen, Str(n, "surface"));
        if (px is { } p && p.Width > 0 && p.Height > 0)
        {
            try
            {
                var bmp = new WriteableBitmap(p.Width, p.Height);
                using (Stream s = System.Runtime.InteropServices.WindowsRuntime.WindowsRuntimeBufferExtensions.AsStream(bmp.PixelBuffer))
                {
                    s.Write(p.Pixels, 0, Math.Min(p.Pixels.Length, p.Width * p.Height * 4));
                }
                bmp.Invalidate();
                grid.Children.Add(new Image { Source = bmp, Stretch = Stretch.UniformToFill });
            }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
        }
        if (n.TryGetProperty("pin", out JsonElement pin) && pin.ValueKind == JsonValueKind.Object)
        {
            double x = Num(pin, "x", 0.5), y = Num(pin, "y", 0.5);
            var glyph = new FontIcon
            {
                Glyph = "",  // MapPin
                FontFamily = SymbolFont,
                FontSize = 22,
                Foreground = Brush(TrimAccent),
                HorizontalAlignment = HorizontalAlignment.Left,
                VerticalAlignment = VerticalAlignment.Top,
                IsHitTestVisible = false,
            };
            grid.Children.Add(glyph);
            grid.SizeChanged += (_, _) => glyph.Margin = new Thickness(grid.ActualWidth * x - 11, grid.ActualHeight * y - 22, 0, 0);
        }
        return grid;
    }

    // ---- windows, docked panes, sheets ---------------------------------------------

    private static readonly Dictionary<ScreenAt, (Window Window, long Revision)> ScreenWindows = new();
    private static readonly HashSet<ScreenAt> ClosingFromCore = new();

    [StructLayout(LayoutKind.Sequential)]
    private struct SlotRect { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll", EntryPoint = "GetWindowRect")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SlotGetWindowRect(IntPtr hwnd, out SlotRect rect);

    // UI thread: the open screens as the core lists them; a window per screen,
    // built from its tree and rebuilt when the tree's revision moves.
    private static void SyncScreenWindows(JsonElement list)
    {
        var wanted = new HashSet<ScreenAt>();
        foreach (JsonElement s in list.EnumerateArray())
        {
            var at = new ScreenAt(Str(s, "addon"), Str(s, "id"));
            wanted.Add(at);
            long revision = (long)Num(s, "revision", 0);
            string place = Str(s, "place");
            if (!ScreenWindows.TryGetValue(at, out var open))
            {
                Window w = OpenScreenWindow(s, at, place);
                ScreenWindows[at] = (w, revision);
                w.Content = BuildScreenRoot(s, at, place);
            }
            else if (open.Revision != revision)
            {
                open.Window.Content = BuildScreenRoot(s, at, place);
                ScreenWindows[at] = (open.Window, revision);
            }
        }
        foreach (ScreenAt gone in ScreenWindows.Keys.Where(k => !wanted.Contains(k)).ToList())
        {
            ClosingFromCore.Add(gone);
            try { ScreenWindows[gone].Window.Close(); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
            ScreenWindows.Remove(gone);
            ClosingFromCore.Remove(gone);
        }
    }

    private static UIElement BuildScreenRoot(JsonElement s, ScreenAt at, string place)
    {
        var root = new Grid { Background = Brush(Canvas), RequestedTheme = IslandTheme, Padding = new Thickness(12) };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        if (place != "window")
        {
            var head = new Grid { Margin = new Thickness(0, 0, 0, 8) };
            head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            head.Children.Add(Text(Str(s, "title"), Title, UiFontSize, bold: true));
            Button done = EditButton("Done", () => SlotNative.CloseScreen(at.Addon, at.Screen));
            done.AllowFocusOnInteraction = true;
            done.IsTabStop = true;
            Grid.SetColumn(done, 1);
            head.Children.Add(done);
            root.Children.Add(head);
        }
        if (s.TryGetProperty("tree", out JsonElement tree) && tree.ValueKind == JsonValueKind.Object)
        {
            var body = (FrameworkElement)BuildScreenNode(tree, at);
            Grid.SetRow(body, 1);
            root.Children.Add(body);
        }
        // Esc closes a pane or a sheet, as the Done button does.
        root.KeyDown += (_, e) =>
        {
            if (e.Key == Windows.System.VirtualKey.Escape && place != "window")
            {
                SlotNative.CloseScreen(at.Addon, at.Screen);
                e.Handled = true;
            }
        };
        return root;
    }

    private static Window OpenScreenWindow(JsonElement s, ScreenAt at, string place)
    {
        var w = new Window { Title = Str(s, "title") };
        int width = (int)Num(s, "width", 0), height = (int)Num(s, "height", 0);
        SlotRect main = default;
        bool haveMain = _themeWindow != IntPtr.Zero && SlotGetWindowRect(_themeWindow, out main);
        switch (place)
        {
            case "pane":
            {
                // Docked to the right edge of the viewer window.
                int pw = width > 0 ? width : 340;
                int ph = haveMain ? main.Bottom - main.Top : (height > 0 ? height : 640);
                w.AppWindow.Resize(new Windows.Graphics.SizeInt32(pw, ph));
                if (haveMain) w.AppWindow.Move(new Windows.Graphics.PointInt32(main.Right - pw, main.Top));
                break;
            }
            case "sheet":
            {
                int sw = width > 0 ? width : 520, sh = height > 0 ? height : 420;
                w.AppWindow.Resize(new Windows.Graphics.SizeInt32(sw, sh));
                if (haveMain)
                {
                    w.AppWindow.Move(new Windows.Graphics.PointInt32(
                        main.Left + (main.Right - main.Left - sw) / 2, main.Top + 60));
                }
                break;
            }
            default:
                w.AppWindow.Resize(new Windows.Graphics.SizeInt32(width > 0 ? width : 760, height > 0 ? height : 560));
                break;
        }
        w.Closed += (_, _) =>
        {
            if (ClosingFromCore.Contains(at)) return;
            ScreenWindows.Remove(at);
            SlotNative.CloseScreen(at.Addon, at.Screen);
        };
        w.Activate();
        return w;
    }
}
