// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;

namespace MediaViewer.Chrome;

/// <summary>
/// docs/plans/edit-refresh.md S8 (§3.2 Faces): the Faces tool's panel in the
/// Enhance tab, the twin of the Mac's FacesPanelView.swift.
/// </summary>
/// <remarks>
/// While the Faces tool is armed the canvas draws a numbered box per face (a
/// click chooses it or leaves it out); here each face has its thumbnail, the
/// person's name from People and "from 37 photos of Anna", and its fixes:
/// Open eyes (only where the eyes look shut), Improve face and, with Enhance,
/// Restore face. Do all runs
/// each chosen face's best fix into one new file. With People off, a button
/// opens Local search to turn it on. Native owns the state (shell/faces_tool.h)
/// and pushes it with <see cref="SetFacesPanel"/> (chrome_faces_args); the
/// panel sends Command.Faces with shell::pack_faces_op. The fixes are a table
/// (<see cref="FaceActions"/>, shell::kFaceActions): S10's Restore face is the
/// third row, run by Enhance's docked panel on the chosen faces. Button, TextBlock, Border, Grid, StackPanel and Image only
/// (tools/check-winui-controls.ps1).
/// </remarks>
public static partial class IslandHost
{
    // Mirrors chrome_face_card_args / chrome_faces_args (chrome_host.h).
    private const int FaceCardSize = 48;
    private const int FacesArgsSize = 40;

    // shell::faces_status and shell::faces_op.
    private static class FacesStatus
    {
        public const int Idle = 0, Looking = 1, Ready = 2, NoPack = 3, PeopleOff = 4, Failed = 5;
    }
    private static class FacesOp
    {
        public const int Run = 0, Toggle = 1, ChooseAll = 2, ChooseNone = 3, People = 4;
        // Appended in the S8 review: read again; the panel's row lights its box.
        public const int Retry = 5, Hover = 6;
    }
    private const int FaceDoAll = -1;

    // shell::face_action, in wire order (append only, with shell/faces_tool.h).
    private sealed record FaceActionInfo(int Id, string Label, string Busy, string Glyph, string Tip);
    private static readonly FaceActionInfo[] FaceActions =
    {
        new(0, "Open eyes", "Opening eyes…", "",
            "This person's open eyes from a photo of the same moment or from People, matched to this light. Saves a new file."),
        new(1, "Improve face", "Finding a better face…", "",
            "A clearly better face of the same person from your photos, turned the same way, in this photo's light. Saves a new file."),
        // S10's model through Enhance's docked panel, on this face (offered while Enhance has it).
        new(2, "Restore face", "Restoring in Enhance…", "\uE77B",
            "Enhance's face model makes this face sharper and clearer, checked by People to still look like the same person. Saves a new file."),
    };

    private sealed class FaceCardModel
    {
        public long Person;
        public string Name = "";
        public string Source = "";
        public int Actions;
        public bool Shut;
        public WriteableBitmap? Thumb;
        public string Title => Name.Length > 0 ? Name : Person != 0 ? "Unnamed person" : "Not in People yet";
        public bool Offers(int action) => (Actions & (1 << action)) != 0;
    }

    private static readonly List<FaceCardModel> FaceCards = new();
    private static bool _facesArmed;
    private static int _facesStatus;
    private static int _facesChosen;
    private static bool _facesRunning;
    private static int _facesRunningFace = -1;
    private static int _facesRunningAction;
    private static uint _facesRevision = uint.MaxValue;
    // The hub's slot for the panel (BuildEnhanceHub), rebuilt in place.
    private static StackPanel? _facesHost;

