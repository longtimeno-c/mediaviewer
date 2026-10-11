// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Media;

namespace MediaViewer.Chrome;

/// <summary>
/// PR 29 (docs/design/20): the Edit workspace — one visible door to the PR 10–14 edits.
/// A fifth panel island at the top of the right column: the strip (title, tabs,
/// Undo / Reset / Original / Save copy) and, under it, the Crop or Trim pane.
/// The Colour, Info and Jobs tabs are the adjust, metadata and Jobs panes,
/// which native places under the strip. The Mac twin is EditView.swift.
/// </summary>
/// <remarks>
/// Every control sends a command the keyboard already has, and its tooltip and
/// accessible name say which key, so the pane teaches the keys rather than
/// replacing them. Native owns the workspace (shell/edit_workspace.h) and the
/// crop draft (shell/edit_session.h) and pushes what to show (SetEditView,
/// <c>chrome_edit_args</c>); nothing here edits pixels or reads a file. The
/// buttons never take focus on a click, so the crop and trim keys stay with
/// the canvas.
/// </remarks>
public static partial class IslandHost
{
    // chrome_host.h chrome_edit_args: 12 int/float words, two pointers, two lengths,
    // then the document's app list (pointer, length) and its flag.
    internal const int EditArgsSize = 88;

    private static DesktopWindowXamlSource? _editPane;
    private static bool _editPaneVisible;

    // Mirrors chrome_edit_action (chrome_host.h).
    private static class EditActions
    {
        public const int CancelCrop = 0;
        public const int OriginalOff = 1;
        public const int OriginalOn = 2;
        public const int SaveCopy = 3;
    }

    // shell::edit_tab and shell::crop_aspect values (both stable on the wire).
    private const int TabCrop = 0, TabColour = 1, TabInfo = 2, TabTrim = 3, TabJobs = 4;
    private const int SubjectNone = 0, SubjectClip = 2;
    private static readonly string[] AspectLabels = { "Free", "Original", "1:1", "4:3", "3:2", "16:9", "5:4" };

    // chrome_host.h kEditTrim*.
    private const int TrimArmedFlag = 1, TrimPreviewingFlag = 2, TrimHasMarkerFlag = 4;

    // Last view native pushed.
    private static bool _editOpen;
    private static int _editTab;
    private static int _editSubject;
    private static bool _editCropActive;
    private static int _editAspect;
    private static bool _editPortrait;
    private static float _editStraighten;
    private static int _editCount;
    private static bool _editOriginal;
    private static int _editCropW;
    private static int _editCropH;
    private static int _editTrimFlags;
    private static string _editName = "";
    private static string _editTrimLabel = "";

    // What the island was built for; anything else is updated in place, so a
    // straighten drag is never interrupted by a rebuild.
    private static string _editShape = "";
    private static bool _updatingEdit;

    private static Button? _editBarButton;
    // A PDF or DOCX (docs/design/20): "Open in <app>" and a ▾ of every app,
    // in place of the Edit button. Names come from native, default first.
    private static bool _editDocument;
    private static string[] _openApps = Array.Empty<string>();
    private static StackPanel? _openInBar;
    private static Button? _openInButton;
    private static Button? _openInMore;
    private static MenuFlyout? _openInFlyout;
    private static TextBlock? _editNameText;
    private static TextBlock? _editCountText;
    private static Button? _editUndo;
    private static Button? _editReset;
    private static Button? _editOriginalButton;
    private static TextBlock? _editCropSize;
    private static Button? _editOrient;
    private static Slider? _editStraightenSlider;
    private static TextBlock? _editStraightenText;
    private static readonly List<(Button Button, int Tab)> EditTabButtons = new();
    private static readonly List<Button> EditAspectButtons = new();

    private static void DropEditUi()
    {
        _editShape = "";
        _editNameText = null;
        _editCountText = null;
        _editUndo = null;
        _editReset = null;
        _editOriginalButton = null;
        _editCropSize = null;
        _editOrient = null;
        _editStraightenSlider = null;
        _editStraightenText = null;
        EditTabButtons.Clear();
        EditAspectButtons.Clear();
    }

