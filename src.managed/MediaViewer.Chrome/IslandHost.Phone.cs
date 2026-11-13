// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using MediaViewer.Interop;
using MediaViewer.Shared;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Storage.Pickers;

namespace MediaViewer.Chrome;

/// <summary>
/// Settings → Phone access, part of the app (docs/plans/phone-in-the-app.md,
/// owner 2026-11-12), with or without Local search: the NAS folder, what the
/// phone searches, the service on the NAS (claim, addresses, named folders),
/// the code a phone scans with its check code, the paired phones and Remove,
/// and handing the library to or from another computer. The Mac twin is
/// PhoneAccessView.swift; the words are Shared/PhoneWords.cs, checked against it.
/// </summary>
/// <remarks>
/// Every request is mv_phone_request ([no-block]: the app's phone thread does
/// the work) and the state is read from memory (mv_phone_state_json) every half
/// second while Settings is open, never while it is not. The signing key never
/// passes through here: the core keeps it in Credential Manager sealed with
/// DPAPI (wire/secrets_win.cpp). Text entry is FakeInput (a TextBox fail-fasts in
/// this island). The rows are rebuilt only when the state changes; typed text
/// survives, and the focused field or button is focused again once laid out.
/// Keys and addresses shown here never reach a log (rule 6).
/// </remarks>
public static partial class IslandHost
{
    private static StackPanel? _phoneRows;
    private static PhoneLinkState? _phoneShown;
    private static Microsoft.UI.Dispatching.DispatcherQueueTimer? _phoneTimer;
    private static FakeInput? _phoneLocal, _phonePublic, _phoneNewLabel, _phoneNewPath;
    // Phone backup (1.1): a new backup folder's sizes, and whether the new folder is one.
    private static FakeInput? _phoneNewLimit, _phoneNewReserve;
    private static bool _phoneNewIsBackup;
    // The NAS passphrase: never kept here or in the core (owner call C2), only typed.
    private static FakeInput? _phonePass, _phonePassAgain, _phoneUnlock;
    // The person's own relay: its key goes to the NAS and is kept nowhere here.
    private static FakeInput? _phoneRelayAddress, _phoneRelayKey;
    private static bool _phoneRelayEditing;
    private static Button? _phoneRelaySave;
    private static ComboBox? _phoneCodeFor;
    private static bool _phoneChanging;
    // A refused action asked for the passphrase: the field stays until an unlock
    // lands or the person cancels (the core clears its error as each request starts).
    private static bool _phoneUnlockWanted;
    private static Button? _phoneUnlockGo;
    private static readonly List<Flyout> PhoneFlyouts = new();
    private static string? _phoneFocus;  // the automation name of the focused control
    private static TextBlock? _phoneCountdown;
    private static ProgressLine? _phonePublishLine;
    private static Button? _phoneClaim, _phoneAdd;
    private static bool _phoneConfirmRestart, _phoneConfirmHandOver;

    private static void AddPhoneSettings(StackPanel view)
    {
        view.Children.Add(SettingsSection("Phone access"));
        _phoneRows = new StackPanel { Spacing = Space(8) };
        view.Children.Add(_phoneRows);
        _phoneLocal = PhoneInput("nas.local:7878", "Address on your network");
        _phonePublic = PhoneInput("photos.example.com:7878", "Address from outside");
        _phoneNewLabel = PhoneInput("Label", "Name of the new shared folder", 120);
        _phoneNewPath = PhoneInput("Folder on the NAS, like /volume1/Video", "Path of the new shared folder on the NAS", 0);
        _phoneNewLimit = PhoneInput("GB", "Storage limit in gigabytes", 70);
        _phoneNewReserve = PhoneInput("GB", "Free space to keep on the NAS, in gigabytes", 60);
        _phoneNewReserve.SetText("1");
        _phoneNewLimit.Changed += PhoneFieldsChanged;
        _phoneNewReserve.Changed += PhoneFieldsChanged;
        _phoneLocal.Changed += PhoneFieldsChanged;
        _phoneLocal.Submitted += SavePhoneAddresses;
        _phonePublic.Submitted += SavePhoneAddresses;
        _phoneNewPath.Changed += PhoneFieldsChanged;
        _phoneNewPath.Submitted += AddPhoneFolder;
        _phoneNewLabel.Submitted += AddPhoneFolder;
        _phonePass = PhoneSecret("Passphrase", "NAS passphrase");
        _phonePassAgain = PhoneSecret("Again", "The passphrase again");
        _phoneUnlock = PhoneSecret("NAS passphrase", "NAS passphrase to unlock");
        _phonePass.Changed += PhoneFieldsChanged;
        _phonePassAgain.Changed += PhoneFieldsChanged;
        _phoneUnlock.Submitted += UnlockPhone;
        _phoneUnlock.Changed += PhoneFieldsChanged;
        _phoneRelayAddress = PhoneInput("relay.example.com:7879", "Relay address");
        _phoneRelayKey = PhoneSecret("Relay key", "Relay key");
        _phoneRelayAddress.Changed += PhoneFieldsChanged;
        _phoneRelayKey.Changed += PhoneFieldsChanged;
        _phoneRelayKey.Submitted += SavePhoneRelay;
        _phoneCodeFor = new ComboBox { FontFamily = UiFont, FontSize = UiFontSize, Foreground = Brush(Title), Width = 170 };
        _phoneCodeFor.Items.Add(PhoneWords.Unlimited);
        foreach ((string label, long _) in PhoneWords.Durations) _phoneCodeFor.Items.Add(label);
        _phoneCodeFor.SelectedIndex = 0;
        AutomationProperties.SetName(_phoneCodeFor, "How long the phone may use this library");
    }

