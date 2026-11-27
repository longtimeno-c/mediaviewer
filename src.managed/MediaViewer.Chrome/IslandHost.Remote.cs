// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

/// <summary>
/// docs/plans/desktop-client.md: another computer's folders, pinned at the top of the
/// folder tree as the Mac's FolderTreeView pins them. One row per paired server (a
/// self-hosted computer or a Synology); it expands to the server's folders, listed
/// through mv_list_subdirectories off the UI thread like any folder, and a folder
/// opens through the tree's own open. Below them, "Connect to a server…": paste the
/// other computer's code (no password, owner call 2), compare its check, connect.
/// Read-only from here (owner call 1). The rows follow mv_remote_generation, polled
/// every two seconds while the tree is up and never otherwise.
/// </summary>
public static partial class IslandHost
{
    private static StackPanel? _treeRemote;
    private static StackPanel? _remoteRows;
    private static StackPanel? _remoteConnect;
    private static FakeInput? _remoteCode;
    private static TextBlock? _remoteNote;
    private static Button? _remoteConfirm;
    private static string _remotePendingCode = "";
    private static ulong _remoteGen = ulong.MaxValue;
    private static Microsoft.UI.Dispatching.DispatcherQueueTimer? _remoteTimer;
    private static readonly Dictionary<string, TreeNodeUi> _remoteNodes = new(StringComparer.Ordinal);

    internal static bool IsRemoteKey(string path) => path.StartsWith("remote:", StringComparison.Ordinal);

    private static StackPanel BuildRemoteSection()
    {
        _remoteNodes.Clear();
        _remoteGen = ulong.MaxValue;
        _treeRemote = new StackPanel { Padding = Pad(8, 6, 8, 6), Spacing = Space(2) };
        _remoteRows = new StackPanel { Spacing = Space(2) };
        _treeRemote.Children.Add(_remoteRows);
        Button connect = FlatButton(Text("Connect to a server…", Body), Pad(6, 2, 8, 2));
        AutomationProperties_SetName(connect, "Connect to a server");
        connect.Click += (_, _) => ToggleRemoteConnect();
        _treeRemote.Children.Add(connect);
        _remoteConnect = new StackPanel { Spacing = Space(6), Visibility = Visibility.Collapsed, Padding = Pad(6, 4, 6, 4) };
        _remoteConnect.Children.Add(Text("Paste the code from the other computer: its Phone access card, or "
                                         + "`driftwire host pair` on it. Its folders then appear here, read-only.", Body));
        _remoteCode = new FakeInput("mediaviewer://pair#…", 240) { MaxUtf8Bytes = 4096 };
        AutomationProperties_SetName(_remoteCode, "Pairing code");
        _remoteCode.Submitted += RemoteCheckCode;
        _remoteConnect.Children.Add(_remoteCode);
        Button check = FlatButton(Text("Check code", Title), Pad(8, 4, 8, 4));
        check.Click += (_, _) => RemoteCheckCode();
        _remoteConnect.Children.Add(check);
        _remoteNote = Text("", Body);
        _remoteConnect.Children.Add(_remoteNote);
        _remoteConfirm = FlatButton(Text("Connect", Title), Pad(8, 4, 8, 4));
        _remoteConfirm.Visibility = Visibility.Collapsed;
        _remoteConfirm.Click += (_, _) => RemoteConnect();
        _remoteConnect.Children.Add(_remoteConfirm);
        _treeRemote.Children.Add(_remoteConnect);
        _treeRemote.Children.Add(new Border { Height = 1, Background = Brush(Hairline), Margin = Pad(0, 6, 0, 0) });

        _remoteTimer ??= _dispatcher?.DispatcherQueue.CreateTimer();
        if (_remoteTimer is not null)
        {
            _remoteTimer.Interval = TimeSpan.FromSeconds(2);
            _remoteTimer.IsRepeating = true;
            _remoteTimer.Tick -= OnRemoteTick;
            _remoteTimer.Tick += OnRemoteTick;
            _remoteTimer.Start();
        }
        SyncRemoteRows();
        return _treeRemote;
    }

    private static void DropRemoteUi()
    {
        _remoteTimer?.Stop();
        _treeRemote = null;
        _remoteRows = null;
        _remoteConnect = null;
        _remoteCode = null;
        _remoteNote = null;
        _remoteConfirm = null;
        _remotePendingCode = "";
        _remoteNodes.Clear();
    }

    private static void OnRemoteTick(Microsoft.UI.Dispatching.DispatcherQueueTimer sender, object args) => SyncRemoteRows();

    private static string RemoteNote(string state) => state switch
    {
        "unreachable" => "Can't reach it",
        "access_ended" => "Access ended",
        "wrong_service" => "Not the paired server",
        "too_old" => "Needs an update",
        _ => "",
    };

    // The role the server gave this PC (#686 §2), said only while nothing is wrong.
    // The same words as the Mac's RemoteServer.roleWords.
    internal static string RemoteRoleWords(string role) => role switch
    {
        "view" => "View only",
        "change" => "Can change files",
        "admin" => "Admin",
        _ => "",
    };

