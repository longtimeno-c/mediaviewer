// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Windows.Storage.Pickers;

namespace MediaViewer.Ai.Chrome;

/// <summary>
/// Settings → Local search → Import and export (plan/17 "Sharing an index",
/// 2026-09-28): the index of some folders written to one .mvindex file, and a
/// file merged into this index with each of its folders pointed at where those
/// files are on this PC (a NAS mapped to another letter, a copied card).
/// People and thumbnails travel only when ticked; People is off by default,
/// because the file then identifies the people in it.
/// </summary>
/// <remarks>
/// No text entry (the Settings island fail-fasts on a TextBox): folders are
/// chosen with the picker. export / import / transfer_json / cancel are
/// [no-block]; inspect_export reads the file on a worker. Progress is read on
/// the status tick while a transfer runs. The section is rebuilt only when it
/// changes shape, so a button keeps keyboard focus.
/// </remarks>
internal sealed partial class ManagePanel
{
    private sealed class ImportRoot
    {
        public long Id;
        public string Name = "";
        public string Was = "";
        public long Assets;
        public string Target = "";
        public bool Include = true;
    }

    private sealed class ImportPlan
    {
        public string File = "";
        public string From = "";
        public string Model = "";
        public bool PictureUsable;
        public int AdoptQuality;
        public string WhyNot = "";
        public long Faces = -1;  // -1: the file has no People
        public bool PeopleReady;
        public bool PeopleMatch;
        public long Thumbs;
        public List<ImportRoot> Roots = new();
    }

    private StackPanel? _transfer;
    private MediaViewer.Shared.FlatBar? _transferBar;
    private List<RootRow> _rootsShown = new();
    private bool _exportOpen;
    private readonly HashSet<ulong> _exportRoots = new();
    private bool _exportThumbs = true;
    private bool _exportPeople;
    private ImportPlan? _importPlan;
    private bool _importThumbs = true;
    private bool _importPeople;
    private ulong _transferJob;
    private bool _transferRunning;
    private bool _transferIsImport;
    private string _transferNote = "";

    private void AddTransferSection()
    {
        Root.Children.Add(Heading("Import and export"));
        _transfer = new StackPanel { Spacing = 8 };
        Root.Children.Add(_transfer);
        ShowTransfer();
    }

    private CheckBox Check(string label, bool on, Action<bool> changed, bool enabled = true)
    {
        var box = new CheckBox
        {
            Content = label,
            IsChecked = on,
            IsEnabled = enabled,
            FontFamily = _look.Font,
            FontSize = _look.FontSize - 2,
        };
        box.Checked += (_, _) => changed(true);
        box.Unchecked += (_, _) => changed(false);
        return box;
    }

    private void ShowTransfer()
    {
        if (_transfer is null) return;
        _transfer.Children.Clear();
        _transferBar = null;

        Button export = _look.Button("Export…", () =>
        {
            _importPlan = null;
            _exportOpen = true;
            _exportRoots.Clear();
            foreach (RootRow r in _rootsShown) _exportRoots.Add(r.Id);
            _exportPeople = false;
            ShowTransfer();
        });
        export.IsEnabled = !_transferRunning && _rootsShown.Count > 0;
        _transfer.Children.Add(Row("Export the index",
            "Save what is indexed to one file, to import on another computer or after moving a library, so nothing is indexed twice. Your photos and videos are not in it.",
            export));
        if (_exportOpen) _transfer.Children.Add(ExportOptions());

        Button import = _look.Button("Import…", () => _ = ChooseImport());
        import.IsEnabled = !_transferRunning;
        _transfer.Children.Add(Row("Import an index",
            "Add an exported index to this one. Folders already indexed here keep their own results; files that differ are indexed again.",
            import));
        if (_importPlan is not null) _transfer.Children.Add(ImportOptions(_importPlan));

        if (_transferRunning)
        {
            var row = new Grid { ColumnSpacing = 12 };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var labels = new StackPanel { Spacing = 4 };
            labels.Children.Add(_look.Text(_transferIsImport ? "Importing…" : "Exporting…", 12));
            _transferBar = new MediaViewer.Shared.FlatBar(_look[AddonColour.Hairline], _look[AddonColour.Accent]);
            labels.Children.Add(_transferBar.Root);
            row.Children.Add(labels);
            Button cancel = _look.Button("Cancel", () =>
            {
                try { _api.TransferCancel(); }
                catch (MediaViewerException) { }
            });
            Grid.SetColumn(cancel, 1);
            row.Children.Add(cancel);
            _transfer.Children.Add(_look.Card(row, 12));
        }
        if (_transferNote.Length > 0) _transfer.Children.Add(_look.Text(_transferNote, 12));
    }

