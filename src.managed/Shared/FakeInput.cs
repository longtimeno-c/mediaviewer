// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.System;
using Windows.UI.Core;

namespace MediaViewer.Shared;

/// <summary>How a <see cref="FakeInput"/> draws: getters, so a theme change repaints.</summary>
internal sealed class FakeInputLook
{
    public required Func<FontFamily> Font { get; init; }
    public required Func<double> FontSize { get; init; }
    public required Func<Brush> Title { get; init; }       // text, caret, focused border
    public required Func<Brush> Body { get; init; }        // placeholder
    public required Func<Brush> Canvas { get; init; }      // the box
    public required Func<Brush> Hairline { get; init; }    // the unfocused border
    public required Func<Brush> Selection { get; init; }   // selected text's highlight
    public required Func<Brush> SelectionInk { get; init; }
}

/// <summary>
/// A type-in field with no WinUI TextBox, for the main chrome and the add-ons
/// (linked into each assembly). A TextBox in this island host is a
/// Microsoft.UI.Xaml fail-fast (0xC000027B): its default context flyout is a
/// TextCommandBarFlyout, a controls-library type this host cannot create, and
/// it is looked up the moment the box's template changes (measured 2026-09-28:
/// a TextBox added to the live tree crashes; the Local search panel's Ctrl+F
/// crash). tools/check-winui-controls.ps1 keeps TextBox out.
/// </summary>
/// <remarks>
/// It edits like a text box (owner, 2026-09-27: "Ctrl+A in the search bar to
/// clear / rewrite it"): a caret and a selection, Ctrl+A / C / X / V, Ctrl+Z /
/// Ctrl+Y (Ctrl+Shift+Z), ← → Home End with Shift to select and Ctrl for words,
/// Backspace / Delete (Ctrl: a word). Enter and Down are the field's only when
/// something listens (<see cref="Submitted"/>, <see cref="MoveDown"/>);
/// otherwise they go on to the container, as a TextBox's would.
/// </remarks>
internal class FakeInput : ContentControl
{
    private const int UndoDepth = 100;

    private readonly FakeInputLook _look;
    private readonly StackPanel _row;
    private readonly TextBlock _before;
    private readonly Border _selection;
    private readonly TextBlock _selected;
    private readonly TextBlock _after;
    private readonly Rectangle _caret;
    private readonly Border _inner;
    private readonly bool _bare;
    private readonly List<(string Text, int Caret)> _undo = new();
    private readonly List<(string Text, int Caret)> _redo = new();
    private Microsoft.UI.Dispatching.DispatcherQueueTimer? _blink;
    private string _placeholder;
    private int _caretAt;
    private int _anchor;
    private bool _typing;  // the last edit was a typed character: the next one joins its undo step
    private bool _tookChar;
    private string _text = "";

    /// <summary>Setting it is an edit, as TextBox.Text: <see cref="Changed"/> runs.
    /// <see cref="SetText"/> replaces the text quietly.</summary>
    public string Text
    {
        get => _text;
        set
        {
            string next = value ?? "";
            if (next == _text) return;
            _text = next;
            _caretAt = _anchor = next.Length;
            _typing = false;
            Changed?.Invoke();
            Paint();
        }
    }

    public string PlaceholderText
    {
        get => _placeholder;
        set
        {
            _placeholder = value ?? "";
            Paint();
        }
    }

    public event Action? Changed;
    public event Action? Submitted;
    public event Action? MoveDown;

    // The main chrome's fields keep Enter and Down even with no listener, as
    // they always have: the native router must not see them while one has focus.
    protected bool OwnsEnterAndDown { get; init; }

