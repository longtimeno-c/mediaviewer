// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
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

    // Lazy, like IconFont: Attach touches IslandHost statics (RegisterUiFont)
    // before EnsureApp starts XAML, and a FontFamily built then throws
    // RPC_E_WRONG_THREAD from the type initializer, so no island attaches.
    private static FontFamily? _symbolFont;
    private static FontFamily SymbolFont => _symbolFont ??= new("Segoe Fluent Icons,Segoe MDL2 Assets");

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
        "minus" => "",
        _ => "",
    };

    private static string JStr(JsonElement n, string key) =>
        n.TryGetProperty(key, out JsonElement v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";

    private static double JNum(JsonElement n, string key, double fallback) =>
        n.TryGetProperty(key, out JsonElement v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : fallback;

    private static bool JFlag(JsonElement n, string key) =>
        n.TryGetProperty(key, out JsonElement v) && v.ValueKind == JsonValueKind.True;

    private static IEnumerable<JsonElement> JArr(JsonElement n, string key) =>
        n.TryGetProperty(key, out JsonElement v) && v.ValueKind == JsonValueKind.Array
            ? v.EnumerateArray() : Enumerable.Empty<JsonElement>();

    private static HashSet<string> SelectedSet(JsonElement n) =>
        new(JArr(n, "selection").Where(e => e.ValueKind == JsonValueKind.String).Select(e => e.GetString() ?? ""),
            StringComparer.Ordinal);

    private static void SendScreenEvent(ScreenAt at, string node, uint kind, string valueJson) =>
        SlotNative.Event(at.Addon, at.Screen, node, kind, valueJson);

    private static string JsonString(string s) => JsonSerializer.Serialize(s);

    private static string JsonList(IEnumerable<string> list) => JsonSerializer.Serialize(list.ToArray());

    // ---- the renderer ---------------------------------------------------------------

    private static UIElement BuildScreenNode(JsonElement n, ScreenAt at)
    {
        UIElement e = BuildScreenNodeInner(n, at);
        // A row built around a control (#508: a toggle beside its label): the
        // control carries the name, the help and the enabled state.
        UIElement named = e is FrameworkElement { Tag: ScreenInline inline } ? inline.Control : e;
        string label = JStr(n, "label");
        if (label.Length > 0) AutomationProperties.SetName(named, label);
        string help = JStr(n, "help");
        if (help.Length > 0) ToolTipService.SetToolTip(named, help);
        if (JFlag(n, "hidden")) e.Visibility = Visibility.Collapsed;
        if (JFlag(n, "disabled") && named is Control c) c.IsEnabled = false;
        if (e is FrameworkElement fe) RegisterScreenId(JStr(n, "id"), fe);
        return e;
    }

    private static UIElement BuildScreenNodeInner(JsonElement n, ScreenAt at)
    {
        string id = JStr(n, "id");
        switch (JStr(n, "type"))
        {
            case "stack" when JStr(n, "axis") == "horizontal" &&
                              JArr(n, "children").Any(c => JStr(c, "type") == "text_field"):
            {
                // #508: a field and its buttons on one row, as the Mac's HStack:
                // the field takes the width the buttons leave.
                var row = new Grid { ColumnSpacing = JNum(n, "spacing", 8) };
                int col = 0;
                foreach (JsonElement c in JArr(n, "children"))
                {
                    bool field = JStr(c, "type") == "text_field";
                    row.ColumnDefinitions.Add(new ColumnDefinition
                    {
                        Width = field ? new GridLength(1, GridUnitType.Star) : GridLength.Auto,
                    });
                    var child = (FrameworkElement)BuildScreenNode(c, at);
                    child.VerticalAlignment = VerticalAlignment.Center;
                    Grid.SetColumn(child, col++);
                    row.Children.Add(child);
                }
                return ScreenRevealed(JStr(n, "look") == "card" ? ScreenCard(row) : row, n, at);
            }
            case "stack":
            {
                var panel = new StackPanel
                {
                    Orientation = JStr(n, "axis") == "horizontal" ? Orientation.Horizontal : Orientation.Vertical,
                    Spacing = JNum(n, "spacing", 8),
                };
                foreach (JsonElement c in JArr(n, "children"))
                {
                    UIElement child = BuildScreenNode(c, at);
                    if (child is FrameworkElement hfe && panel.Orientation == Orientation.Horizontal)
                    {
                        hfe.VerticalAlignment = VerticalAlignment.Center;
                    }
                    if (child is FrameworkElement fe && panel.Orientation == Orientation.Vertical)
                    {
                        // A node that fills its width on the Mac (`maxWidth: .infinity`, a
                        // TextField, a ProgressView) still fills in a centred stack: centring a
                        // link picture shrank its path column to nothing, so the picture was
                        // two circles squashed against the badge.
                        bool fills = JStr(c, "type") is "link" or "progress" or "separator" or "text_field";
                        fe.HorizontalAlignment = JStr(n, "align") switch
                        {
                            _ when fills => HorizontalAlignment.Stretch,
                            "center" => HorizontalAlignment.Center,
                            "end" => HorizontalAlignment.Right,
                            "fill" => HorizontalAlignment.Stretch,
                            _ => fe.HorizontalAlignment,
                        };
                        // Text in a centred stack is centred too (#442's code and accept cards).
                        if (fe is TextBlock tb && JStr(n, "align") == "center") tb.TextAlignment = TextAlignment.Center;
                    }
                    panel.Children.Add(child);
                }
                // "look": "card" (#442): a rounded surface, as Settings' cards; "reveal"
                // (API 9): brought into view and marked when its token moves.
                return ScreenRevealed(JStr(n, "look") == "card" ? ScreenCard(panel) : panel, n, at);
            }
            case "grid":
            {
                int columns = Math.Max(1, (int)JNum(n, "columns", 2));
                double spacing = JNum(n, "spacing", 8);
                var grid = new Grid { ColumnSpacing = spacing, RowSpacing = spacing };
                for (int i = 0; i < columns; ++i) grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                int k = 0;
                foreach (JsonElement c in JArr(n, "children"))
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
                var inner = new StackPanel { Spacing = Space(8) };
                foreach (JsonElement c in JArr(n, "children")) inner.Children.Add(BuildScreenNode(c, at));
                bool horizontal = JStr(n, "axis") == "horizontal";
                if (horizontal) inner.Orientation = Orientation.Horizontal;
                var scroll = new ScrollViewer
                {
                    Content = inner,
                    HorizontalScrollBarVisibility = horizontal ? ScrollBarVisibility.Auto : ScrollBarVisibility.Disabled,
                    VerticalScrollBarVisibility = horizontal ? ScrollBarVisibility.Disabled : ScrollBarVisibility.Auto,
                };
                double h = JNum(n, "height", 0);
                if (h > 0) scroll.MaxHeight = h;
                return scroll;
            }
            case "split":
            {
                double f = JNum(n, "fraction", 0.5);
                bool vertical = JStr(n, "axis") == "vertical";
                var grid = new Grid();
                JsonElement[] parts = JArr(n, "children").ToArray();
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
                    child.Margin = Pad(8);
                    if (vertical) Grid.SetRow(child, i * 2); else Grid.SetColumn(child, i * 2);
                    grid.Children.Add(child);
                }
                return grid;
            }
            case "text":
            {
                string style = JStr(n, "style");
                (double size, bool bold) = ScreenTextStyle(style);
                TextBlock t = SText(JStr(n, "text"),
                    style is "secondary" or "caption" ? Body : Title,
                    size,
                    bold: bold,
                    maxLines: (int)JNum(n, "lines", 0),
                    display: style == "heading");
                if (style == "heading")
                {
                    AutomationProperties.SetHeadingLevel(t, Microsoft.UI.Xaml.Automation.Peers.AutomationHeadingLevel.Level2);
                }
                if (style == "mono") t.FontFamily = new FontFamily("Cascadia Mono,Consolas");
                if (style == "display")
                {
                    // #442: a short code read off the screen across a room.
                    t.FontFamily = new FontFamily("Segoe UI Variable Display,Segoe UI");
                    t.FontSize = UiFontSize + 20;
                    t.FontWeight = Microsoft.UI.Text.FontWeights.SemiBold;
                    t.CharacterSpacing = 60;
                }
                if (JFlag(n, "copyable")) t.IsTextSelectionEnabled = true;
                return t;
            }
            case "image":
            {
                var img = new Image { Stretch = Stretch.Uniform };
                double w = JNum(n, "width", 0), h = JNum(n, "height", 0);
                if (w > 0) img.Width = w;
                if (h > 0) img.Height = h;
                string? file = SlotImagePath(at.Addon, JStr(n, "src"));
                if (file is not null)
                {
                    try { img.Source = new BitmapImage(new Uri(file)); }
                    catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
                }
                return img;
            }
            case "thumbnail":
                return SlotThumb(JStr(n, "path"), JNum(n, "size", 96), JStr(n, "label"));
            case "thumbnail_grid":
                return BuildThumbnailGrid(n, at, id);
            case "list":
                return BuildRows(n, at, id, table: false);
            case "table":
                return BuildRows(n, at, id, table: true);
            case "button":
            {
                string label = JStr(n, "label");
                // "copy" (2026-10-19): the click also puts this text on the
                // clipboard, here, and the label says "Copied" for a moment.
                string copy = JStr(n, "copy");
                Button? self = null;
                Button b = ScreenButton(label, () =>
                {
                    if (copy.Length > 0 && self != null) CopyToClipboard(self, copy, label);
                    SendScreenEvent(at, id, SlotNative.EventClick, "");
                }, JStr(n, "style"));
                self = b;
                string symbol = JStr(n, "symbol");
                if (symbol.Length > 0)
                {
                    var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(6) };
                    row.Children.Add(new FontIcon { Glyph = SlotGlyph(symbol), FontFamily = SymbolFont, FontSize = 12, Foreground = b.Foreground });
                    TextBlock words = SText(label, Title, UiFontSize - 2);
                    words.Foreground = b.Foreground;
                    row.Children.Add(words);
                    b.Content = row;
                }
                return b;
            }
            case "toggle":
            {
                var t = new ToggleSwitch
                {
                    IsOn = JFlag(n, "value"),
                    OnContent = "",
                    OffContent = "",
                };
                t.Toggled += (_, _) => SendScreenEvent(at, id, SlotNative.EventChange, t.IsOn ? "true" : "false");
                return InlineToggle(JStr(n, "label"), t);
            }
            case "choice":
            {
                var box = new ComboBox { Header = SText(JStr(n, "label"), Title, UiFontSize - 2), MinWidth = 160 };
                string value = JStr(n, "value");
                var ids = new List<string>();
                foreach (JsonElement o in JArr(n, "options"))
                {
                    ids.Add(JStr(o, "id"));
                    box.Items.Add(new ComboBoxItem { Content = JStr(o, "text") });
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
                double lo = JNum(n, "min", 0), hi = JNum(n, "max", 1);
                var s = new Slider
                {
                    Header = JStr(n, "label"),
                    Minimum = lo,
                    Maximum = hi,
                    StepFrequency = JNum(n, "step", 0) > 0 ? JNum(n, "step", 0) : (hi - lo) / 100,
                    Value = JNum(n, "value", lo),
                    MinWidth = 160,
                };
                // Reported when let go (pointer or key), not on every step of a drag.
                void Report() => SendScreenEvent(at, id, SlotNative.EventChange, ScreenNumberJson(s.Value));
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
            case "text_field" when JStr(n, "look") == "code":
                return BuildCodeField(n, at, id);
            case "text_field":
            {
                // #508: as wide as its row allows (the Mac's TextField), never
                // narrower than a short path.
                var field = new FakeInput(JStr(n, "placeholder").Length > 0 ? JStr(n, "placeholder") : JStr(n, "label"),
                                          look: InScreenWindow ? ScreenInputLook(_screenFace!) : null)
                {
                    Text = JStr(n, "value"),
                    MinWidth = 180,
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
                if (JStr(n, "pick") != "folder") return field;
                // "pick": "folder" (#410): the system folder dialog, reported as the
                // field's change; typing a path still works.
                var row = new Grid { ColumnSpacing = Space(8) };
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                row.Children.Add(field);
                Button choose = ScreenButton("Choose\u2026", () => PickScreenFolder(at, id, field, v => last = v));
                choose.VerticalAlignment = VerticalAlignment.Center;
                AutomationProperties.SetName(choose, "Choose a folder for " + JStr(n, "label"));
                Grid.SetColumn(choose, 1);
                row.Children.Add(choose);
                return row;
            }
            case "progress" when JStr(n, "look") == "ring":
                return ScreenRing(JNum(n, "value", -1), JNum(n, "size", 64));
            case "progress" when JStr(n, "look") == "glow":
                return BuildGlowBar(JNum(n, "value", -1));  // contribution API 10 (IslandHost.Glow.cs)
            case "tiles":
                return BuildTiles(n, at, id);
            case "link":
                return BuildLink(n);
            case "progress":
            {
                double v = JNum(n, "value", -1);
                var track = new Grid { Height = 6, MinWidth = 120, Background = Brush(Hairline), CornerRadius = new CornerRadius(3) };  // a pill: half its height
                var fill = new Border
                {
                    Background = Brush(TrimAccent),
                    CornerRadius = new CornerRadius(3),  // the bar's pill ends: half its height
                    HorizontalAlignment = HorizontalAlignment.Left,
                };
                track.Children.Add(fill);
                // Indeterminate: a still third (no animation that would tick).
                track.SizeChanged += (_, _) => fill.Width = track.ActualWidth * (v < 0 ? 0.33 : Math.Clamp(v, 0, 1));
                AutomationProperties.SetHelpText(track, v < 0 ? "In progress" : $"{Math.Round(v * 100)} percent");
                return track;
            }
            case "separator":
                return new Border { Height = 1, Background = Brush(Hairline), Margin = Pad(0, 4, 0, 4) };
            case "surface":
                return WithGlow(BuildSurface(n, at), JStr(n, "glow"));
            default:
                return new Border();
        }
    }

    // ---- 2026-10-19 additions: a button's copy, the link picture --------------------

    private static void CopyToClipboard(Button b, string text, string label)
    {
        try
        {
            var package = new Windows.ApplicationModel.DataTransfer.DataPackage();
            package.SetText(text);
            Windows.ApplicationModel.DataTransfer.Clipboard.SetContent(package);
        }
        catch (Exception)
        {
            return;  // the clipboard is busy (another app holds it): the click still counts
        }
        object? before = b.Content;
        b.Content = SText("✓ Copied", Title, UiFontSize - 2);
        AutomationProperties.SetName(b, "Copied");
        DispatcherQueueTimer? t = b.DispatcherQueue?.CreateTimer();
        if (t == null) return;
        t.Interval = TimeSpan.FromMilliseconds(1500);
        t.IsRepeating = false;
        t.Tick += (_, _) =>
        {
            b.Content = before;
            AutomationProperties.SetName(b, label);
        };
        t.Start();
    }

    /// <summary>
    /// The path a transfer takes (ScreenView.swift's LinkNode is the twin):
    /// this computer on the left, the other on the right, the bearer's badge
    /// between them, and dots that travel the way the bytes go. Wi-Fi is a
    /// straight line, Bluetooth short dashes, the internet an arc, the DHT an
    /// arc through a few nodes, a relay an arc through the computer in the
    /// middle. Dots move only while "moving" or "connecting", at 30 fps at
    /// most, never with Windows' animations off, and stop when it leaves the
    /// tree; done and failed are still.
    /// </summary>
    // Where each live link's dots are (0..1 along the path) and when, kept across
    // rebuilds of its window.
    private static readonly System.Diagnostics.Stopwatch LinkClock = System.Diagnostics.Stopwatch.StartNew();
    private static readonly Dictionary<string, (double Phase, double At)> LinkPhases = new(StringComparer.Ordinal);

    private static FrameworkElement BuildLink(JsonElement n)
    {
        string via = JStr(n, "via"), state = JStr(n, "state");
        bool sending = JStr(n, "direction") != "receive";
        double value = JNum(n, "value", -1);
        Windows.UI.Color tint = via switch
        {
            "bluetooth" => Windows.UI.Color.FromArgb(255, 10, 132, 255),
            "wifi" => Windows.UI.Color.FromArgb(255, 48, 176, 199),
            "internet" => Windows.UI.Color.FromArgb(255, 94, 92, 230),
            "dht" => Windows.UI.Color.FromArgb(255, 255, 149, 0),
            _ => Windows.UI.Color.FromArgb(255, 175, 82, 222),
        };
        string glyph = state == "paused" ? "" : via switch
        {
            "bluetooth" => "",
            "wifi" => "",
            "internet" => "",
            "dht" => "",
            "relay" => "",
            _ => "",
        };
        bool reduce = false;
        try { reduce = !new Windows.UI.ViewManagement.UISettings().AnimationsEnabled; } catch (Exception) { }
        bool failed = state == "failed";
        double lift = via is "internet" or "dht" or "relay" or "server" ? 22 : 0;
        const double H = 64;

        var root = new StackPanel { Spacing = Space(6), HorizontalAlignment = HorizontalAlignment.Stretch };
        var strip = new Grid { HorizontalAlignment = HorizontalAlignment.Stretch };
        strip.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(88) });
        strip.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        strip.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(88) });

        FrameworkElement Device(string? symbol, string name, string mark)
        {
            var face = new Grid { Width = 48, Height = 48, HorizontalAlignment = HorizontalAlignment.Center };
            face.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse { Fill = Brush(ChromeColour.Surface), Stroke = Brush(Hairline), StrokeThickness = 1 });
            if (symbol != null)
            {
                face.Children.Add(new FontIcon { Glyph = symbol, FontFamily = SymbolFont, FontSize = 20, Foreground = Brush(Title) });
            }
            else
            {
                string ini = string.Concat(name.Split(new[] { ' ', '-', '_', '.' }, StringSplitOptions.RemoveEmptyEntries)
                                               .Take(2).Select(w => char.ToUpperInvariant(w[0])));
                TextBlock mono = SText(ini.Length > 0 ? ini : "?", Title, 17, bold: true);
                mono.HorizontalAlignment = HorizontalAlignment.Center;
                mono.VerticalAlignment = VerticalAlignment.Center;
                face.Children.Add(mono);
            }
            if (mark.Length > 0)
            {
                face.Children.Add(new Border
                {
                    Width = 18,
                    Height = 18,
                    CornerRadius = new CornerRadius(9),
                    Background = new SolidColorBrush(mark == "done" ? Windows.UI.Color.FromArgb(255, 52, 168, 83)
                                                                    : Windows.UI.Color.FromArgb(255, 230, 80, 60)),
                    HorizontalAlignment = HorizontalAlignment.Right,
                    VerticalAlignment = VerticalAlignment.Bottom,
                    Child = new FontIcon { Glyph = mark == "done" ? "" : "", FontFamily = SymbolFont, FontSize = 10, Foreground = new SolidColorBrush(Colors.White) },
                });
            }
            var col = new StackPanel { Spacing = 4, Width = 88, VerticalAlignment = VerticalAlignment.Top };
            col.Children.Add(face);
            // Two lines, as a Nearby tile's name: "The other computer" fits, a long name
            // ends in an ellipsis on the second line instead of the first.
            TextBlock label = SText(name, Body, UiFontSize - 3, maxLines: 2);
            label.HorizontalAlignment = HorizontalAlignment.Center;
            label.TextAlignment = TextAlignment.Center;
            label.TextTrimming = TextTrimming.CharacterEllipsis;
            col.Children.Add(label);
            return col;
        }

        FrameworkElement left = Device("", JStr(n, "from") is { Length: > 0 } f ? f : "This computer", "");
        FrameworkElement right = Device(null, JStr(n, "to"), state == "done" ? "done" : failed ? "failed" : "");
        Grid.SetColumn(right, 2);
        strip.Children.Add(left);
        strip.Children.Add(right);

        // The path, drawn on a canvas sized to its column. The devices are top-aligned (a
        // two-line name grows downward), so the canvas's middle is lifted to the circles'
        // middle (24): the path meets each computer at its centre.
        var canvas = new Microsoft.UI.Xaml.Controls.Canvas
        {
            Height = H,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Top,
            Margin = new Thickness(0, 24 - H / 2, 0, 0),
        };
        Grid.SetColumn(canvas, 1);
        strip.Children.Add(canvas);
        var track = new Microsoft.UI.Xaml.Shapes.Polyline
        {
            Stroke = new SolidColorBrush(failed ? Windows.UI.Color.FromArgb(110, 128, 128, 128)
                                                : Windows.UI.Color.FromArgb(72, tint.R, tint.G, tint.B)),
            StrokeThickness = 3,
            StrokeStartLineCap = PenLineCap.Round,
            StrokeEndLineCap = PenLineCap.Round,
        };
        if (via == "bluetooth") track.StrokeDashArray = new DoubleCollection { 1, 1.7 };
        if (via == "dht") track.StrokeDashArray = new DoubleCollection { 0.3, 2 };
        var moved = new Microsoft.UI.Xaml.Shapes.Polyline
        {
            Stroke = new SolidColorBrush(tint),
            StrokeThickness = 3,
            StrokeStartLineCap = PenLineCap.Round,
            StrokeEndLineCap = PenLineCap.Round,
        };
        canvas.Children.Add(track);
        canvas.Children.Add(moved);
        var nodes = new List<Microsoft.UI.Xaml.Shapes.Ellipse>();
        if (via == "dht")
        {
            for (int i = 0; i < 4; i++)
            {
                var e = new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 5, Height = 5, Fill = new SolidColorBrush(Windows.UI.Color.FromArgb(115, tint.R, tint.G, tint.B)) };
                nodes.Add(e);
                canvas.Children.Add(e);
            }
        }
        bool live = state is "moving" or "connecting";
        bool moving = state == "moving";
        int count = live ? (moving ? 5 : 3) : 0;
        var dots = new List<Microsoft.UI.Xaml.Shapes.Ellipse>();
        double r = moving ? 3.5 : 3;
        for (int i = 0; i < count; i++)
        {
            var d = new Microsoft.UI.Xaml.Shapes.Ellipse { Width = r * 2, Height = r * 2, Fill = new SolidColorBrush(tint) };
            dots.Add(d);
            canvas.Children.Add(d);
        }
        var badge = new Grid { Width = 34, Height = 34 };
        badge.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse { Fill = Brush(ChromeColour.Canvas) });
        badge.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse
        {
            Fill = new SolidColorBrush(Windows.UI.Color.FromArgb(failed ? (byte)20 : (byte)41, tint.R, tint.G, tint.B)),
            Stroke = new SolidColorBrush(Windows.UI.Color.FromArgb(153, tint.R, tint.G, tint.B)),
            StrokeThickness = 1.5,
        });
        badge.Children.Add(new FontIcon { Glyph = glyph, FontFamily = SymbolFont, FontSize = 15, Foreground = failed ? Brush(ChromeColour.Disabled) : new SolidColorBrush(tint) });
        canvas.Children.Add(badge);
        // A relay: the computer in between, named under it.
        TextBlock? through = JStr(n, "through") is { Length: > 0 } th ? SText(th, Body, UiFontSize - 4, maxLines: 1) : null;
        if (through != null) canvas.Children.Add(through);

        Windows.Foundation.Point At(double s, double w)
        {
            double y0 = H / 2 + lift * 0.25;
            return new Windows.Foundation.Point(6 + (w - 12) * s, y0 - lift * 4 * s * (1 - s));
        }
        PointCollection Line(double a, double b, double w)
        {
            var pts = new PointCollection();
            for (int i = 0; i <= 48; i++) pts.Add(At(a + (b - a) * i / 48.0, w));
            return pts;
        }
        double speed = moving ? (via == "bluetooth" ? 0.35 : via == "wifi" ? 0.7 : 0.5) : 0.25;
        // The tree is rebuilt with every progress step (ten a second): the dots
        // carry on from where the last build left them instead of starting over.
        string key = JStr(n, "id") + "/" + (sending ? "send" : "receive");
        if (!live) LinkPhases.Remove(key);
        double Phase()
        {
            double now = LinkClock.Elapsed.TotalSeconds;
            (double phase, double at) = LinkPhases.TryGetValue(key, out var kept) ? kept : (0, now);
            phase = (phase + (now - at) * speed) % 1.0;
            LinkPhases[key] = (phase, now);
            return phase;
        }
        void PlaceDots(double w)
        {
            double t = live && !reduce ? Phase() : 0;
            for (int i = 0; i < dots.Count; i++)
            {
                double s = reduce ? (i + 1.0) / (dots.Count + 1) : (t + (double)i / dots.Count) % 1.0;
                if (!sending) s = 1 - s;
                Windows.Foundation.Point q = At(s, w);
                Microsoft.UI.Xaml.Controls.Canvas.SetLeft(dots[i], q.X - r);
                Microsoft.UI.Xaml.Controls.Canvas.SetTop(dots[i], q.Y - r);
                dots[i].Opacity = (moving ? 1.0 : 0.55) * Math.Min(1, Math.Min(s, 1 - s) * 6);
            }
        }
        void Layout()
        {
            double w = canvas.ActualWidth;
            if (w <= 12) return;
            track.Points = Line(0, 1, w);
            double done = state == "done" ? 1 : Math.Max(0, value);
            moved.Points = done > 0 && !failed ? (sending ? Line(0, done, w) : Line(1 - done, 1, w)) : new PointCollection();
            double[] ss = { 0.22, 0.36, 0.64, 0.8 }, dy = { -14, 12, -13, 11 };
            for (int i = 0; i < nodes.Count; i++)
            {
                Windows.Foundation.Point q = At(ss[i], w);
                Microsoft.UI.Xaml.Controls.Canvas.SetLeft(nodes[i], q.X - 2.5);
                Microsoft.UI.Xaml.Controls.Canvas.SetTop(nodes[i], q.Y + dy[i] - 2.5);
            }
            Windows.Foundation.Point mid = At(0.5, w);
            Microsoft.UI.Xaml.Controls.Canvas.SetLeft(badge, mid.X - 17);
            Microsoft.UI.Xaml.Controls.Canvas.SetTop(badge, mid.Y - 17);
            if (through != null)
            {
                through.Measure(new Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
                Microsoft.UI.Xaml.Controls.Canvas.SetLeft(through, mid.X - through.DesiredSize.Width / 2);
                Microsoft.UI.Xaml.Controls.Canvas.SetTop(through, mid.Y + 19);
            }
            PlaceDots(w);
        }
        canvas.SizeChanged += (_, _) => Layout();
        if (live && !reduce)
        {
            // On the window's frames, not a free-running 33 ms timer that beats
            // against the refresh (uneven steps): at most 30 fps, every other
            // frame at 60 Hz.
            double lastStep = -1;
            void Step(object? sender, object e)
            {
                double now = LinkClock.Elapsed.TotalSeconds;
                if (now - lastStep < 0.030) return;
                lastStep = now;
                PlaceDots(canvas.ActualWidth);
            }
            bool stepping = false;
            canvas.Loaded += (_, _) =>
            {
                if (stepping) return;
                stepping = true;
                Microsoft.UI.Xaml.Media.CompositionTarget.Rendering += Step;
            };
            canvas.Unloaded += (_, _) =>
            {
                if (!stepping) return;
                stepping = false;
                Microsoft.UI.Xaml.Media.CompositionTarget.Rendering -= Step;
            };
        }
        root.Children.Add(strip);

        var captionRow = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(6), HorizontalAlignment = HorizontalAlignment.Center };
        captionRow.Children.Add(SText(JStr(n, "caption"), Body, UiFontSize - 2));
        if (value >= 0) captionRow.Children.Add(SText($"{Math.Round(value * 100)}%", Title, UiFontSize - 2));
        root.Children.Add(captionRow);

        AutomationProperties.SetName(root, JStr(n, "label"));
        AutomationProperties.SetHelpText(root, value >= 0 ? $"{Math.Round(value * 100)} percent" : state switch
        {
            "moving" => "Moving",
            "paused" => "Paused",
            "done" => "Done",
            "failed" => "Stopped",
            _ => "Connecting",
        });
        return root;
    }

    // ---- #442 additions: card, ring, tiles, code entry ------------------------------

    private static Border ScreenCard(UIElement content) => new()
    {
        Child = content,
        Padding = Pad(14),
        Background = Brush(ChromeColour.Surface),
        BorderBrush = Brush(Hairline),
        BorderThickness = new Thickness(1),
        CornerRadius = Corner(10),
        HorizontalAlignment = HorizontalAlignment.Stretch,
    };

    /// <summary>An arc from 12 o'clock, clockwise, over <paramref name="fraction"/> of a
    /// circle of <paramref name="size"/> drawn with a <paramref name="line"/> stroke.</summary>
    private static Geometry RingArc(double size, double line, double fraction)
    {
        double r = (size - line) / 2, c = size / 2;
        if (fraction >= 0.999)
        {
            return new EllipseGeometry { Center = new Windows.Foundation.Point(c, c), RadiusX = r, RadiusY = r };
        }
        double angle = Math.Max(0.02, fraction) * 2 * Math.PI;
        var figure = new PathFigure { StartPoint = new Windows.Foundation.Point(c, c - r), IsClosed = false, IsFilled = false };
        figure.Segments.Add(new ArcSegment
        {
            Point = new Windows.Foundation.Point(c + r * Math.Sin(angle), c - r * Math.Cos(angle)),
            Size = new Windows.Foundation.Size(r, r),
            IsLargeArc = fraction > 0.5,
            SweepDirection = SweepDirection.Clockwise,
        });
        var g = new PathGeometry();
        g.Figures.Add(figure);
        return g;
    }

    // "look": "ring": a circle that fills, its percentage in the middle; an
    // unknown value is a still third (nothing ticks while nothing moves).
    private static FrameworkElement ScreenRing(double value, double size)
    {
        double line = Math.Max(3, size / 14);
        var root = new Grid { Width = size, Height = size, HorizontalAlignment = HorizontalAlignment.Center };
        root.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse
        {
            Width = size - line,
            Height = size - line,
            Margin = new Thickness(line / 2),
            Stroke = Brush(Hairline),
            StrokeThickness = line,
        });
        root.Children.Add(new Microsoft.UI.Xaml.Shapes.Path
        {
            Data = RingArc(size, line, value < 0 ? 0.33 : Math.Clamp(value, 0, 1)),
            Stroke = Brush(ChromeColour.TrimAccent),
            StrokeThickness = line,
            StrokeStartLineCap = PenLineCap.Round,
            StrokeEndLineCap = PenLineCap.Round,
        });
        if (value >= 0 && size >= 40)
        {
            TextBlock pct = SText($"{Math.Round(value * 100)}%", Title, size / 4.5, bold: true);
            pct.HorizontalAlignment = HorizontalAlignment.Center;
            pct.VerticalAlignment = VerticalAlignment.Center;
            root.Children.Add(pct);
        }
        AutomationProperties.SetHelpText(root, value < 0 ? "In progress" : $"{Math.Round(value * 100)} percent");
        return root;
    }

    // Large round tiles (#442): a monogram in a circle, the name and a caption
    // under it; a ring round the circle while "progress" >= 0; a done / failed
    // mark. A click, Space or Enter on one is `activate` with its id; a
    // "static" set is shown, not chosen (no focus, no event). One row that
    // scrolls sideways when the computers do not fit.
    private static UIElement BuildTiles(JsonElement n, ScreenAt at, string id)
    {
        double size = JNum(n, "size", 72);
        bool still = JFlag(n, "static");
        HashSet<string> selected = SelectedSet(n);
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(12) };
        foreach (JsonElement item in JArr(n, "items"))
        {
            string itemId = JStr(item, "id");
            string name = JStr(item, "text"), detail = JStr(item, "detail");
            double progress = JNum(item, "progress", -1);
            string mark = JStr(item, "mark");
            var face = new Grid { Width = size + 14, Height = size + 14 };
            face.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse
            {
                Width = size,
                Height = size,
                Fill = Brush(ChromeColour.TrimAccent),
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
            });
            TextBlock mono = SText(JStr(item, "initials") is { Length: > 0 } ini ? ini : "?", Title, size * 0.36, bold: true);
            mono.Foreground = new SolidColorBrush(Colors.White);
            mono.HorizontalAlignment = HorizontalAlignment.Center;
            mono.VerticalAlignment = VerticalAlignment.Center;
            face.Children.Add(mono);
            if (progress >= 0)
            {
                face.Children.Add(new Microsoft.UI.Xaml.Shapes.Path
                {
                    Data = RingArc(size + 14, 4, Math.Clamp(progress, 0, 1)),
                    Stroke = Brush(ChromeColour.TrimAccent),
                    StrokeThickness = 4,
                    StrokeStartLineCap = PenLineCap.Round,
                    StrokeEndLineCap = PenLineCap.Round,
                });
            }
            else if (selected.Contains(itemId))
            {
                face.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse { Stroke = Brush(ChromeColour.TrimAccent), StrokeThickness = 2 });
            }
            if (mark.Length > 0)
            {
                var badge = new Border
                {
                    Width = size * 0.32,
                    Height = size * 0.32,
                    CornerRadius = new CornerRadius(size * 0.16),
                    Background = new SolidColorBrush(mark == "done" ? Windows.UI.Color.FromArgb(255, 52, 168, 83)
                                                                    : Windows.UI.Color.FromArgb(255, 230, 126, 34)),
                    HorizontalAlignment = HorizontalAlignment.Right,
                    VerticalAlignment = VerticalAlignment.Bottom,
                    Child = new FontIcon
                    {
                        Glyph = mark == "done" ? "\uE73E" : "\uE7BA",
                        FontFamily = SymbolFont,
                        FontSize = size * 0.18,
                        Foreground = new SolidColorBrush(Colors.White),
                    },
                };
                face.Children.Add(badge);
            }
            var tile = new StackPanel { Spacing = Space(4), Width = size + 40 };
            tile.Children.Add(face);
            TextBlock title = SText(name, Title, UiFontSize - 2, maxLines: 2);
            title.TextAlignment = TextAlignment.Center;
            title.HorizontalAlignment = HorizontalAlignment.Center;
            tile.Children.Add(title);
            if (detail.Length > 0)
            {
                TextBlock caption = SText(detail, Body, UiFontSize - 4, maxLines: 1);
                caption.HorizontalAlignment = HorizontalAlignment.Center;
                tile.Children.Add(caption);
            }
            string spoken = detail.Length > 0 ? $"{name}, {detail}" : name;
            if (still)
            {
                AutomationProperties.SetName(tile, spoken);
                row.Children.Add(tile);
                continue;
            }
            Button b = EditButton("", () => SendScreenEvent(at, id, SlotNative.EventActivate, JsonString(itemId)));
            b.Content = tile;
            b.Padding = Pad(4);
            // Top-aligned, as the Mac's grid (`alignment: .top`): a name that wraps to a
            // second line must not push its circle below its neighbours'.
            b.VerticalAlignment = VerticalAlignment.Top;
            b.VerticalContentAlignment = VerticalAlignment.Top;
            b.AllowFocusOnInteraction = true;
            b.IsTabStop = true;
            AutomationProperties.SetName(b, spoken);
            row.Children.Add(b);
        }
        UIElement body = row;
        if (!still)
        {
            body = new ScrollViewer
            {
                Content = row,
                HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
                VerticalScrollBarVisibility = ScrollBarVisibility.Disabled,
                HorizontalScrollMode = ScrollMode.Auto,
                VerticalScrollMode = ScrollMode.Disabled,
            };
        }
        else
        {
            row.HorizontalAlignment = HorizontalAlignment.Center;
        }
        return body;
    }

    private static readonly MediaViewer.Shared.FakeInputLook CodeInputLook = new()
    {
        Font = () => new FontFamily("Segoe UI Variable Display,Segoe UI"),
        FontSize = () => UiFontSize + 14,
        Title = () => Brush(Title),
        Body = () => Brush(Body),
        Canvas = () => Brush(Canvas),
        Hairline = () => Brush(Hairline),
        Selection = () => Brush(TextSelection),
        SelectionInk = () => Brush(TextSelectionInk),
        Accent = () => Brush(ChromeColour.Accent),
    };

    // "look": "code" (#442): large, digits only (grouped "123 456" as typed); it
    // submits by itself once "digits" are typed (Enter submits too) and clears for
    // a retry. FakeInput, as every field here: a WinUI TextBox fail-fasts.
    private static UIElement BuildCodeField(JsonElement n, ScreenAt at, string id)
    {
        int digits = (int)JNum(n, "digits", 6);
        var field = new MediaViewer.Shared.FakeInput(CodeInputLook,
            JStr(n, "placeholder").Length > 0 ? JStr(n, "placeholder") : JStr(n, "label"), 240)
        {
            HorizontalAlignment = HorizontalAlignment.Center,
        };
        bool busy = false;
        field.Changed += () =>
        {
            if (busy) return;
            string only = new string(field.Text.Where(char.IsAsciiDigit).Take(digits).ToArray());
            string shown = only.Length > 3 ? only[..3] + " " + only[3..] : only;
            busy = true;
            try
            {
                if (only.Length == digits)
                {
                    SendScreenEvent(at, id, SlotNative.EventSubmit, JsonString(only));
                    field.SetText("");
                }
                else if (shown != field.Text)
                {
                    field.SetText(shown);
                }
            }
            finally { busy = false; }
        };
        field.Submitted += () =>
        {
            string only = new string(field.Text.Where(char.IsAsciiDigit).ToArray());
            if (only.Length > 0) SendScreenEvent(at, id, SlotNative.EventSubmit, JsonString(only));
        };
        field.Loaded += (_, _) => field.Focus(FocusState.Programmatic);
        return field;
    }

    private static string ScreenNumberJson(double v) =>
        v == Math.Round(v) && Math.Abs(v) < 1e15
            ? ((long)v).ToString(System.Globalization.CultureInfo.InvariantCulture)
            : v.ToString("R", System.Globalization.CultureInfo.InvariantCulture);

    private static UIElement BuildNumber(JsonElement n, ScreenAt at, string id)
    {
        double lo = JNum(n, "min", -1e12), hi = JNum(n, "max", 1e12), step = JNum(n, "step", 1);
        double value = JNum(n, "value", Math.Max(lo, 0));
        string label = JStr(n, "label");
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(6) };
        TextBlock shown = SText($"{label}: {ScreenNumberJson(value)}", Title, UiFontSize - 2);
        shown.VerticalAlignment = VerticalAlignment.Center;
        void Step(double by)
        {
            value = Math.Clamp(value + by, lo, hi);
            shown.Text = $"{label}: {ScreenNumberJson(value)}";
            SendScreenEvent(at, id, SlotNative.EventChange, ScreenNumberJson(value));
        }
        Button minus = ScreenButton("−", () => Step(-step));
        Button plus = ScreenButton("+", () => Step(step));
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
        bool multi = JFlag(n, "multi");
        var rows = new StackPanel { Spacing = 0 };
        var buttons = new List<Button>();
        JsonElement[] columns = table ? JArr(n, "columns").ToArray() : Array.Empty<JsonElement>();
        if (table)
        {
            var header = new Grid { Padding = Pad(8, 4, 8, 4) };
            for (int c = 0; c < columns.Length; ++c)
            {
                header.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                TextBlock h = SText(JStr(columns[c], "title"), Body, UiFontSize - 4, bold: true);
                Grid.SetColumn(h, c);
                header.Children.Add(h);
            }
            rows.Children.Add(header);
            rows.Children.Add(new Border { Height = 1, Background = Brush(Hairline) });
        }
        foreach (JsonElement r in JArr(n, table ? "rows" : "items"))
        {
            string rowId = JStr(r, "id");
            UIElement content;
            string name;
            if (table)
            {
                var g = new Grid();
                string[] cells = JArr(r, "cells").Select(c => c.GetString() ?? "").ToArray();
                for (int c = 0; c < columns.Length; ++c)
                {
                    g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                    TextBlock cell = SText(c < cells.Length ? cells[c] : "", Title, UiFontSize - 3, maxLines: 1);
                    Grid.SetColumn(cell, c);
                    g.Children.Add(cell);
                }
                content = g;
                name = string.Join(", ", cells);
            }
            else
            {
                var col = new StackPanel { Spacing = Space(1) };
                col.Children.Add(SText(JStr(r, "text"), Title, UiFontSize - 2));
                string detail = JStr(r, "detail");
                if (detail.Length > 0) col.Children.Add(SText(detail, Body, UiFontSize - 4));
                content = col;
                name = detail.Length > 0 ? $"{JStr(r, "text")}, {detail}" : JStr(r, "text");
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
    // UI thread: paths whose job is in flight, with the other tiles waiting on it.
    private static readonly Dictionary<string, List<(Image Img, double Size)>> SlotThumbPending = new(StringComparer.Ordinal);

    private static FrameworkElement SlotThumb(string path, double size, string label)
    {
        var img = new Image { Width = size, Height = size, Stretch = Stretch.Uniform };
        var box = new Border
        {
            Width = size,
            Height = size,
            CornerRadius = Corner(6),
            // #508: on a card (itself the surface colour) an empty or loading
            // tile still shows as a tile.
            Background = Brush(InScreenWindow ? ChromeColour.Placeholder : ChromeColour.Surface),
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
        // One job per path in flight: a tile realised again (or the same file in two
        // tiles) waits for that job rather than queueing another.
        if (SlotThumbPending.TryGetValue(path, out var waiting))
        {
            waiting.Add((img, size));
            return;
        }
        SlotThumbPending[path] = new List<(Image, double)>();
        _ = Task.Run(async () =>
        {
            await SlotThumbGate.WaitAsync().ConfigureAwait(false);
            string? jpeg = null;
            try
            {
                jpeg = SlotNative.ThumbnailPath(path);
            }
            catch (Exception ex)
            {
                // A clip (no poster here) or a file that cannot be read: an empty
                // tile, and the path leaves the pending list (#508: an escaped
                // throw left it there, so no later tile of it ever painted).
                System.Diagnostics.Debug.WriteLine(ex.GetType().Name);
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
                if (SlotThumbPending.Remove(path, out var others))
                {
                    foreach ((Image other, double otherSize) in others)
                    {
                        if (other.Tag is string op && op == path) SetSlotThumb(other, jpeg, otherSize);
                    }
                }
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
        double tile = JNum(n, "tile", 128);
        bool multi = JFlag(n, "multi");
        HashSet<string> selected = SelectedSet(n);
        var vms = new List<SlotTileVm>();
        foreach (JsonElement item in JArr(n, "items"))
        {
            string path = JStr(item, "path");
            string label = JStr(item, "label");
            vms.Add(new SlotTileVm(path, label.Length > 0 ? label : Path.GetFileName(path), vms.Count));
        }
        var realised = new Dictionary<int, Button>();
        ScrollViewer? scroller = null;
        var repeater = new ItemsRepeater
        {
            Layout = new UniformGridLayout { MinItemWidth = tile, MinItemHeight = tile, MinColumnSpacing = Space(8), MinRowSpacing = Space(8) },
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
            // Bounded, or a vertical stack / section / scroll measures it at infinite
            // height and the repeater realises (and asks a thumbnail for) every tile.
            // Never taller than the window it is in; a few rows until that is known.
            MaxHeight = Math.Max(tile + 16, 4 * (tile + 8)),
        };
        ScrollViewer bounded = scroller;
        void FitWindow()
        {
            if (bounded.XamlRoot is { } root && root.Size.Height > 0)
            {
                bounded.MaxHeight = Math.Max(tile + 16, root.Size.Height);
            }
        }
        XamlRoot? watched = null;
        void RootChanged(XamlRoot sender, XamlRootChangedEventArgs e) => FitWindow();
        bounded.Loaded += (_, _) =>
        {
            FitWindow();
            if (watched is null && bounded.XamlRoot is { } root)
            {
                watched = root;
                root.Changed += RootChanged;
            }
        };
        bounded.Unloaded += (_, _) =>
        {
            if (watched is not null) watched.Changed -= RootChanged;
            watched = null;
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

    // The add-on's pixels with the chrome's pin and names over them, the pin in
    // the accent colour (docs/plans/metadata-search-and-place.md slice 3). One
    // element for Narrator, named by its label.
    //
    // An "interactive" surface (owner, 2026-10-12: zoom the map with the trackpad
    // or the mouse): a mouse wheel, or a precision touchpad pinch (which arrives
    // as Ctrl + wheel), zooms about the pointer; a touchpad two-finger scroll
    // without Ctrl is left to the pane's ScrollViewer. A drag pans, a double-click
    // is reported. While a gesture runs the current pixels are scaled / moved at
    // once (a RenderTransform, no present), and each gesture is reported to the
    // add-on's worker once it settles; the add-on's next pixels rebuild the node.
    private static UIElement BuildSurface(JsonElement n, ScreenAt at)
    {
        double height = JNum(n, "height", 120);
        double width = JNum(n, "width", 0);
        string id = JStr(n, "id");
        var frame = new Grid
        {
            Height = height,
            CornerRadius = Corner(6),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(1),
            Background = Brush(ChromeColour.Surface),
        };
        if (width > 0) frame.Width = width;
        var content = new Grid();
        var transform = new CompositeTransform();
        content.RenderTransform = transform;
        frame.Children.Add(content);
        frame.SizeChanged += (_, _) =>
            frame.Clip = new RectangleGeometry { Rect = new Windows.Foundation.Rect(0, 0, frame.ActualWidth, frame.ActualHeight) };

        string surfaceId = JStr(n, "surface");
        var image = new Image { Stretch = Stretch.UniformToFill };
        content.Children.Add(image);
        // Registered so new pixels swap into this Image (RefreshSurfaces) without
        // rebuilding the node: a pinch or drag in progress keeps its transform.
        var liveEntry = new LiveSurface(image) { Revision = SetSurfacePixels(image, at, surfaceId) };
        LiveSurfaces[(at, surfaceId)] = liveEntry;

        (double X, double Y)? pinAt = null;
        if (n.TryGetProperty("pin", out JsonElement pin) && pin.ValueKind == JsonValueKind.Object)
        {
            pinAt = (JNum(pin, "x", 0.5), JNum(pin, "y", 0.5));
        }
        // The add-on's names, drawn here so the text is native (priority order,
        // skipping any that would overlap one already drawn or the pin).
        var marks = new Microsoft.UI.Xaml.Controls.Canvas { IsHitTestVisible = false };
        content.Children.Add(marks);
        var glyph = new FontIcon
        {
            Glyph = "",  // MapPin
            FontFamily = SymbolFont,
            FontSize = 22,
            Foreground = Brush(TrimAccent),
            HorizontalAlignment = HorizontalAlignment.Left,
            VerticalAlignment = VerticalAlignment.Top,
            IsHitTestVisible = false,
            Visibility = pinAt is null ? Visibility.Collapsed : Visibility.Visible,
        };
        content.Children.Add(glyph);
        JsonElement[] markRows = JArr(n, "marks").ToArray();
        frame.SizeChanged += (_, _) =>
        {
            double w = frame.ActualWidth, h = frame.ActualHeight;
            if (pinAt is { } pa) glyph.Margin = new Thickness(w * pa.X - 11, h * pa.Y - 22, 0, 0);
            marks.Children.Clear();
            var taken = new List<Windows.Foundation.Rect>();
            if (pinAt is { } pp) taken.Add(new Windows.Foundation.Rect(w * pp.X - 12, h * pp.Y - 24, 24, 26));
            foreach (JsonElement m in markRows)
            {
                string text = JStr(m, "text");
                if (text.Length == 0) continue;
                var r = new Windows.Foundation.Rect(JNum(m, "x", 0) * w + 6, JNum(m, "y", 0) * h - 8, text.Length * 6.6 + 4, 16);
                if (r.Right > w + 1 || r.Top < -1 || r.Bottom > h + 1) continue;
                if (taken.Any(t => RectsOverlap(t, r))) continue;
                taken.Add(r);
                var label = new TextBlock
                {
                    Text = text,
                    FontFamily = UiFont,
                    FontSize = 11,
                    Foreground = Brush(Body),
                };
                Microsoft.UI.Xaml.Controls.Canvas.SetLeft(label, r.X);
                Microsoft.UI.Xaml.Controls.Canvas.SetTop(label, r.Y);
                marks.Children.Add(label);
            }
        };

        bool live = !JFlag(n, "disabled") && n.TryGetProperty("interactive", out JsonElement inter) &&
                    (inter.ValueKind == JsonValueKind.True || inter.ValueKind == JsonValueKind.Object);
        if (!live) return frame;
        double lo = 0.05, hi = 20;
        if (n.TryGetProperty("interactive", out JsonElement b) && b.ValueKind == JsonValueKind.Object)
        {
            lo = JNum(b, "min_scale", lo);
            hi = Math.Max(lo, JNum(b, "max_scale", hi));
        }
        double committed = 1;
        (double X, double Y) wheelAnchor = (0.5, 0.5);
        var settle = _dispatcher?.DispatcherQueue.CreateTimer();
        if (settle is not null)
        {
            settle.Interval = TimeSpan.FromMilliseconds(150);
            settle.IsRepeating = false;
            settle.Tick += (_, _) =>
            {
                double step = transform.ScaleX / committed;
                committed = transform.ScaleX;
                SendScreenEvent(at, id, 10, JsonSerializer.Serialize(new
                {
                    scale = Math.Clamp(step, 0.05, 20),
                    x = Math.Clamp(wheelAnchor.X, 0, 1),
                    y = Math.Clamp(wheelAnchor.Y, 0, 1),
                }));
            };
        }
        frame.PointerWheelChanged += (_, e) =>
        {
            bool ctrl = (e.KeyModifiers & Windows.System.VirtualKeyModifiers.Control) != 0;
            // A touchpad's two-finger scroll is the pane's; its pinch arrives with Ctrl.
            if (e.Pointer.PointerDeviceType == Microsoft.UI.Input.PointerDeviceType.Touchpad && !ctrl) return;
            var point = e.GetCurrentPoint(frame);
            double w = Math.Max(1, frame.ActualWidth), h = Math.Max(1, frame.ActualHeight);
            wheelAnchor = (point.Position.X / w, point.Position.Y / h);
            double factor = Math.Pow(1.1, point.Properties.MouseWheelDelta / 120.0);
            double next = Math.Clamp(transform.ScaleX * factor, lo, hi);
            transform.CenterX = point.Position.X;
            transform.CenterY = point.Position.Y;
            transform.ScaleX = next;
            transform.ScaleY = next;
            settle?.Stop();
            settle?.Start();
            e.Handled = true;
        };
        Windows.Foundation.Point? dragFrom = null;
        double baseX = 0, baseY = 0;
        frame.PointerPressed += (_, e) =>
        {
            var point = e.GetCurrentPoint(frame);
            if (!point.Properties.IsLeftButtonPressed) return;
            dragFrom = point.Position;
            baseX = transform.TranslateX;
            baseY = transform.TranslateY;
            frame.CapturePointer(e.Pointer);
        };
        frame.PointerMoved += (_, e) =>
        {
            if (dragFrom is not { } from) return;
            var now = e.GetCurrentPoint(frame).Position;
            transform.TranslateX = baseX + now.X - from.X;
            transform.TranslateY = baseY + now.Y - from.Y;
        };
        frame.PointerReleased += (_, e) =>
        {
            if (dragFrom is not { } from) return;
            dragFrom = null;
            frame.ReleasePointerCapture(e.Pointer);
            var now = e.GetCurrentPoint(frame).Position;
            double w = Math.Max(1, frame.ActualWidth), h = Math.Max(1, frame.ActualHeight);
            double dx = (now.X - from.X) / w, dy = (now.Y - from.Y) / h;
            if (Math.Abs(dx) < 0.005 && Math.Abs(dy) < 0.005) return;  // a click, not a drag
            SendScreenEvent(at, id, 11, JsonSerializer.Serialize(new
            {
                dx = Math.Clamp(dx, -10, 10),
                dy = Math.Clamp(dy, -10, 10),
            }));
        };
        frame.DoubleTapped += (_, e) =>
        {
            var pos = e.GetPosition(frame);
            double w = Math.Max(1, frame.ActualWidth), h = Math.Max(1, frame.ActualHeight);
            SendScreenEvent(at, id, 12, JsonSerializer.Serialize(new
            {
                x = Math.Clamp(pos.X / w, 0, 1),
                y = Math.Clamp(pos.Y / h, 0, 1),
            }));
        };
        // The add-on's new pixels already show what was reported: keep only the part of
        // the gesture not reported yet (a pinch still settling, a drag still held).
        liveEntry.AfterSwap = () =>
        {
            double rest = transform.ScaleX / committed;
            committed = 1;
            transform.ScaleX = rest;
            transform.ScaleY = rest;
            if (dragFrom is null)
            {
                transform.TranslateX = 0;
                transform.TranslateY = 0;
            }
        };
        return frame;
    }

    private sealed class LiveSurface(Image image)
    {
        public Image Image { get; } = image;
        public ulong Revision { get; set; }
        public Action? AfterSwap { get; set; }
    }

    // UI thread: every surface element on screen, by (screen or section, surface id).
    private static readonly Dictionary<(ScreenAt At, string Surface), LiveSurface> LiveSurfaces = new();

    // Fills `image` with the surface's current pixels; its revision (0: none yet).
    private static ulong SetSurfacePixels(Image image, ScreenAt at, string surface)
    {
        var px = SlotNative.Surface(at.Addon, at.Screen, surface);
        if (px is not { } p || p.Width <= 0 || p.Height <= 0) return 0;
        try
        {
            var bmp = new WriteableBitmap(p.Width, p.Height);
            using (Stream s = System.Runtime.InteropServices.WindowsRuntime.WindowsRuntimeBufferExtensions.AsStream(bmp.PixelBuffer))
            {
                s.Write(p.Pixels, 0, Math.Min(p.Pixels.Length, p.Width * p.Height * 4));
            }
            bmp.Invalidate();
            image.Source = bmp;
            return p.Revision;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex.Message);
            return 0;
        }
    }

    // Only pixels moved: swap them into the surfaces of `at` whose revision changed
    // (a copy-free revision check first), nothing rebuilt.
    private static void RefreshSurfaces(ScreenAt at)
    {
        foreach (var entry in LiveSurfaces.Where(e => e.Key.At == at).ToList())
        {
            ulong rev = SlotNative.SurfaceRevision(at.Addon, at.Screen, entry.Key.Surface);
            if (rev == 0 || rev == entry.Value.Revision) continue;
            ulong shown = SetSurfacePixels(entry.Value.Image, at, entry.Key.Surface);
            if (shown == 0) continue;
            entry.Value.Revision = shown;
            entry.Value.AfterSwap?.Invoke();
        }
    }

    // A screen or section rebuilt or gone: its old elements are not refreshed any more.
    private static void ForgetSurfaces(ScreenAt at)
    {
        foreach (var key in LiveSurfaces.Keys.Where(k => k.At == at).ToList()) LiveSurfaces.Remove(key);
    }

    private static bool RectsOverlap(Windows.Foundation.Rect a, Windows.Foundation.Rect b) =>
        a.X < b.X + b.Width && b.X < a.X + a.Width && a.Y < b.Y + b.Height && b.Y < a.Y + a.Height;

    // ---- windows, docked panes, sheets ---------------------------------------------

    private static readonly Dictionary<ScreenAt, (Window Window, long Revision)> ScreenWindows = new();
    private static readonly HashSet<ScreenAt> ClosingFromCore = new();

    [StructLayout(LayoutKind.Sequential)]
    private struct SlotRect { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll", EntryPoint = "GetWindowRect")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SlotGetWindowRect(IntPtr hwnd, out SlotRect rect);

    // #511: screens with a pointer down in them. A window rebuilt between a
    // press and its release loses the click (a transfer's progress moves the
    // tree ten times a second, and Cancel did nothing); while a pointer is down
    // the tree waits, and the release brings it up to date.
    private static readonly HashSet<ScreenAt> ScreenPressed = new();

    // UI thread: the open screens as the core lists them; a window per screen,
    // built from its tree and rebuilt when the tree's revision moves.
    private static void SyncScreenWindows(JsonElement list)
    {
        var wanted = new HashSet<ScreenAt>();
        SyncSettingsScreen(list);  // a settings page is drawn in Settings, never a window
        foreach (JsonElement s in list.EnumerateArray())
        {
            string place = JStr(s, "place");
            if (place == "settings") continue;
            var at = new ScreenAt(JStr(s, "addon"), JStr(s, "id"));
            wanted.Add(at);
            long revision = (long)JNum(s, "revision", 0);
            if (!ScreenWindows.TryGetValue(at, out var open))
            {
                Window w = OpenScreenWindow(s, at, place);
                ScreenWindows[at] = (w, revision);
                FrameworkElement content = BuildScreenRoot(s, at, place);
                w.Content = content;
                if (place == "window") FitScreenWindow(w, content, (int)JNum(s, "height", 0));
            }
            else if (open.Revision != revision && !ScreenPressed.Contains(at))
            {
                // #508: the new tree keeps the old one's scroll and focus.
                ScreenPlace? kept = NoteScreenPlace(open.Window.Content);
                FrameworkElement content = BuildScreenRoot(s, at, place);
                if (kept is { } k) RestoreScreenPlace(content, k);
                open.Window.Content = content;
                ScreenWindows[at] = (open.Window, revision);
            }
            else
            {
                // Same tree: only pixels can have moved (a post_surface moves the
                // list's "pixels", never the tree's revision). Swap them in place.
                RefreshSurfaces(at);
            }
        }
        foreach (ScreenAt gone in ScreenWindows.Keys.Where(k => !wanted.Contains(k)).ToList())
        {
            ScreenPressed.Remove(gone);
            ForgetSurfaces(gone);
            ForgetReveals(gone);
            ClosingFromCore.Add(gone);
            try { ScreenWindows[gone].Window.Close(); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
            ScreenWindows.Remove(gone);
            ClosingFromCore.Remove(gone);
        }
    }

    // UI thread, at XAML shutdown: every screen window closes while its runtime is
    // still alive. Closing from here is not the person closing it, so the core is
    // not told (the add-ons are stopping too).
    private static void CloseScreenWindowsForShutdown()
    {
        foreach (ScreenAt at in ScreenWindows.Keys.ToList())
        {
            ClosingFromCore.Add(at);
            try { ScreenWindows[at].Window.Close(); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
            ForgetSurfaces(at);
            ForgetReveals(at);
        }
        ScreenWindows.Clear();
        ScreenPressed.Clear();
        ClosingFromCore.Clear();
        LiveSurfaces.Clear();
    }

    private static FrameworkElement BuildScreenRoot(JsonElement s, ScreenAt at, string place)
    {
        // #508: the window's face and the ids of what is built, for this build only.
        var ids = new Dictionary<string, FrameworkElement>(StringComparer.Ordinal);
        var reveals = new HashSet<string>(StringComparer.Ordinal);
        _screenFace = ScreenWindowFace();
        _screenIds = ids;
        _revealBuilt = reveals;
        try
        {
            return BuildScreenRootInner(s, at, place, ids);
        }
        finally
        {
            _screenFace = null;
            _screenIds = null;
            _revealBuilt = null;
            PruneReveals(at, reveals);
        }
    }

    private static FrameworkElement BuildScreenRootInner(JsonElement s, ScreenAt at, string place,
                                                         Dictionary<string, FrameworkElement> ids)
    {
        var root = new Grid { Background = Brush(Canvas), RequestedTheme = IslandTheme, Padding = Pad(16, 12, 16, 16) };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        if (place != "window")
        {
            var head = new Grid { Margin = Pad(0, 0, 0, 8) };
            head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            TextBlock title = SText(JStr(s, "title"), Title, TypeSize(17), bold: true, display: true);
            title.VerticalAlignment = VerticalAlignment.Center;
            head.Children.Add(title);
            Button done = ScreenButton("Done", () => SlotNative.CloseScreen(at.Addon, at.Screen));
            Grid.SetColumn(done, 1);
            head.Children.Add(done);
            root.Children.Add(head);
        }
        if (s.TryGetProperty("tree", out JsonElement tree) && tree.ValueKind == JsonValueKind.Object)
        {
            var body = (FrameworkElement)BuildScreenNode(tree, at);
            Grid.SetRow(body, 1);
            root.Children.Add(body);
            if (body is ScrollViewer { Content: FrameworkElement inner } scroll &&
                scroll.VerticalScrollBarVisibility != ScrollBarVisibility.Disabled)
            {
                // #508: a scrolling tree's margin is inside the scroller, so its bar
                // sits on the window's edge, not over the cards, and shows only
                // when there is more to see.
                root.Padding = new Thickness(0, place != "window" ? Space(12) : 0, 0, 0);
                if (place != "window") ((FrameworkElement)root.Children[0]).Margin = Pad(16, 0, 16, 8);
                inner.Margin = Pad(16, place != "window" ? 0 : 14, 16, 18);
            }
        }
        ScreenIdsOf.AddOrUpdate(root, ids);
        // #511: no rebuild between a press and its release (ScreenPressed).
        // Handled events too: a Button marks its own press handled. The
        // release's catch-up runs after the click, from the queue.
        root.AddHandler(UIElement.PointerPressedEvent, new PointerEventHandler((_, _) => ScreenPressed.Add(at)), true);
        var released = new PointerEventHandler((_, _) =>
        {
            if (ScreenPressed.Remove(at)) DispatcherQueueControllerTryEnqueue(ReadSlotScreens);
        });
        root.AddHandler(UIElement.PointerReleasedEvent, released, true);
        root.AddHandler(UIElement.PointerCanceledEvent, released, true);
        root.AddHandler(UIElement.PointerCaptureLostEvent, released, true);
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

    // #410: a screen's folder field. The dialog belongs to the screen's own
    // window (else the viewer's); the folder goes to the add-on as the field's
    // change, as if typed.
    private static async void PickScreenFolder(ScreenAt at, string id, FakeInput field, Action<string> remember)
    {
        try
        {
            var picker = new Windows.Storage.Pickers.FolderPicker();
            picker.FileTypeFilter.Add("*");
            IntPtr owner = ScreenWindows.TryGetValue(at, out var open)
                ? WinRT.Interop.WindowNative.GetWindowHandle(open.Window)
                : _themeWindow;
            WinRT.Interop.InitializeWithWindow.Initialize(picker, owner);
            Windows.Storage.StorageFolder? folder = await picker.PickSingleFolderAsync();
            if (folder is null) return;
            field.Text = folder.Path;
            remember(folder.Path);
            SendScreenEvent(at, id, SlotNative.EventChange, JsonString(folder.Path));
        }
        catch (Exception ex)
        {
            // Never let an exception out of a XAML event: that is a fail-fast.
            System.Diagnostics.Debug.WriteLine(ex);
        }
    }

    [DllImport("user32.dll", EntryPoint = "GetDpiForWindow")]
    private static extern uint SlotGetDpiForWindow(IntPtr hwnd);

    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")]
    private static extern IntPtr SlotSetWindowLongPtr(IntPtr hwnd, int index, IntPtr value);

    [DllImport("user32.dll", EntryPoint = "SetPropW", CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SlotSetProp(IntPtr hwnd, string name, IntPtr data);

    private const int SlotGwlpHwndParent = -8;

    [DllImport("dwmapi.dll", EntryPoint = "DwmSetWindowAttribute")]
    private static extern int SlotDwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);

    // #442: an add-on's window is a MediaViewer window. The app's icon in the
    // title bar, the taskbar and Alt+Tab (AppIcon, as Import's windows); sized in
    // DIPs at the viewer's DPI with a floor it cannot be shrunk past; a window or
    // sheet is owned by the viewer (it stays above it and minimises with it) and
    // centred over it; and every one is marked so the viewer's key router leaves
    // its keys alone (main.cpp handle_app_key): a digit typed into a field is
    // text, not a rating.
    private static Window OpenScreenWindow(JsonElement s, ScreenAt at, string place)
    {
        string title = JStr(s, "title");
        var w = new Window { Title = title.Length > 0 ? title : "MediaViewer" };
        MediaViewer.Shared.AppIcon.Apply(w);
        IntPtr hwnd = WinRT.Interop.WindowNative.GetWindowHandle(w);
        int width = (int)JNum(s, "width", 0), height = (int)JNum(s, "height", 0);
        SlotRect main = default;
        bool haveMain = _themeWindow != IntPtr.Zero && SlotGetWindowRect(_themeWindow, out main);
        uint dpi = _themeWindow != IntPtr.Zero ? SlotGetDpiForWindow(_themeWindow) : 0;
        double scale = dpi > 0 ? dpi / 96.0 : 1.0;
        int Px(double dip) => (int)Math.Round(dip * scale);
        int minW, minH;
        switch (place)
        {
            case "pane":
            {
                // Docked to the right edge of the viewer window.
                int pw = Px(width > 0 ? width : 340);
                int ph = haveMain ? main.Bottom - main.Top : Px(height > 0 ? height : 640);
                w.AppWindow.Resize(new Windows.Graphics.SizeInt32(pw, ph));
                if (haveMain) w.AppWindow.Move(new Windows.Graphics.PointInt32(main.Right - pw, main.Top));
                (minW, minH) = (Px(260), Px(320));
                break;
            }
            case "sheet":
            {
                int sw = Px(width > 0 ? width : 520), sh = Px(height > 0 ? height : 420);
                w.AppWindow.Resize(new Windows.Graphics.SizeInt32(sw, sh));
                if (haveMain)
                {
                    w.AppWindow.Move(new Windows.Graphics.PointInt32(
                        main.Left + (main.Right - main.Left - sw) / 2, main.Top + Px(60)));
                }
                (minW, minH) = (Px(360), Px(280));
                break;
            }
            default:
            {
                int ww = Px(width > 0 ? width : 760), wh = Px(height > 0 ? height : 560);
                w.AppWindow.Resize(new Windows.Graphics.SizeInt32(ww, wh));
                if (haveMain)
                {
                    // Over the viewer, a third of the way down, as the AI windows open.
                    int mw = main.Right - main.Left, mh = main.Bottom - main.Top;
                    w.AppWindow.Move(new Windows.Graphics.PointInt32(
                        main.Left + (mw - ww) / 2, main.Top + Math.Max(0, (mh - wh) / 3)));
                }
                (minW, minH) = (Px(Math.Min(420, width > 0 ? width : 420)), Px(Math.Min(480, height > 0 ? height : 480)));
                break;
            }
        }
        try
        {
            if (w.AppWindow.Presenter is Microsoft.UI.Windowing.OverlappedPresenter op)
            {
                op.PreferredMinimumWidth = minW;
                op.PreferredMinimumHeight = minH;
                op.IsMinimizable = place == "pane";  // an owned window minimises with the viewer
            }
            // #508: the title bar in the window's own light or dark, not a light
            // bar over a dark page.
            // Dark when the window's canvas is (DWMWA_USE_IMMERSIVE_DARK_MODE).
            Windows.UI.Color bg = Brush(Canvas).Color;
            int dark = 0.2126 * bg.R + 0.7152 * bg.G + 0.0722 * bg.B < 128 ? 1 : 0;
            _ = SlotDwmSetWindowAttribute(hwnd, 20, ref dark, sizeof(int));
            if (place != "pane" && _themeWindow != IntPtr.Zero) SlotSetWindowLongPtr(hwnd, SlotGwlpHwndParent, _themeWindow);
            SlotSetProp(hwnd, "MediaViewer.AddonWindow", 1);
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or COMException)
        {
            // Cosmetic: the window still works without the floor or the owner.
            System.Diagnostics.Debug.WriteLine(ex.Message);
        }
        w.Closed += (_, _) =>
        {
            if (ClosingFromCore.Contains(at)) return;
            ScreenWindows.Remove(at);
            ForgetSurfaces(at);
            ForgetReveals(at);
            SlotNative.CloseScreen(at.Addon, at.Screen);
        };
        w.Activate();
        return w;
    }
}