    public static int SetFacesPanel(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < FacesArgsSize) return unchecked((int)0x80070057);
            IntPtr cards = Marshal.ReadIntPtr(arg, 0);
            int count = Math.Clamp(Marshal.ReadInt32(arg, 8), 0, 16);
            bool armed = Marshal.ReadInt32(arg, 12) != 0;
            int status = Marshal.ReadInt32(arg, 16);
            int chosen = Marshal.ReadInt32(arg, 20);
            bool running = Marshal.ReadInt32(arg, 24) != 0;
            int runningFace = Marshal.ReadInt32(arg, 28);
            int runningAction = Marshal.ReadInt32(arg, 32);
            uint revision = unchecked((uint)Marshal.ReadInt32(arg, 36));
            bool armedChanged = armed != _facesArmed;
            _facesArmed = armed;
            if (revision == _facesRevision && !armedChanged) return 0;
            // The same read (a choice changed): keep the pictures already made.
            bool sameRead = _facesStatus == FacesStatus.Ready && status == FacesStatus.Ready && count == FaceCards.Count;
            _facesRevision = revision;
            _facesStatus = status;
            _facesChosen = chosen;
            _facesRunning = running;
            _facesRunningFace = runningFace;
            _facesRunningAction = runningAction;
            var next = new List<FaceCardModel>(count);
            for (int i = 0; cards != IntPtr.Zero && i < count; i++)
            {
                IntPtr c = checked(cards + FaceCardSize * i);
                IntPtr rgba = Marshal.ReadIntPtr(c, 0);
                var card = new FaceCardModel
                {
                    Name = Marshal.PtrToStringUTF8(Marshal.ReadIntPtr(c, 8)) ?? "",
                    Source = Marshal.PtrToStringUTF8(Marshal.ReadIntPtr(c, 16)) ?? "",
                    Person = Marshal.ReadInt64(c, 24),
                    Actions = Marshal.ReadInt32(c, 40),
                    Shut = Marshal.ReadInt32(c, 44) != 0,
                };
                int w = Marshal.ReadInt32(c, 32), h = Marshal.ReadInt32(c, 36);
                FaceCardModel? old = sameRead ? FaceCards[i] : null;
                if (old is not null && old.Person == card.Person && old.Name == card.Name && old.Thumb is not null)
                    card.Thumb = old.Thumb;
                else if (rgba != IntPtr.Zero && w > 0 && h > 0 && w <= 512 && h <= 512)
                    card.Thumb = FaceBitmap(rgba, w, h);
                next.Add(card);
            }
            FaceCards.Clear();
            FaceCards.AddRange(next);
            if (armedChanged) RenderEnhanceInPlace();  // the Faces card's "Choosing" chip
            RenderFaces();
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    private static WriteableBitmap FaceBitmap(IntPtr rgba, int w, int h)
    {
        int bytes = checked(w * h * 4);
        byte[] px = new byte[bytes];
        Marshal.Copy(rgba, px, 0, bytes);
        // RGBA -> BGRA (opaque, so premultiplied is the same bytes).
        for (int o = 0; o < bytes; o += 4) (px[o], px[o + 2]) = (px[o + 2], px[o]);
        var bitmap = new WriteableBitmap(w, h);
        px.CopyTo(0, bitmap.PixelBuffer, 0, bytes);
        bitmap.Invalidate();
        return bitmap;
    }

    private static int PackFacesOp(int op, int face, int action) => op * 100000 + (face + 1) * 100 + (action + 1);
    private static void SendFaces(int op, int face = -1, int action = 0) =>
        Send(Command.Faces, PackFacesOp(op, face, action));
    private static bool FaceChosen(int i) => i < 32 && (_facesChosen & (1 << i)) != 0;
    private static int FacesChosenCount => Enumerable.Range(0, FaceCards.Count).Count(FaceChosen);

    // The bar's and the panel's words.
    private static string FacesSummary() => _facesStatus switch
    {
        FacesStatus.NoPack => "Needs Local search",
        FacesStatus.PeopleOff => "Needs People",
        FacesStatus.Failed => "Could not read the faces",
        FacesStatus.Ready when FaceCards.Count == 0 => "No faces found",
        FacesStatus.Ready => $"{(FaceCards.Count == 1 ? "1 face" : $"{FaceCards.Count} faces")} · {FacesChosenCount} chosen",
        _ => "Looking for faces…",
    };

    // The Enhance tab's slot: the panel while the Faces tool is armed.
    private static void RenderFaces()
    {
        StackPanel? host = _facesHost;
        if (host is null) return;
        host.Children.Clear();
        if (!_facesArmed)
        {
            host.Visibility = Visibility.Collapsed;
            return;
        }
        host.Visibility = Visibility.Visible;
        host.Children.Add(BuildFacesPanel());
    }

