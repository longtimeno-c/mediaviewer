// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using MediaViewer.Interop;
using MediaViewer.Shared;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Ai.Chrome;

/// <summary>
/// "Move to…": a searchable list of people, named first, as a flyout on a
/// button (the person sheet's and the unknown faces sheet's). It opens on the
/// grid's people at once, then everyone the index knows (people_json, read on a
/// worker: the grid may be narrowed to the open folder). A FakeInput, not a
/// TextBox: that fail-fasts in this island host. Rule 6: names are shown, never logged.
/// </summary>
internal sealed class PersonPicker
{
    private readonly AiChrome _chrome;
    private readonly PeopleGrid _grid;
    private readonly Func<ulong> _except;
    private readonly Action<PickPerson> _picked;
    private readonly Flyout _flyout;
    private readonly FakeInput _search;
    private readonly StackPanel _list;
    private readonly TextBlock _none;
    private List<PickPerson> _people = new();
    private int _generation;

    public PersonPicker(AiChrome chrome, PeopleGrid grid, Button button, Func<ulong> except, Action<PickPerson> picked)
    {
        _chrome = chrome;
        _grid = grid;
        _except = except;
        _picked = picked;
        Look look = chrome.Look;
        _search = new FakeInput(look.Input(13), "Search people", 240);
        AutomationProperties.SetName(_search, "Search people");
        _search.Changed += Fill;
        _search.Submitted += () =>
        {
            List<PickPerson> shown = Shown();
            if (shown.Count > 0) Pick(shown[0]);
        };
        _list = new StackPanel { Spacing = 2 };
        AutomationProperties.SetName(_list, "People");
        _none = look.Text("Nobody by that name", 12);
        _none.Visibility = Visibility.Collapsed;
        var column = new StackPanel { Spacing = 8, Width = 260 };
        column.Children.Add(_search);
        column.Children.Add(_none);
        column.Children.Add(new ScrollViewer
        {
            Content = _list,
            MaxHeight = 300,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
        });
        _flyout = new Flyout { Content = column };
        _flyout.Opening += (_, _) => Open();
        _flyout.Opened += (_, _) => _search.Focus(FocusState.Programmatic);
        button.Flyout = _flyout;
        Button = button;
    }

    public Button Button { get; }

    /// <summary>M in a sheet: the picker, from the keyboard.</summary>
    public void Show()
    {
        if (Button.IsEnabled && Button.Visibility == Visibility.Visible) _flyout.ShowAt(Button);
    }

    private void Open()
    {
        _search.SetText("");
        _people = _grid.People.Select(p => new PickPerson(p.Id, p.Name, p.Faces)).ToList();
        Fill();
        int g = ++_generation;
        AiApi api = _chrome.Api;
        _ = Task.Run(() =>
        {
            List<PickPerson>? everyone = null;
            try { everyone = PeopleWords.ParsePeople(api.PeopleJson()); }  // [worker-thread]
            catch (Exception ex) when (ex is MediaViewerException or System.Text.Json.JsonException) { }
            if (everyone is null || everyone.Count == 0) return;
            _list.DispatcherQueue.TryEnqueue(() =>
            {
                if (g != _generation) return;
                _people = everyone;
                Fill();
            });
        });
    }

    private List<PickPerson> Shown() => PeopleWords.PickerOrder(_people, _search.Text, _except());

    private void Fill()
    {
        Look look = _chrome.Look;
        _list.Children.Clear();
        List<PickPerson> shown = Shown();
        foreach (PickPerson p in shown.Take(200))
        {
            string label = PeopleWords.PickName(p);
            var row = new Button
            {
                Content = look.Text(label, 13, AddonColour.Title, wrap: false),
                HorizontalAlignment = HorizontalAlignment.Stretch,
                HorizontalContentAlignment = HorizontalAlignment.Left,
                Padding = new Thickness(8, 4, 8, 5),
                BorderThickness = new Thickness(0),
                Background = look.Tint(AddonColour.Title, 0),
            };
            AutomationProperties.SetName(row, "Move to " + label);
            PickPerson chosen = p;
            row.Click += (_, _) => Pick(chosen);
            _list.Children.Add(row);
        }
        _none.Visibility = shown.Count == 0 && _people.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private void Pick(PickPerson p)
    {
        ++_generation;
        _flyout.Hide();
        _picked(p);
    }
}
