// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using MediaViewer.Interop;
using MediaViewer.Shared;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Windows.Storage.Pickers;

namespace MediaViewer.Ai.Chrome;

/// <summary>
/// Settings → Local search → Phone access (docs/plans/mobile.md §5–§7.5), the
/// Windows twin of the Mac's PhoneAccessView.swift and PhoneLinkView.swift:
/// the NAS folder the catalogue is published to and Publish now; then the
/// service on the NAS (claim it, its addresses), the code a phone scans with
/// its check code, the paired phones and Remove, the named folders, and
/// handing the library to or from another computer. Nearby sync needs no row:
/// the pack serves the newest generation to paired phones on this network by
/// itself once something is published.
/// </summary>
/// <remarks>
/// Every request is phone_publish / phone_link, [no-block]: the pack's own
/// thread does the work and this card only asks and watches, reading
/// phone_json / phone_link_json on the status tick. The signing key never
/// passes through here: the pack keeps it in Credential Manager sealed with
/// DPAPI (secrets_win.cpp), where the Mac keeps it in the Keychain. Text entry
/// is FakeInput (a TextBox fail-fasts in this island). Keys and addresses shown
/// here never reach a log (rule 6). The link rows are rebuilt only when the
/// pack's answer changes, so a button keeps keyboard focus.
/// </remarks>
internal sealed partial class ManagePanel
{
    private StackPanel? _phoneSection;
    private TextBlock? _phoneDestination;
    private Button? _phoneChoose;
    private TextBlock? _phoneStatus;
    private Button? _phonePublish;
    private Border? _phoneBarRow;
    private FlatBar? _phoneBar;
    private StackPanel? _phoneLink;
    private PhonePublishState _phonePub = PhonePublishState.None;
    private PhoneLinkState? _phoneLinkShown;
    private bool _phoneAsked;
    private bool _phoneConfirmRestart;
    private bool _phoneConfirmHandOver;
    private TextBlock? _phoneCountdown;
    // Kept across rebuilds, so what is typed survives the pack's answer changing.
    private FakeInput? _phoneLocal, _phonePublic, _phoneNewLabel, _phoneNewPath;
    private FakeInput? _phoneFocused;
    private Button? _phoneClaim, _phoneAdd;
    // The status tick follows the pack's index events, which a claim or a code
    // does not raise: while the card is on screen it is also read once a second
    // (the Mac's TimelineView under the code), and not at all while it is not.
    private Microsoft.UI.Dispatching.DispatcherQueueTimer? _phoneTimer;

    private void AddPhoneSection()
    {
        if (!_api.HasPhone) return;  // a pack from before these entries
        StackPanel body = Section("Phone access");
        _phoneSection = body;

        _phoneDestination = _look.Text("", 12);
        _phoneChoose = _look.Button("Choose…", () => _ = ChoosePhoneFolder());
        body.Children.Add(Row("Folder on your NAS", _phoneDestination, _phoneChoose));

        _phoneStatus = _look.Text("", 12);
        _phonePublish = _look.Button("Publish now", PublishOrCancel);
        body.Children.Add(Row("Catalogue", _phoneStatus, _phonePublish));

        _phoneBar = new FlatBar(_look[AddonColour.Hairline], _look[AddonColour.Accent], height: 3);
        _phoneBarRow = new Border { Child = _phoneBar.Root, Padding = new Thickness(12, 0, 12, 12), Visibility = Visibility.Collapsed };
        body.Children.Add(_phoneBarRow);

        _phoneLink = new StackPanel();
        body.Children.Add(_phoneLink);

        _phoneLocal = PhoneInput("nas.local:7878", 240, "Address on your network");
        _phonePublic = PhoneInput("photos.example.com:7878", 240, "Address from outside");
        _phoneNewLabel = PhoneInput("Label", 120, "Name of the new shared folder");
        _phoneNewPath = PhoneInput("Folder on the NAS, like /volume1/Video", 0, "Path of the new shared folder on the NAS");
        _phoneLocal.Changed += PhoneFieldsChanged;
        _phoneLocal.Submitted += SavePhoneAddresses;
        _phonePublic.Submitted += SavePhoneAddresses;
        _phoneNewPath.Changed += PhoneFieldsChanged;
        _phoneNewPath.Submitted += AddPhoneFolder;
        _phoneNewLabel.Submitted += AddPhoneFolder;

        _phoneTimer = Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread()?.CreateTimer();
        if (_phoneTimer is not null)
        {
            _phoneTimer.Interval = TimeSpan.FromSeconds(1);
            _phoneTimer.IsRepeating = true;
            _phoneTimer.Tick += (_, _) => PollPhone();
            body.Loaded += (_, _) => _phoneTimer.Start();
            body.Unloaded += (_, _) => _phoneTimer.Stop();
        }
        PollPhone();
    }

