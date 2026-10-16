// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Windows.Storage.Pickers;

namespace MediaViewer.Chrome;

/// <summary>
/// Settings → Add-ons → From others (docs/design/25): add-ons from other makers, one
/// <c>.mvaddon</c> file each, installed from a file or a link. The Windows
/// twin of OpenAddons.swift.
///
/// The core reads, checks and installs (src/addon/open_store.h, through
/// <see cref="AddonNative"/>); this asks, downloads and shows. It decides
/// nothing about trust: what the sheet says an add-on is, can and cannot do,
/// and why one is refused, all come from the core's JSON, the same on both
/// hosts.
///
/// A link is fetched only on a click, with a plain GET: no cookies, one fixed
/// User-Agent, nothing about the user's files (rule 6). Nothing is fetched in
/// the background, so an add-on's maker cannot learn when MediaViewer runs.
/// </summary>
public static partial class IslandHost
{
    private sealed record OpenTheme(string Id, string Name);

    // PR 56: a keymap pack an add-on carries (Settings → Keyboard shortcuts → Key layout).
    private sealed record OpenKeymap(string Id, string Name);

    private sealed record OpenAddon(string Folder, string Id, string Name, string Version, string State,
        string Description, string Licence, long Size, string UpdateUrl, string Publisher, string PublisherUrl,
        string Fingerprint, string Adds, IReadOnlyList<string> Can, string Cannot,
        IReadOnlyList<OpenTheme> Themes, bool Settings, IReadOnlyList<OpenKeymap> Keymaps)
    {
        public string PublisherHost =>
            Uri.TryCreate(PublisherUrl, UriKind.Absolute, out Uri? u) ? u.Host : "";
    }

    /// <summary>docs/design/25 PR 57: a script add-on as the core runs it.</summary>
    private sealed record ScriptCommand(string Id, string Name, string Key, string Note);

    private sealed record ScriptState(string State, string Stop, string Message, string LastError,
        IReadOnlyList<ScriptCommand> Commands);

    /// <summary>A package that has been looked at and is waiting for an answer.</summary>
    private sealed record OpenOffer(string Path, bool Temporary, bool Ok, string Message, string Sha256,
        string Relation, string InstalledVersion, OpenAddon? Addon);

    private const long OpenAddonMaxBytes = 64L << 20;

    private static List<OpenAddon> _openAddons = new();
    private static Dictionary<string, ScriptState> _openScripts = new();
    private static OpenOffer? _openOffer;
    private static bool _openBusy;
    private static bool _openEnteringLink;
    private static string? _openConfirmRemove;
    // The Remove question's "Keep its settings" (PR 56), on until turned off.
    private static bool _openKeepSettings = true;
    // Installed open add-ons' settings pages by add-on id (PR 56).
    private static Dictionary<string, AddonSettingsPage> _openPages = new(StringComparer.Ordinal);
    private static string _openMessage = "";
    private static string _openProgress = "";
    // The download's share once its size is known; null while connecting,
    // checking, installing, or for a server that sends no length.
    private static double? _openFraction;
    // The bar in the row now, updated in place by download reports so its
    // sweep is not restarted ten times a second.
    private static AddonProgressView? _openProgressView;
    private static StackPanel? _openRow;
    private static FakeInput? _openLink;
    // What was typed into the link field, kept across the row's rebuilds (a
    // refused link must not wipe it; seen in the first Windows run).
    private static string _openLinkText = "";
    private static TextBlock? _themeDetail;

    // Redirects are followed by hand, so each hop can be held to https.
    private static readonly HttpClient OpenAddonHttp = CreateOpenAddonClient();

    private static HttpClient CreateOpenAddonClient()
    {
        var client = new HttpClient(new HttpClientHandler { UseCookies = false, AllowAutoRedirect = false })
        {
            Timeout = TimeSpan.FromMinutes(10),
        };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("MediaViewer");
        return client;
    }