    private static FakeInput PhoneSecret(string placeholder, string name)
    {
        var field = new FakeInput(placeholder, 240) { Secret = true };
        AutomationProperties.SetName(field, name);
        TrackPhoneFocus(field, name);
        return field;
    }

    private static FakeInput PhoneInput(string placeholder, string name, double width = 240)
    {
        var field = new FakeInput(placeholder, width);
        AutomationProperties.SetName(field, name);
        TrackPhoneFocus(field, name);
        return field;
    }

    private static void TrackPhoneFocus(Control c, string name)
    {
        c.GotFocus += (_, _) => _phoneFocus = name;
        c.LostFocus += (_, _) =>
        {
            if (_phoneFocus == name) _phoneFocus = null;
        };
    }

    /// <summary>PresentSettings: read now, and every half second while Settings shows.</summary>
    private static void RefreshPhone()
    {
        if (_phoneRows is null) return;
        PollPhone();
        if (_phoneTimer is null && _dispatcher is not null)
        {
            _phoneTimer = _dispatcher.DispatcherQueue.CreateTimer();
            _phoneTimer.Interval = TimeSpan.FromMilliseconds(500);
            _phoneTimer.IsRepeating = true;
            _phoneTimer.Tick += (_, _) =>
            {
                if (!_settingsVisible)
                {
                    _phoneTimer?.Stop();  // Settings closed: nothing is read until it opens again
                    ForgetPhoneSecrets();
                    return;
                }
                PollPhone();
            };
        }
        _phoneTimer?.Start();
    }

    // Typed and never sent: gone when Settings closes or its row does.
    private static void ForgetPhoneSecrets()
    {
        _phonePass?.SetText("");
        _phonePassAgain?.SetText("");
        _phoneUnlock?.SetText("");
        _phoneRelayKey?.SetText("");
        _phoneRelayEditing = false;
        _phoneChanging = false;
        _phoneUnlockWanted = false;
    }

    private static void PollPhone()
    {
        PhoneLinkState? s = PhoneWords.ParseLink(PhoneNative.StateJson());
        if (s is null) return;
        if (s.Error == "needs_unlock") _phoneUnlockWanted = true;
        if (s.UnlockedSeconds > 0 && _phoneUnlockWanted)
        {
            _phoneUnlockWanted = false;
            _phoneUnlock?.SetText("");
            _phoneShown = null;  // the field goes: draw again
        }
        if (_phoneShown is null || !_phoneShown.SameAs(s))
        {
            // The core's addresses fill an empty field.
            if (_phoneLocal!.Text.Length == 0 && s.Local.Length > 0) _phoneLocal.SetText(s.Local);
            if (_phonePublic!.Text.Length == 0 && s.Public.Length > 0) _phonePublic.SetText(s.Public);
            _phoneShown = s;
            ShowPhone(s);
        }
        if (_phoneCountdown is not null)
            _phoneCountdown.Text = PhoneWords.Countdown(s.QrExpires, DateTimeOffset.UtcNow.ToUnixTimeSeconds());
        if (_phonePublishLine is not null && s.Catalogue is { Running: true } c) ShowPublishProgress(c);
    }

    private static void PhoneRequest(params (string Key, string Value)[] fields)
    {
        var o = new System.Text.Json.Nodes.JsonObject();
        foreach ((string k, string v) in fields) o[k] = v;
        PhoneRequest(o);
    }

    private static void PhoneRequest(System.Text.Json.Nodes.JsonObject o)
    {
        _ = PhoneNative.Request(o.ToJsonString());
        PollPhone();
    }

    // ---- the rows ----

    private static void DetachPhoneInputs()
    {
        foreach (FrameworkElement? f in new FrameworkElement?[]
                 { _phoneLocal, _phonePublic, _phoneNewLabel, _phoneNewPath, _phoneNewLimit, _phoneNewReserve, _phonePass, _phonePassAgain, _phoneUnlock,
                   _phoneRelayAddress, _phoneRelayKey, _phoneCodeFor })
            if (f?.Parent is Panel p) p.Children.Remove(f);
    }

