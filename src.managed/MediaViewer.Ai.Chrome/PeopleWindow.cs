// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.ObjectModel;
using System.Text.Json;
using MediaViewer.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Shapes;
using Windows.ApplicationModel.DataTransfer;
using Windows.System;

namespace MediaViewer.Ai.Chrome;

internal sealed record PersonVm(ulong Id, string Name, long Faces, ulong CoverFace, double[] Box)
{
    public string Label => Name.Length > 0 ? Name : "Unnamed";
}

internal sealed record FaceVm(ulong Id, string Path, long PtsMs, double[] Box);

/// <summary>
/// People (plan/17 PR 24): the clusters as circular covers, a person's faces,
/// and the minimum corrections — rename, "Not this person" (Delete), "Split
/// into new person" on a multi-selection, and merging: drag a person onto
/// another, Ctrl-click several and "Merge into…", or the detail pane's
/// "Merge into…" (owner report, 2026-09-27: merge was hard to find). A window of its
/// own because naming needs a text field, which the Settings island cannot
/// host.
/// </summary>
/// <remarks>
/// Crops are made in the view from the stored box (an ImageBrush transform);
/// nothing is ever written to disk. Rule 6: names and paths are shown, never
/// logged. Keyboard: arrows move in either grid, Enter opens a person, F2
/// names them, Delete says "not this person", Esc goes back, then closes.
/// </remarks>
internal sealed class PeopleWindow : Window
{
    private const double Circle = 88, FaceSize = 104;

    private readonly AiChrome _chrome;
    private readonly AiApi _api;
    private readonly Look _look;
    private readonly ObservableCollection<PersonVm> _peopleItems = new();
    private readonly ObservableCollection<FaceVm> _faceItems = new();
    private readonly GridView _peopleGrid;
    private readonly GridView _faceGrid;
    private readonly TextBox _name;
    private readonly TextBlock _detailTitle;
    private readonly Button _reject;
    private readonly Button _split;
    private readonly Button _photos;
    private readonly Button _refine;
    private bool _refining;
    // Buttons with a Flyout, not DropDownButtons: that control has no default
    // style in this island host and fail-fasts when it enters the tree.
    private readonly Button _merge;
    private readonly Grid _detail;
    private readonly StackPanel _mergeBar;
    private readonly TextBlock _mergeCount;
    private readonly Button _mergeSelected;
    private readonly TextBlock _hint;
    private readonly TextBlock _note;
    private PersonVm? _person;
    private int _loadGeneration;
    // people_json in flight; more AI_PEOPLE events while it runs fold into one
    // more read after it (a face landing posts one for every face).
    private bool _loading;
    private bool _loadAgain;
    private int _noteGeneration;
    private const string DragPrefix = "mediaviewer-person:";

