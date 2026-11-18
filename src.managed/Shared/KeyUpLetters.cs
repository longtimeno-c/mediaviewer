// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
namespace MediaViewer.Shared;

/// <summary>
/// FakeInput's fallback for an island that never raises CharacterReceived: a
/// letter or digit is typed when its key comes up. Only a key that went down in
/// this field, as plain typing, may type on the way up. Not a shortcut's letter:
/// the V of Ctrl+V comes up after Ctrl is let go more often than not, and typed
/// a stray "v" after the paste. Not a key pressed before the field had focus:
/// search's Ctrl+F focuses the field on F's way down, and typed an "f".
/// Plain .NET (key codes as ints), so MediaViewer.Chrome.Tests can test it.
/// </summary>
internal sealed class KeyUpLetters
{
    private readonly HashSet<int> _down = new();
    private int _last = -1;

    /// <summary>A key went down here. <paramref name="command"/>: the field took it
    /// (an edit or a caret key), or Ctrl or Alt was held: never typed.</summary>
    public void Down(int key, bool command)
    {
        _last = key;
        if (command) _down.Remove(key);
        else _down.Add(key);
    }

    /// <summary>CharacterReceived typed the last key down: its key-up types nothing.</summary>
    public void Typed()
    {
        _down.Remove(_last);
    }

    /// <summary>A key came up: true when it is to be typed now.</summary>
    public bool Up(int key) => _down.Remove(key);

    /// <summary>Focus went: keys still held are not this field's any more.</summary>
    public void Clear()
    {
        _down.Clear();
        _last = -1;
    }
}
