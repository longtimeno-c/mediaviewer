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
/// Settings → Add-ons → From others (plan/23): add-ons from other makers, one
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

    private sealed record OpenAddon(string Folder, string Id, string Name, string Version, string State,
        string Description, string Licence, long Size, string UpdateUrl, string Publisher, string PublisherUrl,
        string Fingerprint, string Adds, IReadOnlyList<string> Can, string Cannot,
        IReadOnlyList<OpenTheme> Themes)
    {
        public string PublisherHost =>
            Uri.TryCreate(PublisherUrl, UriKind.Absolute, out Uri? u) ? u.Host : "";
    }

    /// <summary>A package that has been looked at and is waiting for an answer.</summary>
    private sealed record OpenOffer(string Path, bool Temporary, bool Ok, string Message, string Sha256,
        string Relation, string InstalledVersion, OpenAddon? Addon);

    private const long OpenAddonMaxBytes = 64L << 20;

    private static List<OpenAddon> _openAddons = new();
    private static OpenOffer? _openOffer;
    private static bool _openBusy;
    private static bool _openEnteringLink;
    private static string? _openConfirmRemove;
    private static string _openMessage = "";
    private static string _openProgress = "";
    private static StackPanel? _openRow;
    private static FakeInput? _openLink;
    private static ComboBox? _themePicker;
    private static TextBlock? _themeDetail;
    private static List<string> _themeKeys = new();

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
        string addonId = Str(e, "id");
        string folder = Str(e, "folder");
        string addonName = Str(e, "name");
        long size = e.TryGetProperty("size", out JsonElement s) && s.ValueKind == JsonValueKind.Number
            ? s.GetInt64() : 0;
        return new OpenAddon(folder.Length == 0 ? addonId : folder, addonId,
            addonName.Length == 0 ? addonId : addonName, Str(e, "version"), Str(e, "state"),
            Str(e, "description"), Str(e, "licence"), size, Str(e, "update_url"), Str(publisher, "name"),
            Str(publisher, "url"), Str(publisher, "fingerprint"), Str(e, "adds"), can, Str(e, "cannot"),
            themes);
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
            DispatcherQueueControllerTryEnqueue(() =>
            {
                _openAddons = list;
                RefreshOpenAddonRow();
                RefreshThemeRow();
            });
        });
    }

    // ---- Settings → Appearance → Theme -------------------------------------------

    private static FrameworkElement BuildThemeRow()
    {
        _themePicker = new ComboBox
        {
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Title),
            Width = 180,
        };
        _themePicker.SelectionChanged += (_, _) =>
        {
            if (_updatingSettingsUi || _themePicker is null) return;
            int i = _themePicker.SelectedIndex;
            if (i < 0 || i >= _themeKeys.Count) return;
            ChooseTheme(_themeKeys[i]);
        };
        FrameworkElement row = SettingsRow("Theme", ThemeDetail(), _themePicker);
        _themeDetail = FindDetail(row);
        RefreshThemeRow();
        return row;
    }

    private static string ThemeDetail() => _themeNote.Length > 0
        ? _themeNote
        : "Colours of the bars, panes and text. Themes come from add-ons; your photos are never recoloured.";

    // SettingsRow's second label, so the row can say why a theme is not showing.
    private static TextBlock? FindDetail(FrameworkElement row) =>
        row is Grid grid && grid.Children.Count > 0 && grid.Children[0] is StackPanel labels &&
        labels.Children.Count > 1 ? labels.Children[1] as TextBlock : null;

    private static void RefreshThemeRow()
    {
        if (_themePicker is null) return;
        bool was = _updatingSettingsUi;
        _updatingSettingsUi = true;
        try
        {
            _themeKeys = new List<string> { "" };
            _themePicker.Items.Clear();
            _themePicker.Items.Add("Default");
            foreach (OpenAddon addon in _openAddons)
            {
                if (addon.State != "ok") continue;
                foreach (OpenTheme theme in addon.Themes)
                {
                    _themeKeys.Add(ThemeKey(addon.Id, theme.Id));
                    _themePicker.Items.Add(addon.Themes.Count == 1 && theme.Name == addon.Name
                        ? theme.Name : $"{theme.Name} ({addon.Name})");
                }
            }
            // A choice whose add-on is not installed now stays listed, so the
            // picker shows what is chosen.
            if (_themeSelection.Length > 0 && !_themeKeys.Contains(_themeSelection))
            {
                _themeKeys.Add(_themeSelection);
                _themePicker.Items.Add("Not installed");
            }
            _themePicker.SelectedIndex = Math.Max(0, _themeKeys.IndexOf(_themeSelection));
            if (_themeDetail is not null) _themeDetail.Text = ThemeDetail();
        }
        finally
        {
            _updatingSettingsUi = was;
        }
    }

    // ---- Settings → Add-ons → From others ------------------------------------------

    private static void AddOpenAddonsSettings(StackPanel view)
    {
        view.Children.Add(Label("From others").WithForeground(Brush(Title)));
        view.Children.Add(WrappedLabel(
            "Add-ons made by other people, installed from a file or a link. MediaViewer does not check them or who made them. The ones it installs are data, such as themes: they cannot run code, read your files, or use the network."));
        _openRow = new StackPanel { Spacing = 6 };
        view.Children.Add(_openRow);
        RefreshOpenAddonRow();
        RefreshOpenAddons();
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
        if (_openOffer is not null)
        {
            _openRow.Children.Add(BuildOpenSheet(_openOffer));
        }
        else
        {
            foreach (OpenAddon addon in _openAddons) _openRow.Children.Add(BuildOpenAddonEntry(addon));
            if (_openEnteringLink)
            {
                var line = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
                _openLink = new FakeInput("https://…/name.mvaddon", 420);
                AutomationProperties.SetName(_openLink, "Link to an add-on");
                _openLink.Submitted += OfferOpenLink;
                line.Children.Add(_openLink);
                line.Children.Add(SettingsButton("Download", OfferOpenLink));
                line.Children.Add(SettingsButton("Cancel", () =>
                {
                    _openEnteringLink = false;
                    RefreshOpenAddonRow();
                }));
                _openRow.Children.Add(line);
                _openRow.Children.Add(WrappedLabel(
                    "The server you name will see this computer's network address, as any download does. Nothing else is sent."));
            }
            else if (!_openBusy)
            {
                var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
                buttons.Children.Add(SettingsButton("Install from file…", ChooseOpenAddonFile));
                buttons.Children.Add(SettingsButton("Install from link…", () =>
                {
                    _openEnteringLink = true;
                    _openMessage = "";
                    RefreshOpenAddonRow();
                }));
                _openRow.Children.Add(buttons);
            }
        }
        string status = _openProgress.Length > 0 ? _openProgress : _openMessage;
        if (status.Length > 0) _openRow.Children.Add(WrappedLabel(status));
    }

    private static FrameworkElement BuildOpenAddonEntry(OpenAddon addon)
    {
        var entry = new StackPanel { Spacing = 4, Margin = new Thickness(0, 4, 0, 4) };
        entry.Children.Add(Label($"{addon.Name} {addon.Version}").WithForeground(Brush(Title)));
        if (addon.Publisher.Length > 0)
            entry.Children.Add(WrappedLabel($"From {addon.Publisher} · key {addon.Fingerprint} · {addon.Adds}"));
        if (addon.State == "needs_update")
            entry.Children.Add(WrappedLabel("Needs a newer MediaViewer, so it is not in use."));
        else if (addon.State != "ok")
            entry.Children.Add(WrappedLabel("This copy did not pass verification, so it is not in use."));
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        if (_openConfirmRemove == addon.Folder)
        {
            buttons.Children.Add(SettingsButton($"Remove {addon.Name}", () => RemoveOpenAddon(addon)));
            buttons.Children.Add(SettingsButton("Cancel", () =>
            {
                _openConfirmRemove = null;
                RefreshOpenAddonRow();
            }));
        }
        else if (!_openBusy)
        {
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
                RefreshOpenAddonRow();
            }));
        }
        entry.Children.Add(buttons);
        return entry;
    }

    // What a package is, and the question (plan/23 "Identity and trust"). Every
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
        var sheet = new StackPanel { Spacing = 8 };
        TextBlock heading = WrappedLabel(title).WithForeground(Brush(Title));
        heading.FontWeight = Microsoft.UI.Text.FontWeights.SemiBold;
        sheet.Children.Add(heading);
        if (a is not null)
        {
            if (a.Description.Length > 0) sheet.Children.Add(WrappedLabel(a.Description));
            var lines = new Grid { ColumnSpacing = 16, RowSpacing = 4 };
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
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        if (offer.Ok)
        {
            sheet.Children.Add(WrappedLabel("MediaViewer has not checked this add-on or who made it.")
                .WithForeground(Brush(Title)));
            Button cancel = SettingsButton("Cancel", CancelOpenOffer);
            buttons.Children.Add(cancel);
            if (!_openBusy)
            {
                buttons.Children.Add(SettingsButton(offer.Relation == "update" ? "Update" : "Install",
                    ConfirmOpenOffer));
            }
            cancel.Loaded += (_, _) =>
            {
                try { cancel.Focus(FocusState.Keyboard); }
                catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex); }
            };
        }
        else
        {
            sheet.Children.Add(WrappedLabel(offer.Message).WithForeground(Brush(Title)));
            buttons.Children.Add(SettingsButton("Close", CancelOpenOffer));
        }
        sheet.Children.Add(buttons);
        var card = new Border
        {
            Child = sheet,
            Padding = new Thickness(14),
            Background = Brush(ChromeColour.Surface),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(8),
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
        Uri? uri = HttpsLink(_openLink?.Text ?? "");
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
        RefreshOpenAddonRow();
        string target = Path.Combine(Path.GetTempPath(), $"mediaviewer-addon-{Guid.NewGuid():N}.mvaddon");
        var progress = new UiProgress<long>(done =>
        {
            if (!_openBusy) return;
            _openProgress = $"Downloading… {OpenSizeText(done)}";
            RefreshOpenAddonRow();
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
                    RefreshOpenAddonRow();
                    return;
                }
                InspectOpenAddon(target, temporary: true, updating);
            });
        });
    }

    // One GET, https at every hop, 64 MB at most. Worker.
    private static async Task FetchOpenAddon(Uri uri, string target, IProgress<long> progress)
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
                    progress.Report(done);
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
