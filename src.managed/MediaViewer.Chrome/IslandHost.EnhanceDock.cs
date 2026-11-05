// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

/// <summary>
/// One-click Enhance (owner 2026-11-05, docs/design/29 "One click"): Enhance's
/// "tool" screen (manifest place "edit") is the Edit workspace's Enhance tab
/// cards. Its tree's root is "cards": one stack "card.&lt;word&gt;" per tool,
/// drawn inside that tool's card (the compact choices, the time the run will
/// take, its one button), and a hidden "run" node that says what the run is
/// doing. This draws the card stacks, reads the run node and tells native
/// (chrome_edit_action::enhance_run: the glow over the photo, the tool bar's
/// band, the result opened once), and the tool bar draws the run's line from it
/// (IslandHost.ToolBar.cs). Native opens the screen when the tab shows (main.cpp
/// enhance_cards_sync) and points it at each photo; it is never a window
/// (SyncScreenWindows skips place "edit"). No sheet and no panel. The Mac twin
/// is EnhanceRun.swift.
/// </summary>
/// <remarks>Only Button, TextBlock, Border, Grid, StackPanel and ContentControl
/// (tools/check-winui-controls.ps1); the stacks themselves are the screens' renderer.</remarks>
public static partial class IslandHost
{
    // The cards' screen as the core last listed it (a clone), its revision, and
    // whether the open screen is an Enhance from before one-click.
    private static JsonElement? _enhanceDock;
    private static long _enhanceDockRevision = -1;
    private static bool _enhanceCardsOutdated;
    // Each card's place for its stack.
    private static readonly Dictionary<EnhanceToolKind, ContentControl> EnhanceDockHosts = new();

    // The run, as the hidden "run" node says it.
    private static string _enhanceRunState = "idle";
    private static string _enhanceRunLine = "";
    private static double _enhanceRunFraction = -1;
    private static string _enhanceRunTool = "";
    private static string _enhanceRunDiscard = "";  // Move to Recycle Bin's label, "" none
    private static string _enhanceRunReported = "";

    private static bool EnhanceRunActive => _enhanceRunState != "idle";
    private static bool EnhanceRunWorking => _enhanceRunState is "running" or "downloading";

    private static string? EnhancePhotoOf(EnhanceToolKind kind) => kind switch
    {
        EnhanceToolKind.Remove => "remove",
        EnhanceToolKind.Restyle => "restyle",
        EnhanceToolKind.Detail => "detail",
        EnhanceToolKind.Extend => "extend",
        EnhanceToolKind.RestoreFaces => "faces",
        _ => null,
    };

    // The add-on's stack a card draws: its own tool's, and for the Faces card
    // Restore faces' (the Faces tool staying native's).
    private static string? EnhanceDockWordOf(EnhanceToolKind kind) =>
        kind == EnhanceToolKind.Faces ? "faces" : EnhancePhotoOf(kind);

    // Whether the add-on's cards have this tool's stack.
    private static bool EnhanceDockedOn(EnhanceToolKind kind) =>
        EnhanceDockWordOf(kind) is { } word && EnhanceCardOf(word) is not null;

    private static JsonElement? EnhanceCardOf(string word)
    {
        if (_enhanceDock is not { } s || !s.TryGetProperty("tree", out JsonElement tree) ||
            tree.ValueKind != JsonValueKind.Object || JStr(tree, "id") != "cards")
        {
            return null;
        }
        foreach (JsonElement child in JArr(tree, "children"))
        {
            if (JStr(child, "id") == "card." + word) return child;
        }
        return null;
    }

    /// <summary>Inside a card: the place for its stack, collapsed until the cards have it.</summary>
    private static ContentControl BuildEnhanceDockHost(EnhanceToolKind kind)
    {
        var body = new ContentControl
        {
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            IsTabStop = false,
            Visibility = Visibility.Collapsed,
            Margin = Pad(0, 4, 0, 0),
        };
        EnhanceDockHosts[kind] = body;
        return body;
    }

    // SyncScreenWindows: the core's list. The cards are drawn when their tree's
    // revision moves.
    private static void SyncEnhanceDock(JsonElement list)
    {
        JsonElement? found = null;
        foreach (JsonElement s in list.EnumerateArray())
        {
            if (JStr(s, "place") != "edit" || JStr(s, "addon") != "enhance") continue;
            found = s.Clone();
            break;
        }
        bool had = _enhanceDock is not null;
        _enhanceDock = found;
        if (found is null)
        {
            _enhanceCardsOutdated = false;
            ReadEnhanceRun(null);
            if (!had) return;
            ForgetSurfaces(new ScreenAt("enhance", "tool"));
            _enhanceDockRevision = -1;
            DrawEnhanceDock(force: true);
            return;
        }
        DrawEnhanceDock(force: false);
    }