    private FrameworkElement ExportOptions()
    {
        var box = new StackPanel { Spacing = 6 };
        box.Children.Add(_look.Text("Folders", 12));
        foreach (RootRow r in _rootsShown)
        {
            ulong id = r.Id;
            CheckBox c = Check(r.Path, _exportRoots.Contains(id), on =>
            {
                if (on) _exportRoots.Add(id);
                else _exportRoots.Remove(id);
            });
            ToolTipService.SetToolTip(c, r.Path);
            box.Children.Add(c);
        }
        box.Children.Add(Check("Include thumbnails (a larger file; the other computer shows the gallery at once)",
            _exportThumbs, on => _exportThumbs = on));
        bool peopleOn = _chrome.StatusValid && (_chrome.Status.Flags & MvAiStatus.FlagFacesReady) != 0;
        TextBlock warning = _look.Text(peopleOn
            ? "The file will identify the people in your photos. Share it only with people you trust."
            : "People is off, so there are no faces to include.", 12,
            peopleOn ? AddonColour.Title : AddonColour.Body);
        warning.Visibility = peopleOn && !_exportPeople ? Visibility.Collapsed : Visibility.Visible;
        box.Children.Add(Check("Include People (faces and names)", _exportPeople && peopleOn, on =>
        {
            _exportPeople = on;
            warning.Visibility = on || !peopleOn ? Visibility.Visible : Visibility.Collapsed;
        }, peopleOn));
        box.Children.Add(warning);
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        buttons.Children.Add(_look.Button("Export…", () => _ = Export(peopleOn), accent: true));
        buttons.Children.Add(_look.Button("Cancel", () =>
        {
            _exportOpen = false;
            ShowTransfer();
        }));
        box.Children.Add(buttons);
        return _look.Card(box, 12);
    }

