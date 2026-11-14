// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using MediaViewer.Shared;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

/// <summary>
/// "This computer" → Drives (docs/plans/self-host-drives.md §6, owner 2026-11-14): the drives
/// on this PC, and the five ways to use two or more of them offered equally, side by side:
/// Separate, Mirror (recommended), Span, Stripe (warned, still offered), Parity. "RAID is not a
/// backup" always sits above them; a row Windows cannot do stays and says why. A combined choice
/// erases: each drive's facts and a field to type its name, and Erase and combine stays off until
/// every name matches; the program checks them again and Windows asks for an administrator in its
/// own prompt (UAC). Everything runs in the driftwire program on the link's thread
/// (phone/link.h "host_drives…"). The Mac twin is PhoneDrivesView.swift; the words and rules are
/// Shared/PhoneDrives.cs.
/// </summary>
public static partial class IslandHost
{
    private static bool _phoneDrivesOpen;
    private static readonly List<string> PhoneDrivesPicked = new();  // drive ids, in the list's order
    private static string? _phoneDrivesChoice;
    private static FakeInput? _phoneDrivesLabel;
    private static readonly Dictionary<string, FakeInput> PhoneDriveNames = new();  // drive id → its typed name
    private static string? _phoneDrivesReplacing;   // a set's id
    private static string? _phoneDrivesReplaceWith; // a drive's id
    private static FakeInput? _phoneDrivesReplaceName;
    private static string _phoneDrivesAsked = "";   // "combine" | "replace" until its outcome shows
    private static Button? _phoneDrivesGo, _phoneDrivesReplaceGo;

    private static void DetachPhoneDriveInputs()
    {
        foreach (FrameworkElement? f in PhoneDriveNames.Values.Cast<FrameworkElement?>().Append(_phoneDrivesLabel).Append(_phoneDrivesReplaceName))
            if (f?.Parent is Panel p) p.Children.Remove(f);
        _phoneDrivesGo = null;
        _phoneDrivesReplaceGo = null;
    }

    private static void ShowPhoneAgain()
    {
        if (_phoneShown is { } shown) ShowPhone(shown);
    }

    private static void AddPhoneDrives(StackPanel rows, PhoneLinkState s, bool busy)
    {
        DriveListing? l = PhoneDrives.Listing(s.DrivesJson);
        if (!busy && _phoneDrivesAsked.Length > 0 && s.Error.Length > 0) _phoneDrivesAsked = "";  // the error line says it
        if (l is not null)
            foreach (DriveSet set in l.Sets) AddPhoneDriveSet(rows, set, l, busy);
        if (!_phoneDrivesOpen)
        {
            Button use = PhoneButton("Use two or more drives…", () =>
            {
                _phoneDrivesOpen = true;
                PhoneRequest(("do", "host_drives"));
                ShowPhoneAgain();
            }, "Choose drives on this PC for the server");
            use.IsEnabled = !busy;
            rows.Children.Add(SettingsRow("Drives", "Share each drive as it is, or erase two or more and combine them into one.",
                                          use, keepName: true));
        }
        else if (l is null)
        {
            if (!busy)
            {
                Button again = PhoneButton("Check again", () => PhoneRequest(("do", "host_drives")), "List the drives again");
                rows.Children.Add(SettingsRow("Drives", "The drives on this PC couldn't be listed.", again, keepName: true));
            }
        }
        else
        {
            AddPhoneDriveChooser(rows, s, l, busy);
        }
        // "Made "Photos" (mirror, 2 TB); shared as "Photos"." once a run this card asked for ends.
        if (_phoneDrivesAsked.Length > 0 && !busy && s.Error.Length == 0 && s.HostSaid.Length > 0)
            rows.Children.Add(PhoneIndented(PhoneNote(s.HostSaid)));
    }