    public static int ShowEditPane(IntPtr arg, int sizeBytes) => ShowPanel(
        arg, sizeBytes, _editPane, BuildEditPane,
        onShown: () => _editPaneVisible = true,
        onHidden: () =>
        {
            _editPaneVisible = false;
            DropEditUi();
        });

    /// <summary>
    /// Native pushes the workspace, the crop draft and the trim state whenever
    /// any of them changes, open or not (the bar button follows the item).
    /// In: chrome_edit_args (EditArgsSize bytes; pointers valid for the call).
    /// </summary>
    public static int SetEditView(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < EditArgsSize) return unchecked((int)0x80070057);
            _editOpen = Marshal.ReadInt32(arg, 0) != 0;
            _editTab = Marshal.ReadInt32(arg, 4);
            _editSubject = Marshal.ReadInt32(arg, 8);
            _editCropActive = Marshal.ReadInt32(arg, 12) != 0;
            _editAspect = Math.Clamp(Marshal.ReadInt32(arg, 16), 0, AspectLabels.Length - 1);
            _editPortrait = Marshal.ReadInt32(arg, 20) != 0;
            _editStraighten = BitConverter.Int32BitsToSingle(Marshal.ReadInt32(arg, 24));
            _editCount = Marshal.ReadInt32(arg, 28);
            _editOriginal = Marshal.ReadInt32(arg, 32) != 0;
            _editCropW = Marshal.ReadInt32(arg, 36);
            _editCropH = Marshal.ReadInt32(arg, 40);
            _editTrimFlags = Marshal.ReadInt32(arg, 44);
            static string Utf8(long ptr, int len) =>
                ptr == 0 || len <= 0 ? "" : Marshal.PtrToStringUTF8(checked((IntPtr)ptr), len) ?? "";
            _editName = Utf8(Marshal.ReadInt64(arg, 48), Marshal.ReadInt32(arg, 64));
            _editTrimLabel = Utf8(Marshal.ReadInt64(arg, 56), Marshal.ReadInt32(arg, 68));
            _openApps = Utf8(Marshal.ReadInt64(arg, 72), Marshal.ReadInt32(arg, 80))
                .Split('\n', StringSplitOptions.RemoveEmptyEntries);
            _editDocument = Marshal.ReadInt32(arg, 84) != 0;
            RenderEditBarButton();
            if (_editPaneVisible) RenderEdit();
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    private static bool EditIsClip => _editSubject == SubjectClip;
    private static string EditTitle => EditIsClip ? "Edit video" : "Edit image";

    // ---- the command bar's button ----------------------------------------------

    private static Button BuildEditBarButton()
    {
        // The bar's own TextButton, so it looks and behaves like Open / View
        // (see FlattenButton for why nothing here takes a custom template).
        _editBarButton = TextButton("Edit image", () => Send(Command.EditWorkspace));
        FlattenButton(_editBarButton);  // hidden with no media on the canvas
        RenderEditBarButton();
        return _editBarButton;
    }

    private static void RenderEditBarButton()
    {
        if (_editBarButton is null) return;
        string label = _editOpen ? "Done" : EditTitle;
        SetButtonText(_editBarButton, label);
        // Shown only over media: not on the empty start, not over the gallery.
        _editBarButton.Visibility = _editOpen || (_editSubject != SubjectNone && !_galleryVisible)
            ? Visibility.Visible : Visibility.Collapsed;
        _editBarButton.Background = _editOpen ? Brush(ChromeColour.PressedWash) : Brush(Colors.Transparent);
        string tip = _editOpen ? "Close the editor  Enter or Esc"
                     : EditIsClip ? "Edit video: trim, split, clip tools  Enter"
                                  : "Edit image: crop, rotate, colour, info  Enter";
        ToolTipService.SetToolTip(_editBarButton, tip);
        AutomationProperties_SetName(_editBarButton, _editOpen ? "Done editing" : EditTitle);
        RenderOpenInBar();
    }