    private static Border BuildFacesPanel()
    {
        var col = new StackPanel { Spacing = Space(10) };
        // The header: the icon, "Faces", the summary, Done.
        var head = new Grid { ColumnSpacing = Space(10) };
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.Children.Add(new Border
        {
            Width = 30, Height = 30, CornerRadius = Corner(8),
            Background = Brush(ChromeColour.SelectionWash),
            Child = new FontIcon { Glyph = RailGlyph.Faces, FontFamily = IconFont, FontSize = TypeSize(15),
                                   Foreground = Brush(ChromeColour.Accent) },
        });
        var words = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
        TextBlock title = Text("Faces", Title, UiFontSize - 1, maxLines: 1);
        AutomationProperties.SetHeadingLevel(title, Microsoft.UI.Xaml.Automation.Peers.AutomationHeadingLevel.Level3);
        words.Children.Add(title);
        words.Children.Add(Text(FacesSummary(), Body, UiFontSize - 5, maxLines: 1));
        Grid.SetColumn(words, 1);
        head.Children.Add(words);
        Button done = EditButton("Done", () => SendToolAction(ToolBarAction.Done), tip: "Put the Faces tool down  Esc");
        Grid.SetColumn(done, 2);
        head.Children.Add(done);
        col.Children.Add(head);

        switch (_facesStatus)
        {
            case FacesStatus.NoPack:
                col.Children.Add(FacesEmpty("Faces uses Local search",
                    "Local search's People finds who is who, and their other photos give the open eyes and better " +
                    "faces. Everything runs on this PC.", "Get Local search…", () => OpenAddonSettings(LocalSearchPageId),
                    "Check again"));
                break;
            case FacesStatus.PeopleOff:
                col.Children.Add(FacesEmpty("Turn on People",
                    "People in Local search finds each face's other photos on this PC, so a fix can borrow their " +
                    "open eyes or a clearer face.", "Turn on People…", () =>
                    {
                        SendFaces(FacesOp.People);
                        OpenAddonSettings(LocalSearchPageId);
                    }, "Check again"));
                break;
            case FacesStatus.Failed:
                col.Children.Add(FacesEmpty("Could not read this photo's faces", "Try again, or check the photo opens.",
                    "Try again", () => SendFaces(FacesOp.Retry)));
                break;
            case FacesStatus.Ready when FaceCards.Count == 0:
                col.Children.Add(FacesEmpty("No faces found",
                    "Faces works on photos where People can see a face, front on or turned a little.", null, null));
                break;
            case FacesStatus.Ready:
                col.Children.Add(Hint("Click a face's box on the photo, or its picture here, to choose it."));
                for (int i = 0; i < FaceCards.Count; i++) col.Children.Add(BuildFaceRow(i));
                col.Children.Add(BuildFacesFooter());
                break;
            default:
            {
                var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8) };
                var spin = new Spinner(16);
                row.Children.Add(spin.Root);
                spin.Active = true;
                row.Children.Add(Text("Looking for faces in this photo…", Body, UiFontSize - 4));
                col.Children.Add(row);
                break;
            }
        }
        var panel = new Border
        {
            Child = col,
            Padding = Pad(12),
            CornerRadius = Corner(10),
            Background = Brush(ChromeColour.Surface2),
            BorderBrush = Brush(ChromeColour.Accent),
            BorderThickness = new Thickness(1.5),
        };
        AutomationProperties_SetName(panel, $"Faces, {FacesSummary()}");
        return panel;
    }

    // One face: its picture (a click chooses it), who it is, its fixes.
    private static Border BuildFaceRow(int i)
    {
        FaceCardModel f = FaceCards[i];
        bool chosen = FaceChosen(i);
        bool busy = _facesRunning && (_facesRunningFace == i || (_facesRunningFace < 0 && chosen));
        var grid = new Grid { ColumnSpacing = Space(10) };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        // The thumbnail with the number the box on the photo wears.
        var picture = new Grid { Width = 56, Height = 56 };
        var frame = new Border
        {
            Width = 56, Height = 56,
            CornerRadius = Corner(10),
            BorderBrush = Brush(chosen ? ChromeColour.Accent : Hairline),
            BorderThickness = new Thickness(chosen ? 2 : 0.5),
            Background = Brush(ChromeColour.Surface2),
            Opacity = chosen ? 1.0 : 0.6,
        };
        if (f.Thumb is not null)
            frame.Child = new Image { Source = f.Thumb, Stretch = Stretch.UniformToFill };
        else
            frame.Child = new FontIcon { Glyph = RailGlyph.Faces, FontFamily = IconFont, FontSize = TypeSize(22),
                                         Foreground = Brush(Body) };
        picture.Children.Add(frame);
        picture.Children.Add(new Border
        {
            Width = 18, Height = 18,
            CornerRadius = new CornerRadius(9),
            HorizontalAlignment = HorizontalAlignment.Left,
            VerticalAlignment = VerticalAlignment.Top,
            Margin = new Thickness(-5, -5, 0, 0),
            Background = chosen ? Brush(ChromeColour.Accent) : new SolidColorBrush(ColorHelper.FromArgb(180, 0, 0, 0)),
            BorderBrush = new SolidColorBrush(ColorHelper.FromArgb(chosen ? (byte)0 : (byte)230, 255, 255, 255)),
            BorderThickness = new Thickness(1),
            Child = new TextBlock
            {
                Text = (i + 1).ToString(System.Globalization.CultureInfo.InvariantCulture),
                FontFamily = UiFont, FontSize = TypeSize(10), FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                // White on the dark disc (the canvas badge's), the accent's ink when chosen.
                Foreground = chosen ? Brush(ChromeColour.OnAccent) : new SolidColorBrush(Colors.White),
                HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
            },
        });
        int face = i;
        Button pick = ChromeButton(picture, new Thickness(0), () => SendFaces(FacesOp.Toggle, face),
                                   name: $"Face {i + 1}, {f.Title}",
                                   tip: chosen ? "Chosen: click to leave this face out" : "Click to choose this face",
                                   enabled: !_facesRunning);
        pick.VerticalAlignment = VerticalAlignment.Top;
        grid.Children.Add(pick);

        var words = new StackPanel { Spacing = Space(3) };
        var nameRow = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(6) };
        TextBlock name = Text(f.Title, f.Name.Length > 0 ? Title : Body, UiFontSize - 2, maxLines: 1);
        name.TextTrimming = TextTrimming.CharacterEllipsis;
        nameRow.Children.Add(name);
        if (f.Shut)
        {
            nameRow.Children.Add(new Border
            {
                Padding = Pad(6, 1, 6, 1),
                CornerRadius = new CornerRadius(999),
                Background = Brush(ChromeColour.SelectionWash),
                VerticalAlignment = VerticalAlignment.Center,
                Child = Text("Eyes closed", ChromeColour.Accent, UiFontSize - 6, maxLines: 1),
            });
        }
        words.Children.Add(nameRow);
        if (f.Source.Length > 0) words.Children.Add(Text(f.Source, Body, UiFontSize - 5, maxLines: 1));
        if (busy)
        {
            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(6) };
            var spin = new Spinner(12);
            row.Children.Add(spin.Root);
            spin.Active = true;
            FaceActionInfo? running = FaceActions.FirstOrDefault(a => a.Id == _facesRunningAction);
            row.Children.Add(Text(running?.Busy ?? "Working…", Body, UiFontSize - 5));
            words.Children.Add(row);
        }
        else
        {
            var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(4) };
            foreach (FaceActionInfo a in FaceActions)
            {
                if (!f.Offers(a.Id)) continue;
                var content = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(4) };
                content.Children.Add(new FontIcon { Glyph = a.Glyph, FontFamily = IconFont, FontSize = TypeSize(13) });
                content.Children.Add(ButtonLabel(a.Label, UiFontSize - 3));
                int id = a.Id;
                Button b = ChromeButton(content, CompactButtonPadding, () => SendFaces(FacesOp.Run, face, id),
                                        align: HorizontalAlignment.Left, name: a.Label, tip: a.Tip,
                                        enabled: !_facesRunning && (_editWand & WandEyesBusy) == 0);
                actions.Children.Add(b);
            }
            words.Children.Add(actions);
        }
        Grid.SetColumn(words, 1);
        grid.Children.Add(words);
        var card = new Border
        {
            Child = grid,
            Padding = Pad(8),
            CornerRadius = Corner(8),
            Background = chosen ? Brush(ChromeColour.SelectionWash) : Brush(Colors.Transparent),
        };
        // The row under the pointer lights its face's box on the photo.
        card.PointerEntered += (_, _) =>
        {
            if (!chosen) card.Background = Brush(ChromeColour.HoverWash);
            SendFaces(FacesOp.Hover, face);
        };
        card.PointerExited += (_, _) =>
        {
            if (!chosen) card.Background = Brush(Colors.Transparent);
            SendFaces(FacesOp.Hover, -1);
        };
        return card;
    }

    private static StackPanel BuildFacesFooter()
    {
        var col = new StackPanel { Spacing = Space(6) };
        var row = new Grid { ColumnSpacing = Space(6) };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        int chosen = FacesChosenCount;
        bool doingAll = _facesRunning && _facesRunningAction == FaceDoAll;
        Button all = AccentButton(doingAll ? "Working…" : chosen == FaceCards.Count ? "Do all" : $"Do all {chosen} chosen",
                                  () => SendFaces(FacesOp.Run, -1, FaceDoAll));
        all.IsEnabled = !_facesRunning && chosen > 0 && (_editWand & WandEyesBusy) == 0;
        ToolTipService.SetToolTip(all, "Each chosen face's best fix, in one new file: a clearly better face of the " +
                                       "same person, else open eyes where they are shut.");
        row.Children.Add(all);
        Button every = EditButton("All", () => SendFaces(FacesOp.ChooseAll), tip: "Choose every face");
        every.IsEnabled = !_facesRunning && chosen < FaceCards.Count;
        Grid.SetColumn(every, 2);
        row.Children.Add(every);
        Button none = EditButton("None", () => SendFaces(FacesOp.ChooseNone), tip: "Leave every face out");
        none.IsEnabled = !_facesRunning && chosen > 0;
        Grid.SetColumn(none, 3);
        row.Children.Add(none);
        col.Children.Add(row);
        col.Children.Add(Hint("Each fix writes a new file; this photo is never changed."));
        return col;
    }

    // An empty state: what is missing, the one button that fixes it and, when
    // it was fixed elsewhere (People turned on), "Check again" (a new read).
    private static Grid FacesEmpty(string title, string text, string? action, Action? run, string? recheck = null)
    {
        var grid = new Grid { ColumnSpacing = Space(10) };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.Children.Add(new FontIcon
        {
            Glyph = RailGlyph.Faces, FontFamily = IconFont, FontSize = TypeSize(18),
            Foreground = Brush(ChromeColour.Accent), VerticalAlignment = VerticalAlignment.Top,
        });
        var col = new StackPanel { Spacing = Space(6) };
        col.Children.Add(Text(title, Title, UiFontSize - 2));
        col.Children.Add(Hint(text));
        if (action is not null && run is not null)
        {
            Button b = AccentButton(action, run);
            if (recheck is null)
            {
                b.HorizontalAlignment = HorizontalAlignment.Left;
                col.Children.Add(b);
            }
            else
            {
                var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(6) };
                buttons.Children.Add(b);
                buttons.Children.Add(EditButton(recheck, () => SendFaces(FacesOp.Retry),
                                                tip: "Read this photo's faces again"));
                col.Children.Add(buttons);
            }
        }
        Grid.SetColumn(col, 1);
        grid.Children.Add(col);
        return grid;
    }

    // Settings → Add-ons → `page` (Local search holds People).
    private static void OpenAddonSettings(string page)
    {
        if (!_settingsVisible)
        {
            _addonPageWanted = page;
            _addonListWanted = false;
            Send(Command.OpenSettings);
            return;
        }
        _showGeneralSettings?.Invoke();
        OpenAddonPage(page);
    }
}
