// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Numerics;
using MediaViewer.Interop;
using Microsoft.UI.Composition;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Input;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.UI.Core;
using DispatcherQueue = Microsoft.UI.Dispatching.DispatcherQueue;
using DispatcherQueueTimer = Microsoft.UI.Dispatching.DispatcherQueueTimer;
using VirtualKey = Windows.System.VirtualKey;

namespace MediaViewer.Ai.Chrome;

/// <summary>One result: a photo, or a clip's best moment with the others grouped under it.</summary>
internal sealed class ResultTile
{
    public int Index { get; init; }
    public string Path { get; init; } = "";
    public long PtsMs { get; init; } = -1;
    public uint More { get; init; }
    public bool Video { get; init; }
    public MvAiMatch Match { get; init; }
    /// <summary>For a speech match, the words that matched; fetched off the UI thread.</summary>
    public string? Snippet { get; set; }
    public bool SnippetRequested { get; set; }
    public TextBlock? Caption { get; set; }
    public string Name => System.IO.Path.GetFileName(Path);
    public (string, long) Key => (Path, PtsMs);
    public ImageSource? Thumb { get; set; }
    public bool ThumbRequested { get; set; }
    public bool Revealed { get; set; }
    public Image? View { get; set; }
}

/// <summary>
/// The search panel (plan/17 PR 22, the brief's "Search panel"): a floating,
/// translucent window centred over the viewer. Never modal — the viewer stays
/// usable behind it; it steps aside when the viewer is clicked.
/// </summary>
/// <remarks>
/// Keyboard-complete (plan/16 verify: open search, type, navigate results,
/// open a moment, next / previous match, close — no mouse): the query is
/// focused with the last query selected; typing searches after ~200 ms; Down
/// enters the grid; arrows, Home / End and Page Up / Down move; Enter opens
/// the results as the viewer's listing on that tile (a clip lands paused on
/// its moment) and Ctrl+Enter opens them in the gallery grid; Esc steps back
/// from the grid to the query, then closes. Tab reaches the scope and kind
/// chips and the footer. The window is marked so the native key router leaves
/// its keys alone (Native.Adopt).
/// </remarks>
internal sealed class SearchWindow : Window, IDisposable
{
    private const int MaxResults = 240;
    private const double TileW = 176, ImageH = 132, TileH = 156, Gap = 10, GridPad = 16;
    private const int StaggerCount = 16;
    private const int StaggerMs = 25;

    private readonly AiChrome _chrome;
    private readonly AiApi _api;
    private readonly Look _look;
    private readonly DispatcherQueueTimer _debounce;
    private readonly DispatcherQueueTimer _poll;
    private readonly SemaphoreSlim _thumbGate = new(3);
    private readonly ThumbCache _thumbs = new(320);

    private Grid _root = null!;
    private Border _card = null!;
    private TextBox _query = null!;
    private Border _similarChip = null!;
    private TextBlock _similarText = null!;
    private readonly ToggleButton[] _scopeChips = new ToggleButton[3];
    private readonly ToggleButton[] _kindChips = new ToggleButton[3];
    private readonly ToggleButton[] _findChips = new ToggleButton[3];
    private readonly string[] _scopeTips = new string[3];
    private bool _findAudio;  // the audio availability ShowFind last drew
    private StackPanel _findRow = null!;
    private TextBlock _count = null!;
    private ScrollViewer _scroll = null!;
    private ItemsRepeater _repeater = null!;
    private Border _ring = null!;
    private ContentControl _gridHost = null!;
    private StackPanel _empty = null!;
    private ProgressRing _footerRing = null!;
    private TextBlock _footerText = null!;
    private Border _badge = null!;
    private TextBlock _badgeText = null!;
    private Button _pause = null!;
    private Button _indexAnyway = null!;
    private TextBlock _hint = null!;

    private List<ResultTile> _tiles = new();
    private int _sel = -1;
    private ulong _search;    // on screen
    private ulong _pending;   // asked for, not answered
    private bool? _openWhenReady;
    private AiChrome.SimilarTo? _similar;
    private MvAiScope _scope = MvAiScope.Folder;
    private MvAiKinds _kinds = MvAiKinds.All;
    private MvAiKinds _find;  // MV_AI_FIND_* bits; none = all
    private bool _visible;
    private bool _disposing;
    private bool _settingText;
    private long _lastRunTick;
    private ulong _lastRunFrames;
    private int _batch;
    private bool _indexOffered;
    private bool? _startedIndexing;  // Index chosen in this showing (recursive?)

    public event Action? Hidden;

