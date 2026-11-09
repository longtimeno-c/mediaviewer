// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

/// <summary>
/// #612, Settings -> Storage (docs/design/04 "Thumbnail cache upkeep"): what
/// MediaViewer keeps on disk with sizes, the thumbnail cache's optional cap,
/// and Clear. The sizes come from the core (mv_storage_report_json), measured
/// on a worker: walking folders never runs on this thread (rule 1). The Mac
/// twin is SettingsView.swift's Storage section.
/// </summary>
public static partial class IslandHost
{
    private static StackPanel? _storageRows;
    // Thumbnails' Clear, which says "Clearing…" on itself while it runs.
    private static Button? _storageClear;
    private static ComboBox? _thumbCap;
    private static bool _storageMeasuring;

    // The cap's choices. 0, the default, is no cap: a cap too small brings
    // back cold thumbnails, so the page offers none smaller than 1 GB.
    private static readonly ulong[] ThumbCapChoices =
        { 0, 1_000_000_000, 2_000_000_000, 5_000_000_000, 10_000_000_000, 20_000_000_000 };

    private static string ThumbCapFile => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MediaViewer", "thumb-cap.txt");

    private static ulong _thumbCapBytes;

    // The shell's own places, which the core does not know: the crash
    // database (crash_reporter_win.cpp crashes_dir) and the telemetry spool
    // (telemetry.cpp spool_path, under the per-user install root).
    private static string StorageExtraPlaces()
    {
        string local = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MediaViewer");
        return "crash_reports\t" + Path.Combine(local, "Crashes") + "\n" +
               "telemetry\t" + Path.Combine(local, "telemetry");
    }

    /// <summary>Chrome start: the cap chosen before, handed to the core before
    /// its first sweep. A tiny file read on a worker.</summary>
    private static void LoadThumbCacheCap()
    {
        _ = Task.Run(() =>
        {
            try
            {
                if (!File.Exists(ThumbCapFile)) return;
                if (ulong.TryParse(File.ReadAllText(ThumbCapFile).Trim(), NumberStyles.None,
                        CultureInfo.InvariantCulture, out ulong cap))
                {
                    _thumbCapBytes = cap;
                    MediaViewerSession.SetThumbnailCacheCap(cap);
                }
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or MediaViewerException)
            {
            }
        });
    }

