// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using MediaViewer.Shared;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

/// <summary>
/// #510 (docs/design/18 "Settings → Add-ons"): Settings → Add-ons is a short
/// list, one row per add-on with its state in a few words (Installed · 1.2.0,
/// Update available · 1.3.0, Restart to finish updating, Not installed), and
/// each row opens that add-on's own page: its line, Install / Update / Open /
/// Remove, its progress and messages, and its settings. The order is Import,
/// Local search, Places, Driftwire, Enhance, any other installed add-on of
/// MediaViewer's, then From other makers. The Mac's twin is AddonListView.swift;
/// the words are Shared/AddonListWords.cs.
/// </summary>
/// <remarks>
/// The pages are the panels the add-ons always had (Import's card, Local
/// search's panel, the slot add-ons' cards, From others), built once with
/// Settings and refreshed in place as before; a page shows one of them, moved
/// into the page view, with a back button. The slot add-ons share one panel
/// (<c>_slotSettingsPanel</c>) that draws only the card of the add-on whose page
/// is open (<see cref="_slotPageId"/>). Opening Settings shows the list unless
/// something asked for a page (Local search's Settings, a package to install).
/// </remarks>
public static partial class IslandHost
{
    private const string ImportPageId = "import";
    private const string LocalSearchPageId = "ai";
    private const string OthersPageId = "others";

    // The page open in Settings → Add-ons; "" shows the General page itself.
    private static string _addonPage = "";
    // A page asked for before Settings was built or shown: opened by PresentSettings.
    private static string? _addonPageWanted;
    // The slot add-on whose card _slotSettingsPanel draws (null: none, the list is up).
    private static string? _slotPageId;
    private static StackPanel? _addonListPanel;
    private static readonly Dictionary<string, (Button Row, TextBlock State)> AddonListRows = new(StringComparer.Ordinal);
    private static string _addonListKey = "";
    private static ScrollViewer? _settingsScroll;
    private static FrameworkElement? _settingsMain;
    private static StackPanel? _addonPageView;
    private static TextBlock? _addonPageTitle;
    private static Border? _addonPageBody;
    private static Button? _addonPageBack;
    private static FrameworkElement? _importPage, _localSearchPage, _slotsPage, _othersPage;
    private static double _settingsListOffset;

    private sealed record AddonListEntry(string Id, string Name, string Line);

    /// <summary>
    /// BuildSettingsScreen: the heading, the restart marker and the list into
    /// <paramref name="into"/>; the pages kept for <see cref="OpenAddonPage"/>.
    /// </summary>
    private static void BuildAddonsList(StackPanel into, FrameworkElement importPage, FrameworkElement localSearchPage,
                                        FrameworkElement slotsPage, FrameworkElement othersPage)
    {
        TextBlock heading = Heading(AddonListWords.Heading);
        heading.FontSize = TypeHeading;
        AutomationProperties.SetHeadingLevel(heading, Microsoft.UI.Xaml.Automation.Peers.AutomationHeadingLevel.Level2);
        into.Children.Add(heading);
        into.Children.Add(BuildAddonsRestartRow());  // #510: "Restart to finish updating …"
        _addonListPanel = new StackPanel { Spacing = Space(6) };
        into.Children.Add(_addonListPanel);
        _importPage = importPage;
        _localSearchPage = localSearchPage;
        _slotsPage = slotsPage;
        _othersPage = othersPage;
        AddonListRows.Clear();
        _addonListKey = "";
        RefreshAddonList();
    }