    // `bare`: no box of its own, for a field whose container draws one.
    public FakeInput(FakeInputLook look, string placeholder, double width = 0, bool bare = false)
    {
        _look = look;
        _placeholder = placeholder;
        _bare = bare;
        ProtectedCursor = InputSystemCursor.Create(InputSystemCursorShape.IBeam);
        if (width > 0) Width = width;
        else HorizontalAlignment = HorizontalAlignment.Stretch;
        IsTabStop = true;
        AllowFocusOnInteraction = true;
        UseSystemFocusVisuals = true;
        _before = MakeRun();
        _selected = MakeRun();
        _after = MakeRun();
        _selection = new Border
        {
            Child = _selected,
            VerticalAlignment = VerticalAlignment.Center,
            IsHitTestVisible = false,
        };
        _caret = new Rectangle
        {
            Width = 1,
            Height = look.FontSize() + 4,
            Fill = look.Title(),
            Margin = new Thickness(1, 0, 0, 0),
            VerticalAlignment = VerticalAlignment.Center,
            Visibility = Visibility.Collapsed,
            IsHitTestVisible = false,
        };
        _row = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            VerticalAlignment = VerticalAlignment.Center,
        };
        _row.Children.Add(_before);
        _row.Children.Add(_caret);
        _row.Children.Add(_selection);
        _row.Children.Add(_after);
        _inner = new Border
        {
            Child = _row,
            Background = look.Canvas(),
            BorderBrush = look.Hairline(),
            BorderThickness = new Thickness(1),
            Padding = new Thickness(10, 6, 10, 6),
            MinHeight = 32,
            HorizontalAlignment = HorizontalAlignment.Stretch,
        };
        Content = _inner;
        HorizontalContentAlignment = HorizontalAlignment.Stretch;
        Paint();
        GotFocus += (_, _) => { StartCaret(); Paint(); };
        LostFocus += (_, _) =>
        {
            StopCaret();
            _anchor = _caretAt;
            _typing = false;
            Paint();
        };
        PointerPressed += (_, e) =>
        {
            Focus(FocusState.Pointer);
            // A click takes the whole text, so typing replaces it.
            SelectAll();
            e.Handled = true;
        };
        CharacterReceived += (_, e) =>
        {
            char c = e.Character;
            if (char.IsControl(c)) return;
            _tookChar = true;
            Type(c.ToString());
            e.Handled = true;
        };
        KeyDown += (_, e) =>
        {
            _tookChar = false;
            if (OnKey(e.Key)) e.Handled = true;
        };
        KeyUp += (_, e) =>
        {
            // This island sometimes never raises CharacterReceived on a
            // ContentControl; letters still have to reach the field.
            if (_tookChar) return;
            if (!TryCharFromKey(e, out char c)) return;
            Type(c.ToString());
            e.Handled = true;
        };
    }

    private TextBlock MakeRun() => new()
    {
        FontFamily = _look.Font(),
        FontSize = _look.FontSize(),
        Foreground = _look.Body(),
        VerticalAlignment = VerticalAlignment.Center,
        IsHitTestVisible = false,
    };

    private static bool Down(VirtualKey vk) =>
        InputKeyboardSource.GetKeyStateForCurrentThread(vk).HasFlag(CoreVirtualKeyStates.Down);

    private int SelStart => Math.Min(_anchor, _caretAt);
    private int SelEnd => Math.Max(_anchor, _caretAt);
    private bool HasSelection => _anchor != _caretAt;

    // True when the key was the field's.
    private bool OnKey(VirtualKey key)
    {
        bool ctrl = Down(VirtualKey.Control);
        bool shift = Down(VirtualKey.Shift);
        if (Down(VirtualKey.Menu)) return false;  // Alt+ belongs to the system
        bool caretKey = key is VirtualKey.Left or VirtualKey.Right or VirtualKey.Home or VirtualKey.End or
            VirtualKey.Back or VirtualKey.Delete;
        // An empty field has nothing to move over or delete: those keys go
        // on to its container, as they always did.
        if (caretKey && _text.Length == 0) return false;
        switch (key)
        {
            case VirtualKey.Enter:
                if (Submitted is null && !OwnsEnterAndDown) return false;
                Submitted?.Invoke();
                return true;
            case VirtualKey.Down:
                if (MoveDown is null && !OwnsEnterAndDown) return false;
                MoveDown?.Invoke();
                return true;
            case VirtualKey.Left:
                if (HasSelection && !shift) MoveCaret(ctrl ? WordLeft(SelStart) : SelStart, false);
                else MoveCaret(ctrl ? WordLeft(_caretAt) : CharLeft(_caretAt), shift);
                return true;
            case VirtualKey.Right:
                if (HasSelection && !shift) MoveCaret(ctrl ? WordRight(SelEnd) : SelEnd, false);
                else MoveCaret(ctrl ? WordRight(_caretAt) : CharRight(_caretAt), shift);
                return true;
            case VirtualKey.Home:
                MoveCaret(0, shift);
                return true;
            case VirtualKey.End:
                MoveCaret(_text.Length, shift);
                return true;
            case VirtualKey.Back:
                if (HasSelection) Replace(SelStart, SelEnd, "");
                else if (_caretAt > 0) Replace(ctrl ? WordLeft(_caretAt) : CharLeft(_caretAt), _caretAt, "");
                return true;
            case VirtualKey.Delete:
                if (HasSelection) Replace(SelStart, SelEnd, "");
                else if (_caretAt < _text.Length) Replace(_caretAt, ctrl ? WordRight(_caretAt) : CharRight(_caretAt), "");
                return true;
        }
        if (!ctrl) return false;
        switch (key)
        {
            case VirtualKey.A:
                SelectAll();
                return true;
            case VirtualKey.C:
                CopySelection();
                return true;
            case VirtualKey.X:
                if (CopySelection()) Replace(SelStart, SelEnd, "");
                return true;
            case VirtualKey.V:
                PasteAsync();
                return true;
            case VirtualKey.Z:
                if (shift) Redo();
                else Undo();
                return true;
            case VirtualKey.Y:
                Redo();
                return true;
        }
        return false;
    }

    private static bool TryCharFromKey(KeyRoutedEventArgs e, out char c)
    {
        c = '\0';
        if (Down(VirtualKey.Control) || Down(VirtualKey.Menu)) return false;
        int v = (int)e.OriginalKey;
        if (v >= (int)VirtualKey.A && v <= (int)VirtualKey.Z)
        {
            c = (char)v;
            if (!Down(VirtualKey.Shift)) c = char.ToLowerInvariant(c);
            return true;
        }
        if (v >= (int)VirtualKey.Number0 && v <= (int)VirtualKey.Number9)
        {
            c = (char)v;
            return true;
        }
        return false;
    }

    // ---- editing ----------------------------------------------------------------

    private void Type(string s)
    {
        bool joins = _typing && !HasSelection;
        Replace(SelStart, SelEnd, s, joins);
        _typing = true;
    }

    // Every edit goes through here: one undo step (typing joins the last),
    // the caret after the new text, Changed once.
    private void Replace(int start, int end, string s, bool joinUndo = false)
    {
        string next = _text[..start] + s + _text[end..];
        if (next == _text) return;
        if (!joinUndo) Push(_undo, (_text, _caretAt));
        _redo.Clear();
        _typing = false;
        _text = next;
        _caretAt = _anchor = start + s.Length;
        Changed?.Invoke();
        Paint();
    }

    private static void Push(List<(string Text, int Caret)> stack, (string Text, int Caret) state)
    {
        stack.Add(state);
        if (stack.Count > UndoDepth) stack.RemoveAt(0);
    }

    private void Undo() => Step(_undo, _redo);
    private void Redo() => Step(_redo, _undo);

    private void Step(List<(string Text, int Caret)> source, List<(string Text, int Caret)> target)
    {
        _typing = false;
        if (source.Count == 0) return;
        (string text, int caret) = source[^1];
        source.RemoveAt(source.Count - 1);
        Push(target, (_text, _caretAt));
        _text = text;
        _caretAt = _anchor = Math.Clamp(caret, 0, text.Length);
        Changed?.Invoke();
        Paint();
    }

    private void MoveCaret(int to, bool extend)
    {
        _typing = false;
        _caretAt = Math.Clamp(to, 0, _text.Length);
        if (!extend) _anchor = _caretAt;
        Paint();
    }

    private bool CopySelection()
    {
        if (!HasSelection) return false;
        try
        {
            var package = new Windows.ApplicationModel.DataTransfer.DataPackage();
            package.SetText(_text.Substring(SelStart, SelEnd - SelStart));
            Windows.ApplicationModel.DataTransfer.Clipboard.SetContent(package);
            return true;
        }
        catch (Exception ex)
        {
            // Another process holding the clipboard: nothing is cut either.
            System.Diagnostics.Debug.WriteLine(ex);
            return false;
        }
    }

    private async void PasteAsync()
    {
        // Never let an exception out of an async void: that is a fail-fast.
        try
        {
            var view = Windows.ApplicationModel.DataTransfer.Clipboard.GetContent();
            if (!view.Contains(Windows.ApplicationModel.DataTransfer.StandardDataFormats.Text)) return;
            string pasted = await view.GetTextAsync();
            // One line: breaks and tabs become spaces, other controls go.
            var line = new System.Text.StringBuilder(pasted.Length);
            foreach (char ch in pasted)
            {
                if (ch == '\r' || ch == '\n' || ch == '\t') line.Append(' ');
                else if (!char.IsControl(ch)) line.Append(ch);
            }
            if (line.Length == 0) return;
            Replace(SelStart, SelEnd, line.ToString());
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
        }
    }

    private int CharLeft(int i)
    {
        if (i <= 0) return 0;
        --i;
        if (i > 0 && char.IsLowSurrogate(_text[i]) && char.IsHighSurrogate(_text[i - 1])) --i;
        return i;
    }

    private int CharRight(int i)
    {
        if (i >= _text.Length) return _text.Length;
        ++i;
        if (i < _text.Length && char.IsLowSurrogate(_text[i]) && char.IsHighSurrogate(_text[i - 1])) ++i;
        return i;
    }

    // Ctrl+← / Ctrl+Backspace: to the start of this word or the one before.
    private int WordLeft(int i)
    {
        while (i > 0 && char.IsWhiteSpace(_text[i - 1])) --i;
        while (i > 0 && !char.IsWhiteSpace(_text[i - 1])) --i;
        return i;
    }

    // Ctrl+→ / Ctrl+Delete: past this word and the spaces after it.
    private int WordRight(int i)
    {
        while (i < _text.Length && !char.IsWhiteSpace(_text[i])) ++i;
        while (i < _text.Length && char.IsWhiteSpace(_text[i])) ++i;
        return i;
    }

    /// <summary>Replaces the text without an edit: no <see cref="Changed"/>, no undo.</summary>
    public void SetText(string value)
    {
        string next = value ?? "";
        if (next == _text) return;
        _text = next;
        _caretAt = _anchor = next.Length;
        _undo.Clear();
        _redo.Clear();
        _typing = false;
        Paint();
    }

    /// <summary>Selects the text, so the next character replaces it.</summary>
    public void SelectAll()
    {
        _typing = false;
        _anchor = 0;
        _caretAt = _text.Length;
        Paint();
    }

    /// <summary>As TextBox.Select: <paramref name="length"/> 0 puts the caret at <paramref name="start"/>.</summary>
    public void Select(int start, int length)
    {
        _typing = false;
        _anchor = Math.Clamp(start, 0, _text.Length);
        _caretAt = Math.Clamp(start + length, 0, _text.Length);
        Paint();
    }

    private void Paint()
    {
        bool focused = FocusState != FocusState.Unfocused;
        bool empty = _text.Length == 0;
        if (empty)
        {
            _before.Text = focused ? "" : _placeholder;
            _selected.Text = "";
            _after.Text = "";
        }
        else
        {
            int start = SelStart, end = SelEnd;
            _before.Text = _text[..start];
            _selected.Text = _text[start..end];
            _after.Text = _text[end..];
        }
        Brush ink = empty ? _look.Body() : _look.Title();
        _before.Foreground = ink;
        _after.Foreground = ink;
        bool shown = focused && HasSelection;
        _selected.Foreground = shown ? _look.SelectionInk() : ink;
        _selection.Background = shown ? _look.Selection() : null;
        _caret.Fill = _look.Title();
        // The caret sits at its end of the selection.
        int at = HasSelection && _caretAt == SelEnd ? 2 : 1;
        if (_row.Children.IndexOf(_caret) != at)
        {
            _row.Children.Remove(_caret);
            _row.Children.Insert(at, _caret);
        }
        _caret.Visibility = focused ? Visibility.Visible : Visibility.Collapsed;
        _caret.Opacity = 1;
        if (_bare)
        {
            _inner.Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            _inner.BorderThickness = new Thickness(0);
            _inner.Padding = new Thickness(4, 5, 4, 5);
            _inner.MinHeight = 0;
            return;
        }
        _inner.Background = _look.Canvas();
        _inner.BorderBrush = focused ? _look.Title() : _look.Hairline();
        _inner.BorderThickness = new Thickness(focused ? 2 : 1);
    }

    private void StartCaret()
    {
        _blink ??= DispatcherQueue.CreateTimer();
        _blink.Interval = TimeSpan.FromMilliseconds(530);
        _blink.IsRepeating = true;
        _blink.Tick -= OnBlink;
        _blink.Tick += OnBlink;
        _caret.Visibility = Visibility.Visible;
        _caret.Opacity = 1;
        _blink.Start();
    }

    private void StopCaret()
    {
        _blink?.Stop();
        _caret.Opacity = 1;
    }

    private void OnBlink(Microsoft.UI.Dispatching.DispatcherQueueTimer sender, object args)
    {
        if (FocusState == FocusState.Unfocused) return;
        _caret.Opacity = _caret.Opacity > 0.5 ? 0 : 1;
    }
}