    internal PeopleWindow(AiChrome chrome)
    {
        _chrome = chrome;
        _api = chrome.Api;
        _look = chrome.Look;
        Title = "People";
        try { SystemBackdrop = new MicaBackdrop(); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine(ex.Message); }
        Native.Adopt(WinRT.Interop.WindowNative.GetWindowHandle(this), chrome.Host.MainWindow);

        _peopleGrid = new GridView
        {
            // Ctrl / Shift picks several people to merge.
            SelectionMode = ListViewSelectionMode.Extended,
            IsItemClickEnabled = true,
            CanDragItems = true,
            ItemsSource = _peopleItems,
            Padding = new Thickness(12),
        };
        _peopleGrid.ContainerContentChanging += (_, e) =>
        {
            if (e.InRecycleQueue || e.Item is not PersonVm p) return;
            e.ItemContainer.Content = PersonTile(p);
            AutomationProperties.SetName(e.ItemContainer, $"{p.Label}, {p.Faces} photos");
        };
        _peopleGrid.ItemClick += (_, e) =>
        {
            if (Modified()) return;  // Ctrl / Shift-click only selects
            if (e.ClickedItem is PersonVm p) ShowPerson(p, focusFaces: true);
        };
        _peopleGrid.SelectionChanged += (_, _) =>
        {
            UpdateMergeBar();
            if (_peopleGrid.SelectedItems.Count == 1 && _peopleGrid.SelectedItem is PersonVm p && p.Id != _person?.Id)
            {
                ShowPerson(p, focusFaces: false);
            }
        };
        // Double-click: their photos in the gallery (a click selects and shows
        // their faces here, for correcting).
        _peopleGrid.DoubleTapped += (_, e) =>
        {
            DependencyObject? d = e.OriginalSource as DependencyObject;
            while (d is not null and not GridViewItem) d = VisualTreeHelper.GetParent(d);
            if (d is GridViewItem item && _peopleGrid.ItemFromContainer(item) is PersonVm p)
            {
                _person = p;
                ShowPhotos();
                e.Handled = true;
            }
        };
        // Drag a person (or the selected people) onto another to merge them.
        _peopleGrid.DragItemsStarting += (_, e) =>
        {
            string ids = string.Join(",", e.Items.OfType<PersonVm>().Select(p => p.Id));
            if (ids.Length == 0)
            {
                e.Cancel = true;
                return;
            }
            e.Data.SetText(DragPrefix + ids);
            e.Data.RequestedOperation = DataPackageOperation.Move;
        };

        _faceGrid = new GridView
        {
            SelectionMode = ListViewSelectionMode.Extended,
            ItemsSource = _faceItems,
            Padding = new Thickness(8),
        };
        _faceGrid.ContainerContentChanging += (_, e) =>
        {
            if (e.InRecycleQueue || e.Item is not FaceVm f) return;
            e.ItemContainer.Content = FaceTile(f);
            AutomationProperties.SetName(e.ItemContainer, System.IO.Path.GetFileName(f.Path));
        };
        _faceGrid.SelectionChanged += (_, _) => UpdateButtons();

        _detailTitle = _look.Text("", 18, AddonColour.Title, wrap: false);
        _name = new TextBox { PlaceholderText = "Add a name", FontFamily = _look.Font, FontSize = 16, MinWidth = 240 };
        AutomationProperties.SetName(_name, "Name");
        _name.KeyDown += (_, e) =>
        {
            if (e.Key != VirtualKey.Enter) return;
            CommitName();
            _faceGrid.Focus(FocusState.Keyboard);
            e.Handled = true;
        };
        _name.LostFocus += (_, _) => CommitName();
        _merge = new Button { Content = "Merge into…  ▾", Flyout = new MenuFlyout() };
        ToolTipService.SetToolTip(_merge, "This person is someone else in the list: their faces join them");
        ((MenuFlyout)_merge.Flyout).Opening += (_, _) => FillMergeMenu();
        _photos = _look.Button("Show their photos", ShowPhotos);
        _reject = _look.Button("Not this person", RejectSelected);
        ToolTipService.SetToolTip(_reject, "The face leaves this person and never rejoins them (Delete)");
        _split = _look.Button("Split into new person", SplitSelected);
        _refine = _look.Button("Refine faces", RefinePerson);
        ToolTipService.SetToolTip(_refine, "Check every face against this person and move out the ones that don't match");

        var head = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        head.Children.Add(_name);
        head.Children.Add(_merge);
        head.Children.Add(_photos);
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        actions.Children.Add(_reject);
        actions.Children.Add(_split);
        actions.Children.Add(_refine);
        actions.Children.Add(_look.Text("Select faces (Ctrl / Shift for several) to correct them.", 12));
        _detail = new Grid { RowSpacing = 10, Padding = new Thickness(16), Visibility = Visibility.Collapsed };
        _detail.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        _detail.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        _detail.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        _detail.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        _detail.Children.Add(_detailTitle);
        Grid.SetRow(head, 1);
        _detail.Children.Add(head);
        Grid.SetRow(_faceGrid, 2);
        _detail.Children.Add(_faceGrid);
        Grid.SetRow(actions, 3);
        _detail.Children.Add(actions);

        var left = new Grid();
        left.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        left.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        var leftHead = new StackPanel { Padding = new Thickness(16, 16, 16, 0), Spacing = 6 };
        leftHead.Children.Add(_look.Text("People", 20, AddonColour.Title));
        leftHead.Children.Add(_look.Text("Found on this computer only. Face data is never shared, and can be deleted in Settings.", 12));
        _hint = _look.Text("Double-click a person to see their photos. The same person twice? Drag one onto the other, or Ctrl-click several and merge them.", 12);
        leftHead.Children.Add(_hint);
        // "3 people selected · Merge into… · Cancel": the target keeps its name.
        _mergeCount = _look.Text("", 12, AddonColour.Title, wrap: false);
        _mergeCount.VerticalAlignment = VerticalAlignment.Center;
        _mergeSelected = new Button { Content = "Merge into…  ▾", Flyout = new MenuFlyout() };
        ((MenuFlyout)_mergeSelected.Flyout).Opening += (_, _) => FillMergeSelectedMenu();
        _mergeBar = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 8,
            Padding = new Thickness(10, 6, 10, 6),
            CornerRadius = new CornerRadius(6),
            Background = _look.Tint(AddonColour.Accent, 28),
            Visibility = Visibility.Collapsed,
        };
        _mergeBar.Children.Add(_mergeCount);
        _mergeBar.Children.Add(_mergeSelected);
        _mergeBar.Children.Add(_look.Button("Cancel", () => _peopleGrid.SelectedItems.Clear()));
        leftHead.Children.Add(_mergeBar);
        _note = _look.Text("", 12, AddonColour.Title);
        _note.Visibility = Visibility.Collapsed;
        leftHead.Children.Add(_note);
        left.Children.Add(leftHead);
        Grid.SetRow(_peopleGrid, 1);
        left.Children.Add(_peopleGrid);

