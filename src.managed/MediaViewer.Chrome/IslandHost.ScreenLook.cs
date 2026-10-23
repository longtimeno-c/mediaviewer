// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.CompilerServices;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;

namespace MediaViewer.Chrome;

/// <summary>
/// How an add-on's own window looks and keeps its place (#508), for every
/// add-on with a screen: the renderer itself is IslandHost.Screens.cs.
/// </summary>
/// <remarks>
/// <para>A screen window draws in the system face, Segoe UI Variable (the
/// theme's face when the theme names one), on a type ramp with a clear
/// heading / body / secondary order; buttons are filled, as Settings' are;
/// a toggle sits on the right of its label. A section in the info pane keeps
/// the chrome's face: it sits among the chrome's own text.</para>
/// <para>A new tree revision rebuilds the window's content. Before it does,
/// each scroller's offset and the focused control are noted by node id, and
/// put back once the new content has loaded, so flipping a switch or making
/// a code neither jumps to the top nor drops the keyboard focus.</para>
/// </remarks>
public static partial class IslandHost
{
    // Set while a screen window's tree is built (UI thread only); null for the
    // info pane's sections, which keep the chrome's face and sizes.
    private static FontFamily? _screenFace;
    // Node id -> the element built for it, for the window being built.
    private static Dictionary<string, FrameworkElement>? _screenIds;

    private static FontFamily? _systemTextFace, _systemDisplayFace;
    private static FontFamily SystemTextFace => _systemTextFace ??= new("Segoe UI Variable Text,Segoe UI");
    private static FontFamily SystemDisplayFace => _systemDisplayFace ??= new("Segoe UI Variable Display,Segoe UI");

    // The theme's face when it names an installed one, else the system's.
    private static FontFamily ScreenWindowFace() =>
        _theme is { Font.Length: > 0 } t && string.Equals(UiFont.Source, t.Font, StringComparison.Ordinal)
            ? UiFont : SystemTextFace;

    private static bool InScreenWindow => _screenFace is not null;

    /// <summary>Text in a screen: the window's face when building a window.</summary>
    private static TextBlock SText(string text, ChromeColour color, double size = 0, bool bold = false,
                                   int maxLines = 0, bool display = false)
    {
        TextBlock t = Text(text, color, size, bold, maxLines);
        if (_screenFace is { } face)
        {
            t.FontFamily = display && ReferenceEquals(face, SystemTextFace) ? SystemDisplayFace : face;
        }
        return t;
    }

    // A text node's size and weight. In a window, the Fluent ramp (Body 14,
    // BodyStrong, a 17 heading, 13 / 12 secondary and caption); in the pane,
    // the chrome face's whole sizes as before.
    private static (double Size, bool Bold) ScreenTextStyle(string style) => InScreenWindow
        ? style switch
        {
            "title" => (TypeSize(14), true),
            "heading" => (TypeSize(17), true),
            "secondary" => (TypeSize(13), false),
            "caption" => (TypeSize(12), false),
            _ => (TypeSize(14), false),
        }
        : style switch
        {
            "title" => (UiFontSize - 1, true),
            "heading" => (UiFontSize + 1, true),
            "secondary" => (UiFontSize - 3, false),
            "caption" => (UiFontSize - 4, false),
            _ => (UiFontSize - 2, false),
        };

    // Lazy, like SymbolFont: a brush built in IslandHost's type initializer
    // (before XAML starts) throws there, and then no island attaches.
    private static SolidColorBrush? _destructiveLight, _destructiveDark;
    private static SolidColorBrush DestructiveLight => _destructiveLight ??= new(Windows.UI.Color.FromArgb(255, 196, 43, 28));
    private static SolidColorBrush DestructiveDark => _destructiveDark ??= new(Windows.UI.Color.FromArgb(255, 255, 153, 164));

    /// <summary>A screen's button. In a window it is filled as Settings' buttons
    /// are (the accent for "primary", red text for "destructive") so it reads as
    /// a button on a card; in the pane, the chrome's flat button.</summary>
    private static Button ScreenButton(string label, Action click, string style = "plain")
    {
        Button b = EditButton(label, click);
        // Reached by Tab like every control here (EditButton's default keeps
        // focus off the canvas; a screen is a window of its own).
        b.AllowFocusOnInteraction = true;
        b.IsTabStop = true;
        if (_screenFace is not { } face)
        {
            if (style == "primary") b.Background = Brush(Hairline);
            return b;
        }
        b.FontFamily = face;
        b.FontSize = TypeSize(14);
        if (b.Content is TextBlock tb)
        {
            tb.FontFamily = face;
            tb.FontSize = TypeSize(14);
        }
        FillSettingsButton(b, accent: style == "primary");
        b.MinHeight = 32;
        b.Padding = Pad(12, 5, 12, 6);
        if (style == "destructive")
        {
            SolidColorBrush red = IslandTheme == ElementTheme.Dark ? DestructiveDark : DestructiveLight;
            b.Foreground = red;
            if (b.Content is TextBlock label2) label2.Foreground = red;
            foreach (string key in new[] { "ButtonForeground", "ButtonForegroundPointerOver", "ButtonForegroundPressed" })
                b.Resources[key] = red;
        }
        return b;
    }