    private FakeInput PhoneInput(string placeholder, double width, string name)
    {
        var field = new FakeInput(_look.Input(12), placeholder, width);
        AutomationProperties.SetName(field, name);
        field.GotFocus += (_, _) => _phoneFocused = field;
        field.LostFocus += (_, _) =>
        {
            if (_phoneFocused == field) _phoneFocused = null;
        };
        return field;
    }

    // ---- reading the pack (status tick, 4 Hz) ----

    private void PollPhone()
    {
        if (_phoneSection is null) return;
        try { _phonePub = PhoneWords.ParsePublish(_api.PhoneJson()); }
        catch (MediaViewerException) { _phonePub = PhonePublishState.None; }
        ShowPhonePublish();

        bool linkOn = _phonePub.Destination.Length > 0 && _api.HasPhoneLink;
        if (!linkOn)
        {
            if (_phoneLinkShown is not null)
            {
                _phoneLinkShown = null;
                DetachPhoneInputs();
                _phoneLink!.Children.Clear();
            }
            return;
        }
        if (!_phoneAsked)
        {
            _phoneAsked = true;
            PhoneRequest(("do", "refresh"));
            return;  // PhoneRequest polled
        }
        PhoneLinkState? link = null;
        try { link = PhoneWords.ParseLink(_api.PhoneLinkJson()); }
        catch (MediaViewerException) { }
        if (link is null) return;
        if (_phoneLinkShown is null || !_phoneLinkShown.SameAs(link))
        {
            // The pack's addresses fill an empty field, as the Mac's onChange does.
            if (_phoneLocal!.Text.Length == 0 && link.Local.Length > 0) _phoneLocal.SetText(link.Local);
            if (_phonePublic!.Text.Length == 0 && link.Public.Length > 0) _phonePublic.SetText(link.Public);
            _phoneLinkShown = link;
            ShowPhoneLink(link);
        }
        if (_phoneCountdown is not null)
            _phoneCountdown.Text = PhoneWords.Countdown(link.QrExpires, DateTimeOffset.UtcNow.ToUnixTimeSeconds());
    }

    private void ShowPhonePublish()
    {
        PhonePublishState s = _phonePub;
        _phoneDestination!.Text = s.Destination.Length == 0 ? "Where your phone will read the catalogue from." : s.Destination;
        _phoneChoose!.Content = s.Destination.Length == 0 ? "Choose…" : "Change…";
        _phoneChoose.IsEnabled = !s.Running;
        _phoneStatus!.Text = PhoneWords.PublishStatus(s, DateTimeOffset.UtcNow.ToUnixTimeSeconds());
        string action = s.Running ? "Cancel" : "Publish now";
        if (_phonePublish!.Content as string != action)
        {
            _phonePublish.Content = action;
            AutomationProperties.SetName(_phonePublish, action);
        }
        _phoneBarRow!.Visibility = s.Running ? Visibility.Visible : Visibility.Collapsed;
        if (s.Running) _phoneBar!.Value = s.Fraction;
    }

    // ---- the catalogue ----

    private void PublishOrCancel()
    {
        if (_phonePub.Running)
        {
            try { _api.PhoneCancel(); }
            catch (MediaViewerException) { }
            PollPhone();
            return;
        }
        if (_phonePub.Destination.Length == 0) _ = ChoosePhoneFolder();
        else PublishPhone(_phonePub.Destination);
    }