    private async Task Export(bool peopleOn)
    {
        if (_exportRoots.Count == 0) return;
        var picker = new FileSavePicker { SuggestedFileName = "MediaViewer index" };
        picker.FileTypeChoices.Add("MediaViewer index", new List<string> { ".mvindex" });
        WinRT.Interop.InitializeWithWindow.Initialize(picker, _chrome.Host.MainWindow);
        Windows.Storage.StorageFile? file;
        try { file = await picker.PickSaveFileAsync(); }
        catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or UnauthorizedAccessException)
        {
            return;
        }
        if (file is null) return;
        MvAiTransfer flags = MvAiTransfer.None;
        if (_exportThumbs) flags |= MvAiTransfer.Thumbs;
        if (_exportPeople && peopleOn) flags |= MvAiTransfer.People;
        try
        {
            // The picker made an empty file; the export replaces it when done.
            _transferJob = _api.ExportIndex(file.Path, _exportRoots.ToList(), flags);
        }
        catch (MediaViewerException ex)
        {
            _transferNote = ex.Status == MvStatus.Busy ? "Another import or export is running." : "The index could not be exported.";
            ShowTransfer();
            return;
        }
        _exportOpen = false;
        Started(import: false);
    }

    private async Task ChooseImport()
    {
        var picker = new FileOpenPicker();
        picker.FileTypeFilter.Add(".mvindex");
        WinRT.Interop.InitializeWithWindow.Initialize(picker, _chrome.Host.MainWindow);
        Windows.Storage.StorageFile? file;
        try { file = await picker.PickSingleFileAsync(); }
        catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or UnauthorizedAccessException)
        {
            return;
        }
        if (file is null) return;
        _exportOpen = false;
        _transferNote = "Reading the file…";
        ShowTransfer();
        string path = file.Path;
        AiApi api = _api;
        _ = Task.Run(() =>
        {
            ImportPlan? plan = null;
            try
            {
                using JsonDocument doc = JsonDocument.Parse(api.InspectExport(path));  // [worker-thread]
                plan = ReadPlan(path, doc.RootElement);
            }
            catch (Exception ex) when (ex is MediaViewerException or JsonException or KeyNotFoundException
                                           or InvalidOperationException or FormatException) { }
            _chrome.Host.Post(() =>
            {
                _importPlan = plan;
                _transferNote = plan is null ? "That file is not a MediaViewer index, or it comes from a newer version." : "";
                _importThumbs = plan is not null && plan.Thumbs > 0;
                _importPeople = false;
                ShowTransfer();
            });
        });
    }

    private static ImportPlan ReadPlan(string file, JsonElement o)
    {
        var plan = new ImportPlan
        {
            File = file,
            From = o.TryGetProperty("from", out JsonElement f) ? f.GetString() ?? "" : "",
            Model = o.TryGetProperty("model", out JsonElement m) ? m.GetString() ?? "" : "",
            PictureUsable = o.TryGetProperty("picture_usable", out JsonElement pu) && pu.ValueKind == JsonValueKind.True,
            AdoptQuality = o.TryGetProperty("adopt_quality", out JsonElement aq) ? aq.GetInt32() : 0,
            WhyNot = o.TryGetProperty("why_not", out JsonElement wn) ? wn.GetString() ?? "" : "",
            Thumbs = o.TryGetProperty("thumbs", out JsonElement th) ? th.GetInt64() : 0,
        };
        if (o.TryGetProperty("people", out JsonElement p) && p.ValueKind == JsonValueKind.Object)
        {
            plan.Faces = p.TryGetProperty("faces", out JsonElement fc) ? fc.GetInt64() : 0;
            plan.PeopleReady = p.TryGetProperty("ready", out JsonElement pr) && pr.ValueKind == JsonValueKind.True;
            plan.PeopleMatch = p.TryGetProperty("match", out JsonElement pm) && pm.ValueKind == JsonValueKind.True;
        }
        foreach (JsonElement r in o.GetProperty("roots").EnumerateArray())
        {
            string was = r.TryGetProperty("path", out JsonElement rp) ? rp.GetString() ?? "" : "";
            // The same place here (a NAS on the same path) answers itself.
            bool here = r.TryGetProperty("exists", out JsonElement ex) && ex.ValueKind == JsonValueKind.True;
            plan.Roots.Add(new ImportRoot
            {
                Id = r.GetProperty("id").GetInt64(),
                Name = r.TryGetProperty("name", out JsonElement n) ? n.GetString() ?? "" : "",
                Was = was,
                Assets = r.TryGetProperty("assets", out JsonElement a) ? a.GetInt64() : 0,
                Target = here ? was : "",
            });
        }
        return plan;
    }

    private FrameworkElement ImportOptions(ImportPlan plan)
    {
        var box = new StackPanel { Spacing = 6 };
        long files = plan.Roots.Sum(r => r.Assets);
        string summary = $"{files:N0} files in {(plan.Roots.Count == 1 ? "1 folder" : $"{plan.Roots.Count} folders")}";
        if (plan.From.Length > 0) summary += $", exported on {plan.From}";
        if (plan.Model.Length > 0) summary += $", indexed with {plan.Model}";
        box.Children.Add(_look.Text(summary + ".", 12, AddonColour.Title));
        if (plan.WhyNot == "loading")
        {
            box.Children.Add(_look.Text(
                "Local search is still loading its models. The import waits for them, then uses this file's pictures if they were made with a model this PC has.", 12));
        }
        else if (!plan.PictureUsable)
        {
            box.Children.Add(_look.Text(plan.WhyNot == "no_models"
                ? "Local search could not load its models, so the pictures in this file cannot be used here."
                : "This file was indexed with another search model than this PC uses, so its pictures will be indexed again here. Clear this PC's index first to use the file's model instead.",
                12, AddonColour.Title));
        }
        else if (plan.AdoptQuality != 0)
        {
            box.Children.Add(_look.Text("This index is empty, so search will use the file's model" +
                                        (plan.Model.Length > 0 ? $" ({plan.Model})." : "."), 12));
        }
        box.Children.Add(_look.Text("Where are these folders on this PC?", 12));
        Button go = _look.Button("Import", RunImport, accent: true);
        void UpdateGo() => go.IsEnabled = plan.Roots.Any(r => r.Include) && plan.Roots.Where(r => r.Include).All(r => r.Target.Length > 0);
        foreach (ImportRoot r in plan.Roots)
        {
            var grid = new Grid { ColumnSpacing = 8 };
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            ImportRoot root = r;
            CheckBox include = Check("", root.Include, on =>
            {
                root.Include = on;
                UpdateGo();
            });
            include.MinWidth = 0;
            AutomationProperties.SetName(include, "Import " + root.Name);
            grid.Children.Add(include);
            var labels = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
            labels.Children.Add(_look.Text($"{root.Name} · {root.Assets:N0} files", 13, AddonColour.Title));
            TextBlock where = _look.Text(root.Target.Length > 0 ? root.Target : "Was " + root.Was, 12, wrap: false);
            ToolTipService.SetToolTip(where, root.Was);
            labels.Children.Add(where);
            Grid.SetColumn(labels, 1);
            grid.Children.Add(labels);
            Button choose = _look.Button(root.Target.Length > 0 ? "Change…" : "Choose…", () => _ = ChooseTarget(root));
            AutomationProperties.SetName(choose, "Choose where " + root.Name + " is on this PC");
            Grid.SetColumn(choose, 2);
            grid.Children.Add(choose);
            box.Children.Add(grid);
        }
        if (plan.Thumbs > 0)
        {
            box.Children.Add(Check("Import thumbnails", _importThumbs, on => _importThumbs = on));
        }
        if (plan.Faces > 0)
        {
            box.Children.Add(Check(plan.PeopleReady
                ? $"Import People ({plan.Faces:N0} faces)"
                : $"Import People ({plan.Faces:N0} faces; turns People on)", _importPeople, on => _importPeople = on));
            if (plan.PeopleReady && !plan.PeopleMatch)
            {
                box.Children.Add(_look.Text("Its faces come from another People model and would be left out.", 12));
            }
        }
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        buttons.Children.Add(go);
        buttons.Children.Add(_look.Button("Cancel", () =>
        {
            _importPlan = null;
            ShowTransfer();
        }));
        box.Children.Add(buttons);
        UpdateGo();
        return _look.Card(box, 12);
    }

    private async Task ChooseTarget(ImportRoot root)
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
        if (folder is null) return;
        root.Target = folder.Path;
        root.Include = true;
        ShowTransfer();
    }

    private void RunImport()
    {
        ImportPlan? plan = _importPlan;
        if (plan is null) return;
        var map = plan.Roots.Where(r => r.Include && r.Target.Length > 0)
                            .Select(r => new Dictionary<string, object> { ["id"] = r.Id, ["path"] = r.Target })
                            .ToList();
        if (map.Count == 0) return;
        MvAiTransfer flags = MvAiTransfer.None;
        if (_importThumbs && plan.Thumbs > 0) flags |= MvAiTransfer.Thumbs;
        if (_importPeople && plan.Faces > 0) flags |= MvAiTransfer.People;
        try
        {
            _transferJob = _api.ImportIndex(plan.File, JsonSerializer.Serialize(map), flags);
        }
        catch (MediaViewerException ex)
        {
            _transferNote = ex.Status == MvStatus.Busy ? "Another import or export is running." : "The index could not be imported.";
            ShowTransfer();
            return;
        }
        _importPlan = null;
        Started(import: true);
    }

    private void Started(bool import)
    {
        _transferRunning = true;
        _transferIsImport = import;
        _transferNote = "";
        ShowTransfer();
    }

    /// <summary>The status tick, while a transfer runs.</summary>
    private void PollTransfer()
    {
        if (!_transferRunning) return;
        try
        {
            using JsonDocument doc = JsonDocument.Parse(_api.TransferJson());  // [no-block]
            JsonElement t = doc.RootElement;
            if (t.GetProperty("id").GetUInt64() != _transferJob) return;
            if (_transferBar is not null) _transferBar.Value = t.GetProperty("fraction").GetDouble();
            if (t.GetProperty("done").ValueKind != JsonValueKind.True) return;
            _transferRunning = false;
            int status = t.GetProperty("status").GetInt32();
            JsonElement outcome = t.GetProperty("outcome");
            _transferNote = _transferIsImport ? ImportNote(status, outcome) : ExportNote(status, outcome);
        }
        catch (Exception ex) when (ex is MediaViewerException or JsonException or KeyNotFoundException
                                       or InvalidOperationException or FormatException)
        {
            return;
        }
        ShowTransfer();
        if (_transferIsImport)
        {
            RefreshSettings();
            RefreshRoots();
            _chrome.RefreshCoverage();
        }
    }

    private static long Count(JsonElement o, string key) =>
        o.ValueKind == JsonValueKind.Object && o.TryGetProperty(key, out JsonElement v) && v.ValueKind == JsonValueKind.Number
            ? v.GetInt64() : 0;

    private static string ExportNote(int status, JsonElement o)
    {
        if (status == (int)MvStatus.Cancelled) return "The export was cancelled.";
        if (status != (int)MvStatus.Ok) return "The index could not be exported.";
        string s = $"Exported {Count(o, "assets"):N0} files ({Look.Size(Count(o, "bytes"))}).";
        if (o.TryGetProperty("people_included", out JsonElement p) && p.ValueKind == JsonValueKind.True) s += " People included.";
        long missing = Count(o, "thumbs_missing");
        if (missing > 0) s += $" {missing:N0} thumbnails were not made yet and were left out.";
        return s;
    }

    private static string ImportNote(int status, JsonElement o)
    {
        if (status == (int)MvStatus.Cancelled) return "The import was cancelled. Nothing it had not finished was kept.";
        if (status != (int)MvStatus.Ok) return "The index could not be imported.";
        string s = $"Imported {Count(o, "added") + Count(o, "replaced"):N0} files.";
        long kept = Count(o, "kept");
        if (kept > 0) s += $" {kept:N0} already indexed here were kept.";
        if (o.TryGetProperty("picture_usable", out JsonElement pu) && pu.ValueKind == JsonValueKind.False)
            s += " Their pictures were indexed with another model, so they will be indexed again here.";
        if (Count(o, "adopted_quality") != 0) s += " Search now uses the model the file was made with.";
        if (Count(o, "faces") > 0) s += $" People: {Count(o, "faces"):N0} faces.";
        string why = o.TryGetProperty("people_why", out JsonElement w) ? w.GetString() ?? "" : "";
        if (why == "no_piece") s += " Install People to bring in its faces.";
        else if (why == "model") s += " Its faces came from another People model and were left out.";
        return s + " Folders are checked now; anything changed here is indexed again.";
    }
}