    private static void AddStorageSettings(StackPanel view)
    {
        view.Children.Add(SettingsSection("Storage"));
        _storageRows = new StackPanel { Spacing = Space(8) };
        view.Children.Add(_storageRows);
        _thumbCap = new ComboBox
        {
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Foreground = Brush(Title),
            Width = 180,
        };
        foreach (ulong choice in ThumbCapChoices)
            _thumbCap.Items.Add(choice == 0 ? "No limit" : MediaViewer.Shared.ByteSize.Text((long)choice));
        _thumbCap.SelectedIndex = Math.Max(0, Array.IndexOf(ThumbCapChoices, _thumbCapBytes));
        _thumbCap.SelectionChanged += (_, _) =>
        {
            if (_updatingSettingsUi || _thumbCap.SelectedIndex < 0) return;
            ulong cap = ThumbCapChoices[_thumbCap.SelectedIndex];
            _thumbCapBytes = cap;
            MediaViewerSession? session = _folderSession;
            _ = Task.Run(() =>
            {
                try
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(ThumbCapFile)!);
                    File.WriteAllText(ThumbCapFile, cap.ToString(CultureInfo.InvariantCulture));
                }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
                {
                }
                MediaViewerSession.SetThumbnailCacheCap(cap);
                // A new cap applies now: the sweep is a background job behind
                // the view, waiting while the viewer is busy.
                if (cap != 0) session?.SweepThumbnailCache();
                RefreshStorage();
            });
        };
        view.Children.Add(SettingsRow("Thumbnail cache limit",
            "The cache already removes thumbnails of files that are gone or changed. A limit also removes the oldest; with a small one, folders open slower.",
            _thumbCap));
        _storageRows.Children.Add(Label("Measuring…"));
    }

    /// <summary>Measures on a worker, then fills the rows on this thread.</summary>
    private static void RefreshStorage()
    {
        if (_storageMeasuring) return;
        _storageMeasuring = true;
        string extra = StorageExtraPlaces();
        _ = Task.Run(() =>
        {
            string json = "";
            try
            {
                json = MediaViewerSession.StorageReportJson(extra);
            }
            catch (MediaViewerException)
            {
            }
            DispatcherQueueControllerTryEnqueue(() =>
            {
                _storageMeasuring = false;
                FillStorageRows(json);
            });
        });
    }

    // Owner 2026-11-04: each add-on's storage is on its own page (Settings →
    // Add-ons), Local search's pieces (ai-audio, ai-docs, ai-faces, ai-large,
    // ai-cuda) with it; the Storage section shows their total. Store folder →
    // bytes, from the last measurement.
    private static readonly Dictionary<string, long> _storageAddons = new();

    private static string StoragePageOf(string folder) =>
        folder == "ai" || folder.StartsWith("ai-", StringComparison.Ordinal) ? LocalSearchPageId : folder;

    private static string StoragePieceWords(string folder) => folder switch
    {
        "ai" => "Photo and video search: the add-on, its models and the index",
        "ai-audio" => "Sound and speech search models",
        "ai-docs" => "Document search model",
        "ai-faces" => "People: the face models",
        "ai-large" => "The large picture model (Maxxed)",
        "ai-cuda" => "The NVIDIA graphics runtime",
        "enhance" => "The add-on and the models it downloaded",
        "places" => "The add-on and its place database",
        _ => "The add-on and its data",
    };

    /// <summary>An add-on page's "On this PC" block, from the last measurement.</summary>
    private static void FillAddonPageStorage(string page)
    {
        if (_addonPageStorage is null) return;
        _addonPageStorage.Children.Clear();
        var rows = _storageAddons.Where(kv => StoragePageOf(kv.Key) == page).OrderBy(kv => kv.Key).ToList();
        if (rows.Count == 0) return;
        var head = new Grid();
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        // The Mac's AddonStorageBlock: the heading and total at 15, pieces at 13.
        head.Children.Add(Label("On this PC", TypeSize(15)));
        TextBlock total = Label(MediaViewer.Shared.ByteSize.Text(rows.Sum(kv => kv.Value)), TypeSize(15));
        Grid.SetColumn(total, 1);
        head.Children.Add(total);
        _addonPageStorage.Children.Add(head);
        foreach (var (folder, bytes) in rows)
        {
            var line = new Grid { ColumnSpacing = Space(12) };
            line.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            line.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            line.Children.Add(Label(StoragePieceWords(folder), TypeSize(TypeNote), mute: true));
            TextBlock size = Label(MediaViewer.Shared.ByteSize.Text(bytes), TypeSize(TypeNote), mute: true);
            Grid.SetColumn(size, 1);
            line.Children.Add(size);
            _addonPageStorage.Children.Add(line);
        }
    }

    private static void FillStorageRows(string json)
    {
        if (_storageRows is null) return;
        _storageRows.Children.Clear();
        _storageClear = null;
        if (string.IsNullOrEmpty(json))
        {
            _storageRows.Children.Add(Label("Sizes are not available."));
            return;
        }
        try
        {
            using JsonDocument doc = JsonDocument.Parse(json);
            _storageAddons.Clear();
            long addonTotal = 0;
            foreach (JsonElement row in doc.RootElement.GetProperty("rows").EnumerateArray())
            {
                string id = row.GetProperty("id").GetString() ?? "";
                long bytes = row.GetProperty("bytes").GetInt64();
                if (id.StartsWith("addon:", StringComparison.Ordinal))
                {
                    _storageAddons[id["addon:".Length..]] = bytes;
                    addonTotal += bytes;
                    continue;  // on the add-on's own page
                }
                (string title, string detail) = StorageWords(id);
                if (title.Length == 0) continue;
                string size = MediaViewer.Shared.ByteSize.Text(bytes);
                if (id == "thumbnails")
                {
                    var right = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(12) };
                    var sizeText = Label(size);
                    sizeText.VerticalAlignment = VerticalAlignment.Center;
                    right.Children.Add(sizeText);
                    Button clear = SettingsButton("Clear", ClearThumbnailCache);
                    clear.IsEnabled = bytes > 0;
                    AutomationProperties.SetName(clear, "Clear thumbnails");
                    right.Children.Add(clear);
                    _storageClear = clear;
                    _storageRows.Children.Add(SettingsRow(title, detail, right));
                }
                else
                {
                    _storageRows.Children.Add(SettingsRow(title, detail, Label(size)));
                }
            }
            if (addonTotal > 0)
            {
                _storageRows.Children.Add(SettingsRow("Add-ons", "Each add-on's page (Settings → Add-ons) shows what it keeps.",
                                                      Label(MediaViewer.Shared.ByteSize.Text(addonTotal))));
            }
            if (_addonPage.Length > 0) FillAddonPageStorage(_addonPage);
        }
        catch (Exception ex) when (ex is JsonException or KeyNotFoundException or InvalidOperationException
                                       or FormatException)
        {
            _storageRows.Children.Clear();
            _storageRows.Children.Add(Label("Sizes are not available."));
        }
    }

    private static void ClearThumbnailCache()
    {
        MediaViewerSession? session = _folderSession;
        if (session is null) return;
        // The button says it on itself, as the Mac's; the rows stay where they
        // are until the new sizes replace them (nothing collapses and regrows).
        if (_storageClear is not null)
        {
            SetButtonText(_storageClear, "Clearing…");
            _storageClear.IsEnabled = false;
        }
        // Deletes thousands of files: a worker, never this thread.
        _ = Task.Run(() =>
        {
            try
            {
                session.ClearThumbnailCache();
            }
            catch (MediaViewerException)
            {
            }
            RefreshStorage();
        });
    }

    // What each row is called (io/storage.h has the ids). An add-on is its
    // folder name; the first-party ones by their product name.
    private static (string Title, string Detail) StorageWords(string id)
    {
        switch (id)
        {
            case "thumbnails":
                return ("Thumbnails", "Previews of the photos and videos you have browsed. They tidy themselves; Clear removes them all, and they are made again as you browse.");
            case "search_cache":
                return ("File search", "The tags of files in folders you have searched or sorted by date taken.");
            case "clipboard":
                return ("Clipboard copies", "Images copied with Ctrl+Alt+C.");
            case "metadata_snapshots":
                return ("Metadata snapshots", "What a file's details were before you changed them, so you can revert. Never removed automatically.");
            case "crash_reports":
                return ("Crash reports", "Kept on this computer. Nothing is sent without your answer.");
            case "telemetry":
                return ("Diagnostics", "Anonymous diagnostics waiting to be sent, when you have turned them on.");
            case "addon_settings":
                return ("Add-on settings", "What you chose on add-ons' settings pages.");
        }
        if (id.StartsWith("addon:", StringComparison.Ordinal))
        {
            string folder = id["addon:".Length..];
            return folder switch
            {
                "ai" => ("AI search", "The add-on and its index of your photos."),
                "places" => ("Places", "The add-on and its place database."),
                "import" => ("Import", "The add-on."),
                "driftwire" => ("Driftwire", "The add-on."),
                "enhance" => ("Enhance", "The add-on and the models it downloaded."),
                "instagram" => (InstagramName, "The add-on."),
                _ => (folder, "An add-on and its data."),
            };
        }
        if (id.StartsWith("open_addon:", StringComparison.Ordinal))
            return (id["open_addon:".Length..], "An add-on from another maker.");
        return ("", "");
    }
}