    private async Task ChoosePhoneFolder()
    {
        var picker = new FolderPicker();
        picker.FileTypeFilter.Add("*");
        WinRT.Interop.InitializeWithWindow.Initialize(picker, _chrome.Host.MainWindow);
        Windows.Storage.StorageFolder? folder;
        try { folder = await picker.PickSingleFolderAsync(); }
        catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or UnauthorizedAccessException)
        {
            return;
        }
        // A library (Pictures in the side bar) has no path: not a NAS folder.
        if (folder is null || string.IsNullOrEmpty(folder.Path)) return;
        PublishPhone(folder.Path);
    }

    private void PublishPhone(string destination)
    {
        try { _api.PhonePublish(destination); }
        catch (MediaViewerException) { }  // Busy: one is running; the poll shows it
        PollPhone();
    }

    // ---- the link ----

    private void PhoneRequest(params (string Key, string Value)[] fields)
    {
        var w = new System.Text.Json.Nodes.JsonObject();
        foreach ((string k, string v) in fields) w[k] = v;
        try { _api.PhoneLink(w.ToJsonString()); }
        catch (MediaViewerException) { }
        PollPhone();
    }

    private void SavePhoneAddresses()
    {
        if (_phoneLinkShown is not { Claimed: true }) return;
        PhoneRequest(("do", "set_addresses"), ("local", _phoneLocal!.Text), ("public", _phonePublic!.Text));
    }

    private void PhoneFieldsChanged()
    {
        bool busy = _phoneLinkShown is { Busy.Length: > 0 };
        if (_phoneClaim is not null) _phoneClaim.IsEnabled = !busy && _phoneLocal!.Text.Length > 0;
        string path = _phoneNewPath!.Text;
        _phoneNewLabel!.PlaceholderText = path.Length == 0 ? "Label" : PhoneWords.ShareLabel(path);
        if (_phoneAdd is not null)
            _phoneAdd.IsEnabled = !busy && path.StartsWith('/') && PhoneWords.ShareLabel(path).Length > 0;
    }

    private void AddPhoneFolder()
    {
        string path = _phoneNewPath!.Text;
        if (_phoneAdd is not { IsEnabled: true } || !path.StartsWith('/')) return;
        string label = _phoneNewLabel!.Text.Length == 0 ? PhoneWords.ShareLabel(path) : _phoneNewLabel.Text;
        _phoneNewLabel.SetText("");
        _phoneNewPath.SetText("");
        PhoneRequest(("do", "add_folder"), ("label", label), ("path", path));
    }

    // The Mac's "Get Driftwire…": Settings on Driftwire's page.
    private void GetDriftwire()
    {
        try { ShowAddonPage(_chrome.Host, "driftwire"); }
        catch (TypeLoadException)
        {
            // A host older than IAddonPages: Driftwire is in Settings → Add-ons.
        }
    }

    // Its own method so that on a host whose Interop has no IAddonPages, only
    // this call fails to load (caught above), as ModelPicker.RequestPieces.
    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    private static void ShowAddonPage(IAddonHost2 host, string id)
    {
        if (host is IAddonPages pages) pages.ShowAddonPage(id);
    }

    private void DetachPhoneInputs()
    {
        foreach (FakeInput? f in new[] { _phoneLocal, _phonePublic, _phoneNewLabel, _phoneNewPath })
            if (f?.Parent is Panel p) p.Children.Remove(f);
    }

    private void ShowPhoneLink(PhoneLinkState s)
    {
        FakeInput? refocus = _phoneFocused;
        DetachPhoneInputs();
        StackPanel link = _phoneLink!;
        link.Children.Clear();
        _phoneCountdown = null;
        _phoneClaim = null;
        _phoneAdd = null;
        bool busy = s.Busy.Length > 0;
        // A question whose subject went away goes with it.
        if (!s.OtherPublisher) _phoneConfirmRestart = false;
        if (s.AskingName.Length == 0) _phoneConfirmHandOver = false;

        if (!s.Driftwire)
        {
            link.Children.Add(Row("Driftwire", "Phone access reaches your NAS and your phone through it.",
                                  _look.Button("Get Driftwire…", GetDriftwire)));
        }
        else
        {
            if (PhoneWords.BusyText(s.Busy) is string line) link.Children.Add(PhoneSweep(line));
            if (s.Mismatch)
            {
                link.Children.Add(Row("The share doesn't match this PC",
                    "Its library.json names another library or an older epoch, perhaps after a restore. This PC goes on publishing its own; phones will refuse what doesn't match until it is fixed.",
                    new Border()));
            }
            if (s.OtherPublisher) AddOtherPublisher(link, s, busy);
            if (s.AskingName.Length > 0) AddAsking(link, s, busy);
            if (!s.Claimed) AddClaim(link, busy);
            else AddClaimed(link, s, busy);
        }
        if (PhoneWords.ErrorText(s.Error) is string e) link.Children.Add(PhoneNote(e));
        PhoneFieldsChanged();
        if (refocus?.Parent is not null) refocus.Focus(FocusState.Programmatic);
    }

    private void AddClaim(StackPanel link, bool busy)
    {
        _phoneClaim = _look.Button("Claim", () =>
            PhoneRequest(("do", "claim"), ("local", _phoneLocal!.Text), ("public", _phonePublic!.Text)));
        link.Children.Add(Row("Service on your NAS",
            "Claim it from the NAS's own network, once. This PC writes a one-time token to the share to prove it can.",
            _phoneClaim));
        link.Children.Add(PhoneField("Address on your network", _phoneLocal!));
        AddPublicField(link);
    }

    private void AddPublicField(StackPanel link)
    {
        link.Children.Add(PhoneField("Address from outside", _phonePublic!));
        if (PhoneWords.WarningText(_phoneLinkShown?.Warning ?? "") is string w) link.Children.Add(PhoneNote(w));
    }

    private void AddClaimed(StackPanel link, PhoneLinkState s, bool busy)
    {
        if (s.Qr.Length == 0)
        {
            Button show = _look.Button("Show code", () => PhoneRequest(("do", "pair")));
            show.IsEnabled = !busy;
            link.Children.Add(Row("Add a phone", PhoneWords.AddDetail(s), show));
        }
        else
        {
            link.Children.Add(CodeBlock(s));
        }
        foreach (PhoneDevice phone in s.Phones)
        {
            string key = phone.Key;
            Button remove = _look.Button("Remove", () => PhoneRequest(("do", "revoke"), ("key", key)));
            remove.IsEnabled = !busy;
            AutomationProperties.SetName(remove, $"Remove {phone.Name}");
            link.Children.Add(Row(phone.Name, PhoneWords.KeyDetail(phone.Key), remove));
        }
        if (!s.Reachable) link.Children.Add(PhoneNote("Couldn't reach the NAS. These are the phones it knew about last."));
        link.Children.Add(PhoneField("Address on your network", _phoneLocal!));
        AddPublicField(link);

        foreach (PhoneFolder f in s.Folders)
        {
            FrameworkElement control;
            if (f.IsCatalogue) control = new Border();
            else
            {
                string id = f.Id;
                Button stop = _look.Button("Stop sharing", () => PhoneRequest(("do", "remove_folder"), ("id", id)));
                stop.IsEnabled = !busy;
                AutomationProperties.SetName(stop, $"Stop sharing {f.Label}");
                control = stop;
            }
            link.Children.Add(Row(f.Label, f.Path, control));
        }
        var add = new Grid { ColumnSpacing = 8, Padding = new Thickness(12) };
        add.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        add.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        add.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        add.Children.Add(_phoneNewLabel!);
        Grid.SetColumn(_phoneNewPath!, 1);
        add.Children.Add(_phoneNewPath!);
        _phoneAdd = _look.Button("Add", AddPhoneFolder);
        AutomationProperties.SetName(_phoneAdd, "Add the shared folder");
        Grid.SetColumn(_phoneAdd, 2);
        add.Children.Add(_phoneAdd);
        link.Children.Add(add);
    }

    // The code, the check code beside it, and how long it is good for.
    private FrameworkElement CodeBlock(PhoneLinkState s)
    {
        var grid = new Grid { ColumnSpacing = 16, Padding = new Thickness(12) };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.Children.Add(QrImage(s.Qr, 240));

        var words = new StackPanel { Spacing = 6 };
        words.Children.Add(_look.Text("Scan with MediaViewer on your phone", 14, AddonColour.Title));
        words.Children.Add(_look.Text($"Your phone will show {s.QrLibrary} and this code. Check they match before you confirm.", 12));
        TextBlock check = _look.Text(s.QrFingerprint, 13, AddonColour.Title);
        check.FontFamily = new FontFamily("Cascadia Mono, Consolas");
        check.IsTextSelectionEnabled = true;
        AutomationProperties.SetName(check, "Check code");
        words.Children.Add(check);
        _phoneCountdown = _look.Text(PhoneWords.Countdown(s.QrExpires, DateTimeOffset.UtcNow.ToUnixTimeSeconds()), 12);
        words.Children.Add(_phoneCountdown);
        Button cancel = _look.Button("Cancel", () => PhoneRequest(("do", "pair_stop")));
        AutomationProperties.SetName(cancel, "Cancel the code");
        words.Children.Add(cancel);
        Grid.SetColumn(words, 1);
        grid.Children.Add(words);
        return grid;
    }

    /// <summary>
    /// The QR on white with a 12-DIP quiet zone (the Mac's QRImage). The modules
    /// are one path of row runs, each grown by a hair so neighbours overlap and no
    /// anti-aliased seam shows between them. Encoded on a worker; it is bounded
    /// work, but it is not the UI thread's.
    /// </summary>
    private static Border QrImage(string text, double side)
    {
        var box = new Border
        {
            Width = side,
            Height = side,
            CornerRadius = new CornerRadius(8),
            Background = new SolidColorBrush(Microsoft.UI.Colors.White),
            Padding = new Thickness(12),
        };
        AutomationProperties.SetName(box, "Code for your phone to scan");
        Microsoft.UI.Dispatching.DispatcherQueue ui = Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread();
        _ = Task.Run(() =>
        {
            QrCode? q = QrCode.Encode(text);
            if (q is null) return;
            var runs = new List<(int X, int Y, int Length)>();
            for (int y = 0; y < q.Size; ++y)
                for (int x = 0; x < q.Size;)
                {
                    if (!q[x, y]) { ++x; continue; }
                    int start = x;
                    while (x < q.Size && q[x, y]) ++x;
                    runs.Add((start, y, x - start));
                }
            ui.TryEnqueue(() =>
            {
                double inner = side - 24;
                double m = inner / q.Size;
                const double hair = 0.02;
                var geometry = new PathGeometry { FillRule = FillRule.Nonzero };
                foreach ((int x, int y, int n) in runs)
                {
                    double x0 = x * m, y0 = y * m, x1 = (x + n + hair) * m, y1 = (y + 1 + hair) * m;
                    var figure = new PathFigure { StartPoint = new Windows.Foundation.Point(x0, y0), IsClosed = true, IsFilled = true };
                    figure.Segments.Add(new LineSegment { Point = new Windows.Foundation.Point(x1, y0) });
                    figure.Segments.Add(new LineSegment { Point = new Windows.Foundation.Point(x1, y1) });
                    figure.Segments.Add(new LineSegment { Point = new Windows.Foundation.Point(x0, y1) });
                    geometry.Figures.Add(figure);
                }
                box.Child = new Microsoft.UI.Xaml.Shapes.Path
                {
                    Data = geometry,
                    Fill = new SolidColorBrush(Microsoft.UI.Colors.Black),
                    Width = inner,
                    Height = inner,
                };
            });
        });
        return box;
    }

    // ---- another computer ----

    private void AddOtherPublisher(StackPanel link, PhoneLinkState s, bool busy)
    {
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        Button ask = _look.Button("Ask to hand over", () => PhoneRequest(("do", "takeover_ask")));
        ask.IsEnabled = !busy;
        Button restart = _look.Button("Start over…", () =>
        {
            _phoneConfirmRestart = true;
            RedrawPhoneLink();
        });
        restart.IsEnabled = !busy;
        buttons.Children.Add(ask);
        buttons.Children.Add(restart);
        link.Children.Add(Row("Published from another computer",
            $"Key {s.OtherFingerprint}. Ask it to hand over, and phones follow by themselves. If it's gone, start over and scan each phone again.",
            buttons));
        if (_phoneConfirmRestart)
        {
            link.Children.Add(ConfirmRow("Start over from this PC? Every phone will have to scan a new code.", "Start over", () =>
            {
                _phoneConfirmRestart = false;
                PhoneRequest(("do", "takeover_restart"));
            }, () =>
            {
                _phoneConfirmRestart = false;
                RedrawPhoneLink();
            }, focus: true));
        }
    }

    private void AddAsking(StackPanel link, PhoneLinkState s, bool busy)
    {
        Button hand = _look.Button("Hand over…", () =>
        {
            _phoneConfirmHandOver = true;
            RedrawPhoneLink();
        });
        hand.IsEnabled = !busy;
        link.Children.Add(Row($"{s.AskingName} wants to publish this library",
            $"Its key is {s.AskingFingerprint}. Check it matches what that computer shows.", hand));
        if (_phoneConfirmHandOver)
        {
            string key = s.AskingKey;
            link.Children.Add(ConfirmRow($"Hand this library to {s.AskingName}? This PC will stop publishing it. Phones follow by themselves.",
                "Hand over", () =>
                {
                    _phoneConfirmHandOver = false;
                    PhoneRequest(("do", "takeover_allow"), ("key", key));
                }, () =>
                {
                    _phoneConfirmHandOver = false;
                    RedrawPhoneLink();
                }, focus: true, destructive: false));
        }
    }

    // ---- pieces ----

    private Grid PhoneField(string title, FakeInput input)
    {
        var grid = new Grid { ColumnSpacing = 20, Padding = new Thickness(12) };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        TextBlock label = _look.Text(title, 14, AddonColour.Title);
        label.VerticalAlignment = VerticalAlignment.Center;
        grid.Children.Add(label);
        input.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(input, 1);
        grid.Children.Add(input);
        return grid;
    }

    private void RedrawPhoneLink()
    {
        if (_phoneLinkShown is { } shown) ShowPhoneLink(shown);
    }

    private TextBlock PhoneNote(string text)
    {
        TextBlock t = _look.Text(text, 12);
        t.Margin = new Thickness(12, 0, 12, 12);
        return t;
    }

    /// <summary>
    /// The progress language's line with no fraction (docs/design/20 "Craft
    /// rules"): a 3 DIP rounded track, a calm 1.8 s accent sweep, one line under
    /// it. With Windows' animations off the sweep is a still, as JobBar's.
    /// </summary>
    private Border PhoneSweep(string text)
    {
        const double height = 3;
        var slide = new TranslateTransform();
        var pulse = new Border
        {
            Background = _look[AddonColour.Accent],
            CornerRadius = new CornerRadius(height / 2),
            HorizontalAlignment = HorizontalAlignment.Left,
            RenderTransform = slide,
        };
        var track = new Grid { Height = height, Background = _look[AddonColour.Hairline], CornerRadius = new CornerRadius(height / 2) };
        track.Children.Add(pulse);
        Storyboard? sweep = null;
        bool animate = _chrome.Host.AnimationsEnabled;
        track.SizeChanged += (_, e) =>
        {
            double w = e.NewSize.Width;
            track.Clip = new RectangleGeometry { Rect = new Windows.Foundation.Rect(0, 0, w, height) };
            pulse.Width = w * (animate ? 0.3 : 0.5);
            sweep?.Stop();
            sweep = null;
            if (!animate || w <= 0) return;
            var move = new DoubleAnimation
            {
                From = -pulse.Width,
                To = w,
                Duration = new Duration(TimeSpan.FromMilliseconds(1800)),
                RepeatBehavior = RepeatBehavior.Forever,
                EasingFunction = new SineEase { EasingMode = EasingMode.EaseInOut },
            };
            Storyboard.SetTarget(move, slide);
            Storyboard.SetTargetProperty(move, "X");
            sweep = new Storyboard();
            sweep.Children.Add(move);
            sweep.Begin();
        };
        track.Unloaded += (_, _) =>
        {
            sweep?.Stop();
            sweep = null;
        };
        var column = new StackPanel { Spacing = 6 };
        column.Children.Add(track);
        column.Children.Add(_look.Text(text, 12));
        var row = new Border { Child = column, Padding = new Thickness(12) };
        AutomationProperties.SetName(row, text);
        return row;
    }
}
