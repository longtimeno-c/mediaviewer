// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// FakeInput's key-up fallback (src.managed/Shared/KeyUpLetters.cs): which key
// coming up may still type its letter.
using MediaViewer.Shared;

namespace MediaViewer.Chrome.Tests;

public class KeyUpLettersTests
{
    private const int A = 'A', B = 'B', V = 'V', Control = 0x11;

    [Fact]
    public void CtrlVLetGoOfCtrlFirstTypesNoV()
    {
        var k = new KeyUpLetters();
        k.Down(Control, command: true);
        k.Down(V, command: true);  // pasted
        Assert.False(k.Up(Control));
        Assert.False(k.Up(V));  // Ctrl is up by now: still the paste's V
    }

    [Fact]
    public void CtrlFThatMovesFocusIntoTheFieldTypesNoF()
    {
        // Search's Ctrl+F is handled before the field (PreviewKeyDown) and
        // focuses it: the field sees only the keys coming up.
        var k = new KeyUpLetters();
        Assert.False(k.Up(Control));
        Assert.False(k.Up('F'));
        // Typing afterwards still works.
        k.Down('F', command: false);
        Assert.True(k.Up('F'));
    }

    [Fact]
    public void APlainLetterTypesOnTheWayUpWhenNoCharacterCame()
    {
        var k = new KeyUpLetters();
        k.Down(A, command: false);
        Assert.True(k.Up(A));
        Assert.False(k.Up(A));  // once
    }

    [Fact]
    public void ALetterCharacterReceivedTypedIsNotTypedAgain()
    {
        var k = new KeyUpLetters();
        k.Down(A, command: false);
        k.Typed();
        Assert.False(k.Up(A));
    }

    [Fact]
    public void OverlappingLettersBothType()
    {
        var k = new KeyUpLetters();
        k.Down(A, command: false);
        k.Down(B, command: false);
        Assert.True(k.Up(A));
        Assert.True(k.Up(B));
    }

    [Fact]
    public void AKeyPressedElsewhereOrBeforeFocusLeftTypesNothing()
    {
        var k = new KeyUpLetters();
        Assert.False(k.Up(A));  // went down before the field had focus
        k.Down(B, command: false);
        k.Clear();              // focus went
        Assert.False(k.Up(B));
    }
}