    private static void AddPhoneDriveChooser(StackPanel rows, PhoneLinkState s, DriveListing l, bool busy)
    {
        if (l.Error.Length > 0) rows.Children.Add(SettingsRow("Drives", l.Error, new Border()));
        // Drives that went away since they were picked are no longer picked.
        PhoneDrivesPicked.RemoveAll(id => l.Disks.All(d => d.Id != id));
        foreach (DriveDisk d in l.Disks)
        {
            var box = new CheckBox { IsChecked = PhoneDrivesPicked.Contains(d.Id), MinWidth = 0, IsEnabled = !busy };
            AutomationProperties.SetName(box, $"Use {d.Name}");
            TrackPhoneFocus(box, $"Use {d.Name}");
            DriveDisk disk = d;
            box.Click += (_, _) =>
            {
                bool on = box.IsChecked == true;
                var ids = PhoneDrivesPicked.Where(id => id != disk.Id).ToList();
                if (on) ids.Add(disk.Id);
                PhoneDrivesPicked.Clear();
                PhoneDrivesPicked.AddRange(l.Disks.Select(k => k.Id).Where(ids.Contains));  // the list's order
                _phoneDrivesChoice = null;
                if (PhoneDrivesPicked.Count > 0) PhoneRequestJson(PhoneDrives.ChoicesRequest(PhoneDrivesPicked));
                ShowPhoneAgain();
            };
            string detail = d.Refused.Length == 0 ? d.Facts : d.Facts + "\nCan't be erased: " + d.Refused;
            rows.Children.Add(SettingsRow(d.Name, detail, box, keepName: true));
        }
        if (l.Disks.Count == 0)
        {
            Button again = PhoneButton("Check again", () => PhoneRequest(("do", "host_drives")), "List the drives again");
            again.IsEnabled = !busy;
            rows.Children.Add(SettingsRow("Drives", "No drives besides this PC's own. Connect the drives and choose Check again.",
                                          again, keepName: true));
        }
        DriveChoices? c = PhoneDrives.Choices(s.DrivesChoicesJson);
        if (PhoneDrivesPicked.Count > 0 && c is not null)
        {
            AddPhoneDriveOptions(rows, c, s.Busy == "host_drives_choices", busy);
            DriveOption? chosen = c.Options.FirstOrDefault(o => o.Choice == _phoneDrivesChoice && o.Offered);
            if (chosen is not null) AddPhoneDriveConfirm(rows, chosen, PickedDisks(l), busy);
        }
        Button close = PhoneButton("Close", () =>
        {
            _phoneDrivesOpen = false;
            PhoneDrivesPicked.Clear();
            _phoneDrivesChoice = null;
            ForgetPhoneDriveNames();
            ShowPhoneAgain();
        }, "Close the drives step");
        close.IsEnabled = !busy;
        close.HorizontalAlignment = HorizontalAlignment.Right;
        rows.Children.Add(close);
    }

    private static List<DriveDisk> PickedDisks(DriveListing l) =>
        PhoneDrivesPicked.Select(id => l.Disks.FirstOrDefault(d => d.Id == id)).OfType<DriveDisk>().ToList();

    // The five, side by side and equal: one star column each, one height for all.
    private static void AddPhoneDriveOptions(StackPanel rows, DriveChoices c, bool reading, bool busy)
    {
        var panel = new StackPanel { Spacing = Space(8), Margin = Pad(12, 0, 12, 0), Opacity = reading ? 0.5 : 1 };
        TextBlock line = Text(c.NotABackup, Title, TypeSize(12), bold: true);
        panel.Children.Add(line);
        var grid = new Grid { ColumnSpacing = Space(8) };
        for (int i = 0; i < c.Options.Count; ++i)
        {
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            Button card = PhoneDriveCard(c.Options[i], busy);
            Grid.SetColumn(card, i);
            grid.Children.Add(card);
        }
        panel.Children.Add(grid);
        rows.Children.Add(panel);
    }