    // A window's text field: its face, and the window's canvas inside the box so
    // it stands out from the card it sits on.
    private static MediaViewer.Shared.FakeInputLook? _screenInputLook;
    private static FontFamily? _screenInputFace;

    private static MediaViewer.Shared.FakeInputLook ScreenInputLook(FontFamily face)
    {
        if (_screenInputLook is not null && ReferenceEquals(_screenInputFace, face)) return _screenInputLook;
        _screenInputFace = face;
        return _screenInputLook = new MediaViewer.Shared.FakeInputLook
        {
            Font = () => face,
            FontSize = () => TypeSize(14),
            Title = () => Brush(Title),
            Body = () => Brush(Body),
            Canvas = () => Brush(Canvas),
            Hairline = () => Brush(Hairline),
            Selection = () => Brush(TextSelection),
            SelectionInk = () => Brush(TextSelectionInk),
            Accent = () => Brush(ChromeColour.Accent),
        };
    }

    /// <summary>The element that carries a node's name, help and enabled state
    /// when the node is built as a row around a control (a toggle and its label).</summary>
    private sealed class ScreenInline(Control control)
    {
        public Control Control { get; } = control;
    }

    /// <summary>A toggle on the right of its label, as Settings' rows and the
    /// Mac's Toggle; clicking the label flips it too.</summary>
    private static FrameworkElement InlineToggle(string label, ToggleSwitch toggle)
    {
        var row = new Grid { ColumnSpacing = Space(16), MinHeight = 32 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        (double size, _) = ScreenTextStyle("body");
        TextBlock text = SText(label, Title, size);
        text.VerticalAlignment = VerticalAlignment.Center;
        // Tapping the words flips the switch, as a label does; the switch is
        // the one control Narrator and Tab see.
        text.Tapped += (_, e) =>
        {
            if (!toggle.IsEnabled) return;
            toggle.IsOn = !toggle.IsOn;
            e.Handled = true;
        };
        AutomationProperties.SetAccessibilityView(text, Microsoft.UI.Xaml.Automation.Peers.AccessibilityView.Raw);
        row.Children.Add(text);
        toggle.Header = null;
        toggle.OnContent = "";
        toggle.OffContent = "";
        toggle.MinWidth = 0;
        toggle.Width = 48;
        toggle.VerticalAlignment = VerticalAlignment.Center;
        toggle.HorizontalAlignment = HorizontalAlignment.Right;
        Grid.SetColumn(toggle, 1);
        row.Children.Add(toggle);
        row.Tag = new ScreenInline(toggle);
        return row;
    }

    // ---- the window's size ---------------------------------------------------------------

    /// <summary>Once, when a window's first tree has laid out: a window taller than
    /// what it shows shrinks to its content (never below a floor), so it opens
    /// without an empty band at the bottom. One taller than the screen asked for
    /// keeps that height and scrolls.</summary>
    private static void FitScreenWindow(Window w, FrameworkElement content, int asked)
    {
        void Fit(object sender, RoutedEventArgs e)
        {
            content.Loaded -= Fit;
            try
            {
                if (content is not Panel { Children.Count: > 0 } p || p.Children[^1] is not ScrollViewer sv) return;
                sv.UpdateLayout();
                double spare = sv.ViewportHeight - sv.ExtentHeight;
                if (spare < 8 || content.XamlRoot is not { } root) return;
                double scale = root.RasterizationScale;
                Windows.Graphics.SizeInt32 size = w.AppWindow.Size;
                int floor = (int)Math.Round(Math.Min(asked > 0 ? asked : 480, 360) * scale);
                int height = Math.Max(floor, size.Height - (int)Math.Floor(spare * scale));
                if (height < size.Height) w.AppWindow.Resize(new Windows.Graphics.SizeInt32(size.Width, height));
            }
            catch (Exception ex)
            {
                // Cosmetic: the window keeps the size it opened at.
                System.Diagnostics.Debug.WriteLine(ex.Message);
            }
        }
        content.Loaded += Fit;
    }

    // ---- keeping the place across a rebuild --------------------------------------------

    private readonly record struct ScreenPlace(Dictionary<string, double> Offsets, string? FocusId, int FocusIndex,
                                               FocusState FocusHow);

    // The ids the last build of each window registered.
    private static readonly ConditionalWeakTable<UIElement, Dictionary<string, FrameworkElement>> ScreenIdsOf = new();

    private static void RegisterScreenId(string id, FrameworkElement e)
    {
        if (_screenIds is null || id.Length == 0) return;
        _screenIds.TryAdd(id, e);  // a repeated id: the first keeps it
    }

    // Depth-first, the focusable controls under (and including) `root`.
    private static IEnumerable<Control> ScreenControls(DependencyObject root)
    {
        if (root is Control c && c.IsTabStop) yield return c;
        int n = VisualTreeHelper.GetChildrenCount(root);
        for (int i = 0; i < n; ++i)
        {
            foreach (Control inner in ScreenControls(VisualTreeHelper.GetChild(root, i))) yield return inner;
        }
    }

    // Content whose place is not put back yet (it has not loaded): a second
    // revision right behind the first keeps that place, not the top.
    private sealed class PendingPlace(ScreenPlace place)
    {
        public ScreenPlace Place { get; } = place;
    }

    private static readonly ConditionalWeakTable<UIElement, PendingPlace> PendingPlaces = new();

    private static ScreenPlace? NoteScreenPlace(UIElement? content)
    {
        if (content is not null && PendingPlaces.TryGetValue(content, out PendingPlace? pending)) return pending.Place;
        if (content is null || !ScreenIdsOf.TryGetValue(content, out var ids)) return null;
        try
        {
            var offsets = new Dictionary<string, double>(StringComparer.Ordinal);
            foreach ((string id, FrameworkElement e) in ids)
            {
                if (e is ScrollViewer sv && sv.VerticalOffset > 0) offsets[id] = sv.VerticalOffset;
            }
            string? focusId = null;
            int focusIndex = -1;
            FocusState how = FocusState.Programmatic;
            if (content.XamlRoot is { } root && FocusManager.GetFocusedElement(root) is DependencyObject focused)
            {
                var byElement = new Dictionary<DependencyObject, string>();
                foreach ((string id, FrameworkElement e) in ids) byElement.TryAdd(e, id);
                for (DependencyObject? up = focused; up is not null; up = VisualTreeHelper.GetParent(up))
                {
                    if (!byElement.TryGetValue(up, out string? id)) continue;
                    focusId = id;
                    focusIndex = ScreenControls(up).TakeWhile(c => !ReferenceEquals(c, focused)).Count();
                    if (focused is Control fc && fc.FocusState == FocusState.Keyboard) how = FocusState.Keyboard;
                    break;
                }
            }
            return new ScreenPlace(offsets, focusId, focusIndex, how);
        }
        catch (Exception ex)
        {
            // Never let the rebuild fail over its place: start at the top instead.
            System.Diagnostics.Debug.WriteLine(ex.Message);
            return null;
        }
    }

    private static void RestoreScreenPlace(FrameworkElement content, ScreenPlace place)
    {
        if (!ScreenIdsOf.TryGetValue(content, out var ids)) return;
        PendingPlaces.AddOrUpdate(content, new PendingPlace(place));
        // Focusing a control scrolls it into view: the focus put back on a toggle
        // near the bottom pulled a person who had scrolled up to the transfer back
        // down on every state change. The refocused control does not ask to be
        // brought into view (handled at its source, before any ScrollViewer on the
        // way up acts on it); the kept offsets are the place.
        var stay = new Windows.Foundation.TypedEventHandler<UIElement, BringIntoViewRequestedEventArgs>((_, args) => args.Handled = true);
        void Apply(object sender, RoutedEventArgs e)
        {
            content.Loaded -= Apply;
            PendingPlaces.Remove(content);
            Control? refocused = null;
            try
            {
                if (place.FocusId is { } fid && ids.TryGetValue(fid, out FrameworkElement? target))
                {
                    Control? c = ScreenControls(target).Skip(Math.Max(0, place.FocusIndex)).FirstOrDefault()
                                 ?? ScreenControls(target).FirstOrDefault();
                    if (c is { IsEnabled: true })
                    {
                        refocused = c;
                        c.BringIntoViewRequested += stay;
                        c.Focus(place.FocusHow);
                    }
                }
                foreach ((string id, double offset) in place.Offsets)
                {
                    if (ids.TryGetValue(id, out FrameworkElement? el) && el is ScrollViewer sv)
                    {
                        sv.UpdateLayout();
                        sv.ChangeView(null, offset, null, disableAnimation: true);
                    }
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine(ex.Message);
            }
            // A request the focus raised from the next layout pass is still the
            // restore's; one from the person after that scrolls as usual.
            if (refocused is null) return;
            if (refocused.DispatcherQueue is not { } queue ||
                !queue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low,
                                  () => refocused.BringIntoViewRequested -= stay))
            {
                refocused.BringIntoViewRequested -= stay;
            }
        }
        content.Loaded += Apply;
    }
}