        var root = new Grid();
        root.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        root.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1.3, GridUnitType.Star) });
        root.Children.Add(left);
        Grid.SetColumn(_detail, 1);
        root.Children.Add(_detail);
        root.KeyDown += OnKeyDown;
        Content = root;
        UpdateButtons();
    }

    internal void Present()
    {
        AppWindow.MoveAndResize(Native.CentreOver(_chrome.Host.MainWindow, 1100, 720));
        Activate();
        Refresh();
        _peopleGrid.Focus(FocusState.Programmatic);
    }

    internal void OnThemeChanged()
    {
        // Brushes are shared and recolour in place; the crops keep theirs.
    }

    // ---- loading (worker) --------------------------------------------------------------

    internal void Refresh()
    {
        if (_loading)
        {
            _loadAgain = true;
            return;
        }
        _loading = true;
        int generation = ++_loadGeneration;
        AiApi api = _api;
        _ = Task.Run(() =>
        {
            var people = new List<PersonVm>();
            try
            {
                using JsonDocument doc = JsonDocument.Parse(api.PeopleJson());
                foreach (JsonElement p in doc.RootElement.EnumerateArray())
                {
                    people.Add(new PersonVm(
                        p.GetProperty("id").GetUInt64(),
                        p.TryGetProperty("name", out JsonElement n) ? n.GetString() ?? "" : "",
                        p.TryGetProperty("faces", out JsonElement f) ? f.GetInt64() : 0,
                        p.TryGetProperty("cover_face", out JsonElement c) ? c.GetUInt64() : 0,
                        Box(p, "cover_box")));
                }
            }
            // FormatException too: a read that throws must still clear _loading,
            // or the list would stop following the index.
            catch (Exception ex) when (ex is MediaViewerException or JsonException or InvalidOperationException
                                           or FormatException) { }
            DispatcherQueue.TryEnqueue(() =>
            {
                if (generation == _loadGeneration) Apply(people);
                if (!_loadAgain)
                {
                    _loading = false;
                    return;
                }
                // While faces stream in: at most two reads a second.
                _loadAgain = false;
                _ = Task.Delay(500).ContinueWith(_ => DispatcherQueue.TryEnqueue(() =>
                {
                    _loading = false;
                    Refresh();
                }), TaskScheduler.Default);
            });
        });
    }

    private static bool Same(PersonVm a, PersonVm b) =>
        a.Id == b.Id && a.Name == b.Name && a.Faces == b.Faces && a.CoverFace == b.CoverFace;

    /// <summary>
    /// The new list, changed in place where it can be: a rebuilt grid loses
    /// its scroll, selection and crops every time a face lands.
    /// </summary>
    private void Apply(List<PersonVm> people)
    {
        var selected = _peopleGrid.SelectedItems.OfType<PersonVm>().Select(p => p.Id).ToHashSet();
        ulong? keep = _person?.Id;
        if (_peopleItems.Select(p => p.Id).SequenceEqual(people.Select(p => p.Id)))
        {
            for (int i = 0; i < people.Count; ++i)
            {
                if (!Same(_peopleItems[i], people[i])) _peopleItems[i] = people[i];
            }
        }
        else
        {
            _peopleItems.Clear();
            foreach (PersonVm p in people) _peopleItems.Add(p);
        }
        foreach (PersonVm p in _peopleItems)
        {
            if (selected.Contains(p.Id) && !_peopleGrid.SelectedItems.Contains(p)) _peopleGrid.SelectedItems.Add(p);
        }
        PersonVm? again = keep is ulong id ? _peopleItems.FirstOrDefault(p => p.Id == id) : null;
        if (again is null)
        {
            _person = null;
            _detail.Visibility = Visibility.Collapsed;
        }
        else if (_person is null || !Same(_person, again))
        {
            // Its faces changed (more found, a merge): read them again.
            if (selected.Count <= 1 && !_peopleGrid.SelectedItems.Contains(again)) _peopleGrid.SelectedItem = again;
            ShowPerson(again, focusFaces: false);
        }
        UpdateMergeBar();
        UpdateButtons();
    }

    private static double[] Box(JsonElement e, string key)
    {
        if (!e.TryGetProperty(key, out JsonElement b) || b.ValueKind != JsonValueKind.Array || b.GetArrayLength() != 4)
        {
            return new[] { 0.0, 0.0, 1.0, 1.0 };
        }
        return b.EnumerateArray().Select(v => v.GetDouble()).ToArray();
    }

    private void ShowPerson(PersonVm p, bool focusFaces)
    {
        _person = p;
        _detail.Visibility = Visibility.Visible;
        _detailTitle.Text = p.Faces == 1 ? $"{p.Label} · 1 photo" : $"{p.Label} · {p.Faces:N0} photos";
        _name.Text = p.Name;
        _faceItems.Clear();
        UpdateButtons();
        ulong id = p.Id;
        AiApi api = _api;
        _ = Task.Run(() =>
        {
            var faces = new List<FaceVm>();
            try
            {
                using JsonDocument doc = JsonDocument.Parse(api.PersonFacesJson(id));
                foreach (JsonElement f in doc.RootElement.EnumerateArray())
                {
                    faces.Add(new FaceVm(
                        f.GetProperty("face").GetUInt64(),
                        f.TryGetProperty("path", out JsonElement path) ? path.GetString() ?? "" : "",
                        f.TryGetProperty("pts_ms", out JsonElement pts) ? pts.GetInt64() : -1,
                        Box(f, "box")));
                }
            }
            catch (Exception ex) when (ex is MediaViewerException or JsonException or InvalidOperationException) { }
            DispatcherQueue.TryEnqueue(() =>
            {
                if (_person?.Id != id) return;
                _faceItems.Clear();
                foreach (FaceVm f in faces) _faceItems.Add(f);
                if (focusFaces) _faceGrid.Focus(FocusState.Keyboard);
            });
        });
    }

    // ---- tiles -----------------------------------------------------------------------------

    private UIElement PersonTile(PersonVm p)
    {
        var panel = new StackPanel
        {
            Width = Circle + 24,
            Spacing = 6,
            Padding = new Thickness(4),
            AllowDrop = true,
            CornerRadius = new CornerRadius(8),
        };
        ToolTipService.SetToolTip(panel, "Drag onto another person to merge them");
        panel.DragOver += (_, e) =>
        {
            if (!e.DataView.Contains(StandardDataFormats.Text)) return;
            e.AcceptedOperation = DataPackageOperation.Move;
            e.DragUIOverride.Caption = $"Merge into {p.Label}";
            e.DragUIOverride.IsGlyphVisible = false;
            panel.Background = _look.Tint(AddonColour.Accent, 40);
        };
        panel.DragLeave += (_, _) => panel.Background = null;
        panel.Drop += async (_, e) =>
        {
            panel.Background = null;
            if (!e.DataView.Contains(StandardDataFormats.Text)) return;
            DragOperationDeferral deferral = e.GetDeferral();
            try
            {
                string text = await e.DataView.GetTextAsync();
                if (!text.StartsWith(DragPrefix, StringComparison.Ordinal)) return;
                var ids = text[DragPrefix.Length..].Split(',')
                    .Select(t => ulong.TryParse(t, out ulong v) ? v : 0)
                    .Where(v => v != 0 && v != p.Id)
                    .ToList();
                PersonVm? target = _peopleItems.FirstOrDefault(x => x.Id == p.Id);
                if (target is not null && ids.Count > 0) Merge(target, ids);
            }
            catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or InvalidOperationException) { }
            finally
            {
                deferral.Complete();
            }
        };
        panel.Children.Add(Crop(p.CoverFace, p.Box, Circle, round: true, p.Label));
        TextBlock name = _look.Text(p.Label, 13, p.Name.Length > 0 ? AddonColour.Title : AddonColour.Body, wrap: false);
        name.HorizontalAlignment = HorizontalAlignment.Center;
        panel.Children.Add(name);
        TextBlock count = _look.Text($"{p.Faces:N0}", 11, AddonColour.Body, wrap: false);
        count.HorizontalAlignment = HorizontalAlignment.Center;
        panel.Children.Add(count);
        return panel;
    }

    private UIElement FaceTile(FaceVm f)
    {
        FrameworkElement crop = Crop(f.Id, f.Box, FaceSize, round: false, System.IO.Path.GetFileName(f.Path));
        ToolTipService.SetToolTip(crop, f.PtsMs >= 0
            ? $"{System.IO.Path.GetFileName(f.Path)} at {Look.Moment(f.PtsMs)}"
            : System.IO.Path.GetFileName(f.Path));
        return crop;
    }

    // face id -> the JPEG it was found in (face_thumb); a person's faces are
    // often one photo, and the grid re-realises tiles as it scrolls.
    private readonly Dictionary<ulong, string> _thumbPaths = new();
    private readonly SemaphoreSlim _thumbGate = new(3);

    /// <summary>
    /// The face region, cropped by an ImageBrush transform in the view from
    /// the JPEG face_thumb hands out (the still's JPEG-512 or the moment's
    /// thumb, any format). Nothing is ever written.
    /// </summary>
    private FrameworkElement Crop(ulong face, double[] box, double size, bool round, string label)
    {
        Shape shape = round ? new Ellipse() : new Rectangle { RadiusX = 8, RadiusY = 8 };
        shape.Width = size;
        shape.Height = size;
        shape.Fill = _look.Tint(AddonColour.Surface, 255);
        var initial = _look.Text(label.Length > 0 ? label[..1].ToUpperInvariant() : "?", size / 2.6, AddonColour.Body, wrap: false);
        initial.HorizontalAlignment = HorizontalAlignment.Center;
        initial.VerticalAlignment = VerticalAlignment.Center;
        var grid = new Grid { Width = size, Height = size, HorizontalAlignment = HorizontalAlignment.Center };
        grid.Children.Add(shape);
        grid.Children.Add(initial);
        if (face == 0) return grid;
        double x = box[0], y = box[1], w = Math.Max(0.01, box[2]), h = Math.Max(0.01, box[3]);
        // Pad the face a little so a crop reads as a portrait, not a mask.
        const double Pad = 0.25;
        double px = Math.Max(0, x - w * Pad), py = Math.Max(0, y - h * Pad);
        double pw = Math.Min(1 - px, w * (1 + 2 * Pad)), ph = Math.Min(1 - py, h * (1 + 2 * Pad));
        void Show(string path)
        {
            try
            {
                var bitmap = new BitmapImage { DecodePixelWidth = (int)Math.Min(1024, size * 2 / pw) };
                bitmap.UriSource = new Uri(path);
                var brush = new ImageBrush
                {
                    ImageSource = bitmap,
                    Stretch = Stretch.Fill,
                    // Relative coordinates: the box [px, px+pw] maps to [0, 1].
                    RelativeTransform = new CompositeTransform
                    {
                        ScaleX = 1 / pw,
                        ScaleY = 1 / ph,
                        TranslateX = -px / pw,
                        TranslateY = -py / ph,
                    },
                };
                bitmap.ImageOpened += (_, _) =>
                {
                    shape.Fill = brush;
                    initial.Visibility = Visibility.Collapsed;
                };
            }
            catch (Exception ex) when (ex is UriFormatException or ArgumentException) { }
        }

        if (_thumbPaths.TryGetValue(face, out string? known))
        {
            Show(known);
            return grid;
        }
        AiApi api = _api;
        _ = Task.Run(async () =>
        {
            await _thumbGate.WaitAsync().ConfigureAwait(false);
            string? path = null;
            try { path = api.FaceThumb(face); }  // [worker-thread]: may make the JPEG
            catch (MediaViewerException) { }
            finally { _thumbGate.Release(); }
            if (string.IsNullOrEmpty(path)) return;
            DispatcherQueue.TryEnqueue(() =>
            {
                _thumbPaths[face] = path;
                Show(path);
            });
        });
        return grid;
    }

    // ---- corrections ------------------------------------------------------------------------

    private List<ulong> SelectedFaces() => _faceGrid.SelectedItems.OfType<FaceVm>().Select(f => f.Id).ToList();

    private void UpdateButtons()
    {
        int n = _faceGrid.SelectedItems.Count;
        _reject.IsEnabled = n > 0;
        _split.IsEnabled = n > 0 && n < _faceItems.Count;
        _merge.IsEnabled = _person is not null && _peopleItems.Count > 1;
        _photos.IsEnabled = _person is not null;
        _refine.IsEnabled = _person is not null && !_refining;
    }

    private void CommitName()
    {
        if (_person is null) return;
        string name = _name.Text.Trim();
        if (name == _person.Name) return;
        try { _api.PersonRename(_person.Id, name); }
        catch (MediaViewerException) { return; }
        _person = _person with { Name = name };
        _detailTitle.Text = _person.Faces == 1 ? $"{_person.Label} · 1 photo" : $"{_person.Label} · {_person.Faces:N0} photos";
    }

    private void RejectSelected()
    {
        List<ulong> faces = SelectedFaces();
        if (faces.Count == 0) return;
        foreach (ulong f in faces)
        {
            try { _api.FaceReject(f); }
            catch (MediaViewerException) { }
        }
        foreach (FaceVm vm in _faceItems.Where(v => faces.Contains(v.Id)).ToList()) _faceItems.Remove(vm);
        UpdateButtons();
    }

    private void SplitSelected()
    {
        List<ulong> faces = SelectedFaces();
        if (faces.Count == 0) return;
        try { _api.FaceSplit(faces); }
        catch (MediaViewerException) { return; }
        foreach (FaceVm vm in _faceItems.Where(v => faces.Contains(v.Id)).ToList()) _faceItems.Remove(vm);
        UpdateButtons();
        // AI_PEOPLE brings the new person into the list.
    }

    private void RefinePerson()
    {
        if (_person is null || _refining) return;
        PersonVm p = _person;
        _refining = true;
        UpdateButtons();
        AiApi api = _api;
        _ = Task.Run(() =>
        {
            uint? removed = null;
            try { removed = api.PersonRefine(p.Id); }
            catch (MediaViewerException) { }
            DispatcherQueue.TryEnqueue(() =>
            {
                _refining = false;
                ShowNote(removed switch
                {
                    null => "Couldn't refine this person. Try again.",
                    0 => $"All of {p.Label}'s faces match.",
                    1 => $"1 face moved out of {p.Label}.",
                    _ => $"{removed} faces moved out of {p.Label}.",
                });
                if (_person?.Id == p.Id) ShowPerson(_person, focusFaces: false);
                else UpdateButtons();
                Refresh();
            });
        });
    }

    private void FillMergeMenu()
    {
        var menu = (MenuFlyout)_merge.Flyout;
        menu.Items.Clear();
        if (_person is null) return;
        foreach (PersonVm other in _peopleItems.Where(p => p.Id != _person.Id))
        {
            ulong into = other.Id;
            var item = new MenuFlyoutItem { Text = $"{other.Label} ({other.Faces:N0})" };
            item.Click += (_, _) =>
            {
                if (_person is null) return;
                Merge(other, new List<ulong> { _person.Id });
            };
            menu.Items.Add(item);
        }
    }

    private static bool Modified() =>
        Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Control).HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down) ||
        Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Shift).HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);

    private void UpdateMergeBar()
    {
        int n = _peopleGrid.SelectedItems.Count;
        bool several = n >= 2;
        _mergeBar.Visibility = several ? Visibility.Visible : Visibility.Collapsed;
        _hint.Visibility = several || _peopleItems.Count < 2 ? Visibility.Collapsed : Visibility.Visible;
        _mergeCount.Text = $"{n} people selected";
    }

    private void FillMergeSelectedMenu()
    {
        var menu = (MenuFlyout)_mergeSelected.Flyout;
        menu.Items.Clear();
        List<PersonVm> chosen = _peopleGrid.SelectedItems.OfType<PersonVm>().ToList();
        foreach (PersonVm target in chosen)
        {
            var item = new MenuFlyoutItem { Text = $"{target.Label} ({target.Faces:N0})" };
            item.Click += (_, _) => Merge(target, chosen.Select(p => p.Id).ToList());
            menu.Items.Add(item);
        }
    }

    /// <summary>
    /// Several people are one: their faces move to <paramref name="target"/>,
    /// which keeps its name (or takes the first name among them, as faces_db
    /// does). The list changes now; person_merge can wait for the indexer, so
    /// the rows are rewritten on a worker and AI_PEOPLE brings the truth.
    /// </summary>
    private void Merge(PersonVm target, List<ulong> ids)
    {
        List<ulong> from = ids.Where(id => id != target.Id).Distinct().ToList();
        List<PersonVm> moved = _peopleItems.Where(p => from.Contains(p.Id)).ToList();
        if (moved.Count == 0) return;
        string name = target.Name.Length > 0 ? target.Name : moved.FirstOrDefault(p => p.Name.Length > 0)?.Name ?? "";
        PersonVm merged = target with { Name = name, Faces = target.Faces + moved.Sum(p => p.Faces) };
        _peopleGrid.SelectedItems.Clear();
        foreach (PersonVm p in moved) _peopleItems.Remove(p);
        int at = _peopleItems.IndexOf(target);
        if (at >= 0) _peopleItems[at] = merged;
        ShowNote(moved.Count == 1
            ? $"Merged into {merged.Label}."
            : $"Merged {moved.Count + 1} people into {merged.Label}.");
        _peopleGrid.SelectedItem = merged;
        ShowPerson(merged, focusFaces: false);
        AiApi api = _api;
        _ = Task.Run(() =>
        {
            bool failed = false;
            foreach (ulong f in from)
            {
                try { api.PersonMerge(target.Id, f); }
                catch (MediaViewerException) { failed = true; }
            }
            DispatcherQueue.TryEnqueue(() =>
            {
                if (failed) ShowNote("Some faces could not be merged. Try again.");
                Refresh();
            });
        });
    }

    /// <summary>A line under the header that clears itself after a few seconds.</summary>
    private void ShowNote(string text)
    {
        int g = ++_noteGeneration;
        _note.Text = text;
        _note.Visibility = Visibility.Visible;
        _ = Task.Delay(5000).ContinueWith(_ => DispatcherQueue.TryEnqueue(() =>
        {
            if (g == _noteGeneration) _note.Visibility = Visibility.Collapsed;
        }), TaskScheduler.Default);
    }

    private void ShowPhotos()
    {
        if (_person is null) return;
        ulong search;
        try { search = _api.SearchPerson(_person.Id, null, MvAiScope.All); }
        catch (MediaViewerException) { return; }
        _chrome.OpenSearchAsList(search, _person.Name.Length > 0 ? $"Photos of {_person.Name}" : "Photos of this person");
    }

    private void OnKeyDown(object sender, KeyRoutedEventArgs e)
    {
        object? focused = FocusManager.GetFocusedElement(Content.XamlRoot);
        if (focused is TextBox) return;
        bool inFaces = focused is GridViewItem item && ReferenceEquals(ItemsControl.ItemsControlFromItemContainer(item), _faceGrid);
        switch (e.Key)
        {
            case VirtualKey.Delete when inFaces || _faceGrid.SelectedItems.Count > 0:
                RejectSelected();
                e.Handled = true;
                break;
            case VirtualKey.F2 when _person is not null:
                _name.Focus(FocusState.Keyboard);
                _name.SelectAll();
                e.Handled = true;
                break;
            case VirtualKey.Enter when focused is GridViewItem { Content: not null } g &&
                                       _peopleGrid.ItemFromContainer(g) is PersonVm p:
                ShowPerson(p, focusFaces: true);
                e.Handled = true;
                break;
            case VirtualKey.Escape:
                if (inFaces) _peopleGrid.Focus(FocusState.Keyboard);
                else Close();
                e.Handled = true;
                break;
        }
    }
}