    private static void ShowPhone(PhoneLinkState s)
    {
        StackPanel rows = _phoneRows!;
        string? refocus = _phoneFocus;
        foreach (Flyout f in PhoneFlyouts) f.Hide();  // its button leaves the tree now
        PhoneFlyouts.Clear();
        DetachPhoneInputs();
        rows.Children.Clear();
        _phoneCountdown = null;
        _phonePublishLine = null;
        _phoneClaim = null;
        _phoneAdd = null;
        _phoneSetPass = null;
        _phoneUnlockGo = null;
        _phoneRelaySave = null;
        bool busy = s.Busy.Length > 0;
        if (!s.OtherPublisher) _phoneConfirmRestart = false;
        if (s.AskingName.Length == 0) _phoneConfirmHandOver = false;

        if (s.Secondary)
        {
            rows.Children.Add(PhoneNote("Set up in the first MediaViewer window."));
            return;
        }
        if (s.Starting)
        {
            rows.Children.Add(PhoneNote("Starting…"));
            return;
        }
        if (!s.Driftwire)
        {
            rows.Children.Add(PhoneNote("This copy of MediaViewer can't keep phone keys."));
            return;
        }
        if (PhoneWords.BusyText(s.Busy) is string line)
        {
            var progress = new ProgressLine();
            progress.Show(line, null);
            rows.Children.Add(progress.Root);
        }
        Button choose = PhoneButton(s.Folder.Length == 0 ? "Choose…" : "Change…", () => _ = ChoosePhoneFolder(), "Choose the folder on your NAS");
        choose.IsEnabled = !busy && s.Catalogue is not { Running: true };
        rows.Children.Add(SettingsRow("Folder on your NAS", PhoneWords.FolderDetail(s), choose, keepName: true));
        if (s.Folder.Length > 0)
        {
            AddPhoneSearch(rows, s, busy);
            if (s.Mismatch)
            {
                rows.Children.Add(SettingsRow("The share doesn't match this PC",
                    "Its library.json names another library or an older epoch, perhaps after a restore. This PC goes on publishing its own; phones will refuse what doesn't match until it is fixed.",
                    new Border()));
            }
            if (s.OtherPublisher) AddPhoneOtherPublisher(rows, s, busy);
            if (s.AskingName.Length > 0) AddPhoneAsking(rows, s, busy);
            if (!s.Claimed) AddPhoneClaim(rows, busy);
            else AddPhoneClaimed(rows, s, busy);
        }
        if (PhoneWords.ErrorText(s, PhoneClock) is string e) rows.Children.Add(PhoneNote(e));
        // A secret whose row is not drawn holds nothing.
        if (!IsInTree(_phonePass)) { _phonePass!.SetText(""); _phonePassAgain!.SetText(""); }
        if (!IsInTree(_phoneUnlock)) _phoneUnlock!.SetText("");
        PhoneFieldsChanged();
        RefocusPhone(rows, refocus);
    }

    private static bool IsInTree(FrameworkElement? e) => e?.Parent is not null;

    private static string PhoneClock(long unix) => DateTimeOffset.FromUnixTimeSeconds(unix).ToLocalTime().ToString("t");

    private static string PhoneWhen(long unix)
    {
        DateTimeOffset t = DateTimeOffset.FromUnixTimeSeconds(unix).ToLocalTime();
        return t.ToString("ddd ") + t.ToString("t");  // the person's own clock: 14:00 or 2:00 PM
    }

    // Once laid out: a control that has not loaded cannot take focus.
    private static void RefocusPhone(StackPanel rows, string? name)
    {
        if (name is null) return;
        DispatcherQueueControllerTryEnqueue(() =>
        {
            Control? target = FindPhoneControl(rows, name);
            target?.Focus(FocusState.Keyboard);
        });
    }

    private static Control? FindPhoneControl(DependencyObject root, string name)
    {
        int n = VisualTreeHelper.GetChildrenCount(root);
        for (int i = 0; i < n; ++i)
        {
            DependencyObject child = VisualTreeHelper.GetChild(root, i);
            if (child is Control c && AutomationProperties.GetName(c) == name) return c;
            if (FindPhoneControl(child, name) is Control found) return found;
        }
        return null;
    }

    private static Button PhoneButton(string label, Action click, string? name = null)
    {
        Button b = SettingsButton(label, click);
        string n = name ?? label;
        AutomationProperties.SetName(b, n);
        TrackPhoneFocus(b, n);
        return b;
    }

    private static TextBlock PhoneNote(string text)
    {
        TextBlock t = Label(text);
        t.FontSize = TypeSize(12);
        t.TextWrapping = TextWrapping.Wrap;
        return t;
    }

    private static void AddPhoneSearch(StackPanel rows, PhoneLinkState s, bool busy)
    {
        FrameworkElement action;
        if (!s.LocalSearch)
        {
            action = PhoneButton("Get Local search…", () =>
            {
                _showGeneralSettings?.Invoke();
                OpenAddonPage(LocalSearchPageId);
            });
        }
        else if (s.Catalogue is { Running: true })
        {
            action = new Border();  // the progress line below carries Cancel
        }
        else
        {
            Button publish = PhoneButton("Publish now", () => PhoneRequest(("do", "publish")));
            publish.IsEnabled = !busy;
            action = publish;
        }
        rows.Children.Add(SettingsRow("Search on your phone",
            PhoneWords.SearchDetail(s, DateTimeOffset.UtcNow.ToUnixTimeSeconds()), action, keepName: action is Button));
        if (s.Catalogue is { Running: true } c)
        {
            // One progress line (docs/design/20 "Craft rules"), ending in a quiet Cancel.
            _phonePublishLine = new ProgressLine(("Cancel", () => PhoneRequest(("do", "cancel"))));
            ShowPublishProgress(c);
            rows.Children.Add(new Border { Child = _phonePublishLine.Root, Padding = Pad(12, 0, 12, 0) });
        }
    }

    private static void ShowPublishProgress(PhonePublishState c) =>
        _phonePublishLine?.Show(c.Phase == "sending" ? "Sending to the NAS" : "Building the catalogue",
                                Math.Clamp(c.Fraction, 0, 1), key: "phone-publish", timed: true);