    /// <summary>The page view: back, the add-on's name, its panel. Built once.</summary>
    private static StackPanel BuildAddonPageView()
    {
        var view = new StackPanel
        {
            Spacing = Space(12),
            Padding = Pad(24, 12, 24, 24),
            MaxWidth = 800,
            HorizontalAlignment = HorizontalAlignment.Stretch,
        };
        var backLabel = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(6) };
        backLabel.Children.Add(new FontIcon
        {
            Glyph = "",  // ChevronLeft
            FontFamily = SymbolFont,
            FontSize = UiFontSize - 2,
            Foreground = Brush(Title),
            VerticalAlignment = VerticalAlignment.Center,
        });
        backLabel.Children.Add(ButtonLabel(AddonListWords.Back, UiFontSize));
        _addonPageBack = ChromeButton(backLabel, CompactButtonPadding, CloseAddonPage, align: HorizontalAlignment.Left,
                                      name: AddonListWords.Back, tip: "Back to the list of add-ons (Esc)");
        _addonPageBack.AllowFocusOnInteraction = true;
        _addonPageBack.Margin = Pad(-8, 0, 0, 0);
        view.Children.Add(_addonPageBack);
        _addonPageTitle = Heading("");
        _addonPageTitle.FontSize = TypeHeading;
        AutomationProperties.SetHeadingLevel(_addonPageTitle, Microsoft.UI.Xaml.Automation.Peers.AutomationHeadingLevel.Level2);
        view.Children.Add(_addonPageTitle);
        // Every page's body in one card, as the Mac's AddonPageView.
        _addonPageBody = new Border
        {
            Padding = Pad(14),
            Background = Brush(ChromeColour.Surface),
            CornerRadius = Corner(RadiusCard),
            BorderBrush = Brush(Hairline),
            BorderThickness = new Thickness(1),
        };
        view.Children.Add(_addonPageBody);
        // The add-on's own settings (place "settings"), its cards under the page's.
        view.Children.Add(BuildAddonPageSettings());
        _addonPageView = view;
        return view;
    }

    private static List<AddonListEntry> AddonListEntries()
    {
        bool Restart(string name) => _addonRestartFor.Any(u => u.Name == name);
        string FirstParty(AddonSlot slot, string name, bool busy) => AddonListWords.Line(
            _addonStatesRead, slot.State.Installed, slot.State.Version, slot.State.State, UpdateVersion(slot),
            Restart(name), busy);
        string Slot(SlotAddonRow? row, AddonSlot channel, string name) => AddonListWords.Line(
            _slotAddonsRead, row is not null, row?.Version ?? "", row?.State ?? "",
            row is null ? null : SlotUpdateVersion(channel, row), Restart(name), channel.Phase is not null);

        var list = new List<AddonListEntry>
        {
            new(ImportPageId, "Import", FirstParty(ImportSlot, "Import", ImportSlot.Phase is not null)),
            new(LocalSearchPageId, "Local search", FirstParty(AiSlot, "Local search",
                AddonSlots.Any(s => (s == AiSlot || s.Parent == AiSlot.Id) && s.Phase is not null))),
        };
        SlotAddonRow? places = SlotAddonRows.FirstOrDefault(r => r.Id == PlacesChannel.Id);
        list.Add(new(PlacesChannel.Id, "Places", Slot(places, PlacesChannel, "Places")));
        SlotAddonRow? driftwire = SlotAddonRows.FirstOrDefault(r => r.Id == DriftwireId);
        list.Add(new(DriftwireId, "Driftwire", Slot(driftwire, DriftwireSlot, "Driftwire")));
        SlotAddonRow? enhance = SlotAddonRows.FirstOrDefault(r => r.Id == EnhanceId);
        list.Add(new(EnhanceId, "Enhance", Slot(enhance, EnhanceSlot, "Enhance")));
        foreach (SlotAddonRow row in SlotAddonRows.Where(r => r.Id != PlacesChannel.Id && r.Id != DriftwireId &&
                                                              r.Id != EnhanceId)
                                                  .OrderBy(r => r.Name, StringComparer.CurrentCultureIgnoreCase))
        {
            list.Add(new(row.Id, row.Name, Slot(row, SlotChannelFor(row), row.Name)));
        }
        list.Add(new(OthersPageId, AddonListWords.FromOthers, AddonListWords.Others(_openAddons.Count)));
        return list;
    }

    /// <summary>
    /// Anything an add-on's state hangs on changed: the rows' lines are set in
    /// place, so a row with keyboard focus keeps it; the rows are made again
    /// only when the set of add-ons changes. Also the open page's title.
    /// </summary>
    private static void RefreshAddonList()
    {
        RefreshAddonUpdates();  // #525: the bar's notice follows the same states
        if (_addonListPanel is null) return;
        List<AddonListEntry> entries = AddonListEntries();
        string key = string.Join("\n", entries.Select(e => e.Id + "\t" + e.Name));
        if (key != _addonListKey)
        {
            _addonListKey = key;
            _addonListPanel.Children.Clear();
            AddonListRows.Clear();
            foreach (AddonListEntry e in entries)
            {
                (Button row, TextBlock state) = AddonListRow(e);
                AddonListRows[e.Id] = (row, state);
                // The settings rows' card around the flat button: its hover
                // wash shows inside it, and no template or resource is touched
                // (both fail-fast or throw in these islands).
                _addonListPanel.Children.Add(new Border
                {
                    Child = row,
                    Background = Brush(ChromeColour.Surface),
                    BorderBrush = Brush(Hairline),
                    BorderThickness = new Thickness(1),
                    CornerRadius = Corner(RadiusCard),
                });
            }
        }
        foreach (AddonListEntry e in entries)
        {
            if (!AddonListRows.TryGetValue(e.Id, out var r)) continue;
            if (r.State.Text != e.Line) r.State.Text = e.Line;
            AutomationProperties.SetName(r.Row, $"{e.Name}, {e.Line}");
        }
        if (_addonPage.Length > 0 && _addonPageTitle is not null)
        {
            AddonListEntry? open = entries.FirstOrDefault(e => e.Id == _addonPage);
            if (open is not null) _addonPageTitle.Text = open.Name;
        }
    }

    // A row: the name, its state, a chevron; the whole row opens the page.
    private static (Button, TextBlock) AddonListRow(AddonListEntry e)
    {
        var grid = new Grid { ColumnSpacing = Space(12) };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        TextBlock name = Label(e.Name);
        name.Foreground = Brush(Title);
        name.VerticalAlignment = VerticalAlignment.Center;
        grid.Children.Add(name);
        TextBlock state = Label(e.Line);
        state.FontSize = TypeSize(12);
        state.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(state, 1);
        grid.Children.Add(state);
        var chevron = new FontIcon
        {
            Glyph = "",  // ChevronRight
            FontFamily = SymbolFont,
            FontSize = UiFontSize - 2,
            Foreground = Brush(Body),
            VerticalAlignment = VerticalAlignment.Center,
        };
        Grid.SetColumn(chevron, 2);
        grid.Children.Add(chevron);
        string id = e.Id;
        Button row = ChromeButton(grid, Pad(14, 12, 14, 12), () => OpenAddonPage(id),
                                  align: HorizontalAlignment.Stretch, contentAlign: HorizontalAlignment.Stretch);
        row.AllowFocusOnInteraction = true;
        row.CornerRadius = Corner(RadiusCard);
        return (row, state);
    }

    /// <summary>
    /// Opens an add-on's page. Before Settings is up (a jump from elsewhere),
    /// it is opened when Settings next shows.
    /// </summary>
    private static void OpenAddonPage(string id)
    {
        if (_settingsScroll is null || _addonPageView is null || _addonPageBody is null || !_settingsVisible)
        {
            _addonPageWanted = id;
            return;
        }
        _addonPageWanted = null;
        FrameworkElement? page = id switch
        {
            ImportPageId => _importPage,
            LocalSearchPageId => _localSearchPage,
            OthersPageId => _othersPage,
            _ => _slotsPage,
        };
        if (page is null) return;
        if (_addonPage.Length == 0) _settingsListOffset = _settingsScroll.VerticalOffset;
        _addonPage = id;
        bool slot = id is not (ImportPageId or LocalSearchPageId or OthersPageId);
        _slotPageId = slot ? id : null;
        _slotConfirming = null;
        if (slot) RefreshSlotSettings();
        _addonPageBody.Child = page;
        _settingsScroll.Content = _addonPageView;
        RefreshAddonList();  // the title
        _settingsScroll.ChangeView(null, 0, null, disableAnimation: true);
        SyncAddonSettingsScreen();
        _dispatcher?.DispatcherQueue.TryEnqueue(() => _addonPageBack?.Focus(FocusState.Programmatic));
    }

    /// <summary>Back (the button, Esc): the list, scrolled where it was, the row focused.</summary>
    private static void CloseAddonPage()
    {
        if (_addonPage.Length == 0 || _settingsScroll is null || _settingsMain is null) return;
        string was = _addonPage;
        _addonPage = "";
        _slotPageId = null;
        _slotConfirming = null;
        if (_addonPageBody is not null) _addonPageBody.Child = null;
        _settingsScroll.Content = _settingsMain;
        RefreshSlotSettings();  // draws no card while the list is up
        SyncAddonSettingsScreen();
        // Laid out first: before that the list's extent is 0 and the offset clamps to the top.
        _settingsScroll.UpdateLayout();
        _settingsScroll.ChangeView(null, _settingsListOffset, null, disableAnimation: true);
        _dispatcher?.DispatcherQueue.TryEnqueue(() =>
        {
            if (AddonListRows.TryGetValue(was, out var r)) r.Row.Focus(FocusState.Programmatic);
        });
    }

    /// <summary>PresentSettings: a page something asked for, else the list.</summary>
    private static void ShowWantedAddonPage()
    {
        string? wanted = _addonPageWanted;
        _addonPageWanted = null;
        if (wanted is not null) OpenAddonPage(wanted);
        else if (_addonListWanted) ShowAddonList();  // #525: the updates notice
        else if (_addonPage.Length > 0) CloseAddonPage();
    }
}