    private static Button PhoneDriveCard(DriveOption o, bool busy)
    {
        bool chosen = _phoneDrivesChoice == o.Choice;
        var body = new StackPanel { Spacing = Space(6) };
        body.Children.Add(Text(o.Title, o.Offered ? Title : ChromeColour.Disabled, TypeSize(13), bold: true));
        if (o.Recommended) body.Children.Add(PhoneDriveBadge("", "For photos", Brush(ChromeColour.Accent)));    // CheckMark
        if (o.Warning) body.Children.Add(PhoneDriveBadge("", "Risky", Brush(TrackWarm)));                      // Warning
        if (o.Offered)
        {
            body.Children.Add(Text(o.SpaceWords, Body, TypeSize(11)));
            body.Children.Add(Text("If one dies: " + o.DeadWords, Body, TypeSize(11)));
            body.Children.Add(Text(o.SpeedWords, Body, TypeSize(11)));
        }
        else
        {
            body.Children.Add(Text(o.WhyNot, Body, TypeSize(11)));
        }
        var b = new Button
        {
            Content = body,
            Padding = Pad(12),
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Left,
            VerticalContentAlignment = VerticalAlignment.Top,
            AllowFocusOnInteraction = true,
        };
        FlattenButton(b);
        // The chosen card lifts (Craft rules: selection is a lighter surface, the ring is the keyboard's).
        b.Background = Brush(chosen ? ChromeColour.PressedWash : ChromeColour.Surface);
        b.CornerRadius = Corner(RadiusCard);
        b.IsEnabled = o.Offered && !busy;
        string name = o.Offered ? o.Title : $"{o.Title}, not available: {o.WhyNot}";
        AutomationProperties.SetName(b, name);
        TrackPhoneFocus(b, name);
        b.Click += (_, _) =>
        {
            _phoneDrivesChoice = o.Choice;
            ForgetPhoneDriveNames();
            if (o.Erases && PhoneDriveLabel().Text.Length == 0) PhoneDriveLabel().SetText("Photos");
            ShowPhoneAgain();
        };
        return b;
    }