    private static string Str(JsonElement e, string name) =>
        e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out JsonElement v) &&
        v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";

    private static OpenAddon ParseOpenAddon(JsonElement e)
    {
        JsonElement publisher = e.TryGetProperty("publisher", out JsonElement p) ? p : default;
        var can = new List<string>();
        if (e.TryGetProperty("can", out JsonElement c) && c.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement line in c.EnumerateArray()) can.Add(line.GetString() ?? "");
        }
        var themes = new List<OpenTheme>();
        if (e.TryGetProperty("themes", out JsonElement t) && t.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement theme in t.EnumerateArray())
            {
                string id = Str(theme, "id");
                if (id.Length == 0) continue;
                string name = Str(theme, "name");
                themes.Add(new OpenTheme(id, name.Length == 0 ? id : name));
            }
        }
        var keymaps = new List<OpenKeymap>();
        if (e.TryGetProperty("keymaps", out JsonElement k) && k.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement pack in k.EnumerateArray())
            {
                string id = Str(pack, "id");
                if (id.Length == 0) continue;
                string name = Str(pack, "name");
                keymaps.Add(new OpenKeymap(id, name.Length == 0 ? id : name));
            }
        }
        string addonId = Str(e, "id");
        string folder = Str(e, "folder");
        string addonName = Str(e, "name");
        long size = e.TryGetProperty("size", out JsonElement s) && s.ValueKind == JsonValueKind.Number
            ? s.GetInt64() : 0;
        return new OpenAddon(folder.Length == 0 ? addonId : folder, addonId,
            addonName.Length == 0 ? addonId : addonName, Str(e, "version"), Str(e, "state"),
            Str(e, "description"), Str(e, "licence"), size, Str(e, "update_url"), Str(publisher, "name"),
            Str(publisher, "url"), Str(publisher, "fingerprint"), Str(e, "adds"), can, Str(e, "cannot"),
            themes, e.TryGetProperty("settings", out JsonElement st) && st.ValueKind == JsonValueKind.True,
            keymaps);
    }

    /// <summary>Settings opened, or something changed: what is installed. Verifies
    /// every installed file, so a worker.</summary>
    private static void RefreshOpenAddons()
    {
        _ = Task.Run(() =>
        {
            var list = new List<OpenAddon>();
            try
            {
                using JsonDocument doc = JsonDocument.Parse(AddonNative.OpenList());
                foreach (JsonElement e in doc.RootElement.EnumerateArray()) list.Add(ParseOpenAddon(e));
            }
            catch (Exception ex) when (ex is MediaViewerException or JsonException
                                           or DllNotFoundException or EntryPointNotFoundException)
            {
                System.Diagnostics.Debug.WriteLine(ex.GetType().Name);
            }
            // PR 56: the settings pages, with their values, read here too (each
            // re-verifies its add-on).
            var pages = new Dictionary<string, AddonSettingsPage>(StringComparer.Ordinal);
            foreach (OpenAddon addon in list)
            {
                if (addon.Settings && addon.State == "ok" && ReadSettingsPage(addon.Id) is AddonSettingsPage page)
                    pages[addon.Id] = page;
            }
            Dictionary<string, ScriptState> scripts = ReadScripts();
            DispatcherQueueControllerTryEnqueue(() =>
            {
                _openAddons = list;
                _openPages = pages;
                _openScripts = scripts;
                RefreshOpenAddonRow();
                RefreshThemeRow();
                RefreshKeymapRow();
            });
        });
    }

    /// <summary>The scripts' state only (no file is read): after a start or a stop
    /// the core made on its own.</summary>
    internal static void RefreshOpenScripts()
    {
        if (_openRow is null) return;
        _ = Task.Run(() =>
        {
            Dictionary<string, ScriptState> scripts = ReadScripts();
            DispatcherQueueControllerTryEnqueue(() =>
            {
                _openScripts = scripts;
                RefreshOpenAddonRow();
            });
        });
    }

    private static Dictionary<string, ScriptState> ReadScripts()
    {
        var scripts = new Dictionary<string, ScriptState>();
        try
        {
            using JsonDocument doc = JsonDocument.Parse(AddonNative.ScriptStatus());
            foreach (JsonElement e in doc.RootElement.EnumerateArray())
            {
                var commands = new List<ScriptCommand>();
                if (e.TryGetProperty("commands", out JsonElement cs) && cs.ValueKind == JsonValueKind.Array)
                {
                    foreach (JsonElement c in cs.EnumerateArray())
                        commands.Add(new ScriptCommand(Str(c, "id"), Str(c, "name"), Str(c, "key"), Str(c, "note")));
                }
                scripts[Str(e, "id")] = new ScriptState(Str(e, "state"), Str(e, "stop"), Str(e, "message"),
                    Str(e, "last_error"), commands);
            }
        }
        catch (Exception ex) when (ex is MediaViewerException or JsonException
                                       or DllNotFoundException or EntryPointNotFoundException)
        {
            System.Diagnostics.Debug.WriteLine(ex.GetType().Name);
        }
        return scripts;
    }

    private static void RestartOpenScript(OpenAddon addon)
    {
        _openMessage = $"{addon.Name} is starting again.";
        RefreshOpenAddonRow();
        _ = Task.Run(() =>
        {
            try
            {
                AddonNative.ScriptRestart(addon.Id);
            }
            catch (Exception ex) when (ex is MediaViewerException or DllNotFoundException
                                           or EntryPointNotFoundException)
            {
                System.Diagnostics.Debug.WriteLine(ex.GetType().Name);
            }
            Dictionary<string, ScriptState> scripts = ReadScripts();
            DispatcherQueueControllerTryEnqueue(() =>
            {
                _openScripts = scripts;
                RefreshOpenAddonRow();
            });
        });
    }

    // Settings → Appearance → Theme is IslandHost.ThemePicker.cs.

    // ---- Settings → Add-ons → From others ------------------------------------------

    private static void AddOpenAddonsSettings(StackPanel view)
    {
        view.Children.Add(Label("From others").WithForeground(Brush(Title)));
        view.Children.Add(Note(
            "Add-ons made by other people, installed from a file or a link. MediaViewer does not check them or who made them. A theme is data. An add-on with a script runs it inside MediaViewer, stopped if it uses too much memory or time, with only the permissions it showed when you installed it. No add-on can use the network."));
        _openRow = new StackPanel { Spacing = Space(8) };
        view.Children.Add(_openRow);
        RefreshOpenAddonRow();
        RefreshOpenAddons();
    }

    /// <summary>Esc on an open "Remove …?" or install question: false when none is open.</summary>
    private static bool CancelOpenAddonConfirm()
    {
        if (_openConfirmRemove is not null)
        {
            _openConfirmRemove = null;
            RefreshOpenAddonRow();
            return true;
        }
        if (_openOffer is not null && !_openBusy)
        {
            CancelOpenOffer();
            return true;
        }
        return false;
    }

    private static TextBlock WithForeground(this TextBlock text, Microsoft.UI.Xaml.Media.Brush brush)
    {
        text.Foreground = brush;
        return text;
    }

    private static void RefreshOpenAddonRow()
    {
        if (_openRow is null) return;
        _openRow.Children.Clear();
        _openLink = null;
        _openProgressView = null;
        if (_openOffer is not null)
        {
            _openRow.Children.Add(BuildOpenSheet(_openOffer));
        }
        else
        {
            foreach (OpenAddon addon in _openAddons) _openRow.Children.Add(BuildOpenAddonEntry(addon));
            if (_openEnteringLink)
            {
                var line = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8) };
                _openLink = new FakeInput("https://…/name.mvaddon", 420);
                AutomationProperties.SetName(_openLink, "Link to an add-on");
                if (_openLinkText.Length > 0) _openLink.SetText(_openLinkText);
                _openLink.Changed += () => _openLinkText = _openLink?.Text ?? _openLinkText;
                _openLink.Submitted += OfferOpenLink;
                line.Children.Add(_openLink);
                line.Children.Add(SettingsButton("Download", OfferOpenLink));
                line.Children.Add(SettingsButton("Cancel", () =>
                {
                    _openEnteringLink = false;
                    RefreshOpenAddonRow();
                }));
                _openRow.Children.Add(line);
                _openRow.Children.Add(Note(
                    "The server you name will see this computer's network address, as any download does. Nothing else is sent."));
            }
            else
            {
                // Busy: shown, disabled (the Mac's .disabled(store.busy)), not hidden.
                var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8) };
                Button file = SettingsButton("Install from file…", ChooseOpenAddonFile);
                Button link = SettingsButton("Install from link…", () =>
                {
                    _openEnteringLink = true;
                    _openMessage = "";
                    RefreshOpenAddonRow();
                });
                file.IsEnabled = link.IsEnabled = !_openBusy;
                buttons.Children.Add(file);
                buttons.Children.Add(link);
                _openRow.Children.Add(buttons);
            }
        }
        // While busy a bar, as the Mac's AddonProgressView (OpenAddons.swift),
        // so a download never sits on frozen text; the last outcome otherwise.
        if (_openProgress.Length > 0)
        {
            _openProgressView = new AddonProgressView(320);
            _openProgressView.Show(_openFraction, _openProgress);
            _openRow.Children.Add(_openProgressView.Root);
        }
        else if (_openMessage.Length > 0)
        {
            _openRow.Children.Add(Note(_openMessage));
        }
    }

    private static FrameworkElement BuildOpenAddonEntry(OpenAddon addon)
    {
        var entry = new StackPanel { Spacing = Space(4), Margin = Pad(0, 4, 0, 4) };
        entry.Children.Add(Label($"{addon.Name} {addon.Version}").WithForeground(Brush(Title)));
        if (addon.Publisher.Length > 0)
            entry.Children.Add(Note($"From {addon.Publisher} · key {addon.Fingerprint} · {addon.Adds}"));
        if (addon.State == "needs_update")
            entry.Children.Add(Note("Needs a newer MediaViewer, so it is not in use."));
        else if (addon.State != "ok")
            entry.Children.Add(Note("This copy did not pass verification, so it is not in use."));
        ScriptState? script = addon.State == "ok" && _openScripts.TryGetValue(addon.Id, out ScriptState? s)
            ? s : null;
        if (script is not null)
        {
            // docs/design/25 PR 57: running, or stopped with the limit and why.
            entry.Children.Add(script.State switch
            {
                "running" => Note("Running."),
                "starting" => Note("Starting…"),
                _ => Note($"Stopped. {script.Message}").WithForeground(Brush(Title)),
            });
            if (script.LastError.Length > 0 && script.State != "stopped")
                entry.Children.Add(Note($"Its last error: {script.LastError}"));
            foreach (ScriptCommand c in script.Commands)
            {
                string keys = c.Key.Length == 0 ? "no key" : c.Key;
                entry.Children.Add(Note(c.Note.Length == 0
                    ? $"Command: {c.Name} ({keys})"
                    : $"Command: {c.Name}. {c.Note}."));
            }
        }
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8) };
        if (_openConfirmRemove == addon.Folder)
        {
            // PR 56: what was chosen on its page can outlive it, so installing
            // it again finds the same values (docs/design/25 "Settings page").
            if (addon.Settings)
            {
                var keep = new ToggleSwitch
                {
                    IsOn = _openKeepSettings,
                    FontFamily = UiFont,
                    FontSize = UiFontSize,
                    Foreground = Brush(Title),
                };
                keep.Toggled += (_, _) => _openKeepSettings = keep.IsOn;
                entry.Children.Add(SettingsRow("Keep its settings",
                    "Installing it again brings back what you chose on its page.", keep));
            }
            // Remove is the default (accent), Esc or Cancel keeps it.
            Button remove = SettingsButton($"Remove {addon.Name}", () => RemoveOpenAddon(addon), accent: true);
            remove.IsEnabled = !_openBusy;
            buttons.Children.Add(remove);
            buttons.Children.Add(SettingsButton("Cancel", () =>
            {
                _openConfirmRemove = null;
                RefreshOpenAddonRow();
            }));
        }
        else
        {
            if (script is not null && script.State == "stopped")
            {
                Button again = SettingsButton("Start again", () => RestartOpenScript(addon));
                ToolTipService.SetToolTip(again, $"Runs {addon.Name}'s script again from the start, with the same limits.");
                buttons.Children.Add(again);
            }
            if (addon.UpdateUrl.Length > 0 && addon.State != "invalid")
            {
                Button check = SettingsButton("Check for update", () => CheckOpenAddonUpdate(addon));
                string host = Uri.TryCreate(addon.UpdateUrl, UriKind.Absolute, out Uri? u)
                    ? u.Host : "its maker's server";
                ToolTipService.SetToolTip(check, $"Asks {host} for a newer version. Only when you click.");
                buttons.Children.Add(check);
            }
            buttons.Children.Add(SettingsButton("Remove…", () =>
            {
                _openConfirmRemove = addon.Folder;
                _openKeepSettings = true;
                RefreshOpenAddonRow();
            }));
            foreach (UIElement b in buttons.Children)
            {
                if (b is Button button) button.IsEnabled = !_openBusy;
            }
        }
        entry.Children.Add(buttons);
        // PR 56: its settings page, under its row.
        if (addon.State == "ok" && _openPages.TryGetValue(addon.Id, out AddonSettingsPage? page))
            entry.Children.Add(BuildSettingsPage(page));
        return entry;
    }

    // What a package is, and the question (docs/design/25 "Identity and trust"). Every
    // line but the description comes from the core. Cancel is first and takes
    // the focus: Enter does not install.
    private static FrameworkElement BuildOpenSheet(OpenOffer offer)
    {
        OpenAddon? a = offer.Addon;
        string title = a is null ? "This add-on cannot be installed"
            : !offer.Ok ? $"“{a.Name}” {a.Version} cannot be installed"
            : offer.Relation == "update" ? $"Update “{a.Name}” from {offer.InstalledVersion} to {a.Version}?"
            : offer.Relation == "repair" ? $"Install “{a.Name}” {a.Version} again?"
            : $"Install “{a.Name}” {a.Version}?";
        var sheet = new StackPanel { Spacing = Space(8) };
        TextBlock heading = WrappedLabel(title).WithForeground(Brush(Title));
        heading.FontWeight = Microsoft.UI.Text.FontWeights.SemiBold;
        sheet.Children.Add(heading);
        if (a is not null)
        {
            if (a.Description.Length > 0) sheet.Children.Add(Note(a.Description));
            var lines = new Grid { ColumnSpacing = Space(16), RowSpacing = Space(4) };
            lines.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            lines.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            void Line(string label, string value)
            {
                int row = lines.RowDefinitions.Count;
                lines.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
                TextBlock name = Label(label);
                // Not selectable: text selection pulls in text services, which
                // these islands cannot load (tools/check-winui-controls.ps1).
                TextBlock text = WrappedLabel(value).WithForeground(Brush(Title));
                Grid.SetRow(name, row);
                Grid.SetRow(text, row);
                Grid.SetColumn(text, 1);
                lines.Children.Add(name);
                lines.Children.Add(text);
            }
            Line("From", a.PublisherHost.Length == 0 ? a.Publisher : $"{a.Publisher} · {a.PublisherHost}");
            Line("Key", a.Fingerprint);
            Line("Adds", a.Adds);
            foreach (string can in a.Can) Line("Can", can);
            Line("Cannot", a.Cannot);
            Line("Size", $"{OpenSizeText(a.Size)} · {a.Licence}");
            sheet.Children.Add(lines);
        }
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8) };
        if (offer.Ok)
        {
            sheet.Children.Add(WrappedLabel("MediaViewer has not checked this add-on or who made it.")
                .WithForeground(Brush(Title)));
            // Cancel is the default (accent, the Mac's .defaultAction): Enter
            // does not install. Install stays while busy, disabled.
            Button cancel = SettingsButton("Cancel", CancelOpenOffer, accent: true);
            buttons.Children.Add(cancel);
            Button go = SettingsButton(offer.Relation == "update" ? "Update" : "Install", ConfirmOpenOffer);
            go.IsEnabled = !_openBusy;
            buttons.Children.Add(go);
            cancel.Loaded += (_, _) =>
            {
                try { cancel.Focus(FocusState.Keyboard); }
                catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
            };
        }
        else
        {
            sheet.Children.Add(WrappedLabel(offer.Message).WithForeground(Brush(Title)));
            buttons.Children.Add(SettingsButton("Close", CancelOpenOffer, accent: true));
        }
        sheet.Children.Add(buttons);
        var card = new Border
        {
            Child = sheet,
            Padding = Pad(14),
            Background = Brush(ChromeColour.Surface),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(1),
            CornerRadius = Corner(8),
        };
        AutomationProperties.SetName(card, title);
        // The question is at the foot of Settings: bring it into view.
        card.Loaded += (_, _) =>
        {
            try { card.StartBringIntoView(); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
        };
        return card;
    }

    private static string OpenSizeText(long bytes) =>
        bytes >= 1024 * 1024 ? $"{bytes / (1024.0 * 1024.0):0.0} MB"
        : bytes >= 1024 ? $"{Math.Max(1, (long)Math.Round(bytes / 1024.0))} KB"
        : $"{bytes} bytes";

    // ---- from a file ----------------------------------------------------------------

    private static async void ChooseOpenAddonFile()
    {
        try
        {
            var picker = new FileOpenPicker();
            picker.FileTypeFilter.Add(".mvaddon");
            WinRT.Interop.InitializeWithWindow.Initialize(picker, _themeWindow);
            Windows.Storage.StorageFile? file = await picker.PickSingleFileAsync();
            if (file is not null) InspectOpenAddon(file.Path, temporary: false, updating: null);
        }
        catch (Exception ex)
        {
            // Never let an exception out of a XAML event: that is a fail-fast.
            System.Diagnostics.Debug.WriteLine(ex);
        }
    }

    /// <summary>
    /// Native: an add-on package was handed to the app (a drop, Open with, the
    /// command line). In: { int32 bytes; UTF-8 path }. Native has opened
    /// Settings; this shows what the package is and asks. Nothing is installed
    /// without that answer.
    /// </summary>
    public static int OfferAddon(IntPtr arg, int sizeBytes)
    {
        try
        {
            if (arg == IntPtr.Zero || sizeBytes < 4) return unchecked((int)0x80070057);
            int len = Marshal.ReadInt32(arg);
            if (len <= 0 || 4 + len > sizeBytes) return unchecked((int)0x80070057);
            byte[] bytes = new byte[len];
            Marshal.Copy(arg + 4, bytes, 0, len);
            string path = System.Text.Encoding.UTF8.GetString(bytes);
            // The General page holds the sheet, whichever page was last open.
            if (_settingsHost is not null && _showGeneralSettings is not null) _showGeneralSettings();
            else _settingsKeyboard = false;
            InspectOpenAddon(path, temporary: false, updating: null);
            return 0;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            return unchecked((int)0x80004005);
        }
    }

    // ---- from a link ----------------------------------------------------------------

    private static Uri? HttpsLink(string text)
    {
        if (!Uri.TryCreate(text.Trim(), UriKind.Absolute, out Uri? uri)) return null;
        if (uri.Scheme != Uri.UriSchemeHttps || uri.Host.Length == 0 || uri.UserInfo.Length > 0) return null;
        return uri;
    }

    private static void OfferOpenLink()
    {
        _openLinkText = _openLink?.Text ?? _openLinkText;
        Uri? uri = HttpsLink(_openLinkText);
        if (uri is null)
        {
            _openMessage = "A link to an add-on starts with https://.";
            RefreshOpenAddonRow();
            return;
        }
        DownloadOpenAddon(uri, updating: null);
    }

    private static void CheckOpenAddonUpdate(OpenAddon addon)
    {
        Uri? uri = HttpsLink(addon.UpdateUrl);
        if (uri is not null) DownloadOpenAddon(uri, addon);
    }

    private sealed class OpenDownloadRefused(string message) : Exception(message);

    private static void DownloadOpenAddon(Uri uri, OpenAddon? updating)
    {
        if (_openBusy) return;
        _openBusy = true;
        _openEnteringLink = false;
        _openMessage = "";
        _openProgress = "Connecting…";
        _openFraction = null;
        RefreshOpenAddonRow();
        string target = Path.Combine(Path.GetTempPath(), $"mediaviewer-addon-{Guid.NewGuid():N}.mvaddon");
        var progress = new UiProgress<(long Done, long Total)>(p =>
        {
            if (!_openBusy) return;
            // "1.2 MB of 3.4 MB" with a bar once the server sent a length; the
            // running size over a sweeping bar when it did not.
            _openFraction = p.Total > 0 ? Math.Min(1.0, (double)p.Done / p.Total) : null;
            _openProgress = p.Total > 0 ? $"{OpenSizeText(p.Done)} of {OpenSizeText(p.Total)}"
                                        : $"Downloading… {OpenSizeText(p.Done)}";
            if (_openProgressView is not null) _openProgressView.Show(_openFraction, _openProgress);
            else RefreshOpenAddonRow();
        });
        _ = Task.Run(async () =>
        {
            string failure = "";
            try
            {
                await FetchOpenAddon(uri, target, progress).ConfigureAwait(false);
            }
            catch (OpenDownloadRefused ex)
            {
                failure = ex.Message;
            }
            catch (Exception ex) when (ex is HttpRequestException or IOException or TaskCanceledException
                                           or UnauthorizedAccessException)
            {
                failure = "The add-on could not be downloaded. Check the link and the connection.";
            }
            DispatcherQueueControllerTryEnqueue(() =>
            {
                _openBusy = false;
                _openProgress = "";
                if (failure.Length > 0)
                {
                    DeleteQuietly(target);
                    _openMessage = failure;
                    _openEnteringLink = updating is null;  // the link is still there to fix
                    RefreshOpenAddonRow();
                    return;
                }
                InspectOpenAddon(target, temporary: true, updating);
            });
        });
    }

    // One GET, https at every hop, 64 MB at most. Worker.
    private static async Task FetchOpenAddon(Uri uri, string target, IProgress<(long Done, long Total)> progress)
    {
        for (int hop = 0; hop < 6; ++hop)
        {
            using var request = new HttpRequestMessage(HttpMethod.Get, uri);
            using HttpResponseMessage response = await OpenAddonHttp
                .SendAsync(request, HttpCompletionOption.ResponseHeadersRead).ConfigureAwait(false);
            int code = (int)response.StatusCode;
            if (code is 301 or 302 or 303 or 307 or 308)
            {
                Uri? next = response.Headers.Location;
                if (next is null) throw new OpenDownloadRefused("The server did not send the add-on.");
                if (!next.IsAbsoluteUri) next = new Uri(uri, next);
                // A link that was safe to read must not end somewhere that is not.
                if (next.Scheme != Uri.UriSchemeHttps)
                    throw new OpenDownloadRefused("The link led somewhere that is not https, so it was not followed.");
                uri = next;
                continue;
            }
            if (code == 404) throw new OpenDownloadRefused("There is no add-on at that link.");
            if (code != 200) throw new OpenDownloadRefused("The server did not send the add-on.");
            const string tooLarge = "That download is larger than 64 MB, the most MediaViewer installs from a link.";
            if (response.Content.Headers.ContentLength is long length && length > OpenAddonMaxBytes)
                throw new OpenDownloadRefused(tooLarge);
            await using Stream input = await response.Content.ReadAsStreamAsync().ConfigureAwait(false);
            await using var output = new FileStream(target, FileMode.CreateNew, FileAccess.Write, FileShare.None);
            long total = response.Content.Headers.ContentLength ?? 0;
            byte[] buffer = new byte[64 * 1024];
            long done = 0;
            long reported = Environment.TickCount64;
            for (;;)
            {
                int got = await input.ReadAsync(buffer).ConfigureAwait(false);
                if (got == 0) break;
                done += got;
                if (done > OpenAddonMaxBytes) throw new OpenDownloadRefused(tooLarge);
                await output.WriteAsync(buffer.AsMemory(0, got)).ConfigureAwait(false);
                if (Environment.TickCount64 - reported >= 100)
                {
                    reported = Environment.TickCount64;
                    progress.Report((done, total));
                }
            }
            return;
        }
        throw new OpenDownloadRefused("The link redirected too many times.");
    }

    private static void DeleteQuietly(string path)
    {
        _ = Task.Run(() =>
        {
            try { File.Delete(path); }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                System.Diagnostics.Debug.WriteLine(ex.GetType().Name);
            }
        });
    }

    // ---- the answer -----------------------------------------------------------------

    private static void DiscardOpenOffer()
    {
        if (_openOffer is { Temporary: true } o) DeleteQuietly(o.Path);
        _openOffer = null;
    }

    private static void InspectOpenAddon(string path, bool temporary, OpenAddon? updating)
    {
        if (_openBusy) return;
        _openBusy = true;
        _openMessage = "";
        _openProgress = "Checking…";
        _openFraction = null;
        DiscardOpenOffer();
        RefreshOpenAddonRow();
        _ = Task.Run(() =>
        {
            OpenOffer found;
            try
            {
                using JsonDocument doc = JsonDocument.Parse(AddonNative.OpenInspect(path));
                JsonElement e = doc.RootElement;
                string message = Str(e, "message");
                found = new OpenOffer(path, temporary,
                    e.TryGetProperty("ok", out JsonElement ok) && ok.ValueKind == JsonValueKind.True,
                    message.Length == 0 ? "This file could not be read." : message, Str(e, "sha256"),
                    Str(e, "relation"), Str(e, "installed_version"),
                    e.TryGetProperty("publisher", out _) ? ParseOpenAddon(e) : null);
            }
            catch (Exception ex) when (ex is MediaViewerException or JsonException
                                           or DllNotFoundException or EntryPointNotFoundException)
            {
                found = new OpenOffer(path, temporary, false, "This file could not be read.", "", "", "", null);
            }
            DispatcherQueueControllerTryEnqueue(() =>
            {
                _openBusy = false;
                _openProgress = "";
                _openOffer = found;  // so a download of ours is deleted with it
                if (updating is not null)
                {
                    if (found.Addon is null || found.Addon.Id != updating.Id)
                    {
                        // An update link that serves something else is not an update.
                        DiscardOpenOffer();
                        _openMessage = $"The update link of {updating.Name} did not serve {updating.Name}, so nothing was installed.";
                    }
                    else if (found.Ok && found.Relation == "repair")
                    {
                        DiscardOpenOffer();
                        _openMessage = $"{updating.Name} {updating.Version} is the newest version.";
                    }
                }
                RefreshOpenAddonRow();
            });
        });
    }

    private static void ConfirmOpenOffer()
    {
        if (_openOffer is not { Ok: true } offer || _openBusy) return;
        _openBusy = true;
        _openProgress = "Installing…";
        _openFraction = null;
        RefreshOpenAddonRow();
        _ = Task.Run(() =>
        {
            bool ok = false;
            string why = "";
            try
            {
                using JsonDocument doc = JsonDocument.Parse(AddonNative.OpenInstall(offer.Path, offer.Sha256));
                ok = doc.RootElement.TryGetProperty("ok", out JsonElement o) && o.ValueKind == JsonValueKind.True;
                why = Str(doc.RootElement, "message");
            }
            catch (Exception ex) when (ex is MediaViewerException or JsonException
                                           or DllNotFoundException or EntryPointNotFoundException)
            {
                System.Diagnostics.Debug.WriteLine(ex.GetType().Name);
            }
            DispatcherQueueControllerTryEnqueue(() =>
            {
                _openBusy = false;
                _openProgress = "";
                string name = offer.Addon?.Name ?? "The add-on";
                string version = offer.Addon?.Version ?? "";
                if (ok)
                {
                    _openMessage = offer.Relation == "update" ? $"{name} updated to {version}."
                        : $"{name} {version} installed.";
                    if (offer.Relation == "fresh" && offer.Addon is { Themes.Count: > 0 })
                        _openMessage += " Choose its theme under Appearance.";
                }
                else
                {
                    _openMessage = why.Length == 0 ? $"{name} could not be installed." : why;
                }
                DiscardOpenOffer();
                RefreshOpenAddonRow();
                RefreshOpenAddons();
                ThemeAddonsChanged();
            });
        });
    }

    private static void CancelOpenOffer()
    {
        DiscardOpenOffer();
        _openMessage = "";
        RefreshOpenAddonRow();
    }

    private static void RemoveOpenAddon(OpenAddon addon)
    {
        _openConfirmRemove = null;
        if (_openBusy) return;
        _openBusy = true;
        bool forget = addon.Settings && !_openKeepSettings;
        RefreshOpenAddonRow();
        _ = Task.Run(() =>
        {
            bool removed = false;
            try
            {
                AddonNative.OpenRemove(addon.Folder);
                removed = true;
            }
            catch (Exception ex) when (ex is MediaViewerException or DllNotFoundException
                                           or EntryPointNotFoundException)
            {
                System.Diagnostics.Debug.WriteLine(ex.GetType().Name);
            }
            if (removed && forget)
            {
                try { AddonNative.SettingsForget(addon.Id); }
                catch (Exception ex) when (ex is MediaViewerException or EntryPointNotFoundException)
                {
                    System.Diagnostics.Debug.WriteLine(ex.GetType().Name);
                }
            }
            DispatcherQueueControllerTryEnqueue(() =>
            {
                _openBusy = false;
                _openMessage = removed ? $"{addon.Name} removed." : $"{addon.Name} could not be removed.";
                RefreshOpenAddonRow();
                RefreshOpenAddons();
                ThemeAddonsChanged();
            });
        });
    }
}
