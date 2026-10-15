// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using MediaViewer.Interop;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.System;
using Windows.UI;
using Windows.UI.ViewManagement;

namespace MediaViewer.Chrome;

/// <summary>
/// Settings → Appearance → Theme (issue #394): a sheet of cards, one per theme,
/// each drawn in that theme's own seven tokens, in place of a plain list. The
/// Mac twin is ThemePicker.swift.
///
/// Default, the built-in palettes, then every theme an installed add-on
/// contributes. A built-in's tokens are in the binary. An add-on's are read
/// through <see cref="AddonNative.OpenTheme"/> on a worker when the sheet opens
/// (it re-verifies the add-on), and its card shows a plain hairline until they
/// arrive. Choosing closes the sheet and applies the theme as the list did.
///
/// Keyboard: the cards are buttons, arrows move between them (XY focus), Enter
/// or Space chooses, Esc closes.
/// </summary>
public static partial class IslandHost
{
    private sealed record ThemeChoice(string Key, string Name, string Detail, ThemeTokens? Tokens);

    private const int ThemeSheetColumns = 4;

    private static Button? _themeButton;
    private static StackPanel? _themeButtonStrip;
    private static TextBlock? _themeButtonName;
    private static Flyout? _themeSheet;
    // Add-on themes' tokens, by "addon/theme", for the swatches only.
    private static readonly Dictionary<string, ThemeTokens> _themePreviews = new(StringComparer.Ordinal);
    private static readonly HashSet<string> _themePreviewsReading = new(StringComparer.Ordinal);