    internal SearchWindow(AiChrome chrome)
    {
        _chrome = chrome;
        _api = chrome.Api;
        _look = chrome.Look;
        Title = "Search photos and videos";

        if (AppWindow.Presenter is not OverlappedPresenter)
        {
            AppWindow.SetPresenter(OverlappedPresenter.Create());
        }
        if (AppWindow.Presenter is OverlappedPresenter p)
        {
            // A panel, not a document window: no caption, no resize, no min / max.
            p.SetBorderAndTitleBar(true, false);
            p.IsResizable = false;
            p.IsMaximizable = false;
            p.IsMinimizable = false;
        }
        AppWindow.IsShownInSwitchers = false;
        try { SystemBackdrop = new DesktopAcrylicBackdrop(); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
        Native.Adopt(WinRT.Interop.WindowNative.GetWindowHandle(this), chrome.Host.MainWindow);

        Content = BuildLayout();
        AppWindow.Closing += (_, e) =>
        {
            if (_disposing) return;
            e.Cancel = true;  // Alt+F4 hides, like Esc; the last query is kept
            Hide();
        };
        Activated += (_, e) =>
        {
            // Clicking the viewer: step aside rather than cover it.
            if (e.WindowActivationState == WindowActivationState.Deactivated && _visible) Hide();
        };

        DispatcherQueue q = DispatcherQueue;
        _debounce = q.CreateTimer();
        _debounce.Interval = TimeSpan.FromMilliseconds(200);
        _debounce.IsRepeating = false;
        _debounce.Tick += (_, _) => RunQuery(quiet: false);
        _poll = q.CreateTimer();
        _poll.Interval = TimeSpan.FromMilliseconds(250);  // <= 4 Hz, only while visible
        _poll.Tick += (_, _) => _chrome.ReadStatus();
        _chrome.StatusChanged += OnStatus;
    }

    // ---- layout ------------------------------------------------------------------------

    private UIElement BuildLayout()
    {
        _root = new Grid { Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
        var body = new Grid { Padding = new Thickness(20, 18, 20, 12), RowSpacing = 12 };
        body.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        body.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        body.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        body.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        // The query, with "Similar to …" as a removable token before it.
        _similarText = _look.Text("", _look.FontSize, AddonColour.Title, wrap: false);
        _similarText.VerticalAlignment = VerticalAlignment.Center;
        _similarText.MaxWidth = 320;
        var removeSimilar = new Button
        {
            Content = "✕",
            FontSize = 12,
            Padding = new Thickness(6, 2, 6, 2),
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
            BorderThickness = new Thickness(0),
            IsTabStop = false,
        };
        AutomationProperties.SetName(removeSimilar, "Remove find similar");
        removeSimilar.Click += (_, _) => ClearSimilar(runQuery: true);
        var chipRow = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4 };
        chipRow.Children.Add(_similarText);
        chipRow.Children.Add(removeSimilar);
        _similarChip = new Border
        {
            Child = chipRow,
            Padding = new Thickness(12, 4, 4, 4),
            CornerRadius = new CornerRadius(16),
            Background = _look.Tint(AddonColour.Accent, 48),
            BorderBrush = _look[AddonColour.Accent],
            BorderThickness = new Thickness(1),
            VerticalAlignment = VerticalAlignment.Center,
            Visibility = Visibility.Collapsed,
        };
        _query = new TextBox
        {
            FontFamily = _look.Font,
            FontSize = 22,
            PlaceholderText = "Describe a photo or a moment — “dog on a beach”",
            BorderThickness = new Thickness(0),
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
            Padding = new Thickness(8, 6, 8, 6),
            VerticalAlignment = VerticalAlignment.Center,
            IsSpellCheckEnabled = false,
        };
        AutomationProperties.SetName(_query, "Search photos and videos");
        _query.TextChanged += (_, _) => OnQueryChanged();
        var glyph = _look.Text("⌕", 26, AddonColour.Body, wrap: false);
        glyph.VerticalAlignment = VerticalAlignment.Center;
        var queryRow = new Grid { ColumnSpacing = 8 };
        queryRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        queryRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        queryRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        queryRow.Children.Add(glyph);
        Grid.SetColumn(_similarChip, 1);
        queryRow.Children.Add(_similarChip);
        Grid.SetColumn(_query, 2);
        queryRow.Children.Add(_query);
        var queryCard = new Border
        {
            Child = queryRow,
            Padding = new Thickness(12, 6, 12, 6),
            CornerRadius = new CornerRadius(10),
            Background = _look.Tint(AddonColour.Surface, 200),
            BorderBrush = _look[AddonColour.Hairline],
            BorderThickness = new Thickness(1),
        };
        body.Children.Add(queryCard);

        // Filters (2026-09-27): three captioned groups instead of nine look-alike
        // chips. "Look in" and "Show" are single-choice segmented tracks; "Match
        // by" is toggle tokens, all on at rest (the last one on stays on). Sound
        // and Speech without the Audio piece stay visible, dimmed, with a tooltip
        // that says how to get them: a disabled control shows no tooltip. High
        // contrast keeps the system's own toggle visuals.
        bool hc = HighContrast();
        (string Name, string Tip)[] scopeNames =
        {
            ("This folder", "Search the open folder only."),
            ("+ Subfolders", "Search the open folder and the folders inside it."),
            ("Everywhere", "Search every folder in the index."),
        };
        var scopes = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2 };
        for (int i = 0; i < 3; ++i)
        {
            int s = i;
            _scopeTips[i] = scopeNames[i].Tip;
            _scopeChips[i] = Segment(scopeNames[i].Name, () => SetScope((MvAiScope)s), hc);
            ToolTipService.SetToolTip(_scopeChips[i], scopeNames[i].Tip);
            scopes.Children.Add(_scopeChips[i]);
        }
        (string Name, string Tip, MvAiKinds Kind)[] kindNames =
        {
            ("All", "Show photos and videos.", MvAiKinds.All),
            ("Photos", "Show photos only.", MvAiKinds.Photos),
            ("Videos", "Show videos only.", MvAiKinds.Videos),
        };
        var kinds = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2 };
        for (int i = 0; i < 3; ++i)
        {
            MvAiKinds k = kindNames[i].Kind;
            _kindChips[i] = Segment(kindNames[i].Name, () => SetKinds(k), hc);
            ToolTipService.SetToolTip(_kindChips[i], kindNames[i].Tip);
            kinds.Children.Add(_kindChips[i]);
        }
        _count = _look.Text("", 13, AddonColour.Body, wrap: false);
        _count.HorizontalAlignment = HorizontalAlignment.Right;
        _count.VerticalAlignment = VerticalAlignment.Center;
        var chips = new Grid { ColumnSpacing = 22 };
        chips.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        chips.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        chips.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        FrameworkElement scopeGroup = Group("Look in", Track(scopes, hc));
        chips.Children.Add(scopeGroup);
        FrameworkElement kindGroup = Group("Show", Track(kinds, hc));
        Grid.SetColumn(kindGroup, 1);
        chips.Children.Add(kindGroup);
        Grid.SetColumn(_count, 2);
        chips.Children.Add(_count);

        // What the words are matched against (audio, 2026-09-27).
        var tokens = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        (string Name, string Glyph, MvAiKinds Bit)[] finds =
        {
            ("Picture", GlyphPicture, MvAiKinds.FindPictures),
            ("Sound", GlyphSound, MvAiKinds.FindSounds),
            ("Speech", GlyphSpeech, MvAiKinds.FindSpeech),
        };
        for (int i = 0; i < 3; ++i)
        {
            MvAiKinds bit = finds[i].Bit;
            _findChips[i] = Token(finds[i].Name, finds[i].Glyph, () => ToggleFind(bit), hc);
            tokens.Children.Add(_findChips[i]);
        }
        _findRow = new StackPanel { Orientation = Orientation.Horizontal };
        _findRow.Children.Add(Group("Match by", tokens));
        WireArrows(_scopeChips);
        WireArrows(_kindChips);
        WireArrows(_findChips);
        _findAudio = AudioReady;
        ShowFind();

        var chipRows = new StackPanel { Spacing = 10 };
        chipRows.Children.Add(chips);
        chipRows.Children.Add(_findRow);
        Grid.SetRow(chipRows, 1);
        body.Children.Add(chipRows);

        // The grid, with one selection ring that glides between tiles.
        _repeater = new ItemsRepeater
        {
            Layout = new UniformGridLayout
            {
                MinItemWidth = TileW,
                MinItemHeight = TileH,
                MinColumnSpacing = Gap,
                MinRowSpacing = Gap,
                ItemsStretch = UniformGridLayoutItemsStretch.None,
                ItemsJustification = UniformGridLayoutItemsJustification.Start,
            },
            ItemTemplate = new TileFactory(this),
        };
        _ring = new Border
        {
            Width = TileW + 8,
            Height = ImageH + 8,
            CornerRadius = new CornerRadius(12),
            BorderThickness = new Thickness(2.5),
            BorderBrush = _look[AddonColour.Accent],
            IsHitTestVisible = false,
            Opacity = 0,
        };
        var ringLayer = new Canvas { IsHitTestVisible = false };
        ringLayer.Children.Add(_ring);
        var gridContent = new Grid { Padding = new Thickness(GridPad) };
        gridContent.Children.Add(_repeater);
        gridContent.Children.Add(ringLayer);
        _scroll = new ScrollViewer
        {
            Content = gridContent,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
        };
        _gridHost = new ContentControl
        {
            Content = _scroll,
            IsTabStop = true,
            UseSystemFocusVisuals = false,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            VerticalContentAlignment = VerticalAlignment.Stretch,
        };
        AutomationProperties.SetName(_gridHost, "Results");
        _gridHost.GotFocus += (_, _) =>
        {
            if (_sel < 0 && _tiles.Count > 0) Select(0);
            else MoveRing(animate: false);
        };
        _gridHost.LostFocus += (_, _) => _ring.Opacity = 0;
        _scroll.SizeChanged += (_, _) => MoveRing(animate: false);

        _empty = new StackPanel
        {
            Spacing = 12,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            MaxWidth = 520,
            Visibility = Visibility.Collapsed,
        };
        var middle = new Grid();
        middle.Children.Add(_gridHost);
        middle.Children.Add(_empty);
        Grid.SetRow(middle, 2);
        body.Children.Add(middle);