    private static void AddPhoneClaim(StackPanel rows, bool busy)
    {
        _phoneClaim = PhoneButton("Claim", ClaimPhone, "Claim the service");
        _phoneClaim.IsEnabled = !busy;
        rows.Children.Add(SettingsRow("Service on your NAS",
            "Claim it from the NAS's own network, once. This PC writes a one-time token to the share to prove it can.",
            _phoneClaim, keepName: true));
        rows.Children.Add(SettingsRow("Address on your network", "", _phoneLocal!, keepName: true));
        AddPhonePublicField(rows);
        rows.Children.Add(SettingsRow("NAS passphrase",
            "The one you set on the NAS with driftwire serve init. If it has none, choose one here: enter it twice, at least 10 characters.",
            _phonePass!, keepName: true));
        rows.Children.Add(SettingsRow("Again, to choose it", "Leave empty when the NAS already has one.", _phonePassAgain!, keepName: true));
    }

    private static void ClaimPhone()
    {
        string request = PhoneWords.ClaimRequest(_phoneLocal!.Text, _phonePublic!.Text, _phonePass!.Text, _phonePassAgain!.Text);
        _phonePass.SetText("");
        _phonePassAgain.SetText("");
        _ = PhoneNative.Request(request);
        PollPhone();
    }

    private static void AddPhonePublicField(StackPanel rows)
    {
        rows.Children.Add(SettingsRow("Address from outside", "", _phonePublic!, keepName: true));
        if (PhoneWords.WarningText(_phoneShown?.Warning ?? "") is string w) rows.Children.Add(PhoneNote(w));
    }