    // The rows as the core lists them; only a new generation rebuilds anything, and a
    // row that stays keeps its node (its expansion and listed folders).
    private static void SyncRemoteRows()
    {
        if (_remoteRows is null) return;
        ulong gen = RemoteNative.Generation();
        if (gen == _remoteGen) return;
        _remoteGen = gen;
        // A pairing that worked or a server forgotten: native pins it with the
        // folders (the welcome card, the jump list's Open > Recent), or drops its pin.
        Send(Command.RemoteChanged);
        var rows = new List<(string Name, string Key, string State, string Role)>();
        try
        {
            using JsonDocument doc = JsonDocument.Parse(RemoteNative.ServersJson());
            foreach (JsonElement e in doc.RootElement.EnumerateArray())
            {
                string name = e.GetProperty("name").GetString() ?? "";
                string key = e.GetProperty("key").GetString() ?? "";
                string state = e.GetProperty("state").GetString() ?? "";
                string role = e.TryGetProperty("role", out JsonElement r) ? r.GetString() ?? "" : "";
                if (IsRemoteKey(key)) rows.Add((name, key, state, role));
            }
        }
        catch (JsonException) { return; }
        catch (InvalidOperationException) { return; }
        catch (KeyNotFoundException) { return; }
        _remoteRows.Children.Clear();
        var keep = new HashSet<string>(rows.Select(r => r.Key), StringComparer.Ordinal);
        foreach (string gone in _remoteNodes.Keys.Where(k => !keep.Contains(k)).ToList()) _remoteNodes.Remove(gone);
        foreach (var (name, key, state, role) in rows)
        {
            string note = RemoteNote(state);
            if (note.Length == 0) note = RemoteRoleWords(role);
            string label = note.Length == 0 ? name : name + " · " + note;
            if (!_remoteNodes.TryGetValue(key, out TreeNodeUi? node) || node.Label != label)
            {
                node = new TreeNodeUi(label, key, 0);
                _remoteNodes[key] = node;
            }
            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(4) };
            // A kept node is still its old row's child: XAML refuses a second parent.
            (node.Node.Parent as Panel)?.Children.Remove(node.Node);
            row.Children.Add(node.Node);
            Button forget = FlatButton(Text("Forget", Body, TypeSize(TypeSmall)), Pad(6, 2, 6, 2), tabStop: false);
            AutomationProperties_SetName(forget, "Forget " + name);
            string k = key;
            forget.Click += (_, _) =>
            {
                RemoteNative.Forget(k);
                _remoteGen = ulong.MaxValue;
            };
            row.Children.Add(forget);
            _remoteRows.Children.Add(row);
        }
    }

    private static void ToggleRemoteConnect()
    {
        if (_remoteConnect is null) return;
        bool show = _remoteConnect.Visibility != Visibility.Visible;
        _remoteConnect.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        if (show) _remoteCode?.Focus(FocusState.Programmatic);
    }

    // The code's own words, before anything is sent: whose it is and the check to compare.
    private static void RemoteCheckCode()
    {
        if (_remoteCode is null || _remoteNote is null || _remoteConfirm is null) return;
        string code = _remoteCode.Text.Trim();
        _remoteConfirm.Visibility = Visibility.Collapsed;
        _remotePendingCode = "";
        if (code.Length == 0) return;
        try
        {
            using JsonDocument doc = JsonDocument.Parse(RemoteNative.PreviewJson(code));
            JsonElement root = doc.RootElement;
            if (root.TryGetProperty("error", out JsonElement error))
            {
                _remoteNote.Text = error.GetString() ?? "";
                return;
            }
            string name = root.GetProperty("name").GetString() ?? "the server";
            string check = root.GetProperty("fingerprint").GetString() ?? "";
            _remoteNote.Text = $"Check that {name} shows the same check: {check}. This computer will see its "
                               + "folders, read-only; its owner can remove this computer there at any time.";
            _remoteConfirm.Content = Text("Connect to " + name, Title);
            _remoteConfirm.Visibility = Visibility.Visible;
            _remotePendingCode = code;
        }
        catch (JsonException) { }
        catch (InvalidOperationException) { }
        catch (KeyNotFoundException) { }
    }

    private static void RemoteConnect()
    {
        if (_remotePendingCode.Length == 0 || _remoteNote is null || _remoteConfirm is null) return;
        if (!RemoteNative.PairStart(_remotePendingCode)) return;
        _remoteConfirm.Visibility = Visibility.Collapsed;
        _remoteNote.Text = "Connecting…";
        _remotePendingCode = "";
        _remoteCode?.SetText("");
        Microsoft.UI.Dispatching.DispatcherQueueTimer? poll = _dispatcher?.DispatcherQueue.CreateTimer();
        if (poll is null) return;
        poll.Interval = TimeSpan.FromMilliseconds(400);
        poll.IsRepeating = true;
        poll.Tick += (t, _) =>
        {
            try
            {
                using JsonDocument doc = JsonDocument.Parse(RemoteNative.PairStateJson());
                if (doc.RootElement.TryGetProperty("busy", out JsonElement busy) && busy.GetBoolean()) return;
                string words = doc.RootElement.TryGetProperty("words", out JsonElement w) ? w.GetString() ?? "" : "";
                if (_remoteNote is not null) _remoteNote.Text = words;
            }
            catch (JsonException) { }
            catch (InvalidOperationException) { }
            t.Stop();
            _remoteGen = ulong.MaxValue;
            SyncRemoteRows();
        };
        poll.Start();
    }
}