    private static FrameworkElement BuildThemeRow()
    {
        _themeButtonStrip = new StackPanel { Orientation = Orientation.Horizontal, Height = 16 };
        var strip = new Border
        {
            Child = _themeButtonStrip,
            CornerRadius = new CornerRadius(3),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(1),
            VerticalAlignment = VerticalAlignment.Center,
        };
        _themeButtonName = Label("");
        _themeButtonName.Foreground = Brush(Title);
        _themeButtonName.TextTrimming = TextTrimming.CharacterEllipsis;
        _themeButtonName.VerticalAlignment = VerticalAlignment.Center;
        var chevron = Label("›");
        chevron.VerticalAlignment = VerticalAlignment.Center;
        var content = new Grid { ColumnSpacing = Space(8) };
        content.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        content.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        content.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        content.Children.Add(strip);
        Grid.SetColumn(_themeButtonName, 1);
        content.Children.Add(_themeButtonName);
        Grid.SetColumn(chevron, 2);
        content.Children.Add(chevron);
        _themeButton = new Button
        {
            Content = content,
            Width = 180,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            AllowFocusOnInteraction = true,
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Title),
        };
        FillSettingsButton(_themeButton, accent: false);
        _themeButton.Padding = new Thickness(10, 6, 10, 6);
        ToolTipService.SetToolTip(_themeButton, "Choose a theme");
        _themeButton.Click += (_, _) =>
        {
            try { OpenThemeSheet(); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }  // never out of a XAML event
        };
        FrameworkElement row = SettingsRow("Theme", ThemeDetail(), _themeButton);
        _themeDetail = FindDetail(row);
        RefreshThemeRow();
        return row;
    }

    private static string ThemeDetail() => _themeNote.Length > 0
        ? _themeNote
        : "Colours and a quiet ground for the window, its bars and panes, and the area around your photos and videos. Your photos are never recoloured.";

    // SettingsRow's second label, so the row can say why a theme is not showing.
    private static TextBlock? FindDetail(FrameworkElement row) =>
        row is Grid grid && grid.Children.Count > 0 && grid.Children[0] is StackPanel labels &&
        labels.Children.Count > 1 ? labels.Children[1] as TextBlock : null;

    private static void RefreshThemeRow()
    {
        if (_themeButton is null || _themeButtonStrip is null || _themeButtonName is null) return;
        string name = ThemeChoiceName(_themeSelection);
        _themeButtonName.Text = name;
        AutomationProperties.SetName(_themeButton, "Theme, " + name);
        ThemeChoice? current = ThemeChoices().FirstOrDefault(c => c.Key == _themeSelection);
        _themeButtonStrip.Children.Clear();
        foreach (Color c in SwatchStrip(SwatchPalette(current?.Tokens)))
            _themeButtonStrip.Children.Add(new Border { Width = 6, Background = Brush(c) });
        if (_themeDetail is not null) _themeDetail.Text = ThemeDetail();
    }

    /// <summary>An add-on was installed, updated or removed: read the swatches again.</summary>
    private static void ForgetThemePreviews() => _themePreviews.Clear();

    // Default, the built-ins, then every add-on theme that verifies.
    private static List<ThemeChoice> ThemeChoices()
    {
        var list = new List<ThemeChoice> { new("", "Default", "Follows the system", null) };
        foreach (BuiltinThemes.Item item in BuiltinThemes.All)
            list.Add(new ThemeChoice(item.Key, item.Name, BuiltinDetail(item), ParseTheme(item.Json)));
        foreach (OpenAddon addon in _openAddons)
        {
            if (addon.State != "ok") continue;
            foreach (OpenTheme theme in addon.Themes)
            {
                string key = ThemeKey(addon.Id, theme.Id);
                list.Add(new ThemeChoice(key, theme.Name, addon.Name, _themePreviews.GetValueOrDefault(key)));
            }
        }
        return list;
    }

    // "Dark · Rain": the appearance the palette keeps and its ground.
    private static string BuiltinDetail(BuiltinThemes.Item item)
    {
        string look = item.Json.Contains("\"dark\":null", StringComparison.Ordinal) ? "Light" : "Dark";
        if (item.Effect == "none") return look;
        string ground = item.Effect.Replace('-', ' ');
        return look + " · " + char.ToUpperInvariant(ground[0]) + ground[1..];
    }

    // What the Theme row's button says for the selection.
    private static string ThemeChoiceName(string selection)
    {
        if (selection.Length == 0) return "Default";
        if (BuiltinThemes.Find(selection) is { } builtin) return builtin.Name;
        foreach (OpenAddon addon in _openAddons)
        {
            if (addon.State != "ok") continue;
            foreach (OpenTheme theme in addon.Themes)
                if (ThemeKey(addon.Id, theme.Id) == selection) return theme.Name;
        }
        // "Not available" when the add-on is there but refused (the row's line
        // says why), "Not installed" when it is gone.
        int slash = selection.IndexOf('/');
        string addonId = slash > 0 ? selection[..slash] : selection;
        return _openAddons.Any(a => a.Id == addonId) ? "Not available" : "Not installed";
    }

    // Windows' appearance, not the chrome's: Default and a two-palette theme
    // follow it.
    private static bool SystemDark()
    {
        if (_themeSettings is null) return true;
        Color f = _themeSettings.GetColorValue(UIColorType.Foreground);
        return f.R * 299 + f.G * 587 + f.B * 114 > 128000;
    }

    // A theme's palette as it would show now; Default's is Windows' own.
    private static ThemePalette SwatchPalette(ThemeTokens? tokens)
    {
        bool dark = SystemDark();
        if (tokens is not null)
        {
            ThemePalette? p = dark ? tokens.Dark ?? tokens.Light : tokens.Light ?? tokens.Dark;
            if (p is not null) return p;
        }
        return DefaultPalette(dark);
    }

    private static Color[] SwatchStrip(ThemePalette p) =>
        new[] { p.Canvas, p.Surface, p.Title, p.Body, p.Hairline, p.Accent };

    private static void LoadThemePreviews()
    {
        var wanted = new List<(string Key, string Addon, string Theme)>();
        foreach (OpenAddon addon in _openAddons)
        {
            if (addon.State != "ok") continue;
            foreach (OpenTheme theme in addon.Themes)
            {
                string key = ThemeKey(addon.Id, theme.Id);
                if (_themePreviews.ContainsKey(key) || !_themePreviewsReading.Add(key)) continue;
                wanted.Add((key, addon.Id, theme.Id));
            }
        }
        if (wanted.Count == 0) return;
        _ = Task.Run(() =>
        {
            var read = new Dictionary<string, ThemeTokens>(StringComparer.Ordinal);
            foreach ((string key, string addon, string theme) in wanted)
            {
                string? json = null;
                try { json = AddonNative.OpenTheme(addon, theme); }
                catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException)
                {
                    System.Diagnostics.Debug.WriteLine(ex.GetType().Name);
                }
                if (ParseTheme(json) is { } tokens) read[key] = tokens;
            }
            DispatcherQueueControllerTryEnqueue(() =>
            {
                foreach ((string key, _, _) in wanted) _themePreviewsReading.Remove(key);
                foreach (KeyValuePair<string, ThemeTokens> pair in read) _themePreviews[pair.Key] = pair.Value;
                if (read.Count == 0) return;
                RefreshThemeRow();
                // An open sheet is rebuilt with the colours; focus returns to the same card.
                if (_themeSheet?.Content is FrameworkElement)
                {
                    string? focusedKey = (FocusManager.GetFocusedElement(_themeSheet.XamlRoot) as FrameworkElement)?.Tag as string;
                    _themeSheet.Content = BuildThemeSheet(out Button? chosen, focusedKey);
                    chosen?.Focus(FocusState.Programmatic);
                }
            });
        });
    }

    private static void OpenThemeSheet()
    {
        if (_themeButton is null) return;
        LoadThemePreviews();
        _themeSheet?.Hide();
        Style style = FlyoutPresenterStyle();
        style.Setters.Add(new Setter(FlyoutPresenter.CornerRadiusProperty, new CornerRadius(RadiusSheet)));
        style.Setters.Add(new Setter(FlyoutPresenter.PaddingProperty, new Thickness(20)));
        style.Setters.Add(new Setter(FrameworkElement.MaxWidthProperty, 720.0));
        style.Setters.Add(new Setter(FrameworkElement.MaxHeightProperty, 600.0));
        var sheet = new Flyout
        {
            FlyoutPresenterStyle = style,
            // Centred over Settings, the rest dimmed: a modal, not a menu.
            Placement = FlyoutPlacementMode.Full,
            LightDismissOverlayMode = LightDismissOverlayMode.On,
            Content = BuildThemeSheet(out Button? chosen, null),
        };
        sheet.Opened += (_, _) => chosen?.Focus(FocusState.Keyboard);
        sheet.Closed += (_, _) =>
        {
            if (ReferenceEquals(_themeSheet, sheet)) _themeSheet = null;
            _themeButton?.Focus(FocusState.Programmatic);
        };
        _themeSheet = sheet;
        sheet.ShowAt(_themeButton);
    }

    private static void ChooseFromSheet(string key)
    {
        _themeSheet?.Hide();
        ChooseTheme(key);
    }

    // `focus` is the card to return focus to; the chosen one otherwise.
    private static FrameworkElement BuildThemeSheet(out Button? focusCard, string? focus)
    {
        focusCard = null;
        var root = new Grid { RowSpacing = Space(14), Width = 660 };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        var head = new StackPanel { Spacing = Space(4) };
        TextBlock title = Label("Theme");
        title.FontSize = TypeSize(20);
        title.Foreground = Brush(Title);
        AutomationProperties.SetHeadingLevel(title, Microsoft.UI.Xaml.Automation.Peers.AutomationHeadingLevel.Level1);
        TextBlock note = Label(_themeNote.Length > 0
            ? _themeNote
            : "Colours and a quiet ground for the window, its bars and panes. Your photos are never recoloured.");
        note.FontSize = TypeSize(12);
        note.TextWrapping = TextWrapping.Wrap;
        head.Children.Add(title);
        head.Children.Add(note);
        root.Children.Add(head);

        var grid = new Grid
        {
            ColumnSpacing = Space(12),
            RowSpacing = Space(12),
            Padding = new Thickness(4),
            XYFocusKeyboardNavigation = XYFocusKeyboardNavigationMode.Enabled,
            TabFocusNavigation = KeyboardNavigationMode.Once,
        };
        for (int c = 0; c < ThemeSheetColumns; c++)
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        List<ThemeChoice> choices = ThemeChoices();
        Button? first = null;
        for (int i = 0; i < choices.Count; i++)
        {
            if (i % ThemeSheetColumns == 0) grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            Button card = ThemeCard(choices[i]);
            Grid.SetRow(card, i / ThemeSheetColumns);
            Grid.SetColumn(card, i % ThemeSheetColumns);
            grid.Children.Add(card);
            first ??= card;
            if (choices[i].Key == (focus ?? _themeSelection)) focusCard = card;
        }
        focusCard ??= first;
        var scroll = new ScrollViewer
        {
            Content = grid,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
        };
        Grid.SetRow(scroll, 1);
        root.Children.Add(scroll);

        var foot = new Grid { ColumnSpacing = Space(12) };
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        TextBlock hint = Label("Arrows move · Enter chooses · Esc closes");
        hint.FontSize = TypeSize(12);
        hint.VerticalAlignment = VerticalAlignment.Center;
        foot.Children.Add(hint);
        Button cancel = SettingsButton("Cancel", () => _themeSheet?.Hide());
        Grid.SetColumn(cancel, 1);
        foot.Children.Add(cancel);
        Grid.SetRow(foot, 2);
        root.Children.Add(foot);

        root.KeyDown += (_, e) =>
        {
            if (e.Key != VirtualKey.Escape) return;
            e.Handled = true;
            _themeSheet?.Hide();
        };
        return root;
    }

    private static Button ThemeCard(ThemeChoice choice)
    {
        bool chosen = choice.Key == _themeSelection;
        bool pending = choice.Tokens is null && choice.Key.Length > 0;
        ThemePalette p = SwatchPalette(choice.Tokens);

        var name = Label(choice.Name);
        name.Foreground = Brush(Title);
        name.FontSize = TypeSize(14);
        name.TextTrimming = TextTrimming.CharacterEllipsis;
        var detail = Label(choice.Detail);
        detail.FontSize = TypeSize(11);
        detail.TextTrimming = TextTrimming.CharacterEllipsis;
        var labels = new StackPanel { Spacing = Space(2) };
        labels.Children.Add(name);
        labels.Children.Add(detail);
        var caption = new Grid { ColumnSpacing = Space(4) };
        caption.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        caption.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        caption.Children.Add(labels);
        if (chosen)
        {
            var check = new FontIcon
            {
                Glyph = "",  // CheckMark
                FontSize = 12,
                Foreground = Brush(ChromeColour.Accent),
                VerticalAlignment = VerticalAlignment.Top,
            };
            Grid.SetColumn(check, 1);
            caption.Children.Add(check);
        }

        var body = new StackPanel { Spacing = Space(8) };
        body.Children.Add(pending ? EmptyPreview() : ThemePreview(p));
        body.Children.Add(caption);
        var frame = new Border
        {
            Child = body,
            Padding = Pad(8),
            CornerRadius = Corner(10),
            Background = Brush(ChromeColour.Surface),
            BorderBrush = Brush(chosen ? ChromeColour.Accent : Hairline),
            BorderThickness = new Thickness(chosen ? 2 : 1),
        };
        var card = new Button
        {
            Content = frame,
            Tag = choice.Key,
            Padding = new Thickness(0),
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            Background = Brush(Colors.Transparent),
            AllowFocusOnInteraction = true,
        };
        FlattenButton(card);
        card.CornerRadius = Corner(10);
        AutomationProperties.SetName(card, choice.Name + ", " + choice.Detail + (chosen ? ", chosen" : ""));
        string key = choice.Key;
        card.Click += (_, _) =>
        {
            try { ChooseFromSheet(key); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }  // never out of a XAML event
        };
        return card;
    }

    private static FrameworkElement EmptyPreview() => new Border
    {
        Height = 92,
        CornerRadius = Corner(6),
        BorderBrush = Brush(Hairline),
        BorderThickness = new Thickness(1),
    };

    // A small window in the theme's colours: a bar, a pane with text, an accent,
    // and the six colours as a strip. Brushes of its own: it never follows the
    // chrome's.
    private static FrameworkElement ThemePreview(ThemePalette p)
    {
        static Border Bar(Color c, double width, double height, double opacity = 1) => new()
        {
            Width = width,
            Height = height,
            CornerRadius = new CornerRadius(height / 2),
            Background = Brush(c),
            Opacity = opacity,
            HorizontalAlignment = HorizontalAlignment.Left,
            VerticalAlignment = VerticalAlignment.Center,
        };

        var window = new Grid { Height = 92, Background = Brush(p.Canvas) };
        window.RowDefinitions.Add(new RowDefinition { Height = new GridLength(14) });
        window.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        window.RowDefinitions.Add(new RowDefinition { Height = new GridLength(8) });

        var bar = new Grid
        {
            Background = Brush(p.Surface),
            BorderBrush = Brush(p.Hairline),
            BorderThickness = new Thickness(0, 0, 0, 1),
            Padding = new Thickness(7, 0, 7, 0),
            ColumnSpacing = 4,
        };
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        bar.Children.Add(Bar(p.Accent, 6, 6));
        Border barText = Bar(p.Body, 28, 4, 0.7);
        Grid.SetColumn(barText, 1);
        bar.Children.Add(barText);
        Border barEnd = Bar(p.Disabled, 14, 4);
        Grid.SetColumn(barEnd, 2);
        bar.Children.Add(barEnd);
        window.Children.Add(bar);

        var pane = new Grid { Margin = new Thickness(8, 6, 8, 6), ColumnSpacing = 6 };
        pane.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        pane.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var text = new StackPanel { Spacing = 4 };
        text.Children.Add(new TextBlock
        {
            Text = "Aa",
            FontSize = 15,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            Foreground = Brush(p.Title),
        });
        text.Children.Add(Bar(p.Body, 44, 3));
        text.Children.Add(Bar(p.Body, 32, 3, 0.6));
        pane.Children.Add(text);
        var box = new Border
        {
            Width = 46,
            Height = 34,
            VerticalAlignment = VerticalAlignment.Top,
            CornerRadius = new CornerRadius(4),
            Background = Brush(p.Surface),
            BorderBrush = Brush(p.Hairline),
            BorderThickness = new Thickness(1),
            Child = new Border
            {
                Width = 18,
                Height = 6,
                Margin = new Thickness(5),
                CornerRadius = new CornerRadius(3),
                Background = Brush(p.Accent),
                HorizontalAlignment = HorizontalAlignment.Right,
                VerticalAlignment = VerticalAlignment.Bottom,
            },
        };
        Grid.SetColumn(box, 1);
        pane.Children.Add(box);
        Grid.SetRow(pane, 1);
        window.Children.Add(pane);

        var strip = new Grid();
        Color[] colours = SwatchStrip(p);
        for (int i = 0; i < colours.Length; i++)
        {
            strip.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            var cell = new Border { Background = Brush(colours[i]) };
            Grid.SetColumn(cell, i);
            strip.Children.Add(cell);
        }
        Grid.SetRow(strip, 2);
        window.Children.Add(strip);

        var frame = new Border
        {
            Child = window,
            CornerRadius = Corner(6),
            BorderBrush = Brush(p.Hairline),
            BorderThickness = new Thickness(1),
        };
        AutomationProperties.SetAccessibilityView(frame, Microsoft.UI.Xaml.Automation.Peers.AccessibilityView.Raw);
        return frame;
    }
}