    private static void AddPhoneClaimed(StackPanel rows, PhoneLinkState s, bool busy)
    {
        AddPhonePassphrase(rows, s, busy);
        if (_phoneUnlockWanted && s.NasPassphrase == "set")
        {
            var unlock = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8) };
            unlock.Children.Add(_phoneUnlock!);
            _phoneUnlockGo = PhoneButton("Unlock", UnlockPhone, "Unlock");
            unlock.Children.Add(_phoneUnlockGo);
            unlock.Children.Add(PhoneButton("Cancel", () =>
            {
                _phoneUnlockWanted = false;
                if (_phoneShown is { } shown) ShowPhone(shown);
            }, "Cancel the unlock"));
            rows.Children.Add(SettingsRow("Unlock",
                "Enter the NAS passphrase, then do that again. This PC keeps the key for 10 minutes and never the passphrase.",
                unlock, keepName: true));
        }
        if (s.Qr.Length == 0)
        {
            var code = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8) };
            _phoneCodeFor!.IsEnabled = s.NasHasFeatures;
            ToolTipService.SetToolTip(_phoneCodeFor, s.NasHasFeatures ? "How long the phone that scans it may use this library" : null);
            code.Children.Add(PhoneTipHolder(_phoneCodeFor, s.NasHasFeatures ? null : "Update the NAS service to use this"));
            Button show = PhoneButton("Show code", () =>
            {
                int i = _phoneCodeFor.SelectedIndex;
                long forS = s.NasHasFeatures && i > 0 ? PhoneWords.Durations[i - 1].Seconds : 0;
                _ = PhoneNative.Request(PhoneWords.PairRequest(forS));
                PollPhone();
            });
            show.IsEnabled = !busy;
            code.Children.Add(show);
            rows.Children.Add(SettingsRow("Add a phone", PhoneWords.AddDetail(s), code, keepName: true));
        }
        else
        {
            rows.Children.Add(PhoneCodeBlock(s));
        }
        long now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
        foreach (PhoneDevice phone in s.Phones)
        {
            string key = phone.Key;
            var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8) };
            buttons.Children.Add(PhoneTipHolder(PhoneAccessMenu(phone, s.NasHasFeatures, busy, now, s.NasHasBackup),
                                                s.NasHasFeatures ? null : "Update the NAS service to use this"));
            Button remove = PhoneButton("Remove", () => PhoneRequest(("do", "revoke"), ("key", key)), $"Remove {phone.Name}");
            remove.IsEnabled = !busy;
            buttons.Children.Add(remove);
            rows.Children.Add(SettingsRow(phone.Name, PhoneWords.PhoneDetail(phone, now, PhoneWhen), buttons, keepName: true));
        }
        if (!s.Reachable) rows.Children.Add(PhoneNote("Couldn't reach the NAS. These are the phones it knew about last."));
        rows.Children.Add(SettingsRow("Address on your network", "", _phoneLocal!, keepName: true));
        AddPhonePublicField(rows);
        AddPhoneRelay(rows, s, busy);
        foreach (PhoneFolder f in s.Folders)
        {
            var controls = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8) };
            string id = f.Id;
            if (f.Backup) controls.Children.Add(BackupLimitMenu(f, busy));
            if (!f.IsCatalogue)
            {
                Button stop = PhoneButton("Stop sharing", () => PhoneRequest(("do", "remove_folder"), ("id", id)), $"Stop sharing {f.Label}");
                stop.IsEnabled = !busy;
                controls.Children.Add(stop);
            }
            rows.Children.Add(SettingsRow(f.Backup ? $"{f.Label} · phone backup" : f.Label,
                                          f.Backup ? PhoneWords.BackupDetail(f) : f.Path, controls, keepName: !f.IsCatalogue));
        }
        var add = new Grid { ColumnSpacing = Space(8), Padding = Pad(12) };
        add.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        add.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        add.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        add.Children.Add(_phoneNewLabel!);
        Grid.SetColumn(_phoneNewPath!, 1);
        add.Children.Add(_phoneNewPath!);
        _phoneAdd = PhoneButton("Add", AddPhoneFolder, "Add the shared folder");
        Grid.SetColumn(_phoneAdd, 2);
        add.Children.Add(_phoneAdd);
        rows.Children.Add(add);
        // "Phones back up to this folder", with its limit and the free space to keep (§10b.1, B1, B5).
        var backup = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8), Padding = Pad(12) };
        Button toggle = PhoneButton(_phoneNewIsBackup ? "Phones back up to this folder: on" : "Phones back up to this folder: off",
                                    () =>
                                    {
                                        _phoneNewIsBackup = !_phoneNewIsBackup;
                                        if (_phoneNewIsBackup && _phoneNewLimit!.Text.Length == 0) _phoneNewLimit.SetText("1000");
                                        PollPhone();
                                    },
                                    "Phones back up their photos to this folder");
        toggle.IsEnabled = s.NasHasBackup && !busy;
        // A disabled control shows no tooltip (docs/design/20 "Craft rules"): its holder carries the reason.
        backup.Children.Add(PhoneTipHolder(toggle, s.NasHasBackup ? null : "Update the NAS service to use this"));
        if (_phoneNewIsBackup)
        {
            backup.Children.Add(PhoneNote("Limit"));
            backup.Children.Add(_phoneNewLimit!);
            backup.Children.Add(PhoneNote("GB, keep free"));
            backup.Children.Add(_phoneNewReserve!);
            backup.Children.Add(PhoneNote("GB"));
        }
        rows.Children.Add(backup);
    }

    // Extend (a timed phone) or Limit… (an unlimited one): a Button with a Flyout of
    // buttons (a DropDownButton or MenuFlyout fail-fasts in this island).
    // A disabled control shows no tooltip (docs/design/20 "Craft rules"): the
    // holder around it carries the reason.
    private static FrameworkElement PhoneTipHolder(FrameworkElement control, string? tip)
    {
        if (control.Parent is Panel p) p.Children.Remove(control);
        else if (control.Parent is Border b) b.Child = null;
        var holder = new Border { Child = control, Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
        if (tip is not null) ToolTipService.SetToolTip(holder, tip);
        return holder;
    }

    private static Button PhoneAccessMenu(PhoneDevice phone, bool features, bool busy, long now, bool backup = false)
    {
        string name = phone.Limited ? $"Extend {phone.Name}'s access" : $"Limit {phone.Name}'s access";
        Button menu = PhoneButton(phone.Limited ? "Extend" : "Limit…", () => { }, name);
        var column = new StackPanel { Spacing = Space(4) };
        var flyout = new Flyout { Content = column };
        string key = phone.Key;
        void Grant(long forS)
        {
            flyout.Hide();
            _ = PhoneNative.Request(PhoneWords.GrantRequest(key, forS));
            PollPhone();
        }
        // A grant counts from now: Extend offers only lengths that end later than now's end.
        foreach ((string label, long seconds) in PhoneWords.ExtendDurations(phone, now))
            column.Children.Add(SettingsButton(label + " from now", () => Grant(seconds)));
        if (phone.Limited) column.Children.Add(SettingsButton(PhoneWords.Unlimited, () => Grant(0)));
        // Phone backup (B2): turning it on adds access (the passphrase, once set); off never needs it.
        if (backup)
        {
            bool backupOn = phone.Backup;
            column.Children.Add(SettingsButton(backupOn ? "Stop backups from this phone" : "Let this phone back up", () =>
            {
                flyout.Hide();
                _ = PhoneNative.Request(PhoneWords.PhoneBackupRequest(key, !backupOn));
                PollPhone();
            }));
        }
        menu.Flyout = flyout;
        PhoneFlyouts.Add(flyout);
        menu.IsEnabled = !busy && features;
        return menu;
    }

    // "NAS passphrase · Locked", Lock, and Change… or Choose…, with its fields inline.
    private static void AddPhonePassphrase(StackPanel rows, PhoneLinkState s, bool busy)
    {
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8) };
        if (s.NasPassphrase == "set" && s.UnlockedSeconds > 0)
        {
            Button lockNow = PhoneButton("Lock", () => PhoneRequest(("do", "lock")), "Lock: forget the NAS key on this PC");
            lockNow.IsEnabled = !busy;
            buttons.Children.Add(lockNow);
        }
        else if (s.NasPassphrase == "set" && !_phoneUnlockWanted)
        {
            Button unlockNow = PhoneButton("Unlock…", () =>
            {
                _phoneUnlockWanted = true;
                if (_phoneShown is { } shown) ShowPhone(shown);
            }, "Unlock with the NAS passphrase");
            unlockNow.IsEnabled = !busy;
            buttons.Children.Add(unlockNow);
        }
        if (s.NasPassphrase is "set" or "none")
        {
            Button change = PhoneButton(s.NasPassphrase == "set" ? "Change…" : "Choose…", () =>
            {
                _phoneChanging = !_phoneChanging;
                if (_phoneShown is { } shown) ShowPhone(shown);
            }, s.NasPassphrase == "set" ? "Change the NAS passphrase" : "Choose a NAS passphrase");
            change.IsEnabled = !busy;
            buttons.Children.Add(change);
        }
        rows.Children.Add(SettingsRow("NAS passphrase", PhoneWords.PassphraseDetail(s), buttons, keepName: true));
        if (!_phoneChanging || _phonePass is null || _phonePassAgain is null) return;
        rows.Children.Add(SettingsRow("New passphrase", "At least 10 characters. Nothing on this PC keeps it.", _phonePass!, keepName: true));
        rows.Children.Add(SettingsRow("Again", "", _phonePassAgain!, keepName: true));
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8), HorizontalAlignment = HorizontalAlignment.Right };
        actions.Children.Add(PhoneButton("Cancel", () =>
        {
            _phoneChanging = false;
            _phonePass.SetText("");
            _phonePassAgain.SetText("");
            if (_phoneShown is { } shown) ShowPhone(shown);
        }, "Cancel the new passphrase"));
        _phoneSetPass = PhoneButton("Set passphrase", () =>
        {
            string pass = _phonePass.Text;
            _phoneChanging = false;
            _phonePass.SetText("");
            _phonePassAgain.SetText("");
            PhoneRequest(("do", "set_passphrase"), ("passphrase", pass));
        });
        actions.Children.Add(_phoneSetPass);
        rows.Children.Add(actions);
    }

    private static Button? _phoneSetPass;

    // "Relay (your own server)": the address and the server's key go to the NAS,
    // which keeps the key; neither this PC nor a phone's code ever holds it.
    private static void AddPhoneRelay(StackPanel rows, PhoneLinkState s, bool busy)
    {
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8) };
        if (s.Relay.Length > 0)
        {
            Button remove = PhoneButton("Remove", () => _ = PhoneRequestJson(PhoneWords.RelayRequest("", "")), "Stop using the relay");
            remove.IsEnabled = !busy;
            buttons.Children.Add(remove);
        }
        string relay = s.Relay;
        Button set = PhoneButton(relay.Length == 0 ? "Set up…" : "Change…", () =>
        {
            _phoneRelayEditing = !_phoneRelayEditing;
            _phoneRelayAddress!.SetText(relay);
            _phoneRelayKey!.SetText("");
            if (_phoneShown is { } shown) ShowPhone(shown);
        }, relay.Length == 0 ? "Set up your own relay" : "Change the relay");
        set.IsEnabled = !busy && s.NasHasRelay;
        buttons.Children.Add(PhoneTipHolder(set, s.NasHasRelay ? null : "Update the NAS service to use this"));
        rows.Children.Add(SettingsRow("Relay (your own server)", PhoneWords.RelayDetail(s), buttons, keepName: true));
        if (!_phoneRelayEditing || _phoneRelayAddress is null || _phoneRelayKey is null) return;
        rows.Children.Add(SettingsRow("Relay address", "", _phoneRelayAddress, keepName: true));
        rows.Children.Add(SettingsRow("Relay key", "The key your driftwire-relay was started with. Your NAS keeps it; this PC doesn't.",
                                      _phoneRelayKey, keepName: true));
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8), HorizontalAlignment = HorizontalAlignment.Right };
        actions.Children.Add(PhoneButton("Cancel", () =>
        {
            _phoneRelayEditing = false;
            _phoneRelayKey.SetText("");
            if (_phoneShown is { } shown) ShowPhone(shown);
        }, "Cancel the relay"));
        _phoneRelaySave = PhoneButton("Save relay", SavePhoneRelay);
        actions.Children.Add(_phoneRelaySave);
        rows.Children.Add(actions);
    }

    private static void SavePhoneRelay()
    {
        if (_phoneRelaySave is not { IsEnabled: true }) return;
        string address = _phoneRelayAddress!.Text, key = _phoneRelayKey!.Text;
        _phoneRelayEditing = false;
        _phoneRelayKey.SetText("");
        _ = PhoneRequestJson(PhoneWords.RelayRequest(address, key));
    }

    private static bool PhoneRequestJson(string json)
    {
        bool sent = PhoneNative.Request(json);
        PollPhone();
        return sent;
    }

    private static void UnlockPhone()
    {
        string pass = _phoneUnlock!.Text;
        if (pass.Length == 0) return;
        _phoneUnlock.SetText("");
        PhoneRequest(("do", "unlock"), ("passphrase", pass));
    }

    // The code, the check code beside it, and how long it is good for.
    private static FrameworkElement PhoneCodeBlock(PhoneLinkState s)
    {
        var grid = new Grid { ColumnSpacing = Space(16), Padding = Pad(12), Background = Brush(ChromeColour.Surface), CornerRadius = Corner(RadiusCard) };
        SettingsCard(grid);
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.Children.Add(PhoneQrImage(s.Qr, 240));
        var words = new StackPanel { Spacing = Space(6) };
        TextBlock head = Label("Scan with MediaViewer on your phone");
        head.Foreground = Brush(Title);
        words.Children.Add(head);
        if (s.QrForSeconds > 0)
        {
            TextBlock guest = PhoneNote(PhoneWords.GuestCodeLine(s.QrForSeconds));
            guest.Foreground = Brush(Title);
            words.Children.Add(guest);
        }
        words.Children.Add(PhoneNote($"Your phone will show {s.QrLibrary} and this code. Check they match before you confirm."));
        TextBlock check = Label(s.QrFingerprint);
        check.Foreground = Brush(Title);
        check.FontFamily = new FontFamily("Cascadia Mono, Consolas");
        check.IsTextSelectionEnabled = true;
        AutomationProperties.SetName(check, "Check code " + s.QrFingerprint);
        words.Children.Add(check);
        _phoneCountdown = PhoneNote(PhoneWords.Countdown(s.QrExpires, DateTimeOffset.UtcNow.ToUnixTimeSeconds()));
        words.Children.Add(_phoneCountdown);
        Button cancel = PhoneButton("Cancel", () => PhoneRequest(("do", "pair_stop")), "Cancel the code");
        cancel.HorizontalAlignment = HorizontalAlignment.Left;
        words.Children.Add(cancel);
        Grid.SetColumn(words, 1);
        grid.Children.Add(words);
        return grid;
    }

    /// <summary>
    /// The QR on white with a 12-DIP quiet zone (the Mac's PhoneQRImage). One path
    /// of row runs; each module is a whole number of physical pixels, so no
    /// edge is smoothed, and each run is grown by a hair so neighbours overlap
    /// with no seam. Encoded on a worker: bounded work, but not this thread's.
    /// </summary>
    private static Border PhoneQrImage(string text, double side)
    {
        var box = new Border
        {
            Width = side,
            Height = side,
            CornerRadius = Corner(RadiusCard),
            Background = new SolidColorBrush(Microsoft.UI.Colors.White),
            Padding = new Thickness(12),
        };
        AutomationProperties.SetName(box, "Code for your phone to scan");
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
            DispatcherQueueControllerTryEnqueue(() =>
            {
                double scale = box.XamlRoot?.RasterizationScale ?? 1;
                double room = (side - 24) * scale;
                double m = Math.Max(1, Math.Floor(room / q.Size)) / scale;  // whole physical pixels a module
                double inner = m * q.Size;
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
                    HorizontalAlignment = HorizontalAlignment.Center,
                    VerticalAlignment = VerticalAlignment.Center,
                };
            });
        });
        return box;
    }

    private static void AddPhoneOtherPublisher(StackPanel rows, PhoneLinkState s, bool busy)
    {
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8) };
        Button ask = PhoneButton("Ask to hand over", () => PhoneRequest(("do", "takeover_ask")));
        ask.IsEnabled = !busy;
        Button restart = PhoneButton("Start over…", () =>
        {
            _phoneConfirmRestart = true;
            if (_phoneShown is { } shown) ShowPhone(shown);
        });
        restart.IsEnabled = !busy;
        buttons.Children.Add(ask);
        buttons.Children.Add(restart);
        rows.Children.Add(SettingsRow("Published from another computer",
            $"Key {s.OtherFingerprint}. Ask it to hand over, and phones follow by themselves. If it's gone, start over and scan each phone again.",
            buttons, keepName: true));
        if (_phoneConfirmRestart)
        {
            rows.Children.Add(PhoneConfirm("Start over from this PC? Every phone will have to scan a new code.", "Start over",
                destructive: true, () =>
                {
                    _phoneConfirmRestart = false;
                    PhoneRequest(("do", "takeover_restart"));
                }, () =>
                {
                    _phoneConfirmRestart = false;
                    if (_phoneShown is { } shown) ShowPhone(shown);
                }));
        }
    }

    private static void AddPhoneAsking(StackPanel rows, PhoneLinkState s, bool busy)
    {
        Button hand = PhoneButton("Hand over…", () =>
        {
            _phoneConfirmHandOver = true;
            if (_phoneShown is { } shown) ShowPhone(shown);
        });
        hand.IsEnabled = !busy;
        rows.Children.Add(SettingsRow($"{s.AskingName} wants to publish this library",
            $"Its key is {s.AskingFingerprint}. Check it matches what that computer shows.", hand, keepName: true));
        if (_phoneConfirmHandOver)
        {
            string key = s.AskingKey;
            rows.Children.Add(PhoneConfirm($"Hand this library to {s.AskingName}? This PC will stop publishing it. Phones follow by themselves.",
                "Hand over", destructive: false, () =>
                {
                    _phoneConfirmHandOver = false;
                    PhoneRequest(("do", "takeover_allow"), ("key", key));
                }, () =>
                {
                    _phoneConfirmHandOver = false;
                    if (_phoneShown is { } shown) ShowPhone(shown);
                }));
        }
    }

    // "Start over? [Start over] [Cancel]" under the row that asked; Esc cancels.
    private static Grid PhoneConfirm(string text, string action, bool destructive, Action perform, Action cancel)
    {
        var grid = new Grid { ColumnSpacing = Space(10), Padding = Pad(12), Background = Brush(ChromeColour.Surface), CornerRadius = Corner(RadiusCard) };
        SettingsCard(grid);
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        TextBlock label = PhoneNote(text);
        label.Foreground = Brush(Title);
        label.VerticalAlignment = VerticalAlignment.Center;
        grid.Children.Add(label);
        Button go = SettingsButton(action, perform, accent: !destructive);
        AutomationProperties.SetName(go, action);
        Grid.SetColumn(go, 1);
        grid.Children.Add(go);
        Button no = SettingsButton("Cancel", cancel);
        AutomationProperties.SetName(no, "Cancel");
        Grid.SetColumn(no, 2);
        grid.Children.Add(no);
        grid.KeyDown += (_, e) =>
        {
            if (e.Key != Windows.System.VirtualKey.Escape) return;
            e.Handled = true;  // Esc answers this, and does not close Settings
            cancel();
        };
        // Keyboard focus goes to the safe answer, once it is laid out.
        no.Loaded += (_, _) => no.Focus(FocusState.Programmatic);
        return grid;
    }

    // ---- actions ----

    private static async Task ChoosePhoneFolder()
    {
        var picker = new FolderPicker();
        picker.FileTypeFilter.Add("*");
        WinRT.Interop.InitializeWithWindow.Initialize(picker, _themeWindow);
        Windows.Storage.StorageFolder? folder;
        try { folder = await picker.PickSingleFolderAsync(); }
        catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or UnauthorizedAccessException)
        {
            return;
        }
        // A library (Pictures in the side bar) has no path: not a NAS folder.
        if (folder is null || string.IsNullOrEmpty(folder.Path)) return;
        PhoneRequest(("do", "folder"), ("path", folder.Path));
    }

    private static void SavePhoneAddresses()
    {
        if (_phoneShown is not { Claimed: true }) return;
        PhoneRequest(("do", "set_addresses"), ("local", _phoneLocal!.Text), ("public", _phonePublic!.Text));
    }

    private static void PhoneFieldsChanged()
    {
        bool busy = _phoneShown is { Busy.Length: > 0 };
        string again = _phonePassAgain!.Text;
        if (_phoneClaim is not null)
            _phoneClaim.IsEnabled = !busy && _phoneLocal!.Text.Length > 0 && (again.Length == 0 || again == _phonePass!.Text);
        if (_phoneSetPass is not null)
            _phoneSetPass.IsEnabled = !busy && _phonePass!.Text.Length >= 10 && again == _phonePass.Text;
        if (_phoneUnlockGo is not null) _phoneUnlockGo.IsEnabled = !busy && _phoneUnlock!.Text.Length > 0;
        if (_phoneRelaySave is not null)
            _phoneRelaySave.IsEnabled = !busy && _phoneRelayAddress!.Text.Trim().Length > 0 &&
                                        _phoneRelayKey!.Text.Length >= PhoneWords.RelayKeyMinimum;
        string path = _phoneNewPath!.Text;
        _phoneNewLabel!.PlaceholderText = path.Length == 0 ? "Label" : PhoneWords.ShareLabel(path);
        if (_phoneAdd is not null)
            _phoneAdd.IsEnabled = !busy && path.StartsWith('/') && PhoneWords.ShareLabel(path).Length > 0;
    }

    private static void AddPhoneFolder()
    {
        string path = _phoneNewPath!.Text;
        if (_phoneAdd is not { IsEnabled: true } || !path.StartsWith('/')) return;
        string label = _phoneNewLabel!.Text.Length == 0 ? PhoneWords.ShareLabel(path) : _phoneNewLabel.Text;
        if (_phoneNewIsBackup)
        {
            long? limit = PhoneWords.Bytes(_phoneNewLimit!.Text), reserve = PhoneWords.Bytes(_phoneNewReserve!.Text);
            if (limit is not > 0 || reserve is null) return;
            _phoneNewLabel.SetText("");
            _phoneNewPath.SetText("");
            _phoneNewIsBackup = false;
            PhoneRequest(("do", "add_folder"), ("label", label), ("path", path), ("backup", "1"),
                         ("limit", limit.Value.ToString(System.Globalization.CultureInfo.InvariantCulture)),
                         ("reserve", reserve.Value.ToString(System.Globalization.CultureInfo.InvariantCulture)));
            return;
        }
        _phoneNewLabel.SetText("");
        _phoneNewPath.SetText("");
        PhoneRequest(("do", "add_folder"), ("label", label), ("path", path));
    }

    // A backup folder's Change limit: a flyout with its limit and the free space to keep.
    private static Button BackupLimitMenu(PhoneFolder f, bool busy)
    {
        Button menu = PhoneButton("Change limit", () => { }, $"Change the storage limit of {f.Label}");
        var column = new StackPanel { Spacing = Space(4) };
        var flyout = new Flyout { Content = column };
        FakeInput limit = PhoneInput("GB", "Storage limit in gigabytes", 70);
        FakeInput reserve = PhoneInput("GB", "Free space to keep on the NAS, in gigabytes", 60);
        limit.SetText(PhoneWords.Gigabytes(f.Limit));
        reserve.SetText(PhoneWords.Gigabytes(f.Reserve));
        column.Children.Add(PhoneNote("Limit (GB)"));
        column.Children.Add(limit);
        column.Children.Add(PhoneNote("Keep free (GB)"));
        column.Children.Add(reserve);
        string id = f.Id;
        column.Children.Add(SettingsButton("Save", () =>
        {
            long? l = PhoneWords.Bytes(limit.Text), r = PhoneWords.Bytes(reserve.Text);
            if (l is not > 0 || r is null) return;
            flyout.Hide();
            _ = PhoneNative.Request(PhoneWords.SetBackupRequest(id, l.Value, r.Value));
            PollPhone();
        }));
        menu.Flyout = flyout;
        PhoneFlyouts.Add(flyout);
        menu.IsEnabled = !busy;
        return menu;
    }
}