    // Draws each card's stack inside its card (BuildEnhanceHub asks again,
    // forced, when the tab is rebuilt), and reads the run.
    private static void DrawEnhanceDock(bool force)
    {
        if (_enhanceDock is not { } s || !s.TryGetProperty("tree", out JsonElement tree) ||
            tree.ValueKind != JsonValueKind.Object)
        {
            foreach (ContentControl body in EnhanceDockHosts.Values)
            {
                body.Content = null;
                body.Visibility = Visibility.Collapsed;
            }
            if (_editTab == TabEnhance) RenderEnhanceInPlace();
            return;
        }
        _enhanceCardsOutdated = JStr(tree, "id") != "cards";
        ReadEnhanceRun(_enhanceCardsOutdated ? null : tree);
        var at = new ScreenAt(JStr(s, "addon"), JStr(s, "id"));
        long revision = (long)JNum(s, "revision", 0);
        if (!force && revision == _enhanceDockRevision) return;
        if (ScreenPressed.Contains(at)) return;  // #511: the release catches up
        foreach ((EnhanceToolKind kind, ContentControl body) in EnhanceDockHosts)
        {
            JsonElement? card = EnhanceDockWordOf(kind) is { } word ? EnhanceCardOf(word) : null;
            if (card is not { } c)
            {
                body.Content = null;
                body.Visibility = Visibility.Collapsed;
                continue;
            }
            ScreenPlace? kept = NoteScreenPlace(body.Content as UIElement);
            FrameworkElement content = BuildSettingsScreenContent(c, at);
            if (kept is { } k) RestoreScreenPlace(content, k);
            body.Content = content;
            body.Visibility = Visibility.Visible;
        }
        _enhanceDockRevision = revision;
        if (_editTab == TabEnhance) RenderEnhanceInPlace();
    }

    private static JsonElement? FindScreenNode(JsonElement node, string id)
    {
        if (node.ValueKind != JsonValueKind.Object) return null;
        if (JStr(node, "id") == id) return node;
        foreach (JsonElement child in JArr(node, "children"))
        {
            if (FindScreenNode(child, id) is { } hit) return hit;
        }
        return null;
    }

    // The hidden "run" node: what the tool bar says, and what native is told
    // ("<state>\t<id>\t<result>", shell/enhance_run.h parse_enhance_run).
    private static void ReadEnhanceRun(JsonElement? tree)
    {
        string state = "idle", line = "", tool = "", discard = "", result = "", id = "0";
        double fraction = -1;
        if (tree is { } t && FindScreenNode(t, "run") is { } run)
        {
            state = JStr(run, "label");
            line = FindScreenNode(t, "run.line") is { } l ? JStr(l, "text") : "";
            tool = FindScreenNode(t, "run.tool") is { } tl ? JStr(tl, "text") : "";
            id = FindScreenNode(t, "run.id") is { } i ? JStr(i, "text") : "0";
            fraction = FindScreenNode(t, "run.progress") is { } p ? JNum(p, "value", -1) : -1;
            if (state == "done" && FindScreenNode(t, "run.result") is { } r) result = JStr(r, "path");
            discard = FindScreenNode(t, "result.discard") is { } d ? JStr(d, "label") : "";
        }
        if (state.Length == 0) state = "idle";
        bool changed = state != _enhanceRunState || line != _enhanceRunLine || discard != _enhanceRunDiscard ||
                       fraction != _enhanceRunFraction;
        _enhanceRunState = state;
        _enhanceRunLine = line;
        _enhanceRunFraction = fraction;
        _enhanceRunTool = tool;
        _enhanceRunDiscard = discard;
        if (changed) RefreshEnhanceRunBar();
        int code = state switch
        {
            "running" => 1,
            "done" => 2,
            "failed" => 3,
            "cancelled" => 4,
            "downloading" => 5,
            _ => 0,
        };
        string wire = code + "\t" + (ulong.TryParse(id, out ulong n) ? n : 0UL) + "\t" + result;
        if (wire == _enhanceRunReported) return;
        _enhanceRunReported = wire;
        _treePending = wire;
        Send(Command.EditAction, EditActions.EnhanceRun);
    }

    private static readonly ScreenAt EnhanceCardsAt = new("enhance", "tool");
    private static void EnhanceRunCancel() => SendScreenEvent(EnhanceCardsAt, "cancel", 1, "");
    private static void EnhanceRunDismiss() => SendScreenEvent(EnhanceCardsAt, "run.dismiss", 1, "");
    private static void EnhanceRunDiscard() => SendScreenEvent(EnhanceCardsAt, "result.discard", 1, "");

    /// <summary>
    /// The screens' renderer asks before a button tells its add-on: true when
    /// the host took the click. The cards have no host buttons (one-click: the
    /// result opens by itself), so nothing is taken.
    /// </summary>
    private static bool TakeScreenButton(ScreenAt at, string id)
    {
        _ = at;
        _ = id;
        return false;
    }
}