    // A document's "Open in <app>": the default app (Enter does the same), and a
    // ▾ listing every app Explorer's Open with would offer. Shown where the
    // Edit button would be, only over a document.
    private static StackPanel BuildOpenInBar()
    {
        _openInButton = TextButton("Open in…", () => Send(Command.OpenInApp, _openApps.Length > 0 ? 0 : -1));
        FlattenButton(_openInButton);
        _openInFlyout = new MenuFlyout
        {
            ShouldConstrainToRootBounds = false,
            MenuFlyoutPresenterStyle = MenuFlyoutPresenterStyle(),
        };
        _openInFlyout.Opening += (_, _) => RefreshOpenInMenu();
        Button? more = null;
        more = TextButton("▾", () =>
        {
            if (more is not null) FlyoutBase.ShowAttachedFlyout(more);
        });
        FlattenButton(more);
        AttachBarFlyout(more, _openInFlyout);
        ToolTipService.SetToolTip(more, "Open with another app");
        AutomationProperties_SetName(more, "Open with another app");
        _openInMore = more;
        _openInBar = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 0 };
        _openInBar.Children.Add(_openInButton);
        _openInBar.Children.Add(more);
        RenderOpenInBar();
        return _openInBar;
    }

    private static void RefreshOpenInMenu()
    {
        if (_openInFlyout is null) return;
        _openInFlyout.Items.Clear();
        for (int i = 0; i < _openApps.Length; i++)
        {
            int row = i;
            _openInFlyout.Items.Add(Item("Open in " + _openApps[i], null, () => Send(Command.OpenInApp, row)));
        }
    }

    private static void RenderOpenInBar()
    {
        if (_openInBar is null || _openInButton is null || _openInMore is null) return;
        _openInBar.Visibility = _editDocument && !_editOpen && !_galleryVisible
            ? Visibility.Visible : Visibility.Collapsed;
        string label = _openApps.Length > 0 ? "Open in " + _openApps[0] : "Open in…";
        SetButtonText(_openInButton, label);
        string app = _openApps.Length > 0 ? _openApps[0] : "its app";
        ToolTipService.SetToolTip(_openInButton, $"Open this document in {app}  Enter");
        AutomationProperties_SetName(_openInButton, label);
        _openInMore.Visibility = _openApps.Length > 1 ? Visibility.Visible : Visibility.Collapsed;
    }

    // ---- buttons in the chrome's flat style --------------------------------------

    // No custom ControlTemplate here. In these islands a Button whose custom
    // template actually loads fail-fasts in Microsoft.UI.Xaml (0xC000027B) once
    // a bar flyout opens or closes afterwards (seen 2026-09-27; the bar's own
    // FlatButtonTemplate has never loaded, which is why its buttons never hit
    // it). WinUI's own Button template is restyled through its lightweight
    // resources instead: no border, no fill at rest or disabled (a disabled
    // button dims rather than turning grey, so it never reads as "selected"),
    // the Mac's FlatButtonStyle washes on hover and press (the appearance's
    // text colour, as docs/design/25 says), and `Background` as the selected wash.
    private static void FlattenButton(Button b)
    {
        SolidColorBrush clear = Brush(Colors.Transparent);
        foreach (string key in new[]
                 {
                     "ButtonBackgroundDisabled", "ButtonBorderBrush", "ButtonBorderBrushPointerOver",
                     "ButtonBorderBrushPressed", "ButtonBorderBrushDisabled",
                 })
        {
            b.Resources[key] = clear;
        }
        b.Resources["ButtonBackgroundPointerOver"] = Brush(ChromeColour.HoverWash);
        b.Resources["ButtonBackgroundPressed"] = Brush(ChromeColour.PressedWash);
        // Icon content inherits the presenter's foreground: keep it the
        // chrome's title colour in every state, not Windows' own text colour.
        foreach (string key in new[] { "ButtonForegroundPointerOver", "ButtonForegroundPressed" })
            b.Resources[key] = Brush(Title);
        b.Resources["ButtonForegroundDisabled"] = Brush(ChromeColour.Disabled);
        b.CornerRadius = new CornerRadius(RadiusButton);
        b.BorderThickness = new Thickness(0);
        // IsEnabledChanged is not raised for a button set disabled before it
        // is in the live tree, so Loaded applies the dim as well.
        void Dim() => b.Opacity = b.IsEnabled ? 1.0 : 0.4;
        Dim();
        b.IsEnabledChanged += (_, _) => Dim();
        b.Loaded += (_, _) => Dim();
    }

    private static Button EditButton(string label, Action click, bool compact = true, string? tip = null,
                                     bool stretch = false)
    {
        double size = compact ? UiFontSize - 2 : UiFontSize;
        var b = new Button
        {
            Content = new TextBlock
            {
                Text = label, FontFamily = UiFont, FontSize = size, Foreground = Brush(Title),
                TextTrimming = TextTrimming.CharacterEllipsis,
            },
            Background = Brush(Colors.Transparent),
            BorderThickness = new Thickness(0),
            Padding = compact ? new Thickness(8, 5, 8, 5) : new Thickness(14, 7, 14, 7),
            FontFamily = UiFont,
            FontSize = size,
            HorizontalAlignment = stretch ? HorizontalAlignment.Stretch : HorizontalAlignment.Left,
            HorizontalContentAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            // A click must not park the keyboard on the island: the crop and
            // trim keys belong to the canvas (the Mac host re-focuses its view).
            AllowFocusOnInteraction = false,
        };
        FlattenButton(b);
        b.Click += (_, _) =>
        {
            try
            {
                click();
            }
            catch (Exception ex)
            {
                // Never let an exception out of a XAML event: that is a fail-fast.
                System.Diagnostics.Debug.WriteLine(ex);
            }
        };
        AutomationProperties_SetName(b, label);
        if (tip is not null) ToolTipService.SetToolTip(b, tip);
        return b;
    }

    private static void SetSelected(Button b, bool selected)
    {
        // FlatButtonStyle: selected wears the pressed wash.
        b.Background = selected ? Brush(ChromeColour.PressedWash) : Brush(Colors.Transparent);
    }

    private static TextBlock SectionTitle(string text)
    {
        TextBlock t = Text(text, Body, UiFontSize - 2);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetHeadingLevel(
            t, Microsoft.UI.Xaml.Automation.Peers.AutomationHeadingLevel.Level2);
        return t;
    }

    private static TextBlock Hint(string text) => Text(text, Body, UiFontSize - 4);

    private static StackPanel Row(params UIElement[] children)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4 };
        foreach (UIElement c in children) row.Children.Add(c);
        return row;
    }

    // ---- the island --------------------------------------------------------------

    private static UIElement BuildEditPane()
    {
        DropEditUi();
        _editShape = EditShape();
        var root = new Grid
        {
            Background = Brush(PanelBg),
            RequestedTheme = IslandTheme,
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(1, 0, 0, 0),
        };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        root.Children.Add(BuildEditStrip());
        var rule = new Border { Height = 1, Background = Brush(Hairline) };
        Grid.SetRow(rule, 1);
        root.Children.Add(rule);
        if (_editTab == TabCrop || _editTab == TabTrim)
        {
            var pane = new StackPanel { Spacing = 14, Padding = new Thickness(14, 12, 14, 14) };
            if (_editTab == TabTrim) BuildTrimPane(pane);
            else BuildCropPane(pane);
            var scroll = new ScrollViewer { Content = pane, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
            Grid.SetRow(scroll, 2);
            root.Children.Add(scroll);
        }
        AutomationProperties_SetName(root, EditTitle);
        RenderEditInPlace();
        return root;
    }

    // What forces a rebuild: the tab, the subject, the crop draft starting or
    // ending, trim arming or previewing (their panes list different buttons).
    private static string EditShape() =>
        $"{_editTab}|{_editSubject}|{_editCropActive}|{_editTrimFlags}|{_editTrimLabel}";

    private static void RenderEdit()
    {
        if (_editPane is null) return;
        if (EditShape() != _editShape)
        {
            _editPane.Content = BuildEditPane();
            return;
        }
        RenderEditInPlace();
    }

    private static UIElement BuildEditStrip()
    {
        var col = new StackPanel { Spacing = 6, Padding = new Thickness(12, 10, 8, 8) };

        var head = new Grid();
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        TextBlock title = Text(EditTitle, Title, UiFontSize, bold: true);
        title.VerticalAlignment = VerticalAlignment.Center;
        head.Children.Add(title);
        _editNameText = Text("", Body, UiFontSize - 2, maxLines: 1);
        _editNameText.TextTrimming = TextTrimming.CharacterEllipsis;
        _editNameText.VerticalAlignment = VerticalAlignment.Center;
        _editNameText.Margin = new Thickness(8, 0, 4, 0);
        Grid.SetColumn(_editNameText, 1);
        head.Children.Add(_editNameText);
        Button done = EditButton("Done", () => Send(Command.EditWorkspace),
                                 tip: "Close the editor  Esc, or Enter again");
        Grid.SetColumn(done, 2);
        head.Children.Add(done);
        col.Children.Add(head);

        // Tabs: a row of plain buttons so each can carry its key.
        var tabs = new Grid { ColumnSpacing = 4 };
        (int Tab, string Label, string Key)[] list = EditIsClip
            ? new[] { (TabTrim, "Trim", "Ctrl+T"), (TabJobs, "Jobs", "Ctrl+J") }
            : new[] { (TabCrop, "Crop", "Shift+C"), (TabColour, "Colour", "Shift+A"), (TabInfo, "Info", "I") };
        for (int i = 0; i < list.Length; i++)
        {
            (int tab, string label, string key) = list[i];
            tabs.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            Button b = EditButton(label, () => Send(Command.EditTab, tab), tip: $"{label}  {key}", stretch: true);
            Grid.SetColumn(b, i);
            tabs.Children.Add(b);
            EditTabButtons.Add((b, tab));
        }
        col.Children.Add(tabs);

        if (EditIsClip)
        {
            col.Children.Add(Row(EditButton("Clip tools…", () => Send(Command.ClipToolsFlyout),
                                            tip: "Rotate, split, remux, save a frame, audio, GIF  Ctrl+S")));
        }
        else
        {
            _editUndo = EditButton("Undo", () => Send(Command.UndoEdit), tip: "Undo the last edit  Ctrl+Z");
            _editReset = EditButton("Reset", () => Send(Command.ResetEdits),
                                    tip: "Reset to the original, exactly  Ctrl+R");
            _editOriginalButton = EditButton("Original",
                () => Send(Command.EditAction, _editOriginal ? EditActions.OriginalOff : EditActions.OriginalOn),
                tip: "Show the original while on (or hold Y); nothing is changed");
            Button save = EditButton("Save copy…", () => Send(Command.EditAction, EditActions.SaveCopy),
                                     tip: "Write a new file with these edits; the original is never changed  Ctrl+S");
            var actions = new Grid();
            actions.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            actions.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            actions.Children.Add(Row(_editUndo, _editReset, _editOriginalButton));
            Grid.SetColumn(save, 1);
            actions.Children.Add(save);
            col.Children.Add(actions);
        }
        _editCountText = Hint("");
        col.Children.Add(_editCountText);
        return col;
    }

    private static void BuildCropPane(StackPanel pane)
    {
        // Start / apply / cancel.
        var crop = new StackPanel { Spacing = 6 };
        crop.Children.Add(SectionTitle("Crop"));
        if (_editCropActive)
        {
            _editCropSize = Text("", Title, UiFontSize - 2);
            crop.Children.Add(_editCropSize);
            crop.Children.Add(Row(
                EditButton("Apply", () => Send(Command.CropCommit), tip: "Apply the crop  Enter"),
                EditButton("Cancel", () => Send(Command.EditAction, EditActions.CancelCrop),
                           tip: "Drop this crop  Esc")));
        }
        else
        {
            crop.Children.Add(Row(EditButton("Crop and straighten", () => Send(Command.CropMode),
                                             tip: "Start cropping  Shift+C")));
        }
        pane.Children.Add(crop);

        // Aspect presets: two rows of plain buttons, the chosen one washed.
        var aspect = new StackPanel { Spacing = 6 };
        aspect.Children.Add(SectionTitle("Aspect ratio"));
        var grid = new Grid { ColumnSpacing = 4, RowSpacing = 4 };
        for (int c = 0; c < 4; c++)
        {
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        }
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        for (int a = 0; a < AspectLabels.Length; a++)
        {
            int preset = a;
            Button b = EditButton(AspectLabels[a],
                () => Send(Command.CropAspectSet, preset + (_editPortrait && HasOrientation(preset) ? 16 : 0)),
                tip: $"Aspect {AspectLabels[a]}  A cycles while cropping", stretch: true);
            AutomationProperties_SetName(b, $"Aspect {AspectLabels[a]}");
            Grid.SetRow(b, a / 4);
            Grid.SetColumn(b, a % 4);
            grid.Children.Add(b);
            EditAspectButtons.Add(b);
        }
        aspect.Children.Add(grid);
        _editOrient = EditButton("Landscape",
            () => Send(Command.CropAspectSet, _editAspect + (_editPortrait ? 0 : 16)),
            tip: "Swap portrait / landscape  X while cropping");
        aspect.Children.Add(_editOrient);
        aspect.Children.Add(Hint("A next ratio · X swap, while cropping"));
        pane.Children.Add(aspect);

        // Straighten: ±45°, half-degree steps; the value is native's.
        var straighten = new StackPanel { Spacing = 6 };
        var head = new Grid();
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        TextBlock st = SectionTitle("Straighten");
        st.VerticalAlignment = VerticalAlignment.Center;
        head.Children.Add(st);
        _editStraightenText = Text("", Body, UiFontSize - 2);
        _editStraightenText.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(_editStraightenText, 1);
        head.Children.Add(_editStraightenText);
        Button level = EditButton("Level", () => Send(Command.CropStraightenSet, 0f), tip: "Back to 0°");
        level.Margin = new Thickness(6, 0, 0, 0);
        AutomationProperties_SetName(level, "Straighten to zero");
        Grid.SetColumn(level, 2);
        head.Children.Add(level);
        straighten.Children.Add(head);
        _editStraightenSlider = new Slider
        {
            Minimum = -45,
            Maximum = 45,
            StepFrequency = 0.5,
            SmallChange = 0.5,
            LargeChange = 5,
            IsThumbToolTipEnabled = false,
        };
        AutomationProperties_SetName(_editStraightenSlider, "Straighten, degrees");
        _editStraightenSlider.ValueChanged += (_, e) =>
        {
            if (_updatingEdit) return;
            _editStraighten = (float)e.NewValue;
            if (_editStraightenText is not null) _editStraightenText.Text = $"{e.NewValue:+0.0;-0.0;0.0}°";
            Send(Command.CropStraightenSet, (float)e.NewValue);
        };
        straighten.Children.Add(_editStraightenSlider);
        straighten.Children.Add(Hint(", . tilt 0.5° while cropping"));
        pane.Children.Add(straighten);

        // Rotate and flip: the PR 10 keys.
        var turn = new StackPanel { Spacing = 6 };
        turn.Children.Add(SectionTitle("Rotate and flip"));
        turn.Children.Add(Row(
            EditButton("⟲ Left", () => Send(Command.RotateCcw), tip: "Rotate left  ["),
            EditButton("⟳ Right", () => Send(Command.RotateCw), tip: "Rotate right  ]"),
            EditButton("Flip H", () => Send(Command.FlipHorizontal), tip: "Flip horizontal  H"),
            EditButton("Flip V", () => Send(Command.FlipVertical), tip: "Flip vertical  V")));
        turn.Children.Add(Hint("A JPEG with only turns is rewritten losslessly."));
        pane.Children.Add(turn);

        pane.Children.Add(Hint("While cropping: arrows move · Shift+arrows resize · Enter apply · Esc cancel"));
    }

    private static void BuildTrimPane(StackPanel pane)
    {
        bool armed = (_editTrimFlags & TrimArmedFlag) != 0;
        bool previewing = (_editTrimFlags & TrimPreviewingFlag) != 0;
        bool marked = (_editTrimFlags & TrimHasMarkerFlag) != 0;
        var trim = new StackPanel { Spacing = 6 };
        trim.Children.Add(SectionTitle("Trim"));
        if (armed)
        {
            if (_editTrimLabel.Length > 0) trim.Children.Add(Text(_editTrimLabel, Title, UiFontSize - 2));
            trim.Children.Add(Row(
                EditButton("Set in", () => Send(Command.TrimIn), tip: "Set in  ["),
                EditButton("Set out", () => Send(Command.TrimOut), tip: "Set out  ]"),
                EditButton(previewing ? "Stop preview" : "Preview", () => Send(Command.TrimPreview),
                           tip: "Loop the cut  P")));
            Button save = EditButton("Save", () => Send(Command.TrimKeyframe),
                                     tip: "Keyframe cut: instant, no re-encode  Enter");
            Button exact = EditButton("Save exact", () => Send(Command.TrimReencode),
                                      tip: "Frame-accurate re-encode, slower  Shift+Enter");
            Button without = EditButton("Copy without in–out", () => Send(Command.TrimRemoveMiddle),
                                        tip: "A copy without the marked range  Ctrl+X");
            save.IsEnabled = exact.IsEnabled = without.IsEnabled = marked;
            trim.Children.Add(Row(save, exact));
            trim.Children.Add(Row(without, EditButton("Clear", () => Send(Command.TrimClear),
                                                      tip: "Clear in and out  Backspace")));
            trim.Children.Add(Row(EditButton("Stop trimming", () => Send(Command.TrimMode), tip: "Ctrl+T")));
        }
        else
        {
            trim.Children.Add(Row(EditButton("Start trimming", () => Send(Command.TrimMode),
                                             tip: "Set in and out points on the scrub bar  Ctrl+T")));
        }
        pane.Children.Add(trim);

        var tools = new StackPanel { Spacing = 6 };
        tools.Children.Add(SectionTitle("Tools"));
        tools.Children.Add(Row(EditButton("Split at playhead", () => Send(Command.ClipSplit), tip: "Ctrl+B")));
        tools.Children.Add(Row(EditButton("More clip tools…", () => Send(Command.ClipToolsFlyout), tip: "Ctrl+S")));
        pane.Children.Add(tools);
        pane.Children.Add(Hint("Space play · , . frame step · J K L shuttle · Q E skip"));
    }

    // Free has none and a square's is itself; Original swaps like the ratios
    // (edit_session.cpp, EditStore.swift hasOrientation).
    private static bool HasOrientation(int aspect) => aspect != 0 && aspect != 2;

    // Everything that moves without a rebuild: labels, washes, enabled states,
    // and the slider (guarded, so native's echo of a drag is not re-sent).
    private static void RenderEditInPlace()
    {
        if (_editNameText is not null) _editNameText.Text = _editName;
        if (_editCountText is not null)
        {
            _editCountText.Text = EditIsClip
                ? "Trims and tools write new files; the clip is never changed."
                : (_editCount == 1 ? "1 edit" : $"{_editCount} edits") + " · the original is never changed";
        }
        foreach ((Button b, int tab) in EditTabButtons)
        {
            bool on = tab == _editTab;
            SetSelected(b, on);
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetItemStatus(b, on ? "Selected" : "");
        }
        bool canEdit = _editSubject != SubjectNone;
        if (_editUndo is not null) _editUndo.IsEnabled = _editCount > 0;
        if (_editReset is not null) _editReset.IsEnabled = _editCount > 0 || _editCropActive;
        if (_editOriginalButton is not null)
        {
            SetSelected(_editOriginalButton, _editOriginal);
            _editOriginalButton.IsEnabled = canEdit;
        }
        if (_editCropSize is not null)
        {
            _editCropSize.Text = _editCropW > 0 ? $"{_editCropW} × {_editCropH} px" : "";
            AutomationProperties_SetName(_editCropSize, $"Crop size {_editCropW} by {_editCropH} pixels");
        }
        for (int i = 0; i < EditAspectButtons.Count; i++)
        {
            SetSelected(EditAspectButtons[i], i == _editAspect);
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetItemStatus(
                EditAspectButtons[i], i == _editAspect ? "Selected" : "");
        }
        if (_editOrient is not null)
        {
            SetButtonText(_editOrient, _editPortrait ? "Portrait" : "Landscape");
            _editOrient.IsEnabled = HasOrientation(_editAspect);
        }
        if (_editStraightenText is not null) _editStraightenText.Text = $"{_editStraighten:+0.0;-0.0;0.0}°";
        if (_editStraightenSlider is not null && Math.Abs(_editStraightenSlider.Value - _editStraighten) > 1e-3)
        {
            _updatingEdit = true;
            try
            {
                _editStraightenSlider.Value = _editStraighten;
            }
            finally
            {
                _updatingEdit = false;
            }
        }
    }
}