        // The footer: status pill, compute badge, Pause, Manage.
        _footerRing = new ProgressRing { Width = 14, Height = 14, MinWidth = 14, MinHeight = 14, IsActive = false };
        _footerText = _look.Text("", 13, AddonColour.Body, wrap: false);
        _footerText.VerticalAlignment = VerticalAlignment.Center;
        var pillRow = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        pillRow.Children.Add(_footerRing);
        pillRow.Children.Add(_footerText);
        var pill = new Border
        {
            Child = pillRow,
            Padding = new Thickness(10, 4, 12, 4),
            CornerRadius = new CornerRadius(12),
            Background = _look.Tint(AddonColour.Surface, 170),
            VerticalAlignment = VerticalAlignment.Center,
        };
        _badgeText = _look.Text("", 12, AddonColour.Body, wrap: false);
        _badge = new Border
        {
            Child = _badgeText,
            Padding = new Thickness(8, 3, 8, 3),
            CornerRadius = new CornerRadius(10),
            BorderBrush = _look[AddonColour.Hairline],
            BorderThickness = new Thickness(1),
            VerticalAlignment = VerticalAlignment.Center,
        };
        _pause = _look.Button("Pause", TogglePause);
        // Paused on battery: carry on until the machine is next on AC (never saved).
        _indexAnyway = _look.Button("Index anyway", IndexAnyway);
        _indexAnyway.Visibility = Visibility.Collapsed;
        ToolTipService.SetToolTip(_indexAnyway,
            "Carry on indexing on battery until this PC is next plugged in. " +
            "The battery setting in Settings → Local search stays as it is.");
        Button manage = _look.Button("Manage…", () =>
        {
            Hide();
            _chrome.Host.ShowSettings();
        });
        // Always there: closing never stops indexing or a search.
        Button close = _look.Button("Close", Hide);
        ToolTipService.SetToolTip(close, "Close the panel (Esc). Indexing carries on in the background.");
        var hint = _look.Text("Enter open · Ctrl+Enter gallery", 13, AddonColour.Body, wrap: false);
        hint.VerticalAlignment = VerticalAlignment.Center;
        hint.Visibility = Visibility.Collapsed;  // with results only
        _hint = hint;
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, VerticalAlignment = VerticalAlignment.Center };
        actions.Children.Add(_indexAnyway);
        actions.Children.Add(_pause);
        actions.Children.Add(manage);
        actions.Children.Add(close);
        var footer = new Grid { ColumnSpacing = 8 };
        footer.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        footer.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        footer.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        footer.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        footer.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        footer.Children.Add(pill);
        Grid.SetColumn(_badge, 1);
        footer.Children.Add(_badge);
        Grid.SetColumn(hint, 3);
        footer.Children.Add(hint);
        Grid.SetColumn(actions, 4);
        footer.Children.Add(actions);
        Grid.SetRow(footer, 3);
        body.Children.Add(footer);

        _card = new Border { Child = body, Background = _look.Tint(AddonColour.Canvas, 110) };
        _root.Children.Add(_card);
        _root.PreviewKeyDown += OnPreviewKeyDown;
        SetScope(MvAiScope.Folder, run: false);
        SetKinds(MvAiKinds.All, run: false);
        return _root;
    }

    // ---- filter controls --------------------------------------------------------------------

    private static bool HighContrast()
    {
        try { return new Windows.UI.ViewManagement.AccessibilitySettings().HighContrast; }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex.Message);
            return false;
        }
    }

    /// <summary>"Look in  [track]": a caption and its control on one line.</summary>
    private FrameworkElement Group(string caption, UIElement control)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        TextBlock label = _look.Text(caption, 13, AddonColour.Body, wrap: false);
        label.VerticalAlignment = VerticalAlignment.Center;
        row.Children.Add(label);
        if (control is FrameworkElement fe) fe.VerticalAlignment = VerticalAlignment.Center;
        row.Children.Add(control);
        return row;
    }

    /// <summary>The recessed track a single-choice group sits in (a segmented control).</summary>
    private Border Track(UIElement segments, bool hc) => new()
    {
        Child = segments,
        Padding = new Thickness(2),
        CornerRadius = new CornerRadius(7),
        Background = hc ? new SolidColorBrush(Microsoft.UI.Colors.Transparent) : _look.LiveTint(AddonColour.Body, 22),
        BorderBrush = _look[AddonColour.Hairline],
        BorderThickness = new Thickness(1),
    };

    /// <summary>
    /// One segment of a track: quiet until chosen, then raised on the surface
    /// colour with the title colour and a hairline. Lightweight styling (the
    /// template's own resource keys), so pointer, pressed and the system focus
    /// visual stay Fluent; high contrast keeps the system's toggle look.
    /// </summary>
    private ToggleButton Segment(string label, Action click, bool hc)
    {
        var b = new ToggleButton
        {
            Content = label,
            FontFamily = _look.Font,
            FontSize = 13,
            Padding = new Thickness(10, 2, 10, 3),
            MinHeight = 0,
            MinWidth = 0,
            Height = 24,
            CornerRadius = new CornerRadius(5),
            BorderThickness = new Thickness(1),
        };
        if (!hc)
        {
            var clear = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            Brushes(b, "Background", clear, _look.LiveTint(AddonColour.Body, 26), _look.LiveTint(AddonColour.Body, 40),
                    _look[AddonColour.Surface], _look[AddonColour.Surface], _look.LiveTint(AddonColour.Surface, 210));
            Brushes(b, "Foreground", _look[AddonColour.Body], _look[AddonColour.Title], _look[AddonColour.Title],
                    _look[AddonColour.Title], _look[AddonColour.Title], _look[AddonColour.Title]);
            Brushes(b, "BorderBrush", clear, clear, clear,
                    _look[AddonColour.Hairline], _look[AddonColour.Hairline], _look[AddonColour.Hairline]);
        }
        AutomationProperties.SetName(b, label);
        b.Click += (_, _) => click();
        return b;
    }

    /// <summary>
    /// A toggle token with its icon (Finder / Photos style): outlined when off,
    /// an accent tint and accent outline when on, never colour alone. Icons are
    /// Segoe Fluent at 12, centred on the 13 pt label.
    /// </summary>
    private ToggleButton Token(string label, string glyph, Action click, bool hc)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        row.Children.Add(new FontIcon
        {
            Glyph = glyph,
            FontFamily = IconFont,
            FontSize = 12,
            VerticalAlignment = VerticalAlignment.Center,
        });
        row.Children.Add(new TextBlock
        {
            Text = label,
            FontFamily = _look.Font,
            FontSize = 13,
            VerticalAlignment = VerticalAlignment.Center,
        });
        var b = new ToggleButton
        {
            Content = row,
            Padding = new Thickness(10, 2, 12, 3),
            MinHeight = 0,
            MinWidth = 0,
            Height = 26,
            CornerRadius = new CornerRadius(13),
            BorderThickness = new Thickness(1),
        };
        if (!hc)
        {
            var clear = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            Brushes(b, "Background", clear, _look.LiveTint(AddonColour.Body, 26), _look.LiveTint(AddonColour.Body, 40),
                    _look.LiveTint(AddonColour.Accent, 44), _look.LiveTint(AddonColour.Accent, 64),
                    _look.LiveTint(AddonColour.Accent, 88));
            Brushes(b, "Foreground", _look[AddonColour.Body], _look[AddonColour.Title], _look[AddonColour.Title],
                    _look[AddonColour.Title], _look[AddonColour.Title], _look[AddonColour.Title]);
            Brushes(b, "BorderBrush", _look[AddonColour.Hairline], _look[AddonColour.Hairline], _look[AddonColour.Hairline],
                    _look[AddonColour.Accent], _look[AddonColour.Accent], _look[AddonColour.Accent]);
        }
        AutomationProperties.SetName(b, label);
        b.Click += (_, _) => click();
        return b;
    }

    /// <summary>ToggleButton lightweight styling: rest, pointer over, pressed, then the checked three.</summary>
    private static void Brushes(ToggleButton b, string part, Brush rest, Brush over, Brush pressed,
                                Brush on, Brush onOver, Brush onPressed)
    {
        b.Resources[$"ToggleButton{part}"] = rest;
        b.Resources[$"ToggleButton{part}PointerOver"] = over;
        b.Resources[$"ToggleButton{part}Pressed"] = pressed;
        b.Resources[$"ToggleButton{part}Checked"] = on;
        b.Resources[$"ToggleButton{part}CheckedPointerOver"] = onOver;
        b.Resources[$"ToggleButton{part}CheckedPressed"] = onPressed;
    }

    /// <summary>Dimmed but still hoverable and focusable, so its tooltip can say why.</summary>
    private static void SetAvailable(ToggleButton b, bool available) => b.Opacity = available ? 1 : 0.4;

    // Left / Right walk a chip group, like a toolbar; Tab moves between groups.
    private static void WireArrows(ToggleButton[] group)
    {
        for (int i = 0; i < group.Length; ++i)
        {
            int at = i;
            group[i].KeyDown += (_, e) =>
            {
                int step = e.Key == VirtualKey.Right ? 1 : e.Key == VirtualKey.Left ? -1 : 0;
                if (step == 0) return;
                for (int n = at + step; n >= 0 && n < group.Length; n += step)
                {
                    if (!group[n].IsEnabled) continue;
                    group[n].Focus(FocusState.Keyboard);
                    break;
                }
                e.Handled = true;
            };
        }
    }

    // ---- what to find (audio, 2026-09-27) ---------------------------------------------------

    // Segoe Fluent Icons (Segoe MDL2 Assets on Windows 10): Photo, Volume, Message.
    private const string GlyphPicture = "";
    private const string GlyphSound = "";
    private const string GlyphSpeech = "";
    private static FontFamily? _iconFont;
    private static FontFamily IconFont => _iconFont ??= new FontFamily("Segoe Fluent Icons, Segoe MDL2 Assets");

    private const MvAiKinds AllFinds = MvAiKinds.FindPictures | MvAiKinds.FindSounds | MvAiKinds.FindSpeech;

    // What can be matched: sounds and speech need the Audio piece.
    private MvAiKinds AvailableFinds => AudioReady ? AllFinds : MvAiKinds.FindPictures;

    // The ones shown on: every available one when none is chosen (the stored
    // "all"), else the chosen ones. The row reads "all on" at rest.
    private MvAiKinds Matching
    {
        get
        {
            MvAiKinds chosen = _find & AvailableFinds;
            return chosen == 0 ? AvailableFinds : chosen;
        }
    }

    private void ToggleFind(MvAiKinds bit)
    {
        MvAiKinds available = AvailableFinds;
        MvAiKinds on = Matching;
        if ((available & bit) == 0 || on == bit)
        {
            // Not installed, or the last one on (a search matches something):
            // the click changes nothing; the tooltip says why.
            ShowFind();
            return;
        }
        on ^= bit;
        // All available on is the same search as none: store it as none.
        _find = on == available ? 0 : on;
        ShowFind();
        RunQuery(quiet: false);
    }

    private void ShowFind()
    {
        MvAiKinds available = AvailableFinds, on = Matching;
        (MvAiKinds Bit, string Tip)[] finds =
        {
            (MvAiKinds.FindPictures, "Match what is in the picture or the video frame."),
            (MvAiKinds.FindSounds, "Match what you hear in videos: “dog barking”, “applause”."),
            (MvAiKinds.FindSpeech, "Match words said in videos."),
        };
        for (int i = 0; i < 3; ++i)
        {
            bool can = (available & finds[i].Bit) != 0;
            bool isOn = (on & finds[i].Bit) != 0;
            _findChips[i].IsChecked = isOn;
            SetAvailable(_findChips[i], can);
            string tip = !can
                ? "Needs the Audio piece: install it in Settings → Local search to find videos by what you hear."
                : isOn && on == finds[i].Bit ? finds[i].Tip + " At least one stays on."
                : isOn ? finds[i].Tip + " On: click to leave it out."
                : finds[i].Tip + " Off: click to include it.";
            ToolTipService.SetToolTip(_findChips[i], tip);
            AutomationProperties.SetHelpText(_findChips[i], tip);
        }
    }

    private bool AudioReady => _chrome.StatusValid && (_chrome.Status.Flags & MvAiStatus.FlagAudioReady) != 0;

    // Without the Audio piece every search is a picture search: no row, no bits.
    private MvAiKinds SearchKinds => AudioReady ? _kinds | _find : _kinds;

    // ---- showing ---------------------------------------------------------------------------

    internal void Present(AiChrome.SimilarTo? similar)
    {
        if (similar is not null)
        {
            _similar = similar;
            _settingText = true;
            _query.Text = "";
            _settingText = false;
        }
        else if (_similar is not null && !_visible)
        {
            // Ctrl+F is a text search; a closed "similar" does not come back.
            ClearSimilar(runQuery: false);
        }
        UpdateSimilarChip();
        if (_chrome.Folder is null && _scope != MvAiScope.All) SetScope(MvAiScope.All, run: false);
        UpdateScopeAvailability();

        AppWindow.MoveAndResize(Native.CentreOver(_chrome.Host.MainWindow, 940, 660));
        if (!_visible)
        {
            _visible = true;
            AppWindow.Show();
            Activate();
            _look.PanelIn(_card);
            _poll.Start();
        }
        else
        {
            Activate();
        }
        _chrome.ReadStatus();  // fresh counts: what was indexed while it was closed
        OnStatus();
        FocusQuery();
        bool grew = _chrome.StatusValid && _chrome.Status.FramesIndexed != _lastRunFrames;
        if (_similar is not null || (_query.Text.Trim().Length > 0 && _pending == 0 && (_tiles.Count == 0 || grew)))
        {
            // Closed while it indexed: the same words answer from the grown index.
            RunQuery(quiet: _tiles.Count > 0);
        }
        else if (_query.Text.Trim().Length == 0 && _similar is null)
        {
            ShowStart();
        }
    }

    private void Hide()
    {
        if (!_visible) return;
        _visible = false;
        _startedIndexing = null;
        _poll.Stop();
        _debounce.Stop();
        _look.PanelOut(_card, () =>
        {
            if (_visible) return;  // shown again inside the fade
            AppWindow.Hide();
            Hidden?.Invoke();
        });
    }

    public void Dispose()
    {
        _disposing = true;
        _poll.Stop();
        _debounce.Stop();
        _chrome.StatusChanged -= OnStatus;
        _chrome.ReleaseSearch(_pending);
        _chrome.ReleaseSearch(_search);
        _pending = _search = 0;
        try { Close(); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
    }

    private void FocusQuery()
    {
        _query.Focus(FocusState.Programmatic);
        _query.SelectAll();  // the previous query, pre-selected: type to replace it
    }

    // ---- query, scope, kinds --------------------------------------------------------------

    private void OnQueryChanged()
    {
        if (_settingText) return;
        _startedIndexing = null;
        if (_similar is not null && _query.Text.Length > 0) ClearSimilar(runQuery: false);
        _openWhenReady = null;
        _debounce.Stop();
        _debounce.Start();
    }

    private void ClearSimilar(bool runQuery)
    {
        _similar = null;
        UpdateSimilarChip();
        if (runQuery)
        {
            FocusQuery();
            RunQuery(quiet: false);
        }
    }

    private void UpdateSimilarChip()
    {
        bool on = _similar is not null;
        _similarChip.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
        _similarText.Text = on ? $"Similar to {_similar!.Value.Name}" +
                                 (_similar.Value.PtsMs >= 0 ? $" at {Look.Moment(_similar.Value.PtsMs)}" : "") : "";
        _query.PlaceholderText = on
            ? "Or describe something else"
            : "Describe a photo or a moment — “dog on a beach”";
    }

    private void SetScope(MvAiScope scope, bool run = true)
    {
        if (scope != MvAiScope.All && _chrome.Folder is null) scope = MvAiScope.All;
        bool changed = scope != _scope;
        _scope = scope;
        // Re-checked every time: a click on the chosen segment unchecks it.
        for (int i = 0; i < 3; ++i) _scopeChips[i].IsChecked = i == (int)scope;
        if (run && changed) RunQuery(quiet: false);
    }

    private void UpdateScopeAvailability()
    {
        // Without a folder the first two are dimmed, not disabled, so their
        // tooltip can still say why; SetScope keeps the choice on Everywhere.
        bool folder = _chrome.Folder is not null;
        string name = folder ? System.IO.Path.GetFileName(_chrome.Folder!.TrimEnd('\\', '/')) : "";
        for (int i = 0; i < 2; ++i)
        {
            SetAvailable(_scopeChips[i], folder);
            ToolTipService.SetToolTip(_scopeChips[i], folder
                ? $"{_scopeTips[i]} (“{name}”)"
                : "Open a folder to search just that folder.");
        }
    }

    private void SetKinds(MvAiKinds kinds, bool run = true)
    {
        bool changed = kinds != _kinds;
        _kinds = kinds;
        _kindChips[0].IsChecked = kinds == MvAiKinds.All;
        _kindChips[1].IsChecked = kinds == MvAiKinds.Photos;
        _kindChips[2].IsChecked = kinds == MvAiKinds.Videos;
        if (run && changed) RunQuery(quiet: false);
    }

    private string? ScopeDir => _scope == MvAiScope.All ? null : _chrome.Folder;

    // ---- searching --------------------------------------------------------------------------

    private void RunQuery(bool quiet)
    {
        _debounce.Stop();
        string q = _query.Text.Trim();
        if (_similar is null && q.Length == 0)
        {
            _chrome.ReleaseSearch(_pending);
            _pending = 0;
            SetTiles(new List<ResultTile>(), ulong.MinValue);
            ShowStart();
            return;
        }
        ulong id;
        try
        {
            id = _similar is AiChrome.SimilarTo s
                ? _api.SearchSimilar(s.Path, s.PtsMs, ScopeDir, _scope, SearchKinds)
                : _api.SearchText(q, ScopeDir, _scope, SearchKinds);
        }
        catch (MediaViewerException)
        {
            ShowEmpty("Search is not available right now.", "Local search may still be getting ready. Try again in a moment.");
            return;
        }
        if (_pending != 0) _chrome.ReleaseSearch(_pending);
        _pending = id;
        _lastRunTick = Environment.TickCount64;
        _lastRunFrames = _chrome.StatusValid ? _chrome.Status.FramesIndexed : 0;
        if (!quiet && _tiles.Count == 0) _count.Text = "Searching…";
    }

    internal void OnSearchDone(ulong id, MvStatus status, long count)
    {
        if (id != _pending)
        {
            if (id != _search) _chrome.ReleaseSearch(id);  // superseded while it ran
            return;
        }
        _pending = 0;
        if (status != MvStatus.Ok)
        {
            ShowEmpty("Search did not finish.", "Try again, or change the words.");
            return;
        }
        int n = (int)Math.Clamp(count, 0, MaxResults);
        AiApi api = _api;
        _ = Task.Run(() =>
        {
            // result_path is cheap, but a list of 240 is still not UI-thread work.
            var tiles = new List<ResultTile>(n);
            try
            {
                for (uint i = 0; i < n; ++i)
                {
                    MvAiResult r = api.ResultAt(id, i);
                    tiles.Add(new ResultTile
                    {
                        Index = (int)i,
                        Path = api.ResultPath(id, i),
                        PtsMs = r.Kind == MvAiKinds.Videos ? r.PtsMs : -1,
                        More = r.MoreInClip,
                        Video = r.Kind == MvAiKinds.Videos,
                        Match = r.Match,
                    });
                }
            }
            catch (MediaViewerException) { }
            DispatcherQueue.TryEnqueue(() => ApplyResults(id, tiles, count));
        });
    }

    private void ApplyResults(ulong id, List<ResultTile> tiles, long total)
    {
        if (_disposing) return;
        if (_search != 0 && _search != id) _chrome.ReleaseSearch(_search);
        _search = id;
        SetTiles(tiles, id);
        if (tiles.Count == 0)
        {
            ShowNothing();
        }
        else
        {
            _empty.Visibility = Visibility.Collapsed;
            _gridHost.Visibility = Visibility.Visible;
            _count.Text = total > tiles.Count ? $"best {tiles.Count} of {total:N0}"
                        : tiles.Count == 1 ? "1 result" : $"{tiles.Count} results";
        }
        if (_openWhenReady is bool gallery && tiles.Count > 0)
        {
            _openWhenReady = null;
            OpenResults(gallery);
        }
    }

    private void SetTiles(List<ResultTile> tiles, ulong search)
    {
        _ = search;
        // A re-run as the index grows keeps what was already on screen still:
        // only new tiles animate in, and the selection stays on its item.
        var old = new Dictionary<(string, long), ResultTile>();
        foreach (ResultTile t in _tiles) old.TryAdd(t.Key, t);
        string? selectedPath = _sel >= 0 && _sel < _tiles.Count ? _tiles[_sel].Path : null;
        long selectedPts = _sel >= 0 && _sel < _tiles.Count ? _tiles[_sel].PtsMs : -1;
        foreach (ResultTile t in tiles)
        {
            if (old.TryGetValue(t.Key, out ResultTile? was))
            {
                t.Revealed = true;
                t.Thumb = was.Thumb;
                if ((t.Match & MvAiMatch.Speech) != 0 && was.Snippet is not null) t.Snippet = was.Snippet;
            }
            else if (_thumbs.TryGet(t.Key, out ImageSource? cached))
            {
                t.Thumb = cached;
            }
        }
        ++_batch;
        _tiles = tiles;
        _hint.Visibility = _tiles.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        _repeater.ItemsSource = _tiles;
        int keep = selectedPath is null ? -1 : _tiles.FindIndex(t => t.Path == selectedPath && t.PtsMs == selectedPts);
        _sel = keep >= 0 ? keep : (_gridHost.FocusState != FocusState.Unfocused && _tiles.Count > 0 ? 0 : -1);
        if (_tiles.Count == 0) _count.Text = "";
        MoveRing(animate: false);
    }

    // ---- empty states ---------------------------------------------------------------------------

    private void ShowStart()
    {
        if (_startedIndexing is not null)
        {
            ShowIndexing();
            return;
        }
        // The folder offer lives here, never as a banner over the viewer.
        if (_chrome.Folder is not null && _chrome.Coverage == 0 && _scope != MvAiScope.All)
        {
            ShowIndexOffer();
            return;
        }
        ShowEmpty("Describe what you are looking for.",
                  "“guy on a skateboard”, “birthday cake”, “sunset over water”. Photos and moments in videos both count." +
                  (AudioReady ? " Or try a sound — “dog barking” — or words someone said." : ""));
    }

    private void ShowNothing()
    {
        if (_startedIndexing is not null && _similar is null)
        {
            ShowIndexing();
            return;
        }
        if (_scope != MvAiScope.All && _chrome.Folder is not null && _chrome.Coverage == 0)
        {
            ShowIndexOffer();
            return;
        }
        string q = _query.Text.Trim();
        bool indexing = _chrome.StatusValid && _chrome.Status.State is MvAiState.Indexing or MvAiState.Yielding;
        if (_similar is not null)
        {
            ShowEmpty("Nothing similar found here.",
                      indexing ? "Results appear as the index grows." : "Try Look in: Everywhere.");
            return;
        }
        ShowEmpty($"Nothing matches “{q}”.",
                  "Try describing what's in the picture: “dog on a beach”." +
                  (AudioReady ? " Or try a sound — “dog barking” — or words someone said." : "") +
                  (indexing ? " Results appear as the index grows." : ""));
    }

    private void ShowIndexOffer()
    {
        _indexOffered = true;
        _gridHost.Visibility = Visibility.Collapsed;
        _empty.Children.Clear();
        _count.Text = "";
        string name = System.IO.Path.GetFileName(_chrome.Folder!.TrimEnd('\\', '/'));
        _empty.Children.Add(Centre(_look.Text($"“{name}” is not indexed yet.", 18, AddonColour.Title)));
        _empty.Children.Add(Centre(_look.Text(
            "Indexing runs in the background at low priority and pauses while you watch or pan: " +
            "you can close this panel and keep viewing. You can search as soon as the first files are done.", 14)));
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, HorizontalAlignment = HorizontalAlignment.Center };
        Button here = _look.Button("Index this folder", () => IndexFolder(false), accent: true);
        buttons.Children.Add(here);
        buttons.Children.Add(_look.Button("Index this folder and subfolders", () => IndexFolder(true)));
        _empty.Children.Add(buttons);
        _empty.Visibility = Visibility.Visible;
        FadeIn(_empty);
    }

    private void ShowEmpty(string title, string detail)
    {
        _indexOffered = false;
        _gridHost.Visibility = Visibility.Collapsed;
        _empty.Children.Clear();
        _count.Text = "";
        _empty.Children.Add(Centre(_look.Text(title, 18, AddonColour.Title)));
        _empty.Children.Add(Centre(_look.Text(detail, 14)));
        _empty.Visibility = Visibility.Visible;
        FadeIn(_empty);
    }

    private static TextBlock Centre(TextBlock t)
    {
        t.TextAlignment = TextAlignment.Center;
        t.HorizontalAlignment = HorizontalAlignment.Center;
        return t;
    }

    private void FadeIn(UIElement e)
    {
        e.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(180) };
        e.Opacity = 0;
        DispatcherQueue.TryEnqueue(() => e.Opacity = 1);
    }

    private void IndexFolder(bool recursive)
    {
        string? dir = _chrome.Folder;
        if (dir is null) return;
        try { _api.IndexFolder(dir, recursive); }
        catch (MediaViewerException)
        {
            ShowEmpty("This folder could not be added.", "Check that it is still there, then try again.");
            return;
        }
        _chrome.RefreshCoverage();
        _chrome.ReadStatus();
        _startedIndexing = recursive;
        ShowIndexing();
        FocusQuery();
        if (_query.Text.Trim().Length > 0) RunQuery(quiet: false);
    }

    /// <summary>
    /// Just asked to index: say plainly that it carries on without the panel,
    /// and offer the way out. Shown until typing, a result, or the panel closes.
    /// </summary>
    private void ShowIndexing()
    {
        bool recursive = _startedIndexing ?? false;
        _indexOffered = false;
        _gridHost.Visibility = Visibility.Collapsed;
        _empty.Children.Clear();
        _count.Text = "";
        string name = _chrome.Folder is string f ? System.IO.Path.GetFileName(f.TrimEnd('\\', '/')) : "this folder";
        _empty.Children.Add(Centre(_look.Text(recursive
            ? $"Indexing “{name}” and its subfolders in the background"
            : $"Indexing “{name}” in the background", 18, AddonColour.Title)));
        string q = _query.Text.Trim();
        _empty.Children.Add(Centre(_look.Text(
            "You can close this and carry on: indexing continues on its own, at low priority, and pauses " +
            "while you watch or pan. Its progress is in the command bar. " +
            (q.Length == 0 ? "Search whenever you like: results appear as it goes."
                           : $"Results for “{q}” appear here as it goes."), 14)));
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, HorizontalAlignment = HorizontalAlignment.Center };
        Button done = _look.Button("Continue in background", Hide, accent: true);
        ToolTipService.SetToolTip(done, "Close the panel (Esc). Indexing carries on; Ctrl+F opens search again.");
        buttons.Children.Add(done);
        buttons.Children.Add(_look.Button("Search now", FocusQuery));
        _empty.Children.Add(buttons);
        _empty.Visibility = Visibility.Visible;
        FadeIn(_empty);
    }

    /// <summary>
    /// Settings → Precision changed: an open description answers again (each
    /// search reads it as it starts; nothing is re-indexed). Similar does not
    /// read it.
    /// </summary>
    internal void OnPrecisionChanged()
    {
        if (_similar is not null || _query.Text.Trim().Length == 0) return;
        RunQuery(quiet: true);
    }

    internal void OnFolderChanged()
    {
        UpdateScopeAvailability();
        if (_visible && _tiles.Count == 0 && _pending == 0 && _query.Text.Trim().Length == 0) ShowStart();
    }

    internal void OnRootsChanged()
    {
        if (!_visible) return;
        if (_indexOffered && _chrome.Coverage != 0 && _tiles.Count == 0) ShowStart();
    }

    // ---- status ----------------------------------------------------------------------------------

    private void OnStatus()
    {
        if (!_chrome.StatusValid)
        {
            _footerText.Text = "Local search is not running.";
            _footerRing.IsActive = false;
            _badge.Visibility = Visibility.Collapsed;
            return;
        }
        MvAiStatus s = _chrome.Status;
        _footerText.Text = Look.StatusLine(s);
        bool audio = (s.Flags & MvAiStatus.FlagAudioReady) != 0;
        if (audio != _findAudio)
        {
            _findAudio = audio;
            ShowFind();
        }
        // The ring spins only while indexing.
        bool busy = Look.Busy(s);
        _footerRing.IsActive = busy && _visible;
        _footerRing.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
        string? why = Look.FallbackReason(s);
        _badgeText.Text = why is null ? Look.ComputeBadge(s) : $"{Look.ComputeBadge(s)} · {why}";
        _badge.Visibility = Visibility.Visible;
        ToolTipService.SetToolTip(_badge, s.ModelText.Length > 0 ? s.ModelText : null);
        bool paused = s.State == MvAiState.Paused;
        _indexAnyway.Visibility = Look.OnBattery(s) ? Visibility.Visible : Visibility.Collapsed;
        _pause.Content = paused ? "Resume" : "Pause";
        _pause.Visibility = s.State == MvAiState.Idle && s.AssetsDone >= s.AssetsTotal ? Visibility.Collapsed : Visibility.Visible;

        // Results appear as the index grows: re-ask now and then while it does.
        if (_visible && _pending == 0 && s.State == MvAiState.Indexing &&
            (_similar is not null || _query.Text.Trim().Length > 0) &&
            Environment.TickCount64 - _lastRunTick > 2500 &&
            s.FramesIndexed > _lastRunFrames + Math.Max(200, _lastRunFrames / 10))
        {
            RunQuery(quiet: true);
        }
    }

    private void TogglePause()
    {
        bool paused = _chrome.StatusValid && _chrome.Status.State == MvAiState.Paused;
        try { _api.Pause(!paused); }
        catch (MediaViewerException) { }
        _chrome.ReadStatus();
    }

    private void IndexAnyway()
    {
        try { _api.IndexAnyway(); }
        catch (MediaViewerException) { }
        _chrome.ReadStatus();
    }

    internal void OnThemeChanged()
    {
        _similarChip.Background = _look.Tint(AddonColour.Accent, 48);
        _card.Background = _look.Tint(AddonColour.Canvas, 110);
    }

    // ---- opening results ------------------------------------------------------------------------

    private void OpenResults(bool gallery)
    {
        if (_tiles.Count == 0 || _search == 0) return;
        int select = Math.Clamp(_sel, 0, _tiles.Count - 1);
        string title = _similar is AiChrome.SimilarTo s ? $"Similar to {s.Name}" : _query.Text.Trim();
        if (title.Length == 0) title = "Search results";
        _chrome.SetActiveSearch(_search, _tiles.Where(t => t.Video).Select(t => t.Path));
        var paths = _tiles.Select(t => t.Path).ToList();
        var moments = _tiles.Select(t => t.Video ? t.PtsMs : -1L).ToList();
        Hide();
        _chrome.Host.OpenList(title, paths, moments, select, gallery);
    }

    // ---- keyboard ---------------------------------------------------------------------------------

    private static bool Down(VirtualKey key) =>
        InputKeyboardSource.GetKeyStateForCurrentThread(key).HasFlag(CoreVirtualKeyStates.Down);

    private void OnPreviewKeyDown(object sender, KeyRoutedEventArgs e)
    {
        object? focused = FocusManager.GetFocusedElement(_root.XamlRoot);
        bool inQuery = ReferenceEquals(focused, _query);
        bool inGrid = ReferenceEquals(focused, _gridHost);
        bool ctrl = Down(VirtualKey.Control);
        switch (e.Key)
        {
            case VirtualKey.Escape:
                if (inGrid) FocusQuery();
                else Hide();
                e.Handled = true;
                return;
            case VirtualKey.Enter when inQuery || inGrid:
                if (_debounce.IsRunning || _pending != 0)
                {
                    // Typed and pressed Enter at once: open when the answer lands.
                    _openWhenReady = ctrl;
                    if (_debounce.IsRunning) RunQuery(quiet: false);
                }
                else
                {
                    OpenResults(gallery: ctrl);
                }
                e.Handled = true;
                return;
            case VirtualKey.F when ctrl:
                FocusQuery();
                e.Handled = true;
                return;
            case VirtualKey.Down when inQuery:
                if (_tiles.Count > 0)
                {
                    _gridHost.Focus(FocusState.Keyboard);
                    Select(Math.Max(0, _sel));
                }
                e.Handled = true;
                return;
            case VirtualKey.Back when inQuery && _similar is not null && _query.Text.Length == 0:
                ClearSimilar(runQuery: true);
                e.Handled = true;
                return;
        }
        if (!inGrid || _tiles.Count == 0) return;
        int cols = Columns();
        int rows = Math.Max(1, (int)(_scroll.ViewportHeight / (TileH + Gap)));
        int next = _sel;
        switch (e.Key)
        {
            case VirtualKey.Left: next = _sel - 1; break;
            case VirtualKey.Right: next = _sel + 1; break;
            case VirtualKey.Up:
                if (_sel < cols)
                {
                    FocusQuery();
                    e.Handled = true;
                    return;
                }
                next = _sel - cols;
                break;
            case VirtualKey.Down: next = Math.Min(_tiles.Count - 1, _sel + cols); break;
            case VirtualKey.Home: next = 0; break;
            case VirtualKey.End: next = _tiles.Count - 1; break;
            case VirtualKey.PageUp: next = Math.Max(_sel % cols, _sel - rows * cols); break;
            case VirtualKey.PageDown: next = Math.Min(_tiles.Count - 1, _sel + rows * cols); break;
            default: return;
        }
        e.Handled = true;
        if (next >= 0 && next < _tiles.Count) Select(next);
    }

    private int Columns()
    {
        double width = Math.Max(0, _scroll.ViewportWidth - 2 * GridPad);
        return Math.Max(1, (int)((width + Gap) / (TileW + Gap)));
    }

    private void Select(int index)
    {
        _sel = Math.Clamp(index, 0, Math.Max(0, _tiles.Count - 1));
        MoveRing(animate: true);
        BringIntoView(_sel);
        if (_sel < _tiles.Count)
        {
            ResultTile t = _tiles[_sel];
            AutomationProperties.SetName(_gridHost,
                t.Video ? $"{t.Name}, {Look.Moment(t.PtsMs)}" : t.Name);
        }
    }

    private (double X, double Y) TileOrigin(int index)
    {
        int cols = Columns();
        return (index % cols * (TileW + Gap), index / cols * (TileH + Gap));
    }

    // The selection ring glides between tiles (a compositor-side translation).
    private void MoveRing(bool animate)
    {
        bool show = _sel >= 0 && _sel < _tiles.Count && _gridHost.FocusState != FocusState.Unfocused;
        _ring.Opacity = show ? 1 : 0;
        if (!show) return;
        (double x, double y) = TileOrigin(_sel);
        _ring.TranslationTransition = animate && _look.Motion
            ? new Vector3Transition { Duration = TimeSpan.FromMilliseconds(160) }
            : null;
        _ring.Translation = new Vector3((float)(x - 4), (float)(y - 4), 0);
    }

    private void BringIntoView(int index)
    {
        (_, double y) = TileOrigin(index);
        y += GridPad;
        double top = _scroll.VerticalOffset, bottom = top + _scroll.ViewportHeight;
        if (y - 8 < top) _scroll.ChangeView(null, Math.Max(0, y - 12), null, !_look.Motion);
        else if (y + TileH + 8 > bottom) _scroll.ChangeView(null, y + TileH + 12 - _scroll.ViewportHeight, null, !_look.Motion);
    }

    // ---- tiles --------------------------------------------------------------------------------------

    private void RequestThumb(ResultTile tile)
    {
        if (tile.ThumbRequested || tile.Thumb is not null) return;
        tile.ThumbRequested = true;
        ulong search = _search;
        AiApi api = _api;
        _ = Task.Run(async () =>
        {
            await _thumbGate.WaitAsync().ConfigureAwait(false);
            string? path = null;
            try { path = api.ResultThumb(search, (uint)tile.Index); }
            catch (MediaViewerException) { }
            finally { _thumbGate.Release(); }
            if (path is null || path.Length == 0) return;
            DispatcherQueue.TryEnqueue(() =>
            {
                if (search != _search) return;
                try
                {
                    // BitmapImage decodes off the UI thread; the size bounds it.
                    var image = new BitmapImage { DecodePixelWidth = (int)TileW * 2 };
                    image.UriSource = new Uri(path);
                    tile.Thumb = image;
                    _thumbs.Put(tile.Key, image);
                    if (tile.View is Image view) view.Source = image;
                }
                catch (Exception ex) when (ex is UriFormatException or ArgumentException) { }
            });
        });
    }

    private FrameworkElement BuildTile(ResultTile tile)
    {
        var image = new Image
        {
            Width = TileW,
            Height = ImageH,
            Stretch = Stretch.UniformToFill,
            Source = tile.Thumb,
        };
        tile.View = image;
        var layers = new Grid { Width = TileW, Height = ImageH };
        layers.Children.Add(new Border { Background = _look.Tint(AddonColour.Surface, 255) });
        layers.Children.Add(image);
        if (tile.Video)
        {
            layers.Children.Add(Badge(Look.Moment(tile.PtsMs), HorizontalAlignment.Left));
            if (tile.More > 0) layers.Children.Add(Badge($"+{tile.More} in this clip", HorizontalAlignment.Right));
        }
        if (MatchBadge(tile.Match) is Border why) layers.Children.Add(why);
        var frame = new Border
        {
            Child = layers,
            CornerRadius = new CornerRadius(8),
            Width = TileW,
            Height = ImageH,
            Shadow = new ThemeShadow(),
        };
        frame.ScaleTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(140) };
        frame.TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(140) };
        frame.CenterPoint = new Vector3((float)TileW / 2, (float)ImageH / 2, 0);
        // Hover lifts the tile: a little scale and a soft shadow.
        frame.PointerEntered += (_, _) =>
        {
            if (_look.Motion) frame.Scale = new Vector3(1.03f, 1.03f, 1);
            frame.Translation = new Vector3(0, 0, 16);
        };
        frame.PointerExited += (_, _) =>
        {
            frame.Scale = Vector3.One;
            frame.Translation = Vector3.Zero;
        };
        var caption = _look.Text(tile.Name, 12, AddonColour.Body, wrap: false);
        caption.Margin = new Thickness(2, 4, 2, 0);
        caption.MaxWidth = TileW;
        tile.Caption = caption;
        var root = new StackPanel { Width = TileW, Height = TileH };
        root.Children.Add(frame);
        root.Children.Add(caption);
        ToolTipService.SetToolTip(root, tile.Video ? $"{tile.Name} at {Look.Moment(tile.PtsMs)}" : tile.Name);
        if (tile.Snippet is string known) ShowSnippet(tile, known);
        else RequestSnippet(tile);
        root.Tapped += (_, _) =>
        {
            _sel = tile.Index;
            OpenResults(gallery: Down(VirtualKey.Control));
        };
        return root;
    }

    // Why a tile matched: picture, sound, speech (Segoe Fluent Icons glyphs),
    // top-right over the thumbnail. Nothing when the pack does not say.
    private Border? MatchBadge(MvAiMatch match)
    {
        if (match == MvAiMatch.None) return null;
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5 };
        var names = new List<string>(3);
        void Add(MvAiMatch bit, string glyph, string name)
        {
            if ((match & bit) == 0) return;
            row.Children.Add(new FontIcon
            {
                Glyph = glyph,
                FontFamily = IconFont,
                FontSize = 12,
                Foreground = new SolidColorBrush(Microsoft.UI.Colors.White),
            });
            names.Add(name);
        }
        Add(MvAiMatch.Picture, GlyphPicture, "picture");
        Add(MvAiMatch.Sound, GlyphSound, "sound");
        Add(MvAiMatch.Speech, GlyphSpeech, "speech");
        var badge = new Border
        {
            Child = row,
            Background = new SolidColorBrush(Windows.UI.Color.FromArgb(170, 0, 0, 0)),
            CornerRadius = new CornerRadius(4),
            Padding = new Thickness(5, 3, 5, 3),
            Margin = new Thickness(6),
            HorizontalAlignment = HorizontalAlignment.Right,
            VerticalAlignment = VerticalAlignment.Top,
        };
        string text = "Matched by " + string.Join(" and ", names);
        ToolTipService.SetToolTip(badge, text);
        AutomationProperties.SetName(badge, text);
        return badge;
    }

    // Speech: the words that matched, one line under the tile instead of the
    // file name (which stays in the tooltip). [worker-thread] in the pack.
    private void RequestSnippet(ResultTile tile)
    {
        if (tile.SnippetRequested || (tile.Match & MvAiMatch.Speech) == 0) return;
        tile.SnippetRequested = true;
        ulong search = _search;
        AiApi api = _api;
        _ = Task.Run(async () =>
        {
            await _thumbGate.WaitAsync().ConfigureAwait(false);
            string snippet = "";
            try { snippet = api.ResultSnippet(search, (uint)tile.Index); }
            catch (MediaViewerException) { }
            finally { _thumbGate.Release(); }
            if (snippet.Length == 0) return;
            DispatcherQueue.TryEnqueue(() =>
            {
                if (search != _search) return;
                tile.Snippet = snippet;
                ShowSnippet(tile, snippet);
            });
        });
    }

    private void ShowSnippet(ResultTile tile, string snippet)
    {
        if (tile.Caption is not TextBlock caption || snippet.Length == 0) return;
        caption.Inlines.Clear();
        caption.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = "“" });
        // The query's words, emphasised where they appear (whole-word, any case).
        var words = new HashSet<string>(
            _query.Text.Split(' ', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
                .Select(w => w.Trim('"', '“', '”', ',', '.', '?', '!').ToLowerInvariant())
                .Where(w => w.Length > 1),
            StringComparer.Ordinal);
        foreach (string piece in SplitWords(snippet))
        {
            string key = piece.Trim('"', '“', '”', ',', '.', '?', '!', ';', ':').ToLowerInvariant();
            var run = new Microsoft.UI.Xaml.Documents.Run { Text = piece };
            if (key.Length > 1 && words.Contains(key))
            {
                run.FontWeight = Microsoft.UI.Text.FontWeights.SemiBold;
                run.Foreground = _look[AddonColour.Title];
            }
            caption.Inlines.Add(run);
        }
        caption.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = "”" });
        caption.FontStyle = Windows.UI.Text.FontStyle.Italic;
        ToolTipService.SetToolTip(caption, $"{tile.Name} at {Look.Moment(tile.PtsMs)}: “{snippet}”");
    }

    // Words with their trailing spaces kept, so the runs read as the original line.
    private static IEnumerable<string> SplitWords(string text)
    {
        int start = 0;
        for (int i = 0; i < text.Length; ++i)
        {
            if (text[i] != ' ') continue;
            yield return text[start..(i + 1)];
            start = i + 1;
        }
        if (start < text.Length) yield return text[start..];
    }

    private Border Badge(string text, HorizontalAlignment side) => new()
    {
        Child = new TextBlock
        {
            Text = text,
            FontFamily = _look.Font,
            FontSize = 12,
            Foreground = new SolidColorBrush(Microsoft.UI.Colors.White),
        },
        Background = new SolidColorBrush(Windows.UI.Color.FromArgb(170, 0, 0, 0)),
        CornerRadius = new CornerRadius(4),
        Padding = new Thickness(5, 1, 5, 2),
        Margin = new Thickness(6),
        HorizontalAlignment = side,
        VerticalAlignment = VerticalAlignment.Bottom,
    };

    // The first 16 new tiles of a batch fade and rise in, 25 ms apart; a fade
    // alone under reduce motion. Compositor-side: input is never held.
    private void Reveal(UIElement element, ResultTile tile)
    {
        if (tile.Revealed) return;
        tile.Revealed = true;
        if (tile.Index >= StaggerCount) return;
        Compositor compositor = Compositor;
        TimeSpan delay = TimeSpan.FromMilliseconds(tile.Index * StaggerMs);
        CubicBezierEasingFunction ease = compositor.CreateCubicBezierEasingFunction(new Vector2(0.2f, 0.8f), new Vector2(0.2f, 1f));
        ScalarKeyFrameAnimation fade = compositor.CreateScalarKeyFrameAnimation();
        fade.Target = "Opacity";
        fade.InsertKeyFrame(0, 0);
        fade.InsertKeyFrame(1, 1, ease);
        fade.Duration = TimeSpan.FromMilliseconds(220);
        fade.DelayTime = delay;
        fade.DelayBehavior = AnimationDelayBehavior.SetInitialValueBeforeDelay;
        element.StartAnimation(fade);
        if (!_look.Motion) return;
        Vector3KeyFrameAnimation rise = compositor.CreateVector3KeyFrameAnimation();
        rise.Target = "Translation";
        rise.InsertKeyFrame(0, new Vector3(0, 12, 0));
        rise.InsertKeyFrame(1, Vector3.Zero, ease);
        rise.Duration = TimeSpan.FromMilliseconds(220);
        rise.DelayTime = delay;
        rise.DelayBehavior = AnimationDelayBehavior.SetInitialValueBeforeDelay;
        element.StartAnimation(rise);
    }

    private sealed class TileFactory(SearchWindow owner) : IElementFactory
    {
        public UIElement GetElement(ElementFactoryGetArgs args)
        {
            var tile = (ResultTile)args.Data;
            FrameworkElement element = owner.BuildTile(tile);
            owner.Reveal(element, tile);
            owner.RequestThumb(tile);
            return element;
        }

        public void RecycleElement(ElementFactoryRecycleArgs args)
        {
            // Not pooled: a tile is cheap, and a recycled one would carry the
            // last tile's image for a frame.
            _ = args;
        }
    }
}

/// <summary>A small LRU of decoded result thumbnails, by (path, moment).</summary>
internal sealed class ThumbCache(int capacity)
{
    private readonly Dictionary<(string, long), LinkedListNode<((string, long) Key, ImageSource Image)>> _map = new();
    private readonly LinkedList<((string, long) Key, ImageSource Image)> _order = new();

    public bool TryGet((string, long) key, out ImageSource? image)
    {
        image = null;
        if (!_map.TryGetValue(key, out var node)) return false;
        _order.Remove(node);
        _order.AddFirst(node);
        image = node.Value.Image;
        return true;
    }

    public void Put((string, long) key, ImageSource image)
    {
        if (_map.TryGetValue(key, out var node))
        {
            _order.Remove(node);
            _map.Remove(key);
        }
        _map[key] = _order.AddFirst((key, image));
        while (_map.Count > capacity && _order.Last is { } last)
        {
            _order.RemoveLast();
            _map.Remove(last.Value.Key);
        }
    }
}