    private static StackPanel PhoneDriveBadge(string glyph, string text, Microsoft.UI.Xaml.Media.Brush ink)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(4) };
        row.Children.Add(new FontIcon { Glyph = glyph, FontFamily = IconFont, FontSize = TypeSize(12), Foreground = ink });
        TextBlock t = Text(text, Body, TypeSize(11));
        t.Foreground = ink;
        row.Children.Add(t);
        return row;
    }

    private static FakeInput PhoneDriveLabel()
    {
        if (_phoneDrivesLabel is null)
        {
            _phoneDrivesLabel = PhoneInput("Photos", "Name of the combined drive", 200);
            _phoneDrivesLabel.Changed += PhoneDrivesFieldsChanged;
        }
        return _phoneDrivesLabel;
    }

    private static FakeInput PhoneDriveName(DriveDisk d)
    {
        if (!PhoneDriveNames.TryGetValue(d.Id, out FakeInput? field))
        {
            field = PhoneInput($"Type “{d.Name}” to erase it", $"Type {d.Name} to erase it");
            field.Changed += PhoneDrivesFieldsChanged;
            PhoneDriveNames[d.Id] = field;
        }
        return field;
    }

    // Typed names never outlive the choice they were typed for.
    private static void ForgetPhoneDriveNames()
    {
        foreach (FakeInput f in PhoneDriveNames.Values) f.SetText("");
        _phoneDrivesReplaceName?.SetText("");
    }

    private static void AddPhoneDriveConfirm(StackPanel rows, DriveOption o, List<DriveDisk> disks, bool busy)
    {
        if (!o.Erases)
        {
            Button share = PhoneButton("Share", () => CombinePhoneDrives(o.Choice), "Share each picked drive as it is");
            share.IsEnabled = !busy;
            rows.Children.Add(SettingsRow("Share each drive as it is",
                "Nothing is erased. Each drive becomes its own folder for your phone.", share, keepName: true));
            return;
        }
        rows.Children.Add(SettingsRow("Name of the combined drive",
            "1 to 27 letters, digits, spaces, - or _. It is shared under this name.", PhoneDriveLabel(), keepName: true));
        foreach (DriveDisk d in disks)
            rows.Children.Add(SettingsRow(d.Name, d.Facts, PhoneDriveName(d), keepName: true));
        var refused = disks.Where(d => d.Refused.Length > 0).ToList();
        _phoneDrivesGo = PhoneButton("Erase and combine", () => CombinePhoneDrives(o.Choice), "Erase the picked drives and combine them");
        rows.Children.Add(SettingsRow($"{o.Title}: erase and combine",
            refused.Count == 0
                ? "Everything on these drives is erased. Windows asks for an administrator once."
                : string.Join("\n", refused.Select(d => $"{d.Name} can't be erased: {d.Refused}")),
            _phoneDrivesGo, keepName: true));
    }

    private static bool PhoneDrivesCanCombine(out List<DriveDisk> disks)
    {
        disks = new List<DriveDisk>();
        if (_phoneShown is not { } s || s.Busy.Length > 0 || PhoneDrives.Listing(s.DrivesJson) is not { } l) return false;
        disks = PickedDisks(l);
        if (disks.Count == 0 || disks.Any(d => d.Refused.Length > 0)) return false;
        return PhoneDrives.GoodLabel(PhoneDriveLabel().Text.Trim()) &&
               PhoneDrives.NamesMatch(disks.Select(d => PhoneDriveName(d).Text).ToList(), disks);
    }

    private static void CombinePhoneDrives(string choice)
    {
        if (_phoneShown is not { } s || s.Busy.Length > 0 || PhoneDrives.Listing(s.DrivesJson) is not { } l) return;
        List<DriveDisk> disks = PickedDisks(l);
        if (disks.Count == 0) return;
        if (choice != "separate" && !PhoneDrivesCanCombine(out disks)) return;
        string request = PhoneDrives.CombineRequest(choice, disks, PhoneDriveLabel().Text, disks.Select(d => PhoneDriveName(d).Text).ToList());
        ForgetPhoneDriveNames();
        PhoneDrivesPicked.Clear();
        _phoneDrivesChoice = null;
        _phoneDrivesAsked = "combine";
        _ = PhoneRequestJson(request);
        ShowPhoneAgain();
    }

    // ---- a set's health, and replacing a drive ----

    private static void AddPhoneDriveSet(StackPanel rows, DriveSet set, DriveListing l, bool busy)
    {
        FrameworkElement control = new Border();
        if (set.CanTakeADrive && _phoneDrivesReplacing != set.Id)
        {
            Button replace = PhoneButton("Replace a drive…", () =>
            {
                _phoneDrivesReplacing = set.Id;
                _phoneDrivesReplaceWith = null;
                _phoneDrivesReplaceName?.SetText("");
                ShowPhoneAgain();
            }, $"Replace the lost drive in {set.Name}");
            replace.IsEnabled = !busy;
            control = replace;
        }
        rows.Children.Add(SettingsRow(set.Name, set.Line, control, keepName: true));
        if (_phoneDrivesReplacing == set.Id) AddPhoneDriveReplace(rows, set, l, busy);
    }

    private static void AddPhoneDriveReplace(StackPanel rows, DriveSet set, DriveListing l, bool busy)
    {
        IReadOnlyList<DriveDisk> candidates = PhoneDrives.Replacements(set, l);
        if (candidates.Count == 0)
        {
            Button again = PhoneButton("Check again", () => PhoneRequest(("do", "host_drives")), "List the drives again");
            again.IsEnabled = !busy;
            rows.Children.Add(SettingsRow("A drive to take its place",
                "Connect a drive at least as big as the others, then choose Check again.", again, keepName: true));
        }
        else
        {
            var pick = new ComboBox { FontFamily = UiFont, FontSize = UiFontSize, Foreground = Brush(Title), Width = 200, IsEnabled = !busy };
            pick.Items.Add("Choose…");
            foreach (DriveDisk d in candidates) pick.Items.Add(d.Name);
            int at = candidates.ToList().FindIndex(d => d.Id == _phoneDrivesReplaceWith);
            pick.SelectedIndex = at + 1;
            AutomationProperties.SetName(pick, "New drive");
            TrackPhoneFocus(pick, "New drive");
            pick.SelectionChanged += (_, _) =>
            {
                int i = pick.SelectedIndex - 1;
                string? id = i >= 0 && i < candidates.Count ? candidates[i].Id : null;
                if (id == _phoneDrivesReplaceWith) return;
                _phoneDrivesReplaceWith = id;
                _phoneDrivesReplaceName?.SetText("");
                ShowPhoneAgain();
            };
            rows.Children.Add(SettingsRow("A drive to take its place",
                "Everything on it is erased, and the mirror rebuilds onto it.", pick, keepName: true));
            if (candidates.FirstOrDefault(d => d.Id == _phoneDrivesReplaceWith) is { } chosen)
            {
                if (_phoneDrivesReplaceName is null)
                {
                    _phoneDrivesReplaceName = PhoneInput("", "Name of the new drive");
                    _phoneDrivesReplaceName.Changed += PhoneDrivesFieldsChanged;
                }
                _phoneDrivesReplaceName.PlaceholderText = $"Type “{chosen.Name}” to erase it";
                rows.Children.Add(SettingsRow(chosen.Name, chosen.Facts, _phoneDrivesReplaceName, keepName: true));
            }
        }
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8), HorizontalAlignment = HorizontalAlignment.Right };
        Button cancel = PhoneButton("Cancel", () =>
        {
            _phoneDrivesReplacing = null;
            _phoneDrivesReplaceName?.SetText("");
            ShowPhoneAgain();
        }, "Don't replace a drive");
        cancel.IsEnabled = !busy;
        _phoneDrivesReplaceGo = PhoneButton("Erase and replace", () => ReplacePhoneDrive(set), $"Erase the new drive and rebuild {set.Name} onto it");
        buttons.Children.Add(cancel);
        buttons.Children.Add(_phoneDrivesReplaceGo);
        rows.Children.Add(buttons);
    }

    private static DriveDisk? PhoneDriveReplacement()
    {
        if (_phoneShown is not { } s || PhoneDrives.Listing(s.DrivesJson) is not { } l) return null;
        DriveSet? set = l.Sets.FirstOrDefault(x => x.Id == _phoneDrivesReplacing);
        return set is null ? null : PhoneDrives.Replacements(set, l).FirstOrDefault(d => d.Id == _phoneDrivesReplaceWith);
    }

    private static bool PhoneDrivesCanReplace() =>
        _phoneShown is { Busy.Length: 0 } && PhoneDriveReplacement() is { } d && _phoneDrivesReplaceName is { } f &&
        PhoneDrives.NamesMatch(new[] { f.Text }, new[] { d });

    private static void ReplacePhoneDrive(DriveSet set)
    {
        if (!PhoneDrivesCanReplace() || PhoneDriveReplacement() is not { } d) return;
        string request = PhoneDrives.ReplaceRequest(set.Name, d.Id, _phoneDrivesReplaceName!.Text);
        _phoneDrivesReplaceName.SetText("");
        _phoneDrivesReplacing = null;
        _phoneDrivesReplaceWith = null;
        _phoneDrivesAsked = "replace";
        _ = PhoneRequestJson(request);
        ShowPhoneAgain();
    }

    /// <summary>PhoneFieldsChanged's part for this step: the erase buttons follow the typed names.</summary>
    private static void PhoneDrivesFieldsChanged()
    {
        if (_phoneDrivesGo is not null) _phoneDrivesGo.IsEnabled = PhoneDrivesCanCombine(out _);
        if (_phoneDrivesReplaceGo is not null) _phoneDrivesReplaceGo.IsEnabled = PhoneDrivesCanReplace();
    }
}
